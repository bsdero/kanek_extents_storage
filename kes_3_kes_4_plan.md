# KES-3 + KES-4 Implementation Plan — Cache Flush-Error State and Auto-Retry

**Read this whole file before touching any code, including the
"Critical interaction between these two fixes" section even if you
are only implementing one of them.** That section describes a real
data-destroying bug that only shows up when both fixes exist together
— if you implement KES-4 without reading it, you can silently
overwrite a caller's unflushed dirty data.

## What these fix

Both from `PENDING_FIXES_SEP2026.md`, both in `src/kes_cache.c`:

- **KES-3** (Medium): `kes_cache_flush_extent()` — the direct
  single-entry flush call — does not set `KES_EXTENT_ERROR` on a
  `write_extent()` failure, unlike `sweep_flush_and_maybe_evict()`
  (used by `kes_cache_sync()`'s sweep path and eviction), which does.
  A caller using `flush_extent()` directly has no way to tell, from
  the entry's own state, that a previous flush attempt failed.
- **KES-4** (Medium; decision confirmed with the project owner:
  **implement real auto-retry, not just documentation**): after a
  `read_extent()` failure, `kes_cache_get_extent()` leaves the entry
  permanently in `KES_EXTENT_ERROR`. A second call for the same id —
  even once whatever caused the failure is fully resolved — returns
  `KES_ERROR_IO` immediately without ever calling `read_extent()`
  again. Today the only recovery is an explicit
  `kes_cache_invalidate()` call.

---

## Critical interaction between these two fixes — read before writing any code

KES-3 makes `kes_cache_flush_extent()` set `entry->state |=
KES_EXTENT_ERROR` on a write failure, **without clearing
`KES_EXTENT_DIRTY`** (mirroring `sweep_flush_and_maybe_evict()`
exactly — the data was never actually persisted, so it must stay
marked dirty). That means, after KES-3, an entry can legitimately be
in state `KES_EXTENT_ERROR | KES_EXTENT_DIRTY` while still holding
**perfectly valid, unflushed caller data** in `entry->data` — the
write failed, but nothing is wrong with what's in memory.

KES-4's auto-retry, if implemented naively as "on `KES_EXTENT_ERROR`,
just call `read_extent()` again," would trigger for this case too —
and a **read** into `entry->data` would silently overwrite that valid,
unflushed dirty data with whatever stale bytes are actually on disk
(the write that would have made them current never landed). That is
real, silent data loss, worse than the bug KES-4 is fixing.

**The fix**: KES-4's auto-retry must only trigger when
`KES_EXTENT_ERROR` is set **and `KES_EXTENT_DIRTY` is not**. Confirmed
by reading every place `state` is set to `KES_EXTENT_ERROR` in the
current file:

- A load failure (`kes_cache_get_extent()`'s miss path) does `entry->state
  = KES_EXTENT_ERROR;` — a **plain overwrite**, not `|=`, on a
  brand-new candidate entry that was never dirty. Always
  ERROR-without-DIRTY.
- A flush failure (`sweep_flush_and_maybe_evict()` today, and
  `kes_cache_flush_extent()` after KES-3 below) does `entry->state |=
  KES_EXTENT_ERROR;` — an OR, deliberately preserving whatever
  `KES_EXTENT_DIRTY` bit was already set. Always ERROR-with-DIRTY (the
  entry was dirty before the failed flush attempt, by definition — you
  don't flush a clean entry).

So `ERROR` alone (without `DIRTY`) unambiguously means "no valid data,
safe to retry via a fresh read," and `ERROR | DIRTY` unambiguously
means "valid but unflushed data, must not be overwritten by a read."
This is not a heuristic — it follows directly from every code path
that can produce each combination. **Implement KES-3 first** (below),
since KES-4's retry logic directly depends on this distinction
existing correctly.

---

## Files you will touch

- `src/kes_cache.c` — `kes_cache_flush_extent()` (KES-3),
  `kes_cache_get_extent()` (KES-4)
- `tests/test_kes_fault_injection.c` — `test_load_failure_hash_table_state`
  needs rewriting (its entire premise is what KES-4 removes); one new
  test needed to lock in the KES-3/KES-4 interaction above

---

## Step 1 (KES-3) — `kes_cache_flush_extent()`: set `KES_EXTENT_ERROR` on write failure

Current code (`src/kes_cache.c`, inside the `if ( entry->state &
KES_EXTENT_DIRTY)` block):

```c
        int result = cache->write_extent( cache->config.device_handle,
                                           id, entry->data,
                                           entry->data_size);
        if ( result == KES_SUCCESS) {
            entry->state &= ~KES_EXTENT_DIRTY;
            entry->state |= KES_EXTENT_CLEAN;

            /*
             * bytes_written/flushes/entries_dirty are all
             * cache-wide state; bytes_written and flushes were
             * previously updated outside cache_lock (same bug
             * class fixed elsewhere in this file for
             * bytes_read/misses/memory_used).
             */
            pthread_mutex_lock( &cache->cache_lock);
            cache->stats.bytes_written += entry->data_size;
            cache->stats.flushes++;
            cache->stats.entries_dirty--;
            pthread_mutex_unlock( &cache->cache_lock);
        } else {
            pthread_mutex_unlock( &entry->lock);
            return(KES_ERROR_IO);
        }
```

Replace the `else` branch:

```c
        int result = cache->write_extent( cache->config.device_handle,
                                           id, entry->data,
                                           entry->data_size);
        if ( result == KES_SUCCESS) {
            entry->state &= ~KES_EXTENT_DIRTY;
            entry->state |= KES_EXTENT_CLEAN;

            pthread_mutex_lock( &cache->cache_lock);
            cache->stats.bytes_written += entry->data_size;
            cache->stats.flushes++;
            cache->stats.entries_dirty--;
            pthread_mutex_unlock( &cache->cache_lock);
        } else {
            /* KES-3: mirror sweep_flush_and_maybe_evict()'s failure
             * handling exactly (src/kes_cache.c) -- set
             * KES_EXTENT_ERROR via OR, deliberately leaving
             * KES_EXTENT_DIRTY set since the data was never actually
             * persisted, instead of only returning an error code with
             * no state change. Without this, a caller relying on the
             * entry's own state (rather than this call's return
             * value alone) to detect a previous flush failure -- the
             * same thing kes_cache_sync()'s sweep path already lets a
             * caller do -- had no way to tell this function's
             * failures apart from success. See this plan's "Critical
             * interaction" section for why KES_EXTENT_DIRTY must stay
             * set here, not just for symmetry with the sweep path. */
            entry->state |= KES_EXTENT_ERROR;
            TRACE_ERR( "kes_cache_flush_extent: write failed for "
                       "extent start_block=%llu block_count=%u",
                       (unsigned long long)id->start_block,
                       id->block_count);
            pthread_mutex_unlock( &entry->lock);
            return(KES_ERROR_IO);
        }
```

The function's return value is unchanged (`KES_ERROR_IO` either way) —
only the entry's own state now reflects the failure too, matching the
sweep path.

---

## Step 2 (KES-4) — `kes_cache_get_extent()`: auto-retry a stale `KES_EXTENT_ERROR` entry

Current code (`src/kes_cache.c`, the cache-hit path, right after the
existing `KES_EXTENT_LOADING` wait loop):

```c
    if ( entry->state & KES_EXTENT_LOADING) {
        entry->cond_waiters++;
        while ( entry->state & KES_EXTENT_LOADING) {
            pthread_cond_wait( &entry->cond, &entry->lock);
        }
        entry->cond_waiters--;
    }

    if ( entry->state & KES_EXTENT_ERROR) {
        pthread_mutex_unlock( &entry->lock);
        pthread_mutex_lock( &cache->cache_lock);
        cache->inflight_lookups--;
        pthread_mutex_unlock( &cache->cache_lock);
        return(KES_ERROR_IO);
    }
```

Replace with (this inserts a retry attempt **between** the two
existing blocks, and changes nothing about either of them otherwise):

```c
    if ( entry->state & KES_EXTENT_LOADING) {
        entry->cond_waiters++;
        while ( entry->state & KES_EXTENT_LOADING) {
            pthread_cond_wait( &entry->cond, &entry->lock);
        }
        entry->cond_waiters--;
    }

    /* KES-4: an entry stuck in KES_EXTENT_ERROR from a previous
     * failed load never got a second chance before this fix -- every
     * later kes_cache_get_extent() call returned KES_ERROR_IO
     * immediately, even once whatever caused the original failure was
     * resolved, until an explicit kes_cache_invalidate(). Retry
     * exactly once here, synchronously, before giving up for this
     * call -- deliberately NOT an unbounded retry loop: a
     * permanently broken backend must not turn a single
     * kes_cache_get_extent() call into an indefinite hang. If this
     * retry also fails, the entry is left in KES_EXTENT_ERROR again
     * and the next caller's own call gets its own one retry attempt,
     * the same way this one did.
     *
     * ONLY safe to do when KES_EXTENT_DIRTY is NOT also set -- see
     * this plan's "Critical interaction between these two fixes"
     * section. An entry that is ERROR *and* DIRTY (from a failed
     * flush, KES-3) still holds valid, unflushed caller data;
     * overwriting it with a fresh read here would silently destroy
     * it. That case falls through unchanged to the existing
     * KES_ERROR_IO return below, exactly as it did before this
     * fix. */
    if ( ( entry->state & KES_EXTENT_ERROR) &&
        !( entry->state & KES_EXTENT_DIRTY)) {
        entry->state = KES_EXTENT_LOADING;
        pthread_mutex_unlock( &entry->lock);

        int retry_result;
        if ( cache->read_extent != NULL) {
            retry_result = cache->read_extent(
                cache->config.device_handle, id, entry->data,
                entry->data_size);
        } else {
            TRACE_ERR( "kes_cache_get_extent: no read_extent "
                       "callback registered while retrying a "
                       "previously-failed load (call "
                       "kes_cache_set_io_callbacks() before using "
                       "the cache)");
            retry_result = KES_ERROR_INVALID;
        }

        pthread_mutex_lock( &entry->lock);
        if ( retry_result == KES_SUCCESS) {
            entry->state = KES_EXTENT_CLEAN;
        } else {
            entry->state = KES_EXTENT_ERROR;
            TRACE_ERR( "kes_cache_get_extent: retry of a previously-"
                       "failed load did not succeed either, for "
                       "extent start_block=%llu block_count=%u -- "
                       "left in KES_EXTENT_ERROR, a future caller "
                       "will retry again",
                       (unsigned long long)id->start_block,
                       id->block_count);
        }
        pthread_cond_broadcast( &entry->cond);
        pthread_mutex_unlock( &entry->lock);

        if ( retry_result == KES_SUCCESS) {
            pthread_mutex_lock( &cache->cache_lock);
            cache->stats.bytes_read += entry->data_size;
            pthread_mutex_unlock( &cache->cache_lock);
        }

        /* Re-acquire entry->lock -- everything below this point
         * (the final ERROR check, and the cache-hit success path
         * past it) still expects to hold it, same as on entry to
         * this whole function section. */
        pthread_mutex_lock( &entry->lock);
    }

    if ( entry->state & KES_EXTENT_ERROR) {
        pthread_mutex_unlock( &entry->lock);
        pthread_mutex_lock( &cache->cache_lock);
        cache->inflight_lookups--;
        pthread_mutex_unlock( &cache->cache_lock);
        return(KES_ERROR_IO);
    }
```

Two things about this shape, both deliberate:

1. **Lock discipline**: `entry->lock` is released before the I/O call
   (matching the existing miss-path convention — never hold an
   entry's lock across real I/O) and released again before taking
   `cache_lock` for the `bytes_read` update (matching the existing
   convention a few lines below in this same function — never hold
   `entry->lock` and `cache_lock` at the same time). Do not
   "simplify" this by holding one lock across the other; that
   convention is load-bearing elsewhere in this file (see the
   `try_evict_entry_locked()` doc comment for the history of why).
2. **Not `misses`**: on a successful retry, `cache->stats.bytes_read`
   is updated (real I/O happened) but `cache->stats.misses` is
   **not** incremented again — the miss was already counted once, when
   this entry was first created on the original failed load. A retry
   of an existing entry is a recovery, not a new distinct miss event.
3. **Reusing `KES_EXTENT_LOADING`, not a new flag**: setting
   `entry->state = KES_EXTENT_LOADING` during the retry means every
   existing piece of code that already protects a loading entry
   (`try_evict_entry_locked()`'s refusal to evict, `cond_waiters`'
   protection against a concurrent free, `kes_cache_flush_extent()`'s
   own wait-for-loading logic) automatically protects a mid-retry
   entry too, for free. A second thread that reaches this same
   function for the same id while a retry is in flight takes the
   *existing* `KES_EXTENT_LOADING` wait loop above (unchanged), not a
   new code path — it does not attempt its own concurrent retry.
4. **Known, accepted inefficiency**: if several threads are all
   already waiting (via the `KES_EXTENT_LOADING` loop above) when a
   retry finishes and fails again, each of them will, in turn,
   individually attempt its own single retry when it wakes and
   re-checks — not just the one thread that saw `ERROR` first. Under
   heavy concurrent contention against one permanently-broken id this
   means multiple sequential retries happen, not exactly one. This is
   still bounded (one retry per waiting thread, not unbounded) and is
   accepted as a minor inefficiency in an already-degraded-backend
   scenario, simpler than adding a second "retry in progress" signal
   distinct from `KES_EXTENT_LOADING`. Do not add that extra
   complexity as part of this plan.

---

## Step 3 — test changes

### 3a. `tests/test_kes_fault_injection.c`: `test_load_failure_hash_table_state`

This test's entire "OBSERVED GAP" section (the header comment) and its
second `kes_cache_get_extent()` call's assertions describe exactly the
behavior KES-4 removes. Read the current file's `test_load_failure_hash_table_state`
function (`tests/test_kes_fault_injection.c`) before editing — it has:

1. Setup: create cache, register `fi_read`/`fi_write`/`fi_sync`,
   configure `g_read_fault` to fail on call 1.
2. First `kes_cache_get_extent()` call: fails, `entry->state ==
   KES_EXTENT_ERROR`, `ref_count == 0`. (Keep this part unchanged —
   still correct after KES-4.)
3. `fi_reset( &g_read_fault)`, then a second `kes_cache_get_extent()`
   call, currently asserted to still fail with `KES_ERROR_IO` and
   `g_read_fault.call_count == 0` (proof `read_extent()` was never
   called again). **This is now wrong** — after KES-4, this call
   auto-retries and should succeed.
4. An `kes_cache_invalidate()` + third `get_extent()` call,
   demonstrating that as the *only* recovery path.

Replace step 3's assertions (`fi_reset( &g_read_fault); buf = NULL;
result = kes_cache_get_extent( cache, &id, &buf);` through the
`g_read_fault.call_count == 0` assertion) with:

```c
    /* Clear the failure condition and retry -- FIXED (KES-4): this
     * now succeeds directly, without needing an explicit
     * kes_cache_invalidate() first. */
    fi_reset( &g_read_fault);
    buf = NULL;
    result = kes_cache_get_extent( cache, &id, &buf);
    TEST_ASSERT( result == KES_SUCCESS,
                "FIXED (KES-4): retry with the failure condition "
                "cleared now succeeds -- kes_cache_get_extent() "
                "auto-retries a stale KES_EXTENT_ERROR entry once "
                "per call instead of returning KES_ERROR_IO forever");
    TEST_ASSERT( buf != NULL, "buffer is non-NULL on the recovered "
                "get");
    TEST_ASSERT( g_read_fault.call_count == 1,
                "proof a real retry attempt happened: the "
                "(already-cleared) fault harness saw exactly one "
                "call");

    entry = find_entry( cache, &id);
    TEST_ASSERT( entry != NULL && entry->state == KES_EXTENT_CLEAN,
                "the entry transitioned to KES_EXTENT_CLEAN after "
                "the successful retry");

    kes_cache_put_extent( cache, &id);
```

Then delete the now-redundant `kes_cache_invalidate()` +
third-`get_extent()` demonstration below it (lines that were
originally proving invalidate() as "the only recovery path" — it's
still *a* valid recovery path, just no longer the only one, and this
specific test's purpose was proving the stuck-forever gap, which no
longer exists). Update the `kes_cache_get_stats()` /
`stats.entries_cached == 1` assertion right after it to just confirm
`entries_cached == 1` following the direct-retry success above
(should already hold without the invalidate step).

Update the function's header comment (currently titled "REAL, OBSERVED
GAP... reported per rule 0.3, NOT fixed here") to describe the fix
instead, following this repo's `PENDING_ITEMS.md` "Resolved" section
convention (what was true, what fixed it, cite this plan/KES-4). Also
update the function's final `TEST_SUCCESS(...)` message, which
currently says "NOT auto-retried... real gap reported, not fixed."

### 3b. New test: KES-3/KES-4 interaction safety

Add a new test function to `tests/test_kes_fault_injection.c` proving
the "Critical interaction" section above holds in practice, not just
in code review. Place it near `test_flush_failure_dirty_state` (the
existing KES-3-adjacent test in this file) and register it in the
file's test-case list the same way every other test here is
registered.

```c
/*
 * KES-3 + KES-4 interaction: an entry left in KES_EXTENT_ERROR |
 * KES_EXTENT_DIRTY by a failed flush (KES-3) must NOT be
 * auto-retried via a fresh read (KES-4) -- that would silently
 * overwrite its still-valid, unflushed data with stale on-disk
 * bytes. See kes_3_kes_4_plan.md's "Critical interaction" section.
 */
static bool test_get_extent_does_not_retry_dirty_error_entry(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    void *buf = NULL;
    kes_extent_id_t id = make_id( 20);
    const char *dirty_payload = "unflushed-dirty-data";
    kes_extent_entry_t *entry;
    int result;

    kes_cache_get_default_config( &cfg, true);
    cache = kes_cache_create( &cfg);
    TEST_ASSERT( cache != NULL, "cache creation");
    kes_cache_set_io_callbacks( cache, fi_read, fi_write, fi_sync);

    fi_reset( &g_read_fault);
    fi_reset( &g_write_fault);

    /* Populate the entry normally, then dirty it with recognizable
     * data. */
    result = kes_cache_get_extent( cache, &id, &buf);
    TEST_ASSERT( result == KES_SUCCESS, "initial load succeeds");
    memcpy( buf, dirty_payload, strlen( dirty_payload) + 1);
    TEST_ASSERT( kes_cache_mark_dirty( cache, &id) == KES_SUCCESS,
                "mark dirty");
    kes_cache_put_extent( cache, &id);

    /* Force the next flush to fail. */
    g_write_fault.mode = FI_MODE_FAIL_NTH;
    g_write_fault.target_n = 1;
    result = kes_cache_flush_extent( cache, &id);
    TEST_ASSERT( result == KES_ERROR_IO, "flush fails as configured");

    entry = find_entry( cache, &id);
    TEST_ASSERT( entry != NULL, "entry still present after failed "
                "flush");
    TEST_ASSERT( (entry->state & KES_EXTENT_ERROR) &&
                (entry->state & KES_EXTENT_DIRTY),
                "FIXED (KES-3): entry is ERROR *and* DIRTY after the "
                "failed flush -- proves KES-3's fix landed");

    /* Clear the write fault (irrelevant -- get_extent() only reads)
     * and confirm a get_extent() call does NOT retry via a read. */
    fi_reset( &g_write_fault);
    fi_reset( &g_read_fault);
    buf = NULL;
    result = kes_cache_get_extent( cache, &id, &buf);
    TEST_ASSERT( result == KES_ERROR_IO,
                "FIXED (KES-4, safely): an ERROR+DIRTY entry is NOT "
                "auto-retried via a read -- that would destroy its "
                "still-valid unflushed data");
    TEST_ASSERT( buf == NULL, "buffer stays NULL");
    TEST_ASSERT( g_read_fault.call_count == 0,
                "proof no read was attempted at all for the "
                "ERROR+DIRTY case");

    /* The original dirty payload must be completely untouched. */
    TEST_ASSERT( memcmp( entry->data, dirty_payload,
                         strlen( dirty_payload) + 1) == 0,
                "CONFIRMED SAFE: the entry's unflushed dirty data is "
                "byte-for-byte unchanged -- KES-4's retry logic did "
                "not overwrite it");

    /* Recovery: flush again, now succeeding, clears both flags. */
    result = kes_cache_flush_extent( cache, &id);
    TEST_ASSERT( result == KES_SUCCESS, "retrying the flush directly "
                "(not via get_extent()) succeeds once the write fault "
                "is cleared");
    TEST_ASSERT( entry->state == KES_EXTENT_CLEAN,
                "entry is CLEAN after the successful flush");

    kes_cache_destroy( cache);
    TEST_SUCCESS( "KES-3/KES-4 interaction: an ERROR+DIRTY entry's "
                  "unflushed data survives untouched; get_extent()'s "
                  "auto-retry correctly excludes this case, and a "
                  "direct flush_extent() retry remains the correct "
                  "recovery path for it");
}
```

Add `{"get_extent does not retry dirty+error entry",
test_get_extent_does_not_retry_dirty_error_entry},` to this file's
test-case array, in the same position/style as its neighbors.

---

## Verification checklist

1. `make clean && make test` — all tests pass, including the
   rewritten `test_load_failure_hash_table_state` and the new
   `test_get_extent_does_not_retry_dirty_error_entry`.
2. `make asan` — clean.
3. `make tsan` — clean. Pay particular attention here: Step 2 adds a
   new unlock/I/O/re-lock window inside `kes_cache_get_extent()`,
   structurally similar to the existing miss path's own such window —
   confirm no new data race is introduced around `entry->data`/
   `entry->state` during that window, the same class of race
   `kes_cache_flush_extent()`'s own `KES_EXTENT_LOADING` wait exists to
   prevent.
4. `make valgrind` — clean.
5. `make check-all` — clean, "ALL CHECKS PASSED".
6. Re-run `tests/test_kes_cache.c` and `tests/test_kes_cache_full.c`
   specifically and confirm nothing that inspects `entry->state`
   directly (several tests in this codebase do, since
   `kes_extent_entry_t` is non-opaque) broke from the new
   `KES_EXTENT_LOADING` transition during a retry — none are expected
   to, since the retry window is extremely short-lived and none of
   those tests inject read/write faults, but confirm by actually
   running them.

## Explicitly out of scope

- Any retry *count* configuration (e.g. "retry N times with backoff")
  — this plan implements exactly one retry per call, a deliberate,
  simple, bounded choice. A configurable retry policy is a separate
  feature request, not part of KES-4 as scoped here.
- Extending the same auto-retry idea to `kes_cache_sync()`'s sweep
  path or `kes_cache_flush_extent()`'s own `KES_EXTENT_LOADING` wait —
  out of scope; this plan only touches `kes_cache_get_extent()`'s
  read-retry path.
- Any change to how `KES_EXTENT_ERROR` combined with `KES_EXTENT_DIRTY`
  is otherwise handled (e.g. whether `get_extent()` should be allowed
  to serve the still-valid dirty data to a caller instead of returning
  `KES_ERROR_IO`) — that pre-existing behavior is unchanged by this
  plan; changing it is a separate, bigger semantic question neither
  KES-3 nor KES-4 asked for.

## Bookkeeping when done

Same convention as the KES-5/KES-6 plans: move `PENDING_FIXES_SEP2026.md`'s
`## KES-3` and `## KES-4` sections to `CLOSED` with the commit
hash(es), and add matching evidence-first entries to
`PENDING_ITEMS.md`'s `## Resolved` section — for KES-4 specifically,
include the Critical Interaction finding and its dedicated test as
part of that writeup, the way the Track A.3.1 entry documents its own
non-obvious hazards, not just the headline fix.
