#define _GNU_SOURCE  /* For ftruncate */

#include <kes/kes_storage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Edge/boundary-input coverage for the kes_storage module -- see
 * plan_phase5.md S3 "A.2" for the exact task list this file
 * implements. Distinct from tests/test_kes_storage_full.c's
 * test_kes_extent_allocate, which only tests a single over-large
 * request against a partially-used bitmap, not actual bit-for-bit
 * exhaustion.
 */

#define TEST_ASSERT(condition, message) \
    do { \
        if ( !(condition)) { \
            printf( "FAIL: %s at %s:%d\n", message, __FILE__, __LINE__); \
            return(false); \
        } \
    } while ( 0)

#define TEST_SUCCESS(test_name) \
    do { \
        printf( "PASS: %s\n", test_name); \
        return(true); \
    } while ( 0)

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_FILE   "/tmp/kes_storage_edge_test"
#define TEST_SIZE   (2 * 1024 * 1024)   /* 2MB */

/* Generous upper bound on how many 1-block extents a 2MB device can
 * hold -- the real runtime block_size is KES_DEFAULT_BLOCK_SIZE
 * (8192), not config.block_size (see the documented, out-of-scope
 * init_storage_descriptor() quirk in
 * tests/test_kes_storage_full.c's test_kes_extent_read_write_32bit_
 * overflow comment), so 2MB / 8192 == 256 total blocks, well under
 * this. */
#define MAX_EXTENTS 512

static void cleanup(void) {
    unlink( TEST_FILE);
}

static int make_storage( const char *path, kes_storage_t **out) {
    kes_storage_config_t cfg = {
        .device_path = path,
        .device_size = TEST_SIZE,
        .block_size = 4096,
        .flags = KES_STORAGE_CREATE,
        .strategy = KES_ALLOC_FIRST_FIT,
    };
    return(kes_storage_create( &cfg, out));
}

/*
 * Allocate 1-block extents in a loop until kes_storage_get_stats()
 * reports free_blocks == 0 (true bit-for-bit exhaustion, not a
 * single over-large request against a partially-used bitmap),
 * confirm the next allocation -- even block_count = 1 -- cleanly
 * fails with KES_ERROR_NOSPACE rather than corrupting anything, that
 * kes_bitmap_get_stats() shows the bitmap's used count still matches
 * storage->desc.used_blocks (no desync between the bitmap and the
 * descriptor stats mirroring it), then free exactly one previously
 * allocated extent and confirm a block_count = 1 allocation succeeds
 * again.
 *
 * kes_storage_t is a fully-defined struct in include/kes/
 * kes_storage.h (not opaque), so storage->bitmap can be passed
 * directly to kes_bitmap_get_stats() here.
 */
static bool test_exhaustion_then_free_and_reallocate(void) {
    kes_storage_t *st = NULL;
    kes_extent_descriptor_t extents[MAX_EXTENTS];
    int allocated = 0;
    kes_storage_stats_t stats;
    uint64_t bm_total = 0, bm_free = 0, bm_used = 0;

    cleanup();
    TEST_ASSERT( make_storage( TEST_FILE, &st) == KES_SUCCESS,
                "create");

    uint64_t initial_free_blocks;
    TEST_ASSERT( kes_storage_get_stats( st, &stats) == KES_SUCCESS,
                "get_stats before filling storage");
    initial_free_blocks = stats.free_blocks;

    kes_extent_request_t req = { .block_count = 1, .alignment = 0,
                                  .hint_block = 0, .flags = 0 };

    while ( allocated < MAX_EXTENTS) {
        int result = kes_extent_allocate( st, &req, &extents[allocated]);

        if ( result != KES_SUCCESS) {
            TEST_ASSERT( result == KES_ERROR_NOSPACE,
                        "allocation failure at exhaustion must be "
                        "NOSPACE, not some other error");
            break;
        }
        allocated++;
    }

    TEST_ASSERT( allocated > 0 && allocated < MAX_EXTENTS,
                "sanity: exhaustion happened within the allocated "
                "extents array, not immediately and not never");

    TEST_ASSERT( kes_storage_get_stats( st, &stats) == KES_SUCCESS,
                "get_stats after filling storage to exhaustion");
    TEST_ASSERT( stats.free_blocks == 0,
                "free_blocks reports exactly 0 once exhausted");
    TEST_ASSERT( stats.used_blocks == initial_free_blocks,
                "used_blocks equals the storage's original "
                "(pre-allocation) free_blocks count -- every "
                "allocatable user block is now accounted for as "
                "used, none lost or double-counted");

    /* The next allocation, even the smallest possible one, must
     * cleanly fail rather than succeed or corrupt state. */
    kes_extent_descriptor_t overflow_ext;
    int overflow_result = kes_extent_allocate( st, &req, &overflow_ext);
    TEST_ASSERT( overflow_result == KES_ERROR_NOSPACE,
                "block_count = 1 request against a 100%-full storage "
                "still returns NOSPACE, not success or corruption");

    /* Bitmap-level corruption check: the bitmap's own accounting
     * must still agree with the descriptor's used/free counts that
     * mirror it -- a real desync here (not just a NOSPACE return)
     * would indicate actual bitmap corruption, not just exhaustion. */
    TEST_ASSERT( kes_bitmap_get_stats( st->bitmap, &bm_total, &bm_free,
                                       &bm_used) == KES_SUCCESS,
                "kes_bitmap_get_stats() succeeds");
    TEST_ASSERT( bm_free == 0,
                "bitmap itself also reports 0 free bits -- exhaustion "
                "is real, not a descriptor/bitmap desync");
    TEST_ASSERT( bm_used == stats.used_blocks,
                "bitmap's used-bit count matches "
                "storage->desc.used_blocks exactly -- no corruption "
                "between the two");
    TEST_ASSERT( bm_total == bm_free + bm_used,
                "bitmap total == free + used (internally consistent)");

    /* Free exactly one previously allocated extent and confirm a
     * fresh 1-block allocation now succeeds again. */
    TEST_ASSERT( kes_extent_free( st, &extents[0]) == KES_SUCCESS,
                "free exactly one previously allocated extent");

    kes_storage_get_stats( st, &stats);
    TEST_ASSERT( stats.free_blocks == 1,
                "exactly 1 block free again after freeing exactly "
                "one 1-block extent");

    kes_extent_descriptor_t reallocated;
    TEST_ASSERT( kes_extent_allocate( st, &req, &reallocated) ==
                    KES_SUCCESS,
                "a block_count = 1 allocation succeeds again "
                "immediately after freeing exactly one block's "
                "worth of room");
    TEST_ASSERT( reallocated.start_block == extents[0].start_block,
                "first-fit reallocates the just-freed block, "
                "consistent with allocate_extent_first_fit()'s "
                "search order");

    kes_storage_get_stats( st, &stats);
    TEST_ASSERT( stats.free_blocks == 0,
                "free_blocks is back to 0 after reallocating the "
                "freed block");

    kes_storage_close( st);
    cleanup();
    TEST_SUCCESS( "storage 100%-full exhaustion, clean NOSPACE, "
                  "bitmap/descriptor consistency, then successful "
                  "free-one-and-reallocate");
}

typedef struct {
    const char *name;
    bool ( *func)(void);
} test_case_t;

static test_case_t test_cases[] = {
    {"exhaustion then free and reallocate",
     test_exhaustion_then_free_and_reallocate},
    {NULL, NULL}
};

int main(void) {
    printf( "=== KES Storage Edge Case Coverage ===\n\n");

    for ( test_case_t *t = test_cases; t->name != NULL; t++) {
        printf( "Running: %s... ", t->name);
        fflush( stdout);
        tests_run++;
        if ( t->func()) {
            tests_passed++;
        }
    }

    printf( "\n=== Test Results ===\n");
    printf( "Tests run: %d\n", tests_run);
    printf( "Tests passed: %d\n", tests_passed);
    printf( "Tests failed: %d\n", tests_run - tests_passed);

    return( ( tests_passed == tests_run) ? 0 : 1);
}
