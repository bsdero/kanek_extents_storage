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

### Phase 6 -- documentation truth pass (PARTIALLY DONE)

`README.md` and `docs/CONTINUATION_PROMPT.md` were corrected to
match the state above (badges, feature lists, a "Known Limitations"
section in each). `docs/CONTINUATION_PROMPT.md` in particular was
substantially rewritten -- it previously claimed flash zones, GC,
and wear leveling as complete, which was never true at any point in
this project's history.

Not done: a pass over the other `docs/` files
(`KES_API_Reference.md`, `KES_Design_Document.md`,
`KES_Project_Structure.md`, `kes_cache_design.md`,
`README_Implementation.md`, `EDGE_DEVICE_OPTIMIZATION_PROMPT.md`,
`PROJECT_OVERVIEW_KES.md`, `TESTS_AND_EXAMPLES.md`) -- these likely
still contain aspirational claims (flash zones, GC, wear leveling,
multi-policy eviction) inherited from the same source as the old
`CONTINUATION_PROMPT.md`. AGENTS.md's existing guidance to treat
`docs/` as design-intent rather than ground truth still applies to
whichever of these haven't been checked.

---

## Phase 5 -- test expansion (P1/P2, PARTIALLY DONE)

Functional coverage per public function now exists for all three
headers: `test_kes_bitmap_full.c` (10/10), `test_kes_storage_full.c`
(15/15), `test_kes_cache_full.c` (12/12) -- one direct test per
function, satisfying most of §6.A. `test_kes_cache.c` (24/24 as of
Track A.3.1, see below) adds
edge-case and concurrency coverage beyond that, including several
items from §6.B/§6.C: no-callback-registered paths, ref_count-leak-
on-load-failure, the P0 duplicate-insert race, and the Phase 3/4
concurrency regression test above. `test_kes_multiprocess.c` (1/1,
see "Cross-process synchronized extent I/O test" above) adds the
first real multi-process (`fork()`-based) coverage, distinct from
every other test binary's thread-based concurrency.

### Phase 5 progress -- Track A.3.1: `kes_cache_destroy()` vs concurrent access (DONE, real bug found and FLAGGED, NOT FIXED)

`Cache Destroy Races Concurrent Access` added to `tests/test_kes_cache.c`
(plan_phase5.md Track A.3.1). Reading `kes_cache_destroy()`
(`src/kes_cache.c`) first, before writing any assertion, established
its actual contract: it calls `kes_cache_stop()` (which only joins
*background* flush threads started by `kes_cache_start()`), then
walks the LRU list under `cache_lock`, freeing each entry's data
buffer, destroying `entry->lock`/`entry->cond`, and `free()`ing the
entry struct -- without ever acquiring `entry->lock` while doing so
and without checking `ref_count`/`pin_count` first. Nothing waits
for, rejects, or otherwise coordinates with a caller still inside
`kes_cache_get_extent()`/`kes_cache_put_extent()` on the same cache.
The actual, currently-inferred contract is therefore "the caller
must quiesce every other thread using this cache before calling
`kes_cache_destroy()`" -- not "`destroy()` is safe to call
concurrently." `include/kes/kes_cache.h` does not state this
explicitly today.

**This is a genuine, confirmed bug, per rule 0.3 reported here and
NOT fixed in this commit.** A standalone, minimal reproduction
(one thread in a tight `get_extent()`/`put_extent()` loop,
`kes_cache_destroy()` called from another thread ~2ms later) was
built outside the test tree and run under both sanitizers:

- **ASan**: heap-use-after-free -- `lru_remove()`
  (`src/kes_cache.c:125`) reads a `kes_extent_entry_t` already freed
  by `kes_cache_destroy()` (`src/kes_cache.c:779`). Reproduced on
  every run of the standalone repro.
- **TSan**: multiple data races on the freed entry's lock/fields
  (`kes_cache_destroy()` vs. `kes_cache_get_extent()`/
  `kes_cache_put_extent()`), plus explicit "heap-use-after-free" and
  "use of an invalid mutex (e.g. uninitialized or destroyed)"
  reports -- all pointing at the same destroy()-vs-get/put
  interleaving.

The automated `tests/test_kes_cache.c` version of this race runs the
racer threads and `kes_cache_destroy()` inside a forked, disposable
child process (mirroring `test_cross_process_racing_io()`'s pattern
in `tests/test_kes_multiprocess.c`) precisely so this real,
unfixed use-after-free cannot nondeterministically crash the whole
test binary: the parent test process only asserts that it observes
the child end (cleanly or via a sanitizer-reported crash) without
hanging or itself crashing, and prints which outcome occurred. Under
`make tsan` this reliably shows the child exiting with TSan's
nonzero status (66) on every observed run; under `make asan` it has
not been observed to reproduce inside this specific multi-threaded
test-binary timing, even though the same bug reproduces every time
in the separate, simpler standalone repro described above (a known
ASan-detection timing artifact, not evidence the bug is
ASan-specific). `make test`/`make asan`/`make tsan` all still pass
71/71 -- the test's own assertions do not depend on the race
actually triggering.

**Not fixed** -- deciding between "make `kes_cache_destroy()`
synchronize with in-flight callers" and "document
caller-must-quiesce-first and let callers enforce it" is a real API
contract decision, not a bug fix, and is out of scope for this
commit per rule 0.3. Whoever picks this up next should read the
`Cache Destroy Races Concurrent Access` test's doc comment in
`tests/test_kes_cache.c` (immediately above
`test_cache_destroy_races_concurrent_access()`) for the full
citation trail before deciding.

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
  `kes_cache_destroy()`-during-concurrent-access test -- **DONE, see
  "Phase 5 progress -- Track A.3.1" above**; running the concurrency
  suite 100+ times in a loop with a logged fixed random seed (a
  stress-test target, not yet added to the Makefile).
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
