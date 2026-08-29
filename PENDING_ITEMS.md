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

### Track A.6 -- randomized/fuzz-adjacent testing (DONE)

New `tests/test_kes_fuzz.c` (2/2), closing plan_phase5.md's Track
A.6 / `KES_HARDENING_PLAN.md` §6.F:

- `test_cache_invariant_fuzz()` (A.6.1): a long randomized loop
  (`KES_FUZZ_ITERATIONS` env var, default 3000) issuing random
  `get_extent`/`put_extent`/`pin_extent`/`unpin_extent`/`mark_dirty`/
  `flush_extent`/`sync`/`invalidate` calls against a fixed 20-id
  extent pool over a 16-entry (`KES_CACHE_MIN_ENTRIES`) cache, so
  eviction pressure and per-id contention are frequent. After
  *every* operation it walks the cache's hash table directly
  (`kes_cache_t`/`kes_extent_entry_t` are fully defined in
  `kes_cache.h`, not truly opaque, so this needed no new public API
  surface) to confirm `ref_count`/`pin_count` stay under a sanity
  ceiling and that `stats.entries_cached`/`stats.memory_used` match
  the live hash-table population exactly. `KES_TEST_SEED` (default
  `time(NULL)`) is printed at the start of the run for
  reproducibility. No invariant violation found across the default
  run plus several fixed-seed reruns at higher iteration counts (1,
  42, 999999, 7, 424242 at 5000-8000 iterations each).
- `test_bitmap_fuzz_vs_reference()` (A.6.2): drives `kes_bitmap_t`
  and a naive `uint8_t`-array reference bitmap through the same
  randomized `kes_bitmap_set_range`/`kes_bitmap_clear_range`/
  `kes_bitmap_find_free` sequence (251-bit bitmap, deliberately not
  a multiple of 8; every 10th iteration forces a whole-bitmap or
  byte-boundary-exact/multi-byte range rather than a fully random
  one) and compares them bit-for-bit, plus `free_bits` against a
  fresh naive scan, after every operation. The naive reference's
  `find_free` mirrors the real function's exact hint-forward-then-
  wrap search order, so success/failure and the returned start bit
  are compared directly. No mismatch found.

Verified: `make test` 72/72 across all 7 binaries (was 70/70
baseline; +2 from this file). `make asan` (full clean rebuild, all 7
binaries including `test_kes_fuzz`) exits 0 with no
AddressSanitizer/UBSan/LeakSanitizer output anywhere in the log.

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

### Phase 5 progress -- Track A.3.2: `make stress` harness (DONE)

New `stress` target in the `Makefile` (plan_phase5.md Track A.3.2):
repeatedly runs `test_kes_cache` (threads) and `test_kes_multiprocess`
(`fork()`) `STRESS_RUNS` times each (default 100, e.g. `make stress
STRESS_RUNS=500`), stopping and reporting the failing run number and
captured output on the first non-zero exit rather than continuing
past a failure. Plain rebuild by default; `SANITIZER=asan` or
`SANITIZER=tsan` layers the same flags the `asan`/`tsan` targets use
on top of a clean rebuild first, since a stress loop's whole point is
surfacing a rare interleaving and both sanitizers are far more likely
than a plain build to turn one into a visible failure.
`KES_TEST_SEED` was not wired up: grepped both `tests/test_kes_cache.c`
and `tests/test_kes_multiprocess.c` first and confirmed neither calls
`rand()`/`srand()` or a randomized `usleep()` -- their only
nondeterminism is real OS thread/process scheduling, so there is no
seed to make reproducible today; noted in the Makefile comment for
whoever adds seeded randomization to either file later.

Actually run, not just written -- pasted evidence:
- Plain: `make stress STRESS_RUNS=100` -- `test_kes_cache: 100/100
  runs passed`, `test_kes_multiprocess: 100/100 runs passed`,
  `stress: ALL RUNS PASSED`.
- `make stress SANITIZER=tsan STRESS_RUNS=30` -- clean rebuild under
  TSan, `test_kes_cache: 30/30 runs passed`, `test_kes_multiprocess:
  30/30 runs passed`, `stress: ALL RUNS PASSED` -- no TSan report
  (this run does not exercise the known A.3.1 `kes_cache_destroy()`
  race, since `test_cache_destroy_races_concurrent_access()` forks
  its racing repro into a disposable child process specifically so it
  cannot fail the parent test binary's exit code).
- `make stress SANITIZER=asan STRESS_RUNS=30` -- clean rebuild under
  ASan+UBSan, `test_kes_cache: 30/30 runs passed`,
  `test_kes_multiprocess: 30/30 runs passed`, `stress: ALL RUNS
  PASSED` -- no ASan/UBSan finding.
- `make test` after this commit: 91/91 (unchanged from the A.3.1
  merge -- this task only adds a Makefile target, no new test cases).

This completes A.3 in full.

### Phase 5 progress (this pass) -- plan_phase5.md Track A.1/A.2 (DONE)

All 9 sub-items of A.1 and both sub-items of A.2 are complete, each
its own commit with pasted `make test`/`make asan`/`make tsan`
evidence:

- **A.1.1** (`tests/test_kes_cache_edge.c`, `test_null_parameter_
  checks`): closes the NULL-parameter gaps left after checking
  `test_kes_cache_full.c` function-by-function --
  `pin_extent`/`unpin_extent`/`mark_dirty`/`flush_extent`'s `id`
  parameter, `invalidate`'s `cache` and `id`, `sync`/`reset_stats`/
  `start`'s `cache` (these three previously only existed inside
  `test_kes_cache_full.c`'s `#if 0` Phase-3-pending block, so were
  never actually compiled/run), and `set_io_callbacks`'s three
  function-pointer parameters individually (observed: none are
  validated, all accepted including NULL -- error is deferred to
  use time).
- **A.1.2** (`src/kes_cache.c`, `kes_cache_get_extent()`; doc comment
  in `include/kes/kes_cache.h`): the plan's premise ("config.block_size
  power-of-2 guard in `kes_cache_create()`") does not match this
  codebase -- `kes_cache_config_t` has no `block_size` field at all;
  it lives on the per-call `kes_extent_id_t` instead. Applied the
  same `KES_IS_POWER_OF_2`/`KES_MIN_BLOCK_SIZE`/`KES_MAX_BLOCK_SIZE`
  guard where `block_size` actually appears: `kes_cache_get_extent()`
  now rejects an invalid `id->block_size` with `KES_ERROR_INVALID`
  before any allocation is attempted.
- **A.1.3** (`test_block_count_zero_and_max`): `block_count == 0`
  currently produces a valid 0-byte cached entry (`aligned_alloc(64,
  0)` returns non-NULL on this platform) -- asserted as observed
  behavior, flagged below as a real but minor, not-yet-fixed gap.
  `block_count == UINT32_MAX` is cleanly rejected as
  `KES_ERROR_BUSY` (the `max_memory` capacity check in
  `make_room_for_new_entry()` fails before any allocation is
  attempted for the resulting ~16TiB request) -- not the `NOMEM` the
  original plan speculated.
- **A.1.4** (`test_start_block_near_max_no_size_wrap`): confirmed by
  reading `src/kes_cache.c` that `id->start_block` is only ever used
  for hashing/equality/logging in this module, never in size
  arithmetic; `extent_data_size()`'s `(size_t)block_count *
  block_size` is already 64-bit-safe. Test reuses
  `test_kes_extent_read_write_32bit_overflow`'s 600000/8192 numbers
  with a custom `max_memory` strictly between the wrapped (~591MiB)
  and true (~4.58GiB) sizes to make a hypothetical wrap
  distinguishable by return code; observed `KES_ERROR_BUSY`, matching
  the unwrapped true size.
- **A.1.5** (`test_pin_unpin_refcount_semantics`; doc comments for
  `kes_cache_pin_extent`/`kes_cache_unpin_extent` in
  `include/kes/kes_cache.h`): documents and tests that pinning is
  reference-counted (N pins require N unpins), that an entry pinned
  3x and unpinned only once survives real eviction pressure as a
  cache hit, and that both an extra unpin beyond the pin count and an
  unpin on a never-pinned entry are safe `KES_SUCCESS` no-ops.
- **A.1.6** (`test_destroy_with_outstanding_reference`): read
  `kes_cache_destroy()` first -- it frees every entry unconditionally,
  never checking `ref_count`/`pin_count`. Test gets an extent, never
  puts it, destroys the cache, asserts `KES_SUCCESS`/no crash, and
  does not dereference the now-dangling buffer afterward. Per the
  plan's rule, run under `make asan` specifically (not just plain
  `make test`) -- clean, 0 ASan findings.
- **A.1.7** (`test_ops_between_stop_and_destroy`): read
  `kes_cache_stop()` and grepped `cache->shutdown`'s other uses --
  confirmed it only tears down background threads and is read nowhere
  else, so `get_extent()`/`put_extent()` after `stop()` just work
  normally. Test asserts that directly.
- **A.1.8** (`test_empty_cache_no_ops`): `kes_cache_sync()` and
  `kes_cache_reset_stats()` on a freshly-created, never-populated
  cache are clean `KES_SUCCESS` no-ops with all-zero stats after.
  `invalidate()`-on-missing-id is already covered by
  `test_cache_invalidate` (`tests/test_kes_cache.c`), not duplicated.
- **A.1.9** (`test_hash_collision_disambiguation`): brute-forces a
  genuine `kes_extent_hash()` bucket collision (guaranteed by the
  pigeonhole principle, sweeping `2 * (bucket_mask + 1)` `start_block`
  values against a live cache's real `bucket_mask` --
  `struct kes_cache` is fully defined in `include/kes/kes_cache.h`,
  not opaque), inserts both colliding ids with distinguishable
  backing data, and confirms both are independently retrievable with
  correct, distinct data -- proving `kes_extent_equal()` actually
  disambiguates within a shared bucket, not just in isolation.
- **A.2.1** (new `tests/test_kes_storage_edge.c`,
  `test_exhaustion_then_free_and_reallocate`): drives a 2MB storage
  to actual bit-for-bit exhaustion (distinct from the existing
  single-over-large-request coverage in `test_kes_extent_allocate`),
  confirms the next allocation cleanly returns `KES_ERROR_NOSPACE`,
  cross-checks `kes_bitmap_get_stats()` against
  `storage->desc.used_blocks` (`kes_storage_t` is non-opaque) to
  confirm no bitmap/descriptor desync at exhaustion, then frees one
  extent and confirms reallocation succeeds and first-fits into the
  freed block.
- **A.2.2** (`tests/test_kes_multiprocess.c`,
  `test_cross_process_racing_io`): the deliberate-race counterpart to
  `test_kes_cross_process_sync_io` -- same fork()/shared-file setup
  with the semaphore turn-taking removed, both processes racing
  `kes_extent_allocate()`/`kes_extent_write()` with zero coordination
  (bounded, 20 ops/side). Does not assert correctness (this is the
  documented, unguarded gap below) -- only that the race completes
  without hanging or crashing either process. A third, non-racing
  read-only re-open after both processes exit makes the real
  corruption visible in the test log: each side's own self-reported
  free/used counts look internally consistent (each only ever saw its
  own private in-memory bitmap), but the real on-disk `used_blocks`
  is observed to be less than the combined allocations both sides
  believed succeeded -- one side's `kes_storage_sync()` silently
  clobbered the other's. Verified stable across repeated runs and
  clean under both `make asan` and `make tsan` (spawns a process, so
  run under both per the plan's concurrency rule) -- no crash, no
  ASan/TSan finding, in either.

Real gap found but deliberately NOT fixed in this pass, per rule 0.3
(no drive-by fixes -- reported instead): **`block_count == 0` is not
validated anywhere in `kes_cache_get_extent()`** and currently
produces a "successful" but nonsensical 0-byte cached entry rather
than being rejected with `KES_ERROR_INVALID`. Low severity (no crash,
no oversized-allocation risk, unlike the `UINT32_MAX` case which *is*
already handled cleanly) but worth a small follow-up guard alongside
the `block_size` check A.1.2 added.

`make test` after this pass: 81/81 (was 70/70 baseline -- 11 new
tests added: 9 in `tests/test_kes_cache_edge.c`, 1 in the new
`tests/test_kes_storage_edge.c`, 1 added to
`tests/test_kes_multiprocess.c`). `make asan`/`make tsan`: clean for
every test in this pass that required them (A.1.6, A.2.2).

### Phase 5 progress (this pass) -- plan_phase5.md Track A.4/A.5

New file `tests/test_kes_fault_injection.c` (A.4), one commit per
numbered sub-item with pasted `make test`/`make asan` evidence:

- **A.4.1** (`tests/test_kes_fault_injection.c`,
  `test_load_failure_hash_table_state` /
  `test_flush_failure_dirty_state`): a small, self-contained
  configurable-failure mock I/O harness (`fi_control_t`/
  `fi_should_fail()` -- fail on the Nth call, or fail on every call
  after the Nth). **Real, observed gap found and reported, NOT fixed
  (rule 0.3 -- production-code changes are out of scope for this
  file)**: after a load failure, `kes_cache_get_extent()` leaves the
  entry permanently in `KES_EXTENT_ERROR` state in the hash table
  (`stats.entries_cached` still counts it) -- a *second* call on the
  same id, even with the failure condition fully cleared, returns
  `KES_ERROR_IO` immediately without ever calling `read_extent()`
  again (proven via a call-count assertion on the fault harness). The
  only recovery path is an explicit `kes_cache_invalidate()` on that
  id before retrying; the test proves that recovery path works and
  that the hash table itself is not otherwise corrupted. Separately,
  `test_flush_failure_dirty_state` confirms
  `KES_HARDENING_PLAN.md` S4.1 point 1 exactly as documented for the
  `kes_cache_sync()` path (`KES_EXTENT_DIRTY` and `KES_EXTENT_ERROR`
  both set on a failed flush, entry not evicted, clean recovery on a
  later successful sync) -- and separately notes that
  `kes_cache_flush_extent()` (the direct single-entry flush call, a
  different code path) does NOT set `KES_EXTENT_ERROR` on failure the
  same way, only returns `KES_ERROR_IO` -- a real, minor behavioral
  inconsistency between the two flush paths, also reported rather than
  fixed. `make test` after this commit: 83/83 (was 81/81 -- 2 new
  tests in the new `tests/test_kes_fault_injection.c`).
- **A.4.2** (`tests/test_kes_fault_injection.c`,
  `test_malloc_failure_nomem`; `Makefile`'s `asan` target): requests a
  genuinely oversized (200GiB) extent against a cache configured with
  a large enough `max_memory` to pass the pre-allocation capacity
  check, so the miss path actually reaches `aligned_alloc()`, which
  fails for real (confirmed empirically: even 100GiB reliably fails
  with `ENOMEM` on this system). Asserts `KES_ERROR_NOMEM` is returned
  cleanly, `*buffer` stays `NULL`, and no partial state leaks
  (`entries_cached`/`memory_used`/`misses` all stay at 0, and the
  cache remains fully usable for a normal-sized extent afterward).
  Confirmed clean under plain `make test`. Under `make asan`,
  AddressSanitizer's default behavior for *any* out-of-memory
  allocation failure (not just requests over its internal
  max-supported-size cap) is to abort the process rather than return
  `NULL` -- confirmed empirically; `ASAN_OPTIONS=
  allocator_may_return_null=1` must be set in the environment *before
  process start* (a `setenv()` inside `main()` is too late -- also
  confirmed empirically). The `asan` Makefile target now sets this
  environment variable specifically when invoking
  `test_kes_fault_injection` (only that one binary -- every other
  test binary keeps ASan's default strict abort-on-OOM behavior, so a
  genuine unexpected OOM elsewhere is still loud). `make test` after
  this commit: 84/84. `make asan`: clean across all 84 tests,
  `KES_ERROR_NOMEM` returned as expected, no abort, no other finding.
- **A.4.3** (`tests/test_kes_fault_injection.c`,
  `test_partial_transfer_not_detected`): a mock `read_extent` that
  reports `KES_SUCCESS` while only actually copying 16 of 4096
  requested bytes. **Documented conclusion (plan_phase5.md's
  either-outcome-acceptable option, not a bug)**: this is NOT
  detected, and structurally cannot be at this layer -- the
  `read_extent`/`write_extent` callback contract
  (`include/kes/kes_cache.h`) is a plain `int` status code against a
  fixed `size` *input* parameter, with no bytes-actually-transferred
  *output* channel at all for `kes_cache_get_extent()` to check
  against. The test only ever reads back the 16 bytes the mock
  actually transferred (never the deliberately-uninitialized tail) to
  keep this Valgrind/MSan-safe for any future run. `make test` after
  this commit: 85/85 (was 84/84). This completes A.4 in full.
- **A.5.1** (new `tests/test_kes_crash_consistency.c`,
  `test_no_sync_reopen_durability`): two distinct observed behaviors,
  discovered while writing this test, not assumed going in. **Case
  1**: a storage file that has *never* been synced/closed even once
  since `kes_storage_create()` becomes completely UNOPENABLE after a
  crash -- `kes_storage_create()` never calls `kes_bitmap_save()` at
  create time (only `save_storage_descriptor()` for block 0), so the
  file's real physical size never reaches the bitmap region near the
  end of the device until a real sync/close happens; a crash before
  that makes `kes_storage_open()`'s `kes_bitmap_load()` read short and
  return `KES_ERROR_IO` (not `KES_ERROR_CORRUPT`). **Case 2**: once a
  file has been synced/closed at least once (fully laid out on disk),
  a *later* crash without a further sync reopens successfully but
  into stale, pre-crash bookkeeping -- confirmed by reading
  `kes_extent_write()`: raw extent DATA is always durable immediately
  (a direct, unbuffered `write()` syscall with no cache layer of its
  own), but `storage->desc.free_blocks`/`used_blocks` and
  `storage->bitmap` are only persisted by `kes_storage_sync()`/
  `kes_storage_close()`. Concretely demonstrated: a fresh allocation
  after such a reopen is handed the exact same blocks back (the
  bitmap thinks they're free) and silently overwrites the "forgotten"
  extent's still-physically-present data -- confirmed by reading it
  back through the original extent descriptor afterward.
  `include/kes/kes_types.h`'s `KES_STORAGE_SYNC` flag doc comment is a
  single line ("Synchronous I/O") with no explicit durability promise
  either way -- flagged as a Track B docs gap (out of scope for this
  pass), not fixed here. `make test` after this commit: 86/86 (was
  85/85 -- 1 new test in the new
  `tests/test_kes_crash_consistency.c`).
- **A.5.2** (`tests/test_kes_crash_consistency.c`,
  `test_truncated_and_corrupted_descriptor`): two distinct cases,
  confirmed to return two *different* error codes by reading
  `load_storage_descriptor()` first. Truncating the file to fewer
  bytes than `sizeof(kes_storage_descriptor_t)` returns
  `KES_ERROR_IO` (the `read()` byte count check fails before the
  magic-number check is ever reached) -- **not** `KES_ERROR_CORRUPT`,
  worth knowing if a caller tries to distinguish "corrupt" from
  "truncated/missing" by return code alone. Overwriting just the
  4-byte magic-number field in an otherwise-intact, correctly-sized
  descriptor returns `KES_ERROR_CORRUPT` as expected (pairs against
  case 1's different code and a distinct corruption pattern from
  `test_kes_storage_open_corrupt()`,
  `tests/test_kes_storage_full.c`, not duplicating it). Either way,
  `kes_storage_open()` fails cleanly (no crash, no `*storage` output)
  rather than proceeding with uninitialized/garbage geometry.
  `make test` after this commit: 87/87 (was 86/86).
- **A.5.3** (`tests/test_kes_crash_consistency.c`,
  `test_bitflipped_bitmap_block`): flips one specific bit (1->0,
  i.e. "used" -> "free") in the *on-disk bitmap region* (not the
  descriptor) for a block that is genuinely still allocated and holds
  live written data, then reopens. No corruption is detected at
  `kes_storage_open()` time (no bitmap checksum exists anywhere in
  this codebase, confirmed by reading `kes_bitmap_load()`/
  `kes_bitmap_save()`) -- characterized precisely:
  `kes_storage_get_stats()`'s `free_blocks`/`used_blocks` come from
  `storage->desc` (loaded from the untouched descriptor block) and so
  still report the *correct, pre-corruption* counts, while the live
  in-memory bitmap (`kes_bitmap_load()` recalculates `free_bits` by
  actually counting the loaded, corrupted bitmap bytes) now silently
  disagrees with those counts by exactly one bit -- a real, silent
  desync between the two redundant sources of truth, visible only by
  inspecting `storage->bitmap` directly (`kes_storage_t`/
  `kes_bitmap_t` are both non-opaque). Concretely demonstrated as a
  real double-allocation hazard, not just a bookkeeping curiosity: a
  fresh 1-block allocation hinted at that exact `start_block` is
  handed the *same, still-live* block back by the allocator (since
  the bitmap now thinks it is free), and writing the new allocation's
  data is shown to silently overwrite the original extent's still-
  valid data. `make test` after this commit: 88/88 (was 87/87). This
  completes A.5 in full.

**Not done** -- see `KES_HARDENING_PLAN.md` §6 for full detail on
each:

- ~~§6.C further concurrency/stress~~ DONE -- a dedicated
  `kes_cache_destroy()`-during-concurrent-access test, see "Phase 5
  progress -- Track A.3.1" above; the `make stress` target running
  `test_kes_cache`/`test_kes_multiprocess` `STRESS_RUNS` times (plain,
  ASan, and TSan variants all actually run), see "Phase 5 progress --
  Track A.3.2" above. Scaling to more threads than CPU cores was not
  separately explored beyond the existing tests' thread counts.
- ~~§6.D fault injection~~ DONE -- see "Phase 5 progress (this pass)
  -- plan_phase5.md Track A.4/A.5" above (`tests/test_kes_fault_
  injection.c`, A.4.1-A.4.3).
- ~~§6.E storage persistence/crash-consistency~~ DONE -- see "Phase 5
  progress (this pass) -- plan_phase5.md Track A.4/A.5" above
  (`tests/test_kes_crash_consistency.c`, A.5.1-A.5.3).
- ~~§6.F randomized/fuzz-adjacent testing~~ DONE -- see "Track A.6 --
  randomized/fuzz-adjacent testing (DONE)" under Resolved above
  (`tests/test_kes_fuzz.c`, 2/2).
- §6.G performance smoke tests: not done -- intentionally skipped,
  optional/non-blocking per the plan (see "Phase 5 progress -- Track
  A.7.2" above).
- ~~§6.H long-run soak test~~ DONE -- `make soak` (TSan, 10 minutes)
  actually run to completion; see "Phase 5 progress -- Track A.7.2"
  above (`tests/test_kes_soak.c`) for the real, TSan-confirmed data
  race it found and did not fix.

---

### Phase 5 progress -- Track A.7.2: `make soak` -- 10-minute run (DONE, real TSan-confirmed data race found and FLAGGED, NOT FIXED)

New `tests/test_kes_soak.c` (plan_phase5.md Track A.7.2): extends
`test_concurrent_sync_vs_get_put`'s shape (`tests/test_kes_cache.c`)
into a widened get/put/pin/unpin/mark_dirty/flush/invalidate op mix
(mirroring `test_kes_fuzz.c`'s A.6.1 switch) running for
`KES_SOAK_SECONDS` (default 2, so it stays fast and harmless inside a
normal `make test` run -- confirmed, adds 1 test, 92/92 after this
commit). Every `SOAK_CHECK_INTERVAL_MS` (500ms) the monitor thread
parks every worker/sync thread at a checkpoint boundary
(`soak_worker_checkpoint()`), then walks the cache's hash table
directly (`kes_extent_entry_t`/`kes_cache_bucket_t` are both
non-opaque) to recompute `entries_cached`/`memory_used` from scratch
and cross-check against `kes_cache_get_stats()`, plus sanity-bound
`ref_count`/`pin_count` and `entries_pinned <= entries_cached` -- the
same quiescent-checkpoint design `test_kes_fuzz.c`'s A.6.1 invariant
checks use, extended to run periodically over wall-clock time instead
of after every op. New `make soak` target (`Makefile`): clean rebuild
under TSan (chosen over ASan -- see the Makefile comment above the
target for the reasoning: this is fundamentally about counters
drifting out of sync, a data-race symptom TSan is built to catch,
where ASan's LeakSanitizer would only catch a strict subset), then
runs `test_kes_soak` with `KES_SOAK_SECONDS=600`.

**Actually run for the full 10 minutes, not just written** -- pasted
evidence: `make soak` ran to completion (1189 quiescent checkpoint
cycles, ~55M worker ops, ~700K sync ops observed by the time of the
finding below); the soak test's own invariant assertions never failed
across any checkpoint (`Tests run: 1 / Tests passed: 1 / Tests failed:
0` -- entries_cached/memory_used matched the live hash-table walk
exactly at every one of the 1189 checks, `entries_pinned` never
exceeded `entries_cached`, no `ref_count`/`pin_count` sanity-ceiling
violation, no leak-shaped drift in the observed `memory_used` range).

**However, TSan itself reported exactly 1 data race during this run
(`ThreadSanitizer: reported 1 warnings`), which is why `make soak`'s
own exit code is nonzero (`Error 66`) even though the test's own
PASS/FAIL logic reported PASS -- this is the correct, intended
outcome of a soak test actually catching something, not a test bug.**
Reading the TSan report plus `src/kes_cache.c` confirms a real,
previously only speculated data race, exactly matching what the
`soak` Makefile target's comment already predicted from earlier
manual development testing (see that comment, written before this run
produced the first automated, reproducible confirmation):

- `kes_cache_get_extent()`'s cache-miss/load path (`src/kes_cache.c`
  lines ~891-925) publishes the new entry into the hash table (state
  `KES_EXTENT_LOADING`) via `hash_find_or_insert()` *before* actually
  reading its data from disk, and deliberately performs that
  `read_extent()` call (which writes the loaded bytes into
  `entry->data`) *without holding `entry->lock`* -- by design, to
  avoid blocking other threads for the duration of the I/O.
  `entry->lock` is only acquired afterward, to publish the final
  `KES_EXTENT_CLEAN`/`KES_EXTENT_ERROR` state (line ~927).
- `kes_cache_flush_extent()` (the direct single-entry flush call,
  `src/kes_cache.c` lines 1178-1230) finds this same
  already-published, still-loading entry via `hash_find()`, takes
  `entry->lock`, and checks only `entry->state & KES_EXTENT_DIRTY`
  before calling `write_extent()` (which reads `entry->data`) --
  it does **not** check `KES_EXTENT_LOADING` the way
  `sweep_flush_and_maybe_evict()` does (`src/kes_cache.c` line 476:
  `(entry->state & KES_EXTENT_DIRTY) && !(entry->state &
  KES_EXTENT_LOADING)`, used by `kes_cache_sync()`'s sweep path and
  eviction). If another thread had already called
  `kes_cache_mark_dirty()` on this same still-loading entry (which
  only sets the `KES_EXTENT_DIRTY` bit under `entry->lock`, so `state`
  can legitimately be `LOADING | DIRTY` at once), `flush_extent()`'s
  `DIRTY` check passes and it reads `entry->data` (locked) while the
  original load's `read_extent()` call is still writing into the
  exact same buffer (unlocked) -- a genuine, TSan-confirmed data race
  on `entry->data` between an intentionally-unlocked in-flight load
  write and a lock-guarded flush read. TSan's report: `Read of size 8
  ... by thread T1 (mutexes: write M0)` at `soak_mock_write ->
  kes_cache_flush_extent (src/kes_cache.c:1202)`, racing a `Previous
  write of size 8 ... by thread T3` (no mutex held) at `soak_mock_read
  -> kes_cache_get_extent (src/kes_cache.c:916)`, both on the same
  heap block allocated by `extent_alloc_data()` inside that same
  `kes_cache_get_extent()` miss path.
- This sharpens, and is a more severe variant of, the A.4.1 finding
  above ("`kes_cache_flush_extent()` doesn't set `KES_EXTENT_ERROR` on
  a write failure the same way the sweep path does") -- both point at
  the same root cause: `kes_cache_flush_extent()` is missing the
  `KES_EXTENT_LOADING` guard `sweep_flush_and_maybe_evict()` already
  has. A.4.1 only observed a state-tracking inconsistency; this soak
  run demonstrates the same gap is a genuine, TSan-confirmed data race
  under real concurrent load, not just a bookkeeping quirk.

**Not fixed** -- per rule 0.3, this is reported, not silently patched.
The fix is almost certainly to add the same `!(entry->state &
KES_EXTENT_LOADING)` guard `sweep_flush_and_maybe_evict()` already
uses (or block/wait on the loading condvar the way `kes_cache_get_extent()`'s
own "wait for an in-flight load" branch around line 1013 already does
for *readers*) to `kes_cache_flush_extent()`, but choosing between
those two shapes is a real design decision for `src/kes_cache.c`, out
of scope for this test-only commit.

A.7.1 (perf smoke) was intentionally left undone: the plan marks it
optional/stretch, explicitly non-blocking, "do not block finishing
this plan on this item" -- skipped in favor of finishing the required
A.7.2 soak run and the plan-wide bookkeeping below.

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
