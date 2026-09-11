#include <kes/kes_storage.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>
#include "trace.h"

/* Internal helper functions */
static int validate_config( const kes_storage_config_t *config);
static int init_storage_descriptor( kes_storage_t *storage, uint64_t size);
static int load_storage_descriptor( kes_storage_t *storage);
static int save_storage_descriptor( kes_storage_t *storage);
static uint64_t calculate_bitmap_blocks( uint64_t total_blocks,
                                          uint32_t block_size);
static int allocate_extent_first_fit( kes_storage_t *storage,
                                      const kes_extent_request_t *request,
                                      kes_extent_descriptor_t *extent);

/* Error string table */
static const char *error_strings[] = {
    "Success",                          /* KES_SUCCESS */
    "Invalid parameters",               /* KES_ERROR_INVALID */
    "Out of memory",                   /* KES_ERROR_NOMEM */
    "Resource not found",              /* KES_ERROR_NOTFOUND */
    "Resource already exists",         /* KES_ERROR_EXISTS */
    "I/O error",                       /* KES_ERROR_IO */
    "No space available",              /* KES_ERROR_NOSPACE */
    "Data corruption detected",        /* KES_ERROR_CORRUPT */
    "Resource busy"                    /* KES_ERROR_BUSY */
};

/* =================================================================
 * Storage Lifecycle Implementation
 * ================================================================= */

int kes_storage_create( const kes_storage_config_t *config,
                         kes_storage_t **storage) {
    if ( config == NULL || storage == NULL) {
        return(KES_ERROR_INVALID);
    }

    int result = validate_config( config);
    if ( result != KES_SUCCESS) {
        return(result);
    }

    /* Allocate storage structure */
    kes_storage_t *sto = calloc( 1, sizeof(kes_storage_t));
    if ( sto == NULL) {
        return(KES_ERROR_NOMEM);
    }

    /* Initialize mutex */
    if ( pthread_mutex_init( &sto->lock, NULL) != 0) {
        free( sto);
        return(KES_ERROR_NOMEM);
    }

    /* Open/create the storage device/file */
    int flags = O_RDWR;
    if ( config->flags & KES_STORAGE_CREATE) {
        flags |= O_CREAT;
    }
    if ( config->flags & KES_STORAGE_TRUNCATE) {
        flags |= O_TRUNC;
    }
    if ( config->flags & KES_STORAGE_SYNC) {
        flags |= O_SYNC;
    }

    sto->fd = open( config->device_path, flags, 0644);
    if ( sto->fd < 0) {
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(KES_ERROR_IO);
    }

    /* Set storage configuration */
    sto->strategy = config->strategy;
    sto->readonly = (config->flags & KES_STORAGE_READONLY) != 0;
    sto->sync_writes = (config->flags & KES_STORAGE_SYNC) != 0;

    /* Initialize storage layout */
    result = init_storage_descriptor( sto, config->device_size);
    if ( result != KES_SUCCESS) {
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }

    /* Create bitmap */
    result = kes_bitmap_create( sto->desc.user_blocks, &sto->bitmap);
    if ( result != KES_SUCCESS) {
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }

    /* KES-6: record the checksum of the freshly-created (all-zero)
     * in-memory bitmap into the descriptor now, so the descriptor
     * this function is about to write is self-consistent from the
     * start rather than carrying a stale/zero checksum until the
     * first real sync. Note this does NOT itself write the bitmap
     * bytes to disk -- kes_storage_create() still only writes block 0
     * (the descriptor) here, same as before this change; see
     * kes_5_plan.md if that is also being applied, since KES-5 adds
     * that missing bitmap write for an unrelated reason (making the
     * file safe to reload-from-disk immediately after create()). The
     * checksum set here stays correct either way, since it is always
     * computed from the in-memory bitmap, not from whatever has
     * physically reached disk. */
    uint32_t initial_checksum;
    result = kes_bitmap_checksum( sto->bitmap, &initial_checksum);
    if ( result != KES_SUCCESS) {
        kes_bitmap_destroy( sto->bitmap);
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }
    sto->desc.bitmap_checksum = initial_checksum;

    /* KES-5: physically write the freshly-created (all-zero) bitmap
     * to disk now, not just its checksum into the descriptor. Without
     * this, the file is not yet fully laid out on disk, so the very
     * first kes_extent_allocate()/kes_extent_free() call's mandatory
     * reload-from-disk step (see those functions below) would try to
     * read a bitmap region that was never written and fail with
     * KES_ERROR_IO on what should be a normal, successful first
     * allocation right after create(). This also happens to close
     * most of the separate, previously-documented "storage file
     * unopenable after a crash before any sync" gap (PENDING_ITEMS.md
     * A.5.1 Case 1) as a side effect, though that is not this change's
     * goal -- see test_no_sync_reopen_durability's updated Case 1. */
    off_t initial_bitmap_offset = sto->desc.bitmap_start_block *
                                   sto->desc.block_size;
    result = kes_bitmap_save( sto->bitmap, sto->fd,
                              initial_bitmap_offset);
    if ( result != KES_SUCCESS) {
        kes_bitmap_destroy( sto->bitmap);
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }

    /* Save initial descriptor to storage */
    result = save_storage_descriptor( sto);
    if ( result != KES_SUCCESS) {
        kes_bitmap_destroy( sto->bitmap);
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }

    *storage = sto;
    return(KES_SUCCESS);
}

int kes_storage_open( const char *device_path, uint32_t flags,
                       kes_storage_t **storage) {
    if ( device_path == NULL || storage == NULL) {
        return(KES_ERROR_INVALID);
    }

    /* Allocate storage structure */
    kes_storage_t *sto = calloc( 1, sizeof(kes_storage_t));
    if ( sto == NULL) {
        return(KES_ERROR_NOMEM);
    }

    /* Initialize mutex */
    if ( pthread_mutex_init( &sto->lock, NULL) != 0) {
        free( sto);
        return(KES_ERROR_NOMEM);
    }

    /* Open the storage device/file */
    int open_flags = (flags & KES_STORAGE_READONLY) ? O_RDONLY : O_RDWR;
    if ( flags & KES_STORAGE_SYNC) {
        open_flags |= O_SYNC;
    }

    sto->fd = open( device_path, open_flags);
    if ( sto->fd < 0) {
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(KES_ERROR_IO);
    }

    /* Set storage configuration */
    sto->readonly = (flags & KES_STORAGE_READONLY) != 0;
    sto->sync_writes = (flags & KES_STORAGE_SYNC) != 0;
    sto->strategy = KES_ALLOC_FIRST_FIT;  /* Default strategy */

    /* KES-5: take a SHARED file lock around the whole read sequence
     * below (descriptor, then bitmap, then the KES-6 checksum check).
     * kes_extent_allocate()/kes_extent_free()/kes_storage_sync()/
     * kes_storage_close() each hold an EXCLUSIVE lock while writing
     * the bitmap region and the descriptor block as two SEPARATE
     * write() calls -- without a lock here, a concurrent open() could
     * read the new bitmap but the not-yet-updated (old-checksum)
     * descriptor, a torn combination that spuriously fails the
     * checksum check below even though neither write was itself
     * corrupt. A shared lock blocks only while an exclusive writer is
     * mid-critical-section, but never conflicts with another reader's
     * concurrent open(). */
    if ( flock( sto->fd, LOCK_SH) != 0) {
        TRACE_SYSERR( "flock(LOCK_SH) failed on fd %d", sto->fd);
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(KES_ERROR_IO);
    }

    /* Load storage descriptor */
    int result = load_storage_descriptor( sto);
    if ( result != KES_SUCCESS) {
        flock( sto->fd, LOCK_UN);
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }

    /* Create and load bitmap */
    result = kes_bitmap_create( sto->desc.user_blocks, &sto->bitmap);
    if ( result != KES_SUCCESS) {
        flock( sto->fd, LOCK_UN);
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }

    /* Load bitmap from storage */
    off_t bitmap_offset = sto->desc.bitmap_start_block *
                           sto->desc.block_size;
    result = kes_bitmap_load( sto->bitmap, sto->fd, bitmap_offset);
    if ( result != KES_SUCCESS) {
        kes_bitmap_destroy( sto->bitmap);
        flock( sto->fd, LOCK_UN);
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }

    /* KES-6: verify the loaded bitmap against the checksum recorded
     * in the descriptor at the last successful save, catching silent
     * on-disk bit flips that kes_bitmap_load()'s own free-bit recount
     * cannot (a flip that preserves the total population count -- one
     * bit 1->0, another 0->1 -- is invisible to that recount alone,
     * but not to a real checksum). */
    uint32_t computed_checksum;
    result = kes_bitmap_checksum( sto->bitmap, &computed_checksum);
    if ( result == KES_SUCCESS &&
        computed_checksum != sto->desc.bitmap_checksum) {
        TRACE_ERR( "bitmap checksum mismatch: on-disk 0x%08x, "
                   "computed 0x%08x -- refusing to open corrupted "
                   "storage %s",
                   sto->desc.bitmap_checksum, computed_checksum,
                   device_path);
        result = KES_ERROR_CORRUPT;
    }
    if ( result != KES_SUCCESS) {
        kes_bitmap_destroy( sto->bitmap);
        flock( sto->fd, LOCK_UN);
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }

    flock( sto->fd, LOCK_UN);

    *storage = sto;
    return(KES_SUCCESS);
}

int kes_storage_close( kes_storage_t *storage) {
    if ( storage == NULL) {
        return(KES_ERROR_INVALID);
    }

    pthread_mutex_lock( &storage->lock);

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

            /* KES-6: refresh the descriptor's bitmap checksum from
             * what was just written, same reasoning as
             * kes_storage_sync(). */
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

    /* Cleanup resources */
    kes_bitmap_destroy( storage->bitmap);
    close( storage->fd);

    pthread_mutex_unlock( &storage->lock);
    pthread_mutex_destroy( &storage->lock);
    free( storage);

    return(KES_SUCCESS);
}

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

    /* KES-6: refresh the descriptor's checksum from the bitmap bytes
     * that were just written, before persisting the descriptor itself
     * -- otherwise the on-disk checksum would keep describing whatever
     * bitmap contents were current at the LAST sync, not this one. */
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

/* =================================================================
 * Extent Management Implementation
 * ================================================================= */

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

int kes_extent_read( kes_storage_t *storage,
                      const kes_extent_descriptor_t *extent,
                      void *buffer, size_t size, uint64_t offset) {
    if ( storage == NULL || extent == NULL || buffer == NULL) {
        return(KES_ERROR_INVALID);
    }

    /* Calculate absolute file offset */
    uint64_t extent_start_byte = (storage->desc.user_start_block +
                                  extent->start_block) *
                                 storage->desc.block_size;
    off_t file_offset = extent_start_byte + offset;

    /* Validate bounds */
    uint64_t extent_size = (uint64_t)extent->block_count *
                            storage->desc.block_size;
    if ( offset + size > extent_size) {
        return(KES_ERROR_INVALID);
    }

    pthread_mutex_lock( &storage->lock);

    /* Seek to position */
    if ( lseek( storage->fd, file_offset, SEEK_SET) != file_offset) {
        pthread_mutex_unlock( &storage->lock);
        return(KES_ERROR_IO);
    }

    /* Read data */
    ssize_t bytes_read = read( storage->fd, buffer, size);
    if ( bytes_read != (ssize_t)size) {
        pthread_mutex_unlock( &storage->lock);
        return(KES_ERROR_IO);
    }

    /* Update statistics */
    storage->stats.reads_completed++;
    storage->stats.bytes_read += size;

    pthread_mutex_unlock( &storage->lock);

    return(KES_SUCCESS);
}

int kes_extent_write( kes_storage_t *storage,
                       const kes_extent_descriptor_t *extent,
                       const void *buffer, size_t size, uint64_t offset) {
    if ( storage == NULL || extent == NULL || buffer == NULL ||
        storage->readonly) {
        return(KES_ERROR_INVALID);
    }

    /* Calculate absolute file offset */
    uint64_t extent_start_byte = (storage->desc.user_start_block +
                                  extent->start_block) *
                                 storage->desc.block_size;
    off_t file_offset = extent_start_byte + offset;

    /* Validate bounds */
    uint64_t extent_size = (uint64_t)extent->block_count *
                            storage->desc.block_size;
    if ( offset + size > extent_size) {
        return(KES_ERROR_INVALID);
    }

    pthread_mutex_lock( &storage->lock);

    /* Seek to position */
    if ( lseek( storage->fd, file_offset, SEEK_SET) != file_offset) {
        pthread_mutex_unlock( &storage->lock);
        return(KES_ERROR_IO);
    }

    /* Write data */
    ssize_t bytes_written = write( storage->fd, buffer, size);
    if ( bytes_written != (ssize_t)size) {
        pthread_mutex_unlock( &storage->lock);
        return(KES_ERROR_IO);
    }

    /* Update statistics */
    storage->stats.writes_completed++;
    storage->stats.bytes_written += size;

    pthread_mutex_unlock( &storage->lock);

    return(KES_SUCCESS);
}

/* =================================================================
 * Information and Statistics
 * ================================================================= */

int kes_storage_get_stats( kes_storage_t *storage,
                            kes_storage_stats_t *stats) {
    if ( storage == NULL || stats == NULL) {
        return(KES_ERROR_INVALID);
    }

    pthread_mutex_lock( &storage->lock);

    /* Copy basic statistics */
    memcpy( stats, &storage->stats, sizeof(kes_storage_stats_t));

    /* Update with current values */
    stats->total_blocks = storage->desc.total_blocks;
    stats->free_blocks = storage->desc.free_blocks;
    stats->used_blocks = storage->desc.used_blocks;

    /* Calculate fragmentation (simplified) */
    if ( stats->used_blocks > 0) {
        stats->fragmentation =
            (uint64_t)(100.0 * (storage->stats.allocated_extents - 1) /
                      stats->used_blocks);
    } else {
        stats->fragmentation = 0;
    }

    pthread_mutex_unlock( &storage->lock);

    return(KES_SUCCESS);
}

int kes_storage_get_descriptor( kes_storage_t *storage,
                                 kes_storage_descriptor_t *descriptor) {
    if ( storage == NULL || descriptor == NULL) {
        return(KES_ERROR_INVALID);
    }

    pthread_mutex_lock( &storage->lock);
    memcpy( descriptor, &storage->desc, sizeof(kes_storage_descriptor_t));
    pthread_mutex_unlock( &storage->lock);

    return(KES_SUCCESS);
}

/* =================================================================
 * Utility Functions Implementation
 * ================================================================= */

void kes_get_version( uint16_t *major, uint16_t *minor, uint16_t *patch) {
    if ( major != NULL) {
        *major = KES_VERSION_MAJOR;
    }
    if ( minor != NULL) {
        *minor = KES_VERSION_MINOR;
    }
    if ( patch != NULL) {
        *patch = KES_VERSION_PATCH;
    }
}

const char *kes_get_error_string( int error_code) {
    int index = -error_code;
    if ( index < 0 || index >= (int)(sizeof(error_strings) /
                                   sizeof(error_strings[0]))) {
        return("Unknown error");
    }
    return(error_strings[index]);
}

uint32_t kes_calculate_blocks_needed( size_t byte_size,
                                       uint32_t block_size) {
    return((uint32_t)KES_BYTES_TO_BLOCKS(byte_size, block_size));
}

/**
 * Calculate total size of extent in bytes
 * @param extent Extent descriptor
 * @return Extent size in bytes, computed using KES_DEFAULT_BLOCK_SIZE
 *
 * NOTE: this always uses KES_DEFAULT_BLOCK_SIZE, not the actual
 * block size of any particular storage instance (kes_extent_descriptor_t
 * doesn't carry block_size). For a storage created with a non-default
 * block_size, this returns the WRONG answer. Prefer computing extent
 * size directly as (uint64_t)extent->block_count * storage->desc.block_size
 * when the storage handle is available. This limitation is tracked,
 * not silently fixed here, because changing this function's inputs
 * is a public API signature change outside this fix's scope.
 */
size_t kes_calculate_extent_size( const kes_extent_descriptor_t *extent) {
    if ( extent == NULL) {
        return(0);
    }
    return((size_t)extent->block_count * KES_DEFAULT_BLOCK_SIZE);
}

int kes_config_validate( const kes_storage_config_t *config) {
    return(validate_config( config));
}

/* =================================================================
 * Internal Helper Functions
 * ================================================================= */

static int validate_config( const kes_storage_config_t *config) {
    if ( config == NULL || config->device_path == NULL) {
        return(KES_ERROR_INVALID);
    }

    /* Validate block size */
    if ( !KES_IS_POWER_OF_2(config->block_size) ||
        config->block_size < KES_MIN_BLOCK_SIZE ||
        config->block_size > KES_MAX_BLOCK_SIZE) {
        return(KES_ERROR_INVALID);
    }

    /* Validate device size */
    if ( config->device_size < config->block_size * 10) {
        return(KES_ERROR_INVALID);  /* Too small */
    }

    return(KES_SUCCESS);
}

static int init_storage_descriptor( kes_storage_t *storage, uint64_t size) {
    kes_storage_descriptor_t *desc = &storage->desc;

    /* Initialize basic fields */
    desc->magic = KES_MAGIC_NUMBER;
    desc->version_major = KES_VERSION_MAJOR;
    desc->version_minor = KES_VERSION_MINOR;
    desc->block_size = KES_DEFAULT_BLOCK_SIZE;
    desc->total_blocks = size / desc->block_size;

    /* Calculate layout */
    uint64_t bitmap_blocks = calculate_bitmap_blocks( desc->total_blocks,
                                                        desc->block_size);

    /* Layout: [descriptor] [user data] [bitmap] */
    desc->bitmap_start_block = desc->total_blocks - bitmap_blocks;
    desc->bitmap_blocks = bitmap_blocks;
    desc->user_start_block = 1;  /* Block 0 is descriptor */
    desc->user_blocks = desc->bitmap_start_block - desc->user_start_block;

    /* Initialize statistics */
    desc->free_blocks = desc->user_blocks;
    desc->used_blocks = 0;
    desc->next_extent_id = 1;

    return(KES_SUCCESS);
}

static int load_storage_descriptor( kes_storage_t *storage) {
    /* Seek to beginning of file */
    if ( lseek( storage->fd, 0, SEEK_SET) != 0) {
        return(KES_ERROR_IO);
    }

    /* Read descriptor */
    ssize_t bytes_read = read( storage->fd, &storage->desc,
                                sizeof(kes_storage_descriptor_t));
    if ( bytes_read != sizeof(kes_storage_descriptor_t)) {
        return(KES_ERROR_IO);
    }

    /* Validate descriptor */
    if ( storage->desc.magic != KES_MAGIC_NUMBER) {
        return(KES_ERROR_CORRUPT);
    }

    /* KES-6: the on-disk format gained a bitmap_checksum field and
     * shrank kes_storage_descriptor_t's reserved bytes accordingly --
     * a breaking change. Reject anything not written by this exact
     * major version rather than silently trusting a stale layout
     * (whose reserved bytes would read as zero, not a real checksum,
     * and would then always spuriously fail the checksum check in
     * kes_storage_open() with a confusing KES_ERROR_CORRUPT instead
     * of this precise one). */
    if ( storage->desc.version_major != KES_VERSION_MAJOR) {
        TRACE_ERR( "incompatible on-disk format version %u.%u "
                   "(library is %u.%u) -- storage files created "
                   "before the KES-6 bitmap-checksum change must be "
                   "recreated",
                   storage->desc.version_major,
                   storage->desc.version_minor,
                   KES_VERSION_MAJOR, KES_VERSION_MINOR);
        return(KES_ERROR_CORRUPT);
    }

    return(KES_SUCCESS);
}

/*
 * noinline: with -O2 -fsanitize=thread, GCC 13 inlines this into
 * kes_storage_create() and then misidentifies the write() source
 * object as storage->desc.magic (4 bytes) instead of the full
 * kes_storage_descriptor_t (112 bytes), raising a false-positive
 * -Wstringop-overread that only reproduces under that specific
 * flag combination (confirmed absent under plain -O2 and under
 * -fsanitize=address). Blocking inlining removes the misanalyzed
 * context; the write() call itself is correctly sized.
 */
static int __attribute__((noinline))
save_storage_descriptor( kes_storage_t *storage) {
    /* Seek to beginning of file */
    if ( lseek( storage->fd, 0, SEEK_SET) != 0) {
        return(KES_ERROR_IO);
    }

    /* Write descriptor */
    ssize_t bytes_written = write( storage->fd, &storage->desc,
                                    sizeof(kes_storage_descriptor_t));
    if ( bytes_written != sizeof(kes_storage_descriptor_t)) {
        return(KES_ERROR_IO);
    }

    return(KES_SUCCESS);
}

static uint64_t calculate_bitmap_blocks( uint64_t total_blocks,
                                          uint32_t block_size) {
    /* Each byte represents 8 blocks */
    uint64_t bitmap_bytes = (total_blocks + 7) / 8;
    return((bitmap_bytes + block_size - 1) / block_size);
}

static int allocate_extent_first_fit( kes_storage_t *storage,
                                      const kes_extent_request_t *request,
                                      kes_extent_descriptor_t *extent) {
    uint64_t found_start;
    int result = kes_bitmap_find_free( storage->bitmap, request->block_count,
                                        request->hint_block, &found_start);

    if ( result != KES_SUCCESS) {
        return(result);
    }

    /* Mark blocks as used */
    result = kes_bitmap_set_range( storage->bitmap, found_start,
                                    request->block_count);
    if ( result != KES_SUCCESS) {
        return(result);
    }

    /* Fill extent descriptor */
    extent->start_block = found_start;
    extent->block_count = request->block_count;
    extent->flags = request->flags;
    extent->extent_id = 0;  /* Will be set by caller */

    return(KES_SUCCESS);
}
