#ifndef KES_CACHE_H
#define KES_CACHE_H

#define _GNU_SOURCE  /* For aligned_alloc, clock_gettime */

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

/* Return codes */
#define KES_SUCCESS          0
#define KES_ERROR_NOMEM     -1
#define KES_ERROR_NOTFOUND  -2
#define KES_ERROR_INVALID   -3
#define KES_ERROR_IO        -4
#define KES_ERROR_BUSY      -5

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

/* Cache eviction policies */
typedef enum {
    KES_CACHE_LRU = 0,           /* Least Recently Used */
    KES_CACHE_LFU,               /* Least Frequently Used */
    KES_CACHE_CUSTOM             /* Custom policy */
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
 * Create a new cache instance
 * @param config Cache configuration
 * @return Cache handle or NULL on error
 */
kes_cache_t *kes_cache_create( const kes_cache_config_t *config);

/**
 * Destroy cache and free all resources
 * @param cache Cache handle
 * @return KES_SUCCESS or error code
 */
int kes_cache_destroy( kes_cache_t *cache);

/**
 * Start background cache management threads
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
 * Get extent data (load from disk if not cached)
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
 * Pin extent in memory (prevent eviction)
 * @param cache Cache handle
 * @param id Extent identifier
 * @return KES_SUCCESS or error code
 */
int kes_cache_pin_extent( kes_cache_t *cache,
                           const kes_extent_id_t *id);

/**
 * Unpin extent (allow eviction)
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
 * Flush all dirty extents to disk (sync operation)
 * @param cache Cache handle
 * @return KES_SUCCESS or error code
 */
int kes_cache_sync( kes_cache_t *cache);

/**
 * Invalidate extent (remove from cache)
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
 * Reset cache statistics
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
