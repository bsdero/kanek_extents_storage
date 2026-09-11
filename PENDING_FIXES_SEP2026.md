# Pending Fixes -- September 2026

Numbered punch-list of every currently-open (not-yet-fixed) issue
against KES as of this writing (HEAD `ab1e419`), pulled from
`PENDING_ITEMS.md` and given stable IDs so implementation work,
commits, and follow-up discussion can reference them unambiguously
(`KES-1`, `KES-2`, ...). Written because two downstream storage
projects are about to start building on this library, so the goal is
to get this list to zero, or at minimum to a state where every
remaining item is a conscious, documented decision rather than an
unnoticed gap.

**This file does not replace `PENDING_ITEMS.md`** -- that remains the
detailed, evidence-first history of what was found, how, and what's
already fixed (read it for full citation trails, exact test names,
and pasted command output). This file is the short, numbered "what's
left" view derived from it, for tracking fixes one at a time. Update
both files together: when an item here is fixed, move its
`PENDING_ITEMS.md` entry to `## Resolved` in that file (matching the
evidence-first format already used there) and mark it CLOSED here
with the commit hash.

Severity is my judgment, not a formal SLA: **Critical** = real
memory-safety bug reachable in normal use, must fix before downstream
projects depend on the affected function. **High** = real correctness
gap under conditions downstream projects are likely to hit (concurrent
access, crash recovery). **Medium** = real gap, but narrower
conditions or has a documented workaround. **Low** = tech debt,
documentation, or a gap in a path that's optional/uncommon.

---

## KES-1 -- `kes_cache_destroy()` use-after-free under concurrent access

**Severity: Critical. Status: CLOSED -- fixed 2026-09-02, commit
`8d40167` (see the "Track A.3.1" entry in `PENDING_ITEMS.md`'s
`## Resolved` section for the full implementation writeup and
verification evidence).**

`kes_cache_destroy()` (`src/kes_cache.c`) frees every LRU entry
unconditionally -- no check of `ref_count`/`pin_count`, no
coordination with a caller still inside `kes_cache_get_extent()`/
`kes_cache_put_extent()` on the same cache. A standalone repro (one
thread looping `get_extent()`/`put_extent()`, another calling
`destroy()` ~2ms later) reproduces a heap-use-after-free under ASan on
every run, and multiple data races plus "use of an invalid mutex"
reports under TSan. The automated regression
(`test_cache_destroy_races_concurrent_access`,
`tests/test_kes_cache.c`) contains the crash inside a forked child
process specifically so it can't take down the whole test binary; it
does not gate any Makefile target today.

**Agreed fix plan** (full detail also in `PENDING_ITEMS.md`'s "Track
A.3.1 fix plan" section):

- **KES-1a -- reuse `ref_count`/`pin_count` via the existing
  eligibility check.** Rewrite `kes_cache_destroy()`'s entry-freeing
  loop to walk the LRU list the same lookup-pin-protected way
  `cache_sweep()` already does (`src/kes_cache.c:517-537`), then call
  `try_evict_entry_locked(cache, entry, true /* discard_dirty */)`
  (`src/kes_cache.c:353-403`) per node instead of freeing directly.
  Explicitly **not** pinning traversal nodes via `ref_count` itself --
  that specific approach is already documented as tried and
  ASan-confirmed broken (see the Phase 3 entry in `PENDING_ITEMS.md`);
  `lookup_pins` remains the traversal-safety field, unchanged.
- **KES-1b -- close the residual "brand-new entry vs. final teardown"
  gap.** A fresh `get_extent()` miss publishes into the hash table
  (`hash_find_or_insert()`, `src/kes_cache.c:905`) before it takes
  `cache_lock` to register in the LRU list
  (`src/kes_cache.c:919-925`); `ref_count` can't gate this window
  because the entry doesn't exist yet. Fix: add a small
  `uint32_t inflight_lookups` counter to `kes_cache_t`, protected by
  `cache_lock` only. `get_extent()` increments it before releasing
  `cache_lock` on entry, decrements it once done touching shared
  bookkeeping (not held across the actual I/O). `destroy()` requires
  `inflight_lookups == 0` (alongside "LRU list empty") before freeing
  `buckets`/`cache_lock`/`bg_cond`/`cache` itself.
- **KES-1c -- fail-fast `KES_ERROR_BUSY`, not a blocking drain.** If a
  pass leaves anything ineligible, leave the cache fully intact and
  return `BUSY` -- same contract `kes_cache_invalidate()` already uses
  for referenced/pinned entries. Avoids adding any new synchronization
  to `put_extent()`/`unpin_extent()`'s hot paths and can never leave
  the cache half-torn-down.
- **KES-1d** -- new "cache is shutting down" rejection in
  `get_extent()` returns `KES_ERROR_INVALID` (no more specific code
  exists in this API).
- **KES-1e** -- gate on a new `destroying` bool, not a reuse of the
  existing `shutdown` bool -- `kes_cache_stop()` is documented as
  restartable via `kes_cache_start()`, and reusing `shutdown` would
  silently break that cycle. Read under `cache_lock` (same lock
  `cache_bg_thread_func()` already reads `shutdown` under), not
  converted to atomics, to avoid mixing a lock-protected write with an
  atomic-only read.

**Implementation checklist**: struct fields in
`include/kes/kes_cache.h`; doc-comment rewrite for `kes_cache_destroy()`'s
new contract; the code changes in `src/kes_cache.c` (`get_extent()`
gate + counter, `destroy()`'s rewritten walk); flip
`test_cache_destroy_races_concurrent_access()` to assert a clean exit
(not "either outcome acceptable"); add a `BUSY`-path test; verify
under `make asan`/`make tsan`/`make check-all`; move both
`PENDING_ITEMS.md` entries to `## Resolved` and update `AGENTS.md`'s
Ground Truth section.

---

## KES-2 -- `kes_extent_allocate()` silently substitutes first-fit for unimplemented allocation strategies

**Severity: Medium. Status: CLOSED -- fixed 2026-09-11, commit
`cfa6261`, per `kes_2_kes_8_plan.md` (see the matching "KES-2" entry
in `PENDING_ITEMS.md`'s `## Resolved` section for the full
implementation writeup and verification evidence).**

`kes_allocation_strategy_t` (`include/kes/kes_types.h:58-64`) declares
`KES_ALLOC_BEST_FIT`/`WORST_FIT`/`NEXT_FIT` as public, settable enum
values with no runtime guard anywhere. `kes_extent_allocate()`'s
strategy dispatch (`src/kes_storage.c:269-274`) only has an explicit
`case` for `KES_ALLOC_FIRST_FIT`; every other value -- including the
three declared-but-unimplemented ones -- falls through `default:` to
`allocate_extent_first_fit()` with no error and no log line. A caller
who explicitly configures `KES_ALLOC_BEST_FIT` silently gets
`FIRST_FIT` placement instead, with no signal anything different
happened. Not covered by any existing test.

The cache layer already guards against exactly this category of gap
(`kes_cache_create()` returns `NULL` for `KES_CACHE_LFU`/`_CUSTOM`
rather than silently falling back to LRU) -- storage has no equivalent.

**Two open decisions for whoever fixes this** (not yet decided):
1. Short-term: reject `BEST_FIT`/`WORST_FIT`/`NEXT_FIT` at
   `kes_storage_open()` or `kes_extent_allocate()` time (mirroring the
   cache layer's pattern) until they're implemented for real.
2. Longer-term: actually implement best/worst/next-fit --
   `KES_HARDENING_PLAN.md` describes this as the main remaining
   allocator work but has no section written for *how*, unlike the
   detailed cache-layer work it specifies elsewhere.

Given two new storage projects are about to depend on this library,
my recommendation is to do (1) immediately (cheap, closes a silent
correctness surprise) regardless of when/whether (2) gets scheduled.

---

## KES-3 -- `kes_cache_flush_extent()` doesn't set `KES_EXTENT_ERROR` on a write failure

**Severity: Medium. Status: CLOSED -- fixed 2026-09-11, commit
`8f777ce`, per `kes_3_kes_4_plan.md` (see the matching "KES-3/KES-4"
entry in `PENDING_ITEMS.md`'s `## Resolved` section for the full
implementation writeup, the critical KES-3/KES-4 interaction finding,
and verification evidence).**

`sweep_flush_and_maybe_evict()` (used by `kes_cache_sync()`'s sweep
path and eviction) sets `KES_EXTENT_ERROR` on a failed flush, keeping
the entry marked dirty/errored rather than silently treating it as
clean. `kes_cache_flush_extent()` -- the direct single-entry flush
call, a different code path in `src/kes_cache.c` around lines
1247-1250 -- does **not**: it only returns `KES_ERROR_IO` to the
caller without updating `entry->state` at all. Confirmed still present
by direct code reading (not just inferred from history). This is a
real behavioral inconsistency between the two flush paths: a caller
using `flush_extent()` directly has no way to tell, from the entry's
own state, that a previous flush attempt failed, whereas a caller
relying on `sync()` does.

**Recommendation**: make `kes_cache_flush_extent()` set
`KES_EXTENT_ERROR` (and leave `KES_EXTENT_DIRTY` set) on a
`write_extent()` failure, mirroring `sweep_flush_and_maybe_evict()`
exactly -- this is a small, localized, low-risk change once KES-1's
locking work is settled (both touch nearby code in the same file).

---

## KES-4 -- Cache entries stuck permanently in `KES_EXTENT_ERROR` after a load failure never auto-retry

**Severity: Medium. Status: CLOSED -- fixed 2026-09-11, commit
`8f777ce`, per `kes_3_kes_4_plan.md` (decision confirmed with the
project owner: real auto-retry, not just documentation -- see the
matching "KES-3/KES-4" entry in `PENDING_ITEMS.md`'s `## Resolved`
section for the full implementation writeup, the critical KES-3/KES-4
interaction finding, and verification evidence).**

After a `read_extent()` failure, `kes_cache_get_extent()` leaves the
entry permanently in `KES_EXTENT_ERROR` state in the hash table
(`stats.entries_cached` still counts it). A *second* call for the same
id -- even once whatever caused the original failure is fully resolved
-- returns `KES_ERROR_IO` immediately without ever calling
`read_extent()` again (confirmed via a call-count assertion in
`test_load_failure_hash_table_state`,
`tests/test_kes_fault_injection.c`). The only recovery path is an
explicit `kes_cache_invalidate()` on that id before retrying; nothing
in the public API does this automatically, and nothing documents that
callers need to.

**Recommendation**: at minimum, document this clearly in
`kes_cache_get_extent()`'s doc comment (`include/kes/kes_cache.h`) so
callers know a transient I/O blip permanently poisons that cache
slot until explicitly invalidated. Consider (open design question, not
decided) whether `get_extent()` should itself detect a stale
`KES_EXTENT_ERROR` entry and retry the load automatically instead of
returning `KES_ERROR_IO` immediately -- this is a behavior change, not
a pure bug fix, so needs a decision before implementing.

---

## KES-5 -- Unsynchronized concurrent access to the same storage file from two `kes_storage_t*` instances corrupts state

**Severity: High. Status: CLOSED -- fixed 2026-09-11, see the matching
"KES-5" entry in `PENDING_ITEMS.md`'s `## Resolved` section for the
full implementation writeup and verification evidence.**

`kes_storage_t`'s `pthread_mutex_t` only coordinates threads within one
process; it cannot coordinate two independent processes (or even two
independent `kes_storage_open()` handles in the same process) against
the same backing file. `test_cross_process_racing_io`
(`tests/test_kes_multiprocess.c`) demonstrates real, concrete
corruption: two processes race `kes_extent_allocate()`/
`kes_extent_write()` against the same file with zero coordination;
each side's own self-reported free/used counts look internally
consistent (each only ever saw its own private in-memory bitmap), but
the real on-disk `used_blocks`, re-checked by a third, non-racing
reopen, is observed to be *less* than the combined allocations both
sides believed succeeded -- one side's `kes_storage_sync()` silently
clobbered the other's bookkeeping. `test_cross_process_sync_io` proves
the *externally synchronized* case round-trips correctly, but that's a
narrower guarantee than "safe for concurrent use."

This is the item I'd flag as highest-priority after KES-1 given the
stated goal: two new storage projects are about to be built on top of
this library, and any design that might open the same backing store
from more than one process (or crash-recover while another process
still holds it open) will hit this immediately and silently.

**Recommendation**: at minimum, document the constraint loudly in
`README.md`/`kes_storage.h` ("exactly one `kes_storage_t*` per backing
file at a time, enforced by the caller, not this library") before any
downstream project starts. A real fix (file locking via `flock()`/
`fcntl()` at `kes_storage_open()` time, rejecting a second concurrent
open) is a bigger, separate design decision -- flag it to whoever
owns the two new projects and let their access pattern (single-writer?
multi-reader? separate processes at all?) drive whether it's needed at
all before building it.

---

## KES-6 -- No bitmap checksum: a silently bit-flipped bitmap block causes real double-allocation

**Severity: High. Status: CLOSED -- fixed 2026-09-11, see the matching
"KES-6" entry in `PENDING_ITEMS.md`'s `## Resolved` section for the
full implementation writeup and verification evidence.**

`kes_bitmap_load()`/`kes_bitmap_save()` have no checksum or other
integrity check on the on-disk bitmap region (confirmed by reading
both). `test_bitflipped_bitmap_block`
(`tests/test_kes_crash_consistency.c`) flips one bit (used -> free)
for a block that is genuinely still allocated and holds live data,
then reopens: `kes_storage_open()` detects nothing.
`kes_storage_get_stats()`'s `free_blocks`/`used_blocks` (sourced from
the descriptor block) still report the correct pre-corruption counts,
while the live in-memory bitmap now silently disagrees with them by
exactly one bit -- two redundant sources of truth silently diverge
with no error surfaced anywhere. Concretely demonstrated as a real
hazard, not a bookkeeping curiosity: a fresh allocation hinted at that
exact block is handed the same still-live block back and silently
overwrites the original extent's still-valid data.

Given the two downstream projects are storage systems (where silent
data corruption is close to the worst possible failure mode), this and
KES-5 are the two items I'd prioritize resolving or at least
explicitly documenting-as-a-known-risk before other projects build on
top of `kes_storage_t`.

**Recommendation**: add a checksum (CRC32 or similar) over the bitmap
region, verified at `kes_bitmap_load()` time, returning
`KES_ERROR_CORRUPT` on mismatch instead of silently trusting corrupted
bytes. This is new functionality, not a one-line fix -- needs its own
design pass (on-disk format change, versioning/migration
consideration for any already-created storage files).

---

## KES-7 -- `KES_STORAGE_SYNC` flag's durability contract is undocumented

**Severity: Low (docs). Status: CLOSED -- documented 2026-09-11, per
`kes_7_kes_9_plan.md` (see the matching "KES-7" entry in
`PENDING_ITEMS.md`'s `## Resolved` section). Documentation-only
resolution, no behavior change.**

`include/kes/kes_types.h`'s `KES_STORAGE_SYNC` flag doc comment is a
single line ("Synchronous I/O") with no explicit durability promise
either way. Meanwhile, `test_no_sync_reopen_durability`
(`tests/test_kes_crash_consistency.c`) established concretely that:
raw extent *data* is always durable immediately (a direct, unbuffered
`write()` with no cache layer of its own), but `free_blocks`/
`used_blocks`/the bitmap itself are only persisted by
`kes_storage_sync()`/`kes_storage_close()` -- so a crash between
allocating+writing an extent and the next sync/close reopens into
stale bookkeeping that can hand the same blocks out again, silently
overwriting the "forgotten" extent's still-present data. Whether
`KES_STORAGE_SYNC` is supposed to change any of this is unstated.

**Recommendation**: document the actual current behavior precisely
(what's durable immediately vs. only on sync/close, and whether
`KES_STORAGE_SYNC` currently does anything at all for this path --
worth double-checking, since the two-line doc comment gives no
confidence either way). Cheap, should be done before any downstream
project makes an assumption about crash durability that isn't true.

---

## KES-8 -- Truncated storage descriptor returns `KES_ERROR_IO`, not `KES_ERROR_CORRUPT`

**Severity: Low (API consistency / documentation). Status: CLOSED --
documented, deliberately not behavior-changed, 2026-09-11, commit
`cfa6261`, per `kes_2_kes_8_plan.md` (see the matching "KES-8" entry
in `PENDING_ITEMS.md`'s `## Resolved` section for the full writeup and
verification evidence). This is a documentation-only resolution, not
a behavior fix -- both error codes are unchanged.**

`load_storage_descriptor()`: truncating the backing file to fewer
bytes than `sizeof(kes_storage_descriptor_t)` returns `KES_ERROR_IO`
(the `read()` byte-count check fails before the magic-number check is
ever reached) -- not `KES_ERROR_CORRUPT`, which is what a
magic-number mismatch on an otherwise-correctly-sized descriptor
returns instead (`test_truncated_and_corrupted_descriptor`,
`tests/test_kes_crash_consistency.c`). Both fail cleanly (no crash, no
`*storage` output), so this is not a safety bug -- but a caller trying
to distinguish "corrupt" from "truncated/missing" by return code alone
will be surprised. Worth a one-line doc note on
`kes_storage_open()`/the error codes rather than a behavior change,
unless whoever picks this up decides truncation should also report
`KES_ERROR_CORRUPT` for consistency.

---

## KES-9 -- Rule 11 (log before every early-return failure path) incompletely applied

**Severity: Low (observability / tech debt). Status: OPEN, ongoing --
ad-hoc policy reaffirmed 2026-09-11, see `kes_7_kes_9_plan.md` section
2 for the exact current boundary of what has Rule 11 logging vs. what
doesn't, and the decision record to keep it ad-hoc rather than sweep.**

`CODING_STYLE.md` Rule 11 requires `TRACE_ERR`/`TRACE_SYSERR`/
`TRACE_ERRNO` logging before every early-return failure path.
`kes_cache.c` and `kes_storage.c` both have this on several paths
added/touched during Phases 1-4, but most early-return paths in
`kes_bitmap.c`/`kes_storage.c`, and the simple NULL/not-found checks
throughout `kes_cache.c`, still fail silently (return an error code,
print nothing). Per `AGENTS.md`'s explicit "no drive-by rewrites"
guidance, this is meant to be applied to new/touched functions
incrementally, not swept across the whole codebase in one pass --
listed here so it isn't forgotten, not as something to fix all at
once. Worth a deliberate decision on whether the two new downstream
projects would benefit enough from consistent error logging across
this library to justify a dedicated sweep (explicitly opting in to
`AGENTS.md`'s "style sweep/retrofit" exception, file by file) rather
than the current ad-hoc pace.

---

## KES-10 -- Partial I/O transfer not detectable by the cache layer (design limitation, not a bug)

**Severity: Low (documented design gap). Status: documented, not
fixed -- 2026-09-11, per `kes_10_kes_11_plan.md`. Confirmed with the
project owner: document only, do not extend the callback signature
(see the matching "KES-10" entry in `PENDING_ITEMS.md`). The
`struct kes_cache` and `kes_cache_set_io_callbacks()` doc comments in
`include/kes/kes_cache.h` now state the contract precisely (a
callback must fully transfer `size` bytes or return an error). Still
OPEN by design if/when a downstream project actually needs
partial-transfer detection -- `kes_10_kes_11_plan.md`'s closing note
sketches the breaking-change shape for that.**

`test_partial_transfer_not_detected`
(`tests/test_kes_fault_injection.c`) confirmed this is structural, not
a bug: the `read_extent`/`write_extent` callback contract
(`include/kes/kes_cache.h`) is a plain `int` status code against a
fixed `size` *input* parameter, with no bytes-actually-transferred
*output* channel for `kes_cache_get_extent()` to check against. A
callback that reports `KES_SUCCESS` while only partially filling the
buffer is silently trusted. Not flagged as urgent since it requires a
misbehaving I/O callback to trigger (the two built-in-ish backends
this project ships don't do this) -- but if either downstream storage
project supplies its own I/O callback (e.g. over a network transport,
where partial reads are a real possibility), this is worth knowing
about upfront. Fixing it means extending the callback signature with
an output byte count, which is an API-breaking change -- worth
deciding now, before consumers exist, rather than later.

---

## KES-11 -- Performance smoke tests (§6.G) not implemented

**Severity: Low. Status: OPEN, intentionally deferred.** Confirmed
with the project owner on 2026-09-11: continue deferring rather than
write synthetic smoke tests now. **Concrete trigger to revisit**:
either downstream project (or this project's own maintainers) has a
real, specific workload characteristic to benchmark against -- e.g.
"extent allocate/free must sustain N ops/sec under M concurrent
callers" or "cache get/put p99 latency must stay under X ms at Y
entries" -- at which point write a smoke test against *that* number,
not an invented one. Until such a number exists, a synthetic benchmark
risks becoming a false signal: a "regression" against a baseline
nobody asked for, or false confidence from a synthetic pattern that
doesn't resemble either downstream project's actual access pattern.
See `kes_10_kes_11_plan.md`.

`KES_HARDENING_PLAN.md` §6.G calls for basic performance smoke tests;
`plan_phase5.md` explicitly marks this optional/stretch and
non-blocking, and it was skipped in favor of finishing the required
fault-injection/crash-consistency/fuzz/soak work.

---

## KES-12 -- `AGENTS.md` Ground Truth section needs reconciliation after Track B docs work

**Severity: Low (bookkeeping). Status: CLOSED -- reconciled
2026-09-11.** The "Phase 6 (docs, partially done)" language was
already gone by the time this pass checked (a prior session had
updated it to "both complete"), but two other claims in the same
"Ground truth" section had gone stale after later fixes landed and
needed correcting:

- The `test_kes_multiprocess` bullet still said unsynchronized
  cross-process/cross-handle access to the same storage file "remains
  an open, unguarded gap" -- stale after KES-5's fix (commit
  `2022272`, `flock()`-based serialization). Updated to note KES-5 is
  fixed and closed, and to mention KES-6's bitmap checksum fix
  (commit `ffa485a`), which the section previously didn't cover at
  all.
- The Architecture section's extent-allocation description still said
  `kes_extent_allocate()` "currently only implements first-fit ...
  regardless of the `kes_allocation_strategy_t` requested" with no
  mention that this silent substitution was fixed -- stale after
  KES-2's fix (commit `cfa6261`, `validate_config()` now rejects any
  non-`KES_ALLOC_FIRST_FIT` strategy). Updated accordingly; first-fit
  is still the only strategy actually implemented, only the silent
  substitution is gone.

Purely a documentation-sync task, no behavior change.

---

## Suggested order of attack

Given the stated goal (get this library solid before two storage
projects start depending on it), my suggested priority order is:

1. **KES-1** (destroy() UAF) -- **CLOSED**, see above. Was the
   highest-severity confirmed memory-safety bug in the library.
2. **KES-5** and **KES-6** (unsynchronized cross-process access,
   missing bitmap checksum) -- both are silent-data-corruption risks
   in the storage layer specifically, which is what both downstream
   projects will sit directly on top of. At minimum, document both
   loudly (cheap) even if the real fixes (file locking, checksums) get
   scheduled separately.
3. **KES-2, KES-3, KES-4** -- medium-severity, well-understood,
   comparatively cheap fixes once KES-1's locking changes are settled
   (KES-3 touches adjacent code).
4. **KES-7, KES-8, KES-9, KES-10, KES-12** -- documentation/consistency
   items, cheap to do opportunistically alongside the above.
5. **KES-11** -- revisit once real consumers exist.
