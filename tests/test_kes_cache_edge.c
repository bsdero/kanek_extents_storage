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

/* ================================================================
 * A.1.2 -- kes_cache_create() with non-power-of-2 config.block_size.
 *
 * The plan's premise for this task does not match the actual header:
 * kes_cache_config_t (include/kes/kes_cache.h) has NO block_size
 * field at all -- block_size lives on kes_extent_id_t instead, a
 * per-call parameter to kes_cache_get_extent()/put_extent()/etc.,
 * not a kes_cache_create()-time config value. There is nothing named
 * "config.block_size" for kes_cache_create() to validate; the plan
 * appears to have confused this with kes_storage_config_t's
 * block_size field, which validate_config() (src/kes_storage.c:531)
 * already checks.
 *
 * Reading src/kes_cache.c end to end confirms the closest real gap:
 * kes_cache_get_extent() (and every other kes_extent_id_t-taking
 * function) never validated id->block_size before this pass -- a
 * non-power-of-2 or absurd block_size would flow straight into
 * extent_data_size()'s size computation. That is the concrete,
 * in-scope version of "add the same KES_IS_POWER_OF_2(...) guard
 * mirroring kes_storage.c:537-539" the plan's recommended fix
 * describes, applied where block_size actually appears in this
 * module. Fixed in kes_cache_get_extent() (src/kes_cache.c): rejects
 * id->block_size that is not a power of 2 in [KES_MIN_BLOCK_SIZE,
 * KES_MAX_BLOCK_SIZE] with KES_ERROR_INVALID, before any allocation
 * is attempted. See kes_cache_get_extent()'s updated doc comment in
 * include/kes/kes_cache.h.
 */
static bool test_get_extent_rejects_non_power_of_2_block_size(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    void *buf = NULL;
    kes_extent_id_t bad_not_pow2 = { .start_block = 1, .block_count = 1,
                                      .block_size = 4097, .reserved = 0 };
    kes_extent_id_t bad_too_small = { .start_block = 1, .block_count = 1,
                                       .block_size = 1024, .reserved = 0 };
    kes_extent_id_t bad_too_large = { .start_block = 1, .block_count = 1,
                                       .block_size = 131072, .reserved = 0 };
    kes_extent_id_t good = make_id( 1);

    default_config( &cfg);
    cache = kes_cache_create( &cfg);
    TEST_ASSERT( cache != NULL, "cache creation for block_size test");
    kes_cache_set_io_callbacks( cache, mock_read, mock_write, mock_sync);

    TEST_ASSERT(
        kes_cache_get_extent( cache, &bad_not_pow2, &buf) ==
            KES_ERROR_INVALID,
        "non-power-of-2 block_size (4097) rejected");
    TEST_ASSERT(
        kes_cache_get_extent( cache, &bad_too_small, &buf) ==
            KES_ERROR_INVALID,
        "below-KES_MIN_BLOCK_SIZE block_size (1024) rejected");
    TEST_ASSERT(
        kes_cache_get_extent( cache, &bad_too_large, &buf) ==
            KES_ERROR_INVALID,
        "above-KES_MAX_BLOCK_SIZE block_size (131072) rejected");
    TEST_ASSERT(
        kes_cache_get_extent( cache, &good, &buf) == KES_SUCCESS,
        "a valid power-of-2, in-range block_size still works");

    kes_cache_put_extent( cache, &good);
    kes_cache_destroy( cache);
    TEST_SUCCESS( "kes_cache_get_extent rejects invalid id->block_size");
}

/* ================================================================
 * A.1.3 -- block_count = 0 / UINT32_MAX in a kes_extent_id_t passed
 * to kes_cache_get_extent().
 *
 * block_count = 0: extent_data_size() (src/kes_cache.c) computes
 * (size_t)0 * block_size == 0. Reading further: extent_alloc_data()
 * calls aligned_alloc(64, KES_ALIGN(0, 64)) i.e. aligned_alloc(64, 0)
 * -- on this platform's glibc that returns a valid non-NULL, zero-
 * size, freeable pointer (confirmed empirically, not assumed), so
 * the miss path proceeds normally: the mock read_extent callback is
 * invoked with size == 0 and the call succeeds, producing a real but
 * empty (0-byte) cached entry. Nothing here crashes, corrupts state,
 * or leaks, so this is asserted as-is (KES_SUCCESS, non-NULL buffer)
 * rather than an assumed rejection -- block_count == 0 is simply not
 * validated anywhere in this module today. This is a real, minor gap
 * (a 0-block extent is a nonsensical request) but is NOT the same
 * class of bug as the UINT32_MAX case below (no oversized allocation
 * risk, no crash risk), so per rule 0.3 it is reported rather than
 * silently fixed here -- see this pass's final report.
 *
 * block_count = UINT32_MAX: extent_data_size() computes
 * (size_t)UINT32_MAX * block_size (EDGE_BLOCK_SIZE == 4096) ==
 * 17,592,186,040,320 bytes (~16 TiB) -- correctly in 64-bit space,
 * not wrapped (id->block_count is cast to size_t before the multiply,
 * and block_size's uint32_t operand promotes to size_t too, so this
 * particular product cannot alias a small value; A.1.4 below probes
 * this more directly). make_room_for_new_entry() (src/kes_cache.c)
 * checks stats.memory_used + aligned_needed <= config.max_memory
 * BEFORE any allocation is attempted -- since ~16 TiB exceeds any
 * realistic config.max_memory (the default edge config here is 8MB),
 * this fails immediately on an empty cache with nothing to evict, and
 * kes_cache_get_extent() returns KES_ERROR_BUSY without ever calling
 * aligned_alloc(). This is the "clean failure, not a crash or huge
 * allocation attempt" the plan asks for -- observed to be BUSY, not
 * the NOMEM the plan speculated as a possibility, because the
 * capacity check rejects the request before allocation is ever
 * attempted.
 */
static bool test_block_count_zero_and_max(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    void *buf = NULL;
    kes_extent_id_t zero_count = { .start_block = 5, .block_count = 0,
                                    .block_size = EDGE_BLOCK_SIZE,
                                    .reserved = 0 };
    kes_extent_id_t max_count = { .start_block = 6,
                                   .block_count = UINT32_MAX,
                                   .block_size = EDGE_BLOCK_SIZE,
                                   .reserved = 0 };
    int result;

    default_config( &cfg);
    cache = kes_cache_create( &cfg);
    TEST_ASSERT( cache != NULL, "cache creation for block_count test");
    kes_cache_set_io_callbacks( cache, mock_read, mock_write, mock_sync);

    result = kes_cache_get_extent( cache, &zero_count, &buf);
    TEST_ASSERT( result == KES_SUCCESS,
                "block_count == 0 currently succeeds with a 0-byte "
                "cached entry -- observed behavior, not an assumed "
                "one (see comment above); no crash either way");
    TEST_ASSERT( buf != NULL,
                "buffer for a 0-byte entry is still a valid, non-NULL "
                "pointer (aligned_alloc(64, 0) on this platform)");
    kes_cache_put_extent( cache, &zero_count);

    result = kes_cache_get_extent( cache, &max_count, &buf);
    TEST_ASSERT( result == KES_ERROR_BUSY,
                "block_count == UINT32_MAX is cleanly rejected as "
                "BUSY (capacity check fails before any allocation "
                "is attempted) rather than crashing or actually "
                "trying to allocate ~16TiB");

    kes_cache_destroy( cache);
    TEST_SUCCESS( "block_count == 0 / UINT32_MAX handled without "
                  "crash or unbounded allocation");
}

/* ================================================================
 * A.1.4 -- start_block * block_size 64-bit overflow, cache-layer
 * analogue of test_kes_extent_read_write_32bit_overflow (storage
 * layer, tests/test_kes_storage_full.c).
 *
 * Reading src/kes_cache.c end to end first: id->start_block is used
 * only for hashing (kes_extent_hash), equality (kes_extent_equal),
 * and log messages in this module -- never in any size/offset
 * arithmetic (grep confirms zero other uses). The only place a
 * kes_extent_id_t feeds a size computation is extent_data_size():
 * (size_t)id->block_count * id->block_size. Since id->block_count is
 * explicitly cast to size_t (64-bit on this platform) before the
 * multiply, and id->block_size (uint32_t) is promoted to size_t by
 * the usual arithmetic conversions rather than staying a 32-bit
 * operand, this product cannot silently truncate to 32 bits the way
 * the storage layer's pre-fix bug did.
 *
 * This test still exercises the "start_block near UINT64_MAX, large
 * block_count" shape the plan asks for (in case some future change
 * reintroduces start_block into a size computation), and directly
 * proves extent_data_size() computes the true 64-bit product rather
 * than a 32-bit-wrapped one -- reusing the exact block_count/
 * block_size pair test_kes_extent_read_write_32bit_overflow uses
 * (600000 * 8192), where:
 *   true size (64-bit)   = 4,915,200,000 bytes (~4.58 GiB)
 *   wrapped size (32-bit)=   620,232,704 bytes (~591 MiB)
 * A custom cache is configured with max_memory = 700,000,000 bytes,
 * strictly between those two values, so the two possible outcomes
 * are distinguishable by kes_cache_get_extent()'s return code alone:
 *   - true (unwrapped) size: 4.9GB > 700MB max_memory -->
 *     make_room_for_new_entry() fails its capacity check on an empty
 *     cache with nothing to evict --> KES_ERROR_BUSY, no allocation
 *     ever attempted. This is the actual, observed result.
 *   - a hypothetical wrapped (591MB) size would fit under 700MB -->
 *     the miss path would actually attempt aligned_alloc(591MB)
 *     (which would very likely succeed) and then call this test's
 *     mock read_extent with size=591MB, which mock_read() rejects
 *     (KES_ERROR_IO, since it only accepts sizes up to
 *     EDGE_BLOCK_SIZE) -- a clearly different, distinguishable
 *     result from BUSY.
 */
static bool test_start_block_near_max_no_size_wrap(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    void *buf = NULL;
    kes_extent_id_t huge = { .start_block = UINT64_MAX - 10,
                              .block_count = 600000,
                              .block_size = 8192, .reserved = 0 };
    int result;

    memset( &cfg, 0, sizeof(cfg));
    cfg.max_memory = 700000000;         /* strictly between the
                                          * wrapped and true sizes */
    cfg.min_memory = KES_CACHE_MIN_MEMORY;
    cfg.max_entries = KES_CACHE_MIN_ENTRIES;
    cfg.policy = KES_CACHE_LRU;
    cfg.background_threads = 1;
    cfg.sync_interval_ms = 1000;

    cache = kes_cache_create( &cfg);
    TEST_ASSERT( cache != NULL,
                "cache creation for start_block overflow test");
    kes_cache_set_io_callbacks( cache, mock_read, mock_write, mock_sync);

    result = kes_cache_get_extent( cache, &huge, &buf);
    TEST_ASSERT( result == KES_ERROR_BUSY,
                "extent_data_size() computed the true ~4.58GiB size "
                "(BUSY, exceeds the 700MB max_memory ceiling), not a "
                "32-bit-wrapped ~591MiB size (which would have fit "
                "and produced KES_ERROR_IO from the undersized mock "
                "read instead)");

    kes_cache_destroy( cache);
    TEST_SUCCESS( "start_block near UINT64_MAX + large block_count: "
                  "no 64-bit size truncation in extent_data_size()");
}

/* ================================================================
 * A.1.5 -- pin/unpin imbalance and refcounted-pin semantics.
 *
 * Verified by reading src/kes_cache.c:1081-1139 (kes_cache_pin_extent/
 * kes_cache_unpin_extent): pinning is refcounted, not boolean.
 * kes_cache_pin_extent() increments entry->pin_count every call, only
 * setting KES_EXTENT_PINNED and bumping stats.entries_pinned on the
 * 0->1 transition; kes_cache_unpin_extent() decrements it, only
 * clearing pinned state and decrementing stats.entries_pinned on the
 * ->0 transition. An extra unpin beyond the pin count is a guarded
 * no-op (`if (entry->pin_count > 0)`) -- it does not underflow.
 * include/kes/kes_cache.h's doc comments for both functions were
 * updated in this same commit to document this plainly.
 *
 * This test pins an entry 3 times, unpins once (still pinned/
 * unevictable), unpins twice more (now evictable), and separately
 * confirms an unpin on a never-pinned entry is a safe KES_SUCCESS
 * no-op. "Unevictable" is proven the same way
 * test_cache_pinned_entries_never_evicted (tests/test_kes_cache.c)
 * does: fill a small-max_entries cache with unrelated traffic and
 * confirm the pinned entry is still a cache hit (never reloaded from
 * the mock backing store) afterward.
 */
static bool test_pin_unpin_refcount_semantics(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    void *buf = NULL;
    kes_extent_id_t pinned_id = make_id( 20);
    kes_cache_stats_t stats;
    kes_extent_id_t never_pinned = make_id( 21);

    memset( &cfg, 0, sizeof(cfg));
    cfg.max_memory = KES_CACHE_MIN_MEMORY;
    cfg.min_memory = KES_CACHE_MIN_MEMORY;
    cfg.max_entries = KES_CACHE_MIN_ENTRIES;   /* 16, small on purpose */
    cfg.policy = KES_CACHE_LRU;
    cfg.background_threads = 1;
    cfg.sync_interval_ms = 1000;

    cache = kes_cache_create( &cfg);
    TEST_ASSERT( cache != NULL, "cache creation for pin/unpin test");
    kes_cache_set_io_callbacks( cache, mock_read, mock_write, mock_sync);

    TEST_ASSERT(
        kes_cache_get_extent( cache, &pinned_id, &buf) == KES_SUCCESS,
        "get the entry that will be pinned 3 times");

    TEST_ASSERT( kes_cache_pin_extent( cache, &pinned_id) == KES_SUCCESS,
                "1st pin");
    TEST_ASSERT( kes_cache_pin_extent( cache, &pinned_id) == KES_SUCCESS,
                "2nd pin");
    TEST_ASSERT( kes_cache_pin_extent( cache, &pinned_id) == KES_SUCCESS,
                "3rd pin");
    kes_cache_get_stats( cache, &stats);
    TEST_ASSERT( stats.entries_pinned == 1,
                "3 pins on 1 entry still counts as 1 pinned entry "
                "(refcounted, not boolean)");
    kes_cache_put_extent( cache, &pinned_id);

    TEST_ASSERT( kes_cache_unpin_extent( cache,
                                         &pinned_id) == KES_SUCCESS,
                "1 of 3 unpins");

    /* Push far more distinct, unpinned entries through the cache
     * than max_entries allows, to create real eviction pressure. */
    for ( int i = 100; i < 140; i++) {
        kes_extent_id_t id = make_id( (uint64_t)i);
        void *b = NULL;
        TEST_ASSERT(
            kes_cache_get_extent( cache, &id, &b) == KES_SUCCESS,
            "get_extent failed while generating eviction pressure");
        kes_cache_put_extent( cache, &id);
    }

    kes_cache_get_stats( cache, &stats);
    TEST_ASSERT( stats.entries_pinned == 1,
                "still pinned after only 1 of 3 unpins");

    uint64_t hits_before = stats.hits;
    TEST_ASSERT(
        kes_cache_get_extent( cache, &pinned_id, &buf) == KES_SUCCESS,
        "still-pinned entry is still gettable after eviction "
        "pressure");
    kes_cache_get_stats( cache, &stats);
    TEST_ASSERT( stats.hits == hits_before + 1,
                "still-pinned entry survived as a cache hit, not "
                "evicted, after only 1 of 3 unpins");
    kes_cache_put_extent( cache, &pinned_id);

    TEST_ASSERT( kes_cache_unpin_extent( cache,
                                         &pinned_id) == KES_SUCCESS,
                "2 of 3 unpins");
    TEST_ASSERT( kes_cache_unpin_extent( cache,
                                         &pinned_id) == KES_SUCCESS,
                "3 of 3 unpins -- fully unpinned now");
    kes_cache_get_stats( cache, &stats);
    TEST_ASSERT( stats.entries_pinned == 0,
                "entries_pinned reaches 0 once pin count is fully "
                "drained");

    /* Extra unpin beyond the pin count: safe no-op, not an error. */
    TEST_ASSERT(
        kes_cache_unpin_extent( cache, &pinned_id) == KES_SUCCESS,
        "extra unpin beyond the pin count is a safe no-op, "
        "KES_SUCCESS, not an error");

    /* Unpin on a never-pinned (but cached) entry: also a safe no-op. */
    TEST_ASSERT(
        kes_cache_get_extent( cache, &never_pinned, &buf) == KES_SUCCESS,
        "get a never-pinned entry");
    TEST_ASSERT(
        kes_cache_unpin_extent( cache, &never_pinned) == KES_SUCCESS,
        "unpin on a never-pinned entry is KES_SUCCESS with no state "
        "change (current code does not special-case this as an "
        "error)");
    kes_cache_put_extent( cache, &never_pinned);

    kes_cache_destroy( cache);
    TEST_SUCCESS( "pin/unpin is refcounted: N pins require N unpins, "
                  "extra unpins are safe no-ops");
}

/* ================================================================
 * A.1.6 -- kes_cache_destroy() with an outstanding, never-released
 * kes_cache_get_extent() reference.
 *
 * Reading kes_cache_destroy() (src/kes_cache.c) first: it walks
 * cache->mru_head to cache->list_next unconditionally, freeing every
 * entry's data buffer, destroying its mutex/cond, and free()-ing the
 * struct -- it does NOT check entry->ref_count (or pin_count) at
 * all before doing so. So the actual, observed contract is: destroy()
 * frees every entry regardless of outstanding references, it does
 * not refuse or defer. This test proves that doing so does not crash
 * or corrupt anything by itself (get an extent, deliberately never
 * put_extent() it, then destroy()) -- it deliberately does NOT then
 * dereference the now-dangling buffer pointer afterward, since doing
 * that would be a real use-after-free this test is not trying to
 * prove is safe (it isn't -- destroy() invalidates the buffer, it
 * just doesn't check first). Per the task instructions this specific
 * test must also be confirmed under `make asan`, not just plain
 * `make test`, since a subtler defect here (e.g. destroy() itself
 * double-freeing, or corrupting bucket/LRU bookkeeping while an
 * entry is still logically referenced) is exactly the shape ASan
 * catches and a plain run would not.
 */
static bool test_destroy_with_outstanding_reference(void) {
    kes_cache_config_t cfg;
    kes_cache_t *cache;
    void *buf = NULL;
    kes_extent_id_t id = make_id( 30);
    int result;

    default_config( &cfg);
    cache = kes_cache_create( &cfg);
    TEST_ASSERT( cache != NULL,
                "cache creation for destroy-with-reference test");
    kes_cache_set_io_callbacks( cache, mock_read, mock_write, mock_sync);

    TEST_ASSERT( kes_cache_get_extent( cache, &id, &buf) == KES_SUCCESS,
                "get an extent and deliberately never put_extent() it "
                "-- ref_count stays at 1 through destroy() below");
    TEST_ASSERT( buf != NULL, "buffer non-NULL before destroy");

    result = kes_cache_destroy( cache);
    TEST_ASSERT( result == KES_SUCCESS,
                "destroy() with an outstanding reference still "
                "succeeds -- observed behavior: it frees the entry "
                "unconditionally rather than refusing or deferring, "
                "per this function's actual implementation");

    /* Deliberately does not touch `buf` here -- it is dangling now
     * that destroy() has freed the entry that owned it. */

    TEST_SUCCESS( "kes_cache_destroy() with an outstanding "
                  "get_extent() reference: no crash (frees the "
                  "entry regardless of ref_count -- run under "
                  "make asan to confirm no corruption)");
}

typedef struct {
    const char *name;
    bool ( *func)(void);
} test_case_t;

static test_case_t test_cases[] = {
    {"NULL parameter checks", test_null_parameter_checks},
    {"get_extent rejects invalid block_size",
     test_get_extent_rejects_non_power_of_2_block_size},
    {"block_count 0 and UINT32_MAX", test_block_count_zero_and_max},
    {"start_block near UINT64_MAX, no size wrap",
     test_start_block_near_max_no_size_wrap},
    {"destroy with outstanding reference",
     test_destroy_with_outstanding_reference},
    {"pin/unpin refcount semantics",
     test_pin_unpin_refcount_semantics},
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
