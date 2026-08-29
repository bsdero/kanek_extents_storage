#define _GNU_SOURCE  /* kes_cache.h requires this before any libc
                       * header for pthread_rwlock_t -- see the
                       * comment at the top of that file. */

#include <kes/kes_cache.h>
#include <kes/kes_types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

/*
 * Long-run soak test (KES_HARDENING_PLAN.md S6.H, plan_phase5.md
 * Track A.7.2). Extends test_concurrent_sync_vs_get_put's shape
 * (tests/test_kes_cache.c) -- get/put worker threads racing dedicated
 * sync() threads over a small, overlapping id pool under a
 * max_entries-constrained cache -- into a widened op mix (get, put,
 * pin, unpin, mark_dirty, flush, invalidate, mirroring test_kes_fuzz.c's
 * A.6.1 switch) that keeps running until a wall-clock deadline instead
 * of a fixed iteration count.
 *
 * Duration is controlled by KES_SOAK_SECONDS (default 2, so this stays
 * fast and harmless as part of a normal `make test` run); `make soak`
 * overrides it to 600 (10 minutes) under TSan -- see the Makefile
 * comment above the `soak` target for why TSan was chosen over ASan
 * for that run.
 *
 * Periodically (every SOAK_CHECK_INTERVAL_MS of wall-clock time) the
 * main thread requests all worker/sync threads to park at their next
 * op boundary (soak_worker_checkpoint()), waits for all of them to
 * report parked, and only then walks the cache's hash table directly
 * to recompute entries_cached/memory_used from scratch and compare
 * against kes_cache_get_stats() -- this quiescent-checkpoint design is
 * what makes an exact-equality comparison meaningful here, unlike a
 * naive concurrent read of cache->stats vs. a concurrent hash-table
 * walk, which would have its own unrelated race window between the
 * two reads. The walk itself still uses the same bucket-lock-then-
 * release-then-per-entry-lock pattern kes_cache.c's own hash_find()
 * uses (rdlock the bucket, pin every entry found via lookup_pins,
 * unlock the bucket, then lock each pinned entry individually to read
 * ref_count/pin_count/data_size before releasing its pin) rather than
 * nesting bucket-then-entry locks, which would invert
 * try_evict_entry_locked()'s entry-then-bucket order and risk
 * deadlock -- see kes_extent_entry_t's lookup_pins doc comment in
 * include/kes/kes_cache.h for the contract this mirrors.
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
 * Configuration (env vars, mirrors test_kes_fuzz.c's KES_TEST_SEED
 * pattern so a failure is reproducible)
 * ================================================================= */

static unsigned int soak_get_seed(void) {
    const char *env = getenv( "KES_TEST_SEED");

    if ( env != NULL) {
        return((unsigned int)strtoul( env, NULL, 10));
    }

    return((unsigned int)time( NULL));
}

static long soak_get_duration_seconds(void) {
    const char *env = getenv( "KES_SOAK_SECONDS");

    if ( env != NULL) {
        long n = strtol( env, NULL, 10);
        if ( n > 0) {
            return(n);
        }
    }

    return(2);
}

/* =================================================================
 * Mock I/O harness (adapted from tests/test_kes_cache.c's
 * mock_read_extent/mock_write_extent and test_kes_fuzz.c's
 * fuzz_mock_read/fuzz_mock_write -- copied rather than shared, per
 * plan_phase5.md A.1's guidance to adapt the minimal subset needed
 * rather than #include-ing another test's .c file)
 * ================================================================= */

#define SOAK_BLOCK_SIZE       KES_MIN_BLOCK_SIZE
#define SOAK_MAX_BLOCK_COUNT  4
#define SOAK_POOL_SIZE        24
#define SOAK_MAX_ENTRIES      KES_CACHE_MIN_ENTRIES
#define SOAK_SANITY_CEILING   1000000u

/* Mirrors the private KES_CACHE_ALIGNMENT (64) from src/kes_cache.c
 * (confirmed via `grep -n KES_CACHE_ALIGNMENT src/kes_cache.c`) --
 * not part of the public API, duplicated here only because
 * kes_cache_stats_t.memory_used is reported pre-aligned and there is
 * no public accessor for the alignment constant itself. */
#define SOAK_MEM_ALIGNMENT 64
#define SOAK_ALIGN(size) \
    (((size) + SOAK_MEM_ALIGNMENT - 1) & ~((size_t)SOAK_MEM_ALIGNMENT - 1))

#define SOAK_NUM_WORKER_THREADS 4
#define SOAK_NUM_SYNC_THREADS   2
#define SOAK_TOTAL_WORKERS \
    (SOAK_NUM_WORKER_THREADS + SOAK_NUM_SYNC_THREADS)

#define SOAK_CHECK_INTERVAL_MS  500
#define SOAK_QUIESCE_TIMEOUT_MS 5000

/* Generous upper bound on live entries a single quiescent walk can
 * see -- SOAK_MAX_ENTRIES (16) is the cache's hard cap, so this is
 * comfortable headroom, not a tight fit. */
#define SOAK_MAX_TRACKED_ENTRIES (SOAK_MAX_ENTRIES * 4)

typedef struct {
    uint8_t data[SOAK_MAX_BLOCK_COUNT * SOAK_BLOCK_SIZE];
} soak_mock_slot_t;

static soak_mock_slot_t g_soak_storage[SOAK_POOL_SIZE];

static int soak_mock_read( void *device, const kes_extent_id_t *id,
                            void *buffer, size_t size) {
    (void)device;

    if ( id->start_block >= SOAK_POOL_SIZE ||
        size > sizeof(g_soak_storage[0].data)) {
        return(-1);
    }

    memcpy( buffer, g_soak_storage[id->start_block].data, size);
    return(0);
}

static int soak_mock_write( void *device, const kes_extent_id_t *id,
                             const void *buffer, size_t size) {
    (void)device;

    if ( id->start_block >= SOAK_POOL_SIZE ||
        size > sizeof(g_soak_storage[0].data)) {
        return(-1);
    }

    memcpy( g_soak_storage[id->start_block].data, buffer, size);
    return(0);
}

static int soak_mock_sync( void *device) {
    (void)device;
    return(0);
}

static void soak_pool_init( kes_extent_id_t pool[SOAK_POOL_SIZE]) {
    for ( int i = 0; i < SOAK_POOL_SIZE; i++) {
        pool[i].start_block = (uint64_t)i;
        pool[i].block_count = (uint32_t)((i % SOAK_MAX_BLOCK_COUNT) + 1);
        pool[i].block_size = SOAK_BLOCK_SIZE;
        pool[i].reserved = 0;
    }
}

/* =================================================================
 * Quiescence coordination -- workers/sync threads park here between
 * individual cache operations whenever the monitor has requested a
 * pause, giving the monitor a genuinely quiescent point to inspect
 * cache state from. All GCC atomic builtins, matching the style
 * src/kes_cache.c already uses for lookup_pins/cond_waiters.
 * ================================================================= */

static volatile int g_soak_stop = 0;
static volatile int g_soak_pause_requested = 0;
static volatile int g_soak_paused_count = 0;

static void soak_worker_checkpoint(void) {
    if ( __atomic_load_n( &g_soak_pause_requested, __ATOMIC_SEQ_CST)) {
        __atomic_fetch_add( &g_soak_paused_count, 1, __ATOMIC_SEQ_CST);
        while ( __atomic_load_n( &g_soak_pause_requested,
                                  __ATOMIC_SEQ_CST)) {
            usleep( 1000);
        }
        __atomic_fetch_sub( &g_soak_paused_count, 1, __ATOMIC_SEQ_CST);
    }
}

/* =================================================================
 * Worker threads
 * ================================================================= */

typedef struct {
    kes_cache_t *cache;
    const kes_extent_id_t *pool;
    unsigned int seed;
    long op_count;
    bool success;
} soak_worker_arg_t;

static void *soak_worker_thread( void *arg) {
    soak_worker_arg_t *w = (soak_worker_arg_t*)arg;
    w->op_count = 0;
    w->success = true;

    while ( !__atomic_load_n( &g_soak_stop, __ATOMIC_SEQ_CST)) {
        soak_worker_checkpoint();

        int op = rand_r( &w->seed) % 10;
        int idx = rand_r( &w->seed) % SOAK_POOL_SIZE;
        const kes_extent_id_t *id = &w->pool[idx];
        void *buffer = NULL;
        int rc;

        if ( op < 6) {
            /* Paired get/put (60%), matching
             * test_concurrent_sync_vs_get_put's shape. Pairing
             * matters: independent, unpaired get/put draws let gets
             * vastly outpace puts under sustained multi-thread load,
             * driving every resident entry's ref_count above 0
             * permanently -- the cache saturates and nearly every
             * later op becomes an immediate KES_ERROR_BUSY. Harmless
             * correctness-wise, but during development this produced
             * hundreds of thousands of TRACE_ERR lines within a
             * couple of seconds, which would be impractical over a
             * real 10-minute `make soak` run. */
            rc = kes_cache_get_extent( w->cache, id, &buffer);
            if ( rc != KES_SUCCESS && rc != KES_ERROR_BUSY) {
                printf( "FAIL: soak get_extent unexpected rc=%d\n", rc);
                w->success = false;
                return(NULL);
            }
            if ( rc == KES_SUCCESS) {
                rc = kes_cache_put_extent( w->cache, id);
                if ( rc != KES_SUCCESS) {
                    printf( "FAIL: soak put_extent unexpected rc=%d\n",
                            rc);
                    w->success = false;
                    return(NULL);
                }
            }
        } else if ( op < 8) {
            /* Paired pin/unpin (20%), same rationale. */
            rc = kes_cache_pin_extent( w->cache, id);
            if ( rc != KES_SUCCESS && rc != KES_ERROR_NOTFOUND) {
                printf( "FAIL: soak pin_extent unexpected rc=%d\n", rc);
                w->success = false;
                return(NULL);
            }
            if ( rc == KES_SUCCESS) {
                rc = kes_cache_unpin_extent( w->cache, id);
                if ( rc != KES_SUCCESS) {
                    printf( "FAIL: soak unpin_extent unexpected "
                            "rc=%d\n", rc);
                    w->success = false;
                    return(NULL);
                }
            }
        } else if ( op == 8) {
            /* mark_dirty chaos op (10%) -- deliberately unpaired,
             * it does not hold any state open the way get/pin do. */
            rc = kes_cache_mark_dirty( w->cache, id);
            if ( rc != KES_SUCCESS && rc != KES_ERROR_NOTFOUND) {
                printf( "FAIL: soak mark_dirty unexpected rc=%d\n", rc);
                w->success = false;
                return(NULL);
            }
        } else {
            /* flush/invalidate chaos ops (5% each), same rationale
             * as mark_dirty above. */
            if ( rand_r( &w->seed) % 2 == 0) {
                rc = kes_cache_flush_extent( w->cache, id);
                if ( rc != KES_SUCCESS && rc != KES_ERROR_NOTFOUND) {
                    printf( "FAIL: soak flush_extent unexpected "
                            "rc=%d\n", rc);
                    w->success = false;
                    return(NULL);
                }
            } else {
                rc = kes_cache_invalidate( w->cache, id);
                if ( rc != KES_SUCCESS && rc != KES_ERROR_NOTFOUND &&
                    rc != KES_ERROR_BUSY) {
                    printf( "FAIL: soak invalidate unexpected rc=%d\n",
                            rc);
                    w->success = false;
                    return(NULL);
                }
            }
        }

        w->op_count++;
    }

    return(NULL);
}

typedef struct {
    kes_cache_t *cache;
    long op_count;
} soak_sync_arg_t;

static void *soak_sync_thread( void *arg) {
    soak_sync_arg_t *s = (soak_sync_arg_t*)arg;
    s->op_count = 0;

    while ( !__atomic_load_n( &g_soak_stop, __ATOMIC_SEQ_CST)) {
        soak_worker_checkpoint();
        kes_cache_sync( s->cache);
        s->op_count++;
        usleep( 1000);
    }

    return(NULL);
}

/* =================================================================
 * Quiescent invariant walk -- see the file header comment for why
 * this locking shape (rdlock bucket, pin every entry found, release
 * bucket, then lock/read/unlock each pinned entry individually) is
 * the safe one.
 * ================================================================= */

static bool soak_walk_invariants( kes_cache_t *cache, uint32_t *out_count,
                                   size_t *out_mem, long check_num) {
    static kes_extent_entry_t *pinned[SOAK_MAX_TRACKED_ENTRIES];
    uint32_t pinned_count = 0;

    for ( uint32_t b = 0; b < cache->bucket_count; b++) {
        kes_cache_bucket_t *bucket = &cache->buckets[b];

        pthread_rwlock_rdlock( &bucket->lock);

        kes_extent_entry_t *entry = bucket->head;
        while ( entry != NULL) {
            if ( pinned_count >= SOAK_MAX_TRACKED_ENTRIES) {
                pthread_rwlock_unlock( &bucket->lock);
                printf( "FAIL: soak walk exceeded scratch capacity "
                        "at check=%ld\n", check_num);
                return(false);
            }
            __atomic_fetch_add( &entry->lookup_pins, 1,
                                 __ATOMIC_SEQ_CST);
            pinned[pinned_count++] = entry;
            entry = entry->hash_next;
        }

        pthread_rwlock_unlock( &bucket->lock);
    }

    uint32_t count = 0;
    size_t mem = 0;

    for ( uint32_t i = 0; i < pinned_count; i++) {
        kes_extent_entry_t *entry = pinned[i];

        pthread_mutex_lock( &entry->lock);
        __atomic_fetch_sub( &entry->lookup_pins, 1, __ATOMIC_SEQ_CST);

        bool ref_ok = ( entry->ref_count < SOAK_SANITY_CEILING);
        bool pin_ok = ( entry->pin_count < SOAK_SANITY_CEILING);

        if ( ref_ok && pin_ok) {
            count++;
            mem += SOAK_ALIGN( entry->data_size);
        }

        pthread_mutex_unlock( &entry->lock);

        if ( !ref_ok) {
            printf( "FAIL: soak ref_count sanity ceiling exceeded "
                    "at check=%ld\n", check_num);
            return(false);
        }
        if ( !pin_ok) {
            printf( "FAIL: soak pin_count sanity ceiling exceeded "
                    "at check=%ld\n", check_num);
            return(false);
        }
    }

    *out_count = count;
    *out_mem = mem;
    return(true);
}

/*
 * Requests all worker/sync threads to park, waits (bounded by
 * SOAK_QUIESCE_TIMEOUT_MS) for all SOAK_TOTAL_WORKERS to report
 * parked, walks the cache's live hash table, compares it against
 * kes_cache_get_stats(), then releases the pause. Returns false (and
 * prints the specific mismatch/timeout) on any invariant violation.
 */
static bool soak_quiesce_and_check( kes_cache_t *cache, long check_num,
                                     size_t *out_actual_mem) {
    __atomic_store_n( &g_soak_pause_requested, 1, __ATOMIC_SEQ_CST);

    struct timespec deadline;
    clock_gettime( CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += SOAK_QUIESCE_TIMEOUT_MS / 1000;
    deadline.tv_nsec += (SOAK_QUIESCE_TIMEOUT_MS % 1000) * 1000000L;
    if ( deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    for (;;) {
        if ( __atomic_load_n( &g_soak_paused_count, __ATOMIC_SEQ_CST) ==
            SOAK_TOTAL_WORKERS) {
            break;
        }

        struct timespec now;
        clock_gettime( CLOCK_MONOTONIC, &now);
        if ( now.tv_sec > deadline.tv_sec ||
            ( now.tv_sec == deadline.tv_sec &&
              now.tv_nsec > deadline.tv_nsec)) {
            printf( "FAIL: soak quiesce timed out waiting for workers "
                    "at check=%ld (parked=%d/%d)\n", check_num,
                    __atomic_load_n( &g_soak_paused_count,
                                      __ATOMIC_SEQ_CST),
                    SOAK_TOTAL_WORKERS);
            __atomic_store_n( &g_soak_pause_requested, 0,
                              __ATOMIC_SEQ_CST);
            return(false);
        }

        usleep( 1000);
    }

    uint32_t actual_count = 0;
    size_t actual_mem = 0;
    bool walk_ok = soak_walk_invariants( cache, &actual_count,
                                          &actual_mem, check_num);

    kes_cache_stats_t stats;
    bool stats_ok = ( kes_cache_get_stats( cache, &stats) ==
                      KES_SUCCESS);

    __atomic_store_n( &g_soak_pause_requested, 0, __ATOMIC_SEQ_CST);

    TEST_ASSERT( walk_ok, "soak invariant walk");
    TEST_ASSERT( stats_ok, "soak get_stats during quiesce");

    if ( stats.entries_cached != actual_count) {
        printf( "FAIL: soak entries_cached mismatch (stats=%u "
                "actual=%u) at check=%ld\n", stats.entries_cached,
                actual_count, check_num);
        return(false);
    }
    if ( stats.memory_used != actual_mem) {
        printf( "FAIL: soak memory_used mismatch (stats=%zu "
                "actual=%zu) at check=%ld\n", stats.memory_used,
                actual_mem, check_num);
        return(false);
    }
    if ( stats.entries_pinned > stats.entries_cached) {
        printf( "FAIL: soak entries_pinned (%u) exceeds "
                "entries_cached (%u) at check=%ld\n",
                stats.entries_pinned, stats.entries_cached, check_num);
        return(false);
    }
    /* make_room_for_new_entry()'s doc comment (src/kes_cache.c) is
     * explicit that its check-then-evict-then-insert sequence is not
     * atomic against other concurrently-racing misses, and that
     * entries_cached/memory_used may transiently overshoot
     * config.max_entries/config.max_memory "by a small, bounded
     * amount (at most one entry per concurrently-racing miss) ... a
     * deliberate, documented deviation allowed by S5.2." A quiescent
     * checkpoint can therefore legitimately observe up to
     * SOAK_NUM_WORKER_THREADS entries beyond the cap -- this is not
     * an invariant this test enforces strictly; unbounded growth
     * beyond that (a genuine leak/desync) is what would actually
     * indicate a bug. */
    if ( stats.entries_cached >
        SOAK_MAX_ENTRIES + SOAK_NUM_WORKER_THREADS) {
        printf( "FAIL: soak entries_cached (%u) exceeds max_entries "
                "(%u) plus the documented per-racing-miss overshoot "
                "tolerance (%d) at check=%ld\n", stats.entries_cached,
                (uint32_t)SOAK_MAX_ENTRIES, SOAK_NUM_WORKER_THREADS,
                check_num);
        return(false);
    }

    printf( "INFO: soak check=%ld entries_cached=%u memory_used=%zu "
            "(live_walk=%zu) hits=%llu misses=%llu evictions=%llu "
            "flushes=%llu\n", check_num, stats.entries_cached,
            stats.memory_used, actual_mem,
            (unsigned long long)stats.hits,
            (unsigned long long)stats.misses,
            (unsigned long long)stats.evictions,
            (unsigned long long)stats.flushes);

    *out_actual_mem = actual_mem;
    return(true);
}

/* =================================================================
 * A.7.2 -- Soak test
 * ================================================================= */

static bool test_soak_mixed_workload(void) {
    long duration_seconds = soak_get_duration_seconds();
    unsigned int seed = soak_get_seed();

    printf( "INFO: soak test duration=%lds seed=%u (rerun with "
            "KES_TEST_SEED=%u to reproduce a failure; set "
            "KES_SOAK_SECONDS for a longer/shorter run -- `make "
            "soak` uses 600)\n", duration_seconds, seed, seed);

    kes_extent_id_t pool[SOAK_POOL_SIZE];
    soak_pool_init( pool);
    memset( g_soak_storage, 0, sizeof(g_soak_storage));

    kes_cache_config_t config;
    memset( &config, 0, sizeof(config));
    config.max_memory = KES_CACHE_MIN_MEMORY;
    config.min_memory = KES_CACHE_MIN_MEMORY / 2;
    /* Pool (24) intentionally exceeds max_entries (16, the minimum
     * kes_cache_create() allows) so eviction pressure and contention
     * over cache slots stay frequent for the whole run, matching
     * test_concurrent_sync_vs_get_put's "force real eviction
     * pressure" intent. */
    config.max_entries = SOAK_MAX_ENTRIES;
    config.policy = KES_CACHE_LRU;
    config.background_threads = 0;
    config.sync_interval_ms = 1000;
    config.enable_prefetch = false;
    config.enable_compression = false;
    config.device_handle = NULL;

    kes_cache_t *cache = kes_cache_create( &config);
    TEST_ASSERT( cache != NULL, "soak cache create");

    TEST_ASSERT( kes_cache_set_io_callbacks( cache, soak_mock_read,
                     soak_mock_write, soak_mock_sync) == KES_SUCCESS,
                 "soak cache set callbacks");

    __atomic_store_n( &g_soak_stop, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n( &g_soak_pause_requested, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n( &g_soak_paused_count, 0, __ATOMIC_SEQ_CST);

    pthread_t worker_threads[SOAK_NUM_WORKER_THREADS];
    soak_worker_arg_t worker_args[SOAK_NUM_WORKER_THREADS];
    pthread_t sync_threads[SOAK_NUM_SYNC_THREADS];
    soak_sync_arg_t sync_args[SOAK_NUM_SYNC_THREADS];

    for ( int i = 0; i < SOAK_NUM_WORKER_THREADS; i++) {
        worker_args[i].cache = cache;
        worker_args[i].pool = pool;
        worker_args[i].seed = seed + (unsigned int)i + 1;
        worker_args[i].op_count = 0;
        worker_args[i].success = true;

        int rc = pthread_create( &worker_threads[i], NULL,
                                 soak_worker_thread, &worker_args[i]);
        TEST_ASSERT( rc == 0, "soak worker thread create");
    }

    for ( int i = 0; i < SOAK_NUM_SYNC_THREADS; i++) {
        sync_args[i].cache = cache;
        sync_args[i].op_count = 0;

        int rc = pthread_create( &sync_threads[i], NULL,
                                 soak_sync_thread, &sync_args[i]);
        TEST_ASSERT( rc == 0, "soak sync thread create");
    }

    struct timespec start;
    struct timespec now;
    clock_gettime( CLOCK_MONOTONIC, &start);

    long check_num = 0;
    size_t mem_high_water = 0;
    size_t mem_low_water = SIZE_MAX;
    bool all_ok = true;

    do {
        usleep( SOAK_CHECK_INTERVAL_MS * 1000);

        size_t actual_mem = 0;
        check_num++;
        if ( !soak_quiesce_and_check( cache, check_num, &actual_mem)) {
            all_ok = false;
            break;
        }

        if ( actual_mem > mem_high_water) {
            mem_high_water = actual_mem;
        }
        if ( actual_mem < mem_low_water) {
            mem_low_water = actual_mem;
        }

        clock_gettime( CLOCK_MONOTONIC, &now);
    } while ( ( now.tv_sec - start.tv_sec) < duration_seconds);

    __atomic_store_n( &g_soak_stop, 1, __ATOMIC_SEQ_CST);

    for ( int i = 0; i < SOAK_NUM_WORKER_THREADS; i++) {
        pthread_join( worker_threads[i], NULL);
    }
    for ( int i = 0; i < SOAK_NUM_SYNC_THREADS; i++) {
        pthread_join( sync_threads[i], NULL);
    }

    long total_worker_ops = 0;
    for ( int i = 0; i < SOAK_NUM_WORKER_THREADS; i++) {
        total_worker_ops += worker_args[i].op_count;
        if ( !worker_args[i].success) {
            all_ok = false;
        }
    }

    long total_sync_ops = 0;
    for ( int i = 0; i < SOAK_NUM_SYNC_THREADS; i++) {
        total_sync_ops += sync_args[i].op_count;
    }

    printf( "INFO: soak summary: %ld checks, %ld worker ops, %ld "
            "sync ops, live memory_used range [%zu, %zu] bytes\n",
            check_num, total_worker_ops, total_sync_ops,
            ( mem_low_water == SIZE_MAX) ? (size_t)0 : mem_low_water,
            mem_high_water);

    kes_cache_destroy( cache);

    TEST_ASSERT( all_ok, "soak test detected an invariant violation "
                "or a worker thread failure -- see FAIL lines above");

    TEST_SUCCESS( "Soak: mixed get/put/pin/unpin/mark_dirty/flush/"
                  "invalidate workers vs. dedicated sync() threads, "
                  "periodic quiesce-and-check of ref_count/pin_count "
                  "bounds plus entries_cached/memory_used against a "
                  "live hash-table walk");
}

/* =================================================================
 * Test Runner
 * ================================================================= */

typedef struct {
    const char *name;
    bool (*func)(void);
} test_case_t;

static test_case_t test_cases[] = {
    {"Soak: Mixed Workload", test_soak_mixed_workload},
    {NULL, NULL}
};

int main(void) {
    printf( "=== KES Soak Test Suite ===\n\n");

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
