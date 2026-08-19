#define _GNU_SOURCE  /* For ftruncate */

#include <kes/kes_bitmap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            printf( "FAIL: %s at %s:%d\n", message, __FILE__, __LINE__); \
            return(false); \
        } \
    } while (0)

#define TEST_SUCCESS(test_name) \
    do { \
        printf( "PASS: %s\n", test_name); \
        return(true); \
    } while (0)

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_BITMAP_FILE "/tmp/kes_bitmap_full_test"

/* kes_bitmap_create() */
static bool test_kes_bitmap_create(void) {
    kes_bitmap_t *bm = NULL;

    TEST_ASSERT( kes_bitmap_create( 100, &bm) == KES_SUCCESS,
                 "create success");
    TEST_ASSERT( bm != NULL, "bitmap non-NULL on success");
    kes_bitmap_destroy( bm);

    TEST_ASSERT( kes_bitmap_create( 0, &bm) == KES_ERROR_INVALID,
                "total_blocks == 0 rejected");
    TEST_ASSERT( kes_bitmap_create( 100, NULL) == KES_ERROR_INVALID,
                "NULL output param rejected");

    TEST_SUCCESS( "kes_bitmap_create");
}

/* kes_bitmap_destroy() */
static bool test_kes_bitmap_destroy(void) {
    kes_bitmap_t *bm = NULL;

    TEST_ASSERT( kes_bitmap_create( 16, &bm) == KES_SUCCESS,
                "create for destroy test");
    kes_bitmap_destroy( bm);          /* must not crash */
    kes_bitmap_destroy( NULL);        /* must not crash */

    TEST_SUCCESS( "kes_bitmap_destroy");
}

/* kes_bitmap_set() */
static bool test_kes_bitmap_set(void) {
    kes_bitmap_t *bm = NULL;
    uint64_t free_before, free_after;

    TEST_ASSERT( kes_bitmap_create( 64, &bm) == KES_SUCCESS, "create");
    kes_bitmap_get_stats( bm, NULL, &free_before, NULL);

    TEST_ASSERT( kes_bitmap_set( bm, 5) == KES_SUCCESS, "set bit 5");
    TEST_ASSERT( kes_bitmap_test( bm, 5) == true, "bit 5 now set");
    kes_bitmap_get_stats( bm, NULL, &free_after, NULL);
    TEST_ASSERT( free_after == free_before - 1, "free_bits decremented once");

    /* idempotent: setting an already-set bit must not double-decrement */
    TEST_ASSERT( kes_bitmap_set( bm, 5) == KES_SUCCESS,
                 "re-set already-set bit");
    kes_bitmap_get_stats( bm, NULL, &free_after, NULL);
    TEST_ASSERT( free_after == free_before - 1,
                 "free_bits unchanged on re-set");

    TEST_ASSERT( kes_bitmap_set( bm, 64) == KES_ERROR_INVALID,
                "out-of-range rejected");
    TEST_ASSERT( kes_bitmap_set( NULL, 0) == KES_ERROR_INVALID,
                "NULL bitmap rejected");

    kes_bitmap_destroy( bm);
    TEST_SUCCESS( "kes_bitmap_set");
}

/* kes_bitmap_clear() */
static bool test_kes_bitmap_clear(void) {
    kes_bitmap_t *bm = NULL;
    uint64_t free_before, free_after;

    TEST_ASSERT( kes_bitmap_create( 64, &bm) == KES_SUCCESS, "create");
    kes_bitmap_set( bm, 7);
    kes_bitmap_get_stats( bm, NULL, &free_before, NULL);

    TEST_ASSERT( kes_bitmap_clear( bm, 7) == KES_SUCCESS, "clear bit 7");
    TEST_ASSERT( kes_bitmap_test( bm, 7) == false, "bit 7 now clear");
    kes_bitmap_get_stats( bm, NULL, &free_after, NULL);
    TEST_ASSERT( free_after == free_before + 1, "free_bits incremented once");

    /* idempotent: clearing an already-clear bit must not double-increment */
    TEST_ASSERT( kes_bitmap_clear( bm, 7) == KES_SUCCESS,
                "re-clear already-clear bit");
    kes_bitmap_get_stats( bm, NULL, &free_after, NULL);
    TEST_ASSERT( free_after == free_before + 1,
                 "free_bits unchanged on re-clear");

    TEST_ASSERT( kes_bitmap_clear( bm, 64) == KES_ERROR_INVALID,
                "out-of-range rejected");
    TEST_ASSERT( kes_bitmap_clear( NULL, 0) == KES_ERROR_INVALID,
                "NULL bitmap rejected");

    kes_bitmap_destroy( bm);
    TEST_SUCCESS( "kes_bitmap_clear");
}

/* kes_bitmap_test() */
static bool test_kes_bitmap_test(void) {
    kes_bitmap_t *bm = NULL;

    TEST_ASSERT( kes_bitmap_create( 32, &bm) == KES_SUCCESS, "create");
    TEST_ASSERT( kes_bitmap_test( bm, 3) == false, "unset bit reads false");
    kes_bitmap_set( bm, 3);
    TEST_ASSERT( kes_bitmap_test( bm, 3) == true, "set bit reads true");

    TEST_ASSERT( kes_bitmap_test( bm, 999) == false,
                "out-of-range reads false, not a crash");
    TEST_ASSERT( kes_bitmap_test( NULL, 0) == false,
                 "NULL bitmap reads false");

    kes_bitmap_destroy( bm);
    TEST_SUCCESS( "kes_bitmap_test");
}

/* kes_bitmap_set_range() */
static bool test_kes_bitmap_set_range(void) {
    kes_bitmap_t *bm = NULL;

    TEST_ASSERT( kes_bitmap_create( 64, &bm) == KES_SUCCESS, "create");
    TEST_ASSERT( kes_bitmap_set_range( bm, 10, 5) == KES_SUCCESS,
                "set range 10..14");
    for (uint64_t i = 10; i < 15; i++) {
        TEST_ASSERT( kes_bitmap_test( bm, i) == true, "bit in range set");
    }
    TEST_ASSERT( kes_bitmap_test( bm, 15) == false,
                "bit just past range untouched");

    TEST_ASSERT( kes_bitmap_set_range( bm, 60, 10) == KES_ERROR_INVALID,
                "range exceeding total_bits rejected");
    TEST_ASSERT( kes_bitmap_set_range( NULL, 0, 1) == KES_ERROR_INVALID,
                "NULL bitmap rejected");

    kes_bitmap_destroy( bm);
    TEST_SUCCESS( "kes_bitmap_set_range");
}

/* kes_bitmap_clear_range() */
static bool test_kes_bitmap_clear_range(void) {
    kes_bitmap_t *bm = NULL;

    TEST_ASSERT( kes_bitmap_create( 64, &bm) == KES_SUCCESS, "create");
    kes_bitmap_set_range( bm, 10, 5);
    TEST_ASSERT( kes_bitmap_clear_range( bm, 10, 5) == KES_SUCCESS,
                "clear range 10..14");
    for (uint64_t i = 10; i < 15; i++) {
        TEST_ASSERT( kes_bitmap_test( bm, i) == false,
                     "bit in range cleared");
    }

    TEST_ASSERT( kes_bitmap_clear_range( bm, 60, 10) == KES_ERROR_INVALID,
                "range exceeding total_bits rejected");
    TEST_ASSERT( kes_bitmap_clear_range( NULL, 0, 1) == KES_ERROR_INVALID,
                "NULL bitmap rejected");

    kes_bitmap_destroy( bm);
    TEST_SUCCESS( "kes_bitmap_clear_range");
}

/* kes_bitmap_find_free() */
static bool test_kes_bitmap_find_free(void) {
    kes_bitmap_t *bm = NULL;
    uint64_t found = 0;

    TEST_ASSERT( kes_bitmap_create( 32, &bm) == KES_SUCCESS, "create");
    kes_bitmap_set_range( bm, 0, 10);   /* occupy the first 10 bits */

    TEST_ASSERT( kes_bitmap_find_free( bm, 4, 0, &found) == KES_SUCCESS,
                "find 4 free bits");
    TEST_ASSERT( found == 10,
                 "first gap starts right after the occupied range");

    /* fill everything, then confirm NOSPACE */
    kes_bitmap_set_range( bm, 10, 22);
    TEST_ASSERT( kes_bitmap_find_free( bm, 1, 0, &found) == KES_ERROR_NOSPACE,
                "full bitmap reports NOSPACE");

    TEST_ASSERT( kes_bitmap_find_free( bm, 0, 0, &found) == KES_ERROR_INVALID,
                "bit_count == 0 rejected");
    TEST_ASSERT( kes_bitmap_find_free( bm, 1, 0, NULL) == KES_ERROR_INVALID,
                "NULL found_start rejected");

    kes_bitmap_destroy( bm);
    TEST_SUCCESS( "kes_bitmap_find_free");
}

/* kes_bitmap_get_stats() */
static bool test_kes_bitmap_get_stats(void) {
    kes_bitmap_t *bm = NULL;
    uint64_t total = 0, free_bits = 0, used = 0;

    TEST_ASSERT( kes_bitmap_create( 40, &bm) == KES_SUCCESS, "create");
    kes_bitmap_set_range( bm, 0, 8);

    TEST_ASSERT( kes_bitmap_get_stats( bm, &total, &free_bits,
                                       &used) == KES_SUCCESS,
                "get_stats success");
    TEST_ASSERT( total == 40, "total_bits correct");
    TEST_ASSERT( used == 8, "used_bits correct");
    TEST_ASSERT( free_bits == 32, "free_bits correct");

    /* individual NULL out-params must be tolerated */
    TEST_ASSERT( kes_bitmap_get_stats( bm, NULL, NULL, NULL) == KES_SUCCESS,
                "all-NULL out-params tolerated");
    TEST_ASSERT( kes_bitmap_get_stats( NULL, &total, NULL,
                                       NULL) == KES_ERROR_INVALID,
                "NULL bitmap rejected");

    kes_bitmap_destroy( bm);
    TEST_SUCCESS( "kes_bitmap_get_stats");
}

/* kes_bitmap_save() + kes_bitmap_load() -- tested together as a round trip */
static bool test_kes_bitmap_save_load(void) {
    kes_bitmap_t *bm_a = NULL, *bm_b = NULL;
    int fd;

    unlink( TEST_BITMAP_FILE);
    fd = open( TEST_BITMAP_FILE, O_RDWR | O_CREAT, 0644);
    TEST_ASSERT( fd >= 0, "open backing file");
    TEST_ASSERT( ftruncate( fd, 4096) == 0, "size backing file");

    TEST_ASSERT( kes_bitmap_create( 64, &bm_a) == KES_SUCCESS, "create bm_a");
    kes_bitmap_set( bm_a, 1);
    kes_bitmap_set( bm_a, 9);
    kes_bitmap_set( bm_a, 63);

    TEST_ASSERT( kes_bitmap_save( bm_a, fd, 0) == KES_SUCCESS, "save bm_a");
    TEST_ASSERT( kes_bitmap_save( bm_a, -1, 0) == KES_ERROR_INVALID,
                "save rejects bad fd");
    TEST_ASSERT( kes_bitmap_save( NULL, fd, 0) == KES_ERROR_INVALID,
                "save rejects NULL bitmap");

    TEST_ASSERT( kes_bitmap_create( 64, &bm_b) == KES_SUCCESS, "create bm_b");
    TEST_ASSERT( kes_bitmap_load( bm_b, fd, 0) == KES_SUCCESS,
                 "load into bm_b");
    TEST_ASSERT( kes_bitmap_load( bm_b, -1, 0) == KES_ERROR_INVALID,
                "load rejects bad fd");
    TEST_ASSERT( kes_bitmap_load( NULL, fd, 0) == KES_ERROR_INVALID,
                "load rejects NULL bitmap");

    TEST_ASSERT( kes_bitmap_test( bm_b, 1) == true, "loaded bit 1 matches");
    TEST_ASSERT( kes_bitmap_test( bm_b, 9) == true, "loaded bit 9 matches");
    TEST_ASSERT( kes_bitmap_test( bm_b, 63) == true, "loaded bit 63 matches");
    TEST_ASSERT( kes_bitmap_test( bm_b, 2) == false,
                "untouched bit still clear after load");

    kes_bitmap_destroy( bm_a);
    kes_bitmap_destroy( bm_b);
    close( fd);
    unlink( TEST_BITMAP_FILE);

    TEST_SUCCESS( "kes_bitmap_save/kes_bitmap_load");
}

typedef struct {
    const char *name;
    bool (*func)(void);
} test_case_t;

static test_case_t test_cases[] = {
    {"kes_bitmap_create",      test_kes_bitmap_create},
    {"kes_bitmap_destroy",     test_kes_bitmap_destroy},
    {"kes_bitmap_set",         test_kes_bitmap_set},
    {"kes_bitmap_clear",       test_kes_bitmap_clear},
    {"kes_bitmap_test",        test_kes_bitmap_test},
    {"kes_bitmap_set_range",   test_kes_bitmap_set_range},
    {"kes_bitmap_clear_range", test_kes_bitmap_clear_range},
    {"kes_bitmap_find_free",   test_kes_bitmap_find_free},
    {"kes_bitmap_get_stats",   test_kes_bitmap_get_stats},
    {"kes_bitmap_save/load",   test_kes_bitmap_save_load},
    {NULL, NULL}
};

int main(void) {
    printf( "=== KES Bitmap Full API Coverage ===\n\n");

    for (test_case_t *t = test_cases; t->name != NULL; t++) {
        printf( "Running: %s... ", t->name);
        fflush( stdout);
        tests_run++;
        if (t->func()) {
            tests_passed++;
        }
    }

    printf( "\n=== Test Results ===\n");
    printf( "Tests run: %d\n", tests_run);
    printf( "Tests passed: %d\n", tests_passed);
    printf( "Tests failed: %d\n", tests_run - tests_passed);

    return((tests_passed == tests_run) ? 0 : 1);
}
