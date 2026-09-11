#ifndef KES_STORAGE_H
#define KES_STORAGE_H

#include "kes_types.h"
#include "kes_bitmap.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Storage structure (minimal implementation) */
struct kes_storage {
    int fd;                          /* File descriptor */
    kes_storage_descriptor_t desc;   /* Storage descriptor */
    kes_bitmap_t *bitmap;            /* Block bitmap */
    kes_allocation_strategy_t strategy; /* Allocation strategy */

    /* Thread safety */
    pthread_mutex_t lock;            /* Storage-wide lock */

    /* Statistics */
    kes_storage_stats_t stats;       /* Runtime statistics */

    /* Configuration */
    bool readonly;                   /* Read-only mode */
    bool sync_writes;                /* Synchronous writes */
};

/* =================================================================
 * Storage Lifecycle Functions
 * ================================================================= */

/**
 * Create a new KES storage instance
 * @param config Storage configuration. config->strategy must be
 *                KES_ALLOC_FIRST_FIT -- BEST_FIT/WORST_FIT/NEXT_FIT
 *                are declared but not yet implemented and are
 *                rejected with KES_ERROR_INVALID (KES-2).
 * @param storage Output parameter for created storage handle
 * @return KES_SUCCESS or error code
 */
int kes_storage_create( const kes_storage_config_t *config,
                         kes_storage_t **storage);

/**
 * Open an existing KES storage instance
 * @param device_path Path to storage device or file
 * @param flags Open flags (readonly, sync, etc.)
 * @param storage Output parameter for opened storage handle
 * @return KES_SUCCESS, KES_ERROR_IO if the file is too short to hold
 *         a full descriptor (e.g. truncated), or KES_ERROR_CORRUPT if
 *         a full descriptor was read but its magic number, format
 *         version, or bitmap checksum is invalid -- these are two
 *         different failure shapes with two different codes, not
 *         interchangeable (KES-8).
 */
int kes_storage_open( const char *device_path, uint32_t flags,
                       kes_storage_t **storage);

/**
 * Close and cleanup storage instance. If not opened readonly, this
 * does the same descriptor/bitmap save + fsync() kes_storage_sync()
 * does (KES-7's durability note in kes_types.h applies here too)
 * before releasing resources.
 * @param storage Storage handle to close
 * @return KES_SUCCESS or error code
 */
int kes_storage_close( kes_storage_t *storage);

/**
 * Synchronize all pending changes to storage: writes the in-memory
 * bitmap and descriptor (free/used block counts, next_extent_id,
 * etc.) to disk and fsync()s the file. This is what makes ALLOCATION
 * BOOKKEEPING durable -- extent DATA written via kes_extent_write()
 * has its own, partly independent durability story (see
 * KES_STORAGE_SYNC's doc comment in kes_types.h, KES-7).
 * @param storage Storage handle to sync
 * @return KES_SUCCESS or error code
 */
int kes_storage_sync( kes_storage_t *storage);

/* =================================================================
 * Extent Management Functions
 * ================================================================= */

/**
 * Allocate a new extent with specified requirements
 * @param storage Storage handle
 * @param request Allocation request specification
 * @param extent Output parameter for allocated extent
 * @return KES_SUCCESS or error code
 */
int kes_extent_allocate( kes_storage_t *storage,
                          const kes_extent_request_t *request,
                          kes_extent_descriptor_t *extent);

/**
 * Free a previously allocated extent
 * @param storage Storage handle
 * @param extent Extent descriptor to free
 * @return KES_SUCCESS or error code
 */
int kes_extent_free( kes_storage_t *storage,
                      const kes_extent_descriptor_t *extent);

/**
 * Read data from an extent
 * @param storage Storage handle
 * @param extent Extent to read from
 * @param buffer Buffer to read data into
 * @param size Number of bytes to read
 * @param offset Byte offset within extent
 * @return KES_SUCCESS or error code
 */
int kes_extent_read( kes_storage_t *storage,
                      const kes_extent_descriptor_t *extent,
                      void *buffer, size_t size, uint64_t offset);

/**
 * Write data to an extent
 * @param storage Storage handle
 * @param extent Extent to write to
 * @param buffer Buffer containing data to write
 * @param size Number of bytes to write
 * @param offset Byte offset within extent
 * @return KES_SUCCESS or error code
 */
int kes_extent_write( kes_storage_t *storage,
                       const kes_extent_descriptor_t *extent,
                       const void *buffer, size_t size, uint64_t offset);

/* =================================================================
 * Storage Information and Statistics
 * ================================================================= */

/**
 * Get storage information and statistics
 * @param storage Storage handle
 * @param stats Output parameter for storage statistics
 * @return KES_SUCCESS or error code
 */
int kes_storage_get_stats( kes_storage_t *storage,
                            kes_storage_stats_t *stats);

/**
 * Get storage descriptor
 * @param storage Storage handle
 * @param descriptor Output parameter for storage descriptor
 * @return KES_SUCCESS or error code
 */
int kes_storage_get_descriptor( kes_storage_t *storage,
                                 kes_storage_descriptor_t *descriptor);

/* =================================================================
 * Utility Functions
 * ================================================================= */

/**
 * Get KES library version information
 * @param major Output parameter for major version
 * @param minor Output parameter for minor version
 * @param patch Output parameter for patch version
 */
void kes_get_version( uint16_t *major, uint16_t *minor, uint16_t *patch);

/**
 * Get human-readable error description
 * @param error_code Error code to describe
 * @return String description of error
 */
const char *kes_get_error_string( int error_code);

/**
 * Calculate blocks needed for given byte size
 * @param byte_size Size in bytes
 * @param block_size Block size in bytes
 * @return Number of blocks needed
 */
uint32_t kes_calculate_blocks_needed( size_t byte_size,
                                       uint32_t block_size);

/**
 * Calculate total size of extent in bytes
 * @param extent Extent descriptor
 * @return Extent size in bytes
 */
size_t kes_calculate_extent_size( const kes_extent_descriptor_t *extent);

/**
 * Validate storage configuration
 * @param config Configuration to validate
 * @return KES_SUCCESS if valid, error code if invalid
 */
int kes_config_validate( const kes_storage_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* KES_STORAGE_H */
