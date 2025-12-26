/*
 * example_kes_usage.c - Example demonstrating KES usage
 *
 * This example shows how to use the minimal KES implementation
 * for basic storage operations.
 *
 * Copyright (C) 2025 KANEK Project
 */

#include "include/kes/kes_storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_error_and_exit(const char* operation, int error_code) {
    printf("Error in %s: %s\n", operation, kes_get_error_string(error_code));
    exit(1);
}

static void print_storage_info(kes_storage_t* storage) {
    kes_storage_stats_t stats;
    kes_storage_get_stats(storage, &stats);
    
    printf("\nStorage Information:\n");
    printf("  Total blocks: %lu\n", stats.total_blocks);
    printf("  Free blocks:  %lu (%.1f%%)\n", 
           stats.free_blocks,
           100.0 * stats.free_blocks / stats.total_blocks);
    printf("  Used blocks:  %lu (%.1f%%)\n",
           stats.used_blocks,
           100.0 * stats.used_blocks / stats.total_blocks);
    printf("  Allocated extents: %lu\n", stats.allocated_extents);
    printf("  Fragmentation: %lu%%\n", stats.fragmentation);
    printf("\n");
}

int main(void) {
    printf("=== KES Storage Example ===\n");
    
    /* Configuration for our example storage */
    kes_storage_config_t config = {
        .device_path = "/tmp/example_storage.kes",
        .device_size = 50 * 1024 * 1024,  /* 50MB */
        .block_size = KES_DEFAULT_BLOCK_SIZE,
        .flags = KES_STORAGE_CREATE | KES_STORAGE_TRUNCATE,
        .strategy = KES_ALLOC_FIRST_FIT
    };
    
    /* Create storage */
    printf("Creating storage...\n");
    kes_storage_t* storage;
    int result = kes_storage_create(&config, &storage);
    if (result != KES_SUCCESS) {
        print_error_and_exit("kes_storage_create", result);
    }
    
    print_storage_info(storage);
    
    /* Allocate some extents */
    printf("Allocating extents...\n");
    
    kes_extent_descriptor_t extents[4];
    kes_extent_request_t request = {
        .block_count = 8,  /* 64KB each */
        .alignment = 0,
        .hint_block = 0,
        .flags = 0
    };
    
    for (int i = 0; i < 4; i++) {
        result = kes_extent_allocate(storage, &request, &extents[i]);
        if (result != KES_SUCCESS) {
            print_error_and_exit("kes_extent_allocate", result);
        }
        
        printf("  Allocated extent %d: start_block=%lu, count=%u, id=%lu\n",
               i + 1, extents[i].start_block, extents[i].block_count,
               extents[i].extent_id);
    }
    
    print_storage_info(storage);
    
    /* Write data to extents */
    printf("Writing data to extents...\n");
    
    for (int i = 0; i < 4; i++) {
        char data[1024];
        snprintf(data, sizeof(data), 
                "This is test data for extent %d (ID: %lu). "
                "The extent starts at block %lu and contains %u blocks. "
                "This demonstrates basic KES functionality!",
                i + 1, extents[i].extent_id, 
                extents[i].start_block, extents[i].block_count);
        
        result = kes_extent_write(storage, &extents[i], data, 
                                 strlen(data) + 1, 0);
        if (result != KES_SUCCESS) {
            print_error_and_exit("kes_extent_write", result);
        }
    }
    
    /* Read data back */
    printf("Reading data back...\n");
    
    for (int i = 0; i < 4; i++) {
        char read_buffer[1024] = {0};
        
        /* Read the same amount we wrote */
        char expected_data[1024];
        snprintf(expected_data, sizeof(expected_data), 
                "This is test data for extent %d (ID: %lu). "
                "The extent starts at block %lu and contains %u blocks. "
                "This demonstrates basic KES functionality!",
                i + 1, extents[i].extent_id, 
                extents[i].start_block, extents[i].block_count);
        
        size_t data_size = strlen(expected_data) + 1;
        
        result = kes_extent_read(storage, &extents[i], read_buffer,
                                data_size, 0);
        if (result != KES_SUCCESS) {
            printf("Failed to read extent %d (ID: %lu, start_block: %lu): %s\n",
                   i + 1, extents[i].extent_id, extents[i].start_block,
                   kes_get_error_string(result));
            print_error_and_exit("kes_extent_read", result);
        }
        
        printf("  Extent %d data: %.100s...\n", i + 1, read_buffer);
    }
    
    /* Free some extents */
    printf("\nFreeing extents 2 and 4...\n");
    
    result = kes_extent_free(storage, &extents[1]);  /* Extent 2 */
    if (result != KES_SUCCESS) {
        print_error_and_exit("kes_extent_free", result);
    }
    
    result = kes_extent_free(storage, &extents[3]);  /* Extent 4 */
    if (result != KES_SUCCESS) {
        print_error_and_exit("kes_extent_free", result);
    }
    
    print_storage_info(storage);
    
    /* Allocate a larger extent */
    printf("Allocating a larger extent...\n");
    
    kes_extent_request_t large_request = {
        .block_count = 20,  /* 160KB */
        .alignment = 0,
        .hint_block = 0,
        .flags = 0
    };
    
    kes_extent_descriptor_t large_extent;
    result = kes_extent_allocate(storage, &large_request, &large_extent);
    if (result != KES_SUCCESS) {
        print_error_and_exit("large allocation", result);
    }
    
    printf("  Large extent: start_block=%lu, count=%u, id=%lu\n",
           large_extent.start_block, large_extent.block_count,
           large_extent.extent_id);
    
    print_storage_info(storage);
    
    /* Demonstrate extent size calculation */
    size_t extent_bytes = kes_calculate_extent_size(&large_extent);
    printf("Large extent size: %zu bytes (%.1f KB)\n", 
           extent_bytes, extent_bytes / 1024.0);
    
    /* Sync storage */
    printf("\nSynchronizing storage...\n");
    result = kes_storage_sync(storage);
    if (result != KES_SUCCESS) {
        print_error_and_exit("kes_storage_sync", result);
    }
    
    /* Show final statistics */
    print_storage_info(storage);
    
    /* Get version information */
    uint16_t major, minor, patch;
    kes_get_version(&major, &minor, &patch);
    printf("KES Library Version: %u.%u.%u\n", major, minor, patch);
    
    /* Close storage */
    printf("\nClosing storage...\n");
    result = kes_storage_close(storage);
    if (result != KES_SUCCESS) {
        print_error_and_exit("kes_storage_close", result);
    }
    
    printf("✅ Example completed successfully!\n");
    printf("\nTo clean up, remove: /tmp/example_storage.kes\n");
    
    return 0;
}
