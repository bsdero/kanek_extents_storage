# Pending Items

Tracks progress against `KES_HARDENING_PLAN.md`'s phases. As of this
writing, Phases 1-4 are complete (bug fix, sanitizer tooling, the
four missing cache functions, capacity enforcement/eviction) and the
P0 concurrency bug is fixed; Phase 5 (test expansion) and Phase 6
(docs truth pass) are partially done -- see their sections below for
exactly what's covered and what's still missing. Read
`KES_HARDENING_PLAN.md` in full before picking up any remaining
item -- it is still the ground-truth work order for *how* to
implement each piece correctly, even though most of it now describes
work already done.

Verified state as of this writing: `make check-all` (normal build +
ASan+UBSan + TSan + Valgrind) passes clean -- 70/70 tests across
`test_kes_minimal`, `test_kes_bitmap_full`, `test_kes_storage_full`,
`test_kes_cache`, `test_kes_cache_full`, `test_kes_multiprocess`; 0
leaks (Valgrind), 0 races (TSan), 0 memory-safety errors (ASan+UBSan).
Do not assume that stays true without rerunning it -- see
`KES_HARDENING_PLAN.md` §0's standing rule about pasted evidence.

---

## Resolved

### P0 -- concurrent cache-miss duplicate hash-table entries (FIXED)

**Concurrent cache-miss on the same extent ID creates duplicate
hash-table entries.** Fixed in commit `dd39a85` ("Fixed race
conditions").

- Was: `hash_find()` and `hash_insert()` were two independent
  operations with no lock held across both, so two threads racing a
  miss on the same uncached `id` could each build and insert their
  own `kes_extent_entry_t`, leaving two distinct cache entries (and
  two `read_extent` I/O calls) for one extent.
- Fix: `hash_find()`/`hash_insert()` were replaced by
  `hash_find_or_insert()` (`src/kes_cache.c`), which holds a single
  bucket lock across the "does an entry already exist" check and the
  insert. `kes_cache_get_extent()`'s miss path now builds a candidate
  entry in `KES_EXTENT_LOADING` state and publishes it through that
  function; a thread that loses the race gets back the winner's
  entry instead of inserting its own, and waits it out the same way
  the cache-hit path already waits out `KES_EXTENT_LOADING`.
- Test coverage: `Concurrent Miss No Duplicate Entry` in
  `tests/test_kes_cache.c` asserts only one entry/one `read_extent`
  call results from a multi-thread race on the same id.

### Phase 3 -- four missing `kes_cache.c` functions (FIXED)

`kes_cache_sync()`, `kes_cache_invalidate()`, `kes_cache_reset_stats()`,
and `kes_cache_start()` are all implemented in `src/kes_cache.c`,
matching `KES_HARDENING_PLAN.md` §4's specified semantics. Notable
deviations from a naive reading of §4, each because testing under
ASan/TSan surfaced a real hazard the plan didn't spell out:

- The shared flush-then-evict sweep (§4.1/§4.4's "factor into one
  internal static function") is `cache_sweep()` plus a per-entry
  helper `sweep_flush_and_maybe_evict()`, and a related helper
  `make_room_for_new_entry()` for Phase 4's eviction-on-miss (same
  per-entry flush/evict primitive, `try_evict_entry_locked()`, reused
  by both).
- The plan's suggested pattern -- "bump `ref_count` to pin the node
  you're mid-walk on" -- is NOT what the code does. Doing exactly
  that produced a real, ASan-confirmed heap-use-after-free during
  development: `ref_count` is modified under `entry->lock` by
  pre-existing code (`kes_cache_get_extent()`'s hit path,
  `kes_cache_put_extent()`, `kes_cache_mark_dirty()`,
  `kes_cache_pin_extent()`/`kes_cache_unpin_extent()`), so pinning it
  under `cache_lock` too was a genuine data race between two
  independent lock domains on the same field, not just a logic
  error. The traversal pin uses a new field, `lookup_pins`
  (`kes_extent_entry_t`, `include/kes/kes_cache.h`), which is only
  ever touched via `__atomic_*` builtins and is also what
  `hash_find()`/`hash_find_or_insert()` use to protect a
  found-but-not-yet-locked entry from a concurrent evictor. See the
  `try_evict_entry_locked()` doc comment (`src/kes_cache.c`) for the
  full three-hazard explanation (lookup-race, traversal-race, and a
  third one below) and why the eligibility check happens once, under
  both the bucket lock and `cache_lock` simultaneously.
- A third, unrelated hazard TSan caught separately:
  `pthread_cond_wait()` (the `KES_EXTENT_LOADING` wait loop in
  `kes_cache_get_extent()`) internally unlocks `entry->lock` while
  parked, so a waiter does not continuously hold the lock the way the
  code's structure suggests -- `ref_count` can independently reach 0
  during exactly that window. A new `cond_waiters` counter
  (`kes_extent_entry_t`) closes this; `try_evict_entry_locked()`
  refuses to evict while it's nonzero.
- `kes_cache_create()` rejecting `KES_CACHE_LFU`/`KES_CACHE_CUSTOM`
  (originally a Phase 4/§5.3 item, implemented alongside Phase 3)
  returns `NULL`, not `KES_ERROR_INVALID` as §5.3 literally says --
  `kes_cache_create()`'s signature returns `kes_cache_t *`, not
  `int`, so `NULL` is its only failure signal, consistent with every
  other validation failure in that function.

Test coverage: `Cache Sync`, `Cache Sync Keeps Referenced Entries`,
`Cache Invalidate`, `Cache Invalidate Discards Dirty Data`,
`Cache Reset Stats`, `Cache Start Background Flush` (proves automatic
flushing actually happens, not just that the thread doesn't crash),
`Cache Start Rejects Zero Threads` in `tests/test_kes_cache.c`.
Passing under normal build, ASan+UBSan, TSan (5 consecutive clean
runs during development), and Valgrind.

### Phase 4 -- eviction / capacity enforcement (FIXED)

`kes_cache_get_extent()`'s miss path calls `make_room_for_new_entry()`
before allocating a new entry, which walks the LRU list from
`lru_tail` toward `mru_head`, flushing and evicting entries with
`ref_count == 0 && pin_count == 0` until `config.max_entries`/
`config.max_memory` are satisfied, incrementing `stats.evictions` per
eviction. Returns `KES_ERROR_BUSY` (not a silent overshoot) if it
can't free enough room -- matching §5.2's recommendation.

Documented, deliberate deviation from strict atomicity (§5.2 allows
this as an alternative to a global lock): the check-then-evict-then-
insert sequence is not atomic against *other* concurrent misses on
*different* ids, so a burst of simultaneous misses can transiently
overshoot the configured limit by a small, bounded amount. See
`make_room_for_new_entry()`'s doc comment in `src/kes_cache.c`.

Test coverage: `Cache Eviction Respects Max Entries` (fills a
16-entry cache with 40 distinct extents, asserts `entries_cached`
never exceeds `max_entries` and `evictions > 0`),
`Cache Pinned Entries Never Evicted` (4 pinned entries survive being
pushed 40 entries past `max_entries` by other traffic),
`Cache Get Extent Busy When Full And Pinned` (BUSY when eviction
truly can't free room), `Cache Create Rejects Unimplemented Policy`
(LFU/CUSTOM rejected, LRU accepted) -- all in `tests/test_kes_cache.c`.

### Concurrency regression coverage added alongside Phases 3/4

`Concurrent Sync vs Get/Put` in `tests/test_kes_cache.c` -- 4 threads
doing get/put and 2 threads calling `kes_cache_sync()` in a loop, all
against a small, overlapping pool of extent ids under a
`max_entries`-constrained cache (forcing real eviction pressure, not
just flushing). This is the test that actually found the three
hazards described in the Phase 3 entry above; it's
`KES_HARDENING_PLAN.md` §6.C's "highest-value single test" for this
codebase. Passing under ASan+UBSan and TSan (5+ consecutive clean
runs each during development).

### Cross-process synchronized extent I/O test (ADDED)

`tests/test_kes_multiprocess.c` (`Cross Process Sync IO`, 1/1) covers
part of the §6.B "concurrent open of the same storage file from two
`kes_storage_t*` instances" gap called out below: a real `fork()` (two
OS processes, not threads) each independently call
`kes_storage_open()` on the same backing file, then take turns
writing/reading a distinguishable payload through
`kes_extent_write()`/`kes_extent_read()`, with turns strictly ordered
by two POSIX semaphores in an anonymous `MAP_SHARED` mapping. Confirms
content one process writes is correctly observed by another process's
independently opened handle once access is externally synchronized.
Passing 20/20 consecutive runs, and clean under ASan+UBSan and TSan.

**This does not close the §6.B item** -- it only proves the
synchronized case; the two processes are never allowed to race each
other, so it says nothing about what happens if they do (each has its
own in-memory bitmap loaded once at `open()` time and only flushed on
`kes_storage_sync()`/`close()`, so unsynchronized concurrent
allocation from two processes against the same file is still expected
to corrupt bitmap/descriptor state -- this remains untested and
unguarded).

### Phase 6 -- documentation truth pass (DONE)

`README.md` and `docs/CONTINUATION_PROMPT.md` were corrected to
match the state above (badges, feature lists, a "Known Limitations"
section in each). `docs/CONTINUATION_PROMPT.md` in particular was
substantially rewritten -- it previously claimed flash zones, GC,
and wear leveling as complete, which was never true at any point in
this project's history.

The remaining 8 `docs/` files (`plan_phase5.md` Track B, B.1-B.8) have
now been checked against `src/*.c`/`include/kes/*.h` directly and
corrected, one file per commit on the `worktree-agent-a18355b7b9c32e9b4`
branch (see that branch's log for exact diffs):

- **B.1 `KES_API_Reference.md`**: full rewrite. Removed documentation
  for a Flash-Specific API, Zone Management API, Garbage Collection
  API, Wear Leveling API, hardware-profile loading, and a
  Serialization API -- none exist in `include/kes/*.h`. Corrected
  every struct field list against the real headers (drift check, not
  just a feature-claim check, per the plan's B.1 instruction) and
  added an entirely new "Cache API" section (`kes_cache.h` was
  previously undocumented here despite being the most-tested layer).
- **B.2 `KES_Design_Document.md`**: added a standing-note banner and
  marked every aspirational section (buddy-system/slab/log-structured/
  hybrid allocation, flash zones, GC, wear leveling, the profile-based
  build system) as explicit "design intent, not implemented" rather
  than deleting it. Corrected the allocation-engine and cache-eviction
  sections to describe the actual first-fit-only allocator and
  LRU-only cache.
- **B.3 `KES_Project_Structure.md`**: full rewrite, replacing the
  fictional multi-directory layout (`src/core/`, `src/flash/`,
  `src/zones/`, `src/platforms/*/`, `tools/kes-*`, a profile-driven
  build system) with the real 3-file structure
  (`kes_bitmap.c`/`kes_storage.c`/`kes_cache.c`) and flat root
  `Makefile`, matching `AGENTS.md`'s "Module layering" section.
- **B.4 `kes_cache_design.md`**: fixed the config struct's field name
  (`policy`, not `eviction_policy`), added the missing
  `KES_EXTENT_ERROR` state, documented `kes_cache_invalidate`/
  `reset_stats`/`get_stats`/`set_io_callbacks` (previously omitted
  entirely) citing this file's own "Resolved" section above, corrected
  the hash-collision description (separate chaining, not robin hood
  hashing + linear probing) and the thread-safety description
  (fine-grained locking, not lock-free), and fixed both config
  examples (the server one used the rejected `KES_CACHE_LFU`).
- **B.5 `README_Implementation.md`**: fixed the "LRU/LFU/Custom"
  eviction claim and the "Custom Eviction Policy" section (now marked
  NOT IMPLEMENTED), removed references to a nonexistent `make ...
  edge` build target and `make benchmark`/`./build/test_runner`, and
  fixed the server config example's use of the rejected
  `KES_CACHE_LFU` policy.
- **B.6 `EDGE_DEVICE_OPTIMIZATION_PROMPT.md`**: banner-labeled rather
  than rewritten (it reads as legitimate forward-looking design intent
  for a future session, per its own filename/framing, matching the
  plan's guidance to prefer a banner for genuinely aspirational
  content) -- also corrected its stale "9/9 tests passing" status line
  to point at the current 70/70-across-6-binaries baseline.
- **B.7 `PROJECT_OVERVIEW_KES.md`**: the file with the most stale
  references per the last review. Corrected buddy-system/log-
  structured allocation, compressed-bitmap and "O(1) find_free"
  claims, ACID/checksum/rollback claims (none exist), the error-code
  list (was missing `KES_ERROR_EXISTS`/`_BUSY`), removed the
  fabricated performance-benchmark table (no benchmark suite exists to
  have produced those numbers), fixed the "Advanced Features" section
  ("Multi-Policy Eviction: LRU, LFU, Clock" and "lock-free" were both
  wrong), and fixed the Quality Assurance/CI sections (no cppcheck,
  clang-analyzer, or CI configuration exists in this repo).
- **B.8 `TESTS_AND_EXAMPLES.md`**: updated to the 70/70-across-6-
  binaries baseline (confirmed by a fresh `make test` run immediately
  before this Phase 6 session started), replacing the stale
  "4/6 Passing, 2 known issues" cache-test status
  (`test_kes_cache.c` is actually 23/23) and documenting the four test
  binaries this file previously omitted entirely
  (`test_kes_bitmap_full.c`, `test_kes_storage_full.c`,
  `test_kes_cache_full.c`, `test_kes_multiprocess.c`). Also corrected
  the "O(1) for first-fit" allocation-speed claim and the unverified
  macOS/ARM64 "covered" platform claims.

Per `plan_phase5.md` §6 (bookkeeping), `AGENTS.md`'s "Ground truth"
section should be updated to drop the "Phase 6 (docs, partially done)"
language once this work has merged to the main checkout -- not done
here, since this pass ran in an isolated worktree in parallel with
other Track A (test expansion) work touching the same file; left for
manual reconciliation at merge time.

---

## Phase 5 -- test expansion (P1/P2, PARTIALLY DONE)

Functional coverage per public function now exists for all three
headers: `test_kes_bitmap_full.c` (10/10), `test_kes_storage_full.c`
(15/15), `test_kes_cache_full.c` (12/12) -- one direct test per
function, satisfying most of §6.A. `test_kes_cache.c` (23/23) adds
edge-case and concurrency coverage beyond that, including several
items from §6.B/§6.C: no-callback-registered paths, ref_count-leak-
on-load-failure, the P0 duplicate-insert race, and the Phase 3/4
concurrency regression test above. `test_kes_multiprocess.c` (1/1,
see "Cross-process synchronized extent I/O test" above) adds the
first real multi-process (`fork()`-based) coverage, distinct from
every other test binary's thread-based concurrency.

**Not done** -- see `KES_HARDENING_PLAN.md` §6 for full detail on
each:

- §6.B edge cases not yet covered: NULL for every individual pointer
  parameter (only some functions tested this way);
  `block_count = 0`/`UINT32_MAX`, `start_block = UINT64_MAX`, and
  `start_block * block_size` overflow combinations for the cache
  layer specifically (the storage layer's 32-bit overflow case *is*
  covered, `test_kes_extent_read_write_32bit_overflow` in
  `tests/test_kes_storage_full.c`); non-power-of-2 `block_size`
  validation; pin/unpin imbalance beyond a single pin/unpin pair;
  `kes_cache_destroy()` with an outstanding unreleased reference;
  operations on a cache between `kes_cache_stop()` and
  `kes_cache_destroy()`; hash-collision disambiguation
  (`kes_extent_equal()` actually used, not just the hash); storage
  layer at 100%-full-then-free-one-block; *unsynchronized* concurrent
  open/access of the same storage file from two `kes_storage_t*`
  instances (still undocumented, unguarded -- the *synchronized* case
  is now covered by `tests/test_kes_multiprocess.c`, see "Cross-process
  synchronized extent I/O test" above, but that test deliberately never
  lets the two processes race; a version that does race them and
  records the resulting corruption/behavior as a known limitation per
  §6.B's own guidance is still needed).
- §6.C further concurrency/stress: scaling the existing tests to more
  threads than CPU cores and higher iteration counts; a dedicated
  `kes_cache_destroy()`-during-concurrent-access test; running the
  concurrency suite 100+ times in a loop with a logged fixed random
  seed (a stress-test target, not yet added to the Makefile).
- §6.D fault injection: configurable-failure-mode I/O wrappers
  (fail on Nth call, or with a probability), `malloc`/`aligned_alloc`
  failure simulation, partial read/write simulation. None of this
  exists yet.
- §6.E storage persistence/crash-consistency: no-explicit-sync
  reopen behavior, truncated/corrupted descriptor detection, bit-
  flipped bitmap block detection. Not covered.
- §6.F randomized/fuzz-adjacent testing: a long random
  get/put/pin/unpin/mark_dirty/flush/sync/invalidate sequence with
  per-operation invariant checks, and randomized bitmap bit-range
  testing against a naive reference implementation. Not done.
- §6.G performance smoke tests: not done (informational only, not
  blocking).
- §6.H long-run soak test (a `make soak` target running the mixed
  workload for 10+ minutes under ASan/TSan): not done.

---

## Infrastructure notes for whoever picks this up

- `kanek_foundations` (KFL) is wired into the build as a sibling-repo
  reference: `make foundations-fetch` clones it to `../kanek_foundations`
  if missing, `-I../kanek_foundations/src` is already in `INCLUDES`,
  and `make foundations` builds `libkfl.a` on demand with CFLAGS you
  pass in (used so ASan/TSan variants stay instrumentation-consistent
  if/when something actually links it). **Nothing in `src/` includes
  or links it yet.**
- `CODING_STYLE.md` Rule 11 (log before every early-return failure
  path via `TRACE_ERR`/`TRACE_SYSERR`/`TRACE_ERRNO`) is now partially
  addressed: `kes_cache.c` (the no-read_extent-callback path, the
  load-failure path, the no-write_extent-callback path in
  `kes_cache_flush_extent()`, the cache-full-can't-evict path in
  `kes_cache_get_extent()`, `kes_cache_start()`'s
  already-running/`pthread_create`-failure paths, and the
  sweep/eviction flush-failure paths) and `kes_storage.c` (the
  double-free/invalid-extent path in `kes_extent_free()`) call
  `TRACE_ERR` today -- Phase 3/4's new functions were written
  Rule-11-compliant from the start, per the original plan here. Most
  early-return paths in `kes_bitmap.c`/`kes_storage.c` and some in
  `kes_cache.c` (e.g. every `KES_ERROR_INVALID`/`KES_ERROR_NOTFOUND`
  NULL/not-found check) still fail silently. `kanek_foundations/src/
  trace.h` provides the macros and is header-only for that subset
  (no linking needed, just `#include "trace.h"`, already reachable
  via the include path). Per AGENTS.md's "no drive-by rewrites"
  guidance, keep applying this to new functions and to existing
  functions only when you're already touching their body for another
  reason -- don't do a blanket sweep.
- Sanitizer/verification targets available: `make asan`, `make tsan`,
  `make valgrind`, `make sanitize-all`, `make check-all`. TSan
  binaries must be run under `setarch $(uname -m) -R` in this
  (WSL2) environment or they crash with "unexpected memory mapping"
  unrelated to any KES bug -- the `tsan` target already does this.
