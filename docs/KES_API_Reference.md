# KANEK Extents Storage (KES) API Reference Manual
## Version 1.0

---

## Standing note (Phase 6 docs truth pass)

This document previously described a much larger API surface than
exists in `include/kes/*.h` -- a Flash-Specific API, Zone Management
API, Garbage Collection API, Wear Leveling API, hardware-profile
loading (`kes_profile_load`), a Serialization API, `kes_get_last_error`,
extent lookup/enumeration/iterators, and extended `kes_extent_descriptor_t`/
`kes_extent_request_t`/`kes_storage_descriptor_t` fields (`allocation_time`,
`zone_id`, `placement_hint`, `zone_preference`) -- none of which are
declared anywhere in this repository. This revision documents only
functions and types that are actually declared in `include/kes/kes_types.h`,
`kes_bitmap.h`, `kes_storage.h`, and `kes_cache.h`, verified against
those headers directly (not re-derived from the old text of this file).
See `AGENTS.md`'s "Ground truth" section and `PENDING_ITEMS.md` for
what is and is not implemented. The previous Cache API was entirely
undocumented here despite being the most-tested, most fully-implemented
layer in the codebase -- that gap is fixed below.

---

## Table of Contents

1. [API Overview](#api-overview)
2. [Core Storage API](#core-storage-api)
3. [Extent Management API](#extent-management-api)
4. [Bitmap Management API](#bitmap-management-api)
5. [Cache API](#cache-api)
6. [Utility Functions](#utility-functions)
7. [Error Handling](#error-handling)
8. [Usage Examples](#usage-examples)

---

## API Overview

### Design Principles

1. **Consistent Error Handling**: all public functions return `int` --
   `KES_SUCCESS` (0) or a negative `KES_ERROR_*` code -- with results
   delivered through output parameters, except `kes_cache_create()`
   (returns `kes_cache_t *` or `NULL`), `kes_bitmap_test()`/
   `kes_extent_hash()`/`kes_extent_equal()` (return the value/hash/bool
   directly), and `kes_get_error_string()`/`kes_calculate_blocks_needed()`/
   `kes_calculate_extent_size()` (pure calculations/lookups).
2. **Thread Safety**: `kes_storage_t` is protected by an internal
   `pthread_mutex_t`; `kes_cache_t` uses per-bucket `pthread_rwlock_t`
   locks plus a per-entry `pthread_mutex_t` and a cache-wide lock (see
   `AGENTS.md`'s "Ground truth" section for the concurrency hazards
   this involved getting right). `kes_bitmap_t` on its own has no
   internal locking -- callers using it directly (outside of
   `kes_storage_t`, which serializes access to its own bitmap) are
   responsible for their own synchronization.
3. **Resource Management**: every `_create`/`_open` has a matching
   `_destroy`/`_close`. Cache extents obtained via
   `kes_cache_get_extent()` must be released with
   `kes_cache_put_extent()`.
4. **Minimal Dependencies**: standard C library (`-std=c99`) plus
   `pthread`; `kes_cache.c`/`kes_storage.c` additionally `#include
   "trace.h"` from the sibling `kanek_foundations` checkout for
   `TRACE_ERR`/`TRACE_SYSERR` macros only (header-only, nothing links
   against it).

### Header Files

```c
#include <kes/kes_storage.h>     /* Core storage + extent API */
#include <kes/kes_cache.h>       /* Extent caching layer API */
#include <kes/kes_bitmap.h>      /* Block bitmap API */
#include <kes/kes_types.h>       /* Shared types, error codes, macros
                                   * (transitively included by the
                                   * three headers above) */
```

There is no `kes_flash.h`, `kes_zone.h`, `kes_serialization.h`, or
similar -- those headers do not exist in this repository.

### Return Codes

From `kes_types.h` -- exactly eight codes, not ten:

```c
#define KES_SUCCESS          0
#define KES_ERROR_INVALID   -1   /* Invalid parameters */
#define KES_ERROR_NOMEM     -2   /* Out of memory */
#define KES_ERROR_NOTFOUND  -3   /* Resource not found */
#define KES_ERROR_EXISTS    -4   /* Resource already exists */
#define KES_ERROR_IO        -5   /* I/O error */
#define KES_ERROR_NOSPACE   -6   /* No space available */
#define KES_ERROR_CORRUPT   -7   /* Data corruption detected */
#define KES_ERROR_BUSY      -8   /* Resource busy -- referenced or
                                   * pinned; currently only returned
                                   * by kes_cache.c (kes_cache_get_extent()
                                   * when eviction can't free room,
                                   * kes_cache_invalidate() on a
                                   * referenced/pinned entry) */
```

There is no `KES_ERROR_VERSION` or `KES_ERROR_TIMEOUT`.

---

## Core Storage API

### Storage Lifecycle Management

#### kes_storage_create

```c
int kes_storage_create( const kes_storage_config_t *config,
                         kes_storage_t **storage);
```

Create a new KES storage instance and format its backing file.

**Parameters:** `config` -- storage configuration; `storage` -- output
handle.

**Returns:** `KES_SUCCESS` or an error code.

```c
kes_storage_config_t config = {
    .device_path = "/tmp/my_storage.kes",
    .device_size = 50 * 1024 * 1024,   /* 50MB */
    .block_size = KES_DEFAULT_BLOCK_SIZE,  /* 8192 */
    .flags = KES_STORAGE_CREATE | KES_STORAGE_TRUNCATE,
    .strategy = KES_ALLOC_FIRST_FIT
};

kes_storage_t *storage;
int result = kes_storage_create(&config, &storage);
```

Note: `config.strategy` is accepted and validated, but only
`KES_ALLOC_FIRST_FIT` is actually implemented -- see "Extent
Management API" below and `AGENTS.md`'s "Module layering" section.

#### kes_storage_open

```c
int kes_storage_open( const char *device_path, uint32_t flags,
                       kes_storage_t **storage);
```

Open an existing KES storage file, reloading its descriptor and
bitmap from block 0 / the bitmap region.

**Parameters:** `device_path`; `flags` (`kes_storage_flags_t` --
`KES_STORAGE_READONLY`/`_CREATE`/`_TRUNCATE`/`_SYNC`); `storage` --
output handle.

#### kes_storage_close

```c
int kes_storage_close( kes_storage_t *storage);
```

#### kes_storage_sync

```c
int kes_storage_sync( kes_storage_t *storage);
```

Synchronize the in-memory bitmap and descriptor to the backing file.

### Storage Information

#### kes_storage_get_stats

```c
int kes_storage_get_stats( kes_storage_t *storage,
                            kes_storage_stats_t *stats);
```

There is no `kes_storage_get_info()`/`kes_storage_info_t` -- this is
the only stats accessor. `kes_storage_stats_t` (`kes_types.h`):

```c
typedef struct {
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t used_blocks;
    uint64_t allocated_extents;
    uint64_t fragmentation;      /* percentage, not a float ratio */

    uint64_t reads_completed;
    uint64_t writes_completed;
    uint64_t bytes_read;
    uint64_t bytes_written;
} kes_storage_stats_t;
```

#### kes_storage_get_descriptor

```c
int kes_storage_get_descriptor( kes_storage_t *storage,
                                 kes_storage_descriptor_t *descriptor);
```

`kes_storage_descriptor_t` (`kes_types.h`) has exactly these fields --
no `index_start_block`/`index_blocks`, no `flags`, no
`allocated_extents`:

```c
typedef struct {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t block_size;
    uint64_t total_blocks;

    uint64_t bitmap_start_block;
    uint64_t bitmap_blocks;
    uint64_t user_start_block;
    uint64_t user_blocks;

    uint64_t free_blocks;
    uint64_t used_blocks;
    uint64_t next_extent_id;

    uint8_t reserved[32];
} kes_storage_descriptor_t;
```

---

## Extent Management API

### kes_extent_allocate

```c
int kes_extent_allocate( kes_storage_t *storage,
                          const kes_extent_request_t *request,
                          kes_extent_descriptor_t *extent);
```

Allocate a new extent. **Always uses first-fit
(`allocate_extent_first_fit()` in `src/kes_storage.c`), regardless of
`config.strategy`** -- best-fit/worst-fit/next-fit are declared in
`kes_allocation_strategy_t` but not wired up to any allocation logic.
There is no buddy-system, slab, log-structured, or hybrid allocator
anywhere in this codebase.

`kes_extent_request_t` and `kes_extent_descriptor_t` (`kes_types.h`)
have exactly these fields -- no `placement_hint` enum, no
`zone_preference`, no `zone_id`, no `allocation_time`:

```c
typedef struct {
    uint32_t block_count;
    uint32_t alignment;          /* 0 = any */
    uint64_t hint_block;
    uint32_t flags;
} kes_extent_request_t;

typedef struct {
    uint64_t start_block;
    uint32_t block_count;
    uint32_t flags;
    uint64_t extent_id;
} kes_extent_descriptor_t;
```

```c
kes_extent_request_t request = {
    .block_count = 16,
    .alignment = 0,
    .hint_block = 0,
    .flags = 0
};

kes_extent_descriptor_t extent;
int result = kes_extent_allocate(storage, &request, &extent);
```

### kes_extent_free

```c
int kes_extent_free( kes_storage_t *storage,
                      const kes_extent_descriptor_t *extent);
```

### kes_extent_read / kes_extent_write

```c
int kes_extent_read( kes_storage_t *storage,
                      const kes_extent_descriptor_t *extent,
                      void *buffer, size_t size, uint64_t offset);

int kes_extent_write( kes_storage_t *storage,
                       const kes_extent_descriptor_t *extent,
                       const void *buffer, size_t size, uint64_t offset);
```

Raw file-offset I/O against the extent's `start_block`/`block_count` --
these do not consult or update the cache layer (see "Module layering"
in `AGENTS.md`).

There is no `kes_extent_sync()`, `kes_extent_lookup()`,
`kes_extent_enumerate()`, or `kes_extent_iterator_t` -- callers are
responsible for tracking which extents they've allocated (see the
per-file/per-page `extent` bookkeeping pattern in
`PROJECT_OVERVIEW_KES.md`'s integration examples).

---

## Bitmap Management API

`kes_bitmap_t` (`include/kes/kes_bitmap.h`) is a single, non-adaptive
raw-buffer bitmap -- there is no `kes_bitmap_strategy_t`, no
`KES_BITMAP_SLIDING_WINDOW`/`_ON_DEMAND`/`_COMPRESSED`/`_HIERARCHICAL`,
and no RLE/hierarchical/compressed variant anywhere in the code.

```c
int kes_bitmap_create( uint64_t total_blocks, kes_bitmap_t **bitmap);
void kes_bitmap_destroy( kes_bitmap_t *bitmap);

int kes_bitmap_set( kes_bitmap_t *bitmap, uint64_t bit_index);
int kes_bitmap_clear( kes_bitmap_t *bitmap, uint64_t bit_index);
bool kes_bitmap_test( kes_bitmap_t *bitmap, uint64_t bit_index);

int kes_bitmap_set_range( kes_bitmap_t *bitmap, uint64_t start_bit,
                           uint32_t bit_count);
int kes_bitmap_clear_range( kes_bitmap_t *bitmap, uint64_t start_bit,
                             uint32_t bit_count);

int kes_bitmap_find_free( kes_bitmap_t *bitmap, uint32_t bit_count,
                           uint64_t start_hint, uint64_t *found_start);

int kes_bitmap_get_stats( kes_bitmap_t *bitmap, uint64_t *total_bits,
                           uint64_t *free_bits, uint64_t *used_bits);

int kes_bitmap_load( kes_bitmap_t *bitmap, int fd, off_t offset);
int kes_bitmap_save( kes_bitmap_t *bitmap, int fd, off_t offset);
```

`kes_bitmap_find_free()` returns `KES_SUCCESS` if a run of `bit_count`
contiguous free bits was found (written to `*found_start`), or
`KES_ERROR_NOSPACE` if not.

---

## Cache API

`kes_cache_t` (`include/kes/kes_cache.h`) is an independent extent
cache that sits in front of a storage backend via caller-supplied I/O
callbacks -- it does not call into `kes_storage.c` (see "Module
layering" in `AGENTS.md`). This section did not exist in earlier
revisions of this document even though the cache layer is the most
thoroughly tested part of KES (`tests/test_kes_cache.c`, 23/23;
`tests/test_kes_cache_full.c`, 12/12 -- see `TESTS_AND_EXAMPLES.md`).

### Cache Lifecycle

```c
kes_cache_t *kes_cache_create( const kes_cache_config_t *config);
int kes_cache_destroy( kes_cache_t *cache);
int kes_cache_start( kes_cache_t *cache);
int kes_cache_stop( kes_cache_t *cache);
```

`kes_cache_create()` returns `NULL` on any invalid config, **including
`config.policy == KES_CACHE_LFU` or `KES_CACHE_CUSTOM`** -- only
`KES_CACHE_LRU` is implemented (see `kes_cache_policy_t` below).
There is no three-argument `kes_cache_create(size, policy, &cache)`
constructor; the real signature takes a single config struct.

`kes_cache_start()` launches `config.background_threads` real
background threads, each waking every `config.sync_interval_ms`
(timed against `CLOCK_MONOTONIC`) to run the same flush-then-evict
sweep `kes_cache_sync()` runs manually. Returns `KES_ERROR_EXISTS` if
already running, `KES_ERROR_INVALID` if `config.background_threads <=
0`.

`kes_cache_config_t` (`kes_cache.h`) -- note the field is `policy`,
not `eviction_policy`, and it is typed `kes_cache_policy_t`, not
`int`:

```c
typedef struct {
    size_t max_memory;
    size_t min_memory;
    uint32_t max_entries;
    kes_cache_policy_t policy;
    int background_threads;
    int sync_interval_ms;
    bool enable_prefetch;
    bool enable_compression;
    void *device_handle;
} kes_cache_config_t;

typedef enum {
    KES_CACHE_LRU = 0,     /* the only implemented policy */
    KES_CACHE_LFU,         /* rejected by kes_cache_create() -- NULL */
    KES_CACHE_CUSTOM       /* rejected by kes_cache_create() -- NULL */
} kes_cache_policy_t;
```

`enable_prefetch`/`enable_compression` are accepted config fields but
there is no prefetching or compression logic anywhere in
`src/kes_cache.c` -- they are currently inert.

### Extent Operations

```c
int kes_cache_get_extent( kes_cache_t *cache,
                           const kes_extent_id_t *id, void **buffer);
int kes_cache_put_extent( kes_cache_t *cache,
                           const kes_extent_id_t *id);
int kes_cache_pin_extent( kes_cache_t *cache,
                           const kes_extent_id_t *id);
int kes_cache_unpin_extent( kes_cache_t *cache,
                             const kes_extent_id_t *id);
int kes_cache_mark_dirty( kes_cache_t *cache,
                           const kes_extent_id_t *id);
int kes_cache_flush_extent( kes_cache_t *cache,
                             const kes_extent_id_t *id);
int kes_cache_sync( kes_cache_t *cache);
int kes_cache_invalidate( kes_cache_t *cache,
                           const kes_extent_id_t *id);
```

`kes_extent_id_t` (`kes_cache.h`) -- this identifies a cached extent
and is a different type from `kes_extent_descriptor_t`:

```c
typedef struct {
    uint64_t start_block;
    uint32_t block_count;
    uint32_t block_size;
    uint32_t reserved;
} kes_extent_id_t;
```

Semantics worth calling out explicitly (per `PENDING_ITEMS.md`'s
"Resolved" section, which is the authoritative source for these):

- **`kes_cache_get_extent()`** loads from disk on a miss, evicting
  from the LRU tail as needed to respect `config.max_entries`/
  `config.max_memory`; returns `KES_ERROR_BUSY` (not a silent
  overshoot) if it can't free enough room because every cached entry
  is currently referenced or pinned.
- **Pinning is reference-counted, not boolean.**
  `kes_cache_pin_extent()` increments `pin_count` on every call
  (setting `KES_EXTENT_PINNED` only on the 0->1 transition);
  `kes_cache_unpin_extent()` decrements it (clearing the pinned state
  only on the ->0 transition). Calling unpin when `pin_count == 0` is
  a guarded no-op -- `KES_SUCCESS`, no underflow, no state change.
- **`kes_cache_sync()`** flushes all dirty extents, then frees any
  entry that ends up unreferenced and unpinned after its flush
  attempt. Entries still referenced or pinned are flushed if dirty but
  never freed by `sync()` -- this is a durability checkpoint, not a
  full eviction pass. A clean no-op on an empty cache.
- **`kes_cache_invalidate()`** discards dirty data unconditionally,
  without writing it back -- this is the difference between
  `invalidate()` (discard) and `sync()`/`flush_extent()` (persist).
  Returns `KES_ERROR_NOTFOUND` if the extent isn't cached,
  `KES_ERROR_BUSY` if it's currently referenced or pinned.

### Cache Management and Statistics

```c
int kes_cache_get_stats( kes_cache_t *cache, kes_cache_stats_t *stats);
int kes_cache_reset_stats( kes_cache_t *cache);
int kes_cache_set_io_callbacks( kes_cache_t *cache,
    int (*read_func)( void *device, const kes_extent_id_t *id,
                       void *buffer, size_t size),
    int (*write_func)( void *device, const kes_extent_id_t *id,
                        const void *buffer, size_t size),
    int (*sync_func)( void *device));
```

`kes_cache_reset_stats()` resets only the cumulative counters (`hits`,
`misses`, `evictions`, `flushes`, `bytes_read`, `bytes_written`) to
zero. It does **not** reset the state counters (`memory_used`,
`entries_cached`, `entries_dirty`, `entries_pinned`) -- those describe
the cache's current contents, not history.

`kes_cache_stats_t` (`kes_cache.h`):

```c
typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t flushes;
    uint64_t bytes_read;
    uint64_t bytes_written;
    size_t memory_used;
    uint32_t entries_cached;
    uint32_t entries_dirty;
    uint32_t entries_pinned;
} kes_cache_stats_t;
```

### Utility Functions (cache)

```c
uint32_t kes_extent_hash( const kes_extent_id_t *id);
bool kes_extent_equal( const kes_extent_id_t *id1,
                        const kes_extent_id_t *id2);
void kes_cache_get_default_config( kes_cache_config_t *config,
                                    bool is_edge_device);
```

---

## Utility Functions

```c
void kes_get_version( uint16_t *major, uint16_t *minor, uint16_t *patch);
const char *kes_get_error_string( int error_code);
uint32_t kes_calculate_blocks_needed( size_t byte_size,
                                       uint32_t block_size);
size_t kes_calculate_extent_size( const kes_extent_descriptor_t *extent);
int kes_config_validate( const kes_storage_config_t *config);
```

There is no `kes_get_build_info()`/`kes_build_info_t`,
`kes_profile_load()`/`kes_hardware_profile_t`,
`kes_profile_auto_detect()`, or `kes_storage_tune()` -- no runtime
hardware-profile system exists.

---

## Error Handling

```c
int result = kes_storage_create(&config, &storage);
if (result != KES_SUCCESS) {
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
            printf("Error: %s\n", kes_get_error_string(result));
            break;
    }
}
```

There is no `kes_get_last_error()`/`kes_error_info_t` -- the only
error-description accessor is `kes_get_error_string()`.

---

## Usage Examples

### Basic Storage Operations

```c
#include <kes/kes_storage.h>
#include <stdio.h>

int main(void) {
    kes_storage_config_t config = {
        .device_path = "/tmp/example.kes",
        .device_size = 50 * 1024 * 1024,
        .block_size = KES_DEFAULT_BLOCK_SIZE,
        .flags = KES_STORAGE_CREATE | KES_STORAGE_TRUNCATE,
        .strategy = KES_ALLOC_FIRST_FIT
    };

    kes_storage_t *storage;
    int result = kes_storage_create(&config, &storage);
    if (result != KES_SUCCESS) {
        printf("Failed to create storage: %s\n",
               kes_get_error_string(result));
        return(1);
    }

    kes_extent_request_t request = {
        .block_count = 16, .alignment = 0, .hint_block = 0, .flags = 0
    };

    kes_extent_descriptor_t extent;
    result = kes_extent_allocate(storage, &request, &extent);
    if (result != KES_SUCCESS) {
        printf("Failed to allocate extent: %s\n",
               kes_get_error_string(result));
        kes_storage_close(storage);
        return(1);
    }

    char data[128 * 1024] = "Hello, KES!";
    kes_extent_write(storage, &extent, data, sizeof(data), 0);

    char read_buffer[128 * 1024];
    kes_extent_read(storage, &extent, read_buffer, sizeof(read_buffer), 0);

    kes_extent_free(storage, &extent);
    kes_storage_sync(storage);
    kes_storage_close(storage);
    return(0);
}
```

### Cache Layer Usage

```c
#include <kes/kes_cache.h>

static int my_read_extent( void *device, const kes_extent_id_t *id,
                            void *buffer, size_t size) {
    /* pread() from `device` at id->start_block * id->block_size */
    return(KES_SUCCESS);
}

static int my_write_extent( void *device, const kes_extent_id_t *id,
                             const void *buffer, size_t size) {
    /* pwrite() to `device` at id->start_block * id->block_size */
    return(KES_SUCCESS);
}

int main(void) {
    kes_cache_config_t config;
    kes_cache_get_default_config(&config, false /* is_edge_device */);
    config.policy = KES_CACHE_LRU;  /* the only implemented policy */

    kes_cache_t *cache = kes_cache_create(&config);
    kes_cache_set_io_callbacks(cache, my_read_extent, my_write_extent,
                                NULL);
    kes_cache_start(cache);  /* optional: automatic background flush */

    kes_extent_id_t id = {
        .start_block = 100, .block_count = 8,
        .block_size = 8192, .reserved = 0
    };

    void *buffer;
    if (kes_cache_get_extent(cache, &id, &buffer) == KES_SUCCESS) {
        kes_cache_mark_dirty(cache, &id);
        kes_cache_put_extent(cache, &id);
    }

    kes_cache_stop(cache);
    kes_cache_destroy(cache);
    return(0);
}
```

There is no `kes_transaction_begin()`/`kes_transaction_commit()`, no
FUSE/SQLite integration code, and no `kes_storage_t`-to-`kes_cache_t`
wiring example anywhere in this repository -- `examples/example_kes_usage.c`
exercises the storage layer only.

---

**Document Version**: 1.0 (Phase 6 docs truth pass)
**Last Updated**: 2026-08-28
