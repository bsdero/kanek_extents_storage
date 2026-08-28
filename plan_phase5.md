# Phase 5/6 Work Plan — Test Hardening & Docs Truth Pass

**Status:** not started. This is an execution plan, not a status
report — do not edit the "done" language into this file casually; see
§6 (bookkeeping) for how to mark items complete.

**Audience:** any LLM coding agent or human picking up KES work after
this point. Assume zero prior context beyond what's in this repo:
read `AGENTS.md`, `PENDING_ITEMS.md`, and `KES_HARDENING_PLAN.md` §6/§7
first — this plan turns those two documents' remaining items into a
concrete, ordered task list with exact file/function targets, so you
should not need to re-derive scope from scratch, but you *do* still
need the "why" those documents carry (especially `KES_HARDENING_PLAN.md`
§4's concurrency-hazard writeups) before touching `src/kes_cache.c`.

---

## 0. Ground rules (inherited from `KES_HARDENING_PLAN.md` §0/§8, restated because they are the most-violated rules in this project's history)

1. **No rounding up.** A task in this plan is done when you have
   pasted, reproducible command output proving it — a green
   `make test` (or `make check-all` where a task specifically touches
   concurrency, see below) — not because the code "looks right" or a
   test was written. `docs/CONTINUATION_PROMPT.md` was wrong for a
   long time because someone did the latter; do not repeat it.
2. **One logical change per commit.** Each numbered task below
   (e.g. "A.2 cache edge cases") is a reasonable commit boundary.
   Do not batch unrelated tasks into one commit just because they're
   both "testing."
3. **No drive-by rewrites.** If you find a real bug while writing a
   test (this plan flags a couple of likely ones below, see A.2.7 and
   A.2.8), fix *that specific thing* in its own commit with its own
   note in `PENDING_ITEMS.md`, and do not use the opportunity to
   reformat or restyle code you weren't already touching.
4. **`CODING_STYLE.md` is binding for every line of test code you
   write**, same as production code — see `AGENTS.md`'s style section.
   The existing test files (`tests/test_kes_cache.c` etc.) mix styles
   (some use `!cache`, `type* name`) — match `CODING_STYLE.md` in new
   code you write, don't copy the old style just because it's nearby,
   and don't reformat the surrounding old code either.
5. **Concurrency-relevant tasks (Track A.3, and any new test that
   spawns threads or processes) must be run under TSan and ASan, not
   just the normal build, before being called done.** Use
   `make tsan` / `make asan`, or `make check-all` for the full sweep.
   Non-concurrency tasks (docs, most edge cases) only need `make test`.
6. **New `tests/test_*.c` files are picked up automatically.**
   `Makefile:57` defines `TEST_SOURCES = $(wildcard tests/test_*.c)`
   and `TEST_TARGETS` derives from it (`Makefile:58`) — you do **not**
   need to hand-edit the Makefile to wire in a new test file as long
   as its name matches `tests/test_*.c` and it follows the existing
   `main()`/test-table pattern (see any existing file in `tests/` for
   the shape: a `TEST_ASSERT`/`TEST_SUCCESS` macro pair, a
   `{name, fn}` table, a loop that runs them and prints a pass/fail
   summary, `main()` returning nonzero on any failure). The one
   exception is the soak test (Track A.7) — see that section for why
   it needs an explicit Makefile target of its own.

---

## 1. Scope

**In scope:** everything under `KES_HARDENING_PLAN.md` §6 (Phase 5 —
test expansion) that `PENDING_ITEMS.md`'s "Phase 5" section marks as
not done, plus `KES_HARDENING_PLAN.md` §7 (Phase 6 — docs truth pass)
for the 8 `docs/` files `PENDING_ITEMS.md` lists as not yet corrected.

**Explicitly out of scope for this plan** (tracked elsewhere, do not
pull into this work):
- Allocation strategies beyond first-fit (best-fit/worst-fit/next-fit)
  — `PENDING_ITEMS.md` lists this as a separate remaining item, not
  part of either Phase 5 or Phase 6. It is source-code feature work,
  not test/doc work, and touches `kes_storage.c`'s allocation
  dispatch — different risk profile from this plan. Leave it for its
  own plan document if/when picked up.
- Implementing LFU or Clock eviction. Both are deliberately rejected
  at `kes_cache_create()` (`src/kes_cache.c:697` per the last review)
  — that rejection is correct, documented behavior, not a gap.
- Multi-writer/single-writer enforcement for concurrent
  `kes_storage_t*` opens on the same file. §6.B and Track A.2 below
  ask you to *test and document* the current (unsafe) behavior, not
  to fix it — fixing it is a design decision `KES_HARDENING_PLAN.md`
  itself says is "beyond this plan's scope," and that's still true
  here.
- `-DTRACE_MIN_LEVEL`/Rule 11 blanket logging sweep — apply
  `TRACE_ERR` only to new functions you write in this plan, per
  `AGENTS.md`'s standing "no blanket sweep" rule.

---

## 2. Baseline (do not re-verify unless something looks off — but do not blindly trust it either, see rule 0.1)

As of the last check-in (`PENDING_ITEMS.md`, this session's review):
`make test` → 70/70 across `test_kes_minimal` (9), `test_kes_bitmap_full`
(10), `test_kes_storage_full` (15), `test_kes_cache` (23),
`test_kes_cache_full` (12), `test_kes_multiprocess` (1). Before
starting Track A, run `make test` yourself and confirm this number —
if it's different, stop and reconcile why before adding more tests on
top of an assumption that may no longer hold.

---

## 3. Track A — Phase 5 test expansion

Organized to match `KES_HARDENING_PLAN.md` §6's lettered subsections
(6.A is already done — functional-per-function coverage exists in
`test_kes_bitmap_full.c`/`test_kes_storage_full.c`/`test_kes_cache_full.c`
— skip it). Each task below states the gap, exactly which existing
test (if any) it must *not* duplicate, which file to add the new
test(s) to, and what to assert.

### A.1 — New test file: `tests/test_kes_cache_edge.c`

Rationale for a new file rather than appending to `test_kes_cache.c`
(1500+ lines already, 23 tests) or `test_kes_cache_full.c` (per-function
success/failure pairs, a different organizing principle): this file is
specifically for edge/boundary inputs that don't map 1:1 to "does
function X work," which is what `test_kes_cache_full.c` is for.

Use the same mock I/O harness pattern as `tests/test_kes_cache.c`
(`mock_read_extent`/`mock_write_extent` at lines 104/134, and
`g_mock_storage` backing array) — copy/adapt the minimal subset you
need rather than `#include`-ing the .c file.

Tests to add (one `TEST_ASSERT`-style function per bullet unless noted):

1. **Per-parameter NULL checks**, one assertion per parameter, for
   every `kes_cache_*` function that takes more than one pointer
   parameter and doesn't already have this in `test_kes_cache_full.c`
   — check that file first function-by-function before writing these,
   several already do this (e.g. `test_kes_cache_get_put_extent`
   likely covers `cache`/`id`/`buffer`). Only add what's missing;
   list in the test's `TEST_SUCCESS` message which parameters you
   checked so a reviewer doesn't have to re-derive it.
2. **`kes_cache_create()` with non-power-of-2 `config.block_size`.**
   Confirmed gap: `grep -n KES_IS_POWER_OF_2 src/*.c` currently only
   matches `src/kes_storage.c:537-539` (`kes_config_validate()`) —
   `kes_cache_create()` in `src/kes_cache.c` does **not** validate
   `config.block_size` (declared at `include/kes/kes_cache.h:38`) at
   all today. Decide and document (a short comment at the top of this
   test, not a design doc) whether this is a real bug or an
   intentional gap:
   - **Recommended:** treat it as a real, small, in-scope bug fix —
     add the same `KES_IS_POWER_OF_2(config->block_size) &&
     config->block_size >= KES_MIN_BLOCK_SIZE && config->block_size
     <= KES_MAX_BLOCK_SIZE` guard (mirroring `kes_storage.c:537-539`)
     to `kes_cache_create()`'s existing validation block, returning
     `NULL` on failure like its other validation failures do. This is
     a one-guard addition consistent with an existing pattern
     elsewhere in the codebase, not new design — it's the kind of fix
     rule 0.3 (no drive-by rewrites) still permits because you're
     *adding a test for exactly this function* and the fix is the
     direct subject of the test, not incidental cleanup.
   - If you decide against fixing it, you must instead write the test
     to assert (and comment why) the current permissive behavior is
     acceptable, and add a line to `PENDING_ITEMS.md`'s "Not done"
     list flagging it as a known gap — do not just skip the test
     silently.
3. **`block_count = 0` / `UINT32_MAX` in a `kes_extent_id_t` passed to
   `kes_cache_get_extent()`.** Assert `KES_ERROR_INVALID` (0) and a
   clean failure — not a crash or huge allocation attempt — for
   `UINT32_MAX` (this will try to allocate ~`UINT32_MAX * block_size`
   bytes if unchecked; if it's not currently rejected, this is a
   candidate for `KES_ERROR_NOMEM` instead, since
   `aligned_alloc`/`malloc` will legitimately fail at that size — read
   `extent_data_size()` and the miss-path allocation in
   `kes_cache_get_extent()` first to see which error path actually
   fires before asserting a specific code).
4. **`start_block * block_size` 64-bit overflow for the cache layer
   specifically.** Note: the storage layer's version of this is
   already covered —
   `test_kes_extent_read_write_32bit_overflow` in
   `tests/test_kes_storage_full.c`. This task is the cache-layer
   analogue: construct a `kes_extent_id_t` with `start_block` near
   `UINT64_MAX` and a large `block_count`, call
   `kes_cache_get_extent()`, and confirm `extent_data_size()`
   (`src/kes_cache.c`, ~line 88) does not silently wrap to a small
   value that would cause a truncated allocation followed by an
   overflowing memcpy in the mock I/O path.
5. **Pin/unpin imbalance and refcounted-pin semantics.** Verified by
   reading `src/kes_cache.c:1081-1139`: pinning **is refcounted**, not
   boolean — `kes_cache_pin_extent()` increments `entry->pin_count`
   every call (only setting `KES_EXTENT_PINNED` and bumping
   `stats.entries_pinned` on the 0→1 transition), and
   `kes_cache_unpin_extent()` decrements it, only clearing the pinned
   state and decrementing `stats.entries_pinned` on the →0 transition;
   calling unpin when `pin_count == 0` is already a guarded no-op
   (`if (entry->pin_count > 0)` at line 1126) — it will not underflow.
   This behavior is currently **undocumented** in
   `include/kes/kes_cache.h`'s doc comments for both functions — as
   part of this task, add a doc comment to both declarations stating
   plainly that pinning is reference-counted (N pins require N unpins
   before the entry becomes evictable again) and that an extra unpin
   beyond the pin count is a safe no-op. Then write the test: pin the
   same entry 3 times, unpin once, assert it's still pinned
   (unevictable — e.g. via a full cache + eviction-pressure check
   like `test_cache_pinned_entries_never_evicted` does), unpin twice
   more, assert it's now evictable; separately, unpin an
   never-pinned entry and assert `KES_SUCCESS` with no state change
   (not an error — matches current code, which doesn't special-case
   this as an error).
6. **`kes_cache_destroy()` with an outstanding, never-released
   `get_extent()` reference.** Get an extent, do not call
   `put_extent()`, then call `kes_cache_destroy()`. Assert: no crash,
   no ASan-detected leak or use-after-free (run this specific test
   under `make asan` before considering it done, since this is exactly
   the shape of bug ASan is good at catching and ordinary `make test`
   will not). Document in the test's comment what `destroy()` actually
   does in this case (frees the entry anyway despite the outstanding
   `ref_count`? refuses to destroy? — read `kes_cache_destroy()`'s
   current implementation first, then assert the actual behavior
   rather than an assumed one).
7. **Operations between `kes_cache_stop()` and `kes_cache_destroy()`.**
   Call `kes_cache_stop()`, then `kes_cache_get_extent()`/`put_extent()`
   on the now-stopped cache, then `kes_cache_destroy()`. Assert no
   crash and a sane return code (again: read current behavior first,
   don't assume `KES_ERROR_INVALID` is what happens — `kes_cache_stop()`
   only tears down background threads per `KES_HARDENING_PLAN.md` §4.4,
   it does not obviously flag the cache as unusable for normal ops, so
   this may currently just work — if so, assert that explicitly rather
   than asserting a rejection that doesn't happen).
8. **Empty-cache no-ops.** Call `kes_cache_sync()`, `kes_cache_invalidate()`
   (on an id that was never inserted — this already returns
   `KES_ERROR_NOTFOUND` per `KES_HARDENING_PLAN.md` §4.2 and is likely
   already covered; check `test_cache_invalidate` in
   `tests/test_kes_cache.c:1010` before duplicating), and
   `kes_cache_reset_stats()` on a freshly-created, never-populated
   cache. Assert `KES_SUCCESS` and no crash for all three where
   applicable.
9. **Hash-collision disambiguation.** `kes_extent_hash()` and
   `kes_extent_equal()` are both directly unit-tested already
   (`test_kes_extent_hash`/`test_kes_extent_equal`,
   `tests/test_kes_cache_full.c:377,391`) but not *together* in a
   lookup scenario. Construct (or find, by brute-force search over a
   small range if the hash function is simple enough — check
   `kes_extent_hash()`'s implementation first) two distinct
   `kes_extent_id_t` values that hash to the same bucket under the
   cache's current bucket count, insert both via `get_extent()`/
   `put_extent()`, and confirm both are independently retrievable
   with correct, distinct data — proving `kes_extent_equal()` is
   actually used to disambiguate within a bucket (`src/kes_cache.c:176,218`
   already call it in `hash_find`/`hash_find_or_insert` — this test
   proves it end-to-end rather than just unit-testing the comparator).
   If constructing a genuine collision is impractical, an acceptable
   fallback is a targeted test against `hash_find`/`hash_find_or_insert`
   directly (if visible to the test — check if they're `static`; if so,
   this may need to become a same-translation-unit test or you accept
   the weaker "many entries, all correct" coverage `test_concurrent_access`
   already gives and note in this file's comment why a true collision
   test wasn't added).

### A.2 — New/extended coverage: storage layer edge cases

Target file: **new** `tests/test_kes_storage_edge.c` for the
exhaustion/cross-process items below; **extend**
`tests/test_kes_multiprocess.c` for the deliberate-race item (it
already has the fork+shared-memory infrastructure — don't duplicate
that setup in a new file).

1. **Exact 100%-full allocation, then free-one-and-reallocate.**
   Distinct from the existing `test_kes_extent_allocate` coverage in
   `tests/test_kes_storage_full.c:191-200`, which only tests a single
   over-large request against a partially-used bitmap — not actual
   bit-for-bit exhaustion. New test: allocate extents in a loop until
   `kes_storage_get_stats()` reports `free_blocks == 0`, confirm the
   next `kes_extent_allocate()` call (even for `block_count = 1`)
   returns `KES_ERROR_NOSPACE` and that a `kes_bitmap_get_stats()` (or
   equivalent) call shows no corruption (used count still matches
   total blocks), then `kes_extent_free()` exactly one previously
   allocated extent and confirm a `block_count = 1` (or smaller)
   allocation now succeeds again.
2. **Unsynchronized concurrent open — deliberately racing.** Add a
   second test function to `tests/test_kes_multiprocess.c`, e.g.
   `test_cross_process_racing_io` (name it to read clearly as the
   *opposite* of the existing `Cross Process Sync IO` test). Reuse the
   `fork()`/shared-backing-file setup, but **remove** the semaphore
   turn-taking — let both child and parent independently call
   `kes_storage_open()` on the same file and issue overlapping
   `kes_extent_allocate()`/`kes_extent_write()` calls with no
   coordination at all. This test's job is **not** to assert
   correctness — `AGENTS.md` and `PENDING_ITEMS.md` already establish
   this is expected-corrupt, unguarded behavior. Its job is to:
   - Run without hanging or crashing the test harness itself (use a
     bounded number of racing operations, not an infinite loop).
   - Record what actually happens (e.g. assert only that the process
     completes and print a summary of divergence — mismatched
     free-block counts between the two processes' final
     `kes_storage_get_stats()`, or a corrupted descriptor magic
     number, whichever you actually observe) as a **documented, known
     limitation**, not a pass/fail correctness assertion. A
     reasonable acceptance shape: the test always "passes" in the
     sense of not crashing/hanging, but its output makes the
     corruption visible in the test log for a human to read — add a
     comment at the top of the test function explaining that this is
     intentional and pointing at the matching `PENDING_ITEMS.md`/
     `AGENTS.md` entries.
   - Do **not** attempt to fix the underlying race as part of this
     task — that's explicitly out of scope (§1 above).

### A.3 — Concurrency/stress scaling (§6.C remainder)

Most of §6.C is already done (`test_concurrent_access`,
`test_concurrent_miss_no_duplicate_entry`,
`test_concurrent_sync_vs_get_put` in `tests/test_kes_cache.c`). Two
items remain:

1. **`kes_cache_destroy()` racing a concurrent `get_extent()`/
   `put_extent()` thread.** Add to `tests/test_kes_cache.c` (it already
   has the thread-pool scaffolding `test_concurrent_access` uses —
   reuse that pattern) a test that starts a thread doing a tight
   get/put loop, then, from the main thread, after a short delay,
   calls `kes_cache_destroy()` while that thread may still be
   mid-operation. Read `kes_cache_destroy()`'s current implementation
   first to determine its actual contract (does it take `cache_lock`
   and wait? does it assume the caller has already quiesced all other
   threads?) before deciding what the test should assert — per
   `KES_HARDENING_PLAN.md` §6.C, "confirm this is either safely
   rejected/synchronized or explicitly documented as
   caller-must-quiesce-first, and write the test to match whichever
   contract you settle on." If the current code has a genuine
   use-after-free here (plausible, since nothing in the function list
   suggests `destroy()` synchronizes with in-flight callers), do not
   silently fix it — this is exactly the kind of finding rule 0.3
   asks you to flag rather than casually patch, because "callers must
   quiesce first" vs. "destroy() must be safe to call concurrently"
   is a real API contract decision, not a bug fix. Write the test to
   the currently-documented-or-inferred contract, run it under TSan/
   ASan, and if it finds a crash, stop and report it rather than
   patching around it in the same commit.
2. **Repeat-N-times stress harness.** Add a `make stress` Makefile
   target (near the existing `asan`/`tsan`/`valgrind` targets) that
   rebuilds normally (or accepts `SANITIZER=asan|tsan` to layer on top
   — your choice, document whichever you pick) and then runs
   `test_kes_cache` and `test_kes_multiprocess` (the two binaries with
   real thread/process races) `STRESS_RUNS` times in a shell loop
   (default `STRESS_RUNS=100`, overridable:
   `make stress STRESS_RUNS=500`), stopping and reporting the failing
   run number on first non-zero exit rather than continuing past a
   failure. For the "fixed random seed, logged" requirement: check
   whether any existing concurrency test already uses randomized
   delays/ordering (skim `test_concurrent_access` and
   `test_concurrent_sync_vs_get_put` for `rand()`/`usleep` with
   variable arguments) — if so, make that seed settable via an
   environment variable (e.g. `KES_TEST_SEED`, defaulting to
   `time(NULL)` if unset) and have the test print the seed it used on
   startup, so a `make stress` failure can be reproduced by rerunning
   with `KES_TEST_SEED=<printed value>`.

### A.4 — Fault injection (§6.D) — new file `tests/test_kes_fault_injection.c`

None of this exists today (confirmed: only `mock_read_extent_always_fail`
exists in `tests/test_kes_cache.c:163`, an unconditional-failure mock,
not a configurable one).

1. **Configurable-failure mock I/O wrapper.** Write a small harness
   (can live in this new file, doesn't need to be shared) wrapping
   `mock_read_extent`/`mock_write_extent`-equivalent logic with a
   global (or thread-local, if you're also using it under concurrency)
   failure-mode struct: fail on the Nth call, fail after call N
   unconditionally, or fail with a given probability (seeded RNG, log
   the seed per rule A.3.2's pattern). Use it to test:
   - A load failure on `get_extent()` does not leave a corrupted or
     half-inserted entry in the hash table — after a failed load,
     confirm a subsequent `get_extent()` on the same id (with the
     failure condition cleared) succeeds cleanly, and that
     `stats.entries_cached` reflects reality at every point (no phantom
     entry counted while none is actually retrievable). Note:
     `test_ref_count_leak_on_load_failure` and
     `test_ref_count_actually_released_on_load_failure` in
     `tests/test_kes_cache.c` already cover the refcount half of this
     — do not duplicate those; this task is specifically about
     hash-table entry state and retry-after-failure, not refcounting.
   - A flush failure (`write_extent` fails) leaves the entry's dirty
     flag set and `KES_EXTENT_ERROR` state, per
     `KES_HARDENING_PLAN.md` §4.1 point 1 — write a test asserting
     this directly (may already be implicitly true from existing sync
     tests; check `test_cache_sync`/`test_cache_invalidate_discards_dirty_data`
     in `tests/test_kes_cache.c` before writing a new one — if they
     already assert this, skip and note why in this file's header
     comment instead of duplicating).
2. **`malloc`/`aligned_alloc` failure simulation.** The most portable
   approach without introducing a new dependency (per `AGENTS.md`'s
   "no new dependencies without flagging it" rule) is requesting an
   unreasonably large `config.max_memory`/extent size that a real
   allocator will legitimately refuse, rather than `LD_PRELOAD`-based
   allocator interposition (which would be a new build-time
   dependency/technique — if you want to go that route instead,
   flag it explicitly as a deviation per rule 0's spirit before doing
   it, don't add it silently). Assert `KES_ERROR_NOMEM` propagates
   cleanly with no partial-state leak (run under ASan).
3. **Partial read/write simulation.** Have the mock I/O callback
   report success but only "transfer" fewer bytes than `size`
   requests. Per `KES_HARDENING_PLAN.md` §6.D: either show this is
   detected and treated as an error, or — if you determine after
   reading `kes_cache_get_extent()`'s read path that byte-count
   verification genuinely isn't this layer's job (the callback
   contract may legitimately assume the caller's I/O layer either
   fully succeeds or returns an error, with no short-transfer
   contract at all) — document that conclusion explicitly in this
   test file's header comment rather than leaving it silently
   untested. Either outcome is acceptable; silence is not.

### A.5 — Crash-consistency (§6.E) — new file `tests/test_kes_crash_consistency.c`

1. **No-explicit-sync reopen.** Write extents without calling
   `kes_storage_sync()`, close the handle (or, to more accurately
   simulate a crash rather than a clean shutdown, skip `close()`
   entirely if the API allows just dropping the fd — check whether
   `kes_storage_close()` itself performs an implicit sync first,
   since if it does, this test needs to bypass `close()` to be
   meaningful), reopen, and confirm the actual observed durability
   matches what `KES_STORAGE_SYNC`'s doc comment
   (`include/kes/kes_storage.h` — find and read it first) promises.
   If the doc comment is vague or silent on this, treat that as a
   Track B (docs) finding, not a test-writing blocker — write the
   test against actually-observed behavior and separately flag the
   doc gap.
2. **Truncated/corrupted descriptor detection.** After creating a
   storage file, open the raw file and truncate it to fewer bytes than
   one full descriptor block, or overwrite the magic-number field with
   garbage, then call `kes_storage_open()` and confirm it returns
   `KES_ERROR_CORRUPT` (or another documented error — check
   `kes_types.h`'s error codes) rather than proceeding with
   uninitialized/garbage geometry values that could cause later
   out-of-bounds access.
3. **Bit-flipped bitmap block.** Similarly, flip a single byte inside
   the persisted bitmap region (not the descriptor) of an otherwise
   valid file, reopen, and confirm either a clean detected-corruption
   error or — if the bitmap format has no self-check (no checksum),
   confirm at minimum that a subsequent allocation against the
   corrupted bitmap doesn't hand out a block that's actually already
   in use per the *un*-corrupted view, i.e. characterize and document
   whatever actually happens rather than assuming a specific outcome.

### A.6 — Randomized/fuzz-adjacent testing (§6.F) — new file `tests/test_kes_fuzz.c`

1. **Cache invariant fuzzing.** A long loop (parameterize the
   iteration count, e.g. `KES_FUZZ_ITERATIONS` env var, default a few
   thousand) issuing random `get_extent`/`put_extent`/`pin_extent`/
   `unpin_extent`/`mark_dirty`/`flush_extent`/`sync`/`invalidate`
   calls against a small, fixed pool of extent IDs (e.g. 10-20
   distinct ids so collisions/contention are frequent), checking after
   **every single operation** (not just at the end):
   - No entry's `ref_count` or `pin_count` goes negative (they're
     presumably unsigned, so "negative" means detecting an
     underflow-to-huge-value instead — assert bounds like `< 1000000`
     as a sanity ceiling rather than literally `>= 0` on an unsigned
     type).
   - `stats.entries_cached` matches the actual live hash-table
     population (you'll need a way to enumerate/count entries — check
     if `kes_cache_get_stats()` alone is sufficient of if you need a
     debug-only enumeration helper; if the latter, keep it
     test-file-local, don't add new public API surface for this).
   - `stats.memory_used` matches the actual sum of live buffer sizes.
   Log the RNG seed at the start of the run (same pattern as A.3.2)
   so any invariant violation is reproducible by rerunning with the
   same `KES_TEST_SEED`.
2. **Bitmap fuzz vs. naive reference.** Write a naive reference
   bitmap (a plain array of `bool`/`uint8_t`, one per bit, with
   trivial set/clear/test/find-free implemented as simple loops) in
   the test file itself. Drive both the real `kes_bitmap_t` and the
   naive reference through the same randomized sequence of
   `bm_set_range`/`bm_clear_range`/`bm_find_free`-equivalent calls
   (check `include/kes/kes_bitmap.h` for exact function names) with
   randomized ranges, including ranges that start/end exactly on a
   byte boundary, span multiple bytes, and cover the entire bitmap.
   After each operation, compare the two bit-for-bit (a linear scan
   comparison against the naive reference is fine here — it's the
   reference, not the thing under test for performance). This closes
   a real, current gap: `tests/test_kes_bitmap_full.c` has one
   direct test per function (§6.A coverage) but no
   comparison-against-a-reference fuzz test.

### A.7 — Performance smoke (§6.G, informational, low priority) and soak test (§6.H)

1. **Perf smoke** — optional/stretch for this pass. If you have time
   after A.1-A.6 and Track B, add a simple throughput measurement
   (sequential gets, random gets, mixed read/write against a
   reasonably sized cache) that just prints numbers — no
   pass/fail assertion needed, this is informational per
   `KES_HARDENING_PLAN.md` §6.G. Record a baseline run's numbers in
   your commit message or `PENDING_ITEMS.md` so a future regression
   has something to compare against. Do not block finishing this plan
   on this item — it's explicitly non-blocking in the source spec.
2. **Soak test.** Extend the mixed-workload concurrent test pattern
   (reuse `test_concurrent_sync_vs_get_put`'s shape from
   `tests/test_kes_cache.c`) into a new test that runs for a
   *configurable* duration via an environment variable, e.g.
   `KES_SOAK_SECONDS` (default something short like `2` so it stays
   fast and harmless as part of the normal `make test` run — this is
   the mechanism that satisfies `KES_HARDENING_PLAN.md` §6.H's
   "gated behind a `make soak` target or an environment variable"
   without needing to exclude it from the `TEST_SOURCES` wildcard).
   Add a `make soak` Makefile target that runs the normal test build
   but invokes this specific binary with `KES_SOAK_SECONDS=600` (10
   minutes) set, ideally layered under ASan or TSan per the spec —
   follow the existing `asan`/`tsan` targets' clean-rebuild pattern
   and pick one (document which, and why, in the Makefile comment
   right above the target) rather than trying to parameterize the
   sanitizer choice for this first pass. Watch during a real 10-minute
   run for: slow leaks (compare `stats.memory_used` drift against
   actual live data at intervals), and counters drifting out of sync
   (same invariant checks as A.6.1, run periodically rather than after
   every op given the duration). This is the one task in this plan
   you should not consider "done" without actually having run the
   full 10-minute `make soak` at least once and reporting what you
   observed — a soak test that's never been soaked is not verified.

---

## 4. Track B — Phase 6 docs truth pass

`PENDING_ITEMS.md`'s "Resolved" section confirms `README.md` and
`docs/CONTINUATION_PROMPT.md` are already corrected. The following 8
files under `docs/` are not yet checked/corrected — go through each,
grep `src/*.c` for the specific function/feature names before deciding
what to cut, per `AGENTS.md`'s "when in doubt, grep, don't trust the
doc" rule. Do this as one file per commit (or a few small, clearly-
labeled commits), not one giant docs commit — a reviewer should be
able to see exactly what changed in which file and why.

For each file, at minimum find and correct/remove/annotate:

1. **`docs/KES_API_Reference.md`** — check for LFU/Clock API
   descriptions (confirmed stale reference exists around policy
   documentation per the last review's line citations); confirm every
   documented function signature actually matches
   `include/kes/*.h` (a drift check, not just a feature-claim check).
2. **`docs/KES_Design_Document.md`** — confirmed stale claims: buddy-
   system allocator, flash-zone/wear-leveling features (multiple line
   citations from the last review — re-grep for "buddy" and "wear"
   case-insensitively to find current line numbers, they may have
   shifted). Replace with a description of the actual first-fit-only
   allocator, or an explicit "planned, not implemented" framing if the
   design intent is worth preserving for future work.
3. **`docs/KES_Project_Structure.md`** — confirmed to list source files
   that don't exist (`kes_allocator_buddy.c`, `kes_cache_lfu.c`,
   `kes_wear_leveling.c`, or similar — re-check exact names, they may
   differ slightly). Replace with the real 3-file structure
   (`kes_bitmap.c`, `kes_storage.c`, `kes_cache.c`) matching
   `AGENTS.md`'s "Module layering" section.
4. **`docs/kes_cache_design.md`** — confirmed stale LFU/Clock claims at
   multiple points. This is likely the file most worth a careful pass
   since it's cache-design-specific — align it with the actual
   `kes_cache_policy_t` behavior (`KES_CACHE_LRU` only, others
   rejected at `create()`), and the actual `kes_cache_sync`/
   `invalidate`/`reset_stats`/`start` semantics as implemented (cite
   `PENDING_ITEMS.md`'s "Resolved" section for the precise, verified
   semantics rather than re-deriving them from source yourself).
5. **`docs/README_Implementation.md`** — confirmed stale LFU claims.
   Check for other implementation-status claims (background sync,
   eviction) and align with `PENDING_ITEMS.md`.
6. **`docs/EDGE_DEVICE_OPTIMIZATION_PROMPT.md`** — confirmed stale
   flash-optimization claims. This one may be legitimately aspirational
   design-intent content (it reads as a prompt for *future* work, per
   its filename) — if so, the fix may be adding a clear banner at the
   top ("this describes planned, not-yet-implemented work — see
   `AGENTS.md`/`PENDING_ITEMS.md` for current state") rather than
   rewriting its content, since the content itself may still be a
   valid future design sketch. Use judgment; note which approach you
   took.
7. **`docs/PROJECT_OVERVIEW_KES.md`** — confirmed stale buddy-allocator,
   flash, and LFU/Clock claims at multiple line citations (largest
   number of stale references of any file per the last review — budget
   more time for this one). Cross-check every feature claim against
   `AGENTS.md`'s "Ground truth" section.
8. **`docs/TESTS_AND_EXAMPLES.md`** — update test counts/binary names
   to match current reality (70/70 across 6 binaries, or whatever the
   count is at the time you do this — re-run `make test` and use the
   fresh number, don't copy last session's number without
   re-verifying per rule 0.1) and confirm example code snippets, if
   any, still compile/match current API signatures.

**Acceptance for Track B:** every file above either corrected to match
verified current behavior, or (for genuinely-aspirational content like
item 6) clearly banner-labeled as design-intent rather than current
state — and no file should claim as implemented anything that isn't,
per a fresh `grep` check against `src/*.c`.

---

## 5. Suggested execution order

Tracks A and B are independent and can interleave, but within Track A:

1. A.1 and A.2 first (edge cases) — cheapest, highest coverage-per-effort,
   and A.1.2's block_size validation fix (if you take the recommended
   path) is a real, small, easily-reviewed code change worth landing
   early and separately.
2. A.4 (fault injection) and A.5 (crash-consistency) next — both are
   fairly mechanical once the harness patterns from A.1 exist.
3. A.6 (fuzz) — benefits from the invariant-checking patterns you'll
   have already written for A.4/A.5.
4. A.3 (concurrency scaling) and A.7 (soak) last — both are the most
   time-expensive to actually run (stress loops, a real 10-minute
   soak) and most likely to surface a genuine bug that needs its own
   follow-up cycle, so leave room after them before declaring the
   whole plan done.
5. Track B (docs) can happen any time, including in parallel with
   Track A by a separate session/agent, since it touches none of the
   same files.

**Final acceptance for this entire plan:** `make check-all` passing
with the full expanded suite (paste the output), plus Track B's 8
files corrected, plus this plan file and `PENDING_ITEMS.md` both
updated per §6 below.

---

## 6. Bookkeeping — how to mark progress

Do **not** edit "done" statuses into this file's task descriptions
directly (that risks this file drifting the same way
`docs/CONTINUATION_PROMPT.md` once did). Instead:

- As each numbered task (A.1.1, A.1.2, ... A.7.2, B.1 ... B.8)
  completes with passing, pasted evidence, add one line for it to
  `PENDING_ITEMS.md`'s Phase 5 section (or a new Phase 5/6 subsection
  if the existing prose structure gets unwieldy — reorganizing that
  file's structure is fine, losing information from it is not),
  stating what was added/changed and where, same style as the
  existing "Resolved" entries in that file.
- Once **all** of Track A and Track B are complete and
  `make check-all` is clean, update `AGENTS.md`'s "Ground truth"
  section to remove the "Phase 5 (test expansion, partially done)"/
  "Phase 6 (docs, partially done)" language and state both are
  complete, with the same evidence-citation style the rest of that
  section already uses (test counts, specific function/file
  references, not vague "everything's done now" language).
- If you stop partway through this plan (context limit, handoff to
  another session), leave a one-line note at the very top of this
  file under a `## Progress` heading you add, listing exactly which
  numbered tasks are done vs. in-progress vs. not-started, so the
  next session doesn't have to re-derive it from git log.
