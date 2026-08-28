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
 * include/kes/kes_types.h's KES_STORAGE_SYNC flag doc comment is a
 * single line ("Synchronous I/O") with no explicit durability promise
 * either way for the *default* (non-KES_STORAGE_SYNC) case this test
 * exercises -- per plan_phase5.md's own guidance, that vagueness is
 * treated here as a Track B (docs) finding to flag, not a blocker for
 * writing this test against actually-observed behavior.
 *
 * TWO DISTINCT OBSERVED BEHAVIORS, discovered by writing this test
 * (not assumed going in):
 *
 * 1. If a storage file has NEVER been synced/closed even once since
 *    kes_storage_create() (confirmed by reading it: it calls
 *    save_storage_descriptor() for block 0, but never
 *    kes_bitmap_save() -- the bitmap region of the file is never
 *    physically written at create time at all, so the underlying
 *    file is only as large as whatever kes_extent_write() calls have
 *    reached, far short of the bitmap's offset near the end of the
 *    device), a "crash" before any sync makes the storage
 *    UNOPENABLE afterward: kes_storage_open() -> kes_bitmap_load()
 *    seeks to the (never-written) bitmap offset, reads fewer bytes
 *    than expected (past the file's real physical end), and returns
 *    KES_ERROR_IO. This is arguably safer than silent corruption --
 *    the storage refuses to open rather than proceeding with a
 *    garbage bitmap -- but it is a total-unavailability failure mode
 *    worth knowing about, not just a data-loss one.
 * 2. Once a storage file HAS been synced/closed at least once (so
 *    the file is already fully laid out on disk), a later crash
 *    WITHOUT a further sync reopens successfully, but into the
 *    STALE bookkeeping from that last sync -- silently "forgetting"
 *    any allocations/writes made since. Raw extent DATA is still
 *    always durable immediately regardless (kes_extent_write() is a
 *    direct, unbuffered write() syscall, unaffected by any
 *    storage-level sync); what is NOT durable without a sync is
 *    storage->desc.free_blocks/used_blocks and storage->bitmap,
 *    which only reach disk via kes_storage_sync()/
 *    kes_storage_close(). This test demonstrates the concrete,
 *    practical consequence of case 2: a fresh allocation after such
 *    a reopen can be handed the exact same blocks back (the bitmap
 *    thinks they are free) and silently overwrite the "forgotten"
 *    data.
 */
static bool test_no_sync_reopen_durability(void) {
    kes_storage_t *st = NULL;
    kes_extent_descriptor_t ext;
    kes_storage_stats_t stats_before, stats_after_crash;
    const char *original_data = "original-data-before-crash";
    const char *new_data = "NEW-DATA-AFTER-REALLOC";
    char read_buf[64];

    /* --- Case 1: crash before ANY sync has ever happened. --- */
    cleanup();
    TEST_ASSERT( make_storage( TEST_FILE, &st) == KES_SUCCESS,
                "create storage (case 1)");
    crash_close( st);
    st = NULL;

    TEST_ASSERT( kes_storage_open( TEST_FILE, 0, &st) ==
                    KES_ERROR_IO,
                "OBSERVED: a storage file that has never been synced/"
                "closed even once is UNOPENABLE after a crash -- "
                "kes_bitmap_load() fails reading a bitmap region that "
                "was never physically written to the file (see "
                "comment above); not the KES_ERROR_CORRUPT one might "
                "assume");
    TEST_ASSERT( st == NULL,
                "*storage was not left pointing at a partially-"
                "initialized handle on this failure");
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
                    stats_before.free_blocks,
                "OBSERVED: free_blocks reverted to the pre-allocation "
                "count -- the allocation was silently forgotten "
                "because it was never synced");
    TEST_ASSERT( stats_after_crash.used_blocks ==
                    stats_before.used_blocks,
                "OBSERVED: used_blocks likewise reverted to 0 -- the "
                "descriptor on disk is still the one "
                "kes_storage_create() originally wrote");

    /* The data itself, however, is still physically present --
     * kes_extent_write() was a direct, unbuffered write(). */
    memset( read_buf, 0, sizeof(read_buf));
    TEST_ASSERT( kes_extent_read( st, &ext, read_buf,
                                  strlen( original_data) + 1, 0) ==
                    KES_SUCCESS,
                "read via the remembered (pre-crash) extent "
                "descriptor still succeeds");
    TEST_ASSERT( strcmp( read_buf, original_data) == 0,
                "OBSERVED: the previously-written data is still "
                "physically present and readable, even though the "
                "storage's own bookkeeping has forgotten the "
                "allocation that produced it");

    /* Concrete consequence: a fresh allocation hinted at the same
     * start_block is handed the same, still-live blocks back. */
    kes_extent_descriptor_t realloc_ext;
    kes_extent_request_t realloc_req = { .block_count = 2,
                                          .alignment = 0,
                                          .hint_block = ext.start_block,
                                          .flags = 0 };
    TEST_ASSERT( kes_extent_allocate( st, &realloc_req,
                                      &realloc_ext) == KES_SUCCESS,
                "a fresh allocation after reopen succeeds");
    TEST_ASSERT( realloc_ext.start_block == ext.start_block,
                "OBSERVED HAZARD: the fresh allocation is handed the "
                "exact same start_block as the 'forgotten' extent -- "
                "the bitmap believes those blocks are free");

    TEST_ASSERT( kes_extent_write( st, &realloc_ext, new_data,
                                   strlen( new_data) + 1, 0) ==
                    KES_SUCCESS,
                "write through the new allocation");

    memset( read_buf, 0, sizeof(read_buf));
    TEST_ASSERT( kes_extent_read( st, &ext, read_buf,
                                  strlen( new_data) + 1, 0) ==
                    KES_SUCCESS,
                "read again via the ORIGINAL (pre-crash) extent "
                "descriptor");
    TEST_ASSERT( strcmp( read_buf, new_data) == 0,
                "CONFIRMED CORRUPTION: the original extent's data was "
                "silently overwritten by the new allocation -- a real, "
                "concrete consequence of unsynced allocations, not "
                "just an abstract bookkeeping curiosity");

    kes_storage_close( st);
    cleanup();
    TEST_SUCCESS( "no-explicit-sync reopen: extent DATA is always "
                  "durable (direct write()), but bitmap/descriptor "
                  "bookkeeping is only durable via sync()/close() -- "
                  "an unsynced crash can lead to real double-"
                  "allocation and silent data corruption "
                  "(KES_STORAGE_SYNC's doc comment does not make this "
                  "durability boundary explicit -- flagged as a "
                  "Track B docs gap, not fixed here)");
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
 * reopens. There is no bitmap checksum anywhere in this codebase
 * (confirmed by reading kes_bitmap_load()/kes_bitmap_save(),
 * src/kes_bitmap.c -- a plain read()/write() of the raw bytes, no
 * validation at all), so no corruption is detected at
 * kes_storage_open() time.
 *
 * Characterized precisely, by reading kes_storage_get_stats() and
 * kes_bitmap_load() first: kes_storage_get_stats()'s free_blocks/
 * used_blocks come from storage->desc (loaded from the untouched
 * descriptor block, block 0), so they still report the CORRECT,
 * pre-corruption counts. But the live in-memory bitmap
 * (kes_bitmap_load() recalculates free_bits by actually counting the
 * loaded, now-corrupted bitmap bytes -- see count_used_bits(),
 * src/kes_bitmap.c) silently disagrees with those counts by exactly
 * one bit. This is a real, silent desync between the two redundant
 * sources of truth (storage->desc's stored counters vs. the bitmap's
 * own live bit population), visible only by inspecting
 * storage->bitmap directly (kes_storage_t/kes_bitmap_t are both
 * non-opaque, same pattern tests/test_kes_storage_edge.c already
 * relies on).
 *
 * This is then demonstrated as a real double-allocation hazard, not
 * just a bookkeeping curiosity: a fresh 1-block allocation hinted at
 * the exact corrupted block is handed that SAME, still-live block
 * back by the allocator (the bitmap now believes it is free), and
 * writing through the new allocation is shown to silently overwrite
 * the original extent's still-valid data at that block.
 */
static bool test_bitflipped_bitmap_block(void) {
    kes_storage_t *st = NULL;
    kes_extent_descriptor_t ext;
    kes_storage_descriptor_t desc;
    kes_storage_stats_t stats_before, stats_after;
    uint64_t bm_total, bm_free_before, bm_used_before;
    uint64_t bm_free_after, bm_used_after;
    const char *original_data = "still-live-block-0-data";
    const char *overwrite_data = "OVERWRITTEN-BY-REALLOC";
    char read_buf[64];
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

    /* Reopen -- no corruption is detected (no bitmap checksum). */
    TEST_ASSERT( kes_storage_open( TEST_FILE, 0, &st) == KES_SUCCESS,
                "reopen succeeds -- no bitmap-level integrity check "
                "exists to catch this");

    TEST_ASSERT( kes_storage_get_stats( st, &stats_after) ==
                    KES_SUCCESS,
                "get_stats after corruption");
    TEST_ASSERT( stats_after.free_blocks == stats_before.free_blocks &&
                stats_after.used_blocks == stats_before.used_blocks,
                "OBSERVED: storage->desc-derived stats are UNCHANGED "
                "by the bitmap corruption -- they come from the "
                "untouched descriptor block, not from counting the "
                "bitmap");

    TEST_ASSERT( kes_bitmap_get_stats( st->bitmap, &bm_total,
                                       &bm_free_after,
                                       &bm_used_after) == KES_SUCCESS,
                "kes_bitmap_get_stats() on the corrupted, reloaded "
                "bitmap");
    bm_free_before = stats_before.free_blocks;
    bm_used_before = stats_before.used_blocks;
    TEST_ASSERT( bm_free_after == bm_free_before + 1,
                "OBSERVED SILENT DESYNC: the live bitmap now reports "
                "exactly one MORE free bit than storage->desc "
                "believes -- kes_bitmap_load() recalculated free_bits "
                "by counting the corrupted bytes, while desc's "
                "counters were loaded unchanged from block 0");
    TEST_ASSERT( bm_used_after == bm_used_before - 1,
                "...and correspondingly one fewer used bit, exactly "
                "matching the single flipped bit");

    /* Concrete consequence: the allocator hands the same, still-live
     * block back. */
    kes_extent_descriptor_t realloc_ext;
    kes_extent_request_t realloc_req = { .block_count = 1,
                                          .alignment = 0,
                                          .hint_block = ext.start_block,
                                          .flags = 0 };
    TEST_ASSERT( kes_extent_allocate( st, &realloc_req,
                                      &realloc_ext) == KES_SUCCESS,
                "a fresh 1-block allocation hinted at the corrupted "
                "block succeeds");
    TEST_ASSERT( realloc_ext.start_block == ext.start_block,
                "OBSERVED HAZARD: it is handed the exact same, "
                "still-live block back -- the bitmap believes it is "
                "free");

    TEST_ASSERT( kes_extent_write( st, &realloc_ext, overwrite_data,
                                   strlen( overwrite_data) + 1, 0) ==
                    KES_SUCCESS,
                "write through the new allocation");

    memset( read_buf, 0, sizeof(read_buf));
    TEST_ASSERT( kes_extent_read( st, &ext, read_buf,
                                  strlen( overwrite_data) + 1, 0) ==
                    KES_SUCCESS,
                "read again via the ORIGINAL extent descriptor");
    TEST_ASSERT( strcmp( read_buf, overwrite_data) == 0,
                "CONFIRMED CORRUPTION: the original extent's still-"
                "referenced data was silently overwritten via the "
                "reallocated block -- a real, concrete consequence of "
                "undetected bitmap corruption");

    kes_storage_close( st);
    cleanup();
    TEST_SUCCESS( "bit-flipped bitmap block: undetected at open() "
                  "time (no bitmap checksum exists), causes a silent "
                  "desync between storage->desc's stats and the live "
                  "bitmap's actual bit population, and concretely "
                  "leads to double-allocation and silent data "
                  "corruption of a still-live block");
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
