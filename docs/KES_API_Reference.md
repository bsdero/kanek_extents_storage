# KANEK Extents Storage (KES) API Reference Manual
## Version 1.0

---

## Table of Contents

1. [API Overview](#api-overview)
2. [Core Storage API](#core-storage-api)
3. [Extent Management API](#extent-management-api)
4. [Bitmap Management API](#bitmap-management-api)
5. [Flash-Specific API](#flash-specific-api)
6. [Zone Management API](#zone-management-api)
7. [Configuration & Profiles API](#configuration--profiles-api)
8. [Serialization API](#serialization-api)
9. [Utility Functions](#utility-functions)
10. [Error Handling](#error-handling)
11. [Usage Examples](#usage-examples)
12. [Integration Patterns](#integration-patterns)

---

## API Overview

### Design Principles

The KES API follows these design principles:

1. **Consistent Error Handling**: All functions return integer error 
   codes with output parameters
2. **Thread Safety**: All APIs are thread-safe by default
3. **Resource Management**: Clear ownership and lifecycle management
4. **Platform Agnostic**: Same API across all supported platforms
5. **Minimal Dependencies**: Standard C library dependencies only

### Header Files

```c
#include <kes/kes_storage.h>     // Core storage API
#include <kes/kes_cache.h>       // Caching layer API  
#include <kes/kes_bitmap.h>      // Bitmap management API
#include <kes/kes_flash.h>       // Flash-specific API
```

### Return Codes

```c
#define KES_SUCCESS              0    // Operation successful
#define KES_ERROR_INVALID       -1    // Invalid parameters
#define KES_ERROR_NOMEM         -2    // Out of memory
#define KES_ERROR_NOTFOUND      -3    // Resource not found
#define KES_ERROR_EXISTS        -4    // Resource already exists
#define KES_ERROR_IO            -5    // I/O error
#define KES_ERROR_NOSPACE       -6    // No space available
#define KES_ERROR_CORRUPT       -7    // Data corruption detected
#define KES_ERROR_VERSION       -8    // Version mismatch
#define KES_ERROR_BUSY          -9    // Resource busy
#define KES_ERROR_TIMEOUT      -10    // Operation timeout
```

---

## Core Storage API

### Storage Lifecycle Management

#### kes_storage_create

Create a new KES storage instance.

```c
int kes_storage_create(const kes_storage_config_t* config,
                       kes_storage_t** storage);
```

**Parameters:**
- `config`: Storage configuration structure
- `storage`: Output parameter for created storage handle

**Returns:** `KES_SUCCESS` on success, error code on failure

**Example:**
```c
kes_storage_config_t config;
kes_profile_load(KES_PROFILE_BUILD_DEFAULT, &config);
config.device_path = "/dev/sdb1";
config.device_size = 1024 * 1024 * 1024;  // 1GB

kes_storage_t* storage;
int result = kes_storage_create(&config, &storage);
if (result != KES_SUCCESS) {
    // Handle error
}
```

#### kes_storage_open

Open an existing KES storage instance.

```c
int kes_storage_open(const char* device_path,
                     uint32_t flags,
                     kes_storage_t** storage);
```

**Parameters:**
- `device_path`: Path to storage device or file
- `flags`: Open flags (readonly, sync, etc.)
- `storage`: Output parameter for opened storage handle

**Returns:** `KES_SUCCESS` on success, error code on failure

**Example:**
```c
kes_storage_t* storage;
int result = kes_storage_open("/dev/sdb1", 
                             KES_STORAGE_READONLY, 
                             &storage);
```

#### kes_storage_close

Close and cleanup storage instance.

```c
int kes_storage_close(kes_storage_t* storage);
```

**Parameters:**
- `storage`: Storage handle to close

**Returns:** `KES_SUCCESS` on success, error code on failure

#### kes_storage_sync

Synchronize all pending changes to storage.

```c
int kes_storage_sync(kes_storage_t* storage);
```

**Parameters:**
- `storage`: Storage handle to sync

**Returns:** `KES_SUCCESS` on success, error code on failure

### Storage Information

#### kes_storage_get_info

Get storage information and statistics.

```c
int kes_storage_get_info(kes_storage_t* storage,
                         kes_storage_info_t* info);
```

**Parameters:**
- `storage`: Storage handle
- `info`: Output parameter for storage information

**Returns:** `KES_SUCCESS` on success, error code on failure

```c
typedef struct {
    uint64_t total_blocks;          // Total blocks in storage
    uint64_t free_blocks;           // Available free blocks
    uint64_t used_blocks;           // Currently used blocks
    uint32_t block_size;            // Block size in bytes
    uint64_t total_extents;         // Number of allocated extents
    float fragmentation_ratio;      // Fragmentation percentage
    
    // Performance statistics
    uint64_t reads_completed;       // Read operations completed
    uint64_t writes_completed;      // Write operations completed
    uint64_t bytes_read;            // Total bytes read
    uint64_t bytes_written;         // Total bytes written
    uint64_t allocation_count;      // Extent allocations performed
    uint64_t deallocation_count;    // Extent deallocations performed
} kes_storage_info_t;
```

#### kes_storage_get_descriptor

Get storage descriptor for external management.

```c
int kes_storage_get_descriptor(kes_storage_t* storage,
                               kes_storage_descriptor_t* descriptor);
```

**Parameters:**
- `storage`: Storage handle
- `descriptor`: Output parameter for storage descriptor

**Returns:** `KES_SUCCESS` on success, error code on failure

---

## Extent Management API

### Extent Allocation

#### kes_extent_allocate

Allocate a new extent with specified requirements.

```c
int kes_extent_allocate(kes_storage_t* storage,
                        const kes_extent_request_t* request,
                        kes_extent_descriptor_t* extent);
```

**Parameters:**
- `storage`: Storage handle
- `request`: Allocation request specification
- `extent`: Output parameter for allocated extent

**Returns:** `KES_SUCCESS` on success, error code on failure

```c
typedef struct {
    uint32_t block_count;           // Requested number of blocks
    uint32_t alignment;             // Block alignment requirement (0=any)
    uint64_t hint_block;            // Preferred starting block
    uint32_t flags;                 // Allocation flags
    
    // Advanced allocation options
    enum {
        KES_ALLOC_PREFER_LOW,       // Prefer low block addresses
        KES_ALLOC_PREFER_HIGH,      // Prefer high block addresses
        KES_ALLOC_PREFER_SEQUENTIAL,// Prefer sequential allocation
        KES_ALLOC_PREFER_RANDOM     // Allow random placement
    } placement_hint;
    
    uint32_t zone_preference;       // Preferred allocation zone
} kes_extent_request_t;

typedef struct {
    uint64_t start_block;           // Starting block address
    uint32_t block_count;           // Number of contiguous blocks
    uint32_t flags;                 // Extent flags
    uint64_t extent_id;             // Unique extent identifier
    uint64_t allocation_time;       // When extent was allocated
    uint32_t zone_id;               // Zone where extent resides
} kes_extent_descriptor_t;
```

**Example:**
```c
kes_extent_request_t request = {
    .block_count = 16,              // Request 16 blocks (128KB)
    .alignment = 8,                 // 8-block alignment
    .hint_block = 1000,             // Prefer around block 1000
    .flags = 0,
    .placement_hint = KES_ALLOC_PREFER_SEQUENTIAL
};

kes_extent_descriptor_t extent;
int result = kes_extent_allocate(storage, &request, &extent);
if (result == KES_SUCCESS) {
    printf("Allocated extent at block %lu, %u blocks\n",
           extent.start_block, extent.block_count);
}
```

#### kes_extent_free

Free a previously allocated extent.

```c
int kes_extent_free(kes_storage_t* storage,
                    const kes_extent_descriptor_t* extent);
```

**Parameters:**
- `storage`: Storage handle
- `extent`: Extent descriptor to free

**Returns:** `KES_SUCCESS` on success, error code on failure

### Extent I/O Operations

#### kes_extent_read

Read data from an extent.

```c
int kes_extent_read(kes_storage_t* storage,
                    const kes_extent_descriptor_t* extent,
                    void* buffer,
                    size_t size,
                    uint64_t offset);
```

**Parameters:**
- `storage`: Storage handle
- `extent`: Extent to read from
- `buffer`: Buffer to read data into
- `size`: Number of bytes to read
- `offset`: Byte offset within extent

**Returns:** `KES_SUCCESS` on success, error code on failure

#### kes_extent_write

Write data to an extent.

```c
int kes_extent_write(kes_storage_t* storage,
                     const kes_extent_descriptor_t* extent,
                     const void* buffer,
                     size_t size,
                     uint64_t offset);
```

**Parameters:**
- `storage`: Storage handle
- `extent`: Extent to write to
- `buffer`: Buffer containing data to write
- `size`: Number of bytes to write
- `offset`: Byte offset within extent

**Returns:** `KES_SUCCESS` on success, error code on failure

#### kes_extent_sync

Synchronize extent data to storage.

```c
int kes_extent_sync(kes_storage_t* storage,
                    const kes_extent_descriptor_t* extent);
```

**Parameters:**
- `storage`: Storage handle
- `extent`: Extent to synchronize

**Returns:** `KES_SUCCESS` on success, error code on failure

### Extent Query Operations

#### kes_extent_lookup

Look up extent by identifier.

```c
int kes_extent_lookup(kes_storage_t* storage,
                      uint64_t extent_id,
                      kes_extent_descriptor_t* extent);
```

**Parameters:**
- `storage`: Storage handle
- `extent_id`: Extent identifier to look up
- `extent`: Output parameter for extent descriptor

**Returns:** `KES_SUCCESS` on success, `KES_ERROR_NOTFOUND` if not found

#### kes_extent_enumerate

Enumerate all allocated extents.

```c
int kes_extent_enumerate(kes_storage_t* storage,
                         kes_extent_iterator_t* iterator);
```

**Parameters:**
- `storage`: Storage handle
- `iterator`: Iterator for extent enumeration

**Returns:** `KES_SUCCESS` on success, error code on failure

```c
typedef struct kes_extent_iterator kes_extent_iterator_t;

int kes_extent_iterator_next(kes_extent_iterator_t* iter,
                             kes_extent_descriptor_t* extent);
void kes_extent_iterator_destroy(kes_extent_iterator_t* iter);
```

---

## Bitmap Management API

### Bitmap Operations

#### kes_bitmap_create

Create a new bitmap for block management.

```c
int kes_bitmap_create(uint64_t total_blocks,
                      kes_bitmap_t** bitmap);
```

**Parameters:**
- `total_blocks`: Total number of blocks to represent
- `bitmap`: Output parameter for created bitmap

**Returns:** `KES_SUCCESS` on success, error code on failure

#### kes_bitmap_destroy

Destroy bitmap and free resources.

```c
void kes_bitmap_destroy(kes_bitmap_t* bitmap);
```

**Parameters:**
- `bitmap`: Bitmap to destroy

#### kes_bitmap_set

Mark a block as used in the bitmap.

```c
int kes_bitmap_set(kes_bitmap_t* bitmap, uint64_t block_number);
```

**Parameters:**
- `bitmap`: Target bitmap
- `block_number`: Block number to mark as used

**Returns:** `KES_SUCCESS` on success, error code on failure

#### kes_bitmap_clear

Mark a block as free in the bitmap.

```c
int kes_bitmap_clear(kes_bitmap_t* bitmap, uint64_t block_number);
```

**Parameters:**
- `bitmap`: Target bitmap
- `block_number`: Block number to mark as free

**Returns:** `KES_SUCCESS` on success, error code on failure

#### kes_bitmap_test

Test if a block is marked as used.

```c
bool kes_bitmap_test(kes_bitmap_t* bitmap, uint64_t block_number);
```

**Parameters:**
- `bitmap`: Target bitmap
- `block_number`: Block number to test

**Returns:** `true` if block is used, `false` if free

#### kes_bitmap_find_free

Find contiguous free blocks in bitmap.

```c
int kes_bitmap_find_free(kes_bitmap_t* bitmap,
                         uint32_t block_count,
                         uint64_t start_hint,
                         uint64_t* start_block);
```

**Parameters:**
- `bitmap`: Target bitmap
- `block_count`: Number of contiguous blocks needed
- `start_hint`: Suggested starting point for search
- `start_block`: Output parameter for found starting block

**Returns:** `KES_SUCCESS` on success, `KES_ERROR_NOSPACE` if no space

#### kes_bitmap_set_range

Mark a range of blocks as used.

```c
int kes_bitmap_set_range(kes_bitmap_t* bitmap,
                         uint64_t start_block,
                         uint32_t block_count);
```

**Parameters:**
- `bitmap`: Target bitmap
- `start_block`: Starting block number
- `block_count`: Number of blocks to mark

**Returns:** `KES_SUCCESS` on success, error code on failure

#### kes_bitmap_clear_range

Mark a range of blocks as free.

```c
int kes_bitmap_clear_range(kes_bitmap_t* bitmap,
                           uint64_t start_block,
                           uint32_t block_count);
```

**Parameters:**
- `bitmap`: Target bitmap
- `start_block`: Starting block number
- `block_count`: Number of blocks to clear

**Returns:** `KES_SUCCESS` on success, error code on failure

---

## Flash-Specific API

### Flash Geometry Detection

#### kes_flash_detect_geometry

Auto-detect flash device geometry.

```c
int kes_flash_detect_geometry(const char* device_path,
                              kes_flash_geometry_t* geometry);
```

**Parameters:**
- `device_path`: Path to flash device
- `geometry`: Output parameter for detected geometry

**Returns:** `KES_SUCCESS` on success, error code on failure

```c
typedef struct {
    uint32_t page_size;             // Flash page size in bytes
    uint32_t pages_per_block;       // Pages per erase block
    uint32_t block_count;           // Total erase blocks
    uint32_t plane_count;           // Number of planes/die
    
    // Performance characteristics
    uint32_t page_read_time_us;     // Page read latency
    uint32_t page_program_time_us;  // Page program latency
    uint32_t block_erase_time_ms;   // Block erase latency
    
    // Controller capabilities
    bool supports_trim;             // TRIM/DISCARD support
    bool supports_barriers;         // Write barrier support
    bool supports_ncq;              // Native command queuing
} kes_flash_geometry_t;
```

### Garbage Collection

#### kes_gc_trigger

Trigger garbage collection operation.

```c
int kes_gc_trigger(kes_storage_t* storage,
                   const kes_gc_config_t* gc_config);
```

**Parameters:**
- `storage`: Storage handle
- `gc_config`: Garbage collection configuration

**Returns:** `KES_SUCCESS` on success, error code on failure

```c
typedef struct {
    enum {
        KES_GC_GREEDY,              // Most invalid blocks first
        KES_GC_COST_BENEFIT,       // Age and invalid count weighted
        KES_GC_WEAR_AWARE           // Consider wear levels
    } gc_policy;
    
    uint32_t target_free_blocks;    // Target number of free blocks
    uint32_t max_gc_time_ms;        // Maximum GC operation time
    bool background_mode;           // Run in background thread
    
    // Progress callback
    void (*progress_callback)(uint32_t percent_complete, void* user_data);
    void* user_data;
} kes_gc_config_t;
```

#### kes_gc_get_status

Get garbage collection status and statistics.

```c
int kes_gc_get_status(kes_storage_t* storage,
                      kes_gc_status_t* status);
```

**Parameters:**
- `storage`: Storage handle
- `status`: Output parameter for GC status

**Returns:** `KES_SUCCESS` on success, error code on failure

```c
typedef struct {
    bool gc_running;                // GC currently running
    uint32_t gc_progress_percent;   // Current GC progress
    uint64_t total_gc_operations;   // Total GC operations performed
    uint64_t blocks_reclaimed;      // Total blocks reclaimed by GC
    uint64_t data_copied_bytes;     // Total data copied during GC
    float gc_efficiency;            // Average GC efficiency
} kes_gc_status_t;
```

### Wear Leveling

#### kes_wear_level_get_status

Get wear leveling status and statistics.

```c
int kes_wear_level_get_status(kes_storage_t* storage,
                              kes_wear_level_status_t* status);
```

**Parameters:**
- `storage`: Storage handle
- `status`: Output parameter for wear leveling status

**Returns:** `KES_SUCCESS` on success, error code on failure

```c
typedef struct {
    uint32_t max_wear_count;        // Most worn block count
    uint32_t min_wear_count;        // Least worn block count
    uint32_t average_wear_count;    // Average wear count
    float wear_variance;            // Wear distribution variance
    uint64_t wear_level_operations; // Total wear leveling operations
    uint64_t data_migrated_bytes;   // Data migrated for wear leveling
} kes_wear_level_status_t;
```

#### kes_wear_level_force_balance

Force wear leveling operation.

```c
int kes_wear_level_force_balance(kes_storage_t* storage);
```

**Parameters:**
- `storage`: Storage handle

**Returns:** `KES_SUCCESS` on success, error code on failure

---

## Zone Management API

### Zone Operations

#### kes_zone_create

Create a new allocation zone.

```c
int kes_zone_create(kes_storage_t* storage,
                    const kes_zone_config_t* config,
                    uint32_t* zone_id);
```

**Parameters:**
- `storage`: Storage handle
- `config`: Zone configuration
- `zone_id`: Output parameter for created zone ID

**Returns:** `KES_SUCCESS` on success, error code on failure

```c
typedef struct {
    uint64_t start_block;           // Zone starting block
    uint32_t block_count;           // Zone size in blocks
    
    enum {
        KES_ZONE_HOT,               // Frequently updated data
        KES_ZONE_WARM,              // Regular data
        KES_ZONE_COLD,              // Archive data
        KES_ZONE_MIXED              // Mixed data types
    } zone_type;
    
    kes_allocation_strategy_t allocation_strategy;
    uint32_t alignment_requirement; // Block alignment within zone
    uint32_t max_extents;           // Maximum extents in zone
} kes_zone_config_t;
```

#### kes_zone_destroy

Destroy an allocation zone.

```c
int kes_zone_destroy(kes_storage_t* storage, uint32_t zone_id);
```

**Parameters:**
- `storage`: Storage handle
- `zone_id`: Zone ID to destroy

**Returns:** `KES_SUCCESS` on success, error code on failure

#### kes_zone_resize

Resize an existing zone.

```c
int kes_zone_resize(kes_storage_t* storage,
                    const kes_zone_resize_request_t* request);
```

**Parameters:**
- `storage`: Storage handle
- `request`: Zone resize request

**Returns:** `KES_SUCCESS` on success, error code on failure

```c
typedef struct {
    uint32_t zone_id;               // Zone to resize
    
    enum {
        KES_ZONE_RESIZE_GROW,       // Increase zone size
        KES_ZONE_RESIZE_SHRINK,     // Decrease zone size
        KES_ZONE_RESIZE_MOVE        // Move zone to different location
    } resize_type;
    
    union {
        uint32_t additional_blocks; // For grow operation
        uint32_t blocks_to_remove;  // For shrink operation
        uint64_t new_start_block;   // For move operation
    } params;
    
    bool preserve_data;             // Preserve existing data
    
    // Progress callback for long operations
    void (*progress_callback)(uint32_t percent, void* user_data);
    void* user_data;
} kes_zone_resize_request_t;
```

#### kes_zone_get_info

Get information about a zone.

```c
int kes_zone_get_info(kes_storage_t* storage,
                      uint32_t zone_id,
                      kes_zone_info_t* info);
```

**Parameters:**
- `storage`: Storage handle
- `zone_id`: Zone ID to query
- `info`: Output parameter for zone information

**Returns:** `KES_SUCCESS` on success, error code on failure

```c
typedef struct {
    uint32_t zone_id;               // Zone identifier
    uint64_t start_block;           // Zone starting block
    uint32_t total_blocks;          // Total blocks in zone
    uint32_t free_blocks;           // Free blocks in zone
    uint32_t used_blocks;           // Used blocks in zone
    uint32_t allocated_extents;     // Number of extents in zone
    float fragmentation_ratio;      // Zone fragmentation
    
    // Zone statistics
    uint64_t allocations_performed; // Total allocations in zone
    uint64_t deallocations_performed; // Total deallocations in zone
    uint64_t bytes_allocated;       // Total bytes allocated
    uint64_t gc_operations;         // GC operations in this zone
} kes_zone_info_t;
```

#### kes_zone_enumerate

Enumerate all zones in storage.

```c
int kes_zone_enumerate(kes_storage_t* storage,
                       kes_zone_iterator_t* iterator);
```

**Parameters:**
- `storage`: Storage handle
- `iterator`: Output parameter for zone iterator

**Returns:** `KES_SUCCESS` on success, error code on failure

```c
typedef struct kes_zone_iterator kes_zone_iterator_t;

int kes_zone_iterator_next(kes_zone_iterator_t* iter,
                           kes_zone_info_t* zone_info);
void kes_zone_iterator_destroy(kes_zone_iterator_t* iter);
```

---

## Configuration & Profiles API

### Profile Management

#### kes_profile_load

Load a predefined hardware profile.

```c
int kes_profile_load(kes_hardware_profile_t profile,
                     kes_storage_config_t* config);
```

**Parameters:**
- `profile`: Hardware profile to load
- `config`: Output parameter for loaded configuration

**Returns:** `KES_SUCCESS` on success, error code on failure

```c
typedef enum {
    KES_PROFILE_BUILD_DEFAULT,      // Build-time default profile
    KES_PROFILE_SMARTPHONE,         // Mobile device profile
    KES_PROFILE_TABLET,             // Tablet device profile
    KES_PROFILE_LAPTOP_LINUX,       // Linux laptop profile
    KES_PROFILE_SERVER_SSD,         // Server SSD profile
    KES_PROFILE_SERVER_NVME,        // Server NVMe profile
    KES_PROFILE_EMBEDDED_EMMC,      // Embedded eMMC profile
    KES_PROFILE_CLOUD_VM,           // Cloud VM profile
    KES_PROFILE_CUSTOM              // Custom profile
} kes_hardware_profile_t;
```

#### kes_profile_auto_detect

Auto-detect appropriate profile for current system.

```c
int kes_profile_auto_detect(kes_storage_config_t* config);
```

**Parameters:**
- `config`: Output parameter for auto-detected configuration

**Returns:** `KES_SUCCESS` on success, error code on failure

#### kes_config_validate

Validate storage configuration.

```c
int kes_config_validate(const kes_storage_config_t* config,
                        kes_config_validation_result_t* result);
```

**Parameters:**
- `config`: Configuration to validate
- `result`: Output parameter for validation result

**Returns:** `KES_SUCCESS` if valid, error code if invalid

```c
typedef struct {
    bool is_valid;                  // Configuration is valid
    char error_messages[1024];      // Detailed error descriptions
    char warnings[1024];            // Configuration warnings
    
    // Performance predictions
    struct {
        float allocation_speed_score;    // 0.0-1.0
        float memory_efficiency_score;   // 0.0-1.0
        float flash_longevity_score;     // 0.0-1.0
    } predicted_performance;
} kes_config_validation_result_t;
```

### Runtime Configuration

#### kes_storage_tune

Tune storage parameters at runtime.

```c
int kes_storage_tune(kes_storage_t* storage,
                     const char* parameter_name,
                     const char* parameter_value);
```

**Parameters:**
- `storage`: Storage handle
- `parameter_name`: Name of parameter to tune
- `parameter_value`: New parameter value (string format)

**Returns:** `KES_SUCCESS` on success, error code on failure

**Tunable Parameters:**
- `"gc_threshold"`: Garbage collection trigger threshold
- `"cache_size"`: Cache size in MB
- `"sync_interval"`: Sync interval in milliseconds
- `"allocation_strategy"`: Allocation strategy name
- `"wear_threshold"`: Wear leveling trigger threshold

**Example:**
```c
// Adjust GC threshold to 80%
kes_storage_tune(storage, "gc_threshold", "80");

// Change allocation strategy to best-fit
kes_storage_tune(storage, "allocation_strategy", "best_fit");
```

---

## Serialization API

### Descriptor Serialization

#### kes_serialize_descriptor

Serialize storage descriptor to buffer.

```c
int kes_serialize_descriptor(const kes_storage_descriptor_t* descriptor,
                             kes_serialize_format_t format,
                             void* buffer,
                             size_t buffer_size,
                             size_t* actual_size);
```

**Parameters:**
- `descriptor`: Storage descriptor to serialize
- `format`: Serialization format (binary/JSON)
- `buffer`: Output buffer for serialized data
- `buffer_size`: Size of output buffer
- `actual_size`: Output parameter for actual serialized size

**Returns:** `KES_SUCCESS` on success, error code on failure

```c
typedef enum {
    KES_SERIALIZE_BINARY,           // Compact binary format
    KES_SERIALIZE_JSON              // Human-readable JSON format
} kes_serialize_format_t;
```

#### kes_deserialize_descriptor

Deserialize storage descriptor from buffer.

```c
int kes_deserialize_descriptor(kes_storage_descriptor_t* descriptor,
                               kes_serialize_format_t format,
                               const void* buffer,
                               size_t buffer_size);
```

**Parameters:**
- `descriptor`: Output parameter for deserialized descriptor
- `format`: Serialization format of input data
- `buffer`: Input buffer containing serialized data
- `buffer_size`: Size of input buffer

**Returns:** `KES_SUCCESS` on success, error code on failure

#### kes_serialize_get_size

Get required buffer size for serialization.

```c
size_t kes_serialize_get_size(const kes_storage_descriptor_t* descriptor,
                              kes_serialize_format_t format);
```

**Parameters:**
- `descriptor`: Storage descriptor to measure
- `format`: Target serialization format

**Returns:** Required buffer size in bytes

#### kes_serialize_validate

Validate serialized descriptor data.

```c
int kes_serialize_validate(const void* buffer,
                           size_t buffer_size,
                           kes_serialize_format_t format);
```

**Parameters:**
- `buffer`: Buffer containing serialized data
- `buffer_size`: Size of input buffer
- `format`: Expected serialization format

**Returns:** `KES_SUCCESS` if valid, error code if invalid

---

## Utility Functions

### General Utilities

#### kes_get_version

Get KES library version information.

```c
void kes_get_version(uint16_t* major, uint16_t* minor, uint16_t* patch);
```

**Parameters:**
- `major`: Output parameter for major version
- `minor`: Output parameter for minor version
- `patch`: Output parameter for patch version

#### kes_get_build_info

Get build-time configuration information.

```c
void kes_get_build_info(kes_build_info_t* build_info);
```

**Parameters:**
- `build_info`: Output parameter for build information

```c
typedef struct {
    char profile_name[64];          // Build profile name
    char enabled_features[256];     // Comma-separated feature list
    char build_timestamp[32];       // Build timestamp
    char compiler_version[64];      // Compiler version used
    uint32_t memory_budget_mb;      // Build-time memory budget
} kes_build_info_t;
```

#### kes_calculate_blocks_needed

Calculate blocks needed for given byte size.

```c
uint32_t kes_calculate_blocks_needed(size_t byte_size, 
                                     uint32_t block_size);
```

**Parameters:**
- `byte_size`: Size in bytes
- `block_size`: Block size in bytes

**Returns:** Number of blocks needed

#### kes_calculate_extent_size

Calculate total size of extent in bytes.

```c
size_t kes_calculate_extent_size(const kes_extent_descriptor_t* extent);
```

**Parameters:**
- `extent`: Extent descriptor

**Returns:** Extent size in bytes

---

## Error Handling

### Error Code Handling

All KES functions return integer error codes. Success is indicated by 
`KES_SUCCESS` (0), and errors are negative values.

```c
int result = kes_storage_create(&config, &storage);
if (result != KES_SUCCESS) {
    // Handle error
    switch (result) {
        case KES_ERROR_INVALID:
            printf("Invalid configuration parameters\n");
            break;
        case KES_ERROR_NOMEM:
            printf("Out of memory\n");
            break;
        case KES_ERROR_IO:
            printf("I/O error accessing storage device\n");
            break;
        default:
            printf("Unknown error: %d\n", result);
            break;
    }
}
```

### Error Information

#### kes_get_error_string

Get human-readable error description.

```c
const char* kes_get_error_string(int error_code);
```

**Parameters:**
- `error_code`: Error code to describe

**Returns:** String description of error

**Example:**
```c
int result = kes_extent_allocate(storage, &request, &extent);
if (result != KES_SUCCESS) {
    printf("Allocation failed: %s\n", kes_get_error_string(result));
}
```

### Error Context

#### kes_get_last_error

Get detailed information about last error.

```c
int kes_get_last_error(kes_storage_t* storage,
                       kes_error_info_t* error_info);
```

**Parameters:**
- `storage`: Storage handle (or NULL for global errors)
- `error_info`: Output parameter for error information

**Returns:** `KES_SUCCESS` if error info available

```c
typedef struct {
    int error_code;                 // Original error code
    char error_message[256];        // Detailed error message
    char function_name[64];         // Function where error occurred
    uint32_t line_number;           // Line number where error occurred
    uint64_t timestamp;             // When error occurred
    
    // Context information
    union {
        struct {
            uint64_t block_number;  // For block-related errors
            uint32_t block_count;
        } block_error;
        
        struct {
            uint64_t extent_id;     // For extent-related errors
            uint64_t start_block;
        } extent_error;
        
        struct {
            const char* device_path; // For device-related errors
            int system_errno;
        } io_error;
    } context;
} kes_error_info_t;
```

---

## Usage Examples

### Basic Storage Operations

```c
#include <kes/kes_storage.h>

int main() {
    // Load default profile configuration
    kes_storage_config_t config;
    kes_profile_load(KES_PROFILE_BUILD_DEFAULT, &config);
    
    // Customize configuration
    config.device_path = "/dev/sdb1";
    config.device_size = 1024 * 1024 * 1024;  // 1GB
    
    // Create storage
    kes_storage_t* storage;
    int result = kes_storage_create(&config, &storage);
    if (result != KES_SUCCESS) {
        printf("Failed to create storage: %s\n", 
               kes_get_error_string(result));
        return 1;
    }
    
    // Allocate an extent
    kes_extent_request_t request = {
        .block_count = 16,          // 128KB extent
        .alignment = 0,             // No special alignment
        .hint_block = 0,            // No location preference
        .flags = 0
    };
    
    kes_extent_descriptor_t extent;
    result = kes_extent_allocate(storage, &request, &extent);
    if (result != KES_SUCCESS) {
        printf("Failed to allocate extent: %s\n",
               kes_get_error_string(result));
        kes_storage_close(storage);
        return 1;
    }
    
    // Write data to extent
    char data[128 * 1024] = "Hello, KES!";
    result = kes_extent_write(storage, &extent, data, sizeof(data), 0);
    if (result != KES_SUCCESS) {
        printf("Failed to write extent: %s\n",
               kes_get_error_string(result));
    }
    
    // Read data back
    char read_buffer[128 * 1024];
    result = kes_extent_read(storage, &extent, read_buffer, 
                            sizeof(read_buffer), 0);
    if (result != KES_SUCCESS) {
        printf("Failed to read extent: %s\n",
               kes_get_error_string(result));
    }
    
    // Verify data
    if (strncmp(data, read_buffer, 11) == 0) {
        printf("Data verification successful!\n");
    }
    
    // Clean up
    kes_extent_free(storage, &extent);
    kes_storage_sync(storage);
    kes_storage_close(storage);
    
    return 0;
}
```

### External Descriptor Management

```c
// Example: Store descriptor in SQLite database
typedef struct {
    sqlite3* db;
    const char* storage_name;
} db_context_t;

static int save_descriptor_to_db(void* buffer, size_t size, 
                                void* user_data) {
    db_context_t* ctx = (db_context_t*)user_data;
    
    const char* sql = "INSERT OR REPLACE INTO storage_descriptors "
                     "(name, descriptor_data) VALUES (?, ?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, ctx->storage_name, -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 2, buffer, size, SQLITE_STATIC);
    
    int result = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return (result == SQLITE_DONE) ? 0 : -1;
}

static int load_descriptor_from_db(void* buffer, size_t size,
                                  void* user_data) {
    db_context_t* ctx = (db_context_t*)user_data;
    
    const char* sql = "SELECT descriptor_data FROM storage_descriptors "
                     "WHERE name = ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, ctx->storage_name, -1, SQLITE_STATIC);
    
    int result = sqlite3_step(stmt);
    if (result == SQLITE_ROW) {
        const void* data = sqlite3_column_blob(stmt, 0);
        int data_size = sqlite3_column_bytes(stmt, 0);
        
        if (data_size <= size) {
            memcpy(buffer, data, data_size);
            result = 0;
        } else {
            result = -1;  // Buffer too small
        }
    } else {
        result = -1;  // Not found
    }
    
    sqlite3_finalize(stmt);
    return result;
}

int main() {
    // Set up database context
    sqlite3* db;
    sqlite3_open("storage.db", &db);
    
    db_context_t db_ctx = {
        .db = db,
        .storage_name = "my_storage"
    };
    
    // Configure external descriptor management
    kes_storage_config_t config;
    kes_profile_load(KES_PROFILE_BUILD_DEFAULT, &config);
    config.device_path = "/dev/sdb1";
    
    config.descriptor_management.use_external = true;
    config.descriptor_management.external_managed.save_callback = 
        save_descriptor_to_db;
    config.descriptor_management.external_managed.load_callback = 
        load_descriptor_from_db;
    config.descriptor_management.external_managed.user_data = &db_ctx;
    
    // Create storage with external descriptor
    kes_storage_t* storage;
    int result = kes_storage_create(&config, &storage);
    
    // Use storage normally...
    
    kes_storage_close(storage);
    sqlite3_close(db);
    return 0;
}
```

---

## Integration Patterns

### File System Integration

```c
// Example: FUSE file system integration
static int fuse_getattr(const char* path, struct stat* stbuf) {
    // Map file path to KES extent
    uint64_t extent_id = path_to_extent_id(path);
    
    kes_extent_descriptor_t extent;
    int result = kes_extent_lookup(storage, extent_id, &extent);
    if (result != KES_SUCCESS) {
        return -ENOENT;
    }
    
    // Fill stat structure
    stbuf->st_mode = S_IFREG | 0644;
    stbuf->st_size = kes_calculate_extent_size(&extent);
    stbuf->st_blocks = extent.block_count;
    stbuf->st_blksize = block_size;
    
    return 0;
}

static int fuse_read(const char* path, char* buffer, size_t size,
                     off_t offset, struct fuse_file_info* fi) {
    uint64_t extent_id = path_to_extent_id(path);
    
    kes_extent_descriptor_t extent;
    int result = kes_extent_lookup(storage, extent_id, &extent);
    if (result != KES_SUCCESS) {
        return -EIO;
    }
    
    result = kes_extent_read(storage, &extent, buffer, size, offset);
    return (result == KES_SUCCESS) ? size : -EIO;
}
```

### Database Integration

```c
// Example: Database transaction integration
int database_transaction_commit(db_transaction_t* tx) {
    // Begin KES transaction
    kes_transaction_batch_t* kes_tx = kes_transaction_begin(storage, 
                                                          tx->operation_count);
    
    // Convert database operations to KES operations
    for (int i = 0; i < tx->operation_count; i++) {
        db_operation_t* op = &tx->operations[i];
        
        switch (op->type) {
            case DB_OP_INSERT:
                // Allocate new extent and add to batch
                kes_extent_request_t req = {
                    .block_count = calculate_blocks_needed(op->data_size),
                    .flags = 0
                };
                kes_extent_descriptor_t extent;
                kes_extent_allocate(storage, &req, &extent);
                kes_transaction_add_write(kes_tx, &extent, op->data);
                break;
                
            case DB_OP_UPDATE:
                // Update existing extent
                kes_extent_descriptor_t extent = lookup_extent(op->record_id);
                kes_transaction_add_write(kes_tx, &extent, op->data);
                break;
                
            case DB_OP_DELETE:
                // Free extent
                kes_extent_descriptor_t extent = lookup_extent(op->record_id);
                kes_transaction_add_free(kes_tx, &extent);
                break;
        }
    }
    
    // Commit KES transaction
    int result = kes_transaction_commit(kes_tx);
    return (result == KES_SUCCESS) ? DB_SUCCESS : DB_ERROR;
}
```

---

**Document Version**: 1.0  
**Last Updated**: December 2025  
**Next Review**: After implementation completion

This API reference provides comprehensive documentation for all KES 
functions, data structures, and usage patterns. The API is designed 
to be consistent, thread-safe, and platform-agnostic while providing 
the flexibility needed for diverse storage applications.
