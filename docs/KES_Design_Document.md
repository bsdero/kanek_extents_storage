# KANEK Extents Storage (KES) Design Document
## Version 1.0

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
- **Location**: Last blocks (default) or external management  
- **Purpose**: Track free/used blocks with efficient allocation
- **Strategies**: Full memory, sliding window, on-demand loading

#### 4. User Data Area
- **Location**: Between metadata and bitmap
- **Organization**: Multiple allocation zones with different strategies
- **Flash Zones**: Hot/warm/cold data separation for wear leveling

### Multi-Strategy Allocation Engine

KES implements multiple allocation strategies selectable at build-time 
or configurable per storage instance:

```c
Allocation Strategies:
├── First Fit (Speed Optimized)
├── Best Fit (Fragmentation Optimized) 
├── Buddy System (Memory Efficient)
├── Slab Allocator (Fixed-Size Optimized)
├── Log-Structured (Flash Optimized)
└── Hybrid (Adaptive Selection)
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

#### Multi-Zone Architecture
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
- **First Fit**: Fast allocation with limited search
- **Best Fit**: Minimize fragmentation with exhaustive search
- **Buddy System**: Power-of-2 allocation with coalescing
- **Log-Structured**: Sequential allocation in zones

### 4. Caching Layer Integration

#### Cache-Storage Interface
- **Read Path**: Cache miss triggers extent loading from storage
- **Write Path**: Cache dirty extents written back to storage
- **Consistency**: Write-through or write-back policies
- **Eviction**: LRU/LFU eviction coordinated with storage

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

#### Flash Longevity
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

### Profile-Based Configuration

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

### Allocation Performance

#### Strategy Performance Matrix
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

### Flash Performance Optimization

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
```c
// Typical integration pattern
kes_storage_config_t config;
kes_profile_load(KES_PROFILE_BUILD_DEFAULT, &config);
config.device_path = "/dev/storage";

kes_storage_t* storage;
int result = kes_storage_create(&config, &storage);

// Use storage for extent operations
kes_extent_descriptor_t extent;
kes_extent_allocate(storage, &request, &extent);
```

#### Descriptor Management Integration
```c
// External descriptor management
kes_descriptor_management_t desc_mgmt = {
    .use_external = true,
    .external_managed = {
        .buffer = my_descriptor_buffer,
        .buffer_size = sizeof(my_descriptor_buffer),
        .save_callback = save_to_database,
        .load_callback = load_from_database,
        .user_data = database_handle
    }
};
```

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
