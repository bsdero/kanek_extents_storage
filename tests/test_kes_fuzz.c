#define _GNU_SOURCE  /* kes_cache.h requires this before any libc
                       * header for pthread_rwlock_t -- see the
                       * comment at the top of that file. */

#include <kes/kes_cache.h>
#include <kes/kes_bitmap.h>
#include <kes/kes_types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/*
 * Randomized/fuzz-adjacent testing (KES_HARDENING_PLAN.md S6.F,
 * plan_phase5.md Track A.6). Two independent fuzz tests:
 *
 * - test_cache_invariant_fuzz(): a long randomized sequence of
 *   get/put/pin/unpin/mark_dirty/flush/sync/invalidate calls against
 *   a small fixed pool of extent ids, checking ref_count/pin_count
 *   sanity and cache->stats consistency against a direct scan of the
 *   live hash table after every single operation.
 * - test_bitmap_fuzz_vs_reference(): drives kes_bitmap_t and a
 *   naive, trivially-correct reference bitmap through the same
 *   randomized set_range/clear_range/find_free sequence and compares
 *   them bit-for-bit after every operation. (Added in a follow-up
 *   commit -- see git history/PENDING_ITEMS.md.)
 *
 * Both read KES_TEST_SEED (default time(NULL)) and KES_FUZZ_ITERATIONS
 * (default a few thousand) from the environment and print the seed
 * actually used, so any failure is reproducible by rerunning with
 * the same KES_TEST_SEED.
 */

#define TEST_ASSERT(condition, message) \
    do { \
        if ( !(condition)) { \
            printf( "FAIL: %s at %s:%d\n", message, __FILE__, \
                    __LINE__); \
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

/* =================================================================
 * Shared seed/iteration-count handling
 * ================================================================= */

static unsigned int fuzz_get_seed(void) {
    const char *env = getenv( "KES_TEST_SEED");

    if ( env != NULL) {
        return((unsigned int)strtoul( env, NULL, 10));
    }

    return((unsigned int)time( NULL));
}

static long fuzz_get_iterations(void) {
    const char *env = getenv( "KES_FUZZ_ITERATIONS");

    if ( env != NULL) {
        long n = strtol( env, NULL, 10);
        if ( n > 0) {
            return(n);
        }
    }

    return(3000);
}

/* =================================================================
 * A.6.1 -- Cache invariant fuzzing
 * ================================================================= */

#define FUZZ_BLOCK_SIZE       KES_MIN_BLOCK_SIZE
#define FUZZ_MAX_BLOCK_COUNT  4
#define FUZZ_POOL_SIZE        20
#define FUZZ_MAX_ENTRIES      KES_CACHE_MIN_ENTRIES
#define FUZZ_SANITY_CEILING   1000000u

/* Mirrors the private KES_CACHE_ALIGNMENT (64) from src/kes_cache.c
 * (confirmed via `grep -n KES_CACHE_ALIGNMENT src/kes_cache.c`) --
 * not part of the public API, duplicated here only because
 * kes_cache_stats_t.memory_used is reported pre-aligned and there is
 * no public accessor for the alignment constant itself. */
#define FUZZ_MEM_ALIGNMENT 64
#define FUZZ_ALIGN(size) \
    (((size) + FUZZ_MEM_ALIGNMENT - 1) & ~((size_t)FUZZ_MEM_ALIGNMENT - 1))

typedef struct {
    uint8_t data[FUZZ_MAX_BLOCK_COUNT * FUZZ_BLOCK_SIZE];
} fuzz_mock_slot_t;

static fuzz_mock_slot_t g_fuzz_storage[FUZZ_POOL_SIZE];

static int fuzz_mock_read( void *device, const kes_extent_id_t *id,
                            void *buffer, size_t size) {
    (void)device;

    if ( id->start_block >= FUZZ_POOL_SIZE ||
        size > sizeof(g_fuzz_storage[0].data)) {
        return(-1);
    }

    memcpy( buffer, g_fuzz_storage[id->start_block].data, size);
    return(0);
}

static int fuzz_mock_write( void *device, const kes_extent_id_t *id,
                             const void *buffer, size_t size) {
    (void)device;

    if ( id->start_block >= FUZZ_POOL_SIZE ||
        size > sizeof(g_fuzz_storage[0].data)) {
        return(-1);
    }

    memcpy( g_fuzz_storage[id->start_block].data, buffer, size);
    return(0);
}

static int fuzz_mock_sync( void *device) {
    (void)device;
    return(0);
}

static void fuzz_pool_init( kes_extent_id_t pool[FUZZ_POOL_SIZE]) {
    for ( int i = 0; i < FUZZ_POOL_SIZE; i++) {
        pool[i].start_block = (uint64_t)i;
        pool[i].block_count = (uint32_t)((i % FUZZ_MAX_BLOCK_COUNT) + 1);
        pool[i].block_size = FUZZ_BLOCK_SIZE;
        pool[i].reserved = 0;
    }
}

/*
 * Walks every live entry in the cache's hash table directly --
 * kes_cache_t and kes_extent_entry_t are fully defined in
 * kes_cache.h, not truly opaque -- to compute a ground-truth entry
 * count and memory usage independently of cache->stats, and to
 * sanity-check ref_count/pin_count on every entry along the way.
 * Single-threaded caller only: this fuzz test never spawns a
 * thread, so no bucket lock is taken here, matching every other
 * access in this file.
 */
static bool fuzz_walk_cache_invariants( kes_cache_t *cache,
    uint32_t *out_count, size_t *out_mem, unsigned int seed, long iter) {
    uint32_t count = 0;
    size_t mem = 0;

    for ( uint32_t b = 0; b < cache->bucket_count; b++) {
        kes_extent_entry_t *entry = cache->buckets[b].head;

        while ( entry != NULL) {
            if ( entry->ref_count >= FUZZ_SANITY_CEILING) {
                printf( "FAIL: ref_count sanity ceiling exceeded "
                        "(ref_count=%u) at iter=%ld seed=%u\n",
                        entry->ref_count, iter, seed);
                return(false);
            }
            if ( entry->pin_count >= FUZZ_SANITY_CEILING) {
                printf( "FAIL: pin_count sanity ceiling exceeded "
                        "(pin_count=%u) at iter=%ld seed=%u\n",
                        entry->pin_count, iter, seed);
                return(false);
            }

            count++;
            mem += FUZZ_ALIGN( entry->data_size);
            entry = entry->hash_next;
        }
    }

    *out_count = count;
    *out_mem = mem;
    return(true);
}

static bool test_cache_invariant_fuzz(void) {
    unsigned int seed = fuzz_get_seed();
    long iterations = fuzz_get_iterations();

    printf( "INFO: cache invariant fuzz seed=%u iterations=%ld "
            "(rerun with KES_TEST_SEED=%u to reproduce a failure)\n",
            seed, iterations, seed);
    srand( seed);

    kes_extent_id_t pool[FUZZ_POOL_SIZE];
    fuzz_pool_init( pool);
    memset( g_fuzz_storage, 0, sizeof(g_fuzz_storage));

    kes_cache_config_t config;
    memset( &config, 0, sizeof(config));
    config.max_memory = KES_CACHE_MIN_MEMORY;
    config.min_memory = KES_CACHE_MIN_MEMORY / 2;
    /* Pool (20) intentionally exceeds max_entries (16, the minimum
     * kes_cache_create() allows) so eviction pressure and
     * contention over cache slots are frequent, per the task's
     * "small pool so collisions/contention are frequent" intent. */
    config.max_entries = FUZZ_MAX_ENTRIES;
    config.policy = KES_CACHE_LRU;
    config.background_threads = 0;
    config.sync_interval_ms = 1000;
    config.enable_prefetch = false;
    config.enable_compression = false;
    config.device_handle = NULL;

    kes_cache_t *cache = kes_cache_create( &config);
    TEST_ASSERT( cache != NULL, "fuzz cache create");

    TEST_ASSERT( kes_cache_set_io_callbacks( cache, fuzz_mock_read,
                     fuzz_mock_write, fuzz_mock_sync) == KES_SUCCESS,
                 "fuzz cache set callbacks");

    for ( long iter = 0; iter < iterations; iter++) {
        int op = rand() % 8;
        int idx = rand() % FUZZ_POOL_SIZE;
        const kes_extent_id_t *id = &pool[idx];
        void *buffer = NULL;
        int rc;

        switch ( op) {
        case 0:
            rc = kes_cache_get_extent( cache, id, &buffer);
            if ( rc != KES_SUCCESS && rc != KES_ERROR_BUSY) {
                printf( "FAIL: get_extent unexpected rc=%d at "
                        "iter=%ld seed=%u\n", rc, iter, seed);
                return(false);
            }
            break;
        case 1:
            rc = kes_cache_put_extent( cache, id);
            if ( rc != KES_SUCCESS && rc != KES_ERROR_NOTFOUND) {
                printf( "FAIL: put_extent unexpected rc=%d at "
                        "iter=%ld seed=%u\n", rc, iter, seed);
                return(false);
            }
            break;
        case 2:
            rc = kes_cache_pin_extent( cache, id);
            if ( rc != KES_SUCCESS && rc != KES_ERROR_NOTFOUND) {
                printf( "FAIL: pin_extent unexpected rc=%d at "
                        "iter=%ld seed=%u\n", rc, iter, seed);
                return(false);
            }
            break;
        case 3:
            rc = kes_cache_unpin_extent( cache, id);
            if ( rc != KES_SUCCESS && rc != KES_ERROR_NOTFOUND) {
                printf( "FAIL: unpin_extent unexpected rc=%d at "
                        "iter=%ld seed=%u\n", rc, iter, seed);
                return(false);
            }
            break;
        case 4:
            rc = kes_cache_mark_dirty( cache, id);
            if ( rc != KES_SUCCESS && rc != KES_ERROR_NOTFOUND) {
                printf( "FAIL: mark_dirty unexpected rc=%d at "
                        "iter=%ld seed=%u\n", rc, iter, seed);
                return(false);
            }
            break;
        case 5:
            rc = kes_cache_flush_extent( cache, id);
            if ( rc != KES_SUCCESS && rc != KES_ERROR_NOTFOUND) {
                printf( "FAIL: flush_extent unexpected rc=%d at "
                        "iter=%ld seed=%u\n", rc, iter, seed);
                return(false);
            }
            break;
        case 6:
            rc = kes_cache_sync( cache);
            if ( rc != KES_SUCCESS) {
                printf( "FAIL: sync unexpected rc=%d at iter=%ld "
                        "seed=%u\n", rc, iter, seed);
                return(false);
            }
            break;
        case 7:
        default:
            rc = kes_cache_invalidate( cache, id);
            if ( rc != KES_SUCCESS && rc != KES_ERROR_NOTFOUND &&
                rc != KES_ERROR_BUSY) {
                printf( "FAIL: invalidate unexpected rc=%d at "
                        "iter=%ld seed=%u\n", rc, iter, seed);
                return(false);
            }
            break;
        }

        uint32_t actual_count = 0;
        size_t actual_mem = 0;
        if ( !fuzz_walk_cache_invariants( cache, &actual_count,
                                           &actual_mem, seed, iter)) {
            return(false);
        }

        kes_cache_stats_t stats;
        TEST_ASSERT( kes_cache_get_stats( cache, &stats) ==
                     KES_SUCCESS, "fuzz get_stats");

        if ( stats.entries_cached != actual_count) {
            printf( "FAIL: entries_cached mismatch (stats=%u "
                    "actual=%u) at iter=%ld seed=%u\n",
                    stats.entries_cached, actual_count, iter, seed);
            return(false);
        }

        if ( stats.memory_used != actual_mem) {
            printf( "FAIL: memory_used mismatch (stats=%zu "
                    "actual=%zu) at iter=%ld seed=%u\n",
                    stats.memory_used, actual_mem, iter, seed);
            return(false);
        }
    }

    kes_cache_destroy( cache);

    TEST_SUCCESS( "Cache invariant fuzz (random get/put/pin/unpin/"
                   "mark_dirty/flush/sync/invalidate against a "
                   "20-id pool over a 16-entry cache; ref_count and "
                   "pin_count sanity ceiling, entries_cached and "
                   "memory_used vs. a live hash-table scan, checked "
                   "after every operation)");
}

/* =================================================================
 * Test Runner
 * ================================================================= */

typedef struct {
    const char *name;
    bool (*func)(void);
} test_case_t;

static test_case_t test_cases[] = {
    {"Cache Invariant Fuzz",        test_cache_invariant_fuzz},
    {NULL, NULL}
};

int main(void) {
    printf( "=== KES Fuzz/Randomized Test Suite ===\n\n");

    for ( test_case_t *t = test_cases; t->name != NULL; t++) {
        printf( "Running: %s...\n", t->name);
        tests_run++;
        if ( t->func()) {
            tests_passed++;
        }
        printf( "\n");
    }

    printf( "=== Test Results ===\n");
    printf( "Tests run: %d\n", tests_run);
    printf( "Tests passed: %d\n", tests_passed);
    printf( "Tests failed: %d\n", tests_run - tests_passed);

    return((tests_passed == tests_run) ? 0 : 1);
}
