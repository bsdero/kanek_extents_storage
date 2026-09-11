#ifndef KES_TYPES_H
#define KES_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct kes_storage kes_storage_t;
typedef struct kes_bitmap kes_bitmap_t;

/* Version information */
#define KES_VERSION_MAJOR    2   /* v2: KES-6 added bitmap_checksum */
#define KES_VERSION_MINOR    0
#define KES_VERSION_PATCH    0

/* Return codes */
#define KES_SUCCESS          0
#define KES_ERROR_INVALID   -1
#define KES_ERROR_NOMEM     -2
#define KES_ERROR_NOTFOUND  -3
#define KES_ERROR_EXISTS    -4
#define KES_ERROR_IO        -5
#define KES_ERROR_NOSPACE   -6
#define KES_ERROR_CORRUPT   -7
#define KES_ERROR_BUSY      -8   /* Resource busy (e.g. referenced or
                                   * pinned); shared with the cache
                                   * module, which is the only current
                                   * user of this code. */

/* Magic numbers and constants */
#define KES_MAGIC_NUMBER     0x4B455353  /* "KESS" */
#define KES_DEFAULT_BLOCK_SIZE   8192    /* 8KB default */
#define KES_MIN_BLOCK_SIZE       4096    /* 4KB minimum */
#define KES_MAX_BLOCK_SIZE      65536    /* 64KB maximum */

/* Block size options */
typedef enum {
    KES_BLOCK_SIZE_4K   = 4096,
    KES_BLOCK_SIZE_8K   = 8192,
    KES_BLOCK_SIZE_16K  = 16384,
    KES_BLOCK_SIZE_32K  = 32768,
    KES_BLOCK_SIZE_64K  = 65536
} kes_block_size_t;

/* Storage flags */
typedef enum {
    KES_STORAGE_READONLY    = 0x01,  /* Read-only access */
    KES_STORAGE_CREATE      = 0x02,  /* Create if not exists */
    KES_STORAGE_TRUNCATE    = 0x04,  /* Truncate existing */
    KES_STORAGE_SYNC        = 0x08   /* Synchronous I/O -- see the
                                       * detailed note below (KES-7)
                                       * for exactly what this does
                                       * and does not make durable. */
} kes_storage_flags_t;

/*
 * KES-7: KES_STORAGE_SYNC's actual, confirmed effect (by reading
 * kes_storage_create()/kes_storage_open(), src/kes_storage.c): it
 * adds O_SYNC to the backing fd's open()/create() flags. Nothing
 * else. The kes_storage_t.sync_writes field it also sets
 * (include/kes/kes_storage.h) is stored but never read anywhere else
 * in this codebase (confirmed by grep across src/) -- it has no
 * effect of its own beyond that one open()-time O_SYNC flag.
 *
 * What this precisely changes:
 *   - WITHOUT KES_STORAGE_SYNC: kes_extent_write()'s raw write() call
 *     lands in the OS page cache. It is visible to any other process
 *     reading the same file immediately (ordinary page-cache
 *     coherency) and survives this process exiting or crashing
 *     normally -- but is NOT guaranteed to survive a real power loss
 *     or kernel crash until something calls fsync() on this fd.
 *     kes_storage_sync()/kes_storage_close() are the only calls in
 *     this library that do that.
 *   - WITH KES_STORAGE_SYNC: every write on this fd -- extent data via
 *     kes_extent_write(), and the descriptor/bitmap writes inside
 *     kes_storage_sync()/kes_storage_close()/kes_storage_create() --
 *     becomes synchronous at the kernel level (O_SYNC): durable
 *     against real power loss the moment the write() call returns, at
 *     a real per-write latency cost.
 *
 * What this does NOT change: kes_storage_sync()/kes_storage_close()
 * remain the only calls that persist storage->desc.free_blocks/
 * used_blocks/storage->bitmap to disk at all -- O_SYNC only affects
 * the durability of a write that already happens, it does not cause
 * any additional writes to happen. See
 * tests/test_kes_crash_consistency.c's test_no_sync_reopen_durability
 * for the concrete consequence: a crash between an allocation and an
 * explicit sync can still lose that allocation's bookkeeping even
 * with KES_STORAGE_SYNC set, even though the extent DATA itself was
 * already durable. (If kes_5_plan.md has been applied, this no
 * longer applies -- kes_extent_allocate()/kes_extent_free() persist
 * their own bookkeeping immediately regardless of this flag.)
 */

/* Allocation strategies */
typedef enum {
    KES_ALLOC_FIRST_FIT = 0,         /* Speed optimized */
    KES_ALLOC_BEST_FIT,              /* Fragmentation optimized */
    KES_ALLOC_WORST_FIT,             /* For testing/special cases */
    KES_ALLOC_NEXT_FIT               /* Circular first fit */
} kes_allocation_strategy_t;

/* Extent descriptor - describes a contiguous block range */
typedef struct {
    uint64_t start_block;            /* Starting block address */
    uint32_t block_count;            /* Number of contiguous blocks */
    uint32_t flags;                  /* Extent flags */
    uint64_t extent_id;              /* Unique extent identifier */
} kes_extent_descriptor_t;

/* Storage descriptor - stored in block 0 (minimal version) */
typedef struct {
    uint32_t magic;                  /* Magic number for validation */
    uint16_t version_major;          /* Major version */
    uint16_t version_minor;          /* Minor version */
    uint32_t block_size;             /* Block size in bytes */
    uint64_t total_blocks;           /* Total blocks in storage */

    /* Layout information */
    uint64_t bitmap_start_block;     /* Bitmap start block */
    uint64_t bitmap_blocks;          /* Bitmap blocks count */
    uint64_t user_start_block;       /* User data start block */
    uint64_t user_blocks;            /* User data blocks count */

    /* Statistics */
    uint64_t free_blocks;            /* Current free blocks */
    uint64_t used_blocks;            /* Current used blocks */
    uint64_t next_extent_id;         /* Next extent ID to allocate */

    /* KES-6: CRC-32C of the on-disk bitmap region (kfl_crc32c() over
     * bitmap->data, bitmap->total_bytes), refreshed on every save
     * and verified on kes_storage_open(). */
    uint32_t bitmap_checksum;

    /* Reserved for future use -- shrunk from 32 to 28 bytes to make
     * room for bitmap_checksum above without growing the struct. */
    uint8_t reserved[28];
} kes_storage_descriptor_t;

/* Storage configuration */
typedef struct {
    const char *device_path;         /* Path to storage device/file */
    uint64_t device_size;            /* Total device size in bytes */
    uint32_t block_size;             /* Block size */
    uint32_t flags;                  /* Storage flags */
    kes_allocation_strategy_t strategy; /* Allocation strategy */
} kes_storage_config_t;

/* Extent allocation request */
typedef struct {
    uint32_t block_count;            /* Requested blocks */
    uint32_t alignment;              /* Block alignment (0 = any) */
    uint64_t hint_block;             /* Preferred start block */
    uint32_t flags;                  /* Allocation flags */
} kes_extent_request_t;

/* Storage statistics */
typedef struct {
    uint64_t total_blocks;           /* Total blocks */
    uint64_t free_blocks;            /* Free blocks */
    uint64_t used_blocks;            /* Used blocks */
    uint64_t allocated_extents;      /* Allocated extents */
    uint64_t fragmentation;          /* Fragmentation percentage */

    /* I/O statistics */
    uint64_t reads_completed;        /* Read operations */
    uint64_t writes_completed;       /* Write operations */
    uint64_t bytes_read;             /* Total bytes read */
    uint64_t bytes_written;          /* Total bytes written */
} kes_storage_stats_t;

/* Utility macros */
#define KES_BLOCKS_TO_BYTES(blocks, block_size) \
    ((uint64_t)(blocks) * (block_size))

#define KES_BYTES_TO_BLOCKS(bytes, block_size) \
    (((bytes) + (block_size) - 1) / (block_size))

#define KES_ALIGN_UP(value, alignment) \
    (((value) + (alignment) - 1) & ~((alignment) - 1))

#define KES_IS_POWER_OF_2(x) \
    ((x) != 0 && ((x) & ((x) - 1)) == 0)

#ifdef __cplusplus
}
#endif

#endif /* KES_TYPES_H */
