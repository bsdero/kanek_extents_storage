/*
 * test_kes_minimal.c - Test Suite for Minimal KES Implementation
 *
 * This file contains tests for the minimal KES implementation,
 * focusing on core storage and bitmap functionality.
 *
 * Copyright (C) 2025 KANEK Project
 */

#define _GNU_SOURCE  /* For ftruncate */

#include <kes/kes_storage.h>
#include <kes/kes_bitmap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Test framework macros */
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            printf("FAIL: %s at %s:%d\n", message, __FILE__, __LINE__); \
            return false; \
        } \
    } while (0)

#define TEST_SUCCESS(test_name) \
    do { \
        printf("PASS: %s\n", test_name); \
        return true; \
    } while (0)

/* Test constants */
#define TEST_STORAGE_FILE "/tmp/kes_test_storage"
#define TEST_STORAGE_SIZE (10 * 1024 * 1024)  /* 10MB */

/* Global test statistics */
static int tests_run = 0;
static int tests_passed = 0;

/* Test utility functions */
static void cleanup_test_files(void) {
    unlink(TEST_STORAGE_FILE);
}

/* =================================================================
 * Bitmap Tests
 * ================================================================= */

static bool test_bitmap_creation(void) {
    kes_bitmap_t* bitmap;
    
    /* Test normal creation */
    int result = kes_bitmap_create(1000, &bitmap);
    TEST_ASSERT(result == KES_SUCCESS, "Bitmap creation failed");
    TEST_ASSERT(bitmap != NULL, "Bitmap is NULL");
    
    /* Verify initial state */
    uint64_t total, free, used;
    kes_bitmap_get_stats(bitmap, &total, &free, &used);
    TEST_ASSERT(total == 1000, "Incorrect total bits");
    TEST_ASSERT(free == 1000, "Incorrect free bits");
    TEST_ASSERT(used == 0, "Incorrect used bits");
    
    kes_bitmap_destroy(bitmap);
    
    /* Test invalid parameters */
    result = kes_bitmap_create(0, &bitmap);
    TEST_ASSERT(result == KES_ERROR_INVALID, "Should fail with 0 bits");
    
    result = kes_bitmap_create(1000, NULL);
    TEST_ASSERT(result == KES_ERROR_INVALID, "Should fail with NULL pointer");
    
    TEST_SUCCESS("Bitmap creation");
}

static bool test_bitmap_basic_operations(void) {
    kes_bitmap_t* bitmap;
    kes_bitmap_create(100, &bitmap);
    
    /* Test setting bits */
    int result = kes_bitmap_set(bitmap, 42);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to set bit");
    TEST_ASSERT(kes_bitmap_test(bitmap, 42) == true, "Bit not set");
    
    /* Test clearing bits */
    result = kes_bitmap_clear(bitmap, 42);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to clear bit");
    TEST_ASSERT(kes_bitmap_test(bitmap, 42) == false, "Bit not cleared");
    
    /* Test range operations */
    result = kes_bitmap_set_range(bitmap, 10, 5);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to set range");
    
    for (int i = 10; i < 15; i++) {
        TEST_ASSERT(kes_bitmap_test(bitmap, i) == true, "Range bit not set");
    }
    
    result = kes_bitmap_clear_range(bitmap, 10, 5);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to clear range");
    
    for (int i = 10; i < 15; i++) {
        TEST_ASSERT(kes_bitmap_test(bitmap, i) == false, 
                   "Range bit not cleared");
    }
    
    kes_bitmap_destroy(bitmap);
    TEST_SUCCESS("Bitmap basic operations");
}

static bool test_bitmap_find_free(void) {
    kes_bitmap_t* bitmap;
    kes_bitmap_create(100, &bitmap);
    
    /* Set some bits to create fragmentation */
    kes_bitmap_set_range(bitmap, 10, 5);  /* Occupy 10-14 */
    kes_bitmap_set_range(bitmap, 20, 3);  /* Occupy 20-22 */
    kes_bitmap_set_range(bitmap, 30, 10); /* Occupy 30-39 */
    
    /* Find free space */
    uint64_t found_start;
    int result = kes_bitmap_find_free(bitmap, 3, 0, &found_start);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to find free space");
    TEST_ASSERT(found_start < 10 || (found_start >= 15 && found_start < 20) ||
                (found_start >= 23 && found_start < 30) || found_start >= 40,
                "Found space in occupied area");
    
    /* Try to find space larger than available */
    result = kes_bitmap_find_free(bitmap, 50, 0, &found_start);
    TEST_ASSERT(result == KES_SUCCESS, "Should find 50 contiguous bits");
    
    /* Fill up most space - set bits 0-89, leaving bits 90-99 free (10 bits) */
    kes_bitmap_set_range(bitmap, 0, 90);
    
    /* Try to find 15 contiguous bits - should fail since only 10 are free */
    result = kes_bitmap_find_free(bitmap, 15, 0, &found_start);
    TEST_ASSERT(result == KES_ERROR_NOSPACE, "Should not find 15 bits");
    
    kes_bitmap_destroy(bitmap);
    TEST_SUCCESS("Bitmap find free");
}

/* =================================================================
 * Storage Tests
 * ================================================================= */

static bool test_storage_creation(void) {
    cleanup_test_files();
    
    /* Test storage creation with valid config */
    kes_storage_config_t config = {
        .device_path = TEST_STORAGE_FILE,
        .device_size = TEST_STORAGE_SIZE,
        .block_size = KES_DEFAULT_BLOCK_SIZE,
        .flags = KES_STORAGE_CREATE,
        .strategy = KES_ALLOC_FIRST_FIT
    };
    
    kes_storage_t* storage;
    int result = kes_storage_create(&config, &storage);
    TEST_ASSERT(result == KES_SUCCESS, "Storage creation failed");
    TEST_ASSERT(storage != NULL, "Storage is NULL");
    
    /* Verify storage descriptor */
    kes_storage_descriptor_t desc;
    kes_storage_get_descriptor(storage, &desc);
    TEST_ASSERT(desc.magic == KES_MAGIC_NUMBER, "Invalid magic number");
    TEST_ASSERT(desc.block_size == KES_DEFAULT_BLOCK_SIZE, "Wrong block size");
    TEST_ASSERT(desc.total_blocks > 0, "No blocks allocated");
    
    kes_storage_close(storage);
    cleanup_test_files();
    
    /* Test invalid configurations */
    config.device_path = NULL;
    result = kes_storage_create(&config, &storage);
    TEST_ASSERT(result == KES_ERROR_INVALID, "Should fail with NULL path");
    
    TEST_SUCCESS("Storage creation");
}

static bool test_storage_open_close(void) {
    cleanup_test_files();
    
    /* Create storage first */
    kes_storage_config_t config = {
        .device_path = TEST_STORAGE_FILE,
        .device_size = TEST_STORAGE_SIZE,
        .block_size = KES_DEFAULT_BLOCK_SIZE,
        .flags = KES_STORAGE_CREATE,
        .strategy = KES_ALLOC_FIRST_FIT
    };
    
    kes_storage_t* storage;
    kes_storage_create(&config, &storage);
    kes_storage_close(storage);
    
    /* Now test opening existing storage */
    int result = kes_storage_open(TEST_STORAGE_FILE, 0, &storage);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to open existing storage");
    
    /* Verify descriptor is loaded correctly */
    kes_storage_descriptor_t desc;
    kes_storage_get_descriptor(storage, &desc);
    TEST_ASSERT(desc.magic == KES_MAGIC_NUMBER, "Magic number corrupted");
    TEST_ASSERT(desc.block_size == KES_DEFAULT_BLOCK_SIZE, 
               "Block size corrupted");
    
    kes_storage_close(storage);
    
    /* Test opening non-existent file */
    result = kes_storage_open("/tmp/nonexistent_file", 0, &storage);
    TEST_ASSERT(result == KES_ERROR_IO, "Should fail with nonexistent file");
    
    cleanup_test_files();
    TEST_SUCCESS("Storage open/close");
}

static bool test_extent_allocation(void) {
    cleanup_test_files();
    
    /* Create storage */
    kes_storage_config_t config = {
        .device_path = TEST_STORAGE_FILE,
        .device_size = TEST_STORAGE_SIZE,
        .block_size = KES_DEFAULT_BLOCK_SIZE,
        .flags = KES_STORAGE_CREATE,
        .strategy = KES_ALLOC_FIRST_FIT
    };
    
    kes_storage_t* storage;
    kes_storage_create(&config, &storage);
    
    /* Test basic allocation */
    kes_extent_request_t request = {
        .block_count = 10,
        .alignment = 0,
        .hint_block = 0,
        .flags = 0
    };
    
    kes_extent_descriptor_t extent;
    int result = kes_extent_allocate(storage, &request, &extent);
    TEST_ASSERT(result == KES_SUCCESS, "Extent allocation failed");
    TEST_ASSERT(extent.block_count == 10, "Wrong block count allocated");
    TEST_ASSERT(extent.extent_id > 0, "Invalid extent ID");
    
    /* Test multiple allocations */
    kes_extent_descriptor_t extents[5];
    for (int i = 0; i < 5; i++) {
        result = kes_extent_allocate(storage, &request, &extents[i]);
        TEST_ASSERT(result == KES_SUCCESS, "Multiple allocation failed");
        TEST_ASSERT(extents[i].extent_id > extent.extent_id, 
                   "Extent IDs not increasing");
    }
    
    /* Test freeing extents */
    result = kes_extent_free(storage, &extent);
    TEST_ASSERT(result == KES_SUCCESS, "Extent free failed");

    /* Test double-free is rejected and leaves stats unchanged */
    kes_storage_stats_t stats_after_first_free;
    result = kes_storage_get_stats(storage, &stats_after_first_free);
    TEST_ASSERT(result == KES_SUCCESS, "Failed to get stats after free");

    result = kes_extent_free(storage, &extent);
    TEST_ASSERT(result == KES_ERROR_NOTFOUND,
               "Double-free should return KES_ERROR_NOTFOUND");

    kes_storage_stats_t stats_after_double_free;
    result = kes_storage_get_stats(storage, &stats_after_double_free);
    TEST_ASSERT(result == KES_SUCCESS,
               "Failed to get stats after double-free");
    TEST_ASSERT(stats_after_double_free.used_blocks ==
               stats_after_first_free.used_blocks,
               "Double-free changed used_blocks");
    TEST_ASSERT(stats_after_double_free.free_blocks ==
               stats_after_first_free.free_blocks,
               "Double-free changed free_blocks");
    TEST_ASSERT(stats_after_double_free.allocated_extents ==
               stats_after_first_free.allocated_extents,
               "Double-free changed allocated_extents");

    /* Test allocating after free */
    kes_extent_descriptor_t new_extent;
    result = kes_extent_allocate(storage, &request, &new_extent);
    TEST_ASSERT(result == KES_SUCCESS, "Allocation after free failed");
    
    kes_storage_close(storage);
    cleanup_test_files();
    TEST_SUCCESS("Extent allocation");
}

static bool test_extent_io(void) {
    cleanup_test_files();
    
    /* Create storage */
    kes_storage_config_t config = {
        .device_path = TEST_STORAGE_FILE,
        .device_size = TEST_STORAGE_SIZE,
        .block_size = KES_DEFAULT_BLOCK_SIZE,
        .flags = KES_STORAGE_CREATE,
        .strategy = KES_ALLOC_FIRST_FIT
    };
    
    kes_storage_t* storage;
    kes_storage_create(&config, &storage);
    
    /* Allocate extent */
    kes_extent_request_t request = {
        .block_count = 4,  /* 32KB */
        .alignment = 0,
        .hint_block = 0,
        .flags = 0
    };
    
    kes_extent_descriptor_t extent;
    kes_extent_allocate(storage, &request, &extent);
    
    /* Test writing */
    char write_data[] = "Hello, KES! This is test data for extent I/O.";
    size_t data_size = strlen(write_data) + 1;
    
    int result = kes_extent_write(storage, &extent, write_data, 
                                 data_size, 0);
    TEST_ASSERT(result == KES_SUCCESS, "Extent write failed");
    
    /* Test reading */
    char read_data[100] = {0};
    result = kes_extent_read(storage, &extent, read_data, data_size, 0);
    TEST_ASSERT(result == KES_SUCCESS, "Extent read failed");
    
    /* Verify data integrity */
    TEST_ASSERT(strcmp(write_data, read_data) == 0, 
               "Read data doesn't match written data");
    
    /* Test offset writing/reading */
    char offset_data[] = "OFFSET";
    result = kes_extent_write(storage, &extent, offset_data, 
                             strlen(offset_data), 100);
    TEST_ASSERT(result == KES_SUCCESS, "Offset write failed");
    
    char offset_read[10] = {0};
    result = kes_extent_read(storage, &extent, offset_read, 
                            strlen(offset_data), 100);
    TEST_ASSERT(result == KES_SUCCESS, "Offset read failed");
    TEST_ASSERT(strcmp(offset_data, offset_read) == 0, 
               "Offset data doesn't match");
    
    kes_storage_close(storage);
    cleanup_test_files();
    TEST_SUCCESS("Extent I/O");
}

static bool test_storage_persistence(void) {
    cleanup_test_files();
    
    const char* test_data = "Persistent test data";
    kes_extent_descriptor_t saved_extent;
    
    /* Create storage and write data */
    {
        kes_storage_config_t config = {
            .device_path = TEST_STORAGE_FILE,
            .device_size = TEST_STORAGE_SIZE,
            .block_size = KES_DEFAULT_BLOCK_SIZE,
            .flags = KES_STORAGE_CREATE,
            .strategy = KES_ALLOC_FIRST_FIT
        };
        
        kes_storage_t* storage;
        kes_storage_create(&config, &storage);
        
        kes_extent_request_t request = {.block_count = 2};
        kes_extent_allocate(storage, &request, &saved_extent);
        
        kes_extent_write(storage, &saved_extent, test_data, 
                        strlen(test_data) + 1, 0);
        
        kes_storage_close(storage);  /* This should save all metadata */
    }
    
    /* Reopen and verify data persists */
    {
        kes_storage_t* storage;
        int result = kes_storage_open(TEST_STORAGE_FILE, 0, &storage);
        TEST_ASSERT(result == KES_SUCCESS, "Failed to reopen storage");
        
        char read_data[100] = {0};
        result = kes_extent_read(storage, &saved_extent, read_data, 
                                strlen(test_data) + 1, 0);
        TEST_ASSERT(result == KES_SUCCESS, "Failed to read after reopen");
        TEST_ASSERT(strcmp(test_data, read_data) == 0, 
                   "Data not persistent");
        
        kes_storage_close(storage);
    }
    
    cleanup_test_files();
    TEST_SUCCESS("Storage persistence");
}

static bool test_utility_functions(void) {
    /* Test version information */
    uint16_t major, minor, patch;
    kes_get_version(&major, &minor, &patch);
    TEST_ASSERT(major == KES_VERSION_MAJOR, "Wrong major version");
    TEST_ASSERT(minor == KES_VERSION_MINOR, "Wrong minor version");
    TEST_ASSERT(patch == KES_VERSION_PATCH, "Wrong patch version");
    
    /* Test error strings */
    const char* error_str = kes_get_error_string(KES_SUCCESS);
    TEST_ASSERT(error_str != NULL, "NULL error string for success");
    TEST_ASSERT(strlen(error_str) > 0, "Empty error string");
    
    error_str = kes_get_error_string(KES_ERROR_NOMEM);
    TEST_ASSERT(error_str != NULL, "NULL error string for NOMEM");
    
    /* Test block calculations */
    uint32_t blocks = kes_calculate_blocks_needed(8192, 4096);
    TEST_ASSERT(blocks == 2, "Wrong block calculation");
    
    blocks = kes_calculate_blocks_needed(4095, 4096);
    TEST_ASSERT(blocks == 1, "Wrong block calculation for partial");
    
    /* Test extent size calculation */
    kes_extent_descriptor_t extent = {
        .block_count = 4
    };
    size_t size = kes_calculate_extent_size(&extent);
    TEST_ASSERT(size == 4 * KES_DEFAULT_BLOCK_SIZE, "Wrong extent size");
    
    /* Test config validation */
    kes_storage_config_t good_config = {
        .device_path = "/tmp/test",
        .device_size = 1024 * 1024,
        .block_size = 8192,
        .flags = 0,
        .strategy = KES_ALLOC_FIRST_FIT
    };
    
    int result = kes_config_validate(&good_config);
    TEST_ASSERT(result == KES_SUCCESS, "Valid config rejected");
    
    kes_storage_config_t bad_config = good_config;
    bad_config.device_path = NULL;
    result = kes_config_validate(&bad_config);
    TEST_ASSERT(result == KES_ERROR_INVALID, "Invalid config accepted");
    
    TEST_SUCCESS("Utility functions");
}

/* =================================================================
 * Test Runner
 * ================================================================= */

typedef struct {
    const char* name;
    bool (*func)(void);
} test_case_t;

static test_case_t test_cases[] = {
    {"Bitmap Creation", test_bitmap_creation},
    {"Bitmap Operations", test_bitmap_basic_operations},
    {"Bitmap Find Free", test_bitmap_find_free},
    {"Storage Creation", test_storage_creation},
    {"Storage Open/Close", test_storage_open_close},
    {"Extent Allocation", test_extent_allocation},
    {"Extent I/O", test_extent_io},
    {"Storage Persistence", test_storage_persistence},
    {"Utility Functions", test_utility_functions},
    {NULL, NULL}
};

int main(void) {
    printf("=== KES Minimal Implementation Test Suite ===\n\n");
    
    for (test_case_t* test = test_cases; test->name != NULL; test++) {
        printf("Running: %s... ", test->name);
        fflush(stdout);
        
        tests_run++;
        if (test->func()) {
            tests_passed++;
        }
    }
    
    printf("\n=== Test Results ===\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_run - tests_passed);
    
    if (tests_passed == tests_run) {
        printf("\n✅ All tests PASSED!\n");
        return 0;
    } else {
        printf("\n❌ Some tests FAILED!\n");
        return 1;
    }
}
