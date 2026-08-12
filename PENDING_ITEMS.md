# Pending Items

Work items identified but deliberately **not** implemented in the
session that fixed `KES_HARDENING_PLAN.md` Phase 1 (bug fix) and
Phase 2 (sanitizer tooling). Each item below was flagged rather than
fixed, per that plan's "no drive-by rewrites" / "flag deviations
explicitly" rules, because it exceeded the agreed Phase 1+2 scope.
Read `KES_HARDENING_PLAN.md` in full before starting any of these --
it is still the ground-truth work order; this file only tracks what's
left against it.

Verified state as of this writing: `make check-all` passes clean
(normal build + ASan+UBSan + TSan + Valgrind, 15/15 tests, 0 leaks,
0 races). Do not assume that stays true without rerunning it -- see
`KES_HARDENING_PLAN.md` §0's standing rule about pasted evidence.

---

## P0 -- known correctness bug, found but not fixed

**Concurrent cache-miss on the same extent ID creates duplicate
hash-table entries.**

- Where: `kes_cache_get_extent()` in `src/kes_cache.c`, the miss path
  starting around line 410 (`hash_find` returns NULL) through the
  `hash_insert( cache, entry)` call around line 470.
- What's wrong: `hash_find()` (src/kes_cache.c:157) and `hash_insert()`
  (src/kes_cache.c:180) are two independent operations with no lock
  held across both. If two threads call `kes_cache_get_extent()` for
  the same uncached `id` at nearly the same time, both can miss in
  `hash_find`, both `calloc` + `init_extent_entry` a new
  `kes_extent_entry_t`, and both call `hash_insert()` -- which is a
  blind prepend onto the bucket's linked list with **no check for an
  existing entry with the same id**. Result: two separate cache
  entries for one extent, each independently issuing a `read_extent`
  I/O call, each tracked separately in stats/LRU. This is a logic bug
  (wasted I/O, incorrect caching semantics, a stale/duplicate entry
  that can outlive the "real" one), not a data race in the TSan sense
  -- the two entries are distinct objects, so ThreadSanitizer does not
  flag this on its own. It needs a dedicated correctness test to
  surface, which is exactly what the plan already asks for in
  `KES_HARDENING_PLAN.md` §6.C ("Two threads racing to be the one
  that populates a cache miss for the *same* extent ID
  simultaneously ... confirm only one actually issues the
  `read_extent` I/O call and the other correctly waits").
- Why it wasn't fixed here: a correct fix is a real design change (a
  get-or-create protocol: insert a placeholder entry in
  `KES_EXTENT_LOADING` state while holding the bucket lock across
  both the lookup and the insert, so a second racing thread finds the
  placeholder instead of creating a duplicate, then waits on it the
  same way the existing cache-hit path already waits out
  `KES_EXTENT_LOADING` at src/kes_cache.c:415-417). That's
  Phase 3/4-shaped work (it should share the same locking discipline
  §4.1 asks for in `kes_cache_sync()`), not a one-line lock-placement
  fix like the races already fixed this session.
- Existing test coverage: `test_concurrent_access` in
  `tests/test_kes_cache.c` already drives overlapping `start_block`
  ranges across its 4 threads (thread N covers blocks
  `N*10 .. N*10+49 mod 100`), so misses on the same id are already
  being exercised today -- it just isn't asserting anything that
  would catch the duplicate-entry outcome. A real fix needs its own
  assertion (e.g. hash-table entry count after the race, or asserting
  only one `read_extent` call happened for a given id) in addition to
  the locking fix.

---

## Phase 3 -- four missing `kes_cache.c` functions (P0/P1)

Not started. Full semantics are specified in
`KES_HARDENING_PLAN.md` §4 -- follow them precisely, they were
deliberately designed (the doc calls out real correctness hazards a
plausible-looking alternative would reintroduce):

- `kes_cache_sync()` -- §4.1. Note the locking discipline required
  (drop `cache_lock` before calling into `write_extent`, bump
  `ref_count` to pin the node you're mid-walk on so a concurrent free
  can't yank it out from under you). The stats-locking bugs fixed
  this session in `kes_cache_get_extent`/`kes_cache_flush_extent`
  are exactly the class of mistake this section is warning about --
  reread them before writing this function.
- `kes_cache_invalidate()` -- §4.2. Discards dirty data
  unconditionally, no implicit flush. Returns `KES_ERROR_BUSY` (not a
  new code) if `ref_count > 0 || pin_count > 0`.
- `kes_cache_reset_stats()` -- §4.3. Resets only cumulative counters
  (`hits`, `misses`, `evictions`, `flushes`, `bytes_read`,
  `bytes_written`); state counters (`memory_used`, `entries_cached`,
  `entries_dirty`, `entries_pinned`) must survive unchanged. Write
  the test that specifically checks this distinction.
- `kes_cache_start()` -- §4.4. Requires switching `bg_cond`'s clock
  attribute to `CLOCK_MONOTONIC` (via `pthread_condattr_setclock`)
  before the first timed wait is ever added -- do this as part of the
  same change, not as an afterthought, per the doc's explicit warning
  about retrofitting it later. Factor the flush-then-conditionally-
  free sweep into one internal static function shared by both
  `kes_cache_sync()` and the background thread loop.

Acceptance: unit tests for each function passing under normal build,
ASan, and TSan (the `check-all` target built this session is ready
for this). `kes_cache_start` specifically needs a test that proves
automatic background flushing happens, not just that the thread
doesn't crash.

## Phase 4 -- eviction / capacity enforcement (P1)

Not started. Depends on Phase 3's factored sweep logic. See
`KES_HARDENING_PLAN.md` §5 -- eviction on miss walking the LRU list
from `lru_tail`, `KES_ERROR_BUSY` when eviction can't free enough
room (recommended in the doc, but flag if implemented differently),
and `kes_cache_create()` rejecting `KES_CACHE_LFU`/`KES_CACHE_CUSTOM`
with `KES_ERROR_INVALID` per §5.3.

## Phase 5 -- test expansion (P1/P2)

Not started. This is the largest remaining phase -- see
`KES_HARDENING_PLAN.md` §6.A through §6.H (functional coverage per
public function, edge cases, concurrency/stress, fault injection,
persistence/crash-consistency, randomized/fuzz-adjacent testing,
performance smoke tests, long-run soak test). The duplicate-insert
bug above should get its dedicated test as part of §6.C.

## Phase 6 -- documentation truth pass (P2)

Not started. Correct `docs/CONTINUATION_PROMPT.md` and README status
badges once Phases 3-5 land; add a "Known Limitations" section. Do
this last, once everything it describes is actually true (per the
plan's §0 standing rule).

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
  path via `TRACE_ERR`/`TRACE_SYSERR`/`TRACE_ERRNO`) is still
  unaddressed in this repo's existing code. `kanek_foundations/src/trace.h`
  provides exactly those macros and is header-only for that subset
  (no linking needed, just `#include "trace.h"` once the include path
  is in place, which it now is). The natural place to start applying
  this is Phase 3's four new functions -- write them Rule-11-compliant
  from the start rather than retrofitting existing functions (per
  AGENTS.md's "no drive-by rewrites" guidance, existing early-return
  paths in `kes_bitmap.c`/`kes_storage.c`/`kes_cache.c` should only
  gain `TRACE_*` calls when you're already touching that function's
  body for another reason).
- Sanitizer/verification targets available: `make asan`, `make tsan`,
  `make valgrind`, `make sanitize-all`, `make check-all`. TSan
  binaries must be run under `setarch $(uname -m) -R` in this
  (WSL2) environment or they crash with "unexpected memory mapping"
  unrelated to any KES bug -- the `tsan` target already does this.
