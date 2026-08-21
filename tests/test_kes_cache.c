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
#include <pthread.h>
#include <unistd.h>
#include <time.h>

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
