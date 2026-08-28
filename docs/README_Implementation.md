# KES Cache Implementation Guide

## Standing note (Phase 6 docs truth pass)

This document had a stale "LRU/LFU/Custom" eviction claim (only LRU
is implemented -- `kes_cache_create()` rejects the other two with
`NULL`), a "Multi-Policy Eviction" claim in an earlier revision (since
removed here), and several build commands (`make clean edge`, `CC=...
make clean edge`) referencing a `edge` Makefile target that does not
exist -- there is no edge/server build variant, only `make all` with
fixed flags. Both corrected below against the real root `Makefile`
and `include/kes/kes_cache.h`. See `AGENTS.md`'s "Ground truth"
section and `PENDING_ITEMS.md` for the current, authoritative status.

## Overview

This document provides a comprehensive implementation of the caching system 
for KANEK Extents Storage (KES), designed to meet your requirements for a 
low-level block/extents storage management library.

## Key Features Implemented

### ✅ Platform Support
- **Cross-platform**: Works on ARM, x86, and other POSIX systems
- **Edge device optimized**: Memory-efficient for tablets, smartphones
- **Server optimized**: High-performance for server workloads
- **Thread-safe**: Fine-grained locking and lock-free operations

### ✅ Cache Architecture
- **Hash table**: extent lookup via chained hashing (1024 buckets by
  default), each bucket protected by its own `pthread_rwlock_t`
- **LRU eviction**: the only implemented policy --
  `kes_cache_policy_t` also declares `KES_CACHE_LFU`/`KES_CACHE_CUSTOM`,
  but `kes_cache_create()` rejects both (returns `NULL`) rather than
  falling back to LRU silently
- **Alignment**: each extent's data buffer is `aligned_alloc()`'d to
  `KES_CACHE_ALIGNMENT`; there is no pre-allocated memory pool despite
  `kes_cache_config_t`'s `memory_pool`/`pool_size`/`free_list` fields
  existing in the struct -- see `kes_cache_design.md`'s "Memory
  Layout" section
- **Background threads**: real, `pthread_create()`-based threads
  (`kes_cache_start()`), asynchronously running the same
  flush-then-evict sweep `kes_cache_sync()` runs manually

### ✅ Extent Operations
- **Get/Put**: Reference counting for safe access
- **Pin/Unpin**: Prevent eviction of critical extents
- **Dirty tracking**: Efficient write-back caching
- **Sync operations**: Batch and individual flushing

## Quick Start Guide

### 1. Build the Library

There is no separate `edge`/server build variant -- `make all` always
uses the same fixed flags (`-std=c99 -Wall -Wextra -Werror -fPIC`,
`-O2 -DNDEBUG`). Edge-appropriate cache *configuration* (not a
different build) comes from `kes_cache_get_default_config(&config,
true)` at runtime -- see "Platform-Specific Optimizations" below.

```bash
# Build the library + test binaries
make clean all

# Debug build (DEBUG=1: -g3 -O0 -DDEBUG)
make clean debug

# Cross-compilation for ARM (CC alone, no CROSS_COMPILE variable)
make clean CC=arm-linux-gnueabihf-gcc all
```

### 2. Basic Usage Example

```c
#include <kes/kes_cache.h>

int main() {
    // Configure cache for your platform
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false); // false = server
    
    // Create cache instance
    kes_cache_t* cache = kes_cache_create(&config);
    if (!cache) {
        return -1;
    }
    
    // Set up I/O callbacks (your storage functions)
    kes_cache_set_io_callbacks(cache, 
                               my_read_function,
                               my_write_function,
                               my_sync_function);
    
    // Define an extent
    kes_extent_id_t extent = {
        .start_block = 100,
        .block_count = 8,
        .block_size = 8192
    };
    
    // Get extent data (loads from storage if not cached)
    void* buffer;
    if (kes_cache_get_extent(cache, &extent, &buffer) == KES_SUCCESS) {
        // Use the buffer
        memcpy(buffer, my_data, extent.block_count * extent.block_size);
        
        // Mark as dirty if modified
        kes_cache_mark_dirty(cache, &extent);
        
        // Release reference
        kes_cache_put_extent(cache, &extent);
    }
    
    // Cleanup
    kes_cache_sync(cache);        // Flush all dirty extents
    kes_cache_destroy(cache);
    
    return 0;
}
```

## Implementation Architecture

### Core Data Structures

```
Cache Structure:
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│ Hash Table      │────│ Extent Entries  │────│ LRU Lists       │
│ - Buckets       │    │ - ID            │    │ - MRU Head      │
│ - Read/Write    │    │ - Data Buffer   │    │ - LRU Tail      │
│   Locks         │    │ - State Flags   │    │ - List Links    │
└─────────────────┘    │ - Ref Count     │    └─────────────────┘
                       │ - Access Info   │
                       └─────────────────┘
```

### Memory Layout Optimization

```
Extent Entry Layout (64-byte aligned):
┌──────────────────┬──────────────────┬──────────────────┐
│ Extent ID (16B)  │ Metadata (32B)   │ Links (16B)      │
├──────────────────┼──────────────────┼──────────────────┤
│ start_block      │ state, ref_count │ hash_next        │
│ block_count      │ pin_count        │ hash_prev        │ 
│ block_size       │ access_time      │ list_next        │
│ reserved         │ access_count     │ list_prev        │
└──────────────────┴──────────────────┴──────────────────┘
```

## Advanced Usage Patterns

### 1. Extent Pinning (Critical Data)

```c
// Pin extent to prevent eviction
kes_extent_id_t critical_extent = {...};
kes_cache_pin_extent(cache, &critical_extent);

// Use the extent...

// Unpin when done
kes_cache_unpin_extent(cache, &critical_extent);
```

### 2. Batch Operations

```c
// Process multiple extents efficiently
kes_extent_id_t extents[10];
void* buffers[10];

// Get all extents
for (int i = 0; i < 10; i++) {
    kes_cache_get_extent(cache, &extents[i], &buffers[i]);
}

// Process data...

// Release all at once
for (int i = 0; i < 10; i++) {
    kes_cache_put_extent(cache, &extents[i]);
}
```

### 3. Custom Eviction Policy -- NOT IMPLEMENTED

```c
// config.policy = KES_CACHE_CUSTOM;
// kes_cache_create(&config) returns NULL for this -- KES_CACHE_CUSTOM
// (like KES_CACHE_LFU) is declared in kes_cache_policy_t but has no
// implementation anywhere in src/kes_cache.c. There is no
// custom_entry_data_t or pluggable-policy hook in the real API.
```

## Platform-Specific Optimizations

### Edge Devices (Phones, Tablets)

```c
kes_cache_config_t edge_config = {
    .max_memory = 8 * 1024 * 1024,       // 8MB max
    .min_memory = 2 * 1024 * 1024,       // 2MB min
    .max_entries = 256,                   // Limited entries
    .policy = KES_CACHE_LRU,              // Simple LRU
    .background_threads = 1,              // Single thread
    .sync_interval_ms = 5000,             // 5s intervals
    .enable_prefetch = false,             // Disable prefetch
    .enable_compression = true            // Save memory
};
```

### Server Systems

```c
kes_cache_config_t server_config = {
    .max_memory = 512 * 1024 * 1024,     // 512MB max
    .min_memory = 64 * 1024 * 1024,      // 64MB min  
    .max_entries = 4096,                  // Many entries
    .policy = KES_CACHE_LRU,              // KES_CACHE_LFU is rejected
                                           // by kes_cache_create() --
                                           // not implemented
    .background_threads = 4,              // Multi-threaded
    .sync_interval_ms = 1000,             // 1s intervals
    .enable_prefetch = true,              // Accepted, but currently
                                           // inert -- no prefetch
                                           // logic exists yet
    .enable_compression = false           // Speed over size
};
```

## Performance Tuning Guidelines

### 1. Hash Table Sizing
- Default: 1024 buckets (good for most workloads)
- High concurrency: Increase buckets to reduce lock contention
- Low memory: Reduce buckets to save memory

### 2. Memory Pool Configuration
- Pre-allocate pools to avoid malloc() overhead
- Use huge pages on supported systems
- Align to cache line boundaries (64 bytes)

### 3. Thread Configuration
```c
// Single thread for simple workloads
config.background_threads = 1;

// Multiple threads for high I/O
config.background_threads = min(4, cpu_cores);
```

### 4. Sync Strategy
```c
// Frequent sync (low latency, higher overhead)
config.sync_interval_ms = 100;

// Infrequent sync (higher latency, lower overhead)  
config.sync_interval_ms = 5000;
```

## Integration with KES Storage

### Storage Device Integration

```c
// Your storage device handle
typedef struct {
    int fd;
    uint64_t device_size;
    uint32_t block_size;
    // ... other device-specific fields
} kes_device_t;

// I/O callback implementation
int my_read_extent(void* device, const kes_extent_id_t* id,
                   void* buffer, size_t size) {
    kes_device_t* dev = (kes_device_t*)device;
    
    off_t offset = id->start_block * id->block_size;
    ssize_t bytes_read = pread(dev->fd, buffer, size, offset);
    
    return (bytes_read == size) ? 0 : -1;
}
```

### Bitmap Integration

```c
// Update bitmap when allocating extents
int allocate_extent(kes_device_t* device, kes_extent_id_t* extent) {
    // Find free blocks in bitmap
    uint64_t start_block = find_free_blocks(device->bitmap, 
                                           extent->block_count);
    if (start_block == INVALID_BLOCK) {
        return -1;
    }
    
    // Mark blocks as used
    mark_blocks_used(device->bitmap, start_block, extent->block_count);
    
    extent->start_block = start_block;
    extent->block_size = device->block_size;
    
    return 0;
}
```

## Testing and Validation

### Unit Tests
```bash
make test                 # Build + run all 6 test binaries (70/70
                           # as of the last verified check-in -- see
                           # TESTS_AND_EXAMPLES.md and PENDING_ITEMS.md)
```

There is no single `build/test_runner` binary -- each `tests/test_*.c`
builds to its own binary under `build/tests/`; run one directly, e.g.
`build/tests/test_kes_cache`.

### Performance Benchmarks

**Not implemented.** There is no `make benchmark` target and no
benchmark source files anywhere in this repository. `KES_HARDENING_PLAN.md`
§6.G and `plan_phase5.md`'s Track A.7 describe adding an informational
perf-smoke test as future work.

### Memory Analysis
```bash
# Build with debug symbols
make debug

# The project's own sanitizer/valgrind targets already run every
# test binary for you -- prefer these over invoking valgrind by hand:
make valgrind             # clean rebuild + Valgrind over all tests
make asan                 # clean rebuild + ASan+UBSan over all tests
make tsan                 # clean rebuild + TSan over all tests
make check-all             # all of the above, gated on all passing
```

## Error Handling Best Practices

### 1. Return Code Checking
```c
int result = kes_cache_get_extent(cache, &extent, &buffer);
switch (result) {
    case KES_SUCCESS:
        // Success path
        break;
    case KES_ERROR_NOMEM:
        // Handle out of memory
        break;
    case KES_ERROR_IO:
        // Handle I/O error
        break;
    default:
        // Handle other errors
        break;
}
```

### 2. Resource Cleanup
```c
// Always pair get/put operations
void* buffer;
if (kes_cache_get_extent(cache, &extent, &buffer) == KES_SUCCESS) {
    // Use buffer...
    
    // Always release reference
    kes_cache_put_extent(cache, &extent);
}
```

### 3. Graceful Degradation
```c
// Handle cache failures gracefully
if (kes_cache_get_extent(cache, &extent, &buffer) != KES_SUCCESS) {
    // Fall back to direct I/O
    buffer = malloc(extent_size);
    direct_read_extent(device, &extent, buffer);
}
```

## Future Enhancement Ideas

### 1. Compression Support
- LZ4 compression for cold data
- Adaptive compression based on data patterns
- Memory pressure triggers

### 2. Tiered Storage
- NVMe cache tier
- RAM -> SSD -> HDD hierarchy
- Automatic tier migration

### 3. Machine Learning
- Access pattern prediction
- Smart prefetching algorithms
- Adaptive eviction policies

### 4. Distributed Cache
- Cache coherency protocols
- Network-attached cache
- Cluster-aware eviction

## Troubleshooting Common Issues

### Issue: High Cache Miss Rate
**Diagnosis**: Check access patterns and cache size
```bash
# Monitor cache statistics
kes_cache_stats_t stats;
kes_cache_get_stats(cache, &stats);
printf("Hit rate: %.2f%%\n", 
       100.0 * stats.hits / (stats.hits + stats.misses));
```

**Solutions**:
- Increase cache memory
- Enable prefetching
- Optimize access patterns

### Issue: Memory Pressure
**Diagnosis**: Monitor memory usage
```bash
printf("Memory used: %zu MB\n", stats.memory_used / (1024*1024));
```

**Solutions**:
- Reduce max_memory setting
- Enable compression
- Tune eviction policy

### Issue: Lock Contention
**Diagnosis**: Use profiling tools
```bash
perf record -g ./your_application
perf report
```

**Solutions**:
- Increase hash table buckets
- Reduce lock hold times
- Use lock-free algorithms where possible

## Building on Different Platforms

**Not verified/not a distinct build mode.** The Makefile has no `edge`
target, no iOS/Android cross-compilation recipe, and no CI coverage
for any platform beyond the Linux/WSL2 environment described in
`AGENTS.md`. `CC=` overrides the compiler like any GNU Makefile, but
nothing beyond that (ARM64/iOS/Android toolchain wiring, NDK sysroot
flags, etc.) is provided or tested by this project:

```bash
# Linux x86_64 (the only environment this project is actually
# built/tested in)
make clean all

# Cross-compiler override -- untested by this project's own test
# suite; you are responsible for verifying the result
make clean CC=aarch64-linux-gnu-gcc all
```

This implementation provides a solid foundation for a KES-based cache
system. The design is portable C (no platform-specific code paths in
`src/kes_cache.c` beyond the standard POSIX/pthread APIs), but claims
about tested edge-device or iOS/Android builds should be treated as
aspirational until verified against a real cross-build.
