# KES Extents Cache Design Document

## Standing note (Phase 6 docs truth pass)

This document is mostly accurate for the parts of the cache that are
implemented, but had a stale `eviction_policy` field name, a missing
error state, an LFU config example (LFU is rejected, not implemented),
and an incorrect "robin hood hashing + linear probing" collision
description -- all corrected below against `include/kes/kes_cache.h`
and `src/kes_cache.c` directly. Per `plan_phase5.md`'s Track B.4
guidance, semantics for `kes_cache_sync()`/`kes_cache_invalidate()`/
`kes_cache_reset_stats()`/`kes_cache_start()` below are cited from
`PENDING_ITEMS.md`'s "Resolved" section rather than re-derived from
source. See `AGENTS.md`'s "Ground truth" section for the current,
authoritative implementation status.

## Overview
The KES extents cache is designed to provide efficient in-memory caching of 
disk extents (contiguous blocks) to improve I/O performance across various 
platforms including edge devices and servers.

## Core Design Principles

### 1. Platform Agnostic
- Works on ARM and x86 architectures
- Compatible with POSIX systems (Linux, iOS, etc.)
- Optimized for both high-performance servers and resource-constrained edge
  devices

### 2. Memory Efficiency
- Adaptive cache sizing based on available system memory
- Configurable cache policies (LRU, LFU, or custom)
- Support for memory-mapped I/O where appropriate

### 3. Thread Safety
- Fine-grained locking: a `pthread_rwlock_t` per hash bucket, a
  `pthread_mutex_t`/`pthread_cond_t` per entry, and one cache-wide
  `pthread_mutex_t` (`cache_lock`) for LRU-list/eviction bookkeeping.
  This is **not** lock-free -- getting the locking right across these
  domains (a traversal-vs-lookup race, a `ref_count`-under-two-locks
  race, and a `pthread_cond_wait()` unlock-while-parked hazard) is
  covered in detail in `try_evict_entry_locked()`'s doc comment in
  `src/kes_cache.c` and the Phase 3 entry in `PENDING_ITEMS.md`; read
  that before touching this code.
- Background thread(s) for cache management (`kes_cache_start()`)

## Cache Architecture

### Core Components

#### 1. Cache Manager (`kes_cache_mgr`)
```
+------------------+
|   Cache Manager  |
|------------------|
| - Hash table     |
| - LRU/LFU lists  |
| - Memory pool    |
| - Stats tracking |
| - Background     |
|   thread mgmt    |
+------------------+
```

#### 2. Extent Cache Entry (`kes_extent_entry`)
```
+------------------+
| Extent Entry     |
|------------------|
| - Extent ID      |
| - Memory buffer  |
| - State flags    |
| - Reference cnt  |
| - Dirty flag     |
| - Pin count      |
| - Access time    |
| - Size info      |
+------------------+
```

#### 3. Cache Policies
- **LRU (Least Recently Used)**: Default, and the **only implemented**
  policy.
- **LFU (Least Frequently Used)**: Declared in `kes_cache_policy_t`
  but **not implemented** -- `kes_cache_create()` returns `NULL` if
  `config.policy == KES_CACHE_LFU`, it does not silently fall back to
  LRU.
- **Custom**: Declared as `KES_CACHE_CUSTOM` but likewise **not
  implemented** -- also rejected with `NULL` at `kes_cache_create()`.

### Memory Layout

**Not implemented as a pool.** `kes_cache_t` declares `memory_pool`/
`pool_size`/`free_list` fields (`kes_cache.h`) but `src/kes_cache.c`
does not use them -- each `kes_extent_entry_t` is `calloc()`'d
individually, and each entry's data buffer is allocated separately via
`aligned_alloc()` (`KES_CACHE_ALIGNMENT`-aligned; see the comment at
`src/kes_cache.c:258`, which says plainly "For now, use regular
malloc. In production, this would use [a pool]"). The diagram below is
the pre-allocated-pool design the code comment describes as a future
direction, not current behavior:

```
Cache Memory Pool (design intent, not implemented)
+----------------------------------------+
| Extent Entry 1 | Data Buffer 1 |      |
+----------------------------------------+
| Extent Entry 2 | Data Buffer 2 |      |
+----------------------------------------+
| ...            | ...           |      |
+----------------------------------------+
| Free Space                             |
+----------------------------------------+
```

## Key Data Structures

### 1. Extent Identifier
```c
typedef struct {
    uint64_t start_block;    /* Starting block address */
    uint32_t block_count;    /* Number of contiguous blocks */
    uint32_t block_size;     /* Block size in bytes */
} kes_extent_id_t;
```

### 2. Cache Entry States
```c
typedef enum {
    KES_EXTENT_CLEAN    = 0x01,  /* In sync with disk */
    KES_EXTENT_DIRTY    = 0x02,  /* Modified, needs flush */
    KES_EXTENT_PINNED   = 0x04,  /* Cannot be evicted */
    KES_EXTENT_LOADING  = 0x08,  /* Being read from disk */
    KES_EXTENT_FLUSHING = 0x10,  /* Being written to disk */
    KES_EXTENT_ERROR    = 0x20   /* I/O error occurred */
} kes_extent_state_t;
```

(The `KES_EXTENT_ERROR` state was missing from this document
previously; it is set on a flush failure and asserted directly by
tests -- see `PENDING_ITEMS.md`'s fault-injection notes.)

### 3. Cache Configuration

**Note**: the field is `policy`, typed `kes_cache_policy_t` -- not
`eviction_policy`/`int` as an earlier version of this document showed:

```c
typedef struct {
    size_t max_memory;           /* Maximum cache memory */
    size_t min_memory;           /* Minimum cache memory */
    uint32_t max_entries;        /* Maximum cached extents */
    kes_cache_policy_t policy;   /* KES_CACHE_LRU is the only value
                                   * kes_cache_create() will accept */
    int background_threads;      /* Number of bg threads */
    int sync_interval_ms;        /* Background sync interval */
    bool enable_prefetch;        /* Accepted but currently inert --
                                   * no prefetch logic exists */
    bool enable_compression;     /* Accepted but currently inert --
                                   * no compression logic exists */
    void *device_handle;         /* Opaque handle passed through to
                                   * the I/O callbacks as `device` */
} kes_cache_config_t;
```

## Cache Operations

### Core APIs

#### Cache Lifecycle
```c
kes_cache_t* kes_cache_create(const kes_cache_config_t* config);
int kes_cache_destroy(kes_cache_t* cache);
int kes_cache_start(kes_cache_t* cache);
int kes_cache_stop(kes_cache_t* cache);
```

#### Extent Operations
```c
/* Get extent (load if not cached) */
int kes_cache_get_extent(kes_cache_t* cache, 
                        const kes_extent_id_t* id,
                        void** buffer);

/* Release extent reference */
int kes_cache_put_extent(kes_cache_t* cache,
                        const kes_extent_id_t* id);

/* Pin extent in memory */
int kes_cache_pin_extent(kes_cache_t* cache,
                        const kes_extent_id_t* id);

/* Unpin extent */
int kes_cache_unpin_extent(kes_cache_t* cache,
                          const kes_extent_id_t* id);

/* Mark extent as dirty */
int kes_cache_mark_dirty(kes_cache_t* cache,
                        const kes_extent_id_t* id);

/* Flush specific extent */
int kes_cache_flush_extent(kes_cache_t* cache,
                          const kes_extent_id_t* id);

/* Flush all dirty extents, then free unreferenced/unpinned entries.
 * A clean no-op on an empty cache. See "Semantics of the four
 * previously-missing functions" below. */
int kes_cache_sync(kes_cache_t* cache);

/* Discard a cached extent WITHOUT writing back dirty data.
 * KES_ERROR_NOTFOUND if uncached, KES_ERROR_BUSY if referenced or
 * pinned. */
int kes_cache_invalidate(kes_cache_t* cache, const kes_extent_id_t* id);
```

This document previously omitted `kes_cache_invalidate()`,
`kes_cache_reset_stats()`, `kes_cache_get_stats()`, and
`kes_cache_set_io_callbacks()` entirely -- all four are implemented in
`src/kes_cache.c` and declared in `include/kes/kes_cache.h`; see
`docs/KES_API_Reference.md`'s "Cache API" section for their full
signatures.

### Semantics of the four previously-missing functions (per `PENDING_ITEMS.md`)

- **`kes_cache_sync()`**: flushes every dirty extent, then frees any
  entry that is unreferenced and unpinned after its flush attempt.
  Entries still referenced or pinned are flushed if dirty but never
  freed by `sync()` -- a durability checkpoint, not a full eviction
  pass.
- **`kes_cache_invalidate()`**: discards dirty data unconditionally,
  no implicit flush -- call `kes_cache_flush_extent()` first if the
  data needs to survive.
- **`kes_cache_reset_stats()`**: zeros only the cumulative counters
  (`hits`, `misses`, `evictions`, `flushes`, `bytes_read`,
  `bytes_written`); does **not** zero the state counters
  (`memory_used`, `entries_cached`, `entries_dirty`, `entries_pinned`),
  since those describe current contents, not history.
- **`kes_cache_start()`**: launches real background threads (one per
  `config.background_threads`), each waking every
  `config.sync_interval_ms` against `CLOCK_MONOTONIC` and running the
  same flush-then-evict sweep `kes_cache_sync()` runs manually.
  Returns `KES_ERROR_EXISTS` if already running,
  `KES_ERROR_INVALID` if `config.background_threads <= 0`.

## Cache Algorithms

### 1. Hash Table for Fast Lookup
- 1024 fixed buckets by default (`KES_CACHE_DEFAULT_BUCKETS`,
  `src/kes_cache.c`), each protected by its own `pthread_rwlock_t`.
- Hash function (`kes_extent_hash()`) combines `start_block`,
  `block_count`, and `block_size`.
- Collisions are handled with **separate chaining** (an intrusive
  doubly-linked list per bucket via `hash_next`/`hash_prev`), not
  robin hood hashing or linear probing -- an earlier version of this
  document described the wrong collision strategy.

### 2. LRU Implementation
```
Recently Used  <---->  Least Recently Used
+----------+    +----------+    +----------+
| Entry A  |<-->| Entry B  |<-->| Entry C  |
+----------+    +----------+    +----------+
```

### 3. Memory Management (design intent, not implemented)
- Pre-allocate memory pools to avoid fragmentation -- currently plain
  `calloc()`/`aligned_alloc()` per entry (see "Memory Layout" above).
- Use slab allocation for extent entries -- not implemented.
- Support for huge pages on supported systems -- not implemented.

## Background Thread Operations

### Cache Daemon Responsibilities

`kes_cache_start()`'s background threads run the same
flush-then-evict sweep `kes_cache_sync()` runs manually
(`cache_sweep()`, `src/kes_cache.c`), timed against `CLOCK_MONOTONIC`
every `config.sync_interval_ms`:
1. **Periodic Sync**: Flush dirty extents to disk -- implemented.
2. **Capacity Eviction**: Free unreferenced/unpinned entries as part
   of the same sweep (the same mechanism `kes_cache_get_extent()` uses
   on a miss) -- implemented, but it is not a distinct
   "memory-pressure-triggered" path; there is no separate system
   memory-pressure signal being read.
3. **Prefetching**: Read-ahead for sequential access patterns -- **not
   implemented**. `config.enable_prefetch` is accepted but currently
   inert.
4. **Statistics**: Cache hit/miss/eviction/flush counters are updated
   as a side effect of the sweep, not by a separate stats-collection
   pass.

### Sync Strategy
```
Dirty Extents Queue
+--------+--------+--------+
| Ext A  | Ext B  | Ext C  |
+--------+--------+--------+
     |
     v
Background Thread
     |
     v
Batch Write to Disk
```

## Memory Pressure Handling

### Adaptive Cache Sizing
1. Monitor system memory pressure
2. Shrink cache when system is under pressure
3. Grow cache when memory is available
4. Never go below minimum configured size

### Eviction Policies
1. **Clean First**: Always evict clean extents before dirty ones
2. **Unpinned Only**: Never evict pinned extents
3. **Age Based**: Prefer older, less accessed extents

## Error Handling

### Recovery Strategies
1. **I/O Errors**: Retry with exponential backoff
2. **Memory Allocation Failures**: Trigger emergency eviction
3. **Corruption Detection**: Validate extent checksums
4. **Deadlock Prevention**: Consistent locking order

## Performance Optimizations

### For Edge Devices
1. **Reduced Memory Footprint**: Smaller default cache sizes
2. **Power Awareness**: Batch I/O to reduce wakeups
3. **CPU Efficiency**: Lock-free algorithms where possible

### For Servers
1. **NUMA Awareness**: Allocate memory on correct NUMA nodes
2. **Multi-threading**: Parallel I/O operations
3. **Large Page Support**: Reduce TLB pressure

## Configuration Guidelines

### Edge Devices (Phones, Tablets)
```c
kes_cache_config_t edge_config = {
    .max_memory = 8 * 1024 * 1024,      /* 8MB */
    .min_memory = 2 * 1024 * 1024,      /* 2MB */
    .max_entries = 256,
    .policy = KES_CACHE_LRU,
    .background_threads = 1,
    .sync_interval_ms = 5000            /* 5 seconds */
};
```

`kes_cache_get_default_config(&config, true /* is_edge_device */)`
fills in edge-appropriate defaults directly -- see
`kes_cache_get_default_config()` in `src/kes_cache.c`.

### Server Systems

**Note**: `KES_CACHE_LFU` is rejected by `kes_cache_create()` (returns
`NULL`) -- an earlier version of this example used it, which would
never actually create a cache. Only `KES_CACHE_LRU` works today:

```c
kes_cache_config_t server_config = {
    .max_memory = 512 * 1024 * 1024,    /* 512MB */
    .min_memory = 64 * 1024 * 1024,     /* 64MB */
    .max_entries = 4096,
    .policy = KES_CACHE_LRU,            /* NOT KES_CACHE_LFU --
                                          * rejected, not implemented */
    .background_threads = 4,
    .sync_interval_ms = 1000            /* 1 second */
};
```

## Testing Strategy

### Unit Tests
- Hash table operations
- LRU algorithm correctness (LFU is not implemented, so there is
  nothing to test there)
- Memory management
- Thread safety

Actual coverage: `tests/test_kes_cache.c` (23/23) and
`tests/test_kes_cache_full.c` (12/12) -- see `TESTS_AND_EXAMPLES.md`
for what each covers.

### Integration Tests
- Full cache lifecycle
- Concurrent access patterns
- Memory pressure scenarios
- I/O error handling

### Performance Tests
- Cache hit/miss ratios
- Throughput under various workloads
- Memory usage patterns
- Latency measurements

## Future Enhancements

1. **Compression**: Compress cached extents to save memory
2. **Tiered Storage**: Support for NVMe/SSD cache tiers
3. **Machine Learning**: ML-based prefetching and eviction
4. **Distributed Cache**: Share cache across multiple nodes
