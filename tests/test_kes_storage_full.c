#define _GNU_SOURCE  /* For ftruncate */

#include <kes/kes_storage.h>
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

#define TEST_FILE_A "/tmp/kes_storage_full_a"
#define TEST_SIZE   (2 * 1024 * 1024)   /* 2MB, block_size 4096 */

static void cleanup(void) {
    unlink( TEST_FILE_A);
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

/* kes_config_validate() */
static bool test_kes_config_validate(void) {
    kes_storage_config_t good = {
        .device_path = TEST_FILE_A, .device_size = TEST_SIZE,
        .block_size = 4096, .flags = 0, .strategy = KES_ALLOC_FIRST_FIT,
    };
    TEST_ASSERT( kes_config_validate( &good) == KES_SUCCESS,
                "valid config accepted");

    kes_storage_config_t bad_block = good;
    bad_block.block_size = 4097;   /* not a power of 2 */
    TEST_ASSERT( kes_config_validate( &bad_block) == KES_ERROR_INVALID,
                "non-power-of-2 block_size rejected");

    kes_storage_config_t bad_size = good;
    bad_size.device_size = 100;    /* smaller than block_size * 10 */
    TEST_ASSERT( kes_config_validate( &bad_size) == KES_ERROR_INVALID,
                "undersized device rejected");

    TEST_ASSERT( kes_config_validate( NULL) == KES_ERROR_INVALID,
                "NULL config rejected");

    TEST_SUCCESS( "kes_config_validate");
}

/* kes_storage_create() */
static bool test_kes_storage_create(void) {
    kes_storage_t *st = NULL;

    cleanup();
    TEST_ASSERT( make_storage( TEST_FILE_A, &st) == KES_SUCCESS,
                 "create success");
    TEST_ASSERT( st != NULL, "storage handle non-NULL");
    kes_storage_close( st);

    TEST_ASSERT( kes_storage_create( NULL, &st) == KES_ERROR_INVALID,
                "NULL config rejected");

    kes_storage_config_t cfg = {
        .device_path = TEST_FILE_A, .device_size = TEST_SIZE,
        .block_size = 4096, .flags = 0, .strategy = KES_ALLOC_FIRST_FIT,
    };
    TEST_ASSERT( kes_storage_create( &cfg, NULL) == KES_ERROR_INVALID,
                "NULL output param rejected");

    cleanup();
    TEST_SUCCESS( "kes_storage_create");
}

/* kes_storage_open() + kes_storage_close() */
static bool test_kes_storage_open_close(void) {
    kes_storage_t *st = NULL;

    cleanup();
    TEST_ASSERT( make_storage( TEST_FILE_A, &st) == KES_SUCCESS,
                "create for open test");
    TEST_ASSERT( kes_storage_close( st) == KES_SUCCESS, "close success");

    TEST_ASSERT( kes_storage_open( TEST_FILE_A, 0, &st) == KES_SUCCESS,
                "re-open success");
    TEST_ASSERT( kes_storage_close( st) == KES_SUCCESS,
                 "close after open success");

    TEST_ASSERT( kes_storage_open( NULL, 0, &st) == KES_ERROR_INVALID,
                "NULL device_path rejected");
    TEST_ASSERT( kes_storage_open( "/nonexistent/path/x", 0,
                                   &st) == KES_ERROR_IO,
                "nonexistent file reports IO error");

    TEST_ASSERT( kes_storage_close( NULL) == KES_ERROR_INVALID,
                 "close(NULL) rejected");

    cleanup();
    TEST_SUCCESS( "kes_storage_open/kes_storage_close");
}

/* kes_storage_open() rejecting a corrupted descriptor magic number */
static bool test_kes_storage_open_corrupt(void) {
    kes_storage_t *st = NULL;
    int fd;

    cleanup();
    fd = open( TEST_FILE_A, O_RDWR | O_CREAT, 0644);
    TEST_ASSERT( fd >= 0, "create raw file");
    TEST_ASSERT( ftruncate( fd, TEST_SIZE) == 0, "size raw file");
    /* leave the descriptor region zeroed -- magic won't match
     * KES_MAGIC_NUMBER */
    close( fd);

    TEST_ASSERT( kes_storage_open( TEST_FILE_A, 0, &st) == KES_ERROR_CORRUPT,
                "zeroed/garbage magic number rejected as corrupt");

    cleanup();
    TEST_SUCCESS( "kes_storage_open (corrupt descriptor)");
}

/* kes_storage_sync() */
static bool test_kes_storage_sync(void) {
    kes_storage_t *st = NULL;

    cleanup();
    TEST_ASSERT( make_storage( TEST_FILE_A, &st) == KES_SUCCESS, "create");
    TEST_ASSERT( kes_storage_sync( st) == KES_SUCCESS,
                 "sync writable storage");
    TEST_ASSERT( kes_storage_sync( NULL) == KES_ERROR_INVALID,
                 "NULL storage rejected");
    kes_storage_close( st);

    TEST_ASSERT( kes_storage_open( TEST_FILE_A, KES_STORAGE_READONLY,
                                   &st) == KES_SUCCESS,
                "re-open readonly");
    TEST_ASSERT( kes_storage_sync( st) == KES_ERROR_INVALID,
                "sync on readonly storage rejected");
    kes_storage_close( st);

    cleanup();
    TEST_SUCCESS( "kes_storage_sync");
}

/* kes_extent_allocate() */
static bool test_kes_extent_allocate(void) {
    kes_storage_t *st = NULL;
    kes_extent_descriptor_t ext;
    kes_extent_request_t req = { .block_count = 4, .alignment = 0,
                                  .hint_block = 0, .flags = 0 };

    cleanup();
    TEST_ASSERT( make_storage( TEST_FILE_A, &st) == KES_SUCCESS, "create");

    TEST_ASSERT( kes_extent_allocate( st, &req, &ext) == KES_SUCCESS,
                "allocate success");
    TEST_ASSERT( ext.block_count == 4, "block_count echoed correctly");
    TEST_ASSERT( ext.extent_id != 0, "extent_id assigned");

    kes_extent_request_t bad_req = req;
    bad_req.block_count = 0;
    TEST_ASSERT( kes_extent_allocate( st, &bad_req,
                                      &ext) == KES_ERROR_INVALID,
                "block_count == 0 rejected");
    TEST_ASSERT( kes_extent_allocate( NULL, &req, &ext) == KES_ERROR_INVALID,
                "NULL storage rejected");
    TEST_ASSERT( kes_extent_allocate( st, NULL, &ext) == KES_ERROR_INVALID,
                "NULL request rejected");
    TEST_ASSERT( kes_extent_allocate( st, &req, NULL) == KES_ERROR_INVALID,
                "NULL extent output rejected");

    /* exhaust remaining space, confirm NOSPACE, not corruption */
    kes_storage_stats_t stats;
    kes_storage_get_stats( st, &stats);
    kes_extent_request_t huge_req = {
        .block_count = (uint32_t)(stats.free_blocks + 1),
        .alignment = 0, .hint_block = 0, .flags = 0
    };
    TEST_ASSERT( kes_extent_allocate( st, &huge_req,
                                      &ext) == KES_ERROR_NOSPACE,
                "over-large request reports NOSPACE");

    kes_storage_close( st);
    cleanup();
    TEST_SUCCESS( "kes_extent_allocate");
}

/* kes_extent_free() -- success path only; double-free is fix #3's own
 * dedicated test in tests/test_kes_minimal.c, not duplicated here. */
static bool test_kes_extent_free(void) {
    kes_storage_t *st = NULL;
    kes_extent_descriptor_t ext;
    kes_extent_request_t req = { .block_count = 2, .alignment = 0,
                                  .hint_block = 0, .flags = 0 };

    cleanup();
    TEST_ASSERT( make_storage( TEST_FILE_A, &st) == KES_SUCCESS, "create");
    TEST_ASSERT( kes_extent_allocate( st, &req, &ext) == KES_SUCCESS,
                 "allocate");

    TEST_ASSERT( kes_extent_free( st, &ext) == KES_SUCCESS, "free success");
    TEST_ASSERT( kes_extent_free( NULL, &ext) == KES_ERROR_INVALID,
                "NULL storage rejected");
    TEST_ASSERT( kes_extent_free( st, NULL) == KES_ERROR_INVALID,
                "NULL extent rejected");

    kes_storage_close( st);
    TEST_ASSERT( kes_storage_open( TEST_FILE_A, KES_STORAGE_READONLY,
                                   &st) == KES_SUCCESS,
                "re-open readonly");
    TEST_ASSERT( kes_extent_free( st, &ext) == KES_ERROR_INVALID,
                "free on readonly storage rejected");
    kes_storage_close( st);

    cleanup();
    TEST_SUCCESS( "kes_extent_free");
}

/* kes_extent_read() + kes_extent_write() */
static bool test_kes_extent_read_write(void) {
    kes_storage_t *st = NULL;
    kes_extent_descriptor_t ext;
    kes_extent_request_t req = { .block_count = 2, .alignment = 0,
                                  .hint_block = 0, .flags = 0 };
    char wbuf[4096], rbuf[4096];

    memset( wbuf, 0xAB, sizeof(wbuf));
    cleanup();
    TEST_ASSERT( make_storage( TEST_FILE_A, &st) == KES_SUCCESS, "create");
    TEST_ASSERT( kes_extent_allocate( st, &req, &ext) == KES_SUCCESS,
                 "allocate");

    TEST_ASSERT( kes_extent_write( st, &ext, wbuf, sizeof(wbuf),
                                   0) == KES_SUCCESS,
                "write success");
    TEST_ASSERT( kes_extent_read( st, &ext, rbuf, sizeof(rbuf),
                                  0) == KES_SUCCESS,
                "read success");
    TEST_ASSERT( memcmp( wbuf, rbuf, sizeof(wbuf)) == 0,
                 "read matches write");

    /* bounds violation: offset + size beyond the extent's real size.
     * NOTE: the plan's original listing used offset=4096 here, which
     * assumed a 2-block extent at the requested 4096 block_size (real
     * size 8192) -- offset(4096)+size(4096) == 8192 is exactly at the
     * boundary, not past it, so that offset does not actually violate
     * bounds (and violates even less once you account for the
     * separate, pre-existing kes_storage.c bug noted in
     * test_kes_storage_get_descriptor(), which makes the real extent
     * size 2*KES_DEFAULT_BLOCK_SIZE = 16384). Use an offset far larger
     * than any plausible extent size here instead, so the assertion
     * is robust regardless of that bug. */
    TEST_ASSERT( kes_extent_read( st, &ext, rbuf, sizeof(rbuf),
                                  200000) == KES_ERROR_INVALID,
                "read past extent end rejected");
    TEST_ASSERT( kes_extent_write( st, &ext, wbuf, sizeof(wbuf),
                                   200000) == KES_ERROR_INVALID,
                "write past extent end rejected");

    TEST_ASSERT( kes_extent_read( NULL, &ext, rbuf, sizeof(rbuf),
                                  0) == KES_ERROR_INVALID,
                "read: NULL storage rejected");
    TEST_ASSERT( kes_extent_read( st, &ext, NULL, sizeof(rbuf),
                                  0) == KES_ERROR_INVALID,
                "read: NULL buffer rejected");
    TEST_ASSERT( kes_extent_write( st, &ext, NULL, sizeof(wbuf),
                                   0) == KES_ERROR_INVALID,
                "write: NULL buffer rejected");

    kes_storage_close( st);
    TEST_ASSERT( kes_storage_open( TEST_FILE_A, KES_STORAGE_READONLY,
                                   &st) == KES_SUCCESS,
                "re-open readonly");
    TEST_ASSERT( kes_extent_write( st, &ext, wbuf, sizeof(wbuf),
                                   0) == KES_ERROR_INVALID,
                "write on readonly storage rejected");
    kes_storage_close( st);

    cleanup();
    TEST_SUCCESS( "kes_extent_read/kes_extent_write");
}

/* kes_storage_get_stats() */
static bool test_kes_storage_get_stats(void) {
    kes_storage_t *st = NULL;
    kes_storage_stats_t stats;
    kes_extent_descriptor_t ext;
    kes_extent_request_t req = { .block_count = 3, .alignment = 0,
                                  .hint_block = 0, .flags = 0 };

    cleanup();
    TEST_ASSERT( make_storage( TEST_FILE_A, &st) == KES_SUCCESS, "create");
    TEST_ASSERT( kes_extent_allocate( st, &req, &ext) == KES_SUCCESS,
                 "allocate");

    TEST_ASSERT( kes_storage_get_stats( st, &stats) == KES_SUCCESS,
                 "get_stats success");
    TEST_ASSERT( stats.used_blocks == 3, "used_blocks reflects allocation");
    TEST_ASSERT( stats.allocated_extents == 1,
                 "allocated_extents reflects allocation");

    TEST_ASSERT( kes_storage_get_stats( NULL, &stats) == KES_ERROR_INVALID,
                "NULL storage rejected");
    TEST_ASSERT( kes_storage_get_stats( st, NULL) == KES_ERROR_INVALID,
                "NULL stats output rejected");

    kes_storage_close( st);
    cleanup();
    TEST_SUCCESS( "kes_storage_get_stats");
}

/* kes_storage_get_descriptor() */
static bool test_kes_storage_get_descriptor(void) {
    kes_storage_t *st = NULL;
    kes_storage_descriptor_t desc;

    cleanup();
    TEST_ASSERT( make_storage( TEST_FILE_A, &st) == KES_SUCCESS, "create");

    TEST_ASSERT( kes_storage_get_descriptor( st, &desc) == KES_SUCCESS,
                "get_descriptor success");
    TEST_ASSERT( desc.magic == KES_MAGIC_NUMBER, "magic number correct");
    /* NOTE: kes_storage_create()'s init_storage_descriptor() hardcodes
     * KES_DEFAULT_BLOCK_SIZE and never reads config->block_size (a
     * pre-existing bug in src/kes_storage.c, independent of and not
     * fixed by debugging_plan.md's 8 fixes -- out of scope for this
     * coverage-test file, flagged here instead of silently working
     * around it). So the descriptor's block_size does NOT reflect the
     * 4096 requested by make_storage()'s config; assert what the
     * implementation actually does today. */
    TEST_ASSERT( desc.block_size == KES_DEFAULT_BLOCK_SIZE,
                "block_size reflects current (buggy) behavior: always "
                "KES_DEFAULT_BLOCK_SIZE, config->block_size is ignored");

    TEST_ASSERT( kes_storage_get_descriptor( NULL,
                                             &desc) == KES_ERROR_INVALID,
                "NULL storage rejected");
    TEST_ASSERT( kes_storage_get_descriptor( st, NULL) == KES_ERROR_INVALID,
                "NULL descriptor output rejected");

    kes_storage_close( st);
    cleanup();
    TEST_SUCCESS( "kes_storage_get_descriptor");
}

/* kes_get_version() */
static bool test_kes_get_version(void) {
    uint16_t major = 0, minor = 0, patch = 0;

    kes_get_version( &major, &minor, &patch);
    TEST_ASSERT( major == KES_VERSION_MAJOR, "major matches");
    TEST_ASSERT( minor == KES_VERSION_MINOR, "minor matches");
    TEST_ASSERT( patch == KES_VERSION_PATCH, "patch matches");

    /* individual NULL out-params must be tolerated, not crash */
    kes_get_version( NULL, NULL, NULL);

    TEST_SUCCESS( "kes_get_version");
}

/* kes_get_error_string() -- KES_ERROR_BUSY assertion requires fix #1 */
static bool test_kes_get_error_string(void) {
    TEST_ASSERT( strcmp( kes_get_error_string( KES_SUCCESS), "Success") == 0,
                "KES_SUCCESS string correct");
    TEST_ASSERT( strcmp( kes_get_error_string( KES_ERROR_INVALID),
                        "Invalid parameters") == 0,
                "KES_ERROR_INVALID string correct");
    TEST_ASSERT( strcmp( kes_get_error_string( KES_ERROR_BUSY),
                         "Resource busy") == 0,
                "KES_ERROR_BUSY string correct (added by "
                "debugging_plan.md fix #1)");
    TEST_ASSERT( strcmp( kes_get_error_string( -999), "Unknown error") == 0,
                "out-of-range code reports Unknown error");

    TEST_SUCCESS( "kes_get_error_string");
}

/* kes_calculate_blocks_needed() */
static bool test_kes_calculate_blocks_needed(void) {
    TEST_ASSERT( kes_calculate_blocks_needed( 8192, 4096) == 2,
                 "exact multiple");
    TEST_ASSERT( kes_calculate_blocks_needed( 4097, 4096) == 2,
                 "remainder rounds up");
    TEST_ASSERT( kes_calculate_blocks_needed( 0, 4096) == 0,
                 "zero bytes needs zero blocks");

    TEST_SUCCESS( "kes_calculate_blocks_needed");
}

/* kes_calculate_extent_size() -- see also fix #7's dedicated overflow
 * test in tests/test_kes_minimal.c; this is the basic sanity check. */
static bool test_kes_calculate_extent_size(void) {
    kes_extent_descriptor_t ext = { .start_block = 0, .block_count = 3,
                                     .flags = 0, .extent_id = 0 };

    TEST_ASSERT(
        kes_calculate_extent_size( &ext) == 3 * KES_DEFAULT_BLOCK_SIZE,
        "size == block_count * KES_DEFAULT_BLOCK_SIZE");
    TEST_ASSERT( kes_calculate_extent_size( NULL) == 0,
                 "NULL extent returns 0");

    TEST_SUCCESS( "kes_calculate_extent_size");
}

typedef struct {
    const char *name;
    bool (*func)(void);
} test_case_t;

static test_case_t test_cases[] = {
    {"kes_config_validate",         test_kes_config_validate},
    {"kes_storage_create",          test_kes_storage_create},
    {"kes_storage_open/close",      test_kes_storage_open_close},
    {"kes_storage_open (corrupt)",  test_kes_storage_open_corrupt},
    {"kes_storage_sync",            test_kes_storage_sync},
    {"kes_extent_allocate",         test_kes_extent_allocate},
    {"kes_extent_free",             test_kes_extent_free},
    {"kes_extent_read/write",       test_kes_extent_read_write},
    {"kes_storage_get_stats",       test_kes_storage_get_stats},
    {"kes_storage_get_descriptor",  test_kes_storage_get_descriptor},
    {"kes_get_version",             test_kes_get_version},
    {"kes_get_error_string",        test_kes_get_error_string},
    {"kes_calculate_blocks_needed", test_kes_calculate_blocks_needed},
    {"kes_calculate_extent_size",   test_kes_calculate_extent_size},
    {NULL, NULL}
};

int main(void) {
    printf( "=== KES Storage Full API Coverage ===\n\n");

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
