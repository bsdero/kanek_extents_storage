# Pending Items

Tracks progress against `KES_HARDENING_PLAN.md`'s phases. As of this
writing, Phases 1-4 are complete (bug fix, sanitizer tooling, the
four missing cache functions, capacity enforcement/eviction) and the
P0 concurrency bug is fixed; **Phase 5 (test expansion, `plan_phase5.md`
Track A) and Phase 6 (docs truth pass, `plan_phase5.md` Track B) are
both now complete** -- see their sections below (and `AGENTS.md`'s
Ground Truth section) for exactly what each covers and, importantly,
for the real bugs this pass found and deliberately left unfixed per
`plan_phase5.md` rule 0.3. Read `KES_HARDENING_PLAN.md` in full before
picking up any remaining item (allocation strategies beyond first-fit
is the main one) -- it is still the ground-truth work order for *how*
to implement each piece correctly, even though most of it now
describes work already done.

Verified state as of this writing: `make test` (plain, unsanitized
build) passes clean -- 92/92 tests across all 12 test binaries
(`test_kes_minimal`, `test_kes_bitmap_full`, `test_kes_storage_full`,
`test_kes_storage_edge`, `test_kes_cache`, `test_kes_cache_edge`,
`test_kes_cache_full`, `test_kes_multiprocess`, `test_kes_fault_injection`,
`test_kes_crash_consistency`, `test_kes_fuzz`, `test_kes_soak`).

**`kes_cache_flush_extent()`'s `KES_EXTENT_LOADING` race is now
FIXED and CLOSED** -- see the "`kes_cache_flush_extent()` vs.
in-flight load race (FIXED, CLOSED)" entry under Resolved below for
the fix and the evidence that `make tsan` is now clean (re-verified
this pass: `make tsan` exits 0, no data race reported anywhere near
`kes_cache_flush_extent`; previously this reproduced often enough to
fail even a 2-second `make tsan` smoke run -- see the Track A.7.2
entry, also now marked CLOSED, for the original finding).

**`block_count == 0` is now FIXED and CLOSED** -- see the "`block_count
== 0` rejected in `kes_cache_get_extent()` (FIXED, CLOSED)" entry
under Resolved below for the fix and the evidence. `make check-all`
was re-run for real after the fix (not assumed clean): **exit 0, "ALL
CHECKS PASSED"** -- `make test`/`make asan`/`make tsan`/`make
valgrind` all clean in sequence. `make check-all` is now reliably
clean end-to-end for everything this repo currently gates on.

The `kes_cache_destroy()` vs. concurrent-access use-after-free (Track
A.3.1 below) remains unfixed, but does **not** itself gate any
Makefile target: the test that demonstrates it deliberately forks a
disposable child process to contain the crash, and neither Valgrind's
`--error-exitcode` nor a sanitizer's exit status for the *parent*
process reflects what happens inside that forked child -- confirmed
again in this `make check-all` run (the only TSan/Valgrind findings
are all inside that forked child, at `kes_cache_destroy`/
`destroy_race_thread`, and the overall run still exited 0).

Do not assume any of the above stays true without rerunning it -- see
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

### `kes_cache_flush_extent()` vs. in-flight load race (FIXED, CLOSED)

**Closes the Track A.7.2 finding below and the matching `make check-all`
bookkeeping-run bullet.** Fixed in commit `97ab19a` ("Bugs fixed.").

- Was: `kes_cache_flush_extent()` checked only `entry->state &
  KES_EXTENT_DIRTY` before reading `entry->data` under `entry->lock`,
  unlike `sweep_flush_and_maybe_evict()`, which also requires
  `!(entry->state & KES_EXTENT_LOADING)`. Since
  `kes_cache_get_extent()`'s cache-miss path publishes the entry
  (state `KES_EXTENT_LOADING`) and then calls `read_extent()`
  (writing `entry->data`) *without* holding `entry->lock` by design,
  and `kes_cache_mark_dirty()` is legal on a still-loading entry
  (state can be `LOADING | DIRTY`), a concurrent
  `kes_cache_flush_extent()` call could read `entry->data` while the
  original load's unlocked write was still in flight -- a genuine,
  TSan-confirmed data race, first caught by the Track A.7.2 soak run
  and reproducible even in a 2-second `make tsan` smoke run.
- Fix: `kes_cache_flush_extent()` (`src/kes_cache.c`, immediately
  after acquiring `entry->lock`) now waits out an in-flight load the
  same way `kes_cache_get_extent()`'s own "attach to an in-flight
  load" branch does -- incrementing `entry->cond_waiters` and looping
  on `pthread_cond_wait( &entry->cond, &entry->lock)` while
  `entry->state & KES_EXTENT_LOADING`, so `try_evict_entry_locked()`
  won't free the entry out from under the wait -- before the existing
  `KES_EXTENT_DIRTY` check runs.
- Verified (this pass, re-run to confirm rather than trusting the
  commit message): `make tsan` -- clean, exit 0. The only
  `ThreadSanitizer: data race` reports in the full log are all at
  `kes_cache_destroy` (`src/kes_cache.c:776`/`779`/`787`), which was
  at the time the separate, still-open, deliberately-unfixed Track
  A.3.1 race contained inside a forked child process (did not gate
  the `tsan` target) -- that race is now also fixed and closed, see
  the "Track A.3.1" entry under `## Resolved` -- no report anywhere
  at `kes_cache_flush_extent`/line ~1202 this run, where the fixed
  race used to reproduce reliably.
  `make test`: all binaries still pass (92/92 baseline unaffected --
  this is a pure concurrency fix, no new test cases added in this
  commit).
- **Not closed by this fix**: the related-but-distinct A.4.1 finding
  that `kes_cache_flush_extent()` doesn't set `KES_EXTENT_ERROR` on a
  *write* failure the way `sweep_flush_and_maybe_evict()` does (it
  only returns `KES_ERROR_IO`) is a separate behavioral inconsistency,
  confirmed still present by reading the current
  `kes_cache_flush_extent()` body (`src/kes_cache.c` around line
  1247-1250) -- still open, see A.4.1 below.

### KES-6 -- bitmap checksum: silently bit-flipped bitmap block caused real double-allocation (FIXED, CLOSED)

**Closes the `KES-6` entry in `PENDING_FIXES_SEP2026.md`. Full
implementation plan: `kes_6_plan.md`.**

- Was: `kes_bitmap_load()`/`kes_bitmap_save()` had no integrity check
  on the on-disk bitmap region. A single silently bit-flipped byte
  went completely undetected at `kes_storage_open()` time, causing a
  real double-allocation -- the allocator could hand out a block that
  was still live and holding another extent's data, silently
  overwriting it. `tests/test_kes_crash_consistency.c`'s
  `test_bitflipped_bitmap_block` demonstrated this concretely.
- Fix (breaking on-disk format change, no migration path -- confirmed
  with the project owner): added a `uint32_t bitmap_checksum` field to
  `kes_storage_descriptor_t` (`include/kes/kes_types.h`), shrinking
  `reserved` from 32 to 28 bytes to keep the struct size unchanged,
  and bumped `KES_VERSION_MAJOR` to 2. It holds the CRC-32C of the
  bitmap's on-disk bytes (`bitmap->data`, `bitmap->total_bytes`), via
  a new `kes_bitmap_checksum()` (`include/kes/kes_bitmap.h`,
  `src/kes_bitmap.c`) built on `kfl_crc32c()` from the sibling
  `kanek_foundations` (KFL) repo -- this is the first code in `src/`
  that actually links `libkfl.a`, not just includes its headers.
  `src/kes_storage.c`: `load_storage_descriptor()` now rejects any
  `version_major != KES_VERSION_MAJOR` with `KES_ERROR_CORRUPT` and a
  `TRACE_ERR` naming the mismatch (a stale pre-KES-6 file's `reserved`
  bytes would otherwise read as zero, not a real checksum, and
  spuriously fail the checksum check with a confusing error instead of
  this precise one); the checksum itself is set on
  `kes_storage_create()`, verified on `kes_storage_open()`, and
  refreshed on every `kes_storage_sync()`/`kes_storage_close()`.
  `Makefile`: `$(FOUNDATIONS_LIB)` is now a real file target (not just
  a phony convenience alias) linked into `$(SHARED_LIB)` and every
  test/example binary; `README.md` documents both the breaking format
  change and the new link-time dependency.
- Two additional build-system bugs found and fixed while verifying
  this plan's own checklist, neither described in `kes_6_plan.md`
  itself (that plan only threaded `FOUNDATIONS_CFLAGS` through
  `asan`/`tsan`/`soak`, not `FOUNDATIONS_LDFLAGS`, and didn't
  anticipate either failure mode below):
  - GNU Make auto-propagates command-line variable overrides (e.g.
    the `LDFLAGS=...` the `asan`/`tsan`/`soak` targets set on their
    own `$(MAKE)` invocation) down through nested `$(MAKE)` calls via
    `MAKEFLAGS`. Since wiring `$(FOUNDATIONS_LIB)` into `all`/`tests`
    was the first time those targets ever triggered a recursive build
    of `kanek_foundations`, this silently clobbered that repo's own
    `LDFLAGS = -L. -lkfl -rdynamic -lpthread` default and broke its
    own test binaries (e.g. `testrand` failed to link, "undefined
    reference to `krand64`"). Fixed by adding an explicit
    `FOUNDATIONS_LDFLAGS` variable (mirroring `FOUNDATIONS_CFLAGS`)
    that is always passed explicitly on the recursive `$(MAKE) -C
    $(FOUNDATIONS_SRC)` call, and threaded through the `asan`/`tsan`/
    `soak` targets the same way `FOUNDATIONS_CFLAGS` already was.
  - `libkfl.a`'s objects (specifically `crc32c.o`) were never compiled
    with `-fPIC` in `kanek_foundations`'s own Makefile default, which
    is fine for a static-only consumer but breaks linking that archive
    into `libkes.so` (a `-shared` object) outright -- `ld: ... can not
    be used when making a shared object; recompile with -fPIC`. Not
    ASan-specific: reproduced on a plain, unsanitized `make all` too.
    Fixed by adding `-fPIC` to `FOUNDATIONS_CFLAGS`'s base default.
- Verified (checklist from `kes_6_plan.md`, re-run after both fixes
  above): `make clean && make test` -- 93/93 passing, including
  `test_bitflipped_bitmap_block`'s rewritten assertions (now expects
  `KES_ERROR_CORRUPT`/`st == NULL` on reopen, proving the corruption
  IS detected, rather than the old "undetected" demonstration).
  `make asan` -- clean, 12/12 binaries, no ASan/UBSan findings.
  `make tsan` -- clean, 12/12 binaries, no data races. `make valgrind`
  -- clean, "ERROR SUMMARY: 0 errors" across all 15 runs. `make
  check-all` -- **exit 0, "ALL CHECKS PASSED"**. Additionally, a
  standalone throwaway program confirmed the breaking-change behavior
  concretely: a storage file created by a build with
  `KES_VERSION_MAJOR` manually reverted to 1 was then opened by the
  real (v2) library, producing `kes_storage_open() == KES_ERROR_CORRUPT`
  (`-7`) with `*storage` left `NULL`, and the log line
  `incompatible on-disk format version 1.0 (library is 2.0) --
  storage files created before the KES-6 bitmap-checksum change must
  be recreated`.
- Out of scope (per `kes_6_plan.md`, unchanged): `KES-5`
  (unsynchronized cross-process access, a separate plan that itself
  depends on this one's `kes_bitmap_checksum()`), a migration/repair
  tool for pre-KES-6 files, and checksumming anything beyond the
  bitmap region.

### KES-5 -- unsynchronized cross-process/cross-handle storage access corrupted state (FIXED, CLOSED)

**Closes the `KES-5` entry in `PENDING_FIXES_SEP2026.md`. Full
implementation plan: `kes_5_plan.md`. Depended on KES-6 (bitmap
checksum) above, which landed first.**

- Was: `kes_storage_t`'s `pthread_mutex_t` only coordinates threads
  within one process. It could not coordinate two independent
  processes -- or even two independent `kes_storage_open()` handles in
  the same process -- against the same backing file.
  `tests/test_kes_multiprocess.c`'s `test_cross_process_racing_io`
  demonstrated real, concrete corruption: two processes racing
  `kes_extent_allocate()`/`kes_extent_write()` against the same file
  with zero coordination, and the real on-disk `used_blocks` afterward
  was *less* than the combined allocations both sides believed
  succeeded -- one side's `kes_storage_sync()` silently clobbered the
  other's. Simply wrapping the existing code in `flock()` would not
  have fixed this: the actual defect was a lost-update problem (each
  handle allocating against a private, increasingly-stale in-memory
  bitmap copy loaded once at `open()` time), not two writes literally
  overlapping in time.
- Fix (OS-level serialization, not a single-writer lock -- confirmed
  with the project owner; a second concurrent `kes_storage_open()`
  remains a supported pattern, not rejected): `src/kes_storage.c`'s
  `kes_extent_allocate()`, `kes_extent_free()`, `kes_storage_sync()`,
  and the bitmap-save block inside `kes_storage_close()` each now
  acquire an exclusive `flock(storage->fd, LOCK_EX)` (not `fcntl()`
  record locks -- those are process+inode scoped and would incorrectly
  merge/release across independent same-process handles on a `close()`
  of any one of them; `flock()` is scoped to the open file description
  and correctly covers both the cross-process and same-process/
  independent-handle cases), then perform a full **read-reload-mutate-
  write** cycle against the file -- reloading the descriptor/bitmap
  fresh from disk (discarding any stale in-memory copy), applying the
  mutation, and persisting the result (bitmap, then
  checksum-refreshed descriptor, then `fsync()`) before releasing the
  lock. `kes_storage_create()` now also writes the freshly-created
  bitmap to disk immediately (previously only the descriptor block was
  written at create time), which both the new mandatory reload step
  and a separate, previously-documented "storage file unopenable after
  a crash before any sync" gap (`PENDING_ITEMS.md` A.5.1 Case 1)
  needed. A positive side effect of the correct fix, not a separate
  feature: every `kes_extent_allocate()`/`kes_extent_free()` call is
  now durable on its own the instant it returns, so a crash between an
  allocation and an explicit `kes_storage_sync()` no longer loses
  bookkeeping either.
- A real, previously-unknown race found and fixed while verifying this
  plan's own checklist, not described in `kes_5_plan.md` itself: the
  plan never added any locking to `kes_storage_open()`, reasoning only
  that it "does not reject a second concurrent open." But
  `kes_storage_open()` reads the descriptor and then, separately, the
  bitmap with no lock at all -- and once `kes_extent_allocate()`/
  `kes_extent_free()` started writing the bitmap region and the
  descriptor block as two *separate* `write()` calls while holding
  `LOCK_EX` (every single call, not just at `sync()`/`close()` time as
  before), an unlocked concurrent `kes_storage_open()` could land
  between those two writes and read a torn combination: the new
  bitmap bytes paired with the not-yet-updated (old-checksum)
  descriptor -- tripping KES-6's checksum check and failing with
  `KES_ERROR_CORRUPT` on a perfectly healthy file. First observed as a
  real, reproducible `test_cross_process_racing_io` failure under
  `make asan` (slower execution widened the window), not as a
  theoretical concern. Fixed by having `kes_storage_open()` take a
  **shared** `flock(LOCK_SH)` around its whole read sequence
  (descriptor load, bitmap create/load, checksum verification) --
  compatible with other concurrent readers, but blocks until any
  in-progress exclusive writer's critical section finishes, so it can
  never observe a torn intermediate state.
- Test changes: `tests/test_kes_crash_consistency.c`'s
  `test_no_sync_reopen_durability` rewritten -- Case 1 (crash before
  any sync) now expects `kes_storage_open() == KES_SUCCESS` instead of
  `KES_ERROR_IO` (the file is always fully laid out after `create()`
  now); Case 2 (crash after an unsynced allocate+write) now asserts
  the allocation survives the crash and a fresh allocation hinted at
  the same block is correctly refused the still-live block, instead of
  asserting the old "forgotten allocation -> double-allocation"
  hazard. `tests/test_kes_multiprocess.c`'s
  `test_cross_process_racing_io` upgraded from printing "known
  limitation" observations to real `TEST_ASSERT`s: the real on-disk
  `used_blocks` after both processes exit now must exactly equal the
  sum of both sides' successful allocations (each racing allocation
  requests `block_count == 1`), and the descriptor magic number must
  stay intact.
- A second build-system bug, same class as the two found during KES-6,
  fixed alongside the `kes_storage_open()` race above:
  `Makefile`'s pre-existing `stress` target (`make stress
  SANITIZER=asan|tsan`) had the identical `FOUNDATIONS_CFLAGS`/
  `FOUNDATIONS_LDFLAGS`-propagation gap the `asan`/`tsan`/`soak`
  targets had before KES-6 fixed it for them -- never triggered before
  because `stress` predates `$(FOUNDATIONS_LIB)` being a real
  prerequisite of `all`/`tests`. Fixed the same way: both flags now
  threaded through its `asan`/`tsan` rebuild branches.
- Verified (checklist from `kes_5_plan.md`, re-run after the
  `kes_storage_open()` fix above): `make clean && make test` -- 93/93
  passing, including both rewritten tests' new assertions. `make asan`
  -- clean across three separate runs (12/12 binaries each; the first
  run before the `kes_storage_open()` fix reproduced the
  `KES_ERROR_CORRUPT` race exactly once, confirming it was real, not
  flaky test logic). `make tsan` -- clean, 12/12, no data races and no
  lock-related hangs (the most relevant sanitizer for this change's
  new `flock()`/`pthread_mutex_t` paths). `make valgrind` -- clean,
  "ERROR SUMMARY: 0 errors" across all 15 runs. `make check-all` --
  **exit 0, "ALL CHECKS PASSED."** `make stress SANITIZER=tsan
  STRESS_RUNS=30` -- **"ALL RUNS PASSED"**, 30/30 for both
  `test_kes_cache` and `test_kes_multiprocess`. Additionally, the
  `test_kes_multiprocess` binary was run standalone 40 times in a row
  (plain build) and 25 more times with a 15s timeout per run (to
  specifically watch for a `flock()` deadlock/hang from a missing
  unlock on some path) -- 0 failures, 0 hangs, total wall time ~4s for
  the 25-run batch.
- Out of scope (per `kes_5_plan.md`, unchanged): `kes_extent_read()`/
  `kes_extent_write()` (unchanged -- they operate on raw block data at
  a fixed offset and never touch the shared bitmap/descriptor state
  this plan protects; two callers racing a write to the *same* extent
  is a separate, application-level concern), `kes_storage_get_stats()`/
  `kes_storage_get_descriptor()` still returning a possibly-stale
  snapshot from this handle's last reload (call `kes_storage_sync()`
  first for a guaranteed-fresh cross-process view), concurrent
  `kes_storage_create()` calls racing to create the same not-yet-
  existing file, any change to `kes_cache.c`/`kes_cache.h` (unaffected
  -- the cache layer does not depend on `kes_storage.c`), and a
  non-blocking/timeout variant of the `flock()` acquisition.

### `block_count == 0` rejected in `kes_cache_get_extent()` (FIXED, CLOSED)

**Closes Track A.1.3 below and the matching `make check-all`
bookkeeping-run bullet.**

- Was: `kes_cache_get_extent()` validated `id->block_size` (A.1.2) but
  never `id->block_count`. A request with `block_count == 0` fell
  through to `extent_data_size()` computing `0 * block_size == 0` and
  `aligned_alloc(64, 0)`, which glibc returns non-NULL for on this
  platform -- producing a "successful" but nonsensical 0-byte cached
  entry instead of an error. No crash, no oversized-allocation risk,
  but Valgrind's Memcheck flags the zero-size `aligned_alloc()` call
  itself, failing `make valgrind`/`make check-all` via their
  `--error-exitcode=1`.
- Fix: `kes_cache_get_extent()` (`src/kes_cache.c`) now rejects
  `id->block_count == 0` with `KES_ERROR_INVALID` before any
  allocation is attempted, immediately after the existing
  `id->block_size` guard (A.1.2) and before `*buffer` is touched --
  same shape, same place in the function, same error code.
  `tests/test_kes_cache_edge.c`'s `test_block_count_zero_and_max`
  updated to assert `KES_ERROR_INVALID`/`buf == NULL` for the
  `block_count == 0` case instead of the old "succeeds with a 0-byte
  buffer" assertion; the `block_count == UINT32_MAX` half of that test
  (expects `KES_ERROR_BUSY`) is untouched.
- Verified: `make test` (92/92, no regressions), `make asan` (clean),
  code review pass (no defects). Then `make check-all` re-run in full
  from a clean tree: **exit 0, "ALL CHECKS PASSED"** --
  test/asan/tsan/valgrind all clean in sequence, including Valgrind
  (previously the one target this gap failed). The only TSan/Valgrind
  findings anywhere in that run were the already-known, non-gating
  `kes_cache_destroy()` race (Track A.3.1) inside its disposable
  forked child -- since fixed and closed, see the "Track A.3.1" entry
  under `## Resolved`.
- **Not closed by this fix**: at the time, the `kes_cache_destroy()`
  vs. concurrent-access use-after-free (Track A.3.1) was unrelated,
  still open, still deliberately unfixed pending an API-contract
  decision -- that decision was made and implemented separately, see
  the "Track A.3.1" entry under `## Resolved`.

### Track A.3.1 -- `kes_cache_destroy()` use-after-free under concurrent access (FIXED, CLOSED)

**Closes the "Phase 5 progress -- Track A.3.1" finding and the "Track
A.3.1 fix plan" design section, both formerly below this point (see
git history for their original text) -- this is the fix that plan
described, implemented as specified. Fixed in commit `8d40167` ("fix:
kes_cache_destroy() use-after-free under concurrent access
(KES-1)").**

- Was: `kes_cache_destroy()` walked `cache->mru_head` unconditionally,
  freeing every entry's data buffer, destroying its `lock`/`cond`, and
  `free()`-ing the struct -- without ever checking `ref_count`/
  `pin_count`, and without acquiring `entry->lock` while doing so. A
  standalone repro (one thread in a tight `get_extent()`/
  `put_extent()` loop, `kes_cache_destroy()` called from another
  thread ~2ms later) reproduced a heap-use-after-free under ASan on
  every run, and multiple data races plus "use of an invalid mutex"
  reports under TSan. The automated regression
  (`test_cache_destroy_races_concurrent_access`,
  `tests/test_kes_cache.c`) ran the race inside a forked child process
  specifically so it couldn't take down the whole test binary, and did
  not previously assert on the outcome.
- Fix, in `src/kes_cache.c`/`include/kes/kes_cache.h`:
  - `kes_cache_destroy()` now walks the LRU list the same
    lookup-pin-protected way `cache_sweep()` already does (pin the
    next node via `lookup_pins` under `cache_lock` before releasing it
    and processing the current node), then calls
    `try_evict_entry_locked( cache, entry, true /* discard_dirty */)`
    per node -- the same `ref_count == 0 && pin_count == 0 &&
    cond_waiters == 0 && lookup_pins == 0` eligibility check every
    other evictor in this file already uses -- instead of freeing
    directly. Deliberately does NOT pin traversal nodes via
    `entry->ref_count` itself; that specific approach is the one the
    Phase 3 entry above already documents as tried and
    ASan-confirmed broken.
  - A new `uint32_t inflight_lookups` field on `kes_cache_t`, protected
    by `cache_lock` only, closes a residual gap `ref_count` alone
    can't cover: a brand-new `kes_cache_get_extent()` miss publishes
    its candidate into the hash table (`hash_find_or_insert()`)
    *before* it takes `cache_lock` to add it to the LRU list -- a
    `destroy()` call observing an empty LRU list at exactly that
    instant could otherwise free `cache->buckets`/`cache->cache_lock`/
    `cache` itself while that `get_extent()` call is still about to
    touch them. `kes_cache_get_extent()` increments this counter once,
    right after its existing `block_size`/`block_count` validation,
    and decrements it (also under `cache_lock`) on every subsequent
    return path -- six call sites total: the cache-full `BUSY` path,
    both candidate-allocation `NOMEM` paths, the win-the-race insert
    bookkeeping block (before the actual `read_extent()` I/O, per this
    project's "never hold cache-wide locks across I/O" discipline),
    the hit-path `KES_EXTENT_ERROR` early return, and the hit-path
    success block (which also covers the lose-the-race fallthrough,
    since both routes converge on that one block). Never held across
    the `read_extent()` call itself.
  - A new `bool destroying` field, also `cache_lock`-protected and
    distinct from the existing `shutdown` bool (`kes_cache_start()`
    documents `shutdown` as clearable via a `stop()`/`start()` restart
    cycle -- reusing it here would silently break that), is set by
    `kes_cache_destroy()` once it commits to tearing the cache down.
    While set, `kes_cache_get_extent()` rejects new lookups with
    `KES_ERROR_INVALID`.
  - `kes_cache_destroy()` is fail-fast, not blocking: if
    `inflight_lookups != 0` up front, or the LRU list is non-empty
    after one eviction pass, it leaves the cache object itself fully
    intact and usable (resetting `destroying` back to `false` in the
    latter case) and returns `KES_ERROR_BUSY` -- the same contract
    `kes_cache_invalidate()` already uses for referenced/pinned
    entries. New callers are NOT permanently locked out by a
    `BUSY`-returning `destroy()` attempt; only a call that returns
    `KES_SUCCESS` makes that rejection permanent.
  - A real bug was caught by this change's own `make tsan` run before
    landing: the `BUSY`-path `TRACE_ERR()` call originally read
    `cache->inflight_lookups` for its log message *after* `cache_lock`
    had already been unlocked -- an unlocked read racing
    `kes_cache_get_extent()`'s locked increment/decrement of the same
    field, caught as a TSan data race at `kes_cache_destroy()`'s
    `TRACE_ERR` line. Fixed by capturing the value into a local
    variable before unlocking.
- Fallout from the corrected contract, fixed in the same change: three
  pre-existing tests (`test_kes_cache_full.c`'s
  `test_kes_cache_pin_unpin`/`test_kes_cache_mark_dirty`/
  `test_kes_cache_flush_extent`/`test_kes_cache_get_stats`, and
  `test_kes_cache_edge.c`'s `test_destroy_with_outstanding_reference`)
  called `kes_cache_get_extent()` and never released the reference
  before `destroy()` -- harmless under the old unconditional-free
  behavior, but a real ASan-detected leak now that `destroy()`
  correctly refuses. Fixed by adding the missing `put_extent()` calls
  (and, for the A.1.6 edge test, rewriting it to assert the new
  `KES_ERROR_BUSY`/intact-cache/succeeds-once-released contract
  instead of the old "frees unconditionally" one). `test_kes_fuzz.c`'s
  `test_cache_invariant_fuzz()` needed a different fix: its random
  op sequence has no guarantee of ending with every reference
  released, so it now tracks each pool id's outstanding
  `ref_count`/`pin_count` locally (bumped only on an operation's own
  `KES_SUCCESS`) and drains them precisely after the loop, before
  asserting `destroy()` now succeeds.
- Test changes: `test_cache_destroy_races_concurrent_access()`
  (`tests/test_kes_cache.c`) now asserts a clean child exit
  (`WIFEXITED && WEXITSTATUS == 0`) under both plain and sanitized
  builds, instead of accepting a crash/sanitizer-kill as documented,
  expected behavior -- it now proves the fix rather than the bug. A
  new non-concurrent test, `test_destroy_busy_when_referenced()`,
  proves the `KES_ERROR_BUSY` contract on a single thread: get an
  extent, don't put it, `destroy()` returns `BUSY`, `put_extent()`,
  `destroy()` then returns `SUCCESS`.
- Verified, actually run (not assumed): `make test` -- **93/93**
  across all 12 binaries (was 92/92; +1 for the new BUSY test).
  `make asan` -- clean, exit 0, 0 leak/error findings (confirmed by
  grepping the full log for `leak`/`AddressSanitizer`/
  `LeakSanitizer` after the test-hygiene fixes above; before those
  fixes, this run genuinely caught 280KB+ across two binaries, which
  is what surfaced the need for them). `make tsan` -- clean, exit 0,
  no `ThreadSanitizer` warnings anywhere in the log, including inside
  the destroy-race child that used to reliably trip it (this is also
  what caught the unlocked-`TRACE_ERR`-read bug above, before this
  entry was written).
- `make check-all`'s Valgrind phase caught one more thing on the
  first real run, unrelated to `kes_cache_destroy()` itself: the
  destroy-race child's forked process (Valgrind auto-follows `fork()`
  in this setup, giving each forked child its own independent
  memcheck instance/report) reported "3 errors from 3 contexts,
  possibly lost: 1,088 bytes in 4 blocks" -- all `calloc` via glibc's
  `allocate_dtv`/`_dl_allocate_tls`/`allocate_stack`/`pthread_create`,
  i.e. cached-but-unreferenced thread-stack TLS blocks from two
  *earlier*, unrelated tests in the same binary
  (`test_concurrent_sync_vs_get_put`/
  `test_concurrent_miss_no_duplicate_entry`) that the child inherited
  at `fork()` time. `--error-exitcode=1` makes Valgrind override a
  traced process's own exit status with 1 whenever it finds any
  memcheck error in that process -- so the child's real exit status
  (0, or whatever `kes_cache_destroy()` actually returned) was being
  silently replaced with 1 by Valgrind itself, which is exactly what
  `test_cache_destroy_races_concurrent_access()`'s new clean-exit
  assertion (this fix, see above) correctly flagged as a failure.
  Confirmed via a side-by-side `make valgrind` run against the
  pre-fix tree (`git worktree add ... ab1e419`) that this exact
  "possibly lost: 1,088 bytes in 4 blocks" finding, in the same
  forked child, already existed before this fix too -- it was simply
  never asserted on previously, since the old test's assertion always
  passed regardless of the child's outcome. This is a well-known,
  benign Valgrind/glibc interaction (glibc caches a joined thread's
  stack for reuse rather than unmapping it immediately, which
  Memcheck's conservative reachability scanner can't always prove
  reachable from a live pointer) -- not a real leak, and not
  introduced by this fix. Added `valgrind.supp` (repo root) with a
  suppression matching that specific `pthread_create` stack-allocation
  pattern, and wired it into the `valgrind` Makefile target via
  `--suppressions=valgrind.supp`. Re-verified after adding it: the
  same child now reports "possibly lost: 0 bytes in 0 blocks...
  suppressed: 3 from 3", exits 0, and the destroy-race test passes
  under `make valgrind` alone.
- `make check-all` -- re-run in full from a clean tree with the
  suppression file in place: exit 0, "ALL CHECKS PASSED".
- **Not closed by this fix**: allocation strategies beyond first-fit
  (KES-2), the missing bitmap checksum (KES-6), and unsynchronized
  cross-process storage access (KES-5) are all separate, unrelated
  gaps -- see `PENDING_FIXES_SEP2026.md`.

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

### Phase 5 progress -- Track A.3.1: `kes_cache_destroy()` vs concurrent access (DONE -- FIXED, CLOSED)

`Cache Destroy Races Concurrent Access` (`tests/test_kes_cache.c`,
plan_phase5.md Track A.3.1) found a real, confirmed
`kes_cache_destroy()` use-after-free under concurrent access, and a
design session on 2026-09-02 produced an agreed fix plan for it. Both
the original finding and that fix plan's full detail have been moved
into the "Track A.3.1" entry under `## Resolved` above, now that the
fix itself is implemented, tested, and closed -- see that entry (and
its git history, for the finding/plan's original text) rather than
this pointer.

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
  originally produced a valid 0-byte cached entry (`aligned_alloc(64,
  0)` returns non-NULL on this platform) -- asserted at the time as
  observed behavior, flagged as a real but minor gap. **Since FIXED
  and CLOSED** -- see the "`block_count == 0` rejected in
  `kes_cache_get_extent()` (FIXED, CLOSED)" entry under Resolved
  above; the test now asserts `KES_ERROR_INVALID`/`buf == NULL`
  instead. `block_count == UINT32_MAX` is cleanly rejected as
  `KES_ERROR_BUSY` (the `max_memory` capacity check in
  `make_room_for_new_entry()` fails before any allocation is
  attempted for the resulting ~16TiB request) -- not the `NOMEM` the
  original plan speculated. This half was never broken and is
  unchanged by the fix.
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

Real gap found in this pass, initially left deliberately NOT fixed per
rule 0.3 (no drive-by fixes -- reported instead): `block_count == 0`
was not validated anywhere in `kes_cache_get_extent()` and produced a
"successful" but nonsensical 0-byte cached entry rather than being
rejected with `KES_ERROR_INVALID`. **Since fixed as its own follow-up
piece of work** -- see the "`block_count == 0` rejected in
`kes_cache_get_extent()` (FIXED, CLOSED)" entry under Resolved above.

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

### Phase 5 progress -- Track A.7.2: `make soak` -- 10-minute run (DONE, real TSan-confirmed data race found and FLAGGED; race since FIXED -- see "Resolved" above -- CLOSED)

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

**Not fixed at the time this section was written** -- per rule 0.3,
this was reported, not silently patched, in the commit that added
this test. **Since fixed in commit `97ab19a`** by adding exactly the
condvar-wait shape speculated below (waiting out
`KES_EXTENT_LOADING` on `entry->cond`/`cond_waiters`, mirroring
`kes_cache_get_extent()`'s own reader-side wait) -- see the
"`kes_cache_flush_extent()` vs. in-flight load race (FIXED, CLOSED)"
entry under `## Resolved` above for the fix and re-verification
evidence (`make tsan` clean, no report at `kes_cache_flush_extent`).
This paragraph is left in place as the historical record of the
original finding:

The fix is almost certainly to add the same `!(entry->state &
KES_EXTENT_LOADING)` guard `sweep_flush_and_maybe_evict()` already
uses (or block/wait on the loading condvar the way `kes_cache_get_extent()`'s
own "wait for an in-flight load" branch around line 1013 already does
for *readers*) to `kes_cache_flush_extent()`, but choosing between
those two shapes is a real design decision for `src/kes_cache.c`, out
of scope for this test-only commit.

**Update, discovered while completing the plan-wide `make check-all`
bookkeeping gate below**: this is not a rare, only-under-10-minutes
race. A plain `make tsan` re-run (which now includes
`tests/test_kes_soak.c` via the `TEST_SOURCES` wildcard, at its
default `KES_SOAK_SECONDS=2`) reproduced the identical TSan report
inside a 2-second run: `ThreadSanitizer: reported 1 warnings`,
`test_kes_soak` itself still `PASS` (its own invariants never caught
anything), but the nonzero TSan exit status fails the `tsan` target
(`make: *** [Makefile:208: tsan] Error 1`). **Practical consequence:
`make tsan`/`make check-all` are not currently reliably clean --
they can intermittently fail specifically because
`tests/test_kes_soak.c` is exactly the kind of sustained
mark_dirty+concurrent-load workload that triggers the real,
already-documented `kes_cache_flush_extent()` race above, and does so
often enough to show up even in a short smoke run, not just a full
10-minute soak.** This is being reported as-is, per rule 0.3, rather
than silently adding a workaround (e.g. excluding `test_kes_soak`'s
short default form from `make tsan`) that would hide a real,
reproducible bug behind a green build.

**CLOSED as of commit `97ab19a`** -- see the "Resolved" entry above;
`make tsan` re-run clean after the fix, no further action needed
here.

A.7.1 (perf smoke) was intentionally left undone: the plan marks it
optional/stretch, explicitly non-blocking, "do not block finishing
this plan on this item" -- skipped in favor of finishing the required
A.7.2 soak run and the plan-wide bookkeeping below.

### `make check-all` bookkeeping run -- one real Makefile gap found and FIXED, one real code bug found and left NOT FIXED at the time (since FIXED -- CLOSED)

Running `make check-all` for real (the plan_phase5.md S6 gate before
updating `AGENTS.md`'s Ground Truth section) surfaced two distinct
issues, one fixed here and one deliberately not:

- **Fixed**: the `tsan` target had no equivalent of the `asan`
  target's scoped `ASAN_OPTIONS=allocator_may_return_null=1`
  workaround for `test_kes_fault_injection`'s deliberate 200GiB
  allocator-OOM case (A.4.2) -- TSan shares the same sanitizer-common
  allocator and abort-on-OOM default (confirmed by TSan's own hint
  text), so it aborted the same way ASan originally did before A.4.2
  added that workaround. Fixed by mirroring the `asan` target's
  pattern exactly, scoped to that one binary via `TSAN_OPTIONS`. This
  is a test-harness fix (making the sanitizer behave as intended for
  a test that deliberately induces OOM), not a production-code change,
  so it is not a rule-0.3 "drive-by fix."
- **Was not fixed at the time this section was written**: with that
  gap closed, `make tsan` still failed intermittently -- see the
  "Update, discovered while completing the plan-wide `make check-all`
  bookkeeping gate" paragraph above. This was the real,
  already-documented `kes_cache_flush_extent()` vs.
  `KES_EXTENT_LOADING` race, not a new bug. **CLOSED as of commit
  `97ab19a`** -- see the "`kes_cache_flush_extent()` vs. in-flight
  load race (FIXED, CLOSED)" entry under `## Resolved` above;
  `make tsan` now re-runs clean. The remaining `block_count == 0`
  Memcheck-gating gap (A.1.3) is **also now fixed and closed** -- see
  the "`block_count == 0` rejected in `kes_cache_get_extent()` (FIXED,
  CLOSED)" entry under `## Resolved` above. With both closed,
  `make check-all` was re-run for real (not assumed): **exit 0, "ALL
  CHECKS PASSED."** The `kes_cache_destroy()` vs. concurrent-access
  use-after-free (Track A.3.1), the one item still open at the time
  this paragraph was written, is now **also fixed and closed** -- see
  the "Track A.3.1" entry under `## Resolved` above.

## Open items from ad-hoc review (2026-09-01, not yet in a plan track)

Found while reviewing current repo state against `AGENTS.md`/this
file; none of these were previously called out as their own item, so
recorded here rather than assumed covered by the "allocation
strategies beyond first-fit" one-liner in the summary above.

### `kes_extent_allocate()` silently substitutes first-fit for unimplemented strategies (OPEN, not fixed)

`kes_allocation_strategy_t` (`include/kes/kes_types.h:58-64`) declares
`KES_ALLOC_FIRST_FIT`/`BEST_FIT`/`WORST_FIT`/`NEXT_FIT` as public,
settable enum values (`kes_storage_config_t.strategy`,
`include/kes/kes_types.h:103`) -- a caller can set
`KES_ALLOC_BEST_FIT` today with no compile or runtime error. But
`kes_extent_allocate()`'s strategy dispatch (`src/kes_storage.c:269-274`)
is:

```c
switch ( storage->strategy) {
    case KES_ALLOC_FIRST_FIT:
    default:
        result = allocate_extent_first_fit( storage, request, extent);
        break;
}
```

Only `KES_ALLOC_FIRST_FIT` has an explicit case; every other value
(including the three declared-but-unimplemented ones) falls through
`default:` to `allocate_extent_first_fit()` with no error and no log
line -- a caller who explicitly asks for `BEST_FIT` silently gets
`FIRST_FIT` placement instead, with no signal anything different
happened. This is the same category of gap the cache layer already
guards against deliberately: `kes_cache_create()` (`src/kes_cache.c`)
returns `NULL` for `KES_CACHE_LFU`/`KES_CACHE_CUSTOM` rather than
silently falling back to LRU (see "Phase 3" under Resolved above).
`kes_extent_allocate()`/`kes_storage_open()` have no equivalent guard
for the allocation-strategy enum. Not covered by any existing test
(`test_kes_storage_full.c`/`test_kes_storage_edge.c` never set
`strategy` to anything but the default/`FIRST_FIT`).

**Not fixed here** -- per rule 0.3, reported rather than patched
inline. Two independent decisions for whoever picks this up: (1)
should the short-term fix be rejecting `BEST_FIT`/`WORST_FIT`/`NEXT_FIT`
at `kes_storage_open()` or `kes_extent_allocate()` time (mirroring the
cache layer's pattern) until they're implemented for real, and (2) is
implementing them the actual goal (`KES_HARDENING_PLAN.md` still
describes `allocate_extent_first_fit()`'s sibling functions as the
main remaining piece of work, but has no section written for *how* to
implement best/worst/next-fit the way it does for the cache-layer
work above).

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
