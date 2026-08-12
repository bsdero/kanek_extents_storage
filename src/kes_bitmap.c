#include <kes/kes_bitmap.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* Internal macros for bit manipulation */
#define BITS_PER_BYTE        8
#define BITS_PER_UINT64     64

#define BIT_BYTE_INDEX(bit)  ((bit) / BITS_PER_BYTE)
#define BIT_OFFSET(bit)      ((bit) % BITS_PER_BYTE)
#define BIT_MASK(bit)        (1U << BIT_OFFSET(bit))

/* Fast bit manipulation using 64-bit operations where possible */
#define UINT64_INDEX(bit)    ((bit) / BITS_PER_UINT64)
#define UINT64_OFFSET(bit)   ((bit) % BITS_PER_UINT64)
#define UINT64_MASK(bit)     (1ULL << UINT64_OFFSET(bit))

/* Internal helper functions */
static inline int popcount_byte( uint8_t byte) {
    /* Count number of 1 bits in a byte */
    static const uint8_t popcount_table[256] = {
        0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,
        1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
        1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
        2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
        1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
        2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
        2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
        3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,4,5,5,6,5,6,6,7,5,6,6,7,6,7,7,8
    };
    return(popcount_table[byte]);
}

static uint64_t count_used_bits( kes_bitmap_t *bitmap) {
    uint64_t used_count = 0;

    for ( uint64_t i = 0; i < bitmap->total_bytes; i++) {
        used_count += popcount_byte( bitmap->data[i]);
    }

    return(used_count);
}

/* =================================================================
 * Public API Implementation
 * ================================================================= */

int kes_bitmap_create( uint64_t total_blocks, kes_bitmap_t **bitmap) {
    if ( bitmap == NULL || total_blocks == 0) {
        return(KES_ERROR_INVALID);
    }

    kes_bitmap_t *bm = calloc( 1, sizeof(kes_bitmap_t));
    if ( bm == NULL) {
        return(KES_ERROR_NOMEM);
    }

    bm->total_bits = total_blocks;
    bm->total_bytes = (total_blocks + BITS_PER_BYTE - 1) / BITS_PER_BYTE;
    bm->free_bits = total_blocks;
    bm->search_hint = 0;

    /* Allocate and zero bitmap data */
    bm->data = calloc( bm->total_bytes, 1);
    if ( bm->data == NULL) {
        free( bm);
        return(KES_ERROR_NOMEM);
    }

    *bitmap = bm;
    return(KES_SUCCESS);
}

void kes_bitmap_destroy( kes_bitmap_t *bitmap) {
    if ( bitmap == NULL) {
        return;
    }

    free( bitmap->data);
    free( bitmap);
}

int kes_bitmap_set( kes_bitmap_t *bitmap, uint64_t bit_index) {
    if ( bitmap == NULL || bit_index >= bitmap->total_bits) {
        return(KES_ERROR_INVALID);
    }

    uint64_t byte_idx = BIT_BYTE_INDEX(bit_index);
    uint8_t bit_mask = BIT_MASK(bit_index);

    /* Check if bit is already set */
    if ( bitmap->data[byte_idx] & bit_mask) {
        return(KES_SUCCESS);  /* Already set */
    }

    /* Set the bit */
    bitmap->data[byte_idx] |= bit_mask;
    bitmap->free_bits--;

    return(KES_SUCCESS);
}

int kes_bitmap_clear( kes_bitmap_t *bitmap, uint64_t bit_index) {
    if ( bitmap == NULL || bit_index >= bitmap->total_bits) {
        return(KES_ERROR_INVALID);
    }

    uint64_t byte_idx = BIT_BYTE_INDEX(bit_index);
    uint8_t bit_mask = BIT_MASK(bit_index);

    /* Check if bit is already clear */
    if ( !(bitmap->data[byte_idx] & bit_mask)) {
        return(KES_SUCCESS);  /* Already clear */
    }

    /* Clear the bit */
    bitmap->data[byte_idx] &= ~bit_mask;
    bitmap->free_bits++;

    return(KES_SUCCESS);
}

bool kes_bitmap_test( kes_bitmap_t *bitmap, uint64_t bit_index) {
    if ( bitmap == NULL || bit_index >= bitmap->total_bits) {
        return(false);
    }

    uint64_t byte_idx = BIT_BYTE_INDEX(bit_index);
    uint8_t bit_mask = BIT_MASK(bit_index);

    return((bitmap->data[byte_idx] & bit_mask) != 0);
}

int kes_bitmap_set_range( kes_bitmap_t *bitmap, uint64_t start_bit,
                           uint32_t bit_count) {
    if ( bitmap == NULL || start_bit >= bitmap->total_bits ||
        start_bit + bit_count > bitmap->total_bits) {
        return(KES_ERROR_INVALID);
    }

    /* Set bits one by one for simplicity in minimal implementation */
    for ( uint32_t i = 0; i < bit_count; i++) {
        int result = kes_bitmap_set( bitmap, start_bit + i);
        if ( result != KES_SUCCESS) {
            return(result);
        }
    }

    return(KES_SUCCESS);
}

int kes_bitmap_clear_range( kes_bitmap_t *bitmap, uint64_t start_bit,
                             uint32_t bit_count) {
    if ( bitmap == NULL || start_bit >= bitmap->total_bits ||
        start_bit + bit_count > bitmap->total_bits) {
        return(KES_ERROR_INVALID);
    }

    /* Clear bits one by one for simplicity in minimal implementation */
    for ( uint32_t i = 0; i < bit_count; i++) {
        int result = kes_bitmap_clear( bitmap, start_bit + i);
        if ( result != KES_SUCCESS) {
            return(result);
        }
    }

    return(KES_SUCCESS);
}

int kes_bitmap_find_free( kes_bitmap_t *bitmap, uint32_t bit_count,
                           uint64_t start_hint, uint64_t *found_start) {
    if ( bitmap == NULL || bit_count == 0 || found_start == NULL) {
        return(KES_ERROR_INVALID);
    }

    if ( bitmap->free_bits < bit_count) {
        return(KES_ERROR_NOSPACE);
    }

    /* Start search from hint position */
    uint64_t search_start = start_hint;
    if ( search_start >= bitmap->total_bits) {
        search_start = 0;
    }

    /* Simple linear search for contiguous free bits */
    for ( uint64_t start = search_start;
         start <= bitmap->total_bits - bit_count; start++) {

        /* Check if we can fit the requested bits starting at 'start' */
        bool found = true;
        for ( uint32_t i = 0; i < bit_count && found; i++) {
            if ( kes_bitmap_test( bitmap, start + i)) {
                found = false;
            }
        }

        if ( found) {
            *found_start = start;
            bitmap->search_hint = start + bit_count; /* Update hint */
            return(KES_SUCCESS);
        }
    }

    /* If we started from a hint, try from beginning */
    if ( search_start > 0) {
        for ( uint64_t start = 0;
             start < search_start && start <= bitmap->total_bits - bit_count;
             start++) {

            bool found = true;
            for ( uint32_t i = 0; i < bit_count && found; i++) {
                if ( kes_bitmap_test( bitmap, start + i)) {
                    found = false;
                }
            }

            if ( found) {
                *found_start = start;
                bitmap->search_hint = start + bit_count;
                return(KES_SUCCESS);
            }
        }
    }

    return(KES_ERROR_NOSPACE);
}

int kes_bitmap_get_stats( kes_bitmap_t *bitmap, uint64_t *total_bits,
                           uint64_t *free_bits, uint64_t *used_bits) {
    if ( bitmap == NULL) {
        return(KES_ERROR_INVALID);
    }

    if ( total_bits != NULL) {
        *total_bits = bitmap->total_bits;
    }

    if ( free_bits != NULL) {
        *free_bits = bitmap->free_bits;
    }

    if ( used_bits != NULL) {
        *used_bits = bitmap->total_bits - bitmap->free_bits;
    }

    return(KES_SUCCESS);
}

int kes_bitmap_load( kes_bitmap_t *bitmap, int fd, off_t offset) {
    if ( bitmap == NULL || fd < 0) {
        return(KES_ERROR_INVALID);
    }

    /* Seek to the bitmap location */
    if ( lseek( fd, offset, SEEK_SET) != offset) {
        return(KES_ERROR_IO);
    }

    /* Read bitmap data */
    ssize_t bytes_read = read( fd, bitmap->data, bitmap->total_bytes);
    if ( bytes_read != (ssize_t)bitmap->total_bytes) {
        return(KES_ERROR_IO);
    }

    /* Recalculate free bits count */
    uint64_t used_bits = count_used_bits( bitmap);
    bitmap->free_bits = bitmap->total_bits - used_bits;

    return(KES_SUCCESS);
}

int kes_bitmap_save( kes_bitmap_t *bitmap, int fd, off_t offset) {
    if ( bitmap == NULL || fd < 0) {
        return(KES_ERROR_INVALID);
    }

    /* Seek to the bitmap location */
    if ( lseek( fd, offset, SEEK_SET) != offset) {
        return(KES_ERROR_IO);
    }

    /* Write bitmap data */
    ssize_t bytes_written = write( fd, bitmap->data, bitmap->total_bytes);
    if ( bytes_written != (ssize_t)bitmap->total_bytes) {
        return(KES_ERROR_IO);
    }

    return(KES_SUCCESS);
}
