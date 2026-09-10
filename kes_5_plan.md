# KES-5 Implementation Plan — Cross-Process Storage Access Safety

**Read this whole file before touching any code.** It is written for
an implementer with zero prior context on this decision. Pay special
attention to the "Why locking alone is not enough" section below — the
obvious-looking shortcut (just wrap the existing code in a lock) was
checked and does **not** fix the bug; skipping that section is the
most likely way to produce code that looks right, compiles, passes a
naive test, and still corrupts data under real concurrent access.

## Dependency on KES-6 — read this first

**This plan assumes `kes_6_plan.md` has already been applied.** The
per-operation persist step this plan adds calls `kes_bitmap_checksum()`
and writes `storage->desc.bitmap_checksum`, both of which only exist
after KES-6's changes. If KES-6 has not been applied yet, either:

- do it first (recommended — the two are meant to land together), or
- apply this plan with the checksum lines removed (each code block
  below marks exactly which 3-4 lines to drop if skipping KES-6, under
  an "IF NOT APPLYING KES-6" note).

Do not silently skip KES-6's checksum lines without also removing
them — leaving a call to a function that doesn't exist yet is a build
break, not a partial implementation.

## What this fixes

`KES-5` in `PENDING_FIXES_SEP2026.md`: `kes_storage_t`'s
`pthread_mutex_t` only coordinates threads within one process. It
cannot coordinate two independent processes — or even two independent
`kes_storage_open()` handles in the same process — against the same
backing file. `tests/test_kes_multiprocess.c`'s
`test_cross_process_racing_io` demonstrates real, concrete corruption
today: two processes race `kes_extent_allocate()`/`kes_extent_write()`
against the same file with zero coordination, and the real on-disk
`used_blocks` afterward is *less* than the combined allocations both
sides believed succeeded — one side's `kes_storage_sync()` silently
clobbered the other's.

## Decision already made (do not re-litigate)

**OS-level serialization, not a single-writer lock.** Confirmed with
the project owner. Multiple concurrent opens of the same file (from
different processes, or from independent handles in the same process)
remain a supported pattern — `kes_storage_open()` does **not** reject
a second concurrent open. Instead, each *mutating* operation
(`kes_extent_allocate()`, `kes_extent_free()`, `kes_storage_sync()`,
`kes_storage_close()`) becomes safe against concurrent callers through
OS-level file locking plus a mandatory reload from disk (see below).
This is deliberately chosen so the existing `test_kes_cross_process_sync_io`
test (two processes, externally synchronized via semaphores, both
holding the file open at once) keeps working unchanged, and so
`test_cross_process_racing_io` can be upgraded from "documents
corruption" to "proves correctness" instead of needing a redesign.

`kes_extent_read()`/`kes_extent_write()` are **not** changed by this
plan — see "Explicitly out of scope" at the end for why.

---

## Why locking alone is not enough

This is the part that is easy to get wrong, so read it carefully
before writing any code.

The corruption in `test_cross_process_racing_io` is **not** caused by
two raw `write()` syscalls tearing each other mid-flight — individual
`write()`/`read()` calls on a local file are not the problem. It is a
**lost-update** problem: each `kes_storage_t*` handle loads its own
private copy of the bitmap into memory exactly once, at
`kes_storage_open()` time (`kes_bitmap_load()`, called once inside
`kes_storage_open()`), and only ever writes it back on an explicit
`kes_storage_sync()`/`kes_storage_close()`. Between those two points,
every `kes_extent_allocate()`/`kes_extent_free()` call mutates
**only** that handle's in-memory copy.

Concretely, with two processes A and B, each holding its own handle on
the same file:

1. A and B both `open()` around the same time; each loads an identical
   copy of the on-disk bitmap into its own memory.
2. A calls `kes_extent_allocate()`. This mutates only A's in-memory
   bitmap. B's in-memory bitmap does not change and does not know
   about it.
3. B calls `kes_extent_allocate()`, possibly for a completely
   different reason, at any later point — even seconds later. B's
   allocator searches B's *own, stale* in-memory bitmap, which still
   thinks every block is free the way it was at `open()` time. B can
   pick the exact same blocks A already took.
4. Whichever of A or B calls `kes_storage_sync()`/`kes_storage_close()`
   *last* wins: it writes its own in-memory bitmap over the file
   verbatim, silently erasing whatever the other side already
   persisted — even for blocks the other side allocated that B never
   touched at all.

**Simply wrapping the existing code in `flock()` around each
individual call does not fix this.** A lock only prevents two calls
from literally overlapping in time; it does nothing to stop B's
`kes_extent_allocate()` (step 3) from happening strictly *after* A's
(now safely serialized by the lock) but still operating on the bitmap
B loaded before A's change existed. The lock would make the race
deterministic instead of racy, but the outcome — B silently reusing or
clobbering A's allocation — would be identical.

**The actual fix**: each mutating operation must become an atomic
*read-reload-mutate-write* cycle against the file, not just a
mutate-in-memory operation guarded by a lock. Specifically, under a
single exclusive lock, each operation must:

1. Reload the descriptor and bitmap **fresh from disk** (discarding
   whatever this handle had in memory), so it never acts on stale
   state.
2. Perform the mutation against that freshly-reloaded state.
3. Write the result back to disk immediately, before releasing the
   lock — so the *next* holder of the lock, in any process, sees this
   change when it does its own reload.

This makes every allocate/free call self-contained and durable on its
own, rather than relying on a separate, possibly-much-later
`kes_storage_sync()` call. That has two further consequences worth
knowing before you start (both are good news, and both require test
changes covered in Step 6 below):

- `kes_storage_sync()`'s own job shrinks to "refresh this handle's
  view and force an fsync" — it can no longer be the *only* thing that
  makes an allocation durable, because allocation is now durable on
  its own.
- It also incidentally closes a second, previously-documented gap:
  `PENDING_ITEMS.md`'s A.5.1 finding that a crash after
  `kes_extent_allocate()` but before an explicit `kes_storage_sync()`
  silently "forgets" the allocation. After this fix, that scenario no
  longer loses anything, because the allocate call already persisted
  itself. This is a real, positive side effect of the correct fix for
  KES-5 — not a separate feature being smuggled in — but it means an
  existing test built around that old behavior (`test_no_sync_reopen_durability`)
  will now fail unless you update it (Step 6a).

---

## Why the lock must be `flock()`, not `fcntl()` record locks

Use `flock(storage->fd, LOCK_EX)` / `flock(storage->fd, LOCK_UN)`
(`<sys/file.h>`), not POSIX `fcntl()` byte-range locks. The reason
matters: `flock()` locks are associated with the *open file
description* (i.e. each `open()` call gets its own independent lock),
while `fcntl()` record locks are associated with the *process* and the
*inode* — all locks a single process holds on the same file are
merged/released together when *any* file descriptor for that file in
that process is closed, regardless of which `open()` call produced it.
That would silently break the same-process "two independent handles on
one file" case (also a real scenario this fix must cover, not just
the cross-process one) in a way that is easy to miss in testing. Local
files (this project has no NFS requirement anywhere in its docs) are
exactly `flock()`'s intended use case.

A crashed process automatically releases its `flock()` locks when its
last file descriptor for that open file description is closed by the
kernel on process exit — so a process crashing mid-critical-section
does not deadlock other holders. No timeout/retry logic is needed for
this plan.

---

## Files you will touch

- `src/kes_storage.c` — `#include <sys/file.h>`; rewrite
  `kes_extent_allocate()`, `kes_extent_free()`, `kes_storage_sync()`,
  the bitmap-save block inside `kes_storage_close()`, and add a bitmap
  write to `kes_storage_create()`
- `tests/test_kes_crash_consistency.c` — `test_no_sync_reopen_durability`
  (both Case 1 and Case 2) needs rewriting; their premises no longer
  hold
- `tests/test_kes_multiprocess.c` — `test_cross_process_racing_io`
  should be upgraded from "doesn't crash" to "produces correct
  accounting"
- `README.md` — note the performance trade-off (Step 7)
- `PENDING_ITEMS.md` / `PENDING_FIXES_SEP2026.md` — bookkeeping once
  done

No header (`.h`) changes are needed for this plan — `flock()` state is
tracked by the kernel against the existing `storage->fd`; no new
struct field is required.

---

## Step 1 — `src/kes_storage.c`: add the include

Current top of file:

```c
#include <kes/kes_storage.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include "trace.h"
```

Add `<sys/file.h>` (for `flock()`, `LOCK_EX`, `LOCK_UN`):

```c
#include <kes/kes_storage.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>
#include "trace.h"
```

---

## Step 2 — `kes_storage_create()`: write the initial bitmap to disk

**Why this is required, not optional polish**: without this, the file
is only as large as the descriptor block right after `create()`
returns — `kes_storage_create()` has never called `kes_bitmap_save()`.
The very first `kes_extent_allocate()`/`kes_extent_free()` call after
`create()` will, under this plan's changes, try to reload the bitmap
from disk (Step 3) before it does anything else — and that reload will
fail with `KES_ERROR_IO` reading a region of the file that was never
written, breaking the extremely common "create, then immediately
allocate" workflow. This step exists to prevent that regression, not
as a general durability improvement (though it happens to be one too —
see the comment in the code below).

Current code (inside `kes_storage_create()`, after bitmap creation —
if you already applied KES-6's Step 5a, your checksum block sits
between these two, and this new block goes right after it; either
way, both blocks land between `kes_bitmap_create()` succeeding and
`save_storage_descriptor()`):

```c
    /* Create bitmap */
    result = kes_bitmap_create( sto->desc.user_blocks, &sto->bitmap);
    if ( result != KES_SUCCESS) {
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }

    /* [KES-6's checksum-setting block goes here if already applied] */

    /* Save initial descriptor to storage */
    result = save_storage_descriptor( sto);
```

Insert this block immediately before `/* Save initial descriptor to
storage */` (after KES-6's block, if present):

```c
    /* KES-5: physically write the freshly-created (all-zero) bitmap
     * to disk now, not just its checksum into the descriptor (if
     * KES-6 is applied). Without this, the file is not yet fully
     * laid out on disk, so the very first kes_extent_allocate()/
     * kes_extent_free() call's mandatory reload-from-disk step (see
     * those functions below) would try to read a bitmap region that
     * was never written and fail with KES_ERROR_IO on what should be
     * a normal, successful first allocation right after create().
     * This also happens to close most of the separate, previously-
     * documented "storage file unopenable after a crash before any
     * sync" gap (PENDING_ITEMS.md A.5.1 Case 1) as a side effect,
     * though that is not this change's goal -- see
     * test_no_sync_reopen_durability's updated Case 1 in this plan's
     * Step 6a. */
    off_t bitmap_offset = sto->desc.bitmap_start_block *
                           sto->desc.block_size;
    result = kes_bitmap_save( sto->bitmap, sto->fd, bitmap_offset);
    if ( result != KES_SUCCESS) {
        kes_bitmap_destroy( sto->bitmap);
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }

    /* Save initial descriptor to storage */
    result = save_storage_descriptor( sto);
```

Two independent processes both racing `kes_storage_create()` with
`KES_STORAGE_CREATE`/`KES_STORAGE_TRUNCATE` on the *same, not-yet-
existing* file is a separate, even messier race (both truncating/
initializing the file at once) — **explicitly out of scope for this
plan**. This plan only makes an *already-created* file safe for
concurrent `kes_storage_open()` handles. Do not add locking to
`kes_storage_create()` as part of this step.

---

## Step 3 — rewrite `kes_extent_allocate()`

Replace the entire function body with:

```c
int kes_extent_allocate( kes_storage_t *storage,
                          const kes_extent_request_t *request,
                          kes_extent_descriptor_t *extent) {
    if ( storage == NULL || request == NULL || extent == NULL ||
        storage->readonly) {
        return(KES_ERROR_INVALID);
    }

    if ( request->block_count == 0) {
        return(KES_ERROR_INVALID);
    }

    pthread_mutex_lock( &storage->lock);

    /* KES-5: acquire an exclusive advisory lock on the backing file
     * itself, not just the in-process mutex above -- pthread_mutex_t
     * only coordinates threads within this one process, and cannot
     * stop a second kes_storage_t* (another process, or another
     * independent open() in this same process) from racing this
     * allocation against the same on-disk bitmap. flock() is scoped
     * to the open file description, so it correctly covers both
     * cases with no extra IPC of its own. */
    if ( flock( storage->fd, LOCK_EX) != 0) {
        TRACE_SYSERR( "flock(LOCK_EX) failed on fd %d", storage->fd);
        pthread_mutex_unlock( &storage->lock);
        return(KES_ERROR_IO);
    }

    /* KES-5: refresh this handle's view of the descriptor/bitmap from
     * disk before touching either. Without this reload, the lock
     * above only prevents two operations from literally overlapping
     * -- it does NOT stop this handle from allocating against a
     * bitmap it loaded minutes ago, before some other handle's own
     * (already-persisted) allocation happened. See kes_5_plan.md's
     * "Why locking alone is not enough" section for the full
     * reasoning; do not remove this reload as an "optimization". */
    int result = load_storage_descriptor( storage);
    if ( result == KES_SUCCESS) {
        off_t bitmap_offset = storage->desc.bitmap_start_block *
                               storage->desc.block_size;
        result = kes_bitmap_load( storage->bitmap, storage->fd,
                                   bitmap_offset);
    }

    /* Use allocation strategy */
    if ( result == KES_SUCCESS) {
        switch ( storage->strategy) {
            case KES_ALLOC_FIRST_FIT:
            default:
                result = allocate_extent_first_fit( storage, request,
                                                     extent);
                break;
        }
    }

    if ( result == KES_SUCCESS) {
        /* Update statistics */
        storage->stats.allocated_extents++;
        storage->desc.used_blocks += request->block_count;
        storage->desc.free_blocks -= request->block_count;

        /* Assign unique extent ID */
        extent->extent_id = ++storage->desc.next_extent_id;
    }

    /* KES-5: persist immediately so the next handle to take the lock
     * (in this process or another) sees this allocation, rather than
     * relying on an eventual, possibly-never-called
     * kes_storage_sync()/kes_storage_close(). This also closes the
     * separate "no-sync reopen forgets a crashed allocation"
     * durability gap documented in PENDING_ITEMS.md's A.5.1 finding,
     * as a side effect -- every mutating call is now durable on its
     * own. */
    if ( result == KES_SUCCESS) {
        off_t bitmap_offset = storage->desc.bitmap_start_block *
                               storage->desc.block_size;
        result = kes_bitmap_save( storage->bitmap, storage->fd,
                                   bitmap_offset);
        /* IF NOT APPLYING KES-6: delete the next 6 lines (the
         * kes_bitmap_checksum()/storage->desc.bitmap_checksum block)
         * and call save_storage_descriptor(storage) directly instead,
         * guarded the same way (if result == KES_SUCCESS). */
        if ( result == KES_SUCCESS) {
            uint32_t checksum;
            result = kes_bitmap_checksum( storage->bitmap, &checksum);
            if ( result == KES_SUCCESS) {
                storage->desc.bitmap_checksum = checksum;
                result = save_storage_descriptor( storage);
            }
        }
        if ( result == KES_SUCCESS && fsync( storage->fd) != 0) {
            TRACE_SYSERR( "fsync failed on fd %d after extent "
                          "allocate", storage->fd);
            result = KES_ERROR_IO;
        }
    }

    flock( storage->fd, LOCK_UN);
    pthread_mutex_unlock( &storage->lock);

    return(result);
}
```

Every code path from the `flock()` acquisition onward funnels through
to the single `flock(...LOCK_UN)` / `pthread_mutex_unlock(...)` /
`return(result);` at the end — there is no early `return` inside the
locked section. Preserve that shape; do not add an early `return`
anywhere between the `flock()` call and the final unlock without also
adding the matching `flock(storage->fd, LOCK_UN);` right before it (as
already done for the two pre-lock validation checks at the top, which
correctly return before ever taking the lock).

---

## Step 4 — rewrite `kes_extent_free()`

This one needs more care than allocate: the existing double-free
validation loop must run against the *freshly reloaded* bitmap, and it
already has its own early-return path that must also release the new
lock. Replace the entire function body with:

```c
int kes_extent_free( kes_storage_t *storage,
                      const kes_extent_descriptor_t *extent) {
    if ( storage == NULL || extent == NULL || storage->readonly) {
        return(KES_ERROR_INVALID);
    }

    pthread_mutex_lock( &storage->lock);

    /* KES-5: see kes_extent_allocate() for why both the file lock and
     * the reload below are required together, not just one or the
     * other. */
    if ( flock( storage->fd, LOCK_EX) != 0) {
        TRACE_SYSERR( "flock(LOCK_EX) failed on fd %d", storage->fd);
        pthread_mutex_unlock( &storage->lock);
        return(KES_ERROR_IO);
    }

    int result = load_storage_descriptor( storage);
    if ( result == KES_SUCCESS) {
        off_t bitmap_offset = storage->desc.bitmap_start_block *
                               storage->desc.block_size;
        result = kes_bitmap_load( storage->bitmap, storage->fd,
                                   bitmap_offset);
    }

    if ( result != KES_SUCCESS) {
        flock( storage->fd, LOCK_UN);
        pthread_mutex_unlock( &storage->lock);
        return(result);
    }

    /* Verify every block in the range is currently allocated before
     * changing anything. kes_bitmap_clear_range() is idempotent (a
     * no-op on already-clear bits), so without this check a
     * double-free would silently desync desc.used_blocks/
     * free_blocks/stats.allocated_extents from the bitmap's actual
     * state instead of being rejected. This now runs against the
     * just-reloaded, cross-process-fresh bitmap above, not whatever
     * this handle last happened to have in memory -- reordered ahead
     * of the reload deliberately; do not move it back above the
     * reload. */
    for ( uint32_t i = 0; i < extent->block_count; i++) {
        if ( !kes_bitmap_test( storage->bitmap,
                                extent->start_block + i)) {
            TRACE_ERR( "double-free or invalid extent: block %llu "
                       "(of %u) in range starting at %llu is not "
                       "currently allocated",
                       (unsigned long long)(extent->start_block + i),
                       extent->block_count,
                       (unsigned long long)extent->start_block);
            flock( storage->fd, LOCK_UN);
            pthread_mutex_unlock( &storage->lock);
            return(KES_ERROR_NOTFOUND);
        }
    }

    /* Clear bits in bitmap */
    result = kes_bitmap_clear_range( storage->bitmap,
                                      extent->start_block,
                                      extent->block_count);

    if ( result == KES_SUCCESS) {
        /* Update statistics */
        storage->desc.used_blocks -= extent->block_count;
        storage->desc.free_blocks += extent->block_count;
        storage->stats.allocated_extents--;
    }

    /* KES-5: persist immediately, same reasoning as
     * kes_extent_allocate(). */
    if ( result == KES_SUCCESS) {
        off_t bitmap_offset = storage->desc.bitmap_start_block *
                               storage->desc.block_size;
        result = kes_bitmap_save( storage->bitmap, storage->fd,
                                   bitmap_offset);
        /* IF NOT APPLYING KES-6: delete the next 6 lines, same as in
         * kes_extent_allocate() above. */
        if ( result == KES_SUCCESS) {
            uint32_t checksum;
            result = kes_bitmap_checksum( storage->bitmap, &checksum);
            if ( result == KES_SUCCESS) {
                storage->desc.bitmap_checksum = checksum;
                result = save_storage_descriptor( storage);
            }
        }
        if ( result == KES_SUCCESS && fsync( storage->fd) != 0) {
            TRACE_SYSERR( "fsync failed on fd %d after extent free",
                          storage->fd);
            result = KES_ERROR_IO;
        }
    }

    flock( storage->fd, LOCK_UN);
    pthread_mutex_unlock( &storage->lock);

    return(result);
}
```

Count the `flock(storage->fd, LOCK_UN);` calls in your finished
function: there must be exactly **three** — one after the reload
failure check, one inside the double-free-detected branch, and one at
the very end. Missing any of them leaves the file locked forever for
that error path (the next `kes_extent_allocate()`/`kes_extent_free()`
call — from any handle, including this same one — will hang on
`flock(LOCK_EX)` until the process holding it exits).

---

## Step 5 — rewrite `kes_storage_sync()` and `kes_storage_close()`

### 5a. `kes_storage_sync()`

Replace the entire function body with:

```c
int kes_storage_sync( kes_storage_t *storage) {
    if ( storage == NULL || storage->readonly) {
        return(KES_ERROR_INVALID);
    }

    pthread_mutex_lock( &storage->lock);

    if ( flock( storage->fd, LOCK_EX) != 0) {
        TRACE_SYSERR( "flock(LOCK_EX) failed on fd %d", storage->fd);
        pthread_mutex_unlock( &storage->lock);
        return(KES_ERROR_IO);
    }

    /* KES-5: refresh from disk first so a sync from a handle with no
     * pending local changes of its own (e.g. one that only called
     * kes_extent_write(), which does not touch the bitmap) cannot
     * clobber a fresher on-disk bitmap/descriptor written by another
     * handle's already-persisted allocate()/free() in the meantime.
     * Because kes_extent_allocate()/kes_extent_free() now persist
     * immediately on their own, this reload makes the save below a
     * same-data round-trip in the common case -- harmless, and the
     * safe default regardless of what mutates storage->desc in the
     * future. */
    int result = load_storage_descriptor( storage);
    if ( result == KES_SUCCESS) {
        off_t bitmap_offset = storage->desc.bitmap_start_block *
                               storage->desc.block_size;
        result = kes_bitmap_load( storage->bitmap, storage->fd,
                                   bitmap_offset);
    }

    /* Save bitmap */
    if ( result == KES_SUCCESS) {
        off_t bitmap_offset = storage->desc.bitmap_start_block *
                               storage->desc.block_size;
        result = kes_bitmap_save( storage->bitmap, storage->fd,
                                   bitmap_offset);
    }

    /* IF NOT APPLYING KES-6: delete the next 6 lines (this block and
     * the "Save descriptor" block below stay separate either way --
     * only this checksum block is KES-6-specific). */
    if ( result == KES_SUCCESS) {
        uint32_t checksum;
        result = kes_bitmap_checksum( storage->bitmap, &checksum);
        if ( result == KES_SUCCESS) {
            storage->desc.bitmap_checksum = checksum;
        }
    }

    if ( result == KES_SUCCESS) {
        /* Save descriptor */
        result = save_storage_descriptor( storage);
    }

    if ( result == KES_SUCCESS) {
        /* Force sync to disk */
        if ( fsync( storage->fd) != 0) {
            TRACE_SYSERR( "fsync failed on fd %d during "
                          "kes_storage_sync", storage->fd);
            result = KES_ERROR_IO;
        }
    }

    flock( storage->fd, LOCK_UN);
    pthread_mutex_unlock( &storage->lock);

    return(result);
}
```

### 5b. `kes_storage_close()`

Replace the `if ( !storage->readonly) { ... }` block (only that block
— the mutex lock/unlock and the resource cleanup below it are
unchanged) with:

```c
    /* Save bitmap before closing */
    if ( !storage->readonly) {
        if ( flock( storage->fd, LOCK_EX) == 0) {
            /* KES-5: same reload-before-writeback reasoning as
             * kes_storage_sync() -- don't let a stale in-memory view
             * clobber a fresher on-disk state at close time either. */
            if ( load_storage_descriptor( storage) == KES_SUCCESS) {
                off_t reload_offset =
                    storage->desc.bitmap_start_block *
                    storage->desc.block_size;
                kes_bitmap_load( storage->bitmap, storage->fd,
                                 reload_offset);
            }

            off_t bitmap_offset = storage->desc.bitmap_start_block *
                                   storage->desc.block_size;
            kes_bitmap_save( storage->bitmap, storage->fd,
                             bitmap_offset);

            /* IF NOT APPLYING KES-6: delete this checksum block. */
            uint32_t checksum;
            if ( kes_bitmap_checksum( storage->bitmap, &checksum) ==
                KES_SUCCESS) {
                storage->desc.bitmap_checksum = checksum;
            }

            /* Save descriptor with updated statistics */
            save_storage_descriptor( storage);

            /* Sync file system */
            fsync( storage->fd);

            flock( storage->fd, LOCK_UN);
        } else {
            TRACE_SYSERR( "flock(LOCK_EX) failed on fd %d during "
                          "kes_storage_close -- closing without a "
                          "final sync", storage->fd);
        }
    }
```

This preserves `kes_storage_close()`'s existing contract exactly:
it already ignores the return values of `kes_bitmap_save()`/
`save_storage_descriptor()`/`fsync()` and always returns `KES_SUCCESS`
once it reaches cleanup — that pre-existing looseness is not this
plan's concern to fix, so no new error propagation is added here,
only the lock/reload/checksum steps.

---

## Step 6 — Test changes

### 6a. `tests/test_kes_crash_consistency.c`: `test_no_sync_reopen_durability`

Both cases' premises change. Read the reasoning in "Why locking alone
is not enough" above again before editing this — the new expected
behavior is a *direct, intended consequence* of that fix, not a bug to
work around.

**Case 1** (crash before any sync ever happened) — currently expects
`kes_storage_open()` to fail with `KES_ERROR_IO` (because the file was
never fully laid out). After Step 2 above, `kes_storage_create()`
always writes the full bitmap immediately, so the file IS fully laid
out the moment `create()` returns. Replace:

```c
    TEST_ASSERT( kes_storage_open( TEST_FILE, 0, &st) ==
                    KES_ERROR_IO,
                "OBSERVED: a storage file that has never been synced/"
                "closed even once is UNOPENABLE after a crash -- "
                "kes_bitmap_load() fails reading a bitmap region that "
                "was never physically written to the file (see "
                "comment above); not the KES_ERROR_CORRUPT one might "
                "assume");
    TEST_ASSERT( st == NULL,
                "*storage was not left pointing at a partially-"
                "initialized handle on this failure");
    cleanup();
```

with:

```c
    TEST_ASSERT( kes_storage_open( TEST_FILE, 0, &st) == KES_SUCCESS,
                "FIXED (KES-5): kes_storage_create() now writes the "
                "full bitmap immediately, so a crash before any "
                "explicit sync no longer leaves the file too short "
                "to reopen");
    kes_storage_stats_t stats_case1;
    TEST_ASSERT( kes_storage_get_stats( st, &stats_case1) ==
                    KES_SUCCESS,
                "get_stats on the reopened, never-allocated storage");
    TEST_ASSERT( stats_case1.used_blocks == 0,
                "a storage that crashed before any allocation reopens "
                "with nothing allocated, as expected");
    kes_storage_close( st);
    st = NULL;
    cleanup();
```

**Case 2** (one clean close, then allocate + write + crash without an
explicit sync) — currently expects the allocation to be "forgotten"
(stats revert, and a fresh allocation hinted at the same block is
handed the exact same still-live block, corrupting it). After Step 3,
`kes_extent_allocate()` persists itself immediately — there is no
unpersisted state left to lose when `crash_close()` runs afterward, so
none of that happens anymore. Replace everything from
`TEST_ASSERT( kes_storage_get_stats( st, &stats_after_crash) ==` down
through the end of the function (before `TEST_SUCCESS(...)`) — i.e.
the entire "characterize what happened" section including the
reallocation-hazard demonstration — with:

```c
    TEST_ASSERT( kes_storage_get_stats( st, &stats_after_crash) ==
                    KES_SUCCESS,
                "get_stats after reopen");
    TEST_ASSERT( stats_after_crash.free_blocks ==
                    stats_before.free_blocks - 2,
                "FIXED (KES-5): the allocation is NOT forgotten -- "
                "kes_extent_allocate() persisted it immediately, so "
                "the crash afterward lost nothing");
    TEST_ASSERT( stats_after_crash.used_blocks ==
                    stats_before.used_blocks + 2,
                "used_blocks correctly reflects the allocation that "
                "survived the crash");

    /* The data is still physically present, as before. */
    memset( read_buf, 0, sizeof(read_buf));
    TEST_ASSERT( kes_extent_read( st, &ext, read_buf,
                                  strlen( original_data) + 1, 0) ==
                    KES_SUCCESS,
                "read via the remembered (pre-crash) extent "
                "descriptor still succeeds");
    TEST_ASSERT( strcmp( read_buf, original_data) == 0,
                "the previously-written data is still physically "
                "present and readable");

    /* FIXED (KES-5): a fresh allocation hinted at the same block is
     * NOT handed the same, still-live block back -- the bitmap
     * correctly still marks it used, because the reopen above
     * reloaded the real, persisted state. */
    kes_extent_descriptor_t realloc_ext;
    kes_extent_request_t realloc_req = { .block_count = 2,
                                          .alignment = 0,
                                          .hint_block = ext.start_block,
                                          .flags = 0 };
    TEST_ASSERT( kes_extent_allocate( st, &realloc_req,
                                      &realloc_ext) == KES_SUCCESS,
                "a fresh allocation after reopen still succeeds "
                "(there is free space elsewhere)");
    TEST_ASSERT( realloc_ext.start_block != ext.start_block,
                "FIXED (KES-5): the fresh allocation is NOT handed "
                "the original extent's still-live start_block -- the "
                "bitmap correctly knows those blocks are still used");

    /* Confirm the original data was NOT touched by the new
     * allocation. */
    memset( read_buf, 0, sizeof(read_buf));
    TEST_ASSERT( kes_extent_read( st, &ext, read_buf,
                                  strlen( original_data) + 1, 0) ==
                    KES_SUCCESS,
                "read again via the original extent descriptor");
    TEST_ASSERT( strcmp( read_buf, original_data) == 0,
                "FIXED (KES-5): the original extent's data is "
                "UNCHANGED -- no double-allocation occurred");

    kes_storage_close( st);
    cleanup();
    TEST_SUCCESS( "no-explicit-sync reopen: FIXED by KES-5 -- every "
                  "kes_extent_allocate()/kes_extent_free() call is "
                  "now durable on its own, so a crash between an "
                  "allocation and an explicit sync no longer loses "
                  "bookkeeping or causes double-allocation");
```

Note this drops the `new_data`/`overwrite_data` variables the old test
used for its "confirmed corruption" step, since there is no longer any
corruption to confirm — if the compiler warns about now-unused local
variables (`new_data`), remove their declarations too. Also update the
large block comment above `test_no_sync_reopen_durability()`
(originally documenting "TWO DISTINCT OBSERVED BEHAVIORS" as an open
gap) to describe the fix instead, following this repo's own
`PENDING_ITEMS.md` "Resolved" convention: what was true, what changed
it, why.

### 6b. `tests/test_kes_multiprocess.c`: `test_cross_process_racing_io`

Currently this test deliberately does not assert correctness — its
whole doc comment explains it exists only to prove the race doesn't
crash. After this fix, the race should produce *correct* results, so
upgrade the final verification block (the "third, non-racing reopen"
at the end of the function) from print-only observations to real
assertions. Replace:

```c
    kes_storage_t *final_storage = NULL;
    if ( kes_storage_open( RACE_TEST_FILE, KES_STORAGE_READONLY,
                           &final_storage) == KES_SUCCESS) {
        kes_storage_stats_t final_stats;
        int combined_allocs = shared->parent.successful_allocs +
                               shared->child.successful_allocs;

        if ( kes_storage_get_stats( final_storage,
                                    &final_stats) == KES_SUCCESS) {
            printf( "  [known limitation] real on-disk state after "
                    "both processes exited: used_blocks=%llu "
                    "free_blocks=%llu, vs. %d combined allocations "
                    "the two processes each believed succeeded -- "
                    "any shortfall is allocations one side's "
                    "kes_storage_sync() silently clobbered.\n",
                    (unsigned long long)final_stats.used_blocks,
                    (unsigned long long)final_stats.free_blocks,
                    combined_allocs);
        }
        kes_storage_close( final_storage);
    } else {
        printf( "  [known limitation] final read-only re-open of the "
                "raced storage file failed -- see AGENTS.md/"
                "PENDING_ITEMS.md's unsynchronized-access gap.\n");
    }
```

with:

```c
    kes_storage_t *final_storage = NULL;
    TEST_ASSERT( kes_storage_open( RACE_TEST_FILE, KES_STORAGE_READONLY,
                                   &final_storage) == KES_SUCCESS,
                "FIXED (KES-5): final read-only reopen succeeds");

    kes_storage_stats_t final_stats;
    int combined_allocs = shared->parent.successful_allocs +
                           shared->child.successful_allocs;
    TEST_ASSERT( kes_storage_get_stats( final_storage,
                                        &final_stats) == KES_SUCCESS,
                "get_stats on the final reopen");
    printf( "\n  parent: allocs=%d | child: allocs=%d | combined=%d "
            "| final used_blocks=%llu\n",
            shared->parent.successful_allocs,
            shared->child.successful_allocs, combined_allocs,
            (unsigned long long)final_stats.used_blocks);
    TEST_ASSERT( (int)final_stats.used_blocks == combined_allocs *
                    ( RACE_OP_COUNT > 0 ? 1 : 1),
                "placeholder -- replace with the exact per-op block "
                "count used by run_racing_side() (see below)");

    kes_storage_close( final_storage);
```

**You must fill in the exact accounting yourself** — read
`run_racing_side()` (above `test_cross_process_racing_io()` in the
same file) to find the block count each racing allocation actually
requests, and replace the placeholder assertion above with a real one
of the shape:

```c
    TEST_ASSERT( final_stats.used_blocks ==
                    (uint64_t)combined_allocs * BLOCKS_PER_RACE_ALLOC,
                "FIXED (KES-5): real on-disk used_blocks now exactly "
                "matches the sum of both sides' successful "
                "allocations -- no allocation was silently clobbered");
```

(substitute the real constant/expression for
`BLOCKS_PER_RACE_ALLOC`). Also delete the two now-obsolete
"[known limitation]" `printf()` blocks earlier in the function (the
one comparing `shared->parent.final_free_blocks` against
`shared->child.final_used_blocks` and the one checking
`final_magic != KES_MAGIC_NUMBER`) — those described expected
divergence that should no longer happen; if you want to keep them as
assertions instead of deleting them outright, invert them into
`TEST_ASSERT`s that the two sides' final in-memory views match
(acceptable, since after this fix both are reloading fresh state on
every operation) rather than leaving them as non-asserting `printf`s.

Rewrite the function's large doc comment (currently explaining "This
is intentional and does not test correctness") the same way as
`test_no_sync_reopen_durability`'s comment above — describe what was
true and what fixed it, per `PENDING_ITEMS.md`'s "Resolved" convention.

Expect this test to take noticeably longer to run than before (each
racing allocation now does a full reload + save + fsync under an
exclusive file lock instead of a pure in-memory mutation) — this is
expected and fine, not a bug; see Step 7 below.

### 6c. Other tests — same instruction as `kes_6_plan.md`

Run `make test` after every step above, not just at the end. For any
other test that starts failing, determine whether the assertion
encodes old, now-intentionally-changed behavior or is a genuine
regression, and act accordingly — do not blindly patch code to make an
old assertion pass again.
`tests/test_kes_storage_edge.c`'s `test_exhaustion_then_free_and_reallocate`
was checked during this plan's own research: it uses a single handle
throughout with no external writer touching the file concurrently, so
its reload-before-mutate steps should just read back exactly what that
same handle last wrote — expected to be unaffected, but verify by
running it, not by trusting this note.

---

## Step 7 — `README.md`: document the performance trade-off

Add a short note near this repo's build/usage documentation: every
`kes_extent_allocate()`/`kes_extent_free()` call now does a full
reload of the descriptor and bitmap from disk, a full write-back, and
a synchronous `fsync()`, all under an exclusive file lock — turning
what used to be a pure in-memory operation into a full durable disk
operation on every call. This is a deliberate, necessary consequence
of fixing the cross-process race correctly (see "Why locking alone is
not enough" above), not an oversight. It changes the throughput
profile for allocate/free-heavy workloads significantly. Per
`PENDING_FIXES_SEP2026.md`'s KES-11 (performance smoke tests,
deferred), this trade-off is accepted for now; if either downstream
project needs higher allocate/free throughput later, reducing how
often the fsync/reload happens is a real, separate follow-up design
question — explicitly out of scope for this plan.

---

## Verification checklist (run every one, paste the actual output —
do not assume, per `AGENTS.md`'s standing rule about pasted evidence)

1. `make clean && make test` — all tests pass, including the rewritten
   `test_no_sync_reopen_durability` and `test_cross_process_racing_io`
   with their NEW assertions (not the old ones).
2. `make asan` — clean. Pay attention here: the reload-before-mutate
   logic reuses `storage->bitmap`'s already-allocated buffer
   (`kes_bitmap_load()` reads into `bitmap->data` in place, no
   realloc), so there should be no new allocation-related findings,
   but confirm it.
3. `make tsan` — clean. This is the most important sanitizer run for
   this specific change: it is exactly what would catch a
   forgotten-unlock path (Step 4's "exactly three `flock` unlocks"
   warning) manifesting as a hang, and any residual in-process race
   the new code introduces around `storage->bitmap`/`storage->desc`.
4. `make valgrind` — clean.
5. `make check-all` — clean, "ALL CHECKS PASSED".
6. `make stress SANITIZER=tsan STRESS_RUNS=30` (existing target,
   `Makefile`) — repeats `test_kes_cache`/`test_kes_multiprocess`
   under TSan 30 times each. This is the closest existing tool to a
   real stress test of the new locking; a single clean `make tsan` run
   is not strong enough evidence on its own for a fix whose entire
   point is eliminating a race that was already known to be
   intermittent.
7. Manually re-run the exact scenario `test_cross_process_racing_io`
   automates, a few times in a row outside the test harness if
   possible (e.g. loop the test binary 20+ times), watching for any
   `flock()`-related hang (a sign of a missing unlock on some path)
   rather than just a pass/fail count — a hang would make the test
   binary itself time out or need to be killed, not print a clean
   `FAIL`.

## Explicitly out of scope for this plan

- `kes_extent_read()`/`kes_extent_write()` are unchanged. They
  operate on raw block data at a fixed byte offset derived from
  `extent->start_block` and never touch the shared bitmap/descriptor
  state that this plan protects. Two callers writing to the *same*
  extent concurrently is an application-level data race no different
  from any other raw file access, and is not part of what KES-5
  describes or what `test_cross_process_racing_io` demonstrates —
  adding locking there would be a separate, broader design question
  about single-extent write serialization.
- `kes_storage_get_stats()`/`kes_storage_get_descriptor()` still
  return a snapshot of whatever this handle last reloaded (at the most
  recent allocate/free/sync/open) and can be stale relative to another
  process's more recent, already-persisted change. A caller that wants
  a guaranteed-fresh cross-process view should call
  `kes_storage_sync()` first (which now always reloads) before reading
  stats. Making every stats call itself reload from disk was
  considered and rejected as excessive for this plan's scope.
- Concurrent `kes_storage_create()` calls racing to create/truncate
  the same not-yet-existing file (see Step 2's note).
- Any change to `kes_cache.c`/`kes_cache.h` — the cache layer does not
  depend on `kes_storage.c` (see `AGENTS.md`'s module layering) and is
  unaffected by anything in this plan.
- A configurable/non-blocking variant of the `flock()` acquisition
  (e.g. a timeout, or a `KES_ERROR_BUSY` return on contention instead
  of blocking). Blocking indefinitely is the standard idiom for this
  kind of advisory lock (the same approach SQLite and dpkg use for
  their own lock files) and is accepted as correct default behavior
  for this fix.

## Bookkeeping when done

Same convention as `kes_6_plan.md`'s final section: move
`PENDING_FIXES_SEP2026.md`'s `## KES-5` status to `CLOSED` with the
commit hash, and add a matching evidence-first entry to
`PENDING_ITEMS.md`'s `## Resolved` section, following the exact shape
already used there for KES-1 and the other closed items.
