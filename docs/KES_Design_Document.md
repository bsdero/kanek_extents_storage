# KANEK Extents Storage (KES) Design Document
## Version 1.0

---

## Standing note (Phase 6 docs truth pass)

This document is largely **design-intent written well ahead of the
implementation**, not a description of current behavior -- treat it
the way `AGENTS.md` says to treat the rest of `docs/`. As of this
writing, the actually-implemented library is three source files
(~1600 lines total): a block bitmap (`kes_bitmap.c`), a first-fit-only
extent allocator with persistence (`kes_storage.c`), and an
independent LRU extent cache (`kes_cache.c`). Concretely, **none of
the following exist in `src/*.c` or `include/kes/*.h`**: the buddy
system / slab / log-structured / hybrid allocators, flash zones
(hot/warm/cold), wear leveling (static or dynamic), garbage collection
of any kind, the build-time hardware-profile system
(`PROFILE=smartphone`/`kes_profile_load()`/memory-budget levels), LFU
or Clock cache eviction, compression, encryption, or multi-device
support. Sections below describing these are kept as forward-looking
design sketches (per `plan_phase5.md`'s Track B.2 guidance) rather
than deleted, but every such section is now explicitly marked
**Not implemented**. Where a section describes something that *is*
implemented, it has been corrected to match `src/*.c` exactly. See
`AGENTS.md`'s "Ground truth" section and `PENDING_ITEMS.md` for the
authoritative, maintained status.

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Architecture Overview](#architecture-overview)
3. [Core Components](#core-components)
4. [Platform Adaptations](#platform-adaptations)
5. [Flash-Aware Design](#flash-aware-design)
6. [Build System & Profiles](#build-system--profiles)
7. [Performance Characteristics](#performance-characteristics)
8. [Integration Guidelines](#integration-guidelines)
9. [Future Roadmap](#future-roadmap)

---

## Executive Summary

### Project Goals

KANEK Extents Storage (KES) is a low-level block/extents storage 
management library designed for cross-platform deployment across edge 
devices and high-performance servers. KES provides efficient storage 
management with platform-specific optimizations while maintaining a 
unified API surface.

### Key Design Principles

1. **Platform Agnostic**: Single codebase supporting ARM, x86, POSIX 
   systems
2. **Flash-Aware**: F2FS-inspired optimizations for flash storage 
   longevity
3. **Configurable Performance**: Multiple allocation strategies 
   optimizable for speed, fragmentation, or memory usage
4. **Flexible Layout**: Customizable storage component placement
5. **Build-Time Optimization**: Platform profiles selected at compile 
   time

### Target Platforms

- **Edge Devices**: Smartphones, tablets, embedded systems with eMMC/UFS
- **Laptops**: Linux/macOS laptops with SSD storage
- **Servers**: High-performance servers with NVMe storage
- **Embedded**: Microcontrollers with raw NAND flash

---

## Architecture Overview

### Storage Layout Architecture

KES implements a flexible storage layout that can be customized based 
on deployment requirements:

```
Default Layout:
┌─────────────────┬─────────────────┬─────────────────┬─────────────────┐
│ Storage         │ Extent Index    │ User Data       │ Block Bitmap    │
│ Descriptor      │ (Optional)      │ Extents         │                 │
│ (Block 0)       │ (Block 1+)      │                 │ (Last Blocks)   │
└─────────────────┴─────────────────┴─────────────────┴─────────────────┘

Flexible Layout Examples:
┌─────────────────┬─────────────────┬─────────────────┬─────────────────┐
│ User Data Only  │ External Index  │ External Bitmap │ Custom Layout   │
└─────────────────┴─────────────────┴─────────────────┴─────────────────┘
```

### Core Architectural Components

#### 1. Storage Descriptor
- **Location**: Block 0 (default) or external management
- **Purpose**: Metadata describing storage layout and configuration
- **Content**: Block size, layout information, feature flags, statistics
- **Serialization**: Binary/JSON formats for external storage

#### 2. Extent Index (Optional)
- **Location**: Configurable block location or external file
- **Purpose**: Array of extent descriptors for fast extent lookup
- **Optimization**: Can be disabled to save space on constrained devices

#### 3. Block Bitmap
- **Location**: block 0's descriptor points at a fixed bitmap region
  in the backing file (`bitmap_start_block`/`bitmap_blocks` in
  `kes_storage_descriptor_t`) -- there is no external-management
  option.
- **Purpose**: Track free/used blocks.
- **Strategies**: **only "full memory" actually exists.** `kes_bitmap_t`
  is a single flat in-memory `uint8_t *` buffer, loaded whole on open
  and saved whole on sync -- sliding-window and on-demand loading are
  design intent only, not implemented.

#### 4. User Data Area
- **Location**: Between metadata and bitmap
- **Organization**: **not implemented.** There is no zone concept of
  any kind -- `kes_extent_allocate()` does a single first-fit scan
  over the whole bitmap, with no per-zone strategy selection.
- **Flash Zones**: Hot/warm/cold data separation for wear leveling --
  not implemented; see "Flash-Aware Design" below.

### Multi-Strategy Allocation Engine

**Actual status**: `kes_allocation_strategy_t` (`kes_types.h`) declares
four strategies -- `KES_ALLOC_FIRST_FIT`, `KES_ALLOC_BEST_FIT`,
`KES_ALLOC_WORST_FIT`, `KES_ALLOC_NEXT_FIT` -- but
`kes_extent_allocate()` always calls `allocate_extent_first_fit()`
(`src/kes_storage.c`) regardless of `config.strategy`. There is no
buddy-system, slab, log-structured, or hybrid allocator implementation
anywhere in this codebase; the list below is design-intent only:

```c
Allocation Strategies (design intent -- only First Fit implemented):
├── First Fit (Speed Optimized)              -- IMPLEMENTED
├── Best Fit (Fragmentation Optimized)        -- Not implemented
├── Buddy System (Memory Efficient)           -- Not implemented
├── Slab Allocator (Fixed-Size Optimized)     -- Not implemented
├── Log-Structured (Flash Optimized)          -- Not implemented
└── Hybrid (Adaptive Selection)               -- Not implemented
```

---

## Core Components

### 1. Storage Management Layer

#### Storage Descriptor Management
```c
struct kes_storage_descriptor {
    uint32_t magic;                 // Validation magic number
    uint16_t version_major;         // Major version
    uint16_t version_minor;         // Minor version
    uint32_t block_size;            // Block size (4K-64K)
    uint64_t total_blocks;          // Total storage blocks
    
    // Layout configuration
    uint64_t index_start_block;     // Index location
    uint64_t index_blocks;          // Index size
    uint64_t bitmap_start_block;    // Bitmap location
    uint64_t bitmap_blocks;         // Bitmap size
    uint64_t user_start_block;      // User data start
    uint64_t user_blocks;           // User data size
    
    // Feature flags and statistics
    uint32_t flags;                 // Feature enable flags
    uint64_t free_blocks;           // Current free blocks
    uint64_t allocated_extents;     // Current extent count
};
```

#### Flexible Descriptor Location
- **Internal Storage**: Descriptor stored in block 0 with optional 
  byte offset
- **External Management**: Upper layer provides descriptor buffer 
  with save/load callbacks
- **Shared Block Support**: Descriptor can coexist with upper layer 
  data in same block

### 2. Block Bitmap Management

#### Adaptive Bitmap Strategies
```c
typedef enum {
    KES_BITMAP_FULL_MEMORY,        // Entire bitmap in RAM (servers)
    KES_BITMAP_SLIDING_WINDOW,     // LRU cache of bitmap sections
    KES_BITMAP_ON_DEMAND,          // Load sections as needed (edge)
    KES_BITMAP_COMPRESSED,         // RLE compression in memory
    KES_BITMAP_HIERARCHICAL        // Multi-level bitmap tree
} kes_bitmap_strategy_t;
```

#### Bitmap Operations
- **Set Operations**: Mark blocks as used/free with atomic updates
- **Search Operations**: Find contiguous free blocks efficiently
- **Persistence**: Sync bitmap changes to storage with batching

### 3. Extent Allocation Engine

**Not implemented** -- there is no zone concept (hot/warm/cold/GC, or
size-based) anywhere in `src/kes_storage.c`. `kes_extent_allocate()`
does a single first-fit scan over the whole bitmap via
`kes_bitmap_find_free()`. The sections below describe a possible
future zone-based design, not current behavior.

#### Multi-Zone Architecture (design intent, not implemented)
```c
Flash-Aware Zones:
├── Hot Zone (Frequently updated metadata)
├── Warm Zone (Regular user data)
├── Cold Zone (Archive/sequential data)
└── GC Zone (Garbage collection target)

Size-Based Zones:
├── Small Extent Zone (<8 blocks)
├── Medium Extent Zone (8-64 blocks)
└── Large Extent Zone (>64 blocks)
```

#### Allocation Algorithm Selection
- **First Fit**: Fast allocation with limited search -- **the only
  algorithm actually implemented** (`allocate_extent_first_fit()`).
- **Best Fit**: Minimize fragmentation with exhaustive search -- not
  implemented; declared in `kes_allocation_strategy_t` only.
- **Buddy System**: Power-of-2 allocation with coalescing -- not
  implemented.
- **Log-Structured**: Sequential allocation in zones -- not
  implemented.

### 4. Caching Layer Integration

#### Cache-Storage Interface
- **Read Path**: Cache miss triggers extent loading from storage via
  caller-supplied `read_extent` callback (`kes_cache_set_io_callbacks()`).
- **Write Path**: Dirty extents are written back on
  `kes_cache_flush_extent()`/`kes_cache_sync()`/the background flush
  thread (`kes_cache_start()`), via the caller-supplied `write_extent`
  callback -- KES itself does not call into `kes_storage.c`.
- **Consistency**: write-back only. There is no write-through mode.
- **Eviction**: LRU only -- `kes_cache_create()` rejects
  `KES_CACHE_LFU`/`KES_CACHE_CUSTOM` with `NULL` rather than
  implementing them.

---

## Platform Adaptations

### Edge Device Optimizations

#### Memory Constraints
- **Minimal Metadata**: Reduced in-memory structures
- **Lazy Loading**: Load metadata sections on demand
- **Compression**: Compress cached data when memory pressure

#### Power Efficiency  
- **Batched I/O**: Accumulate small writes to reduce wake-ups
- **Idle Detection**: Defer background operations during battery use
- **Thermal Awareness**: Throttle operations during high temperature

#### Flash Longevity (design intent, not implemented)
- **Wear Leveling**: Distribute writes across flash blocks evenly
- **Write Reduction**: Minimize write amplification through batching
- **Bad Block Management**: Handle and remap bad flash blocks

### Server Optimizations

#### Performance Scaling
- **Multi-threading**: Parallel allocation across zones
- **NUMA Awareness**: Allocate memory on correct NUMA nodes
- **Large Pages**: Use huge pages to reduce TLB pressure

#### High Throughput
- **Batch Operations**: Process multiple extents simultaneously
- **Prefetching**: Read-ahead for sequential access patterns
- **Background Processing**: Asynchronous GC and defragmentation

#### Reliability Features
- **Checksums**: Validate data integrity on critical paths
- **Redundancy**: Optional mirroring across devices
- **Recovery**: Fast crash recovery with journaling

### Laptop/Desktop Balance

#### Adaptive Behavior
- **Power State Awareness**: Reduce activity on battery power
- **SSD Optimization**: Enable TRIM and optimize for SSD characteristics
- **Balanced Caching**: Medium cache sizes with adaptive policies

---

## Flash-Aware Design

**Not implemented.** Everything in this section (hot/cold data
classification, log-structured allocation, garbage collection, dynamic
and static wear leveling) is a design sketch -- none of it exists in
`src/*.c`. Grep confirms: no `wear`, `gc_`, or zone-related identifiers
appear anywhere in the actual source. Kept here as forward-looking
design intent, per `plan_phase5.md`'s Track B.2 guidance, not as a
claim of current behavior.

### F2FS-Inspired Architecture

#### Hot/Cold Data Separation
```c
Data Classification:
├── Hot Data
│   ├── Metadata updates
│   ├── Small frequent writes  
│   └── Temporary data
├── Warm Data
│   ├── Regular user files
│   ├── Medium-sized writes
│   └── Moderate update frequency
└── Cold Data
    ├── Archive data
    ├── Large sequential writes
    └── Infrequent updates
```

#### Log-Structured Allocation
- **Sequential Writes**: Always append new data to log
- **Copy-on-Write**: Never update data in place
- **Zone-Based Organization**: Organize storage into writable zones
- **Garbage Collection**: Reclaim space from invalidated data

### Garbage Collection Strategies

#### GC Trigger Policies
```c
GC Trigger Conditions:
├── Urgent GC (>90% full zones)
├── Background GC (>70% full zones)  
├── Idle GC (System idle time)
└── Wear-Aware GC (Uneven wear distribution)
```

#### GC Selection Algorithms
- **Greedy**: Select zones with most invalid blocks
- **Cost-Benefit**: Weight age and invalid block count
- **Wear-Aware**: Consider block wear levels in selection

### Wear Leveling Implementation

#### Dynamic Wear Leveling
- **Write Distribution**: Spread writes across available blocks
- **Hot/Cold Awareness**: Keep hot data in specific zones
- **Wear Monitoring**: Track per-block write/erase cycles

#### Static Wear Leveling
- **Cold Data Migration**: Move rarely-written data from low-wear 
  blocks
- **Wear Balancing**: Actively balance wear across all blocks
- **Threshold-Based**: Trigger when wear variance exceeds threshold

---

## Build System & Profiles

**Not implemented.** The actual build system is a single flat
`Makefile` at the repository root with fixed targets (`make all`,
`make test`, `make debug`, `make asan`/`tsan`/`valgrind`/`check-all`,
etc. -- see `AGENTS.md`'s "Building and Testing" section for the
complete, accurate list) and no `PROFILE=`/`MEMORY_BUDGET=`/`FEATURES=`
variables, no per-hardware profile `.mk` files, and no feature-flag
system. Everything below is a design sketch for a build system that
does not exist.

### Profile-Based Configuration (design intent, not implemented)

#### Hardware Profiles
```makefile
Available Profiles:
├── smartphone      (Android/iOS phones)
├── tablet         (Larger mobile devices)
├── laptop_linux   (Linux laptops with SSD)
├── laptop_macos   (macOS laptops)
├── server_ssd     (Servers with SSD storage)
├── server_nvme    (Servers with NVMe storage)
├── embedded_emmc  (Embedded eMMC devices)
├── embedded_nand  (Raw NAND flash systems)
├── cloud_vm       (Virtual machine storage)
└── custom         (User-defined configuration)
```

#### Profile Configuration Example
```makefile
# Smartphone Profile
ifeq ($(PROFILE),smartphone)
    ENABLE_FEATURES := flash_gc wear_leveling basic_cache
    DISABLE_FEATURES := multi_device encryption async_io
    MEMORY_BUDGET := small
    OPTIMIZATION_TARGET := flash_life
    MAX_THREADS := 1
    CACHE_SIZE_MB := 8
endif
```

#### Feature Selection Granularity
```makefile
Core Features (Always Enabled):
├── storage          (Basic storage operations)
├── bitmap          (Block bitmap management)
└── allocator       (Extent allocation)

Optional Features:
├── flash_gc        (Flash garbage collection)
├── wear_leveling   (Wear leveling algorithms)
├── compression     (Data compression)
├── encryption      (Data encryption)
├── async_io        (Asynchronous I/O)
├── multi_device    (Multi-device support)
├── advanced_cache  (Advanced caching algorithms)
└── debug_tools     (Debugging and profiling)
```

### Build-Time Optimizations

#### Memory Budget Control
```makefile
Memory Budget Levels:
├── tiny     (4MB cache, 1MB bitmap, 1 thread)
├── small    (16MB cache, 4MB bitmap, 2 threads)  
├── medium   (64MB cache, 16MB bitmap, 4 threads)
├── large    (256MB cache, 64MB bitmap, 8 threads)
└── unlimited (No hard limits)
```

#### Cross-Platform Support
```makefile
Platform Detection:
├── Auto-detection based on uname and system characteristics
├── Manual override with PLATFORM variable
├── Cross-compilation support with CROSS_COMPILE prefix
└── Sysroot support for embedded systems
```

---

## Performance Characteristics

**Not measured.** The figures below are illustrative design targets
for strategies that (aside from First Fit) are not implemented -- they
are not benchmark results. `KES_HARDENING_PLAN.md` §6.G (performance
smoke tests) and the "Perf smoke" item in `plan_phase5.md` Track A.7
are the place to add real, reproducible numbers for the strategies
that actually exist.

### Allocation Performance

#### Strategy Performance Matrix (design targets, not benchmarked)
```
Strategy        | Allocation | Fragmentation | Memory    | Flash
              | Speed      | Resistance    | Overhead  | Friendly
              |------------|---------------|-----------|----------
First Fit      | Excellent  | Poor         | Low       | Fair
Best Fit       | Poor       | Excellent    | Low       | Fair  
Buddy System   | Good       | Good         | Medium    | Fair
Log-Structured | Good       | Fair         | Medium    | Excellent
Hybrid         | Good       | Good         | Medium    | Good
```

#### Scalability Characteristics
- **Small Extents** (<8 blocks): Optimized for metadata and small files
- **Medium Extents** (8-64 blocks): Balanced allocation performance
- **Large Extents** (>64 blocks): Sequential allocation preferred

### Memory Usage Optimization

#### Component Memory Footprint
```c
Typical Memory Usage (64GB storage):
├── Storage Descriptor: 1KB
├── Bitmap (full memory): 2MB  
├── Cache Metadata: 16MB
├── Allocation Structures: 4MB
├── Zone Metadata: 1MB
└── Total Overhead: ~23MB (0.04% of storage)
```

#### Memory Pressure Handling
- **Graceful Degradation**: Disable features under memory pressure
- **Adaptive Algorithms**: Switch to memory-efficient algorithms
- **Emergency Eviction**: Force cache eviction when needed

### Flash Performance Optimization (design intent, not implemented)

#### Write Amplification Reduction
- **Log-Structured Writes**: Minimize random writes
- **Batch Operations**: Group small writes together
- **GC Efficiency**: Optimize garbage collection overhead

#### Read Performance
- **Zone Locality**: Keep related data in same zones
- **Prefetching**: Sequential read-ahead optimization
- **Cache Efficiency**: Smart caching for flash characteristics

---

## Integration Guidelines

### Upper Layer Integration

#### Storage Lifecycle Integration

There is no `kes_profile_load()` or hardware-profile system --
`kes_storage_config_t` is populated directly by the caller:

```c
kes_storage_config_t config = {
    .device_path = "/dev/storage",
    .device_size = 1024ULL * 1024 * 1024,
    .block_size = KES_DEFAULT_BLOCK_SIZE,
    .flags = KES_STORAGE_CREATE,
    .strategy = KES_ALLOC_FIRST_FIT  /* the only strategy actually used */
};

kes_storage_t *storage;
int result = kes_storage_create(&config, &storage);

kes_extent_descriptor_t extent;
kes_extent_allocate(storage, &request, &extent);
```

#### Descriptor Management Integration

**Not implemented.** There is no external/pluggable descriptor
management -- `kes_descriptor_management_t` does not exist.
`kes_storage_descriptor_t` is always stored in block 0 of the backing
file and read/written internally by `kes_storage_open()`/
`kes_storage_sync()`/`kes_storage_close()`; there is no save/load
callback hook.

### File System Integration

#### VFS Integration Points
- **Mount Interface**: Integration with mount/umount operations
- **Block Device Interface**: Standard block device operations
- **FUSE Integration**: User-space file system support

#### Cache Coordination  
- **Page Cache Integration**: Coordinate with kernel page cache
- **Buffer Cache**: Integration with block buffer cache
- **Memory Pressure**: Respond to kernel memory pressure

### Database Integration

#### Transaction Support
- **Atomic Operations**: Extent operations within transactions
- **Consistency**: ACID compliance for metadata operations
- **Recovery**: Crash recovery with write-ahead logging

---

## Future Roadmap

### Version 1.1 Features
- **Compression**: LZ4 compression for cold data
- **Checksums**: Optional data integrity validation
- **Metrics**: Enhanced performance monitoring
- **Multi-Device**: Native multi-device support

### Version 1.2 Features  
- **Encryption**: AES encryption for sensitive data
- **Distributed Storage**: Network-attached storage support
- **Machine Learning**: ML-based access pattern prediction
- **Advanced GC**: Improved garbage collection algorithms

### Long-Term Vision
- **Tiered Storage**: Automatic hot/cold data migration
- **Cloud Integration**: Native cloud storage backends
- **Hardware Acceleration**: GPU/FPGA acceleration support
- **Formal Verification**: Mathematical correctness proofs

---

## Conclusion

KES provides a comprehensive foundation for modern storage systems 
with platform-aware optimizations and flexible deployment options. 
The build-time profile system ensures optimal resource usage across 
diverse hardware platforms while maintaining a unified development 
experience.

The flash-aware design principles ensure longevity of flash-based 
storage devices, making KES suitable for both resource-constrained 
edge devices and high-performance server deployments.

---

**Document Version**: 1.0  
**Last Updated**: December 2025  
**Next Review**: Implementation completion
