#ifndef KES_CACHE_H
#define KES_CACHE_H

/* This header uses pthread_rwlock_t, which on glibc requires a
 * feature-test macro (e.g. _GNU_SOURCE) to be defined before the
 * FIRST libc header is included anywhere in the translation unit --
 * defining it here would only work by luck of include order (see
 * debugging_plan.md fix #2). Every .c file that includes this
 * header must define _GNU_SOURCE (or _POSIX_C_SOURCE >= 200112L) as
 * its own first line, before any #include. src/kes_cache.c and
 * tests/test_kes_cache.c already do this. */

#include "kes_types.h"
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct kes_cache kes_cache_t;
typedef struct kes_extent_entry kes_extent_entry_t;

/* Cache limits */
#define KES_CACHE_MIN_MEMORY    (1024 * 1024)      /* 1MB minimum */
#define KES_CACHE_MAX_MEMORY    (1024 * 1024 * 1024) /* 1GB maximum */
#define KES_CACHE_MIN_ENTRIES   16
#define KES_CACHE_MAX_ENTRIES   65536

/* Extent identifier */
typedef struct {
    uint64_t start_block;    /* Starting block address */
    uint32_t block_count;    /* Number of contiguous blocks */
    uint32_t block_size;     /* Block size in bytes */
    uint32_t reserved;       /* Reserved for alignment */
} kes_extent_id_t;

/* Extent states */
typedef enum {
    KES_EXTENT_CLEAN    = 0x01,  /* In sync with disk */
    KES_EXTENT_DIRTY    = 0x02,  /* Modified, needs flush */
    KES_EXTENT_PINNED   = 0x04,  /* Cannot be evicted */
    KES_EXTENT_LOADING  = 0x08,  /* Being read from disk */
    KES_EXTENT_FLUSHING = 0x10,  /* Being written to disk */
    KES_EXTENT_ERROR    = 0x20   /* I/O error occurred */
} kes_extent_state_t;

/* Cache eviction policies. Only KES_CACHE_LRU is currently
 * implemented -- kes_cache_create() rejects KES_CACHE_LFU and
 * KES_CACHE_CUSTOM with NULL rather than silently falling back to
 * LRU behavior for a config that asked for something else. */
typedef enum {
    KES_CACHE_LRU = 0,           /* Least Recently Used */
    KES_CACHE_LFU,               /* Least Frequently Used -- NOT
                                   * implemented, rejected at
                                   * kes_cache_create() */
    KES_CACHE_CUSTOM             /* Custom policy -- NOT implemented,
                                   * rejected at kes_cache_create() */
} kes_cache_policy_t;

/* Cache configuration */
typedef struct {
    size_t max_memory;           /* Maximum cache memory bytes */
    size_t min_memory;           /* Minimum cache memory bytes */
    uint32_t max_entries;        /* Maximum cached extents */
    kes_cache_policy_t policy;   /* Eviction policy */
    int background_threads;      /* Number of background threads */
    int sync_interval_ms;        /* Background sync interval */
    bool enable_prefetch;        /* Enable read-ahead prefetching */
    bool enable_compression;     /* Enable extent compression */
    void *device_handle;         /* Storage device handle */
} kes_cache_config_t;

/* Cache statistics */
typedef struct {
    uint64_t hits;               /* Cache hits */
    uint64_t misses;             /* Cache misses */
    uint64_t evictions;          /* Number of evictions */
    uint64_t flushes;            /* Number of flushes */
    uint64_t bytes_read;         /* Bytes read from storage */
    uint64_t bytes_written;      /* Bytes written to storage */
    size_t memory_used;          /* Current memory usage */
    uint32_t entries_cached;     /* Current cached entries */
    uint32_t entries_dirty;      /* Current dirty entries */
    uint32_t entries_pinned;     /* Current pinned entries */
} kes_cache_stats_t;

/* Extent entry (internal structure, opaque to users) */
struct kes_extent_entry {
    kes_extent_id_t id;          /* Extent identifier */
    void *data;                  /* Cached data buffer */
    kes_extent_state_t state;    /* Current state flags */
    uint32_t ref_count;          /* Reference count */
    uint32_t pin_count;          /* Pin count */
    uint64_t access_time;        /* Last access timestamp */
    uint64_t access_count;       /* Total access count */
    size_t data_size;            /* Size of cached data */

    /* Number of callers currently holding a raw pointer to this
     * entry -- from hash_find()/hash_find_or_insert() (protected by
     * the entry's hash bucket lock), or mid-traversal in
     * cache_sweep()/make_room_for_new_entry() (protected by
     * cache_lock, since that is what the LRU list itself requires;
     * see the Sweep/Eviction Logic comment in kes_cache.c) -- but
     * has not yet locked `lock` below. Modified only via __atomic
     * builtins. try_evict_entry_locked() (kes_cache.c) will not
     * free an entry while lookup_pins is nonzero, checked while
     * holding *both* the bucket lock and cache_lock at once (the
     * two locks that separately protect the two kinds of pinner
     * above), so a pin taken under either one is guaranteed visible.
     * Every pinner releases its pin immediately after acquiring
     * `lock`. Without this, freeing an entry a concurrent lookup or
     * traversal has already found (but not yet locked) would be a
     * use-after-free the moment that caller proceeds to lock the
     * now-destroyed mutex. */
    uint32_t lookup_pins;

    /* Number of threads currently inside the
     * kes_cache_get_extent() wait-loop for this entry's
     * KES_EXTENT_LOADING flag to clear (src/kes_cache.c). Modified
     * only while `lock` is held, same as ref_count/pin_count/state.
     * This exists because pthread_cond_wait() internally unlocks
     * `lock` while parked, then re-locks it before returning -- so
     * a waiter does NOT continuously hold `lock` for the loop's
     * whole duration even though the code looks like it does, and
     * lookup_pins offers no protection here (the waiter already
     * locked `lock` once, well before this gap). Without
     * cond_waiters, an entry whose ref_count has genuinely dropped
     * to 0 could be freed (destroying `lock/cond`) while a
     * still-parked waiter is registered on that soon-to-be-invalid
     * condvar/mutex pair, which is undefined behavior the instant it
     * wakes and tries to re-lock. try_evict_entry_locked() refuses
     * to evict while cond_waiters != 0. */
    uint32_t cond_waiters;

    /* Hash table linkage */
    struct kes_extent_entry *hash_next;
    struct kes_extent_entry *hash_prev;

    /* LRU/LFU list linkage */
    struct kes_extent_entry *list_next;
    struct kes_extent_entry *list_prev;

    pthread_mutex_t lock;        /* Entry-specific lock */
    pthread_cond_t cond;         /* Condition variable */
};

/* Hash table bucket */
typedef struct {
    kes_extent_entry_t *head;    /* First entry in bucket */
    pthread_rwlock_t lock;       /* Bucket lock */
} kes_cache_bucket_t;

/* Cache structure (internal) */
struct kes_cache {
    kes_cache_config_t config;   /* Cache configuration */
    kes_cache_stats_t stats;     /* Runtime statistics */

    /* Hash table for fast lookup */
    kes_cache_bucket_t *buckets; /* Hash table buckets */
    uint32_t bucket_count;       /* Number of buckets */
    uint32_t bucket_mask;        /* Bucket mask for hashing */

    /* LRU/LFU lists */
    kes_extent_entry_t *mru_head; /* Most recently used */
    kes_extent_entry_t *lru_tail; /* Least recently used */

    /* Memory management */
    void *memory_pool;           /* Pre-allocated memory pool */
    size_t pool_size;            /* Size of memory pool */
    void *free_list;             /* Free entry list */

    /* Background thread management */
    pthread_t *bg_threads;       /* Background threads */
    bool shutdown;               /* Shutdown flag */
    pthread_mutex_t cache_lock;  /* Cache-wide lock */
    pthread_cond_t bg_cond;      /* Background thread condition */

    /* Number of kes_cache_get_extent() calls currently past
     * validation and still touching cache-wide bookkeeping (hash
     * table publish or LRU list touch) for their own lookup, not yet
     * released. Protected by cache_lock only. kes_cache_destroy()
     * requires this to be 0, in addition to an empty LRU list,
     * before it is safe to free cache->buckets/cache->cache_lock/
     * cache->bg_cond/cache itself -- ref_count/pin_count alone
     * cannot gate this window because a brand-new entry is published
     * into the hash table before it is ever added to the LRU list.
     * Never held across the actual read_extent() I/O call, only
     * across the shared-structure bookkeeping around it. */
    uint32_t inflight_lookups;

    /* Set (under cache_lock) by a kes_cache_destroy() call while it
     * attempts to drain the cache. If that attempt succeeds
     * (returns KES_SUCCESS), this stays true permanently -- the
     * cache is gone. If it instead finds entries still
     * referenced/pinned and returns KES_ERROR_BUSY, this is reset
     * to false before returning, since the cache remains fully
     * usable in that case. Distinct from `shutdown` above, which
     * kes_cache_start() clears again on a kes_cache_stop()/
     * kes_cache_start() restart cycle -- reusing `shutdown` here
     * would silently break that cycle. While true,
     * kes_cache_get_extent() rejects new lookups with
     * KES_ERROR_INVALID. Read and written only under cache_lock. */
    bool destroying;

    /* I/O callback functions */
    int (*read_extent)( void *device, const kes_extent_id_t *id,
                         void *buffer, size_t size);
    int (*write_extent)( void *device, const kes_extent_id_t *id,
                          const void *buffer, size_t size);
    int (*sync_device)( void *device);
};

/* =================================================================
 * Cache Lifecycle Functions
 * ================================================================= */

/**
 * Create a new cache instance. Returns NULL if config is invalid,
 * including config->policy requesting KES_CACHE_LFU or
 * KES_CACHE_CUSTOM (unimplemented -- see kes_cache_policy_t).
 * @param config Cache configuration
 * @return Cache handle or NULL on error
 */
kes_cache_t *kes_cache_create( const kes_cache_config_t *config);

/**
 * Destroy cache and free all resources. Safe to call concurrently
 * with kes_cache_get_extent()/kes_cache_put_extent()/etc. calls that
 * began before this call: if any cached entry is still referenced
 * (ref_count > 0) or pinned (pin_count > 0), or a
 * kes_cache_get_extent() call is still mid-lookup, this evicts
 * whatever entries it safely can and then returns KES_ERROR_BUSY,
 * leaving the cache object itself fully intact and usable -- callers
 * may retry once outstanding references are released via
 * kes_cache_put_extent()/kes_cache_unpin_extent(). A KES_ERROR_BUSY
 * return does NOT permanently reject new kes_cache_get_extent()
 * calls; only a call that returns KES_SUCCESS does that. It remains
 * undefined behavior to call any kes_cache_* function on this handle
 * after a call that returned KES_SUCCESS.
 * @param cache Cache handle
 * @return KES_SUCCESS, or KES_ERROR_BUSY if entries are still
 *         referenced/pinned or a lookup is still in flight
 */
int kes_cache_destroy( kes_cache_t *cache);

/**
 * Start background cache management threads. Each thread wakes
 * every config.sync_interval_ms (against CLOCK_MONOTONIC, immune to
 * wall-clock adjustments) and runs the same flush-then-evict sweep
 * kes_cache_sync() runs manually. Returns KES_ERROR_EXISTS if
 * threads are already running (call kes_cache_stop() first to
 * restart), KES_ERROR_INVALID if config.background_threads <= 0.
 * @param cache Cache handle
 * @return KES_SUCCESS or error code
 */
int kes_cache_start( kes_cache_t *cache);

/**
 * Stop background threads and prepare for shutdown
 * @param cache Cache handle
 * @return KES_SUCCESS or error code
 */
int kes_cache_stop( kes_cache_t *cache);

/* =================================================================
 * Extent Operations
 * ================================================================= */

/**
 * Get extent data (load from disk if not cached). Rejects
 * KES_ERROR_INVALID if id->block_size is not a power of 2 in
 * [KES_MIN_BLOCK_SIZE, KES_MAX_BLOCK_SIZE] (kes_types.h) -- there is
 * no block_size field on kes_cache_config_t to validate at
 * kes_cache_create() time, so this per-call id is where it is
 * checked instead. On a cache miss, evicts from the LRU tail as
 * needed to stay within config.max_entries/config.max_memory;
 * returns KES_ERROR_BUSY if eviction cannot free enough room (every
 * cached entry is currently referenced or pinned). Returns
 * KES_ERROR_INVALID if the cache is mid-kes_cache_destroy().
 * @param cache Cache handle
 * @param id Extent identifier
 * @param buffer Pointer to receive data buffer
 * @return KES_SUCCESS or error code
 */
int kes_cache_get_extent( kes_cache_t *cache,
                           const kes_extent_id_t *id,
                           void **buffer);

/**
 * Release extent reference (decrement ref count)
 * @param cache Cache handle
 * @param id Extent identifier
 * @return KES_SUCCESS or error code
 */
int kes_cache_put_extent( kes_cache_t *cache,
                           const kes_extent_id_t *id);

/**
 * Pin extent in memory (prevent eviction). Pinning is
 * reference-counted, not boolean: each call increments the entry's
 * internal pin count, and only the 0->1 transition marks the entry
 * pinned/unevictable and increments kes_cache_stats_t.entries_pinned
 * -- a second, third, etc. pin on an already-pinned entry still
 * returns KES_SUCCESS but does not double-count it in entries_pinned.
 * N pins require N matching kes_cache_unpin_extent() calls before the
 * entry becomes evictable again.
 * @param cache Cache handle
 * @param id Extent identifier
 * @return KES_SUCCESS or error code
 */
int kes_cache_pin_extent( kes_cache_t *cache,
                           const kes_extent_id_t *id);

/**
 * Unpin extent (allow eviction). Decrements the reference-counted
 * pin count kes_cache_pin_extent() maintains; only the ->0
 * transition clears the pinned state and decrements
 * kes_cache_stats_t.entries_pinned. Calling this more times than the
 * entry was pinned is a safe no-op -- the pin count is guarded
 * against underflow and an extra unpin still returns KES_SUCCESS
 * with no state change, it is not treated as an error.
 * @param cache Cache handle
 * @param id Extent identifier
 * @return KES_SUCCESS or error code
 */
int kes_cache_unpin_extent( kes_cache_t *cache,
                             const kes_extent_id_t *id);

/**
 * Mark extent as dirty (needs to be written to disk)
 * @param cache Cache handle
 * @param id Extent identifier
 * @return KES_SUCCESS or error code
 */
int kes_cache_mark_dirty( kes_cache_t *cache,
                           const kes_extent_id_t *id);

/**
 * Flush specific extent to disk
 * @param cache Cache handle
 * @param id Extent identifier
 * @return KES_SUCCESS or error code
 */
int kes_cache_flush_extent( kes_cache_t *cache,
                             const kes_extent_id_t *id);

/**
 * Flush all dirty extents to disk (sync operation), then free any
 * entry that is unreferenced and unpinned after its flush attempt.
 * Entries still referenced or pinned are flushed if dirty but never
 * freed -- this is a durability checkpoint, not a full eviction
 * pass. A clean no-op on an empty cache.
 * @param cache Cache handle
 * @return KES_SUCCESS or error code
 */
int kes_cache_sync( kes_cache_t *cache);

/**
 * Invalidate extent (remove from cache). WARNING: discards any
 * dirty data unconditionally, without writing it back -- this is
 * the difference between invalidate() (discard) and sync()/
 * flush_extent() (persist). Call flush_extent() first if the data
 * needs to survive. Returns KES_ERROR_NOTFOUND if the extent isn't
 * cached, KES_ERROR_BUSY if it is currently referenced or pinned
 * (ref_count > 0 || pin_count > 0).
 * @param cache Cache handle
 * @param id Extent identifier
 * @return KES_SUCCESS or error code
 */
int kes_cache_invalidate( kes_cache_t *cache,
                           const kes_extent_id_t *id);

/* =================================================================
 * Cache Management and Statistics
 * ================================================================= */

/**
 * Get current cache statistics
 * @param cache Cache handle
 * @param stats Statistics structure to fill
 * @return KES_SUCCESS or error code
 */
int kes_cache_get_stats( kes_cache_t *cache, kes_cache_stats_t *stats);

/**
 * Reset cache statistics. Resets only the cumulative counters
 * (hits, misses, evictions, flushes, bytes_read, bytes_written) to
 * zero. Does NOT reset the state counters (memory_used,
 * entries_cached, entries_dirty, entries_pinned) -- those describe
 * the cache's current contents, not history, and zeroing them would
 * desynchronize stats from reality until the next operation
 * happened to correct it.
 * @param cache Cache handle
 * @return KES_SUCCESS or error code
 */
int kes_cache_reset_stats( kes_cache_t *cache);

/**
 * Set I/O callback functions
 * @param cache Cache handle
 * @param read_func Function to read extents from storage
 * @param write_func Function to write extents to storage
 * @param sync_func Function to sync storage device
 * @return KES_SUCCESS or error code
 */
int kes_cache_set_io_callbacks( kes_cache_t *cache,
    int (*read_func)( void *device, const kes_extent_id_t *id,
                       void *buffer, size_t size),
    int (*write_func)( void *device, const kes_extent_id_t *id,
                        const void *buffer, size_t size),
    int (*sync_func)( void *device));

/* =================================================================
 * Utility Functions
 * ================================================================= */

/**
 * Calculate hash for extent identifier
 * @param id Extent identifier
 * @return Hash value
 */
uint32_t kes_extent_hash( const kes_extent_id_t *id);

/**
 * Compare two extent identifiers for equality
 * @param id1 First extent identifier
 * @param id2 Second extent identifier
 * @return true if equal, false otherwise
 */
bool kes_extent_equal( const kes_extent_id_t *id1,
                        const kes_extent_id_t *id2);

/**
 * Get default cache configuration for platform
 * @param config Configuration structure to fill
 * @param is_edge_device true for edge devices, false for servers
 */
void kes_cache_get_default_config( kes_cache_config_t *config,
                                    bool is_edge_device);

#ifdef __cplusplus
}
#endif

#endif /* KES_CACHE_H */
