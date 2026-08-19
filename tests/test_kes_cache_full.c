/*
 * test_kes_cache_full.c - Full public API coverage for kes_cache.h
 *
 * One demo test per public function in kes_cache.h. Four functions
 * declared in kes_cache.h have no implementation yet in kes_cache.c
 * (kes_cache_sync, kes_cache_invalidate, kes_cache_reset_stats,
 * kes_cache_start -- see PENDING_ITEMS.md Phase 3). Calling any of
 * them would fail to LINK, not just fail at runtime, so their demo
 * tests are written below but kept inside an #if 0 block with
 * instructions to enable them once Phase 3 lands.
 */

#define _GNU_SOURCE

#include <kes/kes_cache.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            printf("FAIL: %s at %s:%d\n", message, __FILE__, __LINE__); \
            return false; \
        } \
    } while (0)

#define TEST_SUCCESS(test_name) \
    do { \
        printf("PASS: %s\n", test_name); \
        return true; \
    } while (0)

static int tests_run = 0;
static int tests_passed = 0;

#define MOCK_BLOCK_SIZE   4096
#define MOCK_NUM_BLOCKS   64

static unsigned char g_backing[MOCK_NUM_BLOCKS][MOCK_BLOCK_SIZE];
static int g_write_calls = 0;

static int mock_read(void *dev, const kes_extent_id_t *id, void *buf, size_t size) {
    (void)dev;
    if (id->start_block >= MOCK_NUM_BLOCKS || size > MOCK_BLOCK_SIZE) {
        return KES_ERROR_IO;
    }
    memcpy(buf, g_backing[id->start_block], size);
    return KES_SUCCESS;
}

static int mock_write(void *dev, const kes_extent_id_t *id, const void *buf,
                       size_t size) {
    (void)dev;
    if (id->start_block >= MOCK_NUM_BLOCKS || size > MOCK_BLOCK_SIZE) {
        return KES_ERROR_IO;
    }
    memcpy(g_backing[id->start_block], buf, size);
    g_write_calls++;
    return KES_SUCCESS;
}

static int mock_sync(void *dev) {
    (void)dev;
    return KES_SUCCESS;
}

static void default_config(kes_cache_config_t *cfg) {
    kes_cache_get_default_config(cfg, true);   /* small/fast edge sizing for tests */
}

static kes_extent_id_t make_id(uint64_t start_block) {
    kes_extent_id_t id = { .start_block = start_block, .block_count = 1,
                            .block_size = MOCK_BLOCK_SIZE, .reserved = 0 };
    return id;
}

/* kes_cache_get_default_config() */
static bool test_kes_cache_get_default_config(void) {
    kes_cache_config_t edge, server;

    kes_cache_get_default_config(&edge, true);
    kes_cache_get_default_config(&server, false);

    TEST_ASSERT(edge.max_memory >= KES_CACHE_MIN_MEMORY, "edge config meets min memory");
    TEST_ASSERT(server.max_memory >= KES_CACHE_MIN_MEMORY, "server config meets min memory");
    TEST_ASSERT(edge.max_memory < server.max_memory,
                "edge config is smaller than server config");
    TEST_ASSERT(edge.policy == KES_CACHE_LRU, "default policy is LRU");

    TEST_SUCCESS("kes_cache_get_default_config");
}

/* kes_cache_create() */
static bool test_kes_cache_create(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;

    default_config(&cfg);
    cache = kes_cache_create(&cfg);
    TEST_ASSERT(cache != NULL, "create success");
    kes_cache_destroy(cache);

    TEST_ASSERT(kes_cache_create(NULL) == NULL, "NULL config rejected");

    cfg.max_memory = KES_CACHE_MIN_MEMORY - 1;
    TEST_ASSERT(kes_cache_create(&cfg) == NULL, "below-minimum max_memory rejected");

    default_config(&cfg);
    cfg.max_entries = KES_CACHE_MIN_ENTRIES - 1;
    TEST_ASSERT(kes_cache_create(&cfg) == NULL, "below-minimum max_entries rejected");

    TEST_SUCCESS("kes_cache_create");
}

/* kes_cache_destroy() */
static bool test_kes_cache_destroy(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;

    default_config(&cfg);
    cache = kes_cache_create(&cfg);
    TEST_ASSERT(kes_cache_destroy(cache) == KES_SUCCESS, "destroy success");
    TEST_ASSERT(kes_cache_destroy(NULL) == KES_ERROR_INVALID, "NULL cache rejected");

    TEST_SUCCESS("kes_cache_destroy");
}

/* kes_cache_stop() */
static bool test_kes_cache_stop(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;

    default_config(&cfg);
    cache = kes_cache_create(&cfg);

    /* kes_cache_start() doesn't exist yet (Phase 3), so this only
     * proves stop() is safe to call when no background thread was
     * ever started -- it must not crash. */
    TEST_ASSERT(kes_cache_stop(cache) == KES_SUCCESS,
                "stop() with no background thread running is a safe no-op");
    TEST_ASSERT(kes_cache_stop(NULL) == KES_ERROR_INVALID, "NULL cache rejected");

    kes_cache_destroy(cache);
    TEST_SUCCESS("kes_cache_stop");
}

/* kes_cache_set_io_callbacks() */
static bool test_kes_cache_set_io_callbacks(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;

    default_config(&cfg);
    cache = kes_cache_create(&cfg);

    TEST_ASSERT(kes_cache_set_io_callbacks(cache, mock_read, mock_write, mock_sync)
                == KES_SUCCESS, "set callbacks success");
    TEST_ASSERT(kes_cache_set_io_callbacks(NULL, mock_read, mock_write, mock_sync)
                == KES_ERROR_INVALID, "NULL cache rejected");

    kes_cache_destroy(cache);
    TEST_SUCCESS("kes_cache_set_io_callbacks");
}

/* kes_cache_get_extent() + kes_cache_put_extent() */
static bool test_kes_cache_get_put_extent(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    kes_extent_id_t id = make_id(3);
    void *buf = NULL;

    memset(g_backing[3], 0x42, MOCK_BLOCK_SIZE);
    default_config(&cfg);
    cache = kes_cache_create(&cfg);
    kes_cache_set_io_callbacks(cache, mock_read, mock_write, mock_sync);

    TEST_ASSERT(kes_cache_get_extent(cache, &id, &buf) == KES_SUCCESS,
                "get (miss) success");
    TEST_ASSERT(buf != NULL, "buffer non-NULL");
    TEST_ASSERT(((unsigned char *)buf)[0] == 0x42, "loaded data matches backing store");

    TEST_ASSERT(kes_cache_get_extent(cache, &id, &buf) == KES_SUCCESS,
                "get (hit) success");

    TEST_ASSERT(kes_cache_put_extent(cache, &id) == KES_SUCCESS,
                "put success (1st ref)");
    TEST_ASSERT(kes_cache_put_extent(cache, &id) == KES_SUCCESS,
                "put success (2nd ref)");
    /* one more put than outstanding refs -- must not underflow, per
     * the existing "if (ref_count > 0)" guard in kes_cache_put_extent */
    TEST_ASSERT(kes_cache_put_extent(cache, &id) == KES_SUCCESS,
                "extra put beyond ref count does not underflow/crash");

    kes_extent_id_t missing = make_id(50);
    TEST_ASSERT(kes_cache_put_extent(cache, &missing) == KES_ERROR_NOTFOUND,
                "put on never-fetched id rejected");

    TEST_ASSERT(kes_cache_get_extent(NULL, &id, &buf) == KES_ERROR_INVALID,
                "get: NULL cache rejected");
    TEST_ASSERT(kes_cache_get_extent(cache, NULL, &buf) == KES_ERROR_INVALID,
                "get: NULL id rejected");
    TEST_ASSERT(kes_cache_get_extent(cache, &id, NULL) == KES_ERROR_INVALID,
                "get: NULL buffer output rejected");
    TEST_ASSERT(kes_cache_put_extent(NULL, &id) == KES_ERROR_INVALID,
                "put: NULL cache rejected");
    TEST_ASSERT(kes_cache_put_extent(cache, NULL) == KES_ERROR_INVALID,
                "put: NULL id rejected");

    kes_cache_destroy(cache);
    TEST_SUCCESS("kes_cache_get_extent/kes_cache_put_extent");
}

/* kes_cache_pin_extent() + kes_cache_unpin_extent() */
static bool test_kes_cache_pin_unpin(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    kes_extent_id_t id = make_id(4);
    void *buf = NULL;
    kes_cache_stats_t stats;

    default_config(&cfg);
    cache = kes_cache_create(&cfg);
    kes_cache_set_io_callbacks(cache, mock_read, mock_write, mock_sync);
    kes_cache_get_extent(cache, &id, &buf);

    TEST_ASSERT(kes_cache_pin_extent(cache, &id) == KES_SUCCESS, "pin success");
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.entries_pinned == 1, "entries_pinned reflects one pin");

    /* pin again: refcounted, still only 1 pinned entry, needs 2 unpins */
    TEST_ASSERT(kes_cache_pin_extent(cache, &id) == KES_SUCCESS, "second pin success");
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.entries_pinned == 1, "still one pinned ENTRY (pin is refcounted)");

    TEST_ASSERT(kes_cache_unpin_extent(cache, &id) == KES_SUCCESS, "first unpin success");
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.entries_pinned == 1, "still pinned after only one of two unpins");

    TEST_ASSERT(kes_cache_unpin_extent(cache, &id) == KES_SUCCESS, "second unpin success");
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.entries_pinned == 0, "fully unpinned after matching unpin count");

    /* extra unpin beyond pin count must not underflow/crash */
    TEST_ASSERT(kes_cache_unpin_extent(cache, &id) == KES_SUCCESS,
                "extra unpin beyond pin count does not underflow/crash");

    kes_extent_id_t missing = make_id(50);
    TEST_ASSERT(kes_cache_pin_extent(cache, &missing) == KES_ERROR_NOTFOUND,
                "pin on unknown id rejected");
    TEST_ASSERT(kes_cache_unpin_extent(cache, &missing) == KES_ERROR_NOTFOUND,
                "unpin on unknown id rejected");
    TEST_ASSERT(kes_cache_pin_extent(NULL, &id) == KES_ERROR_INVALID,
                "pin: NULL cache rejected");
    TEST_ASSERT(kes_cache_unpin_extent(NULL, &id) == KES_ERROR_INVALID,
                "unpin: NULL cache rejected");

    kes_cache_destroy(cache);
    TEST_SUCCESS("kes_cache_pin_extent/kes_cache_unpin_extent");
}

/* kes_cache_mark_dirty() */
static bool test_kes_cache_mark_dirty(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    kes_extent_id_t id = make_id(5);
    void *buf = NULL;
    kes_cache_stats_t stats;

    default_config(&cfg);
    cache = kes_cache_create(&cfg);
    kes_cache_set_io_callbacks(cache, mock_read, mock_write, mock_sync);
    kes_cache_get_extent(cache, &id, &buf);

    TEST_ASSERT(kes_cache_mark_dirty(cache, &id) == KES_SUCCESS, "mark dirty success");
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.entries_dirty == 1, "entries_dirty reflects one dirty entry");

    /* idempotent: marking an already-dirty entry again must not double-count */
    TEST_ASSERT(kes_cache_mark_dirty(cache, &id) == KES_SUCCESS, "re-mark dirty success");
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.entries_dirty == 1, "entries_dirty unchanged on re-mark");

    kes_extent_id_t missing = make_id(50);
    TEST_ASSERT(kes_cache_mark_dirty(cache, &missing) == KES_ERROR_NOTFOUND,
                "mark_dirty on unknown id rejected");
    TEST_ASSERT(kes_cache_mark_dirty(NULL, &id) == KES_ERROR_INVALID,
                "NULL cache rejected");

    kes_cache_destroy(cache);
    TEST_SUCCESS("kes_cache_mark_dirty");
}

/* kes_cache_flush_extent() -- success + no-op paths only; the
 * missing-write-callback path is fix #5's own dedicated test. */
static bool test_kes_cache_flush_extent(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    kes_extent_id_t id = make_id(6);
    void *buf = NULL;
    kes_cache_stats_t stats;

    default_config(&cfg);
    cache = kes_cache_create(&cfg);
    kes_cache_set_io_callbacks(cache, mock_read, mock_write, mock_sync);
    kes_cache_get_extent(cache, &id, &buf);

    /* flushing a clean (never-dirtied) entry is a documented no-op success */
    TEST_ASSERT(kes_cache_flush_extent(cache, &id) == KES_SUCCESS,
                "flush of clean entry is a no-op success");

    kes_cache_mark_dirty(cache, &id);
    int writes_before = g_write_calls;
    TEST_ASSERT(kes_cache_flush_extent(cache, &id) == KES_SUCCESS, "flush of dirty entry");
    TEST_ASSERT(g_write_calls == writes_before + 1, "write_extent callback actually invoked");
    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.entries_dirty == 0, "entries_dirty cleared after flush");

    kes_extent_id_t missing = make_id(50);
    TEST_ASSERT(kes_cache_flush_extent(cache, &missing) == KES_ERROR_NOTFOUND,
                "flush of unknown id rejected");
    TEST_ASSERT(kes_cache_flush_extent(NULL, &id) == KES_ERROR_INVALID,
                "NULL cache rejected");

    kes_cache_destroy(cache);
    TEST_SUCCESS("kes_cache_flush_extent");
}

/* kes_cache_get_stats() */
static bool test_kes_cache_get_stats(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    kes_cache_stats_t stats;
    kes_extent_id_t id = make_id(7);
    void *buf = NULL;

    default_config(&cfg);
    cache = kes_cache_create(&cfg);
    kes_cache_set_io_callbacks(cache, mock_read, mock_write, mock_sync);
    kes_cache_get_extent(cache, &id, &buf);
    kes_cache_get_extent(cache, &id, &buf);   /* second call is a hit */

    TEST_ASSERT(kes_cache_get_stats(cache, &stats) == KES_SUCCESS, "get_stats success");
    TEST_ASSERT(stats.misses == 1, "one miss recorded");
    TEST_ASSERT(stats.hits == 1, "one hit recorded");
    TEST_ASSERT(stats.entries_cached == 1, "one entry cached");

    TEST_ASSERT(kes_cache_get_stats(NULL, &stats) == KES_ERROR_INVALID,
                "NULL cache rejected");
    TEST_ASSERT(kes_cache_get_stats(cache, NULL) == KES_ERROR_INVALID,
                "NULL stats output rejected");

    kes_cache_destroy(cache);
    TEST_SUCCESS("kes_cache_get_stats");
}

/* kes_extent_hash() */
static bool test_kes_extent_hash(void) {
    kes_extent_id_t a = make_id(10);
    kes_extent_id_t b = make_id(10);
    kes_extent_id_t c = make_id(11);

    TEST_ASSERT(kes_extent_hash(&a) == kes_extent_hash(&b),
                "identical ids hash identically (determinism)");
    TEST_ASSERT(kes_extent_hash(&a) != kes_extent_hash(&c),
                "different start_block usually hashes differently");

    TEST_SUCCESS("kes_extent_hash");
}

/* kes_extent_equal() */
static bool test_kes_extent_equal(void) {
    kes_extent_id_t a = make_id(10);
    kes_extent_id_t b = make_id(10);
    kes_extent_id_t diff_start = make_id(11);
    kes_extent_id_t diff_count = a;
    kes_extent_id_t diff_size = a;
    diff_count.block_count = 2;
    diff_size.block_size = 8192;

    TEST_ASSERT(kes_extent_equal(&a, &b) == true, "identical ids compare equal");
    TEST_ASSERT(kes_extent_equal(&a, &diff_start) == false,
                "different start_block compares unequal");
    TEST_ASSERT(kes_extent_equal(&a, &diff_count) == false,
                "different block_count compares unequal");
    TEST_ASSERT(kes_extent_equal(&a, &diff_size) == false,
                "different block_size compares unequal");

    TEST_SUCCESS("kes_extent_equal");
}

/* ================================================================
 * BLOCKED -- kes_cache_sync(), kes_cache_invalidate(),
 * kes_cache_reset_stats(), kes_cache_start() are declared in
 * kes_cache.h but have NO implementation in kes_cache.c yet
 * (PENDING_ITEMS.md Phase 3). Calling any of them today fails to
 * LINK, not just fails at runtime -- do not add these to
 * test_cases[] or enable this #if block until Phase 3 lands.
 * When it does: implement the function per KES_HARDENING_PLAN.md
 * §4, flip this to #if 1, add each test to test_cases[] below, and
 * confirm they pass under make check-all before considering Phase 3
 * done, per that plan's own acceptance criteria.
 * ================================================================ */
#if 0

/* kes_cache_reset_stats() -- per KES_HARDENING_PLAN.md §4.3, must
 * reset only cumulative counters, leaving state counters untouched. */
static bool test_kes_cache_reset_stats(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    kes_extent_id_t id = make_id(8);
    void *buf = NULL;
    kes_cache_stats_t stats;

    default_config(&cfg);
    cache = kes_cache_create(&cfg);
    kes_cache_set_io_callbacks(cache, mock_read, mock_write, mock_sync);
    kes_cache_get_extent(cache, &id, &buf);   /* one miss, one cached entry */
    kes_cache_mark_dirty(cache, &id);         /* one dirty entry */

    TEST_ASSERT(kes_cache_reset_stats(cache) == KES_SUCCESS, "reset_stats success");
    kes_cache_get_stats(cache, &stats);

    TEST_ASSERT(stats.misses == 0, "cumulative counter (misses) reset to 0");
    TEST_ASSERT(stats.hits == 0, "cumulative counter (hits) reset to 0");
    TEST_ASSERT(stats.entries_cached == 1,
                "state counter (entries_cached) survives reset unchanged");
    TEST_ASSERT(stats.entries_dirty == 1,
                "state counter (entries_dirty) survives reset unchanged");

    TEST_ASSERT(kes_cache_reset_stats(NULL) == KES_ERROR_INVALID, "NULL cache rejected");

    kes_cache_destroy(cache);
    TEST_SUCCESS("kes_cache_reset_stats");
}

/* kes_cache_invalidate() -- per KES_HARDENING_PLAN.md §4.2, discards
 * dirty data unconditionally (no implicit flush), refuses busy entries. */
static bool test_kes_cache_invalidate(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    kes_extent_id_t id = make_id(9);
    void *buf = NULL;

    default_config(&cfg);
    cache = kes_cache_create(&cfg);
    kes_cache_set_io_callbacks(cache, mock_read, mock_write, mock_sync);
    kes_cache_get_extent(cache, &id, &buf);
    kes_cache_mark_dirty(cache, &id);

    int writes_before = g_write_calls;
    TEST_ASSERT(kes_cache_invalidate(cache, &id) == KES_ERROR_BUSY,
                "invalidate refuses a still-referenced (ref_count > 0) entry");
    kes_cache_put_extent(cache, &id);   /* release the reference */

    TEST_ASSERT(kes_cache_invalidate(cache, &id) == KES_SUCCESS,
                "invalidate succeeds once unreferenced");
    TEST_ASSERT(g_write_calls == writes_before,
                "invalidate discarded dirty data WITHOUT flushing it");

    TEST_ASSERT(kes_cache_get_extent(cache, &id, &buf) == KES_SUCCESS,
                "id is fetchable again after invalidate (re-reads from backing store)");

    TEST_ASSERT(kes_cache_invalidate(cache, NULL) == KES_ERROR_INVALID,
                "NULL id rejected");
    TEST_ASSERT(kes_cache_invalidate(NULL, &id) == KES_ERROR_INVALID,
                "NULL cache rejected");

    kes_extent_id_t missing = make_id(55);
    TEST_ASSERT(kes_cache_invalidate(cache, &missing) == KES_ERROR_NOTFOUND,
                "invalidate on unknown id rejected");

    kes_cache_destroy(cache);
    TEST_SUCCESS("kes_cache_invalidate");
}

/* kes_cache_sync() -- per KES_HARDENING_PLAN.md §4.1, flushes every
 * dirty entry and frees unreferenced ones. */
static bool test_kes_cache_sync(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    kes_extent_id_t id_a = make_id(20), id_b = make_id(21);
    void *buf = NULL;
    kes_cache_stats_t stats;

    default_config(&cfg);
    cache = kes_cache_create(&cfg);
    kes_cache_set_io_callbacks(cache, mock_read, mock_write, mock_sync);
    kes_cache_get_extent(cache, &id_a, &buf);
    kes_cache_mark_dirty(cache, &id_a);
    kes_cache_put_extent(cache, &id_a);       /* unreferenced, should be freed by sync */
    kes_cache_get_extent(cache, &id_b, &buf);
    kes_cache_mark_dirty(cache, &id_b);       /* stays referenced -- must NOT be freed */

    int writes_before = g_write_calls;
    TEST_ASSERT(kes_cache_sync(cache) == KES_SUCCESS, "sync success");
    TEST_ASSERT(g_write_calls == writes_before + 2, "both dirty entries were flushed");

    kes_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.entries_cached == 1,
                "unreferenced flushed entry (id_a) was freed; referenced one (id_b) kept");
    TEST_ASSERT(stats.entries_dirty == 0, "no dirty entries remain after sync");

    TEST_ASSERT(kes_cache_sync(NULL) == KES_ERROR_INVALID, "NULL cache rejected");

    kes_cache_put_extent(cache, &id_b);
    kes_cache_destroy(cache);
    TEST_SUCCESS("kes_cache_sync");
}

/* kes_cache_start() -- per KES_HARDENING_PLAN.md §4.4, must prove
 * automatic background flushing actually happens, not just that the
 * thread doesn't crash. */
static bool test_kes_cache_start(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    kes_extent_id_t id = make_id(30);
    void *buf = NULL;

    default_config(&cfg);
    cfg.sync_interval_ms = 200;   /* short interval so the test doesn't stall */
    cache = kes_cache_create(&cfg);
    kes_cache_set_io_callbacks(cache, mock_read, mock_write, mock_sync);
    kes_cache_get_extent(cache, &id, &buf);
    kes_cache_mark_dirty(cache, &id);

    TEST_ASSERT(kes_cache_start(cache) == KES_SUCCESS, "start success");

    int writes_before = g_write_calls;
    usleep(500 * 1000);   /* sleep past sync_interval_ms */
    TEST_ASSERT(g_write_calls > writes_before,
                "background thread flushed the dirty entry automatically, "
                "with no explicit sync()/flush_extent() call");

    TEST_ASSERT(kes_cache_stop(cache) == KES_SUCCESS, "stop success");
    TEST_ASSERT(kes_cache_start(NULL) == KES_ERROR_INVALID, "NULL cache rejected");

    kes_cache_destroy(cache);
    TEST_SUCCESS("kes_cache_start");
}

#endif /* Phase 3 functions */

typedef struct {
    const char *name;
    bool (*func)(void);
} test_case_t;

static test_case_t test_cases[] = {
    {"kes_cache_get_default_config",      test_kes_cache_get_default_config},
    {"kes_cache_create",                  test_kes_cache_create},
    {"kes_cache_destroy",                 test_kes_cache_destroy},
    {"kes_cache_stop",                    test_kes_cache_stop},
    {"kes_cache_set_io_callbacks",        test_kes_cache_set_io_callbacks},
    {"kes_cache_get_extent/put_extent",   test_kes_cache_get_put_extent},
    {"kes_cache_pin_extent/unpin_extent", test_kes_cache_pin_unpin},
    {"kes_cache_mark_dirty",              test_kes_cache_mark_dirty},
    {"kes_cache_flush_extent",            test_kes_cache_flush_extent},
    {"kes_cache_get_stats",               test_kes_cache_get_stats},
    {"kes_extent_hash",                   test_kes_extent_hash},
    {"kes_extent_equal",                  test_kes_extent_equal},
    /* kes_cache_sync, kes_cache_invalidate, kes_cache_reset_stats,
     * kes_cache_start: add here once Phase 3 implements them and the
     * #if 0 block above is flipped to #if 1. */
    {NULL, NULL}
};

int main(void) {
    printf("=== KES Cache Full API Coverage ===\n\n");

    for (test_case_t *t = test_cases; t->name != NULL; t++) {
        printf("Running: %s... ", t->name);
        fflush(stdout);
        tests_run++;
        if (t->func()) {
            tests_passed++;
        }
    }

    printf("\n=== Test Results ===\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_run - tests_passed);

    return (tests_passed == tests_run) ? 0 : 1;
}
