/*
 * kes_storage.c - KANEK Extents Storage Core Implementation
 *
 * This file implements the core storage management functionality
 * including storage creation, extent allocation, and I/O operations.
 *
 * Copyright (C) 2025 KANEK Project
 */

#include <kes/kes_storage.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

/* Internal helper functions */
static int validate_config(const kes_storage_config_t* config);
static int init_storage_descriptor(kes_storage_t* storage, uint64_t size);
static int load_storage_descriptor(kes_storage_t* storage);
static int save_storage_descriptor(kes_storage_t* storage);
static uint64_t calculate_bitmap_blocks(uint64_t total_blocks, 
                                       uint32_t block_size);
static int allocate_extent_first_fit(kes_storage_t* storage,
                                     const kes_extent_request_t* request,
                                     kes_extent_descriptor_t* extent);

/* Error string table */
static const char* error_strings[] = {
    "Success",                          /* KES_SUCCESS */
    "Invalid parameters",               /* KES_ERROR_INVALID */
    "Out of memory",                   /* KES_ERROR_NOMEM */
    "Resource not found",              /* KES_ERROR_NOTFOUND */
    "Resource already exists",         /* KES_ERROR_EXISTS */
    "I/O error",                       /* KES_ERROR_IO */
    "No space available",              /* KES_ERROR_NOSPACE */
    "Data corruption detected"         /* KES_ERROR_CORRUPT */
};

/* =================================================================
 * Storage Lifecycle Implementation
 * ================================================================= */

int kes_storage_create(const kes_storage_config_t* config,
                       kes_storage_t** storage) {
    if (!config || !storage) {
        return KES_ERROR_INVALID;
    }
    
    int result = validate_config(config);
    if (result != KES_SUCCESS) {
        return result;
    }
    
    /* Allocate storage structure */
    kes_storage_t* sto = calloc(1, sizeof(kes_storage_t));
    if (!sto) {
        return KES_ERROR_NOMEM;
    }
    
    /* Initialize mutex */
    if (pthread_mutex_init(&sto->lock, NULL) != 0) {
        free(sto);
        return KES_ERROR_NOMEM;
    }
    
    /* Open/create the storage device/file */
    int flags = O_RDWR;
    if (config->flags & KES_STORAGE_CREATE) {
        flags |= O_CREAT;
    }
    if (config->flags & KES_STORAGE_TRUNCATE) {
        flags |= O_TRUNC;
    }
    if (config->flags & KES_STORAGE_SYNC) {
        flags |= O_SYNC;
    }
    
    sto->fd = open(config->device_path, flags, 0644);
    if (sto->fd < 0) {
        pthread_mutex_destroy(&sto->lock);
        free(sto);
        return KES_ERROR_IO;
    }
    
    /* Set storage configuration */
    sto->strategy = config->strategy;
    sto->readonly = (config->flags & KES_STORAGE_READONLY) != 0;
    sto->sync_writes = (config->flags & KES_STORAGE_SYNC) != 0;
    
    /* Initialize storage layout */
    result = init_storage_descriptor(sto, config->device_size);
    if (result != KES_SUCCESS) {
        close(sto->fd);
        pthread_mutex_destroy(&sto->lock);
        free(sto);
        return result;
    }
    
    /* Create bitmap */
    result = kes_bitmap_create(sto->desc.user_blocks, &sto->bitmap);
    if (result != KES_SUCCESS) {
        close(sto->fd);
        pthread_mutex_destroy(&sto->lock);
        free(sto);
        return result;
    }
    
    /* Save initial descriptor to storage */
    result = save_storage_descriptor(sto);
    if (result != KES_SUCCESS) {
        kes_bitmap_destroy(sto->bitmap);
        close(sto->fd);
        pthread_mutex_destroy(&sto->lock);
        free(sto);
        return result;
    }
    
    *storage = sto;
    return KES_SUCCESS;
}

int kes_storage_open(const char* device_path, uint32_t flags,
                     kes_storage_t** storage) {
    if (!device_path || !storage) {
        return KES_ERROR_INVALID;
    }
    
    /* Allocate storage structure */
    kes_storage_t* sto = calloc(1, sizeof(kes_storage_t));
    if (!sto) {
        return KES_ERROR_NOMEM;
    }
    
    /* Initialize mutex */
    if (pthread_mutex_init(&sto->lock, NULL) != 0) {
        free(sto);
        return KES_ERROR_NOMEM;
    }
    
    /* Open the storage device/file */
    int open_flags = (flags & KES_STORAGE_READONLY) ? O_RDONLY : O_RDWR;
    if (flags & KES_STORAGE_SYNC) {
        open_flags |= O_SYNC;
    }
    
    sto->fd = open(device_path, open_flags);
    if (sto->fd < 0) {
        pthread_mutex_destroy(&sto->lock);
        free(sto);
        return KES_ERROR_IO;
    }
    
    /* Set storage configuration */
    sto->readonly = (flags & KES_STORAGE_READONLY) != 0;
    sto->sync_writes = (flags & KES_STORAGE_SYNC) != 0;
    sto->strategy = KES_ALLOC_FIRST_FIT;  /* Default strategy */
    
    /* Load storage descriptor */
    int result = load_storage_descriptor(sto);
    if (result != KES_SUCCESS) {
        close(sto->fd);
        pthread_mutex_destroy(&sto->lock);
        free(sto);
        return result;
    }
    
    /* Create and load bitmap */
    result = kes_bitmap_create(sto->desc.user_blocks, &sto->bitmap);
    if (result != KES_SUCCESS) {
        close(sto->fd);
        pthread_mutex_destroy(&sto->lock);
        free(sto);
        return result;
    }
    
    /* Load bitmap from storage */
    off_t bitmap_offset = sto->desc.bitmap_start_block * sto->desc.block_size;
    result = kes_bitmap_load(sto->bitmap, sto->fd, bitmap_offset);
    if (result != KES_SUCCESS) {
        kes_bitmap_destroy(sto->bitmap);
        close(sto->fd);
        pthread_mutex_destroy(&sto->lock);
        free(sto);
        return result;
    }
    
    *storage = sto;
    return KES_SUCCESS;
}

int kes_storage_close(kes_storage_t* storage) {
    if (!storage) {
        return KES_ERROR_INVALID;
    }
    
    pthread_mutex_lock(&storage->lock);
    
    /* Save bitmap before closing */
    if (!storage->readonly) {
        off_t bitmap_offset = storage->desc.bitmap_start_block * 
                             storage->desc.block_size;
        kes_bitmap_save(storage->bitmap, storage->fd, bitmap_offset);
        
        /* Save descriptor with updated statistics */
        save_storage_descriptor(storage);
        
        /* Sync file system */
        fsync(storage->fd);
    }
    
    /* Cleanup resources */
    kes_bitmap_destroy(storage->bitmap);
    close(storage->fd);
    
    pthread_mutex_unlock(&storage->lock);
    pthread_mutex_destroy(&storage->lock);
    free(storage);
    
    return KES_SUCCESS;
}

int kes_storage_sync(kes_storage_t* storage) {
    if (!storage || storage->readonly) {
        return KES_ERROR_INVALID;
    }
    
    pthread_mutex_lock(&storage->lock);
    
    /* Save bitmap */
    off_t bitmap_offset = storage->desc.bitmap_start_block * 
                         storage->desc.block_size;
    int result = kes_bitmap_save(storage->bitmap, storage->fd, bitmap_offset);
    
    if (result == KES_SUCCESS) {
        /* Save descriptor */
        result = save_storage_descriptor(storage);
    }
    
    if (result == KES_SUCCESS) {
        /* Force sync to disk */
        if (fsync(storage->fd) != 0) {
            result = KES_ERROR_IO;
        }
    }
    
    pthread_mutex_unlock(&storage->lock);
    
    return result;
}

/* =================================================================
 * Extent Management Implementation
 * ================================================================= */

int kes_extent_allocate(kes_storage_t* storage,
                        const kes_extent_request_t* request,
                        kes_extent_descriptor_t* extent) {
    if (!storage || !request || !extent || storage->readonly) {
        return KES_ERROR_INVALID;
    }
    
    if (request->block_count == 0) {
        return KES_ERROR_INVALID;
    }
    
    pthread_mutex_lock(&storage->lock);
    
    int result;
    
    /* Use allocation strategy */
    switch (storage->strategy) {
        case KES_ALLOC_FIRST_FIT:
        default:
            result = allocate_extent_first_fit(storage, request, extent);
            break;
    }
    
    if (result == KES_SUCCESS) {
        /* Update statistics */
        storage->stats.allocated_extents++;
        storage->desc.used_blocks += request->block_count;
        storage->desc.free_blocks -= request->block_count;
        
        /* Assign unique extent ID */
        extent->extent_id = ++storage->desc.next_extent_id;
    }
    
    pthread_mutex_unlock(&storage->lock);
    
    return result;
}

int kes_extent_free(kes_storage_t* storage,
                    const kes_extent_descriptor_t* extent) {
    if (!storage || !extent || storage->readonly) {
        return KES_ERROR_INVALID;
    }
    
    pthread_mutex_lock(&storage->lock);
    
    /* Clear bits in bitmap */
    int result = kes_bitmap_clear_range(storage->bitmap,
                                       extent->start_block,
                                       extent->block_count);
    
    if (result == KES_SUCCESS) {
        /* Update statistics */
        storage->desc.used_blocks -= extent->block_count;
        storage->desc.free_blocks += extent->block_count;
        storage->stats.allocated_extents--;
    }
    
    pthread_mutex_unlock(&storage->lock);
    
    return result;
}

int kes_extent_read(kes_storage_t* storage,
                    const kes_extent_descriptor_t* extent,
                    void* buffer, size_t size, uint64_t offset) {
    if (!storage || !extent || !buffer) {
        return KES_ERROR_INVALID;
    }
    
    /* Calculate absolute file offset */
    uint64_t extent_start_byte = (storage->desc.user_start_block + 
                                 extent->start_block) * 
                                storage->desc.block_size;
    off_t file_offset = extent_start_byte + offset;
    
    /* Validate bounds */
    uint64_t extent_size = extent->block_count * storage->desc.block_size;
    if (offset + size > extent_size) {
        return KES_ERROR_INVALID;
    }
    
    pthread_mutex_lock(&storage->lock);
    
    /* Seek to position */
    if (lseek(storage->fd, file_offset, SEEK_SET) != file_offset) {
        pthread_mutex_unlock(&storage->lock);
        return KES_ERROR_IO;
    }
    
    /* Read data */
    ssize_t bytes_read = read(storage->fd, buffer, size);
    if (bytes_read != (ssize_t)size) {
        pthread_mutex_unlock(&storage->lock);
        return KES_ERROR_IO;
    }
    
    /* Update statistics */
    storage->stats.reads_completed++;
    storage->stats.bytes_read += size;
    
    pthread_mutex_unlock(&storage->lock);
    
    return KES_SUCCESS;
}

int kes_extent_write(kes_storage_t* storage,
                     const kes_extent_descriptor_t* extent,
                     const void* buffer, size_t size, uint64_t offset) {
    if (!storage || !extent || !buffer || storage->readonly) {
        return KES_ERROR_INVALID;
    }
    
    /* Calculate absolute file offset */
    uint64_t extent_start_byte = (storage->desc.user_start_block + 
                                 extent->start_block) * 
                                storage->desc.block_size;
    off_t file_offset = extent_start_byte + offset;
    
    /* Validate bounds */
    uint64_t extent_size = extent->block_count * storage->desc.block_size;
    if (offset + size > extent_size) {
        return KES_ERROR_INVALID;
    }
    
    pthread_mutex_lock(&storage->lock);
    
    /* Seek to position */
    if (lseek(storage->fd, file_offset, SEEK_SET) != file_offset) {
        pthread_mutex_unlock(&storage->lock);
        return KES_ERROR_IO;
    }
    
    /* Write data */
    ssize_t bytes_written = write(storage->fd, buffer, size);
    if (bytes_written != (ssize_t)size) {
        pthread_mutex_unlock(&storage->lock);
        return KES_ERROR_IO;
    }
    
    /* Update statistics */
    storage->stats.writes_completed++;
    storage->stats.bytes_written += size;
    
    pthread_mutex_unlock(&storage->lock);
    
    return KES_SUCCESS;
}

/* =================================================================
 * Information and Statistics
 * ================================================================= */

int kes_storage_get_stats(kes_storage_t* storage,
                          kes_storage_stats_t* stats) {
    if (!storage || !stats) {
        return KES_ERROR_INVALID;
    }
    
    pthread_mutex_lock(&storage->lock);
    
    /* Copy basic statistics */
    memcpy(stats, &storage->stats, sizeof(kes_storage_stats_t));
    
    /* Update with current values */
    stats->total_blocks = storage->desc.total_blocks;
    stats->free_blocks = storage->desc.free_blocks;
    stats->used_blocks = storage->desc.used_blocks;
    
    /* Calculate fragmentation (simplified) */
    if (stats->used_blocks > 0) {
        stats->fragmentation = 
            (uint64_t)(100.0 * (storage->stats.allocated_extents - 1) / 
                      stats->used_blocks);
    } else {
        stats->fragmentation = 0;
    }
    
    pthread_mutex_unlock(&storage->lock);
    
    return KES_SUCCESS;
}

int kes_storage_get_descriptor(kes_storage_t* storage,
                               kes_storage_descriptor_t* descriptor) {
    if (!storage || !descriptor) {
        return KES_ERROR_INVALID;
    }
    
    pthread_mutex_lock(&storage->lock);
    memcpy(descriptor, &storage->desc, sizeof(kes_storage_descriptor_t));
    pthread_mutex_unlock(&storage->lock);
    
    return KES_SUCCESS;
}

/* =================================================================
 * Utility Functions Implementation
 * ================================================================= */

void kes_get_version(uint16_t* major, uint16_t* minor, uint16_t* patch) {
    if (major) *major = KES_VERSION_MAJOR;
    if (minor) *minor = KES_VERSION_MINOR;
    if (patch) *patch = KES_VERSION_PATCH;
}

const char* kes_get_error_string(int error_code) {
    int index = -error_code;
    if (index < 0 || index >= (int)(sizeof(error_strings) / 
                                   sizeof(error_strings[0]))) {
        return "Unknown error";
    }
    return error_strings[index];
}

uint32_t kes_calculate_blocks_needed(size_t byte_size, uint32_t block_size) {
    return (uint32_t)KES_BYTES_TO_BLOCKS(byte_size, block_size);
}

size_t kes_calculate_extent_size(const kes_extent_descriptor_t* extent) {
    if (!extent) {
        return 0;
    }
    return extent->block_count * KES_DEFAULT_BLOCK_SIZE;
}

int kes_config_validate(const kes_storage_config_t* config) {
    return validate_config(config);
}

/* =================================================================
 * Internal Helper Functions
 * ================================================================= */

static int validate_config(const kes_storage_config_t* config) {
    if (!config || !config->device_path) {
        return KES_ERROR_INVALID;
    }
    
    /* Validate block size */
    if (!KES_IS_POWER_OF_2(config->block_size) ||
        config->block_size < KES_MIN_BLOCK_SIZE ||
        config->block_size > KES_MAX_BLOCK_SIZE) {
        return KES_ERROR_INVALID;
    }
    
    /* Validate device size */
    if (config->device_size < config->block_size * 10) {
        return KES_ERROR_INVALID;  /* Too small */
    }
    
    return KES_SUCCESS;
}

static int init_storage_descriptor(kes_storage_t* storage, uint64_t size) {
    kes_storage_descriptor_t* desc = &storage->desc;
    
    /* Initialize basic fields */
    desc->magic = KES_MAGIC_NUMBER;
    desc->version_major = KES_VERSION_MAJOR;
    desc->version_minor = KES_VERSION_MINOR;
    desc->block_size = KES_DEFAULT_BLOCK_SIZE;
    desc->total_blocks = size / desc->block_size;
    
    /* Calculate layout */
    uint64_t bitmap_blocks = calculate_bitmap_blocks(desc->total_blocks,
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
    
    return KES_SUCCESS;
}

static int load_storage_descriptor(kes_storage_t* storage) {
    /* Seek to beginning of file */
    if (lseek(storage->fd, 0, SEEK_SET) != 0) {
        return KES_ERROR_IO;
    }
    
    /* Read descriptor */
    ssize_t bytes_read = read(storage->fd, &storage->desc,
                             sizeof(kes_storage_descriptor_t));
    if (bytes_read != sizeof(kes_storage_descriptor_t)) {
        return KES_ERROR_IO;
    }
    
    /* Validate descriptor */
    if (storage->desc.magic != KES_MAGIC_NUMBER) {
        return KES_ERROR_CORRUPT;
    }
    
    return KES_SUCCESS;
}

static int save_storage_descriptor(kes_storage_t* storage) {
    /* Seek to beginning of file */
    if (lseek(storage->fd, 0, SEEK_SET) != 0) {
        return KES_ERROR_IO;
    }
    
    /* Write descriptor */
    ssize_t bytes_written = write(storage->fd, &storage->desc,
                                 sizeof(kes_storage_descriptor_t));
    if (bytes_written != sizeof(kes_storage_descriptor_t)) {
        return KES_ERROR_IO;
    }
    
    return KES_SUCCESS;
}

static uint64_t calculate_bitmap_blocks(uint64_t total_blocks, 
                                       uint32_t block_size) {
    /* Each byte represents 8 blocks */
    uint64_t bitmap_bytes = (total_blocks + 7) / 8;
    return (bitmap_bytes + block_size - 1) / block_size;
}

static int allocate_extent_first_fit(kes_storage_t* storage,
                                     const kes_extent_request_t* request,
                                     kes_extent_descriptor_t* extent) {
    uint64_t found_start;
    int result = kes_bitmap_find_free(storage->bitmap, request->block_count,
                                     request->hint_block, &found_start);
    
    if (result != KES_SUCCESS) {
        return result;
    }
    
    /* Mark blocks as used */
    result = kes_bitmap_set_range(storage->bitmap, found_start,
                                 request->block_count);
    if (result != KES_SUCCESS) {
        return result;
    }
    
    /* Fill extent descriptor */
    extent->start_block = found_start;
    extent->block_count = request->block_count;
    extent->flags = request->flags;
    extent->extent_id = 0;  /* Will be set by caller */
    
    return KES_SUCCESS;
}
