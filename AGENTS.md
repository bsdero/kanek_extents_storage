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

## Ground truth: trust `KES_HARDENING_PLAN.md`, not `docs/`

**Read `KES_HARDENING_PLAN.md` before working on the cache layer.** It
is a verified work order — every claim in it was checked by building
the repo and reading the actual code — and it explicitly documents
that most of `docs/` (`CONTINUATION_PROMPT.md` in particular) is
aspirational and wrong. Concretely, as of this writing:

- `kes_cache.h` declares `kes_cache_sync`, `kes_cache_invalidate`,
  `kes_cache_reset_stats`, and `kes_cache_start` — **none of these are
  implemented** in `kes_cache.c`. `kes_cache_stop()` guards against
  `bg_threads` never having been started, so it doesn't crash, but no
  background sync thread ever runs.
- `kes_cache_policy_t` (`KES_CACHE_LRU`/`LFU`/`CUSTOM`) is stored at
  `kes_cache_create()` and never read again — there is no LFU or Clock
  eviction logic anywhere, despite both being described as complete in
  `docs/CONTINUATION_PROMPT.md`.
- **No eviction or capacity enforcement exists.** `max_entries` /
  `max_memory` are validated once at creation and never checked again;
  `kes_cache_get_extent()` unconditionally grows the cache on every
  miss. The LRU list (`mru_head`/`lru_tail`) is maintained correctly
  but nothing ever consults it to evict.
- The heap-buffer-overflow bug that used to fail `test_kes_cache` (a
  struct cast past the end of the cache's actual buffer allocation,
  `KES_HARDENING_PLAN.md` §1–2) is fixed — all 6/6 cache tests and
  9/9 minimal tests currently pass (`make check-all`: normal build +
  ASan+UBSan + TSan + Valgrind, 15/15 tests, 0 leaks, 0 races). Don't
  assume that stays true without rerunning it.

Treat everything else under `docs/` (design docs, project structure,
edge-device prompts) as design-intent / marketing copy written ahead
of the implementation, not a description of current behavior — this
includes `README.md`'s status badges and feature list (LFU/Clock
eviction, buddy-system allocation, flash optimization are all
aspirational, not implemented). When in doubt about whether a feature
exists, grep `src/*.c` for the function name rather than trusting a
doc.

**`PENDING_ITEMS.md` is the current work tracker** — read it alongside
`KES_HARDENING_PLAN.md` (which is still the design spec/ground-truth
for *how* to implement each piece correctly) before picking up cache
work. It lists, in priority order: a known but unfixed P0 concurrency
bug (`kes_cache_get_extent()` miss path can create duplicate
hash-table entries for the same extent id under concurrent access —
no lock spans the `hash_find`/`hash_insert` pair), the four
unimplemented `kes_cache.h` functions (Phase 3), eviction/capacity
enforcement (Phase 4), test expansion (Phase 5), and a docs truth pass
(Phase 6) to do last.

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

**Rule 11 (log before every early-return failure path) is still
unaddressed in existing KES code**: it names `TRACE_ERR`/
`TRACE_SYSERR`/`TRACE_ERRNO`, and `grep -rn "TRACE_\|trace.h" src/
include/` returns nothing — existing early-return paths in
`kes_bitmap.c`/`kes_storage.c`/`kes_cache.c` fail silently (return an
error code, print nothing). The macros themselves are no longer
missing, though: `../kanek_foundations/src/trace.h` (see the sibling
KFL checkout above) provides exactly these macros and is header-only
for that subset, and the include path already resolves it. Nothing in
`src/` includes or links it yet. Per this repo's "no drive-by
rewrites" rule, don't retrofit `TRACE_*` calls into existing functions
just to satisfy Rule 11 — apply it to new functions you write (Phase 3
of `PENDING_ITEMS.md` is the natural starting point), and only touch
an existing function's error paths when you're already rewriting that
function's body for another reason.

## Building and Testing

All commands run from the repo root (no `src/` subdirectory to `cd`
into — unlike KFL).

```bash
make all          # build build/libkes.a and build/libkes.so.1.0.0
make test-core    # build + run test_kes_minimal only — 9/9 pass, stable
make test         # build + run all tests, including cache (6/6 passing)
make debug        # DEBUG=1: -g3 -O0 -DDEBUG build
make examples     # build examples/example_kes_usage.c
make run-example  # build and run the example program
make clean        # remove build/
make install      # copies to /usr/local/{lib,include/kes} (sudo)
make info         # print resolved build config
make help         # list all targets

make asan         # clean rebuild + run tests under ASan+UBSan
make tsan         # clean rebuild + run tests under TSan
make sanitize-all # asan + tsan, then a plain rebuild
make valgrind     # clean rebuild + run tests under Valgrind
make check-all    # normal + asan + tsan + valgrind, gated on all passing
```

There is no per-test filtering flag — each `tests/test_*.c` maps to
one binary under `build/tests/`. `test` and `test-core` both copy the
built binary to `/tmp` before executing it (see the Makefile's `test`/
`test-core` recipes) — this is existing, intentional behavior, not a
workaround to remove.

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
on demand (not a prerequisite of `all`). As of this writing nothing in
`src/` actually includes or links KFL yet — see the Rule 11 note
below.

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
