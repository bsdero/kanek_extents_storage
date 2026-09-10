# KES-7 + KES-9 Implementation Plan — Documentation and Logging Policy

**Read this whole file before touching anything.** Unlike the other
plans in this set, one of these two items (KES-9) has **no code
change at all** — its "implementation" is writing down a decision so
it stops being ambiguous. Do not interpret "no code change" as "skip
this file"; the point is that a future reader (human or AI) hits a
clear, dated decision instead of re-deriving whether a sweep is
warranted.

## What these are

Both from `PENDING_FIXES_SEP2026.md`:

- **KES-7** (Low, docs): `include/kes/kes_types.h`'s
  `KES_STORAGE_SYNC` flag doc comment is a single line ("Synchronous
  I/O") with no explicit durability promise either way.
  `tests/test_kes_crash_consistency.c`'s `test_no_sync_reopen_durability`
  already established the actual behavior concretely; this fix writes
  that down where a caller would actually look for it.
- **KES-9** (Low, ongoing): `CODING_STYLE.md` Rule 11 (log via
  `TRACE_ERR`/`TRACE_SYSERR`/`TRACE_ERRNO` before every early-return
  failure path) is only partially applied — most early-return paths in
  `kes_bitmap.c`/`kes_storage.c`, and the simple NULL/not-found checks
  throughout `kes_cache.c`, still fail silently. **Decision confirmed
  with the project owner: keep the current ad-hoc policy, do not do a
  full sweep now.** This plan documents that decision and the exact
  boundary of what's already covered vs. not, so a future contributor
  doesn't have to re-derive it from scratch or wonder if it was an
  oversight.

These are grouped together because both are "write down the actual
state of things clearly" tasks with no risk of behavior change, unlike
every other plan in this set.

---

## Step 1 (KES-7) — document `KES_STORAGE_SYNC`'s actual effect

### 1a. `include/kes/kes_types.h`

Current code:

```c
/* Storage flags */
typedef enum {
    KES_STORAGE_READONLY    = 0x01,  /* Read-only access */
    KES_STORAGE_CREATE      = 0x02,  /* Create if not exists */
    KES_STORAGE_TRUNCATE    = 0x04,  /* Truncate existing */
    KES_STORAGE_SYNC        = 0x08   /* Synchronous I/O */
} kes_storage_flags_t;
```

Replace with (the enum's own inline comments are untouched except for
`KES_STORAGE_SYNC`'s, which now points at a detailed block comment
placed right after the enum):

```c
/* Storage flags */
typedef enum {
    KES_STORAGE_READONLY    = 0x01,  /* Read-only access */
    KES_STORAGE_CREATE      = 0x02,  /* Create if not exists */
    KES_STORAGE_TRUNCATE    = 0x04,  /* Truncate existing */
    KES_STORAGE_SYNC        = 0x08   /* Synchronous I/O -- see the
                                       * detailed note below (KES-7)
                                       * for exactly what this does
                                       * and does not make durable. */
} kes_storage_flags_t;

/*
 * KES-7: KES_STORAGE_SYNC's actual, confirmed effect (by reading
 * kes_storage_create()/kes_storage_open(), src/kes_storage.c): it
 * adds O_SYNC to the backing fd's open()/create() flags. Nothing
 * else. The kes_storage_t.sync_writes field it also sets
 * (include/kes/kes_storage.h) is stored but never read anywhere else
 * in this codebase (confirmed by grep across src/) -- it has no
 * effect of its own beyond that one open()-time O_SYNC flag.
 *
 * What this precisely changes:
 *   - WITHOUT KES_STORAGE_SYNC: kes_extent_write()'s raw write() call
 *     lands in the OS page cache. It is visible to any other process
 *     reading the same file immediately (ordinary page-cache
 *     coherency) and survives this process exiting or crashing
 *     normally -- but is NOT guaranteed to survive a real power loss
 *     or kernel crash until something calls fsync() on this fd.
 *     kes_storage_sync()/kes_storage_close() are the only calls in
 *     this library that do that.
 *   - WITH KES_STORAGE_SYNC: every write on this fd -- extent data via
 *     kes_extent_write(), and the descriptor/bitmap writes inside
 *     kes_storage_sync()/kes_storage_close()/kes_storage_create() --
 *     becomes synchronous at the kernel level (O_SYNC): durable
 *     against real power loss the moment the write() call returns, at
 *     a real per-write latency cost.
 *
 * What this does NOT change: kes_storage_sync()/kes_storage_close()
 * remain the only calls that persist storage->desc.free_blocks/
 * used_blocks/storage->bitmap to disk at all -- O_SYNC only affects
 * the durability of a write that already happens, it does not cause
 * any additional writes to happen. See
 * tests/test_kes_crash_consistency.c's test_no_sync_reopen_durability
 * for the concrete consequence: a crash between an allocation and an
 * explicit sync can still lose that allocation's bookkeeping even
 * with KES_STORAGE_SYNC set, even though the extent DATA itself was
 * already durable. (If kes_5_plan.md has been applied, this no
 * longer applies -- kes_extent_allocate()/kes_extent_free() persist
 * their own bookkeeping immediately regardless of this flag.)
 */
```

### 1b. `include/kes/kes_storage.h`

Update `kes_storage_sync()`'s doc comment to point at the flag doc and
state precisely what becomes durable:

```c
/**
 * Synchronize all pending changes to storage: writes the in-memory
 * bitmap and descriptor (free/used block counts, next_extent_id,
 * etc.) to disk and fsync()s the file. This is what makes ALLOCATION
 * BOOKKEEPING durable -- extent DATA written via kes_extent_write()
 * has its own, partly independent durability story (see
 * KES_STORAGE_SYNC's doc comment in kes_types.h, KES-7).
 * @param storage Storage handle to sync
 * @return KES_SUCCESS or error code
 */
int kes_storage_sync( kes_storage_t *storage);
```

Update `kes_storage_close()`'s doc comment similarly (it already does
the same save-then-fsync work as `kes_storage_sync()` before actually
closing, when not readonly):

```c
/**
 * Close and cleanup storage instance. If not opened readonly, this
 * does the same descriptor/bitmap save + fsync() kes_storage_sync()
 * does (KES-7's durability note in kes_types.h applies here too)
 * before releasing resources.
 * @param storage Storage handle to close
 * @return KES_SUCCESS or error code
 */
int kes_storage_close( kes_storage_t *storage);
```

No test changes for this step — nothing here changes behavior, only
documentation.

---

## Step 2 (KES-9) — reaffirm the ad-hoc Rule 11 policy, document the exact boundary

**No code change in this step.** `AGENTS.md` already states the
policy ("apply Rule 11 to new functions, and to an existing function's
error paths only when you're already touching that function's body for
another reason — don't do a blanket sweep unless asked"). This step
exists to record, as of this writing, exactly where that boundary
currently sits, confirmed by reading the actual files rather than
trusting the last time someone wrote this down — so the next person
who wonders "is this an oversight or a decision?" has a dated answer
instead of having to re-audit the whole codebase themselves.

### 2a. Confirmed current state (re-verify before trusting this list —
it will drift as other plans in this set land)

**Has Rule 11 logging** (added deliberately during Phases 1–4 and the
KES-1/KES-2/KES-3/KES-4 fixes, where applicable):
- `kes_cache.c`: the no-`read_extent`-callback path, the load-failure
  path, the no-`write_extent`-callback path in
  `kes_cache_flush_extent()` (and, after KES-3's fix, its write-failure
  path too), the cache-full-can't-evict path in
  `kes_cache_get_extent()`, `kes_cache_start()`'s
  already-running/`pthread_create`-failure paths, the sweep/eviction
  flush-failure paths, and (after KES-4's fix) the retry-failure path.
- `kes_storage.c`: the double-free/invalid-extent path in
  `kes_extent_free()`, and (after this plan set's KES-2 fix) the
  unimplemented-strategy rejection in `validate_config()`.

**Still silent** (returns an error code, logs nothing):
- Most early-return paths in `kes_bitmap.c` — every `KES_ERROR_INVALID`
  NULL/out-of-range check across `kes_bitmap_set`/`kes_bitmap_clear`/
  `kes_bitmap_test`/`kes_bitmap_set_range`/`kes_bitmap_clear_range`/
  `kes_bitmap_find_free`/`kes_bitmap_get_stats`/`kes_bitmap_load`/
  `kes_bitmap_save`, and `kes_bitmap_create`'s NULL/zero check. (The
  one exception: `kes_bitmap_checksum()`, if KES-6's plan has been
  applied — that is a brand-new function, written Rule-11-compliant
  from the start per the "new functions" half of the policy.)
- Most early-return paths in `kes_storage.c` outside what's listed
  above — e.g. `kes_storage_create()`/`kes_storage_open()`'s NULL
  checks, `load_storage_descriptor()`'s IO/magic-mismatch returns
  (unless KES-8's plan has added a comment there — a comment is not a
  `TRACE_ERR` call, KES-8 deliberately did not add one, see
  `kes_2_kes_8_plan.md`), `kes_extent_read()`/`kes_extent_write()`'s
  bounds checks.
- The simple NULL/not-found `KES_ERROR_INVALID`/`KES_ERROR_NOTFOUND`
  checks throughout `kes_cache.c` that aren't part of the specific
  paths listed above (e.g. `kes_cache_put_extent`/
  `kes_cache_pin_extent`/`kes_cache_unpin_extent`/
  `kes_cache_mark_dirty`'s basic argument validation).

### 2b. The decision, stated for the record

Confirmed with the project owner (this plan's own creation): **do not
do a dedicated sweep.** Continue the existing ad-hoc policy —
`TRACE_ERR`/`TRACE_SYSERR`/`TRACE_ERRNO` on every early-return path in
any *new* function, and on an *existing* function's error paths only
when already rewriting that function's body for an unrelated reason
(exactly what KES-2's `validate_config()` hunk and KES-4's
`kes_cache_get_extent()` rewrite in this plan set both do, incidentally
extending the "has logging" list above without a dedicated sweep).

**If this decision is ever revisited**: `AGENTS.md` already names the
mechanism — explicitly opt into the "style sweep/retrofit" exception
it documents, file by file, saying which files were touched. Section
2a above is the starting checklist for exactly which files/functions
that sweep would need to cover; re-verify it against the tree at that
time rather than trusting it verbatim, since other plans in this set
(and any work done between now and then) will have moved individual
items from the "still silent" list to the "has logging" list
incrementally, the same way KES-2/KES-4 already did.

---

## Verification checklist

1. `make clean && make test` — all tests still pass (KES-7 and KES-9
   as scoped here make zero behavior changes; this step is confirming
   that, not discovering it).
2. Read the new `KES_STORAGE_SYNC` block comment back once more after
   writing it and confirm it does not overclaim anything beyond what
   `test_no_sync_reopen_durability` actually demonstrated — this doc
   note is only as trustworthy as it is accurate; do not extend it
   with plausible-sounding claims about durability that weren't
   actually verified by that test or by reading the code.

## Explicitly out of scope

- Any actual Rule 11 sweep (KES-9's decision above is explicit about
  this).
- Any change to `KES_STORAGE_SYNC`'s actual runtime behavior — this
  step is documentation only.
- Removing the dead `kes_storage_t.sync_writes` field noted in Step
  1a's new comment. It is genuinely unused, but deleting a struct
  field the way this codebase's own "no drive-by rewrites" rule treats
  such changes is a separate, deliberate cleanup decision, not
  something to fold into a docs-only plan. Flag it for whoever next
  touches `kes_storage_t` for an unrelated reason, per that same rule.

## Bookkeeping when done

- KES-7: move `PENDING_FIXES_SEP2026.md`'s `## KES-7` section to
  `CLOSED` (documentation fix) with the commit hash, and add a short
  entry to `PENDING_ITEMS.md`'s `## Resolved` section.
- KES-9: do **not** mark this `CLOSED` in the same sense as a fixed
  bug — it is an ongoing policy, not a bounded piece of work. Instead,
  update `PENDING_FIXES_SEP2026.md`'s `## KES-9` section to record the
  date this decision was reaffirmed and link to this plan file, so a
  future reader sees "reaffirmed as ad-hoc on `<date>`, see
  `kes_7_kes_9_plan.md`" instead of the item just looking permanently
  open with no recent decision behind it.
