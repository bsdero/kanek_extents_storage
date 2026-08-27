#define _GNU_SOURCE  /* For aligned_alloc, clock_gettime */

#include <kes/kes_cache.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include "trace.h"

/* Internal constants */
#define KES_CACHE_DEFAULT_BUCKETS    1024
#define KES_CACHE_HASH_SEED         0x12345678
#define KES_CACHE_SYNC_BATCH_SIZE   32

/* Memory alignment for optimal performance */
#define KES_CACHE_ALIGNMENT         64

/* Utility macros */
#define KES_ALIGN(size, align) \
    (((size) + (align) - 1) & ~((align) - 1))

#define KES_CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* =================================================================
 * Internal Helper Functions
 * ================================================================= */

/**
 * Get current timestamp in microseconds
 */
static uint64_t get_timestamp( void) {
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts);
    return((uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}

/**
 * Simple hash function for extent IDs
 */
uint32_t kes_extent_hash( const kes_extent_id_t *id) {
    uint32_t hash = KES_CACHE_HASH_SEED;
    hash ^= (uint32_t)(id->start_block & 0xFFFFFFFF);
    hash ^= (uint32_t)(id->start_block >> 32);
    hash ^= id->block_count;
    hash ^= id->block_size;

    /* Simple mixing to reduce collisions */
    hash = ((hash >> 16) ^ hash) * 0x45d9f3b;
    hash = ((hash >> 16) ^ hash) * 0x45d9f3b;
    hash = (hash >> 16) ^ hash;

    return(hash);
}

/**
 * Compare extent identifiers
 */
bool kes_extent_equal( const kes_extent_id_t *id1,
                        const kes_extent_id_t *id2) {
    return(id1->start_block == id2->start_block &&
           id1->block_count == id2->block_count &&
           id1->block_size == id2->block_size);
}

/**
 * Calculate extent data size
 */
static size_t extent_data_size( const kes_extent_id_t *id) {
    return((size_t)id->block_count * id->block_size);
}

/**
 * Initialize extent entry
 */
static void init_extent_entry( kes_extent_entry_t *entry,
                                const kes_extent_id_t *id) {
    memcpy( &entry->id, id, sizeof(kes_extent_id_t));
    entry->data = NULL;
    entry->state = 0;
    entry->ref_count = 0;
    entry->pin_count = 0;
    entry->lookup_pins = 0;
    entry->cond_waiters = 0;
    entry->access_time = get_timestamp();
    entry->access_count = 0;
    entry->data_size = extent_data_size( id);

    entry->hash_next = NULL;
    entry->hash_prev = NULL;
    entry->list_next = NULL;
    entry->list_prev = NULL;

    pthread_mutex_init( &entry->lock, NULL);
    pthread_cond_init( &entry->cond, NULL);
}

/* =================================================================
 * LRU List Management
 * ================================================================= */

/**
 * Add entry to head of LRU list (most recently used)
 */
static void lru_add_head( kes_cache_t *cache, kes_extent_entry_t *entry) {
    entry->list_next = cache->mru_head;
    entry->list_prev = NULL;

    if ( cache->mru_head != NULL) {
        cache->mru_head->list_prev = entry;
    }
    cache->mru_head = entry;

    if ( cache->lru_tail == NULL) {
        cache->lru_tail = entry;
    }
}

/**
 * Remove entry from LRU list
 */
static void lru_remove( kes_cache_t *cache, kes_extent_entry_t *entry) {
    if ( entry->list_prev != NULL) {
        entry->list_prev->list_next = entry->list_next;
    } else {
        cache->mru_head = entry->list_next;
    }

    if ( entry->list_next != NULL) {
        entry->list_next->list_prev = entry->list_prev;
    } else {
        cache->lru_tail = entry->list_prev;
    }

    entry->list_next = NULL;
    entry->list_prev = NULL;
}

/**
 * Move entry to head of LRU list
 */
static void lru_touch( kes_cache_t *cache, kes_extent_entry_t *entry) {
    if ( entry == cache->mru_head) {
        return;  /* Already at head */
    }

    lru_remove( cache, entry);
    lru_add_head( cache, entry);
}

/* =================================================================
 * Hash Table Management
 * ================================================================= */

/**
 * Find extent entry in hash table. If found, atomically pins it
 * (bumps lookup_pins under the bucket lock) against a concurrent
 * evictor destroying it before the caller gets a chance to lock
 * entry->lock -- see the lookup_pins field doc comment in
 * kes_cache.h. Every caller that receives a non-NULL entry must
 * release this pin (__atomic_fetch_sub on lookup_pins) immediately
 * after it locks entry->lock.
 */
static kes_extent_entry_t *hash_find( kes_cache_t *cache,
                                       const kes_extent_id_t *id) {
    uint32_t hash = kes_extent_hash( id);
    uint32_t bucket_idx = hash & cache->bucket_mask;
    kes_cache_bucket_t *bucket = &cache->buckets[bucket_idx];

    pthread_rwlock_rdlock( &bucket->lock);

    kes_extent_entry_t *entry = bucket->head;
    while ( entry != NULL) {
        if ( kes_extent_equal( &entry->id, id)) {
            __atomic_fetch_add( &entry->lookup_pins, 1,
                                 __ATOMIC_SEQ_CST);
            break;
        }
        entry = entry->hash_next;
    }

    pthread_rwlock_unlock( &bucket->lock);
    return(entry);
}

/**
 * Atomically look up an extent id in the hash table and, if no
 * entry exists for it yet, insert candidate as that entry -- all
 * under a single bucket lock. This closes the miss-then-insert race
 * that hash_find() followed by a separate insert step used to have:
 * two threads racing a miss on the same id could each independently
 * decide to insert their own entry, leaving two distinct cache
 * entries for one extent, each issuing its own read_extent I/O
 * (see PENDING_ITEMS.md, P0).
 *
 * Returns the pre-existing entry if one was already present, in
 * which case candidate was NOT inserted and is still owned by the
 * caller. Returns NULL if no existing entry was found, in which
 * case candidate was inserted and is now owned by the hash table.
 *
 * Like hash_find(), a non-NULL return is pinned via lookup_pins
 * before the bucket lock is released; the caller must release it
 * immediately after locking entry->lock, same as any hash_find()
 * caller.
 */
static kes_extent_entry_t *hash_find_or_insert(
    kes_cache_t *cache, kes_extent_entry_t *candidate) {
    uint32_t hash = kes_extent_hash( &candidate->id);
    uint32_t bucket_idx = hash & cache->bucket_mask;
    kes_cache_bucket_t *bucket = &cache->buckets[bucket_idx];

    pthread_rwlock_wrlock( &bucket->lock);

    kes_extent_entry_t *entry = bucket->head;
    while ( entry != NULL) {
        if ( kes_extent_equal( &entry->id, &candidate->id)) {
            break;
        }
        entry = entry->hash_next;
    }

    if ( entry == NULL) {
        candidate->hash_next = bucket->head;
        candidate->hash_prev = NULL;

        if ( bucket->head != NULL) {
            bucket->head->hash_prev = candidate;
        }
        bucket->head = candidate;
    } else {
        __atomic_fetch_add( &entry->lookup_pins, 1, __ATOMIC_SEQ_CST);
    }

    pthread_rwlock_unlock( &bucket->lock);

    return(entry);
}

/* =================================================================
 * Memory Management
 * ================================================================= */

/**
 * Allocate memory for extent data
 */
/*
 * Allocates the data buffer only -- does NOT touch
 * cache->stats.memory_used. That accounting is cache-wide state and
 * must be updated under cache_lock by the caller; this function's
 * only current caller (the get_extent miss path) does not hold
 * cache_lock across the allocation itself, so updating the stat in
 * here raced when two misses allocated concurrently (confirmed by
 * TSan: src/kes_cache.c:239, "data race ... in extent_alloc_data").
 */
static void *extent_alloc_data( kes_cache_t *cache, size_t size) {
    /* For now, use regular malloc. In production, this would use
     * the memory pool or mmap for large allocations */
    (void)cache;
    size_t aligned_size = KES_ALIGN(size, KES_CACHE_ALIGNMENT);

    return( aligned_alloc( KES_CACHE_ALIGNMENT, aligned_size));
}

/*
 * Frees the data buffer and updates cache->stats.memory_used. Every
 * caller (kes_cache_destroy, try_evict_entry_locked) already holds
 * cache_lock across the whole call; a future caller that does not
 * hold cache_lock must take it before calling this.
 */
static void extent_free_data( kes_cache_t *cache, void *ptr,
                               size_t size) {
    if ( ptr != NULL) {
        free( ptr);
        size_t aligned_size = KES_ALIGN(size, KES_CACHE_ALIGNMENT);
        cache->stats.memory_used -= aligned_size;
    }
}

/**
 * Check whether entry is still evictable, and if so unlink it from
 * its hash bucket and LRU list and free it (destroy its mutex/cond,
 * free its data buffer and the struct itself). Caller must already
 * hold entry->lock.
 *
 * ref_count/pin_count/state are read here under entry->lock, which
 * the caller already holds -- exactly the same lock every other
 * modifier of those fields (kes_cache_get_extent()'s hit path,
 * kes_cache_put_extent(), kes_cache_mark_dirty(),
 * kes_cache_pin_extent()/kes_cache_unpin_extent()) already uses, so
 * this read is race-free and stable for this function's whole
 * duration without needing any additional lock. An earlier build of
 * this function instead protected a *separate* traversal-only pin
 * counter with cache_lock and read ref_count under cache_lock too --
 * ASan caught the real bug in that design: ref_count itself is
 * modified under entry->lock elsewhere in this file, so touching it
 * under cache_lock anywhere (as the old traversal pin did) was a
 * genuine data race between two independent lock domains on the
 * same field, not just a logic error. See cache_sweep()'s and
 * make_room_for_new_entry()'s doc comments for how the traversal
 * pin is done now (via lookup_pins, atomically, no cache_lock
 * needed for the pin itself).
 *
 * lookup_pins == 0 means no thread that already found this entry
 * via hash_find()/hash_find_or_insert() (protected by the bucket
 * lock), or is mid-traversal via cache_sweep()/
 * make_room_for_new_entry() (protected by cache_lock), is still
 * between that lookup and locking entry->lock -- see the
 * lookup_pins field doc comment in kes_cache.h. Because those two
 * kinds of pinner are synchronized by two *different* locks, this
 * function checks lookup_pins exactly once, while holding BOTH
 * locks (bucket wrlock, then cache_lock nested inside it), and does
 * not release either lock until the corresponding removal (hash
 * unlink under the bucket lock, LRU unlink under cache_lock) is
 * also done. An earlier build checked lookup_pins under the bucket
 * lock only, then released it before separately acquiring
 * cache_lock to do the LRU removal -- ASan caught the resulting
 * heap-use-after-free: a cache_sweep()/make_room_for_new_entry()
 * traversal pin (legitimately protected by cache_lock, since that
 * is what lru_remove() also requires) could land in the gap between
 * those two separately-locked steps, completely invisible to a
 * lookup_pins check that had already finished under a different
 * lock. Checking once under both locks together closes that gap for
 * both pinner kinds at once.
 *
 * cond_waiters == 0 closes a third, unrelated hazard TSan caught
 * separately: pthread_cond_wait() (kes_cache_get_extent()'s
 * KES_EXTENT_LOADING wait loop) internally unlocks entry->lock
 * while parked, so a waiter can be mid-wait with entry->lock
 * genuinely free even though it never explicitly released it --
 * ref_count can independently reach 0 during exactly that window,
 * which would otherwise look identical to "safe to evict" here. See
 * the cond_waiters field doc comment in kes_cache.h.
 *
 * `discard_dirty` controls whether a dirty or KES_EXTENT_ERROR entry
 * is still eligible: cache_sweep()/make_room_for_new_entry() pass
 * false (only evict a clean entry -- they are responsible for
 * attempting a flush first), while kes_cache_invalidate() passes
 * true (S4.2's discard-unconditionally semantics: an invalidated
 * entry's dirty data is thrown away, never flushed). ref_count == 0
 * && pin_count == 0 is required either way.
 *
 * On success (returns true), entry has been fully unlinked, its
 * mutex/cond destroyed, and its memory freed -- entry->lock is NOT
 * left locked (there is nothing left to unlock) and the caller must
 * not touch entry again in any way. On failure (returns false --
 * either entry no longer meets the eviction criteria, or a
 * concurrent lookup/traversal pin is in flight), entry is untouched
 * and still holds entry->lock exactly as the caller left it -- the
 * caller is responsible for unlocking it.
 */
static bool try_evict_entry_locked( kes_cache_t *cache,
                                     kes_extent_entry_t *entry,
                                     bool discard_dirty) {
    bool state_ok = ( entry->ref_count == 0 &&
                       entry->pin_count == 0 &&
                       entry->cond_waiters == 0 &&
                       ( discard_dirty ||
                         ( !(entry->state & KES_EXTENT_DIRTY) &&
                           !(entry->state & KES_EXTENT_ERROR))));
    if ( !state_ok) {
        return(false);
    }

    uint32_t hash = kes_extent_hash( &entry->id);
    uint32_t bucket_idx = hash & cache->bucket_mask;
    kes_cache_bucket_t *bucket = &cache->buckets[bucket_idx];

    pthread_rwlock_wrlock( &bucket->lock);
    pthread_mutex_lock( &cache->cache_lock);

    if ( __atomic_load_n( &entry->lookup_pins, __ATOMIC_SEQ_CST) != 0) {
        pthread_mutex_unlock( &cache->cache_lock);
        pthread_rwlock_unlock( &bucket->lock);
        return(false);
    }

    if ( entry->hash_prev != NULL) {
        entry->hash_prev->hash_next = entry->hash_next;
    } else {
        bucket->head = entry->hash_next;
    }
    if ( entry->hash_next != NULL) {
        entry->hash_next->hash_prev = entry->hash_prev;
    }
    entry->hash_next = NULL;
    entry->hash_prev = NULL;

    lru_remove( cache, entry);
    extent_free_data( cache, entry->data, entry->data_size);
    cache->stats.entries_cached--;

    pthread_mutex_unlock( &cache->cache_lock);
    pthread_rwlock_unlock( &bucket->lock);

    pthread_mutex_unlock( &entry->lock);
    pthread_mutex_destroy( &entry->lock);
    pthread_cond_destroy( &entry->cond);
    free( entry);

    return(true);
}

/* =================================================================
 * Sweep / Eviction Logic
 *
 * Shared by kes_cache_sync(), the background thread
 * (kes_cache_start()), and eviction-on-miss (kes_cache_get_extent(),
 * PENDING_ITEMS.md Phase 4). See KES_HARDENING_PLAN.md S4.1 for the
 * locking discipline both walkers below implement: cache_lock is
 * only held for list-traversal steps, never across the
 * write_extent() I/O call -- holding it across I/O would serialize
 * every unrelated get_extent()/put_extent() call on the whole cache
 * behind however long the disk write takes.
 *
 * Walking the LRU list while repeatedly dropping and reacquiring
 * cache_lock (once per node, for that node's I/O) creates two
 * distinct use-after-free hazards, both closed the same way -- by
 * pinning (via entry->lookup_pins, the same atomic counter
 * hash_find()/hash_find_or_insert() use, NOT entry->ref_count --
 * see below) whichever node(s) the walk currently holds raw
 * pointers to, before releasing cache_lock, and releasing the pin
 * again once entry->lock is acquired for that node:
 *
 *   1. The current node could be freed by a *different* concurrent
 *      evictor (another sync() call, the background thread, an
 *      unrelated eviction-on-miss, or invalidate()) while this walk
 *      is blocked inside that node's write_extent() call.
 *   2. The *next* node to visit could likewise be freed by a
 *      concurrent evictor while this walk is still busy with the
 *      current node -- so the next-node pointer captured while
 *      still holding cache_lock is pinned right there, before
 *      cache_lock is released, not just-in-time when the walk
 *      actually reaches it.
 *
 * This pin deliberately reuses lookup_pins rather than
 * entry->ref_count, and that choice is load-bearing, not
 * cosmetic: entry->ref_count is modified under entry->lock
 * everywhere else in this file (kes_cache_get_extent()'s hit path,
 * kes_cache_put_extent(), kes_cache_mark_dirty(),
 * kes_cache_pin_extent()/kes_cache_unpin_extent()). An earlier
 * build of this code pinned via entry->ref_count under cache_lock
 * instead -- a *different* lock protecting the *same* field -- and
 * ASan caught the resulting heap-use-after-free during development:
 * a concurrent get_extent() hit could race a sweep's ref_count
 * bump/drop with no lock in common between them, corrupting the
 * count and triggering a premature "unreferenced, safe to free"
 * decision on an entry a real caller still held. lookup_pins has no
 * such conflict -- it is only ever touched via __atomic builtins,
 * consistently, from every function that touches it.
 * ================================================================= */

/*
 * Flush `entry` if dirty, then evict it if (after the flush attempt)
 * it has become unreferenced, unpinned, and clean. Called with
 * `entry` already lookup_pins-pinned by the caller's traversal (so
 * it cannot be freed by anyone else while this function runs) and
 * entry->lock NOT held. Always returns with entry->lock unlocked --
 * either because try_evict_entry_locked() consumed it (entry
 * evicted), or because this function releases it itself (entry
 * kept). Does not touch cache_lock's held/not-held state across the
 * call: it acquires and releases it internally as needed and
 * returns with it not held, matching how it expects to be entered.
 */
static void sweep_flush_and_maybe_evict( kes_cache_t *cache,
                                          kes_extent_entry_t *entry) {
    pthread_mutex_lock( &entry->lock);

    /* Now that entry->lock is held, a concurrent evictor cannot
     * free this entry out from under us (try_evict_entry_locked()
     * requires entry->lock too) -- release the traversal pin taken
     * by the caller before it dropped cache_lock. */
    __atomic_fetch_sub( &entry->lookup_pins, 1, __ATOMIC_SEQ_CST);

    if ( (entry->state & KES_EXTENT_DIRTY) &&
         !(entry->state & KES_EXTENT_LOADING)) {
        if ( cache->write_extent != NULL) {
            int result = cache->write_extent(
                cache->config.device_handle, &entry->id,
                entry->data, entry->data_size);

            if ( result == KES_SUCCESS) {
                entry->state &= ~KES_EXTENT_DIRTY;
                entry->state &= ~KES_EXTENT_ERROR;
                entry->state |= KES_EXTENT_CLEAN;

                pthread_mutex_lock( &cache->cache_lock);
                cache->stats.bytes_written += entry->data_size;
                cache->stats.flushes++;
                cache->stats.entries_dirty--;
                pthread_mutex_unlock( &cache->cache_lock);
            } else {
                entry->state |= KES_EXTENT_ERROR;
                TRACE_ERR( "cache sweep: flush failed for extent "
                           "start_block=%llu block_count=%u",
                           (unsigned long long)entry->id.start_block,
                           entry->id.block_count);
            }
        } else {
            TRACE_ERR( "cache sweep: entry is dirty but no "
                       "write_extent callback registered");
        }
    }

    if ( !try_evict_entry_locked( cache, entry, false)) {
        pthread_mutex_unlock( &entry->lock);
    }
}

/*
 * Walk every cached entry once (from mru_head to lru_tail),
 * flushing dirty ones and freeing ones that become unreferenced and
 * unpinned. Used by kes_cache_sync() and the background thread. A
 * clean no-op on an empty cache.
 */
static void cache_sweep( kes_cache_t *cache) {
    pthread_mutex_lock( &cache->cache_lock);
    kes_extent_entry_t *entry = cache->mru_head;
    if ( entry != NULL) {
        __atomic_fetch_add( &entry->lookup_pins, 1, __ATOMIC_SEQ_CST);
    }
    pthread_mutex_unlock( &cache->cache_lock);

    while ( entry != NULL) {
        pthread_mutex_lock( &cache->cache_lock);
        kes_extent_entry_t *next = entry->list_next;
        if ( next != NULL) {
            __atomic_fetch_add( &next->lookup_pins, 1, __ATOMIC_SEQ_CST);
        }
        pthread_mutex_unlock( &cache->cache_lock);

        sweep_flush_and_maybe_evict( cache, entry);

        entry = next;
    }
}

/*
 * Attempt to make room for one more entry of `needed_size` bytes by
 * evicting from the LRU tail toward the head (PENDING_ITEMS.md
 * Phase 4 / KES_HARDENING_PLAN.md S5.1). Returns true once
 * config.max_entries/config.max_memory can accommodate the new
 * entry -- either no eviction was needed, or evicting freed enough
 * room. Returns false if it walked the entire LRU list and still
 * cannot fit: every remaining entry is referenced, pinned, or
 * dirty-and-unflushable. Per S5.2, the caller treats false as
 * KES_ERROR_BUSY rather than silently exceeding the configured
 * limit.
 *
 * This check-then-evict-then-insert sequence is not atomic against
 * other concurrent misses each running this same function for a
 * different id -- doing so would require holding cache_lock across
 * the whole sequence (including eviction I/O), which is exactly the
 * global-serialization tradeoff this file's locking discipline
 * avoids everywhere else. Under concurrent misses this can let
 * entries_cached/memory_used transiently overshoot the configured
 * limit by a small, bounded amount (at most one entry per
 * concurrently-racing miss) rather than enforcing it with strict
 * atomicity -- a deliberate, documented deviation allowed by S5.2.
 */
static bool make_room_for_new_entry( kes_cache_t *cache,
                                      size_t needed_size) {
    size_t aligned_needed = KES_ALIGN( needed_size, KES_CACHE_ALIGNMENT);

    pthread_mutex_lock( &cache->cache_lock);
    bool fits = ( cache->stats.entries_cached + 1 <=
                      cache->config.max_entries &&
                  cache->stats.memory_used + aligned_needed <=
                      cache->config.max_memory);
    kes_extent_entry_t *entry = fits ? NULL : cache->lru_tail;
    if ( entry != NULL) {
        __atomic_fetch_add( &entry->lookup_pins, 1, __ATOMIC_SEQ_CST);
    }
    pthread_mutex_unlock( &cache->cache_lock);

    while ( entry != NULL) {
        pthread_mutex_lock( &entry->lock);

        /* Now that entry->lock is held, a concurrent evictor cannot
         * free this entry out from under us -- release the
         * traversal pin taken above (or, on later iterations, taken
         * below for `prev`). See the Sweep/Eviction Logic section
         * comment for why this pin uses lookup_pins, not
         * entry->ref_count. */
        __atomic_fetch_sub( &entry->lookup_pins, 1, __ATOMIC_SEQ_CST);

        if ( (entry->state & KES_EXTENT_DIRTY) &&
             !(entry->state & KES_EXTENT_LOADING) &&
             cache->write_extent != NULL) {
            int result = cache->write_extent(
                cache->config.device_handle, &entry->id,
                entry->data, entry->data_size);

            if ( result == KES_SUCCESS) {
                entry->state &= ~KES_EXTENT_DIRTY;
                entry->state &= ~KES_EXTENT_ERROR;
                entry->state |= KES_EXTENT_CLEAN;

                pthread_mutex_lock( &cache->cache_lock);
                cache->stats.bytes_written += entry->data_size;
                cache->stats.flushes++;
                cache->stats.entries_dirty--;
                pthread_mutex_unlock( &cache->cache_lock);
            } else {
                entry->state |= KES_EXTENT_ERROR;
                TRACE_ERR( "eviction: flush failed for extent "
                           "start_block=%llu block_count=%u",
                           (unsigned long long)entry->id.start_block,
                           entry->id.block_count);
            }
        }

        pthread_mutex_lock( &cache->cache_lock);
        kes_extent_entry_t *prev = entry->list_prev;
        if ( !fits && prev != NULL) {
            __atomic_fetch_add( &prev->lookup_pins, 1, __ATOMIC_SEQ_CST);
        }
        pthread_mutex_unlock( &cache->cache_lock);

        if ( try_evict_entry_locked( cache, entry, false)) {
            pthread_mutex_lock( &cache->cache_lock);
            cache->stats.evictions++;
            fits = ( cache->stats.entries_cached + 1 <=
                          cache->config.max_entries &&
                      cache->stats.memory_used + aligned_needed <=
                          cache->config.max_memory);
            pthread_mutex_unlock( &cache->cache_lock);
        } else {
            pthread_mutex_unlock( &entry->lock);
        }

        if ( fits && prev != NULL) {
            /* Enough room now; release the pin taken above on
             * `prev` without visiting it, and stop the walk. */
            __atomic_fetch_sub( &prev->lookup_pins, 1, __ATOMIC_SEQ_CST);
            prev = NULL;
        }

        entry = prev;
    }

    return(fits);
}

/* =================================================================
 * Cache Operations Implementation
 * ================================================================= */

/**
 * Get default cache configuration
 */
void kes_cache_get_default_config( kes_cache_config_t *config,
                                    bool is_edge_device) {
    memset( config, 0, sizeof(kes_cache_config_t));

    if ( is_edge_device) {
        config->max_memory = 8 * 1024 * 1024;      /* 8MB */
        config->min_memory = 2 * 1024 * 1024;      /* 2MB */
        config->max_entries = 256;
        config->background_threads = 1;
        config->sync_interval_ms = 5000;           /* 5 seconds */
    } else {
        config->max_memory = 512 * 1024 * 1024;    /* 512MB */
        config->min_memory = 64 * 1024 * 1024;     /* 64MB */
        config->max_entries = 4096;
        config->background_threads = 4;
        config->sync_interval_ms = 1000;           /* 1 second */
    }

    config->policy = KES_CACHE_LRU;
    config->enable_prefetch = true;
    config->enable_compression = false;
    config->device_handle = NULL;
}

/**
 * Create cache instance
 */
kes_cache_t *kes_cache_create( const kes_cache_config_t *config) {
    if ( config == NULL || config->max_memory < KES_CACHE_MIN_MEMORY ||
        config->max_entries < KES_CACHE_MIN_ENTRIES) {
        return(NULL);
    }

    /*
     * S5.3: only KES_CACHE_LRU is implemented. Silently accepting
     * KES_CACHE_LFU/KES_CACHE_CUSTOM and behaving as if LRU were
     * selected would hide a mismatch between what the caller asked
     * for and what they got, so reject it outright instead. Note:
     * KES_HARDENING_PLAN.md S5.3 says to return KES_ERROR_INVALID,
     * but kes_cache_create()'s signature returns kes_cache_t* (an
     * existing, unchanged part of the public API), not int -- NULL
     * is this function's only way to signal failure, consistent
     * with every other validation failure a few lines up.
     */
    if ( config->policy != KES_CACHE_LRU) {
        return(NULL);
    }

    kes_cache_t *cache = calloc( 1, sizeof(kes_cache_t));
    if ( cache == NULL) {
        return(NULL);
    }

    /* Copy configuration */
    memcpy( &cache->config, config, sizeof(kes_cache_config_t));

    /* Initialize hash table */
    cache->bucket_count = KES_CACHE_DEFAULT_BUCKETS;
    cache->bucket_mask = cache->bucket_count - 1;
    cache->buckets = calloc( cache->bucket_count,
                              sizeof(kes_cache_bucket_t));
    if ( cache->buckets == NULL) {
        free( cache);
        return(NULL);
    }

    /* Initialize bucket locks */
    for ( uint32_t i = 0; i < cache->bucket_count; i++) {
        pthread_rwlock_init( &cache->buckets[i].lock, NULL);
    }

    /* Initialize cache lock and condition variables. bg_cond is
     * timed-waited on by the background thread (kes_cache_start(),
     * Phase 3.4); its default clock attribute would be
     * CLOCK_REALTIME, which is vulnerable to wall-clock adjustments
     * (NTP steps, manual clock changes) making a timed wait fire
     * early or absurdly late. Switch to CLOCK_MONOTONIC here, before
     * any timed wait is ever added, per
     * KES_HARDENING_PLAN.md S4.4 -- retrofitting this later would
     * mean auditing every timed wait added in the meantime. */
    pthread_mutex_init( &cache->cache_lock, NULL);
    pthread_condattr_t cond_attr;
    pthread_condattr_init( &cond_attr);
    pthread_condattr_setclock( &cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init( &cache->bg_cond, &cond_attr);
    pthread_condattr_destroy( &cond_attr);

    /* Initialize LRU list pointers */
    cache->mru_head = NULL;
    cache->lru_tail = NULL;

    /* Initialize statistics */
    memset( &cache->stats, 0, sizeof(kes_cache_stats_t));

    cache->shutdown = false;

    return(cache);
}

/**
 * Destroy cache
 */
int kes_cache_destroy( kes_cache_t *cache) {
    if ( cache == NULL) {
        return(KES_ERROR_INVALID);
    }

    /* Stop background threads first */
    kes_cache_stop( cache);

    /* Free all cached entries */
    pthread_mutex_lock( &cache->cache_lock);

    kes_extent_entry_t *entry = cache->mru_head;
    while ( entry != NULL) {
        kes_extent_entry_t *next = entry->list_next;

        /* Free entry data */
        if ( entry->data != NULL) {
            extent_free_data( cache, entry->data, entry->data_size);
        }

        /* Cleanup entry locks */
        pthread_mutex_destroy( &entry->lock);
        pthread_cond_destroy( &entry->cond);

        free( entry);
        entry = next;
    }

    pthread_mutex_unlock( &cache->cache_lock);

    /* Cleanup hash table */
    for ( uint32_t i = 0; i < cache->bucket_count; i++) {
        pthread_rwlock_destroy( &cache->buckets[i].lock);
    }
    free( cache->buckets);

    /* Cleanup cache locks */
    pthread_mutex_destroy( &cache->cache_lock);
    pthread_cond_destroy( &cache->bg_cond);

    /* Free background threads array */
    if ( cache->bg_threads != NULL) {
        free( cache->bg_threads);
    }

    free( cache);
    return(KES_SUCCESS);
}

/**
 * Get extent from cache
 */
int kes_cache_get_extent( kes_cache_t *cache,
                           const kes_extent_id_t *id,
                           void **buffer) {
    if ( cache == NULL || id == NULL || buffer == NULL) {
        return(KES_ERROR_INVALID);
    }

    *buffer = NULL;

    /* Fast-path lookup: avoids building a candidate entry for the
     * common case where the extent is already cached. This check is
     * not itself race-free against a concurrent insert, but that is
     * fine -- the only operation that must be atomic is the
     * check-and-insert done below via hash_find_or_insert(), which
     * is the actual race this function has to close. */
    kes_extent_entry_t *entry = hash_find( cache, id);

    if ( entry == NULL) {
        /*
         * Cache miss on the fast lookup. Before building a new
         * entry, make room for it under config.max_entries/
         * config.max_memory if needed (Phase 4 / S5.1), evicting
         * from the LRU tail. See make_room_for_new_entry()'s doc
         * comment for why this is a documented, bounded-overshoot
         * check rather than a strictly atomic one under concurrent
         * misses.
         */
        if ( !make_room_for_new_entry( cache, extent_data_size( id))) {
            TRACE_ERR( "kes_cache_get_extent: cache full and unable "
                       "to evict enough room for extent "
                       "start_block=%llu block_count=%u",
                       (unsigned long long)id->start_block,
                       id->block_count);
            return(KES_ERROR_BUSY);
        }

        /*
         * Build a candidate entry and publish it via
         * hash_find_or_insert(), which holds a single bucket lock
         * across the "does an entry already exist" check and the
         * insert. That is what prevents two threads that both
         * missed above from each inserting their own entry for the
         * same extent id.
         */
        kes_extent_entry_t *candidate = calloc( 1,
                                             sizeof(kes_extent_entry_t));
        if ( candidate == NULL) {
            return(KES_ERROR_NOMEM);
        }

        init_extent_entry( candidate, id);
        candidate->state = KES_EXTENT_LOADING;
        candidate->ref_count = 1;

        candidate->data = extent_alloc_data( cache, candidate->data_size);
        if ( candidate->data == NULL) {
            /* init_extent_entry() already initialized
             * candidate->lock/cond; candidate was never inserted
             * into the hash table or LRU list, so nothing else will
             * ever destroy them if we don't do it here. */
            pthread_mutex_destroy( &candidate->lock);
            pthread_cond_destroy( &candidate->cond);
            free( candidate);
            return(KES_ERROR_NOMEM);
        }

        entry = hash_find_or_insert( cache, candidate);

        if ( entry == NULL) {
            /* We won the race: candidate is now the published entry
             * for this id and this call is responsible for loading
             * it from disk. */
            entry = candidate;

            /*
             * entries_cached and memory_used are cache-wide state;
             * memory_used was previously updated inside
             * extent_alloc_data() with no lock held at all -- same
             * bug class as stats.misses below.
             */
            pthread_mutex_lock( &cache->cache_lock);
            cache->stats.misses++;
            lru_add_head( cache, entry);
            cache->stats.entries_cached++;
            cache->stats.memory_used +=
                KES_ALIGN( entry->data_size, KES_CACHE_ALIGNMENT);
            pthread_mutex_unlock( &cache->cache_lock);

            /* Load data from disk */
            int result;
            if ( cache->read_extent != NULL) {
                result = cache->read_extent( cache->config.device_handle,
                                              id, entry->data,
                                              entry->data_size);
            } else {
                TRACE_ERR( "kes_cache_get_extent: no read_extent "
                           "callback registered (call "
                           "kes_cache_set_io_callbacks() before "
                           "using the cache)");
                result = KES_ERROR_INVALID;
            }

            pthread_mutex_lock( &entry->lock);

            if ( result == KES_SUCCESS) {
                entry->state = KES_EXTENT_CLEAN;
                *buffer = entry->data;
            } else {
                entry->state = KES_EXTENT_ERROR;
                if ( result != KES_ERROR_INVALID) {
                    result = KES_ERROR_IO;
                }
                /* The caller never received a valid buffer, so it
                 * holds no logical reference to this entry --
                 * release the ref_count this function set to 1 at
                 * creation time, or this entry can never be
                 * considered unreferenced again (relevant once
                 * kes_cache_invalidate() exists -- see
                 * PENDING_ITEMS.md Phase 3 -- which refuses to touch
                 * entries with ref_count > 0). */
                if ( entry->ref_count > 0) {
                    entry->ref_count--;
                }
                TRACE_ERR( "load failed for extent start_block=%llu "
                           "block_count=%u, releasing phantom "
                           "ref_count",
                           (unsigned long long)id->start_block,
                           id->block_count);
            }

            /* Wake up any waiting threads */
            pthread_cond_broadcast( &entry->cond);
            pthread_mutex_unlock( &entry->lock);

            /*
             * stats.bytes_read is cache-wide, not per-entry -- it
             * must be protected by cache_lock, not entry->lock.
             * Updating it while only entry->lock was held let two
             * threads populating different misses race on the same
             * counter (confirmed by TSan: src/kes_cache.c:472,
             * "data race ... in kes_cache_get_extent").
             */
            if ( result == KES_SUCCESS) {
                pthread_mutex_lock( &cache->cache_lock);
                cache->stats.bytes_read += entry->data_size;
                pthread_mutex_unlock( &cache->cache_lock);
            }

            return(result);
        }

        /*
         * We lost the race: another thread published an entry for
         * this id between our fast hash_find() miss and now. Our
         * candidate was never published, so its data_size was never
         * added to cache->stats.memory_used -- discard it with a
         * plain free() rather than extent_free_data(), which
         * assumes the buffer it is freeing was already accounted
         * for and would wrongly decrement memory_used here.
         */
        free( candidate->data);
        pthread_mutex_destroy( &candidate->lock);
        pthread_cond_destroy( &candidate->cond);
        free( candidate);
    }

    /* Cache hit -- either a real hit from the fast lookup above, or
     * this call lost the insert race and is attaching to the entry
     * that won it. Either way, wait out any in-progress load the
     * same way. */
    pthread_mutex_lock( &entry->lock);

    /* entry came from hash_find() or hash_find_or_insert()'s
     * found-existing branch, both of which pinned it via
     * lookup_pins; release that pin now that entry->lock is held
     * (see the field's doc comment in kes_cache.h). */
    __atomic_fetch_sub( &entry->lookup_pins, 1, __ATOMIC_SEQ_CST);

    if ( entry->state & KES_EXTENT_LOADING) {
        /* pthread_cond_wait() internally unlocks entry->lock while
         * parked and re-locks it before returning -- cond_waiters
         * marks that gap so try_evict_entry_locked() knows not to
         * free this entry (and destroy entry->lock/cond out from
         * under us) while we're parked in it, even though ref_count
         * may independently and legitimately reach 0 during that
         * exact window (see the cond_waiters field doc comment in
         * kes_cache.h). */
        entry->cond_waiters++;
        while ( entry->state & KES_EXTENT_LOADING) {
            pthread_cond_wait( &entry->cond, &entry->lock);
        }
        entry->cond_waiters--;
    }

    if ( entry->state & KES_EXTENT_ERROR) {
        pthread_mutex_unlock( &entry->lock);
        return(KES_ERROR_IO);
    }

    entry->ref_count++;
    entry->access_time = get_timestamp();
    entry->access_count++;
    *buffer = entry->data;

    pthread_mutex_unlock( &entry->lock);

    /* Update LRU position */
    pthread_mutex_lock( &cache->cache_lock);
    lru_touch( cache, entry);
    cache->stats.hits++;
    pthread_mutex_unlock( &cache->cache_lock);

    return(KES_SUCCESS);
}

/**
 * Release extent reference
 */
int kes_cache_put_extent( kes_cache_t *cache,
                           const kes_extent_id_t *id) {
    if ( cache == NULL || id == NULL) {
        return(KES_ERROR_INVALID);
    }

    kes_extent_entry_t *entry = hash_find( cache, id);
    if ( entry == NULL) {
        return(KES_ERROR_NOTFOUND);
    }

    pthread_mutex_lock( &entry->lock);
    __atomic_fetch_sub( &entry->lookup_pins, 1, __ATOMIC_SEQ_CST);

    if ( entry->ref_count > 0) {
        entry->ref_count--;
    }

    pthread_mutex_unlock( &entry->lock);

    return(KES_SUCCESS);
}

/**
 * Mark extent as dirty
 */
int kes_cache_mark_dirty( kes_cache_t *cache,
                           const kes_extent_id_t *id) {
    if ( cache == NULL || id == NULL) {
        return(KES_ERROR_INVALID);
    }

    kes_extent_entry_t *entry = hash_find( cache, id);
    if ( entry == NULL) {
        return(KES_ERROR_NOTFOUND);
    }

    pthread_mutex_lock( &entry->lock);
    __atomic_fetch_sub( &entry->lookup_pins, 1, __ATOMIC_SEQ_CST);

    if ( !(entry->state & KES_EXTENT_DIRTY)) {
        entry->state |= KES_EXTENT_DIRTY;

        pthread_mutex_lock( &cache->cache_lock);
        cache->stats.entries_dirty++;
        pthread_mutex_unlock( &cache->cache_lock);
    }

    pthread_mutex_unlock( &entry->lock);

    return(KES_SUCCESS);
}

/**
 * Pin extent in memory
 */
int kes_cache_pin_extent( kes_cache_t *cache,
                           const kes_extent_id_t *id) {
    if ( cache == NULL || id == NULL) {
        return(KES_ERROR_INVALID);
    }

    kes_extent_entry_t *entry = hash_find( cache, id);
    if ( entry == NULL) {
        return(KES_ERROR_NOTFOUND);
    }

    pthread_mutex_lock( &entry->lock);
    __atomic_fetch_sub( &entry->lookup_pins, 1, __ATOMIC_SEQ_CST);

    if ( entry->pin_count == 0) {
        entry->state |= KES_EXTENT_PINNED;

        pthread_mutex_lock( &cache->cache_lock);
        cache->stats.entries_pinned++;
        pthread_mutex_unlock( &cache->cache_lock);
    }
    entry->pin_count++;

    pthread_mutex_unlock( &entry->lock);

    return(KES_SUCCESS);
}

/**
 * Unpin extent (allow eviction)
 */
int kes_cache_unpin_extent( kes_cache_t *cache,
                             const kes_extent_id_t *id) {
    if ( cache == NULL || id == NULL) {
        return(KES_ERROR_INVALID);
    }

    kes_extent_entry_t *entry = hash_find( cache, id);
    if ( entry == NULL) {
        return(KES_ERROR_NOTFOUND);
    }

    pthread_mutex_lock( &entry->lock);
    __atomic_fetch_sub( &entry->lookup_pins, 1, __ATOMIC_SEQ_CST);

    if ( entry->pin_count > 0) {
        entry->pin_count--;
        if ( entry->pin_count == 0) {
            entry->state &= ~KES_EXTENT_PINNED;

            pthread_mutex_lock( &cache->cache_lock);
            cache->stats.entries_pinned--;
            pthread_mutex_unlock( &cache->cache_lock);
        }
    }

    pthread_mutex_unlock( &entry->lock);

    return(KES_SUCCESS);
}

/**
 * Get cache statistics
 */
int kes_cache_get_stats( kes_cache_t *cache, kes_cache_stats_t *stats) {
    if ( cache == NULL || stats == NULL) {
        return(KES_ERROR_INVALID);
    }

    pthread_mutex_lock( &cache->cache_lock);
    memcpy( stats, &cache->stats, sizeof(kes_cache_stats_t));
    pthread_mutex_unlock( &cache->cache_lock);

    return(KES_SUCCESS);
}

/**
 * Flush specific extent to disk
 */
int kes_cache_flush_extent( kes_cache_t *cache,
                             const kes_extent_id_t *id) {
    if ( cache == NULL || id == NULL) {
        return(KES_ERROR_INVALID);
    }

    kes_extent_entry_t *entry = hash_find( cache, id);
    if ( entry == NULL) {
        return(KES_ERROR_NOTFOUND);
    }

    pthread_mutex_lock( &entry->lock);
    __atomic_fetch_sub( &entry->lookup_pins, 1, __ATOMIC_SEQ_CST);

    if ( entry->state & KES_EXTENT_DIRTY) {
        if ( cache->write_extent == NULL) {
            TRACE_ERR( "kes_cache_flush_extent: entry is dirty but no "
                       "write_extent callback is registered (call "
                       "kes_cache_set_io_callbacks() before using "
                       "the cache)");
            pthread_mutex_unlock( &entry->lock);
            return(KES_ERROR_INVALID);
        }

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
    }

    pthread_mutex_unlock( &entry->lock);

    return(KES_SUCCESS);
}

/**
 * Stop background threads and prepare for shutdown
 */
int kes_cache_stop( kes_cache_t *cache) {
    if ( cache == NULL) {
        return(KES_ERROR_INVALID);
    }

    pthread_mutex_lock( &cache->cache_lock);
    cache->shutdown = true;
    pthread_cond_broadcast( &cache->bg_cond);
    pthread_mutex_unlock( &cache->cache_lock);

    /* Join background threads if they exist, then null bg_threads
     * out so a second kes_cache_stop() call (kes_cache_destroy()
     * always calls this once itself, even after a caller already
     * called it explicitly) sees bg_threads == NULL and skips the
     * join loop, rather than double-joining already-joined
     * pthread_t handles (undefined behavior). This also makes
     * kes_cache_destroy()'s own `if (cache->bg_threads != NULL)
     * free(...)` a safe no-op afterward. */
    if ( cache->bg_threads != NULL) {
        for ( int i = 0; i < cache->config.background_threads; i++) {
            pthread_join( cache->bg_threads[i], NULL);
        }
        free( cache->bg_threads);
        cache->bg_threads = NULL;
    }

    return(KES_SUCCESS);
}

/**
 * Set I/O callback functions
 */
int kes_cache_set_io_callbacks( kes_cache_t *cache,
    int (*read_func)( void *device, const kes_extent_id_t *id,
                       void *buffer, size_t size),
    int (*write_func)( void *device, const kes_extent_id_t *id,
                        const void *buffer, size_t size),
    int (*sync_func)( void *device)) {
    if ( cache == NULL) {
        return(KES_ERROR_INVALID);
    }

    cache->read_extent = read_func;
    cache->write_extent = write_func;
    cache->sync_device = sync_func;

    return(KES_SUCCESS);
}

/**
 * Flush all dirty extents to disk (sync operation)
 */
int kes_cache_sync( kes_cache_t *cache) {
    if ( cache == NULL) {
        return(KES_ERROR_INVALID);
    }

    cache_sweep( cache);

    return(KES_SUCCESS);
}

/**
 * Invalidate extent (remove from cache)
 */
int kes_cache_invalidate( kes_cache_t *cache,
                           const kes_extent_id_t *id) {
    if ( cache == NULL || id == NULL) {
        return(KES_ERROR_INVALID);
    }

    kes_extent_entry_t *entry = hash_find( cache, id);
    if ( entry == NULL) {
        return(KES_ERROR_NOTFOUND);
    }

    pthread_mutex_lock( &entry->lock);
    __atomic_fetch_sub( &entry->lookup_pins, 1, __ATOMIC_SEQ_CST);

    if ( entry->ref_count > 0 || entry->pin_count > 0) {
        pthread_mutex_unlock( &entry->lock);
        return(KES_ERROR_BUSY);
    }

    bool was_dirty = ( entry->state & KES_EXTENT_DIRTY) != 0;

    /* discard_dirty=true: S4.2 requires discarding unconditionally,
     * no implicit flush -- unlike cache_sweep()/
     * make_room_for_new_entry(), which pass false and only evict a
     * clean entry. */
    if ( !try_evict_entry_locked( cache, entry, true)) {
        /* Lost a race: either another thread's hash_find()/
         * hash_find_or_insert() call took a lookup pin on this
         * entry between our release above and this call, or
         * ref_count/pin_count changed since we checked them above.
         * Leave the entry in place rather than retrying -- the
         * caller can simply call invalidate() again. */
        pthread_mutex_unlock( &entry->lock);
        return(KES_ERROR_BUSY);
    }

    if ( was_dirty) {
        pthread_mutex_lock( &cache->cache_lock);
        cache->stats.entries_dirty--;
        pthread_mutex_unlock( &cache->cache_lock);
    }

    return(KES_SUCCESS);
}

/**
 * Reset cache statistics
 */
int kes_cache_reset_stats( kes_cache_t *cache) {
    if ( cache == NULL) {
        return(KES_ERROR_INVALID);
    }

    pthread_mutex_lock( &cache->cache_lock);

    /* Only the cumulative counters reset -- memory_used,
     * entries_cached, entries_dirty, and entries_pinned describe
     * current state, not history, and must survive unchanged (see
     * the doc comment on this function in kes_cache.h). */
    cache->stats.hits = 0;
    cache->stats.misses = 0;
    cache->stats.evictions = 0;
    cache->stats.flushes = 0;
    cache->stats.bytes_read = 0;
    cache->stats.bytes_written = 0;

    pthread_mutex_unlock( &cache->cache_lock);

    return(KES_SUCCESS);
}

/**
 * Background thread loop: wakes every config.sync_interval_ms (or
 * on kes_cache_stop()'s broadcast) and runs the same sweep
 * kes_cache_sync() runs manually. See cache->cache_lock/bg_cond's
 * CLOCK_MONOTONIC setup in kes_cache_create().
 */
static void *cache_bg_thread_func( void *arg) {
    kes_cache_t *cache = (kes_cache_t *)arg;

    pthread_mutex_lock( &cache->cache_lock);

    while ( !cache->shutdown) {
        struct timespec deadline;

        clock_gettime( CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += cache->config.sync_interval_ms / 1000;
        deadline.tv_nsec +=
            (cache->config.sync_interval_ms % 1000) * 1000000L;
        if ( deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            deadline.tv_sec += 1;
        }

        pthread_cond_timedwait( &cache->bg_cond, &cache->cache_lock,
                                 &deadline);

        if ( cache->shutdown) {
            break;
        }

        pthread_mutex_unlock( &cache->cache_lock);
        cache_sweep( cache);
        pthread_mutex_lock( &cache->cache_lock);
    }

    pthread_mutex_unlock( &cache->cache_lock);

    return(NULL);
}

/**
 * Start background cache management threads
 */
int kes_cache_start( kes_cache_t *cache) {
    if ( cache == NULL) {
        return(KES_ERROR_INVALID);
    }

    if ( cache->config.background_threads <= 0) {
        return(KES_ERROR_INVALID);
    }

    if ( cache->bg_threads != NULL) {
        TRACE_ERR( "kes_cache_start: background threads already "
                   "running (call kes_cache_stop() first)");
        return(KES_ERROR_EXISTS);
    }

    cache->bg_threads = calloc( (size_t)cache->config.background_threads,
                                 sizeof(pthread_t));
    if ( cache->bg_threads == NULL) {
        return(KES_ERROR_NOMEM);
    }

    cache->shutdown = false;

    for ( int i = 0; i < cache->config.background_threads; i++) {
        int result = pthread_create( &cache->bg_threads[i], NULL,
                                      cache_bg_thread_func, cache);
        if ( result != 0) {
            /* pthread_create() returns its error number directly,
             * it does not set errno -- TRACE_ERR (not TRACE_SYSERR)
             * with the explicit code is the correct macro here. */
            TRACE_ERR( "kes_cache_start: pthread_create failed for "
                       "background thread %d (error=%d)", i, result);

            pthread_mutex_lock( &cache->cache_lock);
            cache->shutdown = true;
            pthread_cond_broadcast( &cache->bg_cond);
            pthread_mutex_unlock( &cache->cache_lock);

            for ( int j = 0; j < i; j++) {
                pthread_join( cache->bg_threads[j], NULL);
            }
            free( cache->bg_threads);
            cache->bg_threads = NULL;
            cache->shutdown = false;

            return(KES_ERROR_INVALID);
        }
    }

    return(KES_SUCCESS);
}
