/*
 * test_kes_cache.c - Test suite for KES Cache Library
 *
 * This file contains comprehensive tests for the KES cache system,
 * covering basic operations, thread safety, performance, and edge cases.
 *
 * Test categories:
 * - Basic cache operations (get, put, pin, unpin)
 * - Memory management and eviction
 * - Thread safety and concurrent access
 * - I/O callback functionality
 * - Configuration validation
 * - Performance benchmarks
 *
 * Copyright (C) 2025 KANEK Project
 */

#define _GNU_SOURCE  /* For usleep */

#include <kes/kes_cache.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>

#ifdef __APPLE__
/*
 * Darwin's pthread implementation has no pthread_barrier_t at all --
 * confirmed by a real build attempt on this repo's target machine
 * (this file's only use site is the race regression test below,
 * originally written assuming a glibc-style pthread that has one).
 * Minimal mutex+condvar+generation-counter shim, sufficient for that
 * single rendezvous-then-race use.
 */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned int count;
    unsigned int threshold;
    unsigned int generation;
} pthread_barrier_t;

#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

static int pthread_barrier_init( pthread_barrier_t *barrier,
                                  const void *attr,
                                  unsigned int count) {
    (void)attr;
    if ( count == 0) {
        return(EINVAL);
    }
    pthread_mutex_init( &barrier->mutex, NULL);
    pthread_cond_init( &barrier->cond, NULL);
    barrier->count = 0;
    barrier->threshold = count;
    barrier->generation = 0;
    return(0);
}

static int pthread_barrier_wait( pthread_barrier_t *barrier) {
    unsigned int my_generation;

    pthread_mutex_lock( &barrier->mutex);
    my_generation = barrier->generation;
    barrier->count++;
    if ( barrier->count == barrier->threshold) {
        barrier->generation++;
        barrier->count = 0;
        pthread_cond_broadcast( &barrier->cond);
        pthread_mutex_unlock( &barrier->mutex);
        return(PTHREAD_BARRIER_SERIAL_THREAD);
    }
    while ( my_generation == barrier->generation) {
        pthread_cond_wait( &barrier->cond, &barrier->mutex);
    }
    pthread_mutex_unlock( &barrier->mutex);
    return(0);
}

static int pthread_barrier_destroy( pthread_barrier_t *barrier) {
    pthread_mutex_destroy( &barrier->mutex);
    pthread_cond_destroy( &barrier->cond);
    return(0);
}
#endif /* __APPLE__ */

/* Test constants */
#define TEST_BLOCK_SIZE     8192
#define TEST_EXTENT_COUNT   100
#define TEST_NUM_THREADS    4
#define TEST_ITERATIONS     1000
#define RACE_TEST_THREADS   24
#define RACE_TEST_ROUNDS    60

/* Test data structure */
typedef struct {
    uint8_t pattern[TEST_BLOCK_SIZE];
    uint32_t checksum;
} test_data_t;

/* Global test variables */
static int g_test_failures = 0;

/* =================================================================
 * Test Utilities
 * ================================================================= */

/**
 * Calculate simple checksum for test data
 */
static uint32_t calc_checksum(const void* data, size_t size) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i++) {
        sum += bytes[i];
    }
    return sum;
}

/**
 * Generate test data pattern
 */
static void generate_test_data(test_data_t* data, uint32_t seed) {
    srand(seed);
    for (size_t i = 0; i < TEST_BLOCK_SIZE; i++) {
        data->pattern[i] = (uint8_t)(rand() & 0xFF);
    }
    data->checksum = calc_checksum(data->pattern, TEST_BLOCK_SIZE);
}

/**
 * Test assertion macro
 */
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            printf("FAIL: %s at %s:%d\n", message, __FILE__, __LINE__); \
            g_test_failures++; \
            return false; \
        } \
    } while (0)

/**
 * Test success macro
 */
#define TEST_PASS(test_name) \
    do { \
        printf("PASS: %s\n", test_name); \
        return true; \
    } while (0)

/* =================================================================
 * Mock I/O Functions
 * ================================================================= */

static test_data_t g_mock_storage[TEST_EXTENT_COUNT];
static bool g_mock_initialized = false;

/**
 * Mock read function
 */
static int mock_read_extent(void* device, const kes_extent_id_t* id,
                           void* buffer, size_t size) {
    (void)device; /* Unused */
    
    if (!g_mock_initialized) {
        /* Initialize mock storage on first read */
        for (int i = 0; i < TEST_EXTENT_COUNT; i++) {
            generate_test_data(&g_mock_storage[i], i + 1);
        }
        g_mock_initialized = true;
    }
    
    if (id->start_block >= TEST_EXTENT_COUNT || 
        id->block_count != 1 ||
        id->block_size != TEST_BLOCK_SIZE ||
        size != TEST_BLOCK_SIZE) {
        return -1;
    }
    
    memcpy(buffer, &g_mock_storage[id->start_block], size);
    
    /* Simulate I/O delay */
    usleep(100);
    
    return 0;
}

/**
 * Mock write function
 */
static int mock_write_extent(void* device, const kes_extent_id_t* id,
                            const void* buffer, size_t size) {
    (void)device; /* Unused */
    
    if (id->start_block >= TEST_EXTENT_COUNT ||
        id->block_count != 1 ||
        id->block_size != TEST_BLOCK_SIZE ||
        size != TEST_BLOCK_SIZE) {
        return -1;
    }
    
    memcpy(&g_mock_storage[id->start_block], buffer, size);
    
    /* Simulate I/O delay */
    usleep(200);
    
    return 0;
}

/**
 * Mock read function that always fails -- used to exercise the
 * ref_count leak fix (debugging_plan.md fix #6) on the cache-miss
 * load-failure path. Returns -100 rather than -1: since fix #1
 * unified KES_ERROR_* into kes_types.h, KES_ERROR_INVALID is -1, and
 * kes_cache_get_extent()'s miss path (fix #4) uses that exact value
 * to distinguish "no read_extent callback registered" from "callback
 * ran and failed" -- a real I/O-failure return of -1 from this mock
 * would be misclassified as the former.
 */
static int mock_read_extent_always_fail( void *device,
    const kes_extent_id_t *id, void *buffer, size_t size) {
    (void)device;
    (void)id;
    (void)buffer;
    (void)size;
    return( -100);
}

/*
 * Target id and hit counter used by
 * test_concurrent_miss_no_duplicate_entry() below to detect whether
 * more than one thread ever actually issued a real read_extent I/O
 * for the same raced extent id -- which is exactly what the P0 fix
 * (hash_find_or_insert(), src/kes_cache.c) prevents. g_race_read_count
 * is updated with __atomic builtins since it is written from
 * multiple threads with no other lock protecting it.
 */
static uint64_t g_race_target_block = (uint64_t)-1;
static unsigned int g_race_read_count = 0;

/**
 * Mock read function that wraps mock_read_extent() and additionally
 * counts calls for g_race_target_block.
 */
static int mock_read_extent_counting(void* device,
    const kes_extent_id_t* id, void* buffer, size_t size) {
    if (id->start_block == g_race_target_block) {
        __atomic_fetch_add(&g_race_read_count, 1, __ATOMIC_SEQ_CST);
    }
    return mock_read_extent(device, id, buffer, size);
}

/**
 * Mock sync function
 */
static int mock_sync_device(void* device) {
    (void)device; /* Unused */
    
    /* Simulate sync delay */
    usleep(1000);
    
    return 0;
}

/*
 * Validate a cached buffer against the mock storage's checksum.
 * Takes the raw buffer and the extent's start_block rather than
 * casting the buffer to test_data_t: the cache only ever allocates
 * exactly block_count * block_size bytes for an entry, which is
 * smaller than sizeof(test_data_t) (its trailing checksum field
 * would read 4 bytes past the end of that allocation). The expected
 * checksum is looked up from g_mock_storage instead.
 */
static bool
validate_test_data( const uint8_t *buffer, uint64_t start_block) {
    uint32_t checksum;

    checksum = calc_checksum( buffer, TEST_BLOCK_SIZE);
    return( checksum == g_mock_storage[start_block].checksum);
}

/* =================================================================
 * Test Cases
 * ================================================================= */

/**
 * Test cache creation and destruction
 */
static bool test_cache_lifecycle() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);
    
    /* Test cache creation */
    kes_cache_t* cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");
    
    /* Test I/O callback setup */
    int result = kes_cache_set_io_callbacks(cache, mock_read_extent,
                                           mock_write_extent, 
                                           mock_sync_device);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to set I/O callbacks");
    
    /* Test cache destruction */
    result = kes_cache_destroy(cache);
    TEST_ASSERT(result == KES_SUCCESS, "Cache destruction failed");
    
    TEST_PASS("Cache lifecycle");
}

/**
 * Test basic extent operations
 */
static bool test_basic_operations() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);
    
    kes_cache_t* cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");
    
    kes_cache_set_io_callbacks(cache, mock_read_extent,
                              mock_write_extent, mock_sync_device);
    
    /* Test extent get operation */
    kes_extent_id_t id = {
        .start_block = 0,
        .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };
    
    void* buffer = NULL;
    int result = kes_cache_get_extent(cache, &id, &buffer);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to get extent");
    TEST_ASSERT(buffer != NULL, "Buffer is NULL");
    
    /* Validate data */
    TEST_ASSERT(
        validate_test_data( (const uint8_t *)buffer, id.start_block),
        "Data validation failed");
    
    /* Test extent put operation */
    result = kes_cache_put_extent(cache, &id);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to put extent");
    
    /* Test cache statistics */
    kes_cache_stats_t stats;
    result = kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to get stats");
    TEST_ASSERT(stats.hits == 0, "Unexpected cache hits");
    TEST_ASSERT(stats.misses == 1, "Expected 1 cache miss");
    
    kes_cache_destroy(cache);
    TEST_PASS("Basic operations");
}

/**
 * Test cache hit scenario
 */
static bool test_cache_hit() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);
    
    kes_cache_t* cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");
    
    kes_cache_set_io_callbacks(cache, mock_read_extent,
                              mock_write_extent, mock_sync_device);
    
    kes_extent_id_t id = {
        .start_block = 1,
        .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };
    
    /* First access - should be a miss */
    void* buffer1 = NULL;
    int result = kes_cache_get_extent(cache, &id, &buffer1);
    TEST_ASSERT(result == KES_SUCCESS, "First get failed");
    
    kes_cache_put_extent(cache, &id);
    
    /* Second access - should be a hit */
    void* buffer2 = NULL;
    result = kes_cache_get_extent(cache, &id, &buffer2);
    TEST_ASSERT(result == KES_SUCCESS, "Second get failed");
    TEST_ASSERT(buffer1 == buffer2, "Buffers don't match");
    
    kes_cache_put_extent(cache, &id);
    
    /* Check statistics */
    kes_cache_stats_t stats;
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.hits == 1, "Expected 1 cache hit");
    TEST_ASSERT(stats.misses == 1, "Expected 1 cache miss");
    
    kes_cache_destroy(cache);
    TEST_PASS("Cache hit");
}

/**
 * Test extent pinning
 */
static bool test_extent_pinning() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);
    
    kes_cache_t* cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");
    
    kes_cache_set_io_callbacks(cache, mock_read_extent,
                              mock_write_extent, mock_sync_device);
    
    kes_extent_id_t id = {
        .start_block = 2,
        .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };
    
    /* Get extent and pin it */
    void* buffer = NULL;
    int result = kes_cache_get_extent(cache, &id, &buffer);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to get extent");
    
    result = kes_cache_pin_extent(cache, &id);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to pin extent");
    
    /* Check statistics show pinned extent */
    kes_cache_stats_t stats;
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.entries_pinned == 1, "Expected 1 pinned entry");
    
    /* Unpin extent */
    result = kes_cache_unpin_extent(cache, &id);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to unpin extent");
    
    kes_cache_put_extent(cache, &id);
    kes_cache_destroy(cache);
    TEST_PASS("Extent pinning");
}

/**
 * Test dirty extent handling
 */
static bool test_dirty_extents() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);
    
    kes_cache_t* cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");
    
    kes_cache_set_io_callbacks(cache, mock_read_extent,
                              mock_write_extent, mock_sync_device);
    
    kes_extent_id_t id = {
        .start_block = 3,
        .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };
    
    /* Get extent */
    void* buffer = NULL;
    int result = kes_cache_get_extent(cache, &id, &buffer);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to get extent");
    
    /* Mark as dirty */
    result = kes_cache_mark_dirty(cache, &id);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to mark dirty");
    
    /* Check statistics */
    kes_cache_stats_t stats;
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.entries_dirty == 1, "Expected 1 dirty entry");
    
    /* Flush extent */
    result = kes_cache_flush_extent(cache, &id);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to flush extent");
    
    kes_cache_put_extent(cache, &id);
    kes_cache_destroy(cache);
    TEST_PASS("Dirty extents");
}

/**
 * Test that a failed cache load releases its phantom ref_count
 * instead of leaking it (debugging_plan.md fix #6). ref_count isn't
 * exposed through the public stats API, so the most direct thing we
 * can assert publicly is indirect: request the same id a second time
 * and confirm it still cleanly returns an I/O error rather than
 * hanging, crashing, or behaving differently -- proving no
 * reference-count-related state corruption occurred on the first
 * failed load.
 */
static bool test_ref_count_leak_on_load_failure() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);

    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    kes_cache_set_io_callbacks(cache, mock_read_extent_always_fail,
                              mock_write_extent, mock_sync_device);

    kes_extent_id_t id = {
        .start_block = 4,
        .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };

    /* First get: the load fails, so this must return an I/O error
     * and leave *buffer NULL. */
    void *buffer = NULL;
    int result = kes_cache_get_extent(cache, &id, &buffer);
    TEST_ASSERT(result == KES_ERROR_IO, "Expected I/O error on load");

    /* No kes_cache_put_extent() call here on purpose -- the caller
     * never received a valid reference. Second get on the same id
     * must still cleanly return an error, not hang/crash/behave
     * differently due to a leaked ref_count. */
    void *buffer2 = NULL;
    result = kes_cache_get_extent(cache, &id, &buffer2);
    TEST_ASSERT(result == KES_ERROR_IO,
               "Second get on same id should still cleanly error");

    kes_cache_destroy(cache);
    TEST_PASS("Ref count leak on load failure");
}

/*
 * Test-only helper that looks up a cache entry's ref_count directly.
 *
 * Scope note (per debugging_plan.md fix #6's own guidance on this
 * exact situation): kes_cache_t and kes_extent_entry_t are fully
 * defined -- not just forward-declared -- in include/kes/kes_cache.h.
 * The "internal structure, opaque to users" comment on
 * kes_extent_entry_t there is aspirational; the compiler does not
 * enforce it, so any translation unit that includes the header
 * (including this test file) can already see every field. This
 * helper reimplements the same bucket lookup as kes_cache.c's
 * private static hash_find() using only that already-public struct
 * layout plus the already-public kes_extent_hash()/kes_extent_equal()
 * functions. It adds no new production code and changes no
 * visibility rules in src/kes_cache.c or include/kes/kes_cache.h --
 * the alternative named in the plan (a test-only introspection
 * function added to kes_cache.c under a guard macro) turned out to
 * be unnecessary once the struct layout was actually checked.
 */
static bool
get_entry_ref_count( kes_cache_t *cache, const kes_extent_id_t *id,
                      uint32_t *out_ref_count) {
    uint32_t hash = kes_extent_hash( id);
    uint32_t bucket_idx = hash & cache->bucket_mask;
    kes_extent_entry_t *entry = cache->buckets[bucket_idx].head;

    while ( entry != NULL) {
        if ( kes_extent_equal( &entry->id, id)) {
            *out_ref_count = entry->ref_count;
            return( true);
        }
        entry = entry->hash_next;
    }
    return( false);
}

/*
 * Test-only helper: counts how many entries in the cache's hash
 * table match the given extent id. Unlike get_entry_ref_count()
 * above, which stops at the first match, this walks the whole
 * bucket chain, so it is what actually catches the P0 duplicate-
 * insert bug -- two entries sharing the same id would each satisfy
 * hash_find()/get_entry_ref_count() individually, but this would
 * report 2 instead of 1.
 */
static uint32_t
count_entries_for_id( kes_cache_t *cache, const kes_extent_id_t *id) {
    uint32_t hash = kes_extent_hash( id);
    uint32_t bucket_idx = hash & cache->bucket_mask;
    kes_extent_entry_t *entry = cache->buckets[bucket_idx].head;
    uint32_t count = 0;

    while ( entry != NULL) {
        if ( kes_extent_equal( &entry->id, id)) {
            count++;
        }
        entry = entry->hash_next;
    }
    return( count);
}

/**
 * Test that a failed cache load actually releases ref_count back to
 * 0 (debugging_plan.md fix #6), not just that a second get on the
 * same id "behaves the same". test_ref_count_leak_on_load_failure()
 * above was found by an independent audit to be fully vacuous: with
 * fix #6 reverted, that test still passes 9/9, because the second
 * kes_cache_get_extent() call hits the "entry->state &
 * KES_EXTENT_ERROR" early-return in kes_cache_get_extent() before any
 * ref_count logic ever runs -- a leaked ref_count has zero observable
 * effect on that assertion. This test closes that gap by inspecting
 * entry->ref_count directly via get_entry_ref_count() above.
 */
static bool test_ref_count_actually_released_on_load_failure() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);

    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    kes_cache_set_io_callbacks(cache, mock_read_extent_always_fail,
                              mock_write_extent, mock_sync_device);

    kes_extent_id_t id = {
        .start_block = 41,
        .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };

    /* No kes_cache_put_extent() call anywhere in this test -- the
     * caller never received a valid buffer, so it holds no logical
     * reference and has no reason to call put_extent(). If fix #6's
     * decrement didn't run, ref_count would still read 1 below. */
    void *buffer = NULL;
    int result = kes_cache_get_extent(cache, &id, &buffer);
    TEST_ASSERT(result == KES_ERROR_IO, "Expected I/O error on load");

    uint32_t ref_count = 999; /* sentinel, must be overwritten */
    bool found = get_entry_ref_count(cache, &id, &ref_count);
    TEST_ASSERT(found, "Entry should still exist in the hash table "
               "after a failed load -- fix #6 releases the ref, it "
               "does not remove the entry (that needs "
               "kes_cache_invalidate(), Phase 3, out of scope here)");
    TEST_ASSERT(ref_count == 0,
               "ref_count must be released back to 0 after a failed "
               "load (debugging_plan.md fix #6)");

    kes_cache_destroy(cache);
    TEST_PASS("Ref count actually released on load failure");
}

/**
 * Test that a cache miss with no read_extent callback registered
 * returns KES_ERROR_INVALID rather than silently "succeeding" with
 * an uninitialized buffer (debugging_plan.md fix #4).
 */
static bool test_get_extent_no_read_callback() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);

    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    /* Deliberately do not call kes_cache_set_io_callbacks() at all --
     * cache->read_extent stays NULL. */

    kes_extent_id_t id = {
        .start_block = 5,
        .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };

    void *buffer = (void *)0x1; /* sentinel, must be cleared to NULL */
    int result = kes_cache_get_extent(cache, &id, &buffer);
    TEST_ASSERT(result == KES_ERROR_INVALID,
               "Expected KES_ERROR_INVALID with no read_extent callback");
    TEST_ASSERT(buffer == NULL, "Output buffer should be left NULL");

    kes_cache_destroy(cache);
    TEST_PASS("Get extent with no read callback");
}

/**
 * Test that flushing a dirty entry with no write_extent callback
 * registered returns KES_ERROR_INVALID rather than silently
 * "succeeding" while leaving the data unwritten and the dirty flag
 * still set (debugging_plan.md fix #5).
 */
static bool test_flush_extent_no_write_callback() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);

    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    /* A working read_extent is needed for the initial get_extent;
     * register it, but with write_func == NULL, per
     * kes_cache_set_io_callbacks()'s straightforward
     * assign-whatever-was-passed behavior (verified by reading
     * kes_cache_set_io_callbacks() in src/kes_cache.c -- it takes no
     * special action for a NULL write_func, so passing NULL here is
     * sufficient to leave cache->write_extent unset). */
    kes_cache_set_io_callbacks(cache, mock_read_extent, NULL,
                              mock_sync_device);

    kes_extent_id_t id = {
        .start_block = 6,
        .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };

    void *buffer = NULL;
    int result = kes_cache_get_extent(cache, &id, &buffer);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to get extent");

    result = kes_cache_mark_dirty(cache, &id);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to mark dirty");

    result = kes_cache_flush_extent(cache, &id);
    TEST_ASSERT(result == KES_ERROR_INVALID,
               "Expected KES_ERROR_INVALID with no write_extent callback");

    kes_cache_stats_t stats;
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.entries_dirty >= 1,
               "Dirty flag should not have been silently cleared");

    kes_cache_put_extent(cache, &id);
    kes_cache_destroy(cache);
    TEST_PASS("Flush extent with no write callback");
}

/**
 * Thread data for concurrent tests
 */
typedef struct {
    kes_cache_t* cache;
    int thread_id;
    int iterations;
    bool success;
} thread_data_t;

/**
 * Thread function for concurrent access test
 */
static void* concurrent_access_thread(void* arg) {
    thread_data_t* data = (thread_data_t*)arg;
    data->success = true;
    
    for (int i = 0; i < data->iterations; i++) {
        kes_extent_id_t id = {
            .start_block = (data->thread_id * 10 + i) % TEST_EXTENT_COUNT,
            .block_count = 1,
            .block_size = TEST_BLOCK_SIZE
        };
        
        void* buffer = NULL;
        int result = kes_cache_get_extent(data->cache, &id, &buffer);
        if (result != KES_SUCCESS) {
            data->success = false;
            break;
        }
        
        /* Validate data */
        if ( !validate_test_data( (const uint8_t *)buffer,
                                   id.start_block)) {
            data->success = false;
            break;
        }
        
        kes_cache_put_extent(data->cache, &id);
        
        /* Small delay to encourage interleaving */
        usleep(10);
    }
    
    return NULL;
}

/**
 * Test concurrent access
 */
static bool test_concurrent_access() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);
    
    kes_cache_t* cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");
    
    kes_cache_set_io_callbacks(cache, mock_read_extent,
                              mock_write_extent, mock_sync_device);
    
    /* Create threads */
    pthread_t threads[TEST_NUM_THREADS];
    thread_data_t thread_data[TEST_NUM_THREADS];
    
    for (int i = 0; i < TEST_NUM_THREADS; i++) {
        thread_data[i].cache = cache;
        thread_data[i].thread_id = i;
        thread_data[i].iterations = 50;
        thread_data[i].success = false;
        
        int result = pthread_create(&threads[i], NULL,
                                   concurrent_access_thread,
                                   &thread_data[i]);
        TEST_ASSERT(result == 0, "Failed to create thread");
    }
    
    /* Wait for threads to complete */
    for (int i = 0; i < TEST_NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
        TEST_ASSERT(thread_data[i].success, "Thread failed");
    }
    
    kes_cache_destroy(cache);
    TEST_PASS("Concurrent access");
}

/**
 * Thread data for the same-id race regression test below.
 */
typedef struct {
    kes_cache_t* cache;
    kes_extent_id_t id;
    pthread_barrier_t* barrier;
    void* buffer;
    int result;
} race_thread_data_t;

/**
 * Thread function for test_concurrent_miss_no_duplicate_entry():
 * waits at a barrier so every thread calls kes_cache_get_extent()
 * for the identical id as close to simultaneously as possible.
 */
static void* race_thread_func(void* arg) {
    race_thread_data_t* data = (race_thread_data_t*)arg;

    pthread_barrier_wait(data->barrier);

    data->result = kes_cache_get_extent(data->cache, &data->id,
                                       &data->buffer);
    return NULL;
}

/**
 * P0 regression test: concurrent cache misses on the *same* extent
 * id must not create duplicate hash-table entries.
 *
 * Before the src/kes_cache.c fix (hash_find_or_insert()),
 * kes_cache_get_extent()'s miss path called hash_find() and a
 * separate insert step as two independently-locked operations, with
 * no lock spanning both. Two threads racing a miss on the same id
 * could each build and insert their own kes_extent_entry_t, leaving
 * two distinct cache entries for one extent -- each issuing its own
 * read_extent I/O call (see PENDING_ITEMS.md, P0). This test drives
 * RACE_TEST_THREADS threads through a pthread_barrier so they all
 * call kes_cache_get_extent() for the identical id together, then
 * checks for exactly the symptoms that bug produced.
 *
 * A single barrier-synchronized race is not reliable enough to
 * catch this on its own: the pre-fix window between hash_find()
 * missing and hash_insert() publishing the new entry is narrow
 * (calloc + struct init + aligned_alloc, no I/O yet), so a lone
 * race attempt against the un-fixed code was observed to pass ~14
 * times out of 15 in local testing -- coin-flip odds, not a real
 * regression guard. This test instead runs RACE_TEST_ROUNDS
 * independent racing rounds against distinct extent ids in the same
 * cache and fails on the first round that shows a duplicate; with
 * RACE_TEST_THREADS=24 and RACE_TEST_ROUNDS=60 this reduces the
 * chance of a false PASS against the un-fixed code to a small
 * fraction of a percent.
 */
static bool test_concurrent_miss_no_duplicate_entry() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);

    kes_cache_t* cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    kes_cache_set_io_callbacks(cache, mock_read_extent_counting,
                              mock_write_extent, mock_sync_device);

    pthread_barrier_t barrier;
    TEST_ASSERT(
        pthread_barrier_init(&barrier, NULL, RACE_TEST_THREADS) == 0,
        "Failed to init barrier");

    for (int round = 0; round < RACE_TEST_ROUNDS; round++) {
        kes_extent_id_t id = {
            .start_block = (uint64_t)round,
            .block_count = 1,
            .block_size = TEST_BLOCK_SIZE
        };

        g_race_target_block = id.start_block;
        g_race_read_count = 0;

        pthread_t threads[RACE_TEST_THREADS];
        race_thread_data_t thread_data[RACE_TEST_THREADS];

        for (int i = 0; i < RACE_TEST_THREADS; i++) {
            thread_data[i].cache = cache;
            thread_data[i].id = id;
            thread_data[i].barrier = &barrier;
            thread_data[i].buffer = NULL;
            thread_data[i].result = -1;

            int rc = pthread_create(&threads[i], NULL, race_thread_func,
                                   &thread_data[i]);
            TEST_ASSERT(rc == 0, "Failed to create race thread");
        }

        for (int i = 0; i < RACE_TEST_THREADS; i++) {
            pthread_join(threads[i], NULL);
        }

        for (int i = 0; i < RACE_TEST_THREADS; i++) {
            TEST_ASSERT(thread_data[i].result == KES_SUCCESS,
                       "A racing get_extent call failed");
            TEST_ASSERT(thread_data[i].buffer != NULL,
                       "A racing get_extent call returned a NULL "
                       "buffer");
            TEST_ASSERT(thread_data[i].buffer == thread_data[0].buffer,
                       "Racing threads received different buffers -- "
                       "implies duplicate cache entries for the same "
                       "id");
        }

        uint32_t entry_count = count_entries_for_id(cache, &id);
        TEST_ASSERT(entry_count == 1,
                   "Expected exactly 1 hash-table entry for the "
                   "raced id");

        unsigned int read_count =
            __atomic_load_n(&g_race_read_count, __ATOMIC_SEQ_CST);
        TEST_ASSERT(read_count == 1,
                   "Expected exactly 1 read_extent call for the "
                   "raced id -- duplicate entries would each issue "
                   "their own I/O");

        TEST_ASSERT(
            validate_test_data( (const uint8_t *)thread_data[0].buffer,
                                 id.start_block),
            "Data validation failed");

        /* Release each thread's reference before the next round. */
        for (int i = 0; i < RACE_TEST_THREADS; i++) {
            kes_cache_put_extent(cache, &id);
        }
    }

    pthread_barrier_destroy(&barrier);

    kes_cache_stats_t stats;
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.entries_cached == RACE_TEST_ROUNDS,
               "Expected exactly 1 cache entry per round to have "
               "been created");

    kes_cache_destroy(cache);
    TEST_PASS("Concurrent miss on same extent creates only one entry");
}

/*
 * Phase 3 tests: kes_cache_sync(), kes_cache_invalidate(),
 * kes_cache_reset_stats(), kes_cache_start() -- PENDING_ITEMS.md
 * Phase 3 / KES_HARDENING_PLAN.md S4.
 */

/**
 * kes_cache_sync() must flush a dirty entry to the backing store and,
 * once the entry is unreferenced/unpinned after the flush attempt,
 * free it -- and must be a clean no-op on an already-empty cache.
 */
static bool test_cache_sync() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);

    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    kes_cache_set_io_callbacks(cache, mock_read_extent,
                              mock_write_extent, mock_sync_device);

    kes_extent_id_t id = {
        .start_block = 20,
        .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };

    void *buffer = NULL;
    int result = kes_cache_get_extent(cache, &id, &buffer);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to get extent");

    memset(buffer, 0xAB, TEST_BLOCK_SIZE);
    result = kes_cache_mark_dirty(cache, &id);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to mark dirty");

    result = kes_cache_put_extent(cache, &id);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to put extent");

    result = kes_cache_sync(cache);
    TEST_ASSERT(result == KES_SUCCESS, "sync() failed");

    kes_cache_stats_t stats;
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.flushes == 1, "Expected 1 flush from sync()");
    TEST_ASSERT(stats.entries_dirty == 0,
               "Expected 0 dirty entries after sync()");
    TEST_ASSERT(stats.entries_cached == 0,
               "Unreferenced/unpinned entry should have been freed "
               "by sync() after its flush");

    uint8_t expected[TEST_BLOCK_SIZE];
    memset(expected, 0xAB, TEST_BLOCK_SIZE);
    TEST_ASSERT(memcmp(g_mock_storage[20].pattern, expected,
                      TEST_BLOCK_SIZE) == 0,
               "Dirty data was not written back by sync()");

    /* sync() on an empty cache must be a clean no-op. */
    result = kes_cache_sync(cache);
    TEST_ASSERT(result == KES_SUCCESS,
               "sync() on an empty cache should succeed as a no-op");

    kes_cache_destroy(cache);
    TEST_PASS("Cache sync");
}

/**
 * kes_cache_sync() must flush a dirty entry that is still referenced,
 * but must NOT free it -- a durability checkpoint, not an eviction
 * pass, per KES_HARDENING_PLAN.md S4.1 point 3.
 */
static bool test_cache_sync_keeps_referenced_entries() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);

    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    kes_cache_set_io_callbacks(cache, mock_read_extent,
                              mock_write_extent, mock_sync_device);

    kes_extent_id_t id = {
        .start_block = 21,
        .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };

    void *buffer = NULL;
    int result = kes_cache_get_extent(cache, &id, &buffer);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to get extent");

    result = kes_cache_mark_dirty(cache, &id);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to mark dirty");

    /* Deliberately no put_extent() here -- entry stays referenced
     * (ref_count == 1) through the sync() call below. */
    result = kes_cache_sync(cache);
    TEST_ASSERT(result == KES_SUCCESS, "sync() failed");

    kes_cache_stats_t stats;
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.flushes == 1,
               "Referenced dirty entry should still be flushed");
    TEST_ASSERT(stats.entries_dirty == 0,
               "Expected 0 dirty entries after sync()");
    TEST_ASSERT(stats.entries_cached == 1,
               "Referenced entry must survive sync() -- not freed");

    kes_cache_put_extent(cache, &id);
    kes_cache_destroy(cache);
    TEST_PASS("Cache sync keeps referenced entries");
}

/**
 * kes_cache_invalidate() success/error-path coverage (S4.2):
 * NOTFOUND for an uncached id, BUSY while referenced, SUCCESS (and
 * entry actually removed) once unreferenced/unpinned.
 */
static bool test_cache_invalidate() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);

    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    kes_cache_set_io_callbacks(cache, mock_read_extent,
                              mock_write_extent, mock_sync_device);

    kes_extent_id_t missing_id = {
        .start_block = 998, .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };
    int result = kes_cache_invalidate(cache, &missing_id);
    TEST_ASSERT(result == KES_ERROR_NOTFOUND,
               "Expected NOTFOUND for an uncached extent");

    kes_extent_id_t busy_id = {
        .start_block = 22, .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };
    void *buffer = NULL;
    result = kes_cache_get_extent(cache, &busy_id, &buffer);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to get extent");

    /* Deliberately no put_extent() -- still referenced. */
    result = kes_cache_invalidate(cache, &busy_id);
    TEST_ASSERT(result == KES_ERROR_BUSY,
               "Expected BUSY while the extent is still referenced");
    kes_cache_put_extent(cache, &busy_id);

    kes_extent_id_t id = {
        .start_block = 23, .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };
    result = kes_cache_get_extent(cache, &id, &buffer);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to get extent");
    kes_cache_put_extent(cache, &id);

    result = kes_cache_invalidate(cache, &id);
    TEST_ASSERT(result == KES_SUCCESS,
               "Expected SUCCESS invalidating an unreferenced entry");

    kes_cache_stats_t stats;
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.entries_cached == 1,
               "Only busy_id should remain cached -- id was "
               "invalidated and freed, busy_id was only put(), "
               "never invalidated");

    kes_cache_destroy(cache);
    TEST_PASS("Cache invalidate");
}

/**
 * kes_cache_invalidate() must discard dirty data unconditionally,
 * WITHOUT writing it back -- the data-loss semantics S4.2 requires
 * to distinguish it from sync()/flush_extent().
 */
static bool test_cache_invalidate_discards_dirty_data() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);

    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    kes_cache_set_io_callbacks(cache, mock_read_extent,
                              mock_write_extent, mock_sync_device);

    kes_extent_id_t id = {
        .start_block = 24, .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };

    void *buffer = NULL;
    int result = kes_cache_get_extent(cache, &id, &buffer);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to get extent");

    memset(buffer, 0xCD, TEST_BLOCK_SIZE);
    result = kes_cache_mark_dirty(cache, &id);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to mark dirty");
    result = kes_cache_put_extent(cache, &id);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to put extent");

    kes_cache_stats_t before;
    kes_cache_get_stats(cache, &before);

    result = kes_cache_invalidate(cache, &id);
    TEST_ASSERT(result == KES_SUCCESS, "invalidate() failed");

    kes_cache_stats_t after;
    kes_cache_get_stats(cache, &after);
    TEST_ASSERT(after.flushes == before.flushes,
               "invalidate() must not flush dirty data");
    TEST_ASSERT(after.entries_dirty == 0,
               "entries_dirty should be decremented for the "
               "discarded entry");

    uint8_t not_expected[TEST_BLOCK_SIZE];
    memset(not_expected, 0xCD, TEST_BLOCK_SIZE);
    TEST_ASSERT(memcmp(g_mock_storage[24].pattern, not_expected,
                      TEST_BLOCK_SIZE) != 0,
               "invalidate() must discard dirty data, not persist it");

    kes_cache_destroy(cache);
    TEST_PASS("Cache invalidate discards dirty data without flush");
}

/**
 * kes_cache_reset_stats() must zero only the cumulative counters
 * (hits/misses/evictions/flushes/bytes_read/bytes_written) and
 * leave the state counters (memory_used/entries_cached/
 * entries_dirty/entries_pinned) unchanged (S4.3).
 */
static bool test_cache_reset_stats() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);

    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    kes_cache_set_io_callbacks(cache, mock_read_extent,
                              mock_write_extent, mock_sync_device);

    kes_extent_id_t id = {
        .start_block = 26, .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };

    void *buffer = NULL;
    int result = kes_cache_get_extent(cache, &id, &buffer);  /* miss */
    TEST_ASSERT(result == KES_SUCCESS, "Failed to get extent");
    kes_cache_put_extent(cache, &id);

    result = kes_cache_get_extent(cache, &id, &buffer);       /* hit */
    TEST_ASSERT(result == KES_SUCCESS, "Failed to get extent (hit)");

    kes_cache_mark_dirty(cache, &id);
    kes_cache_pin_extent(cache, &id);

    kes_cache_stats_t before;
    kes_cache_get_stats(cache, &before);
    TEST_ASSERT(before.hits >= 1 && before.misses >= 1 &&
               before.bytes_read > 0,
               "Sanity check: expected nonzero cumulative counters "
               "before reset");
    TEST_ASSERT(before.entries_cached == 1 && before.entries_dirty == 1
               && before.entries_pinned == 1 && before.memory_used > 0,
               "Sanity check: expected nonzero state counters before "
               "reset");

    result = kes_cache_reset_stats(cache);
    TEST_ASSERT(result == KES_SUCCESS, "reset_stats() failed");

    kes_cache_stats_t after;
    kes_cache_get_stats(cache, &after);
    TEST_ASSERT(after.hits == 0 && after.misses == 0 &&
               after.evictions == 0 && after.flushes == 0 &&
               after.bytes_read == 0 && after.bytes_written == 0,
               "Cumulative counters must be zero after reset_stats()");
    TEST_ASSERT(after.memory_used == before.memory_used &&
               after.entries_cached == before.entries_cached &&
               after.entries_dirty == before.entries_dirty &&
               after.entries_pinned == before.entries_pinned,
               "State counters must survive reset_stats() unchanged");

    kes_cache_unpin_extent(cache, &id);
    kes_cache_put_extent(cache, &id);
    kes_cache_destroy(cache);
    TEST_PASS("Cache reset_stats preserves state counters");
}

/**
 * kes_cache_start() must actually run automatic background
 * flushing -- the one behavior the whole background-thread design
 * exists to deliver (S3.4 acceptance criteria) -- and must reject a
 * second start() while already running, and reject
 * background_threads <= 0.
 */
static bool test_cache_start_background_flush() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);
    config.background_threads = 1;
    config.sync_interval_ms = 50;  /* fast, for test speed */

    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    kes_cache_set_io_callbacks(cache, mock_read_extent,
                              mock_write_extent, mock_sync_device);

    kes_extent_id_t id = {
        .start_block = 27, .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };

    void *buffer = NULL;
    int result = kes_cache_get_extent(cache, &id, &buffer);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to get extent");

    memset(buffer, 0xEF, TEST_BLOCK_SIZE);
    kes_cache_mark_dirty(cache, &id);
    kes_cache_put_extent(cache, &id);  /* ref 0 -- eligible for the
                                         * background sweep to flush
                                         * and free */

    result = kes_cache_start(cache);
    TEST_ASSERT(result == KES_SUCCESS,
               "Failed to start background thread");

    result = kes_cache_start(cache);
    TEST_ASSERT(result == KES_ERROR_EXISTS,
               "Expected EXISTS starting an already-running cache");

    /* No explicit sync()/flush_extent() call anywhere in this test
     * from here on -- only the background thread can flush this
     * entry. Sleep several sync intervals to avoid a timing-flaky
     * single-interval wait. */
    usleep(50000 * 6);  /* 300ms, 6x the 50ms interval */

    kes_cache_stats_t stats;
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.flushes >= 1,
               "Background thread should have flushed the dirty "
               "entry automatically");

    uint8_t expected[TEST_BLOCK_SIZE];
    memset(expected, 0xEF, TEST_BLOCK_SIZE);
    TEST_ASSERT(memcmp(g_mock_storage[27].pattern, expected,
                      TEST_BLOCK_SIZE) == 0,
               "Background flush should have written the dirty data "
               "to the mock backing store");

    result = kes_cache_stop(cache);
    TEST_ASSERT(result == KES_SUCCESS,
               "Failed to stop background thread");

    /* A second stop() (kes_cache_destroy() below issues one too)
     * must be safe/idempotent, not double-join. */
    result = kes_cache_stop(cache);
    TEST_ASSERT(result == KES_SUCCESS,
               "Second stop() call should be safe/idempotent");

    kes_cache_destroy(cache);
    TEST_PASS("Background thread automatically flushes dirty entries");
}

/**
 * kes_cache_start() must reject a nonsensical thread count instead
 * of silently doing nothing.
 */
static bool test_cache_start_rejects_zero_threads() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);
    config.background_threads = 0;

    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    int result = kes_cache_start(cache);
    TEST_ASSERT(result == KES_ERROR_INVALID,
               "Expected INVALID for background_threads <= 0");

    kes_cache_destroy(cache);
    TEST_PASS("Start rejects background_threads <= 0");
}

/*
 * Phase 4 tests: capacity enforcement / eviction --
 * PENDING_ITEMS.md Phase 4 / KES_HARDENING_PLAN.md S5.
 */

/**
 * Filling a small-max_entries cache with far more distinct,
 * unreferenced extents than it can hold must never let
 * stats.entries_cached exceed config.max_entries, and must actually
 * evict (S5.1's acceptance criteria).
 */
static bool test_cache_eviction_respects_max_entries() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);
    config.max_entries = KES_CACHE_MIN_ENTRIES;  /* 16 */

    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    kes_cache_set_io_callbacks(cache, mock_read_extent,
                              mock_write_extent, mock_sync_device);

    for (int i = 0; i < 40; i++) {
        kes_extent_id_t id = {
            .start_block = (uint64_t)i, .block_count = 1,
            .block_size = TEST_BLOCK_SIZE
        };
        void *buffer = NULL;
        int result = kes_cache_get_extent(cache, &id, &buffer);
        TEST_ASSERT(result == KES_SUCCESS,
                   "get_extent failed while filling cache past "
                   "max_entries");
        kes_cache_put_extent(cache, &id);

        kes_cache_stats_t stats;
        kes_cache_get_stats(cache, &stats);
        TEST_ASSERT(stats.entries_cached <= config.max_entries,
                   "entries_cached exceeded configured max_entries");
    }

    kes_cache_stats_t stats;
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.evictions > 0,
               "Expected evictions once max_entries was exceeded");

    kes_cache_destroy(cache);
    TEST_PASS("Eviction respects max_entries");
}

/**
 * Pinned entries must never be evicted, even when the cache is
 * repeatedly pushed well past max_entries by other traffic (S5.1
 * acceptance criteria, second test).
 */
static bool test_cache_pinned_entries_never_evicted() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);
    config.max_entries = KES_CACHE_MIN_ENTRIES;  /* 16 */

    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    kes_cache_set_io_callbacks(cache, mock_read_extent,
                              mock_write_extent, mock_sync_device);

    kes_extent_id_t pinned_ids[4];
    for (int i = 0; i < 4; i++) {
        pinned_ids[i].start_block = (uint64_t)i;
        pinned_ids[i].block_count = 1;
        pinned_ids[i].block_size = TEST_BLOCK_SIZE;

        void *buffer = NULL;
        int result = kes_cache_get_extent(cache, &pinned_ids[i],
                                         &buffer);
        TEST_ASSERT(result == KES_SUCCESS,
                   "get_extent failed for a to-be-pinned entry");
        result = kes_cache_pin_extent(cache, &pinned_ids[i]);
        TEST_ASSERT(result == KES_SUCCESS, "pin_extent failed");
        result = kes_cache_put_extent(cache, &pinned_ids[i]);
        TEST_ASSERT(result == KES_SUCCESS, "put_extent failed");
    }

    for (int i = 10; i < 50; i++) {
        kes_extent_id_t id = {
            .start_block = (uint64_t)i, .block_count = 1,
            .block_size = TEST_BLOCK_SIZE
        };
        void *buffer = NULL;
        int result = kes_cache_get_extent(cache, &id, &buffer);
        TEST_ASSERT(result == KES_SUCCESS,
                   "get_extent failed while pushing past max_entries");
        kes_cache_put_extent(cache, &id);
    }

    kes_cache_stats_t before;
    kes_cache_get_stats(cache, &before);

    for (int i = 0; i < 4; i++) {
        void *buffer = NULL;
        int result = kes_cache_get_extent(cache, &pinned_ids[i],
                                         &buffer);
        TEST_ASSERT(result == KES_SUCCESS,
                   "Pinned entry should still be gettable");
        TEST_ASSERT(
            validate_test_data( (const uint8_t *)buffer,
                                 pinned_ids[i].start_block),
            "Pinned entry data corrupted or reloaded incorrectly");
        kes_cache_put_extent(cache, &pinned_ids[i]);
        kes_cache_unpin_extent(cache, &pinned_ids[i]);
    }

    kes_cache_stats_t after;
    kes_cache_get_stats(cache, &after);
    TEST_ASSERT(after.hits == before.hits + 4,
               "All 4 pinned entries should have been cache hits, "
               "confirming none were evicted under capacity "
               "pressure");

    kes_cache_destroy(cache);
    TEST_PASS("Pinned entries are never evicted under capacity "
              "pressure");
}

/**
 * When the cache is full and every entry is pinned (so eviction can
 * free nothing), a further cache miss must return KES_ERROR_BUSY
 * rather than silently exceeding max_entries (S5.2).
 */
static bool test_cache_get_extent_busy_when_full_and_pinned() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);
    config.max_entries = KES_CACHE_MIN_ENTRIES;  /* 16 */

    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    kes_cache_set_io_callbacks(cache, mock_read_extent,
                              mock_write_extent, mock_sync_device);

    kes_extent_id_t ids[KES_CACHE_MIN_ENTRIES];
    for (uint32_t i = 0; i < KES_CACHE_MIN_ENTRIES; i++) {
        ids[i].start_block = (uint64_t)i;
        ids[i].block_count = 1;
        ids[i].block_size = TEST_BLOCK_SIZE;

        void *buffer = NULL;
        int result = kes_cache_get_extent(cache, &ids[i], &buffer);
        TEST_ASSERT(result == KES_SUCCESS,
                   "Failed to fill cache to max_entries");
        result = kes_cache_pin_extent(cache, &ids[i]);
        TEST_ASSERT(result == KES_SUCCESS, "pin_extent failed");
        kes_cache_put_extent(cache, &ids[i]);
    }

    kes_extent_id_t overflow_id = {
        .start_block = 90, .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };
    void *buffer = NULL;
    int result = kes_cache_get_extent(cache, &overflow_id, &buffer);
    TEST_ASSERT(result == KES_ERROR_BUSY,
               "Expected BUSY when the cache is full of pinned "
               "entries and cannot evict enough room");
    TEST_ASSERT(buffer == NULL, "Output buffer should stay NULL");

    for (uint32_t i = 0; i < KES_CACHE_MIN_ENTRIES; i++) {
        kes_cache_unpin_extent(cache, &ids[i]);
    }

    kes_cache_destroy(cache);
    TEST_PASS("get_extent returns BUSY when full and cannot evict");
}

/**
 * kes_cache_create() must reject KES_CACHE_LFU and KES_CACHE_CUSTOM
 * (unimplemented policies, S5.3) rather than silently behaving as
 * if KES_CACHE_LRU had been requested, and must still accept
 * KES_CACHE_LRU.
 */
static bool test_cache_create_rejects_unimplemented_policy() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);

    config.policy = KES_CACHE_LFU;
    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache == NULL,
               "kes_cache_create() should reject KES_CACHE_LFU");

    config.policy = KES_CACHE_CUSTOM;
    cache = kes_cache_create(&config);
    TEST_ASSERT(cache == NULL,
               "kes_cache_create() should reject KES_CACHE_CUSTOM");

    config.policy = KES_CACHE_LRU;
    cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL,
               "kes_cache_create() should still accept KES_CACHE_LRU");
    kes_cache_destroy(cache);

    TEST_PASS("create() rejects unimplemented eviction policies");
}

/*
 * S6.C: the highest-value concurrency test in
 * KES_HARDENING_PLAN.md -- one thread calling sync() repeatedly
 * while other threads continuously get/put a small, overlapping
 * pool of extent ids, run under a small max_entries so real
 * eviction pressure (not just flushing) is exercised too. This is
 * exactly the interleaving that would expose a use-after-free if
 * the lookup_pins-protected eviction path (src/kes_cache.c) has a
 * bug -- run under ASan/TSan, not just the normal build.
 */
typedef struct {
    kes_cache_t *cache;
    kes_extent_id_t *ids;
    int num_ids;
    int iterations;
    bool success;
} mixed_thread_data_t;

static void *mixed_get_put_thread(void *arg) {
    mixed_thread_data_t *data = (mixed_thread_data_t*)arg;
    data->success = true;

    for (int i = 0; i < data->iterations; i++) {
        kes_extent_id_t *id = &data->ids[i % data->num_ids];
        void *buffer = NULL;
        int result = kes_cache_get_extent(data->cache, id, &buffer);

        if (result != KES_SUCCESS && result != KES_ERROR_BUSY) {
            data->success = false;
            break;
        }
        if (result == KES_SUCCESS) {
            kes_cache_put_extent(data->cache, id);
        }
    }
    return NULL;
}

static void *mixed_sync_thread(void *arg) {
    mixed_thread_data_t *data = (mixed_thread_data_t*)arg;

    for (int i = 0; i < data->iterations; i++) {
        kes_cache_sync(data->cache);
    }
    return NULL;
}

static bool test_concurrent_sync_vs_get_put() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);
    config.max_entries = KES_CACHE_MIN_ENTRIES;  /* force real
                                                    * eviction
                                                    * pressure, not
                                                    * just flushing */

    kes_cache_t *cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    kes_cache_set_io_callbacks(cache, mock_read_extent,
                              mock_write_extent, mock_sync_device);

    kes_extent_id_t ids[8];
    for (int i = 0; i < 8; i++) {
        ids[i].start_block = (uint64_t)(60 + i);
        ids[i].block_count = 1;
        ids[i].block_size = TEST_BLOCK_SIZE;
    }

    pthread_t get_put_threads[4];
    mixed_thread_data_t get_put_data[4];
    pthread_t sync_threads[2];
    mixed_thread_data_t sync_data[2];

    for (int i = 0; i < 4; i++) {
        get_put_data[i].cache = cache;
        get_put_data[i].ids = ids;
        get_put_data[i].num_ids = 8;
        get_put_data[i].iterations = 200;
        get_put_data[i].success = false;

        int rc = pthread_create(&get_put_threads[i], NULL,
                                mixed_get_put_thread,
                                &get_put_data[i]);
        TEST_ASSERT(rc == 0, "Failed to create get/put thread");
    }

    for (int i = 0; i < 2; i++) {
        sync_data[i].cache = cache;
        sync_data[i].iterations = 100;

        int rc = pthread_create(&sync_threads[i], NULL,
                                mixed_sync_thread, &sync_data[i]);
        TEST_ASSERT(rc == 0, "Failed to create sync thread");
    }

    for (int i = 0; i < 4; i++) {
        pthread_join(get_put_threads[i], NULL);
        TEST_ASSERT(get_put_data[i].success,
                   "A get/put thread failed unexpectedly");
    }
    for (int i = 0; i < 2; i++) {
        pthread_join(sync_threads[i], NULL);
    }

    kes_cache_destroy(cache);
    TEST_PASS("Concurrent sync() vs get/put on overlapping ids");
}

/*
 * Thread body for the destroy-vs-concurrent-access race test below:
 * a tight get_extent()/put_extent() loop against a small, fixed
 * pool of extent ids. There is no join/stop coordination with the
 * caller -- the enclosing process (a forked child, see
 * test_cache_destroy_races_concurrent_access()) is disposable and
 * simply ends, normally or via a sanitizer/signal, once the race is
 * over.
 */
static void *destroy_race_thread( void *arg) {
    kes_cache_t *cache = ( kes_cache_t *)arg;

    for ( int i = 0; i < 500000; i++) {
        kes_extent_id_t id = {
            .start_block = (uint64_t)(i % 8),
            .block_count = 1,
            .block_size = TEST_BLOCK_SIZE
        };
        void *buffer = NULL;
        int rc = kes_cache_get_extent( cache, &id, &buffer);

        if ( rc == KES_SUCCESS) {
            kes_cache_put_extent( cache, &id);
        }
    }
    return( NULL);
}

/*
 * Number of concurrent racer threads
 * run_destroy_race_child() starts before calling
 * kes_cache_destroy() -- more than one increases the odds that at
 * least one is genuinely mid get_extent()/put_extent() (inside a
 * lock, or between hash_find() and locking entry->lock) at the
 * exact moment destroy() runs, versus a single thread that might
 * happen to be between iterations.
 */
#define DESTROY_RACE_THREAD_COUNT 4

/*
 * Child-process body for
 * test_cache_destroy_races_concurrent_access(): creates its own
 * cache, starts DESTROY_RACE_THREAD_COUNT destroy_race_thread()
 * instances, gives them a short head start, then calls
 * kes_cache_destroy() while they may still be mid
 * get_extent()/put_extent(). kes_cache_destroy()'s return value is
 * deliberately ignored here: with the fix (see the big comment
 * above test_cache_destroy_races_concurrent_access() below),
 * KES_ERROR_BUSY is a legitimate, expected outcome if any racer
 * thread still holds a reference at the moment destroy() runs --
 * given only a 2ms head start and a tight loop, this is likely on
 * most runs. Either return value proves the same thing: no
 * use-after-free occurred. Any still-running racer threads (whether
 * destroy() succeeded or returned BUSY) are simply torn down by the
 * OS along with the rest of the process, which is fine since this
 * function only ever runs inside a disposable forked child.
 */
static void run_destroy_race_child( void) {
    kes_cache_config_t config;
    kes_cache_get_default_config( &config, false);

    kes_cache_t *cache = kes_cache_create( &config);
    if ( cache == NULL) {
        _exit(2);
    }
    kes_cache_set_io_callbacks( cache, mock_read_extent,
                                mock_write_extent, mock_sync_device);

    pthread_t racers[DESTROY_RACE_THREAD_COUNT];
    for ( int i = 0; i < DESTROY_RACE_THREAD_COUNT; i++) {
        int rc = pthread_create( &racers[i], NULL,
                                 destroy_race_thread, cache);
        if ( rc != 0) {
            _exit(3);
        }
    }

    /* Short head start so the racer threads are very likely mid
     * get_extent()/put_extent() -- not just started -- by the time
     * kes_cache_destroy() runs below. */
    usleep(2000);

    kes_cache_destroy( cache);

    _exit(0);
}

/*
 * kes_cache_destroy() racing a concurrent get_extent()/put_extent()
 * thread (KES_HARDENING_PLAN.md S6.C / plan_phase5.md A.3.1).
 *
 * This used to be a genuine, confirmed heap-use-after-free: a
 * standalone, minimal repro of exactly this scenario (one thread in
 * a tight get_extent()/put_extent() loop, kes_cache_destroy() called
 * from another thread shortly after) reproduced under ASan
 * (heap-use-after-free: lru_remove(), src/kes_cache.c:125, reading a
 * kes_extent_entry_t already freed by kes_cache_destroy()) and
 * independently under TSan (multiple data races on the freed
 * entry's lock/fields, plus "heap-use-after-free" and "use of an
 * invalid mutex" reports). Root cause: kes_cache_destroy() walked
 * the LRU list under cache_lock, freeing each entry's data buffer,
 * destroying entry->lock/entry->cond, and free()ing the entry
 * struct -- without ever acquiring entry->lock, and without
 * checking ref_count/pin_count first.
 *
 * Fixed (see the "Track A.3.1" Resolved entry in PENDING_ITEMS.md
 * for the full design/citation trail): kes_cache_destroy() now
 * walks the LRU list the same lookup-pin-protected way cache_sweep()
 * does, and evicts each entry via try_evict_entry_locked() -- the
 * same ref_count/pin_count/cond_waiters/lookup_pins eligibility
 * check every other evictor in this file already uses -- instead of
 * freeing unconditionally. A new cache-wide inflight_lookups counter
 * (incremented/decremented under cache_lock by
 * kes_cache_get_extent()) closes the remaining gap ref_count alone
 * can't cover: a brand-new entry is published into the hash table
 * before it is ever added to the LRU list, so a destroy() call
 * observing an empty LRU list at exactly that moment could otherwise
 * free cache->buckets/cache->cache_lock/cache itself out from under
 * a lookup that is still about to touch them. If anything is still
 * referenced, pinned, waited-on, or mid-lookup, destroy() evicts
 * whatever it safely can and returns KES_ERROR_BUSY, leaving the
 * cache object itself fully intact and usable -- it does not block,
 * and it does not leave the cache half torn down.
 *
 * This test's job now is to prove the fix, not just document the
 * bug: the automated race below (four racer threads inside a forked
 * child, see run_destroy_race_child()) is asserted to always exit
 * cleanly, under both plain and sanitized (ASan/TSan) builds --
 * where it used to reliably trip TSan's race detector (nonzero exit
 * status 66) and occasionally ASan's use-after-free detector. Like
 * test_cross_process_racing_io() in tests/test_kes_multiprocess.c,
 * the race itself still runs inside a forked child process (rather
 * than this test process directly) purely so a *regression* here --
 * if this fix is ever weakened -- fails loudly via this test's
 * assertion instead of nondeterministically crashing the whole test
 * binary and taking every other test down with it.
 */
static bool test_cache_destroy_races_concurrent_access( void) {
    pid_t pid = fork();
    TEST_ASSERT(pid >= 0, "fork() failed");

    if ( pid == 0) {
        run_destroy_race_child();
        /* run_destroy_race_child() always _exit()s; unreachable. */
        _exit(127);
    }

    int status = 0;
    pid_t waited = waitpid( pid, &status, 0);
    TEST_ASSERT(waited == pid,
               "waitpid() did not return the destroy-race child");

    if ( WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf( "  destroy-race child exited normally with status "
                "0 -- no use-after-free, no sanitizer report\n");
    } else if ( WIFEXITED(status)) {
        /* A sanitizer detects a real problem and reports it, then
         * calls its own exit() with a nonzero status (ASan: 1 by
         * default, TSan: 66 by default) rather than raising a
         * signal -- this branch, not WIFSIGNALED below, is what
         * would fire under make asan/make tsan if this fix
         * regressed. */
        printf( "  destroy-race child exited with nonzero status "
                "%d -- a sanitizer caught a problem, see this "
                "test's comment and PENDING_ITEMS.md\n",
                WEXITSTATUS(status));
    } else if ( WIFSIGNALED(status)) {
        printf( "  destroy-race child was killed by signal %d "
                "(%s) -- see this test's comment and "
                "PENDING_ITEMS.md\n",
                WTERMSIG(status), strsignal( WTERMSIG(status)));
    } else {
        printf( "  destroy-race child ended with unexpected wait "
                "status 0x%x\n", status);
    }

    TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
               "destroy-race child did not exit cleanly (status "
               "indicates a crash or sanitizer-caught race -- "
               "kes_cache_destroy()'s concurrent-access fix may "
               "have regressed)");

    TEST_PASS("Cache Destroy Races Concurrent Get/Put (verifies "
              "the kes_cache_destroy() UAF fix -- see "
              "PENDING_ITEMS.md)");
}

/*
 * Non-concurrent companion to the race test above: proves the new
 * KES_ERROR_BUSY contract on a single thread, no forking or
 * sanitizer needed. An entry that is still referenced
 * (kes_cache_get_extent() called, never put back) must make
 * kes_cache_destroy() refuse rather than free it out from under the
 * caller -- and the cache must stay fully usable afterward, not
 * permanently wedged by the failed attempt.
 */
static bool test_destroy_busy_when_referenced( void) {
    kes_cache_config_t config;
    kes_cache_get_default_config( &config, false);

    kes_cache_t *cache = kes_cache_create( &config);
    TEST_ASSERT(cache != NULL, "kes_cache_create() failed");

    kes_cache_set_io_callbacks( cache, mock_read_extent,
                                 mock_write_extent, mock_sync_device);

    kes_extent_id_t id = {
        .start_block = 0,
        .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };
    void *buffer = NULL;
    int rc = kes_cache_get_extent( cache, &id, &buffer);
    TEST_ASSERT(rc == KES_SUCCESS, "get_extent() failed");
    TEST_ASSERT(buffer != NULL, "get_extent() returned NULL buffer");

    /* Entry is still referenced (never put back) -- destroy() must
     * refuse, leaving the cache fully intact and usable. */
    rc = kes_cache_destroy( cache);
    TEST_ASSERT(rc == KES_ERROR_BUSY,
               "destroy() should return BUSY while referenced");

    /* Cache must still be fully usable after the BUSY return --
     * prove it by releasing the reference and destroying again. */
    rc = kes_cache_put_extent( cache, &id);
    TEST_ASSERT(rc == KES_SUCCESS, "put_extent() failed");

    rc = kes_cache_destroy( cache);
    TEST_ASSERT(rc == KES_SUCCESS,
               "destroy() should succeed once unreferenced");

    TEST_PASS("Destroy returns BUSY while referenced, cache stays "
              "usable, succeeds once released");
}

/* =================================================================
 * Test Runner
 * ================================================================= */

/**
 * Test function type
 */
typedef bool (*test_func_t)(void);

/**
 * Test case structure
 */
typedef struct {
    const char* name;
    test_func_t func;
} test_case_t;

/**
 * Test suite
 */
static test_case_t test_suite[] = {
    { "Cache Lifecycle", test_cache_lifecycle },
    { "Basic Operations", test_basic_operations },
    { "Cache Hit", test_cache_hit },
    { "Extent Pinning", test_extent_pinning },
    { "Dirty Extents", test_dirty_extents },
    { "Ref Count Leak On Load Failure",
      test_ref_count_leak_on_load_failure },
    { "Ref Count Actually Released On Load Failure",
      test_ref_count_actually_released_on_load_failure },
    { "Get Extent With No Read Callback",
      test_get_extent_no_read_callback },
    { "Flush Extent With No Write Callback",
      test_flush_extent_no_write_callback },
    { "Concurrent Access", test_concurrent_access },
    { "Concurrent Miss No Duplicate Entry",
      test_concurrent_miss_no_duplicate_entry },
    { "Cache Sync", test_cache_sync },
    { "Cache Sync Keeps Referenced Entries",
      test_cache_sync_keeps_referenced_entries },
    { "Cache Invalidate", test_cache_invalidate },
    { "Cache Invalidate Discards Dirty Data",
      test_cache_invalidate_discards_dirty_data },
    { "Cache Reset Stats", test_cache_reset_stats },
    { "Cache Start Background Flush",
      test_cache_start_background_flush },
    { "Cache Start Rejects Zero Threads",
      test_cache_start_rejects_zero_threads },
    { "Cache Eviction Respects Max Entries",
      test_cache_eviction_respects_max_entries },
    { "Cache Pinned Entries Never Evicted",
      test_cache_pinned_entries_never_evicted },
    { "Cache Get Extent Busy When Full And Pinned",
      test_cache_get_extent_busy_when_full_and_pinned },
    { "Cache Create Rejects Unimplemented Policy",
      test_cache_create_rejects_unimplemented_policy },
    { "Concurrent Sync vs Get/Put",
      test_concurrent_sync_vs_get_put },
    { "Cache Destroy Races Concurrent Access",
      test_cache_destroy_races_concurrent_access },
    { "Destroy Returns Busy When Referenced",
      test_destroy_busy_when_referenced },
    { NULL, NULL } /* Terminator */
};

/**
 * Main test runner
 */
int main(void) {
    printf("=== KES Cache Test Suite ===\n\n");
    
    int tests_run = 0;
    int tests_passed = 0;
    
    for (test_case_t* test = test_suite; test->name != NULL; test++) {
        printf("Running: %s...\n", test->name);
        
        bool result = test->func();
        tests_run++;
        
        if (result) {
            tests_passed++;
        }
        
        printf("\n");
    }
    
    printf("=== Test Results ===\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_run - tests_passed);
    printf("Assertion failures: %d\n", g_test_failures);
    
    if (tests_passed == tests_run && g_test_failures == 0) {
        printf("\nAll tests PASSED!\n");
        return 0;
    } else {
        printf("\nSome tests FAILED!\n");
        return 1;
    }
}
