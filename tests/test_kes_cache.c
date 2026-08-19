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
static int mock_read_extent_always_fail(void* device,
                                       const kes_extent_id_t* id,
                                       void* buffer, size_t size) {
    (void)device;
    (void)id;
    (void)buffer;
    (void)size;
    return -100;
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

    kes_cache_t* cache = kes_cache_create(&config);
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
    void* buffer = NULL;
    int result = kes_cache_get_extent(cache, &id, &buffer);
    TEST_ASSERT(result == KES_ERROR_IO, "Expected I/O error on load");

    /* No kes_cache_put_extent() call here on purpose -- the caller
     * never received a valid reference. Second get on the same id
     * must still cleanly return an error, not hang/crash/behave
     * differently due to a leaked ref_count. */
    void* buffer2 = NULL;
    result = kes_cache_get_extent(cache, &id, &buffer2);
    TEST_ASSERT(result == KES_ERROR_IO,
               "Second get on same id should still cleanly error");

    kes_cache_destroy(cache);
    TEST_PASS("Ref count leak on load failure");
}

/**
 * Test that a cache miss with no read_extent callback registered
 * returns KES_ERROR_INVALID rather than silently "succeeding" with
 * an uninitialized buffer (debugging_plan.md fix #4).
 */
static bool test_get_extent_no_read_callback() {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false);

    kes_cache_t* cache = kes_cache_create(&config);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    /* Deliberately do not call kes_cache_set_io_callbacks() at all --
     * cache->read_extent stays NULL. */

    kes_extent_id_t id = {
        .start_block = 5,
        .block_count = 1,
        .block_size = TEST_BLOCK_SIZE
    };

    void* buffer = (void*)0x1; /* sentinel, must be cleared to NULL */
    int result = kes_cache_get_extent(cache, &id, &buffer);
    TEST_ASSERT(result == KES_ERROR_INVALID,
               "Expected KES_ERROR_INVALID with no read_extent callback");
    TEST_ASSERT(buffer == NULL, "Output buffer should be left NULL");

    kes_cache_destroy(cache);
    TEST_PASS("Get extent with no read callback");
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
    { "Get Extent With No Read Callback",
      test_get_extent_no_read_callback },
    { "Concurrent Access", test_concurrent_access },
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
