# KES Extents Cache Design Document

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
- Lock-free data structures where possible
- Fine-grained locking for critical sections
- Background thread for cache management

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
- **LRU (Least Recently Used)**: Default for general workloads
- **LFU (Least Frequently Used)**: For random access patterns
- **Custom**: Application-specific policies

### Memory Layout

```
Cache Memory Pool
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
    KES_EXTENT_FLUSHING = 0x10   /* Being written to disk */
} kes_extent_state_t;
```

### 3. Cache Configuration
```c
typedef struct {
    size_t max_memory;       /* Maximum cache memory */
    size_t min_memory;       /* Minimum cache memory */
    uint32_t max_entries;    /* Maximum cached extents */
    int eviction_policy;     /* LRU, LFU, etc. */
    int background_threads;  /* Number of bg threads */
    int sync_interval_ms;    /* Background sync interval */
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

/* Flush all dirty extents */
int kes_cache_sync(kes_cache_t* cache);
```

## Cache Algorithms

### 1. Hash Table for Fast Lookup
- Use robin hood hashing for better performance
- Hash function based on extent start_block and size
- Handle hash collisions with linear probing

### 2. LRU Implementation
```
Recently Used  <---->  Least Recently Used
+----------+    +----------+    +----------+
| Entry A  |<-->| Entry B  |<-->| Entry C  |
+----------+    +----------+    +----------+
```

### 3. Memory Management
- Pre-allocate memory pools to avoid fragmentation
- Use slab allocation for extent entries
- Support for huge pages on supported systems

## Background Thread Operations

### Cache Daemon Responsibilities
1. **Periodic Sync**: Flush dirty extents to disk
2. **Memory Pressure**: Evict clean extents when memory is low
3. **Prefetching**: Read-ahead for sequential access patterns
4. **Statistics**: Update cache hit/miss ratios and performance metrics

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
    .eviction_policy = KES_CACHE_LRU,
    .background_threads = 1,
    .sync_interval_ms = 5000            /* 5 seconds */
};
```

### Server Systems
```c
kes_cache_config_t server_config = {
    .max_memory = 512 * 1024 * 1024,    /* 512MB */
    .min_memory = 64 * 1024 * 1024,     /* 64MB */
    .max_entries = 4096,
    .eviction_policy = KES_CACHE_LFU,
    .background_threads = 4,
    .sync_interval_ms = 1000            /* 1 second */
};
```

## Testing Strategy

### Unit Tests
- Hash table operations
- LRU/LFU algorithm correctness
- Memory management
- Thread safety

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
