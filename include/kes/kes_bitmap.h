/*
 * kes_bitmap.h - KANEK Extents Storage Bitmap Management
 *
 * This file provides the bitmap management interface for tracking free
 * and used blocks in KES storage. This minimal implementation provides
 * basic bitmap operations needed for block allocation.
 *
 * Copyright (C) 2025 KANEK Project
 */

#ifndef KES_BITMAP_H
#define KES_BITMAP_H

#include "kes_types.h"
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bitmap structure */
struct kes_bitmap {
    uint8_t* data;                   /* Bitmap data */
    uint64_t total_bits;             /* Total bits in bitmap */
    uint64_t total_bytes;            /* Total bytes allocated */
    uint64_t free_bits;              /* Current free bits count */
    uint64_t search_hint;            /* Search hint for next allocation */
};

/* Bitmap management functions */

/**
 * Create a new bitmap for given number of blocks
 * @param total_blocks Total blocks to represent
 * @param bitmap Output parameter for created bitmap
 * @return KES_SUCCESS or error code
 */
int kes_bitmap_create(uint64_t total_blocks, kes_bitmap_t** bitmap);

/**
 * Destroy bitmap and free memory
 * @param bitmap Bitmap to destroy
 */
void kes_bitmap_destroy(kes_bitmap_t* bitmap);

/**
 * Set bit in bitmap (mark block as used)
 * @param bitmap Target bitmap
 * @param bit_index Bit position to set
 * @return KES_SUCCESS or error code
 */
int kes_bitmap_set(kes_bitmap_t* bitmap, uint64_t bit_index);

/**
 * Clear bit in bitmap (mark block as free)
 * @param bitmap Target bitmap
 * @param bit_index Bit position to clear
 * @return KES_SUCCESS or error code
 */
int kes_bitmap_clear(kes_bitmap_t* bitmap, uint64_t bit_index);

/**
 * Test if bit is set in bitmap
 * @param bitmap Target bitmap
 * @param bit_index Bit position to test
 * @return true if set, false if clear
 */
bool kes_bitmap_test(kes_bitmap_t* bitmap, uint64_t bit_index);

/**
 * Set multiple bits in bitmap (mark range as used)
 * @param bitmap Target bitmap
 * @param start_bit Starting bit position
 * @param bit_count Number of bits to set
 * @return KES_SUCCESS or error code
 */
int kes_bitmap_set_range(kes_bitmap_t* bitmap, uint64_t start_bit,
                         uint32_t bit_count);

/**
 * Clear multiple bits in bitmap (mark range as free)
 * @param bitmap Target bitmap
 * @param start_bit Starting bit position
 * @param bit_count Number of bits to clear
 * @return KES_SUCCESS or error code
 */
int kes_bitmap_clear_range(kes_bitmap_t* bitmap, uint64_t start_bit,
                           uint32_t bit_count);

/**
 * Find contiguous free bits in bitmap
 * @param bitmap Target bitmap
 * @param bit_count Number of contiguous bits needed
 * @param start_hint Suggested starting point for search
 * @param found_start Output parameter for found starting bit
 * @return KES_SUCCESS if found, KES_ERROR_NOSPACE if not found
 */
int kes_bitmap_find_free(kes_bitmap_t* bitmap, uint32_t bit_count,
                         uint64_t start_hint, uint64_t* found_start);

/**
 * Get bitmap statistics
 * @param bitmap Target bitmap
 * @param total_bits Output for total bits
 * @param free_bits Output for free bits
 * @param used_bits Output for used bits
 * @return KES_SUCCESS or error code
 */
int kes_bitmap_get_stats(kes_bitmap_t* bitmap, uint64_t* total_bits,
                         uint64_t* free_bits, uint64_t* used_bits);

/**
 * Load bitmap from storage
 * @param bitmap Target bitmap
 * @param fd File descriptor to read from
 * @param offset Offset in file to start reading
 * @return KES_SUCCESS or error code
 */
int kes_bitmap_load(kes_bitmap_t* bitmap, int fd, off_t offset);

/**
 * Save bitmap to storage
 * @param bitmap Target bitmap
 * @param fd File descriptor to write to
 * @param offset Offset in file to start writing
 * @return KES_SUCCESS or error code
 */
int kes_bitmap_save(kes_bitmap_t* bitmap, int fd, off_t offset);

#ifdef __cplusplus
}
#endif

#endif /* KES_BITMAP_H */
