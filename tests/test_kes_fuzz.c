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
 *   them bit-for-bit after every operation.
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
 * A.6.2 -- Bitmap fuzz vs. naive reference
 * ================================================================= */

/* Deliberately not a multiple of 8 -- forces the bitmap's final byte
 * to be partially used, so "byte-boundary-exact" and "whole bitmap"
 * range flavors below are meaningfully distinct from each other. */
#define BITMAP_FUZZ_TOTAL_BITS 251

typedef struct {
    uint8_t *bits;         /* one byte per bit: 0 or 1 */
    uint64_t total_bits;
} naive_bitmap_t;

static naive_bitmap_t *naive_bitmap_create( uint64_t total_bits) {
    naive_bitmap_t *nb = calloc( 1, sizeof(naive_bitmap_t));

    if ( nb == NULL) {
        return(NULL);
    }

    nb->bits = calloc( total_bits, sizeof(uint8_t));
    if ( nb->bits == NULL) {
        free( nb);
        return(NULL);
    }

    nb->total_bits = total_bits;
    return(nb);
}

static void naive_bitmap_destroy( naive_bitmap_t *nb) {
    if ( nb == NULL) {
        return;
    }

    free( nb->bits);
    free( nb);
}

static void naive_set_range( naive_bitmap_t *nb, uint64_t start,
                              uint32_t count) {
    for ( uint32_t i = 0; i < count; i++) {
        nb->bits[start + i] = 1;
    }
}

static void naive_clear_range( naive_bitmap_t *nb, uint64_t start,
                                uint32_t count) {
    for ( uint32_t i = 0; i < count; i++) {
        nb->bits[start + i] = 0;
    }
}

static uint64_t naive_free_bits( naive_bitmap_t *nb) {
    uint64_t free_count = 0;

    for ( uint64_t i = 0; i < nb->total_bits; i++) {
        if ( nb->bits[i] == 0) {
            free_count++;
        }
    }

    return(free_count);
}

/*
 * Mirrors kes_bitmap_find_free()'s exact search order (linear scan
 * from the clamped hint to the end, then wrap and scan 0-to-hint) so
 * its result -- success/failure and the returned start bit -- can be
 * compared directly with the real implementation, not just checked
 * for "some valid free run." The upfront free-bit-count guard also
 * mirrors the real function's use of a tracked free_bits counter
 * rather than a fresh scan.
 */
static bool naive_find_free( naive_bitmap_t *nb, uint32_t count,
                              uint64_t hint, uint64_t *found_start) {
    if ( count == 0 || naive_free_bits( nb) < count) {
        return(false);
    }

    uint64_t search_start = hint;
    if ( search_start >= nb->total_bits) {
        search_start = 0;
    }

    for ( uint64_t start = search_start;
         start <= nb->total_bits - count; start++) {
        bool ok = true;
        for ( uint32_t i = 0; i < count && ok; i++) {
            if ( nb->bits[start + i] != 0) {
                ok = false;
            }
        }
        if ( ok) {
            *found_start = start;
            return(true);
        }
    }

    if ( search_start > 0) {
        for ( uint64_t start = 0;
             start < search_start &&
             start <= nb->total_bits - count; start++) {
            bool ok = true;
            for ( uint32_t i = 0; i < count && ok; i++) {
                if ( nb->bits[start + i] != 0) {
                    ok = false;
                }
            }
            if ( ok) {
                *found_start = start;
                return(true);
            }
        }
    }

    return(false);
}

static bool test_bitmap_fuzz_vs_reference(void) {
    unsigned int seed = fuzz_get_seed();
    long iterations = fuzz_get_iterations();

    printf( "INFO: bitmap fuzz seed=%u iterations=%ld total_bits=%d "
            "(rerun with KES_TEST_SEED=%u to reproduce a failure)\n",
            seed, iterations, BITMAP_FUZZ_TOTAL_BITS, seed);
    srand( seed);

    kes_bitmap_t *bm = NULL;
    TEST_ASSERT( kes_bitmap_create( BITMAP_FUZZ_TOTAL_BITS, &bm) ==
                 KES_SUCCESS, "fuzz bitmap create");

    naive_bitmap_t *nb = naive_bitmap_create( BITMAP_FUZZ_TOTAL_BITS);
    TEST_ASSERT( nb != NULL, "fuzz naive bitmap create");

    for ( long iter = 0; iter < iterations; iter++) {
        int op = rand() % 3;
        int flavor = (int)(iter % 10);
        uint64_t start;
        uint32_t count;

        if ( flavor == 0) {
            /* whole bitmap */
            start = 0;
            count = BITMAP_FUZZ_TOTAL_BITS;
        } else if ( flavor == 1) {
            /* byte-boundary-exact, multi-byte */
            start = (uint64_t)( rand() %
                                 (BITMAP_FUZZ_TOTAL_BITS / 8)) * 8;
            uint32_t max_count =
                (uint32_t)(BITMAP_FUZZ_TOTAL_BITS - start);
            uint32_t bytes_left = max_count / 8;
            count = (bytes_left > 0) ?
                    (uint32_t)(rand() % bytes_left + 1) * 8 : 8;
            if ( start + count > BITMAP_FUZZ_TOTAL_BITS) {
                count = (uint32_t)(BITMAP_FUZZ_TOTAL_BITS - start);
            }
        } else {
            /* fully random, in-bounds */
            start = (uint64_t)( rand() % BITMAP_FUZZ_TOTAL_BITS);
            uint32_t max_count =
                (uint32_t)(BITMAP_FUZZ_TOTAL_BITS - start);
            count = (uint32_t)(rand() % (int)max_count) + 1;
        }

        if ( op == 0) {
            int rc_real = kes_bitmap_set_range( bm, start, count);
            naive_set_range( nb, start, count);
            if ( rc_real != KES_SUCCESS) {
                printf( "FAIL: set_range rc=%d start=%llu count=%u "
                        "at iter=%ld seed=%u\n", rc_real,
                        (unsigned long long)start, count, iter, seed);
                return(false);
            }
        } else if ( op == 1) {
            int rc_real = kes_bitmap_clear_range( bm, start, count);
            naive_clear_range( nb, start, count);
            if ( rc_real != KES_SUCCESS) {
                printf( "FAIL: clear_range rc=%d start=%llu count=%u "
                        "at iter=%ld seed=%u\n", rc_real,
                        (unsigned long long)start, count, iter, seed);
                return(false);
            }
        } else {
            uint64_t hint =
                (uint64_t)( rand() % BITMAP_FUZZ_TOTAL_BITS);
            uint32_t want = (uint32_t)(rand() % 8) + 1;
            uint64_t real_start = 0;
            uint64_t naive_start = 0;
            int rc_real = kes_bitmap_find_free( bm, want, hint,
                                                 &real_start);
            bool naive_ok = naive_find_free( nb, want, hint,
                                              &naive_start);
            bool real_ok = ( rc_real == KES_SUCCESS);

            if ( real_ok != naive_ok) {
                printf( "FAIL: find_free success mismatch "
                        "(real=%d naive=%d) want=%u hint=%llu at "
                        "iter=%ld seed=%u\n", real_ok, naive_ok,
                        want, (unsigned long long)hint, iter, seed);
                return(false);
            }
            if ( real_ok && real_start != naive_start) {
                printf( "FAIL: find_free start mismatch "
                        "(real=%llu naive=%llu) want=%u hint=%llu "
                        "at iter=%ld seed=%u\n",
                        (unsigned long long)real_start,
                        (unsigned long long)naive_start, want,
                        (unsigned long long)hint, iter, seed);
                return(false);
            }
            /* find_free mutates neither implementation's bit
             * contents -- no naive bit-array update here. */
        }

        for ( uint64_t i = 0; i < BITMAP_FUZZ_TOTAL_BITS; i++) {
            bool real_bit = kes_bitmap_test( bm, i);
            bool naive_bit = ( nb->bits[i] != 0);
            if ( real_bit != naive_bit) {
                printf( "FAIL: bit %llu mismatch (real=%d naive=%d) "
                        "at iter=%ld seed=%u\n",
                        (unsigned long long)i, real_bit, naive_bit,
                        iter, seed);
                return(false);
            }
        }

        uint64_t real_free = 0;
        TEST_ASSERT( kes_bitmap_get_stats( bm, NULL, &real_free,
                         NULL) == KES_SUCCESS,
                     "fuzz bitmap get_stats");
        uint64_t naive_free = naive_free_bits( nb);
        if ( real_free != naive_free) {
            printf( "FAIL: free_bits mismatch (real=%llu naive=%llu) "
                    "at iter=%ld seed=%u\n",
                    (unsigned long long)real_free,
                    (unsigned long long)naive_free, iter, seed);
            return(false);
        }
    }

    kes_bitmap_destroy( bm);
    naive_bitmap_destroy( nb);

    TEST_SUCCESS( "Bitmap fuzz vs. naive reference (set_range/"
                   "clear_range/find_free with whole-bitmap, "
                   "byte-boundary-exact, and fully random ranges; "
                   "bit-for-bit and free_bits compare after every "
                   "operation)");
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
    {"Bitmap Fuzz vs. Reference",   test_bitmap_fuzz_vs_reference},
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
