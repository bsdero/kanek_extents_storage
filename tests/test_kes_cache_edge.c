#define _GNU_SOURCE

#include <kes/kes_cache.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

/*
 * Edge/boundary-input coverage for the kes_cache module -- distinct
 * from tests/test_kes_cache_full.c (one direct success/failure test
 * per public function) and tests/test_kes_cache.c (general
 * functional + concurrency coverage). See plan_phase5.md S3 "A.1" for
 * the exact task list this file implements.
 *
 * Uses a copy/adapted subset of the mock I/O harness pattern from
 * tests/test_kes_cache.c (mock_read_extent/mock_write_extent,
 * g_mock_storage) rather than #include-ing that file, per
 * plan_phase5.md's instruction.
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

#define EDGE_BLOCK_SIZE   4096
#define EDGE_NUM_BLOCKS   2048

static unsigned char g_backing[EDGE_NUM_BLOCKS][EDGE_BLOCK_SIZE];

static int mock_read( void *dev, const kes_extent_id_t *id, void *buf,
                       size_t size) {
    (void)dev;
    if ( id->start_block >= EDGE_NUM_BLOCKS || size > EDGE_BLOCK_SIZE) {
        return(KES_ERROR_IO);
    }
    memcpy( buf, g_backing[id->start_block], size);
    return(KES_SUCCESS);
}

static int mock_write( void *dev, const kes_extent_id_t *id,
                        const void *buf, size_t size) {
    (void)dev;
    if ( id->start_block >= EDGE_NUM_BLOCKS || size > EDGE_BLOCK_SIZE) {
        return(KES_ERROR_IO);
    }
    memcpy( g_backing[id->start_block], buf, size);
    return(KES_SUCCESS);
}

static int mock_sync( void *dev) {
    (void)dev;
    return(KES_SUCCESS);
}

static void default_config( kes_cache_config_t *cfg) {
    /* small/fast edge sizing, same choice test_kes_cache_full.c makes */
    kes_cache_get_default_config( cfg, true);
}

static kes_extent_id_t make_id( uint64_t start_block) {
    kes_extent_id_t id = { .start_block = start_block, .block_count = 1,
                            .block_size = EDGE_BLOCK_SIZE, .reserved = 0 };
    return(id);
}

/* ================================================================
 * A.1.1 -- per-parameter NULL checks not already covered elsewhere.
 *
 * tests/test_kes_cache_full.c already exercises every parameter of
 * kes_cache_get_extent/put_extent (both cache/id/buffer), pin_extent/
 * unpin_extent (cache only), mark_dirty (cache only), flush_extent
 * (cache only), get_stats (cache and stats), create, destroy, stop,
 * and set_io_callbacks (cache only). This test adds exactly what was
 * missing after checking that file function-by-function:
 *
 *   - kes_cache_pin_extent / kes_cache_unpin_extent: NULL `id`
 *     (only NULL `cache` was previously tested).
 *   - kes_cache_mark_dirty / kes_cache_flush_extent: NULL `id`
 *     (only NULL `cache` was previously tested).
 *   - kes_cache_invalidate: NULL `cache` and NULL `id`. The existing
 *     assertions for this live in test_kes_cache_full.c's #if 0
 *     block (Phase 3 functions were unimplemented when that file was
 *     written) and were never flipped on, so in practice these were
 *     never actually compiled or run until now.
 *   - kes_cache_sync / kes_cache_reset_stats / kes_cache_start:
 *     NULL `cache`. Same #if-0-dead-code situation as invalidate()
 *     above -- tests/test_kes_cache.c's versions of these (Cache
 *     Sync, Cache Reset Stats, Cache Start Background Flush) do not
 *     include a NULL-cache check.
 *   - kes_cache_set_io_callbacks: NULL for each of the three function
 *     pointer parameters individually (only NULL `cache` was
 *     previously tested). Per src/kes_cache.c's actual
 *     implementation, kes_cache_set_io_callbacks() does not validate
 *     these at all -- it unconditionally stores whatever is passed,
 *     including NULL, and returns KES_SUCCESS; a NULL callback only
 *     becomes an error later, at the point something tries to use it
 *     (see test_get_extent_no_read_callback/
 *     test_flush_extent_no_write_callback in tests/test_kes_cache.c).
 *     This test asserts that actual, observed behavior rather than
 *     assuming rejection.
 */
static bool test_null_parameter_checks(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    kes_extent_id_t id = make_id( 0);
    void *buf = NULL;

    default_config( &cfg);
    cache = kes_cache_create( &cfg);
    TEST_ASSERT( cache != NULL, "cache creation for NULL-check test");
    kes_cache_set_io_callbacks( cache, mock_read, mock_write, mock_sync);
    TEST_ASSERT( kes_cache_get_extent( cache, &id, &buf) == KES_SUCCESS,
                "seed a real cached entry for id-taking checks below");

    /* pin_extent / unpin_extent: NULL id */
    TEST_ASSERT( kes_cache_pin_extent( cache, NULL) == KES_ERROR_INVALID,
                "pin_extent: NULL id rejected");
    TEST_ASSERT( kes_cache_unpin_extent( cache, NULL) == KES_ERROR_INVALID,
                "unpin_extent: NULL id rejected");

    /* mark_dirty / flush_extent: NULL id */
    TEST_ASSERT( kes_cache_mark_dirty( cache, NULL) == KES_ERROR_INVALID,
                "mark_dirty: NULL id rejected");
    TEST_ASSERT( kes_cache_flush_extent( cache, NULL) == KES_ERROR_INVALID,
                "flush_extent: NULL id rejected");

    /* invalidate: NULL cache, NULL id */
    TEST_ASSERT( kes_cache_invalidate( NULL, &id) == KES_ERROR_INVALID,
                "invalidate: NULL cache rejected");
    TEST_ASSERT( kes_cache_invalidate( cache, NULL) == KES_ERROR_INVALID,
                "invalidate: NULL id rejected");

    /* sync / reset_stats / start: NULL cache */
    TEST_ASSERT( kes_cache_sync( NULL) == KES_ERROR_INVALID,
                "sync: NULL cache rejected");
    TEST_ASSERT( kes_cache_reset_stats( NULL) == KES_ERROR_INVALID,
                "reset_stats: NULL cache rejected");
    TEST_ASSERT( kes_cache_start( NULL) == KES_ERROR_INVALID,
                "start: NULL cache rejected");

    /* set_io_callbacks: NULL for each function pointer individually.
     * Observed behavior (src/kes_cache.c): no validation on these,
     * they are stored as-is and the call still succeeds. */
    TEST_ASSERT(
        kes_cache_set_io_callbacks( cache, NULL, mock_write,
                                    mock_sync) == KES_SUCCESS,
        "set_io_callbacks: NULL read_func is accepted (deferred "
        "error at use time, not at set time)");
    TEST_ASSERT(
        kes_cache_set_io_callbacks( cache, mock_read, NULL,
                                    mock_sync) == KES_SUCCESS,
        "set_io_callbacks: NULL write_func is accepted (deferred "
        "error at use time, not at set time)");
    TEST_ASSERT(
        kes_cache_set_io_callbacks( cache, mock_read, mock_write,
                                    NULL) == KES_SUCCESS,
        "set_io_callbacks: NULL sync_func is accepted (sync_device "
        "is simply never called if NULL)");

    /* restore working callbacks before teardown */
    kes_cache_set_io_callbacks( cache, mock_read, mock_write, mock_sync);
    kes_cache_put_extent( cache, &id);
    kes_cache_destroy( cache);

    TEST_SUCCESS(
        "NULL parameter checks: pin/unpin_extent(id), "
        "mark_dirty/flush_extent(id), invalidate(cache,id), "
        "sync/reset_stats/start(cache), "
        "set_io_callbacks(read_func,write_func,sync_func)");
}

typedef struct {
    const char *name;
    bool ( *func)(void);
} test_case_t;

static test_case_t test_cases[] = {
    {"NULL parameter checks", test_null_parameter_checks},
    {NULL, NULL}
};

int main(void) {
    printf( "=== KES Cache Edge Case Coverage ===\n\n");

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
