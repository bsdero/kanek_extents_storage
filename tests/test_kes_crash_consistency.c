#define _GNU_SOURCE  /* For ftruncate */

#include <kes/kes_storage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/*
 * Crash-consistency coverage for the kes_storage module -- see
 * plan_phase5.md S3 "A.5" for the exact task list this file
 * implements. `kes_storage_t` is a fully-defined struct in
 * include/kes/kes_storage.h (not opaque), so these tests reach
 * storage->fd/storage->bitmap/storage->desc directly where needed --
 * same pattern tests/test_kes_storage_edge.c already relies on.
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

#define TEST_FILE   "/tmp/kes_crash_consistency_test"
#define TEST_SIZE   (2 * 1024 * 1024)   /* 2MB */

/* init_storage_descriptor() (src/kes_storage.c) always uses
 * KES_DEFAULT_BLOCK_SIZE for the on-disk descriptor's block_size,
 * ignoring config.block_size -- confirmed by reading it, same
 * documented quirk tests/test_kes_storage_edge.c's make_storage()
 * comment already notes. Use the real runtime value for every
 * byte-offset computation below. */
#define REAL_BLOCK_SIZE   KES_DEFAULT_BLOCK_SIZE

static void cleanup(void) {
    unlink( TEST_FILE);
}

static int make_storage( const char *path, kes_storage_t **out) {
    kes_storage_config_t cfg = {
        .device_path = path,
        .device_size = TEST_SIZE,
        .block_size = REAL_BLOCK_SIZE,
        .flags = KES_STORAGE_CREATE,
        .strategy = KES_ALLOC_FIRST_FIT,
    };
    return(kes_storage_create( &cfg, out));
}

/*
 * "Crash" a storage handle: close the raw fd and free every in-memory
 * resource kes_storage_close() would free, WITHOUT any of the
 * synchronization steps kes_storage_close() performs first
 * (kes_bitmap_save()/save_storage_descriptor()/fsync()) -- confirmed
 * by reading kes_storage_close() (src/kes_storage.c) first: it always
 * runs those three steps unless storage->readonly, so simply calling
 * it would not exercise the no-sync path this test needs. This
 * reimplements just the non-syncing half of its teardown so the test
 * process itself does not leak memory (which would otherwise show up
 * as a spurious ASan/Valgrind finding unrelated to the real behavior
 * under test).
 */
static void crash_close( kes_storage_t *storage) {
    close( storage->fd);
    kes_bitmap_destroy( storage->bitmap);
    pthread_mutex_destroy( &storage->lock);
    free( storage);
}

/* ================================================================
 * A.5.1 -- no-explicit-sync reopen durability.
 *
 * Writes an extent without ever calling kes_storage_sync(), then
 * "crashes" (crash_close() above, bypassing kes_storage_close()'s
 * implicit sync entirely) rather than closing cleanly, then reopens
 * and characterizes what actually persisted.
 *
 * This used to demonstrate two distinct, real gaps:
 *
 * 1. A storage file that had NEVER been synced/closed even once since
 *    kes_storage_create() (which used to call save_storage_descriptor()
 *    for block 0, but never kes_bitmap_save()) was UNOPENABLE after a
 *    crash before any sync -- kes_bitmap_load() would seek past the
 *    file's real physical end and fail with KES_ERROR_IO.
 * 2. Once a storage file HAD been synced/closed at least once, a later
 *    crash WITHOUT a further sync reopened successfully but into
 *    STALE bookkeeping, silently "forgetting" any allocations/writes
 *    made since -- concretely, a fresh allocation after such a reopen
 *    could be handed the exact same blocks back and silently overwrite
 *    the "forgotten" data.
 *
 * KES-5 (see kes_5_plan.md) fixed both, as a direct consequence of
 * fixing cross-process storage access correctly (not a separate
 * change): kes_storage_create() now writes the full bitmap to disk
 * immediately (fixing case 1), and kes_extent_allocate()/
 * kes_extent_free() now each perform a full reload-mutate-persist
 * cycle under an exclusive flock() on every call, so every mutation is
 * durable on its own the instant it returns -- there is no longer any
 * unpersisted state left for a crash to lose (fixing case 2). This
 * test now proves both fixes.
 */
static bool test_no_sync_reopen_durability(void) {
    kes_storage_t *st = NULL;
    kes_extent_descriptor_t ext;
    kes_storage_stats_t stats_before, stats_after_crash;
    const char *original_data = "original-data-before-crash";
    char read_buf[64];

    /* --- Case 1: crash before ANY sync has ever happened. --- */
    cleanup();
    TEST_ASSERT( make_storage( TEST_FILE, &st) == KES_SUCCESS,
                "create storage (case 1)");
    crash_close( st);
    st = NULL;

    TEST_ASSERT( kes_storage_open( TEST_FILE, 0, &st) == KES_SUCCESS,
                "FIXED (KES-5): kes_storage_create() now writes the "
                "full bitmap immediately, so a crash before any "
                "explicit sync no longer leaves the file too short "
                "to reopen");
    kes_storage_stats_t stats_case1;
    TEST_ASSERT( kes_storage_get_stats( st, &stats_case1) ==
                    KES_SUCCESS,
                "get_stats on the reopened, never-allocated storage");
    TEST_ASSERT( stats_case1.used_blocks == 0,
                "a storage that crashed before any allocation reopens "
                "with nothing allocated, as expected");
    kes_storage_close( st);
    st = NULL;
    cleanup();

    /* --- Case 2: one real sync/close first (fully lays out the
     * file), THEN a crash after further unsynced changes. --- */
    TEST_ASSERT( make_storage( TEST_FILE, &st) == KES_SUCCESS,
                "create storage (case 2)");
    TEST_ASSERT( kes_storage_close( st) == KES_SUCCESS,
                "one clean close to fully lay out the file on disk");
    st = NULL;

    TEST_ASSERT( kes_storage_open( TEST_FILE, 0, &st) == KES_SUCCESS,
                "reopen the fully-laid-out file");
    TEST_ASSERT( kes_storage_get_stats( st, &stats_before) ==
                    KES_SUCCESS,
                "get_stats before allocation");

    kes_extent_request_t req = { .block_count = 2, .alignment = 0,
                                  .hint_block = 0, .flags = 0 };
    TEST_ASSERT( kes_extent_allocate( st, &req, &ext) == KES_SUCCESS,
                "allocate a 2-block extent");
    TEST_ASSERT( kes_extent_write( st, &ext, original_data,
                                   strlen( original_data) + 1, 0) ==
                    KES_SUCCESS,
                "write recognizable data into the extent");

    /* Deliberately no kes_storage_sync() call anywhere above.
     * "Crash" -- bypass kes_storage_close()'s implicit sync. */
    crash_close( st);
    st = NULL;

    /* Reopen and characterize what actually persisted. This time it
     * succeeds -- the file was already fully sized by the earlier
     * clean close, so kes_bitmap_load() has real (if stale) bytes to
     * read at the bitmap offset. */
    TEST_ASSERT( kes_storage_open( TEST_FILE, 0, &st) == KES_SUCCESS,
                "reopen after the unsynced crash (file already fully "
                "laid out from the earlier clean close)");

    TEST_ASSERT( kes_storage_get_stats( st, &stats_after_crash) ==
                    KES_SUCCESS,
                "get_stats after reopen");
    TEST_ASSERT( stats_after_crash.free_blocks ==
                    stats_before.free_blocks - 2,
                "FIXED (KES-5): the allocation is NOT forgotten -- "
                "kes_extent_allocate() persisted it immediately, so "
                "the crash afterward lost nothing");
    TEST_ASSERT( stats_after_crash.used_blocks ==
                    stats_before.used_blocks + 2,
                "used_blocks correctly reflects the allocation that "
                "survived the crash");

    /* The data is still physically present, as before. */
    memset( read_buf, 0, sizeof(read_buf));
    TEST_ASSERT( kes_extent_read( st, &ext, read_buf,
                                  strlen( original_data) + 1, 0) ==
                    KES_SUCCESS,
                "read via the remembered (pre-crash) extent "
                "descriptor still succeeds");
    TEST_ASSERT( strcmp( read_buf, original_data) == 0,
                "the previously-written data is still physically "
                "present and readable");

    /* FIXED (KES-5): a fresh allocation hinted at the same block is
     * NOT handed the same, still-live block back -- the bitmap
     * correctly still marks it used, because the reopen above
     * reloaded the real, persisted state. */
    kes_extent_descriptor_t realloc_ext;
    kes_extent_request_t realloc_req = { .block_count = 2,
                                          .alignment = 0,
                                          .hint_block = ext.start_block,
                                          .flags = 0 };
    TEST_ASSERT( kes_extent_allocate( st, &realloc_req,
                                      &realloc_ext) == KES_SUCCESS,
                "a fresh allocation after reopen still succeeds "
                "(there is free space elsewhere)");
    TEST_ASSERT( realloc_ext.start_block != ext.start_block,
                "FIXED (KES-5): the fresh allocation is NOT handed "
                "the original extent's still-live start_block -- the "
                "bitmap correctly knows those blocks are still used");

    /* Confirm the original data was NOT touched by the new
     * allocation. */
    memset( read_buf, 0, sizeof(read_buf));
    TEST_ASSERT( kes_extent_read( st, &ext, read_buf,
                                  strlen( original_data) + 1, 0) ==
                    KES_SUCCESS,
                "read again via the original extent descriptor");
    TEST_ASSERT( strcmp( read_buf, original_data) == 0,
                "FIXED (KES-5): the original extent's data is "
                "UNCHANGED -- no double-allocation occurred");

    kes_storage_close( st);
    cleanup();
    TEST_SUCCESS( "no-explicit-sync reopen: FIXED by KES-5 -- every "
                  "kes_extent_allocate()/kes_extent_free() call is "
                  "now durable on its own, so a crash between an "
                  "allocation and an explicit sync no longer loses "
                  "bookkeeping or causes double-allocation");
}

/* ================================================================
 * A.5.2 -- truncated/corrupted descriptor detection.
 *
 * Two distinct cases, confirmed to return two DIFFERENT error codes
 * by reading load_storage_descriptor() (src/kes_storage.c) first:
 *
 *   1. Truncating the file to fewer bytes than
 *      sizeof(kes_storage_descriptor_t): the read() byte-count check
 *      fails ("bytes_read != sizeof(...)") BEFORE the magic-number
 *      check is ever reached, so this returns KES_ERROR_IO -- NOT
 *      KES_ERROR_CORRUPT, which a caller distinguishing "corrupt"
 *      from "truncated/missing" purely by return code should know.
 *   2. Overwriting just the 4-byte magic-number field in an
 *      otherwise-intact, correctly-sized descriptor: the byte-count
 *      check passes (a full descriptor's worth of bytes was read),
 *      but the magic check fails, returning KES_ERROR_CORRUPT as
 *      documented -- already covered narrowly by
 *      test_kes_storage_open_corrupt() in
 *      tests/test_kes_storage_full.c (zeroed/garbage magic), NOT
 *      duplicated here in the exact same form; this test instead
 *      pairs it directly against case 1's different error code and a
 *      distinct corruption byte pattern for contrast.
 *
 * Either way, kes_storage_open() must fail cleanly (no crash, no
 * *storage output) rather than proceeding with uninitialized/garbage
 * geometry -- confirmed for both cases below.
 */
static bool test_truncated_and_corrupted_descriptor(void) {
    kes_storage_t *st = NULL;
    int fd;

    /* --- Case 1: truncate below sizeof(descriptor). --- */
    cleanup();
    TEST_ASSERT( make_storage( TEST_FILE, &st) == KES_SUCCESS,
                "create storage (truncation case)");
    TEST_ASSERT( kes_storage_close( st) == KES_SUCCESS,
                "clean close to lay out a valid file first");
    st = NULL;

    fd = open( TEST_FILE, O_WRONLY);
    TEST_ASSERT( fd >= 0, "reopen raw fd to truncate");
    TEST_ASSERT( ftruncate( fd, 10) == 0,
                "truncate to 10 bytes -- well under "
                "sizeof(kes_storage_descriptor_t)");
    close( fd);

    TEST_ASSERT( kes_storage_open( TEST_FILE, 0, &st) ==
                    KES_ERROR_IO,
                "OBSERVED: a truncated (< one full descriptor) file "
                "returns KES_ERROR_IO, not KES_ERROR_CORRUPT -- the "
                "byte-count check in load_storage_descriptor() fails "
                "before the magic-number check is ever reached");
    TEST_ASSERT( st == NULL,
                "*storage was not left pointing at a partially-"
                "initialized handle");
    cleanup();

    /* --- Case 2: intact size, corrupted magic number. --- */
    TEST_ASSERT( make_storage( TEST_FILE, &st) == KES_SUCCESS,
                "create storage (magic-corruption case)");
    TEST_ASSERT( kes_storage_close( st) == KES_SUCCESS,
                "clean close to lay out a valid, full-size file");
    st = NULL;

    fd = open( TEST_FILE, O_WRONLY);
    TEST_ASSERT( fd >= 0, "reopen raw fd to corrupt the magic number");
    uint32_t garbage_magic = 0xDEADBEEF;
    TEST_ASSERT( lseek( fd, 0, SEEK_SET) == 0, "seek to block 0");
    TEST_ASSERT( write( fd, &garbage_magic, sizeof(garbage_magic)) ==
                    (ssize_t)sizeof(garbage_magic),
                "overwrite only the 4-byte magic field, leaving the "
                "rest of the descriptor (and the whole file) intact");
    close( fd);

    TEST_ASSERT( kes_storage_open( TEST_FILE, 0, &st) ==
                    KES_ERROR_CORRUPT,
                "an intact-size descriptor with a bad magic number "
                "returns KES_ERROR_CORRUPT, as documented");
    TEST_ASSERT( st == NULL,
                "*storage was not left pointing at a partially-"
                "initialized handle on this failure either");

    cleanup();
    TEST_SUCCESS( "truncated/corrupted descriptor detection: "
                  "truncation -> KES_ERROR_IO, bad magic -> "
                  "KES_ERROR_CORRUPT -- two different codes for two "
                  "different failure shapes, both rejected cleanly "
                  "before any bitmap/geometry use");
}

/* ================================================================
 * A.5.3 -- bit-flipped bitmap block.
 *
 * Flips exactly one bit (1 -> 0, i.e. "used" -> "free") in the
 * ON-DISK BITMAP region (not the descriptor) for a block that is
 * genuinely still allocated and holds live written data, then
 * reopens.
 *
 * This used to go completely undetected: there was no bitmap checksum
 * anywhere in this codebase (kes_bitmap_load()/kes_bitmap_save() did a
 * plain read()/write() of the raw bytes, no validation at all), so
 * kes_storage_open() would succeed despite the corruption, producing a
 * silent desync between storage->desc's stored counters and the live
 * bitmap's actual bit population, and concretely handing the
 * allocator's next request the same still-live, already-allocated
 * block back -- a real double-allocation and silent-data-corruption
 * hazard, not just a bookkeeping curiosity.
 *
 * KES-6 (see kes_6_plan.md) fixed this by adding a bitmap_checksum
 * field (CRC-32C of the on-disk bitmap region) to
 * kes_storage_descriptor_t, refreshed on every save and verified on
 * every kes_storage_open(). This test now proves the corruption IS
 * detected: kes_storage_open() fails with KES_ERROR_CORRUPT rather
 * than silently trusting the corrupted bytes, so none of the old
 * double-allocation/silent-overwrite demonstration is reachable
 * anymore.
 */
static bool test_bitflipped_bitmap_block(void) {
    kes_storage_t *st = NULL;
    kes_extent_descriptor_t ext;
    kes_storage_descriptor_t desc;
    kes_storage_stats_t stats_before;
    const char *original_data = "still-live-block-0-data";
    off_t bit_byte_offset;
    unsigned char bit_byte;
    unsigned char bit_mask;
    int fd;

    cleanup();
    TEST_ASSERT( make_storage( TEST_FILE, &st) == KES_SUCCESS,
                "create storage");

    kes_extent_request_t req = { .block_count = 2, .alignment = 0,
                                  .hint_block = 0, .flags = 0 };
    TEST_ASSERT( kes_extent_allocate( st, &req, &ext) == KES_SUCCESS,
                "allocate a 2-block extent");
    TEST_ASSERT( kes_extent_write( st, &ext, original_data,
                                   strlen( original_data) + 1, 0) ==
                    KES_SUCCESS,
                "write recognizable data into block 0 of the extent");

    TEST_ASSERT( kes_storage_get_descriptor( st, &desc) == KES_SUCCESS,
                "get descriptor for bitmap layout");
    TEST_ASSERT( kes_storage_get_stats( st, &stats_before) ==
                    KES_SUCCESS,
                "get_stats before corruption");

    /* Clean close -- fully persists the descriptor and the (correct,
     * un-corrupted) bitmap to disk. */
    TEST_ASSERT( kes_storage_close( st) == KES_SUCCESS,
                "clean close to persist a valid bitmap");
    st = NULL;

    /* Flip exactly the bit for ext.start_block (the extent's first
     * block) from used (1) to free (0), directly in the file. */
    bit_byte_offset = (off_t)( desc.bitmap_start_block *
                               desc.block_size +
                               ext.start_block / 8);
    bit_mask = (unsigned char)( 1U << ( ext.start_block % 8));

    fd = open( TEST_FILE, O_RDWR);
    TEST_ASSERT( fd >= 0, "reopen raw fd to corrupt the bitmap");
    TEST_ASSERT( lseek( fd, bit_byte_offset, SEEK_SET) ==
                    bit_byte_offset,
                "seek to the byte covering ext.start_block's bit");
    TEST_ASSERT( read( fd, &bit_byte, 1) == 1, "read that byte");
    TEST_ASSERT( ( bit_byte & bit_mask) != 0,
                "sanity: the bit is currently set (block genuinely "
                "allocated) before we flip it");
    bit_byte &= (unsigned char)~bit_mask;
    TEST_ASSERT( lseek( fd, bit_byte_offset, SEEK_SET) ==
                    bit_byte_offset,
                "seek back to write the flipped byte");
    TEST_ASSERT( write( fd, &bit_byte, 1) == 1,
                "write the corrupted (bit cleared) byte back");
    close( fd);

    /* Reopen -- KES-6's bitmap checksum now catches this. */
    TEST_ASSERT( kes_storage_open( TEST_FILE, 0, &st) ==
                    KES_ERROR_CORRUPT,
                "FIXED (KES-6): a single-bit-flipped bitmap block is "
                "now detected at open() time via the descriptor's "
                "bitmap_checksum field, and kes_storage_open() "
                "correctly refuses to proceed rather than silently "
                "trusting corrupted bytes");
    TEST_ASSERT( st == NULL,
                "*storage was not left pointing at a partially-"
                "initialized handle on this failure");

    cleanup();
    TEST_SUCCESS( "bit-flipped bitmap block: detected at open() time "
                  "via KES-6's bitmap checksum, refusing to open "
                  "rather than risking the double-allocation/silent-"
                  "corruption hazard this test used to demonstrate");
}

typedef struct {
    const char *name;
    bool ( *func)(void);
} test_case_t;

static test_case_t test_cases[] = {
    {"no-sync reopen durability", test_no_sync_reopen_durability},
    {"truncated/corrupted descriptor detection",
     test_truncated_and_corrupted_descriptor},
    {"bit-flipped bitmap block", test_bitflipped_bitmap_block},
    {NULL, NULL}
};

int main(void) {
    printf( "=== KES Storage Crash Consistency Coverage ===\n\n");

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
