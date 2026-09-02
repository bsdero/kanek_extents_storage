# AGENTS.md

This file provides guidance to agentic development tools and LLM-based
coding assistants (Claude Code, Codex, Cursor, GitHub Copilot Workspace,
and similar) when working with code in this repository. It is the
canonical instructions file for this repo; tool-specific filenames
(e.g. `CLAUDE.md`) are symlinks to it and must not diverge from it.

## Project Overview

KANEK Extents Storage (KES) is a small C library for extent-based block
storage management: a bitmap-backed block allocator plus an in-progress
extent caching layer. It's a standalone sibling project to `kanek`
(KFL) and `kanekfs` — do not confuse this repo's conventions with
theirs; nothing outside this directory applies here except where noted.

Three source files make up the whole library (`src/`, ~1600 lines
total): `kes_bitmap.c`, `kes_storage.c`, `kes_cache.c`, each with a
corresponding header in `include/kes/`.

## Ground truth: trust `PENDING_ITEMS.md`/`KES_HARDENING_PLAN.md`, not `docs/`

**Read `PENDING_ITEMS.md` before working on the cache layer** — it is
the current, maintained status/work tracker, and it links back to
`KES_HARDENING_PLAN.md` (still the design spec/ground-truth for *how*
to implement each remaining piece correctly). Both were originally
written because most of `docs/` (`CONTINUATION_PROMPT.md` in
particular, since corrected — see below) turned out to be aspirational
and wrong. Concretely, as of this writing:

- Phases 1–4 of `KES_HARDENING_PLAN.md` are done: the original
  heap-buffer-overflow bug, a P0 concurrency bug (duplicate hash-table
  entries on a racing cache miss), all four previously-missing
  `kes_cache.h` functions (`kes_cache_sync`, `kes_cache_invalidate`,
  `kes_cache_reset_stats`, `kes_cache_start`), and real capacity
  enforcement/eviction are all implemented and tested. `kes_cache_start()`
  runs a real background flush thread; `kes_cache_get_extent()` evicts
  from the LRU tail as needed to respect `config.max_entries`/
  `config.max_memory`, returning `KES_ERROR_BUSY` if it can't free
  enough room.
- `kes_cache_policy_t`: only `KES_CACHE_LRU` is implemented.
  `kes_cache_create()` **rejects** `KES_CACHE_LFU`/`KES_CACHE_CUSTOM`
  (returns `NULL`) rather than silently falling back to LRU — there is
  still no LFU or Clock eviction logic anywhere, despite both being
  described as complete in older versions of `docs/CONTINUATION_PROMPT.md`.
- Verified as of this writing: `make test` (plain, unsanitized build)
  passes clean — 93/93 tests across all twelve test binaries
  (`test_kes_minimal`, `test_kes_bitmap_full`, `test_kes_storage_full`,
  `test_kes_storage_edge`, `test_kes_cache`, `test_kes_cache_edge`,
  `test_kes_cache_full`, `test_kes_multiprocess`,
  `test_kes_fault_injection`, `test_kes_crash_consistency`,
  `test_kes_fuzz`, `test_kes_soak`). **`make check-all` (test + asan +
  tsan + valgrind) is now reliably clean** — re-run in full as of this
  writing: exit 0, "ALL CHECKS PASSED". This was not always true: the
  `kes_cache_flush_extent()` race (Track A.7.2) and the `block_count
  == 0`-unvalidated Valgrind gap (Track A.1.3) each failed it in turn;
  both are now fixed and closed, see `PENDING_ITEMS.md`'s `## Resolved`
  section for each. **Don't assume any of this stays true without
  rerunning it** — this is exactly the failure mode
  `KES_HARDENING_PLAN.md` §0 warns about, and it applies to this file
  too.
- `test_kes_multiprocess` covers a case none of the other binaries
  do: two independent OS processes (a real `fork()`, not threads),
  each with its own `kes_storage_open()` handle on the same backing
  file — a `pthread_mutex_t` inside `kes_storage_t` cannot coordinate
  across processes, so this exercises the storage layer's on-disk I/O
  path (`kes_extent_write`/`kes_extent_read`, which read/write by raw
  file offset and don't consult the in-memory bitmap) rather than its
  in-process locking. Two POSIX semaphores in an anonymous
  `MAP_SHARED` mapping enforce strict turn-taking across 8 steps, each
  side verifying the other's previous write before writing its own.
  This only proves *externally synchronized* cross-process access
  round-trips correctly — it deliberately does not race the two
  processes against each other. **Unsynchronized concurrent access to
  the same storage file from two `kes_storage_t*` instances remains an
  open, unguarded gap** — see the matching item in `PENDING_ITEMS.md`.
- Getting the cache-layer concurrency right required going *beyond*
  `KES_HARDENING_PLAN.md` §4's literal suggestions in a few places
  (its "bump `ref_count` to pin the traversal node" pattern turned out
  to be a real, ASan-confirmed data race against pre-existing code
  that also modifies `ref_count` under a different lock; `pthread_cond_wait()`
  internally unlocking while parked was a separate, TSan-caught hazard
  the plan didn't anticipate). See the `try_evict_entry_locked()` doc
  comment in `src/kes_cache.c` and the Phase 3 entry in
  `PENDING_ITEMS.md` for the full explanation before touching this
  code — the locking here is more subtle than it looks.
- **`kes_cache_flush_extent()` (`src/kes_cache.c`) had a real,
  TSan-confirmed data race, found by the Track A.7.2 soak test and
  reported before being fixed, per `plan_phase5.md` rule 0.3**:
  `kes_cache_get_extent()`'s cache-miss path deliberately calls
  `read_extent()` (writing into `entry->data`) without holding
  `entry->lock`, to avoid blocking other threads during I/O, even
  though the entry is already published in the hash table (state
  `KES_EXTENT_LOADING`) at that point. `kes_cache_flush_extent()` used
  to check only `entry->state & KES_EXTENT_DIRTY` before reading
  `entry->data` under `entry->lock` — unlike
  `sweep_flush_and_maybe_evict()` (used by `kes_cache_sync()`'s sweep
  path and eviction), which also requires `!(entry->state &
  KES_EXTENT_LOADING)`. A `kes_cache_mark_dirty()` call on a
  still-loading entry (legal — `state` can be `LOADING | DIRTY` at
  once) let `kes_cache_flush_extent()` race the in-flight load. **Fixed
  and closed as of commit `97ab19a`**: `kes_cache_flush_extent()` now
  waits out an in-flight load via `entry->cond`/`entry->cond_waiters`
  before checking `KES_EXTENT_DIRTY`, mirroring
  `kes_cache_get_extent()`'s own wait pattern — see the matching
  "Resolved" entry in `PENDING_ITEMS.md` for the full citation trail.
- **`block_count == 0` was not validated in `kes_cache_get_extent()`**
  (Track A.1.3): a request with `block_count == 0` fell through to
  `extent_data_size()` computing 0 and `aligned_alloc(64, 0)`, which
  glibc returns non-NULL for, producing a "successful" but nonsensical
  0-byte cached entry — and separately tripping Valgrind's Memcheck on
  the zero-size allocation, failing `make valgrind`/`make check-all`.
  **Fixed and closed**: `kes_cache_get_extent()` now rejects
  `id->block_count == 0` with `KES_ERROR_INVALID` before any
  allocation, mirroring the `id->block_size` guard above — see the
  matching "Resolved" entry in `PENDING_ITEMS.md`.

`docs/CONTINUATION_PROMPT.md` has been corrected to match current
reality and is safe to read now. Treat everything else under `docs/`
(design docs, project structure, edge-device prompts) as design-intent
/ marketing copy written ahead of the implementation, not a
description of current behavior — this still includes claims about
buddy-system allocation and flash optimization (aspirational, not
implemented). When in doubt about whether a feature exists, grep
`src/*.c` for the function name rather than trusting a doc.

**`PENDING_ITEMS.md` is the current work tracker** — read it before
picking up any cache or storage work. Phase 5 (test expansion,
`plan_phase5.md` Track A) and Phase 6 (docs truth pass, Track B) are
both **complete**: `KES_HARDENING_PLAN.md` §6's edge-case/fault-
injection/crash-consistency/fuzz/soak-test matrix is fully covered
(93/93 tests across 12 binaries, `make test`), and the 8 remaining
`docs/` files got their truth pass. What remains: allocation
strategies beyond first-fit, and the smaller non-concurrency gaps
`PENDING_ITEMS.md` catalogs (load-failure entries never auto-retrying,
etc.). The `kes_cache_destroy()`-vs-concurrent-access use-after-free —
formerly deliberately left unfixed per rule 0.3 pending an
API-contract decision — is now also fixed, alongside the
`kes_cache_flush_extent()` race and `block_count == 0` gap that used
to sit next to it in this list: see the "Track A.3.1" entry under
`PENDING_ITEMS.md`'s `## Resolved` section for the full fix.

**`plan_phase5.md` is the detailed execution record for that Phase
5/6 work** — a task-by-task breakdown (exact files added/edited, exact
test cases, exact acceptance criteria per task) of everything the
paragraph above summarizes, now fully executed (all of Track A and
Track B, per its own §6 bookkeeping rule). Read it for the *how* and
*why* behind any of the above before touching cache/storage/docs code
it covers, instead of re-deriving that history from
`KES_HARDENING_PLAN.md` §6/§7 yourself. If a new multi-task plan
supersedes it, keep this file, `PENDING_ITEMS.md`, and that plan in
sync with each other and with actual code as tasks complete — the same
failure mode that made the old `docs/CONTINUATION_PROMPT.md`
untrustworthy in the first place.

## `CODING_STYLE.md` is binding for all new/edited code

`CODING_STYLE.md` (originally written for KFL) is now the style
contract for this repo too. **Every line of C code you write or edit
in `src/` or `include/kes/` — including test files — must comply with
it**, even though the existing codebase largely does not yet. Do not
wait to be asked; apply it by default to any new function, any hunk
you touch, and any file you create.

The existing tree predates this rule and does **not** conform: it uses
`type* name` pointer style, Doxygen `/** */` block comments, file
banner comments, unbraced single-line `if` bodies in places, and
`return value;` without parens. **Do not silently reformat surrounding
code you aren't otherwise touching** just to bring it into compliance
— that turns a small diff into a repo-wide rewrite. Two exceptions:

- If asked to do a style sweep/retrofit explicitly, do it file by file
  and say which files were touched.
- If you're already rewriting a function's body for an unrelated
  reason, bring that function's braces/pointer style/`return` syntax
  into compliance while you're in there — but don't spill into
  neighboring functions you didn't need to change.

Before finishing any change, re-check it against Rules 1–10 and 12–13
specifically:

- pointer binds to the variable (`char *p`, not `char* p`) — this
  inverts the existing codebase's style, so it's the easiest one to
  backslide on out of habit
- every `if`/`else`/`while`/`for` body braced on its own line, even
  one-liners; explicit `== NULL` / `!= NULL` for pointer checks
  (existing code uses `if (!cache)` style — don't copy that pattern
  into new code)
- `return(value);`, not `return value;`
- `/* */` comments only, no `//`; no file-header banner comments on
  new files
- 78-char line cap, 4-space indent, no tabs
- space after `(`, none before `)`
- anonymous struct typedefs, no `_s` tag, for any new typedef'd struct

**Rule 11 (log before every early-return failure path) is now
partially addressed**: `kes_cache.c` and `kes_storage.c` both call
`TRACE_ERR` on several early-return failure paths added or touched
during Phases 1–4 (the no-read/write-extent-callback paths, the
load/flush-failure paths, the cache-full-can't-evict path,
`kes_cache_start()`'s already-running/`pthread_create`-failure paths,
the double-free/invalid-extent path in `kes_extent_free()`). Most
early-return paths in `kes_bitmap.c`/`kes_storage.c`, and the simple
NULL/not-found checks throughout `kes_cache.c`, still fail silently
(return an error code, print nothing) — this was never meant to be a
blanket retrofit, just applied to new/touched functions as they came
up, per the "no drive-by rewrites" rule below.
`../kanek_foundations/src/trace.h` (see the sibling KFL checkout
above) provides the `TRACE_ERR`/`TRACE_SYSERR`/`TRACE_ERRNO` macros
and is header-only for that subset; the include path already
resolves it. Continue applying it to new functions you write, and to
an existing function's error paths only when you're already
rewriting that function's body for another reason — don't do a
blanket sweep unless asked.

## Building and Testing

All commands run from the repo root (no `src/` subdirectory to `cd`
into — unlike KFL).

```bash
make all          # build build/libkes.a and build/libkes.so.1.0.0
make test-core    # build + run test_kes_minimal only — 9/9 pass, stable
make test         # build + run all 12 test binaries — 93/93 passing
                   # (see "Ground truth" above — this is the number
                   # as of this writing, re-verify before trusting it)
make debug        # DEBUG=1: -g3 -O0 -DDEBUG build
make examples     # build examples/example_kes_usage.c
make run-example  # build and run the example program
make clean        # remove build/, plus stray /tmp binary copies
                   # left by test/test-core/run-example and any
                   # package tarball
make install      # copies to /usr/local/{lib,include/kes} (sudo)
make info         # print resolved build config
make help         # list all targets

make asan         # clean rebuild + run tests under ASan+UBSan
make tsan         # clean rebuild + run tests under TSan
make sanitize-all # asan + tsan, then a plain rebuild
make valgrind     # clean rebuild + run tests under Valgrind
make check-all    # normal + asan + tsan + valgrind, gated on all passing
make soak         # clean TSan rebuild + 10-min test_kes_soak run
                   # (KES_SOAK_SECONDS=600) — this is what found the
                   # kes_cache_flush_extent() race documented above
make stress       # repeat test_kes_cache/test_kes_multiprocess
                   # $(STRESS_RUNS) times each; SANITIZER=asan|tsan
                   # selects a sanitized rebuild first, unset is plain
```

There is no per-test filtering flag — each `tests/test_*.c` maps to
one binary under `build/tests/`. `test` and `test-core` both copy the
built binary to `/tmp` before executing it (see the Makefile's `test`/
`test-core` recipes) — this is existing, intentional behavior, not a
workaround to remove. `make clean` removes those `/tmp` copies too
(by name, derived from `$(TEST_TARGETS)`), so they don't accumulate
across runs.

Build flags: `-std=c99 -Wall -Wextra -Werror -fPIC`, `-O2 -DNDEBUG` by
default. `-Werror` means any new warning fails the build.

ASan and TSan can't share a binary, so `asan`/`tsan`/`valgrind` each do
a full `clean` + rebuild with their own flags before running every
test binary. In this WSL2 environment, TSan binaries must run under
`setarch $(uname -m) -R` or they crash with an unrelated "unexpected
memory mapping" error — the `tsan` target already does this, so use it
rather than invoking a TSan-built test binary directly.

`make all`/`make asan`/`make tsan` first run `foundations-fetch`,
which clones the sibling `kanek_foundations` (KFL) repo to
`../kanek_foundations` if it isn't already checked out there —
`-I../kanek_foundations/src` is on the include path so `trace.h` is
reachable. `make foundations` builds `libkfl.a` from that checkout
on demand (not a prerequisite of `all`). `kes_cache.c` and
`kes_storage.c` both `#include "trace.h"` from that checkout now (see
the Rule 11 note above) — but only for that header-only macro subset;
nothing in `src/` links `libkfl.a`.

## Architecture

### Module layering

```
kes_types.h  (shared enums/structs/error codes, no dependencies)
     |
     +--> kes_bitmap.h/.c   (struct kes_bitmap: raw block bitmap)
     |         |
     |         v
     +--> kes_storage.h/.c  (struct kes_storage: wraps a kes_bitmap_t
     |                        for allocation, owns the backing fd,
     |                        pthread_mutex-protected)
     |
     +--> kes_cache.h/.c    (independent extent cache layer — takes
                              I/O callbacks, does NOT depend on
                              kes_storage.c; a caller wires the two
                              together via kes_cache_set_io_callbacks)
```

`kes_storage_t` owns one `kes_bitmap_t*` for block allocation and one
`kes_storage_descriptor_t` (magic number, block size, layout offsets,
free/used counts) that is written to block 0 of the backing file and
reloaded on `kes_storage_open()`. Extent allocation
(`kes_extent_allocate`) currently only implements first-fit
(`allocate_extent_first_fit` in `kes_storage.c`) regardless of the
`kes_allocation_strategy_t` requested in the config — best-fit/
worst-fit/next-fit are declared in `kes_types.h` but not yet wired up.

`kes_cache_t` is a hash-table + LRU-list extent cache designed to sit
in front of a storage backend via caller-supplied read/write/sync
callbacks (`kes_cache_set_io_callbacks`) — it does not call into
`kes_storage.c` directly. See "Ground truth" above for what's actually
implemented versus declared.

### Error handling convention

All public functions return `int`, `KES_SUCCESS` (0) or a negative
`KES_ERROR_*` code from `kes_types.h` (`KES_ERROR_INVALID`,
`_NOMEM`, `_NOTFOUND`, `_EXISTS`, `_IO`, `_NOSPACE`, `_CORRUPT`).
`kes_get_error_string()` maps codes to messages. Output values come
back through pointer-to-pointer or pointer-to-struct out-parameters,
not return values.
