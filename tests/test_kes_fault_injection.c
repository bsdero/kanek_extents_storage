#define _GNU_SOURCE

#include <kes/kes_cache.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

/*
 * Fault-injection coverage for the kes_cache module -- see
 * plan_phase5.md S3 "A.4" for the exact task list this file
 * implements. None of this existed before this pass (confirmed:
 * tests/test_kes_cache.c only has mock_read_extent_always_fail, an
 * unconditional-failure mock, not a configurable one).
 *
 * Uses a small, self-contained configurable-failure mock I/O harness
 * (fi_control_t / fi_should_fail() below) rather than reusing
 * anything from tests/test_kes_cache.c, per plan_phase5.md's
 * instruction that this harness "can live in this new file, doesn't
 * need to be shared."
 *
 * A.4.1's flush-failure sub-item is driven through kes_cache_sync()
 * (-> cache_sweep() -> sweep_flush_and_maybe_evict(),
 * src/kes_cache.c), matching KES_HARDENING_PLAN.md S4.1's literal
 * text ("On failure, set KES_EXTENT_ERROR, leave dirty set") -- that
 * text is specifically about kes_cache_sync()'s sweep path. Note:
 * kes_cache_flush_extent() (a direct, single-entry flush call) is a
 * DIFFERENT code path in src/kes_cache.c that does NOT set
 * KES_EXTENT_ERROR on a write_extent() failure -- it just unlocks and
 * returns KES_ERROR_IO, leaving state otherwise untouched. This is a
 * real behavioral difference between the two flush paths, distinct
 * from anything already tracked in PENDING_ITEMS.md; noted here and
 * in this pass's final report rather than silently working around it,
 * per rule 0.3 -- not fixed, since production-code changes are out of
 * scope for this file.
 *
 * A.4.2 (malloc/aligned_alloc failure simulation) requests a
 * genuinely oversized extent (200GiB) that a real allocator legitimately
 * refuses (confirmed empirically on this system: aligned_alloc()
 * reliably returns NULL/ENOMEM for requests far smaller than this,
 * e.g. 100GiB, well before this file was written) rather than using
 * LD_PRELOAD-based allocator interposition, per plan_phase5.md's own
 * "most portable approach" guidance. IMPORTANT ASan interaction,
 * confirmed empirically: AddressSanitizer's default behavior for ANY
 * out-of-memory allocation failure (not just requests over its
 * internal max-supported-size cap) is to print a report and
 * ABORT the process, not return NULL -- ASAN_OPTIONS=
 * allocator_may_return_null=1 makes it return NULL instead, matching
 * plain glibc behavior. That option must be set in the environment
 * BEFORE process start (setenv() from inside main() is too late --
 * confirmed empirically, ASan parses ASAN_OPTIONS during its own
 * pre-main constructor). The Makefile's `asan` target sets this
 * environment variable specifically when running this one test
 * binary (see the comment there) so `make asan` can exercise this
 * test's real KES_ERROR_NOMEM path without the whole gate aborting on
 * an intentional, expected allocation failure.
 *
 * A.4.3 (partial read/write simulation): see
 * test_partial_transfer_not_detected()'s comment below for the
 * documented conclusion -- byte-count verification against a short
 * transfer is NOT this layer's job, because the read_extent/
 * write_extent callback contract (kes_cache.h) has no
 * bytes-actually-transferred output channel at all, only a plain int
 * status code against a fixed `size` input.
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

#define FI_BLOCK_SIZE   4096
#define FI_NUM_BLOCKS   64

static unsigned char g_backing[FI_NUM_BLOCKS][FI_BLOCK_SIZE];

/* ================================================================
 * Configurable-failure mock I/O harness (A.4.1's first sub-item).
 * ================================================================ */

typedef enum {
    FI_MODE_NONE = 0,     /* never fail */
    FI_MODE_FAIL_NTH,     /* fail exactly on call number target_n
                            * (1-based) */
    FI_MODE_FAIL_AFTER    /* fail on every call after call number
                            * target_n (target_n == 0 means "fail
                            * every call") */
} fi_mode_t;

typedef struct {
    fi_mode_t mode;
    int target_n;
    int call_count;
} fi_control_t;

static fi_control_t g_read_fault;
static fi_control_t g_write_fault;

static void fi_reset( fi_control_t *fc) {
    fc->mode = FI_MODE_NONE;
    fc->target_n = 0;
    fc->call_count = 0;
}

static bool fi_should_fail( fi_control_t *fc) {
    fc->call_count++;

    switch ( fc->mode) {
    case FI_MODE_FAIL_NTH:
        return( fc->call_count == fc->target_n);
    case FI_MODE_FAIL_AFTER:
        return( fc->call_count > fc->target_n);
    case FI_MODE_NONE:
    default:
        return( false);
    }
}

static int fi_read( void *dev, const kes_extent_id_t *id, void *buf,
                     size_t size) {
    (void)dev;

    if ( fi_should_fail( &g_read_fault)) {
        return(KES_ERROR_IO);
    }
    if ( id->start_block >= FI_NUM_BLOCKS || size > FI_BLOCK_SIZE) {
        return(KES_ERROR_IO);
    }
    memcpy( buf, g_backing[id->start_block], size);
    return(KES_SUCCESS);
}

static int fi_write( void *dev, const kes_extent_id_t *id,
                      const void *buf, size_t size) {
    (void)dev;

    if ( fi_should_fail( &g_write_fault)) {
        return(KES_ERROR_IO);
    }
    if ( id->start_block >= FI_NUM_BLOCKS || size > FI_BLOCK_SIZE) {
        return(KES_ERROR_IO);
    }
    memcpy( g_backing[id->start_block], buf, size);
    return(KES_SUCCESS);
}

static int fi_sync( void *dev) {
    (void)dev;
    return(KES_SUCCESS);
}

static kes_extent_id_t make_id( uint64_t start_block) {
    kes_extent_id_t id = { .start_block = start_block, .block_count = 1,
                            .block_size = FI_BLOCK_SIZE, .reserved = 0 };
    return(id);
}

/*
 * Test-only helper: locate a live cache entry by id and return a
 * pointer to it, or NULL if not currently in the hash table.
 * kes_cache_t/kes_extent_entry_t are fully defined in
 * include/kes/kes_cache.h (not just forward-declared), same pattern
 * tests/test_kes_cache.c's get_entry_ref_count() helper already
 * relies on -- this adds no new production code and changes no
 * visibility rules.
 */
static kes_extent_entry_t *
find_entry( kes_cache_t *cache, const kes_extent_id_t *id) {
    uint32_t hash = kes_extent_hash( id);
    uint32_t bucket_idx = hash & cache->bucket_mask;
    kes_extent_entry_t *entry = cache->buckets[bucket_idx].head;

    while ( entry != NULL) {
        if ( kes_extent_equal( &entry->id, id)) {
            return(entry);
        }
        entry = entry->hash_next;
    }
    return(NULL);
}

/* ================================================================
 * A.4.1 (first sub-item) -- load failure must not leave a corrupted
 * or half-inserted entry in the hash table.
 *
 * tests/test_kes_cache.c's test_ref_count_leak_on_load_failure() and
 * test_ref_count_actually_released_on_load_failure() already cover
 * the ref_count half of this (a failed load's phantom ref_count is
 * correctly released back to 0) -- NOT duplicated here. This test is
 * specifically about hash-table entry state and retry-after-failure.
 *
 * Reading src/kes_cache.c's miss path end to end: on a load failure,
 * kes_cache_get_extent() sets entry->state = KES_EXTENT_ERROR and
 * releases the phantom ref_count, but the entry itself is NOT removed
 * from the hash table or LRU list -- it stays there permanently.
 * stats.entries_cached was already incremented (unconditionally,
 * before the load was even attempted), so it now counts an entry that
 * will never again be retrievable through the normal hit path.
 *
 * REAL, OBSERVED GAP (reported per rule 0.3, NOT fixed here --
 * production-code changes are out of scope for this file; see this
 * pass's final report and the matching PENDING_ITEMS.md note): a
 * second kes_cache_get_extent() call on the same id, even with the
 * failure condition fully cleared, does NOT retry the load. The hit
 * path (src/kes_cache.c) checks `entry->state & KES_EXTENT_ERROR`
 * and returns KES_ERROR_IO immediately, without ever calling
 * read_extent() again -- the entry is permanently stuck until a
 * caller explicitly calls kes_cache_invalidate() on that exact id (a
 * plausible, but currently undocumented, required recovery step) to
 * remove it, after which a fresh get_extent() succeeds normally. This
 * test demonstrates and asserts all of that as actually-observed
 * behavior, including the successful invalidate()+retry recovery
 * path, and confirms the hash table itself is not corrupted (the
 * stuck state is confined to that one entry).
 */
static bool test_load_failure_hash_table_state(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    void *buf = NULL;
    kes_extent_id_t id = make_id( 10);
    kes_cache_stats_t stats;
    kes_extent_entry_t *entry;
    int result;

    kes_cache_get_default_config( &cfg, true);
    cache = kes_cache_create( &cfg);
    TEST_ASSERT( cache != NULL, "cache creation");
    kes_cache_set_io_callbacks( cache, fi_read, fi_write, fi_sync);

    fi_reset( &g_read_fault);
    fi_reset( &g_write_fault);
    g_read_fault.mode = FI_MODE_FAIL_NTH;
    g_read_fault.target_n = 1;    /* the one and only load attempt
                                    * below fails */

    result = kes_cache_get_extent( cache, &id, &buf);
    TEST_ASSERT( result == KES_ERROR_IO,
                "load failure returns KES_ERROR_IO cleanly");
    TEST_ASSERT( buf == NULL, "buffer stays NULL on load failure");

    kes_cache_get_stats( cache, &stats);
    TEST_ASSERT( stats.entries_cached == 1,
                "OBSERVED GAP: entries_cached still counts the failed "
                "entry even though it is not retrievable -- see this "
                "file's header comment");

    entry = find_entry( cache, &id);
    TEST_ASSERT( entry != NULL,
                "the entry itself is still present in the hash table "
                "(not removed on load failure)");
    TEST_ASSERT( entry->state == KES_EXTENT_ERROR,
                "entry is left in exactly KES_EXTENT_ERROR state");
    TEST_ASSERT( entry->ref_count == 0,
                "phantom ref_count was released back to 0 (already "
                "covered by tests/test_kes_cache.c, reconfirmed here)");

    /* Clear the failure condition and retry -- observed behavior:
     * this does NOT succeed, because the entry is still present and
     * still in KES_EXTENT_ERROR state. */
    fi_reset( &g_read_fault);
    buf = NULL;
    result = kes_cache_get_extent( cache, &id, &buf);
    TEST_ASSERT( result == KES_ERROR_IO,
                "OBSERVED GAP: retry with the failure condition "
                "cleared still returns KES_ERROR_IO -- read_extent() "
                "is never called again for this id (see header "
                "comment; g_read_fault.call_count below proves it)");
    TEST_ASSERT( buf == NULL, "buffer stays NULL on the stuck retry");
    TEST_ASSERT( g_read_fault.call_count == 0,
                "proof the retry never even attempted a real read: "
                "the (already-cleared) fault harness saw zero calls");

    /* Only recovery path: explicit invalidate() first. ref_count is
     * already 0 (verified above), so this is not BUSY. */
    result = kes_cache_invalidate( cache, &id);
    TEST_ASSERT( result == KES_SUCCESS,
                "invalidate() removes the stuck ERROR entry");

    buf = NULL;
    result = kes_cache_get_extent( cache, &id, &buf);
    TEST_ASSERT( result == KES_SUCCESS,
                "after invalidate(), a fresh get_extent() on the same "
                "id succeeds normally -- confirms the hash table "
                "itself was never corrupted, only the one entry was "
                "stuck");
    TEST_ASSERT( buf != NULL, "buffer non-NULL on the recovered get");
    kes_cache_put_extent( cache, &id);

    kes_cache_get_stats( cache, &stats);
    TEST_ASSERT( stats.entries_cached == 1,
                "exactly one (the recovered) entry cached now");

    kes_cache_destroy( cache);
    TEST_SUCCESS( "load-failure hash-table state: entry survives in "
                  "KES_EXTENT_ERROR state without corrupting the "
                  "table, but is NOT auto-retried on a cleared "
                  "failure condition -- real gap reported, not fixed "
                  "(see header comment, final report, "
                  "PENDING_ITEMS.md)");
}

/* ================================================================
 * A.4.1 (second sub-item) -- a flush failure (write_extent fails)
 * must leave the entry's dirty flag set and KES_EXTENT_ERROR state,
 * per KES_HARDENING_PLAN.md S4.1 point 1.
 *
 * tests/test_kes_cache.c's test_cache_sync()/
 * test_cache_invalidate_discards_dirty_data() only exercise
 * successful writes -- neither configures write_extent to fail, so
 * this is genuinely new coverage, not a duplicate.
 *
 * Driven through kes_cache_sync() specifically (not
 * kes_cache_flush_extent() -- see this file's header comment for why
 * those two flush paths behave differently on failure), matching
 * S4.1's literal text.
 */
static bool test_flush_failure_dirty_state(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    void *buf = NULL;
    kes_extent_id_t id = make_id( 11);
    kes_cache_stats_t stats;
    kes_extent_entry_t *entry;
    int result;

    kes_cache_get_default_config( &cfg, true);
    cache = kes_cache_create( &cfg);
    TEST_ASSERT( cache != NULL, "cache creation");
    kes_cache_set_io_callbacks( cache, fi_read, fi_write, fi_sync);

    fi_reset( &g_read_fault);
    fi_reset( &g_write_fault);

    result = kes_cache_get_extent( cache, &id, &buf);
    TEST_ASSERT( result == KES_SUCCESS, "get extent to dirty it");
    memset( buf, 0xCD, FI_BLOCK_SIZE);
    TEST_ASSERT( kes_cache_mark_dirty( cache, &id) == KES_SUCCESS,
                "mark dirty");
    TEST_ASSERT( kes_cache_put_extent( cache, &id) == KES_SUCCESS,
                "put -- ref_count back to 0, still dirty");

    /* Every write_extent call from now on fails. */
    g_write_fault.mode = FI_MODE_FAIL_AFTER;
    g_write_fault.target_n = 0;

    result = kes_cache_sync( cache);
    TEST_ASSERT( result == KES_SUCCESS,
                "sync() itself still returns KES_SUCCESS -- it does "
                "not propagate a per-entry flush failure as its own "
                "return code (matches kes_cache_sync()'s existing, "
                "unconditional `return(KES_SUCCESS)`)");

    kes_cache_get_stats( cache, &stats);
    TEST_ASSERT( stats.entries_dirty == 1,
                "dirty flag is still set after the failed flush -- "
                "S4.1's documented behavior");
    TEST_ASSERT( stats.entries_cached == 1,
                "the entry survives the failed flush -- "
                "try_evict_entry_locked()'s discard_dirty=false path "
                "refuses to evict a DIRTY-or-ERROR entry");
    TEST_ASSERT( stats.flushes == 0, "no successful flush was counted");

    entry = find_entry( cache, &id);
    TEST_ASSERT( entry != NULL, "entry still present after failed sync");
    TEST_ASSERT( (entry->state & KES_EXTENT_DIRTY) != 0,
                "KES_EXTENT_DIRTY bit is still set directly on the "
                "entry struct");
    TEST_ASSERT( (entry->state & KES_EXTENT_ERROR) != 0,
                "KES_EXTENT_ERROR bit was set by the failed flush, "
                "exactly per S4.1 point 1");

    /* Recovery: clear the fault and sync() again -- the entry should
     * flush successfully and be freed (unreferenced, unpinned). */
    fi_reset( &g_write_fault);
    result = kes_cache_sync( cache);
    TEST_ASSERT( result == KES_SUCCESS, "recovery sync() succeeds");

    kes_cache_get_stats( cache, &stats);
    TEST_ASSERT( stats.entries_dirty == 0,
                "dirty flag cleared once the flush actually succeeds");
    TEST_ASSERT( stats.flushes == 1, "exactly one successful flush now");
    TEST_ASSERT( stats.entries_cached == 0,
                "entry freed by sync() once clean, unreferenced, "
                "unpinned -- matches normal test_cache_sync() "
                "behavior in tests/test_kes_cache.c");

    uint8_t expected[FI_BLOCK_SIZE];
    memset( expected, 0xCD, FI_BLOCK_SIZE);
    TEST_ASSERT( memcmp( g_backing[11], expected, FI_BLOCK_SIZE) == 0,
                "the dirty data was actually written back once the "
                "fault was cleared -- not silently discarded while "
                "stuck in ERROR state");

    kes_cache_destroy( cache);
    TEST_SUCCESS( "flush-failure dirty state: KES_EXTENT_DIRTY and "
                  "KES_EXTENT_ERROR are both set on a failed sync() "
                  "flush, entry is not evicted, and a later successful "
                  "sync() recovers cleanly (S4.1 point 1)");
}

/* ================================================================
 * A.4.2 -- malloc/aligned_alloc failure simulation.
 *
 * Requests a genuinely oversized (200GiB) extent against a cache
 * configured with a large enough max_memory to pass
 * make_room_for_new_entry()'s pre-allocation capacity check (already
 * exercised at a smaller, capacity-rejected scale by A.1.3's
 * UINT32_MAX case -- that one never reaches aligned_alloc() at all;
 * this one is specifically designed to reach it and have it actually
 * fail). 200GiB was chosen empirically: on this system, even 100GiB
 * reliably fails with a real ENOMEM from aligned_alloc(), while
 * 200GiB stays comfortably under AddressSanitizer's own internal
 * max-supported-size cap (empirically ~1TiB on this system/compiler),
 * so it exercises a genuine allocator-refuses-the-request path rather
 * than ASan's separate "request too big to even consider" static
 * check.
 *
 * See this file's header comment for the ASAN_OPTIONS=
 * allocator_may_return_null=1 requirement this test relies on under
 * `make asan` -- set by the Makefile's `asan` target specifically for
 * this binary, not globally.
 */
#define FI_HUGE_BLOCK_SIZE   KES_MAX_BLOCK_SIZE
#define FI_HUGE_BLOCK_COUNT  3276800u   /* 3276800 * 65536 == 200GiB */

static bool test_malloc_failure_nomem(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    void *buf = (void *)0x1;   /* sentinel: must become NULL */
    kes_extent_id_t huge = { .start_block = 99,
                              .block_count = FI_HUGE_BLOCK_COUNT,
                              .block_size = FI_HUGE_BLOCK_SIZE,
                              .reserved = 0 };
    kes_extent_id_t small = make_id( 20);
    kes_cache_stats_t stats;
    int result;

    memset( &cfg, 0, sizeof(cfg));
    cfg.max_memory = 500ULL * 1024 * 1024 * 1024;   /* 500GiB: passes
                                                       * the capacity
                                                       * check, so the
                                                       * miss path
                                                       * really reaches
                                                       * aligned_alloc()
                                                       */
    cfg.min_memory = KES_CACHE_MIN_MEMORY;
    cfg.max_entries = KES_CACHE_MIN_ENTRIES;
    cfg.policy = KES_CACHE_LRU;
    cfg.background_threads = 1;
    cfg.sync_interval_ms = 1000;

    cache = kes_cache_create( &cfg);
    TEST_ASSERT( cache != NULL, "cache creation for NOMEM test");
    kes_cache_set_io_callbacks( cache, fi_read, fi_write, fi_sync);

    result = kes_cache_get_extent( cache, &huge, &buf);
    TEST_ASSERT( result == KES_ERROR_NOMEM,
                "a 200GiB extent request, past the capacity check but "
                "genuinely too large for the allocator, cleanly "
                "returns KES_ERROR_NOMEM");
    TEST_ASSERT( buf == NULL,
                "*buffer is set to NULL, not left as a stale/garbage "
                "pointer");

    kes_cache_get_stats( cache, &stats);
    TEST_ASSERT( stats.entries_cached == 0,
                "no partial entry was left in entries_cached");
    TEST_ASSERT( stats.memory_used == 0,
                "no partial memory_used accounting leaked");
    TEST_ASSERT( stats.misses == 0,
                "misses was not incremented -- the failure happened "
                "before the candidate was ever published");

    /* Cache remains fully usable afterward -- no corruption from the
     * failed attempt. */
    void *small_buf = NULL;
    result = kes_cache_get_extent( cache, &small, &small_buf);
    TEST_ASSERT( result == KES_SUCCESS,
                "a normal-sized extent still works after the NOMEM "
                "failure -- cache state was not corrupted");
    TEST_ASSERT( small_buf != NULL, "normal buffer is non-NULL");
    kes_cache_put_extent( cache, &small);

    kes_cache_destroy( cache);
    TEST_SUCCESS( "malloc/aligned_alloc failure: KES_ERROR_NOMEM "
                  "propagates cleanly with no partial-state leak "
                  "(run under make asan with ASAN_OPTIONS="
                  "allocator_may_return_null=1 -- see header comment)");
}

typedef struct {
    const char *name;
    bool ( *func)(void);
} test_case_t;

static test_case_t test_cases[] = {
    {"load failure: hash-table state / retry",
     test_load_failure_hash_table_state},
    {"flush failure: dirty + ERROR state (S4.1)",
     test_flush_failure_dirty_state},
    {"malloc/aligned_alloc failure: KES_ERROR_NOMEM",
     test_malloc_failure_nomem},
    {NULL, NULL}
};

int main(void) {
    printf( "=== KES Cache Fault Injection Coverage ===\n\n");

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
