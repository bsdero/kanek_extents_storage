# KES Tests and Examples Documentation

## Standing note (Phase 6 docs truth pass)

This document previously described only 2 of the current 6 test
binaries (`test_kes_minimal.c` and `test_kes_cache.c`), and its cache
test status ("4/6 Passing", "2 known issues") was stale by a wide
margin. The verified current baseline, from a fresh `make test` run
immediately before this pass: **70/70 tests passing across all 6
binaries** -- `test_kes_minimal` (9), `test_kes_bitmap_full` (10),
`test_kes_storage_full` (15), `test_kes_cache` (23),
`test_kes_cache_full` (12), `test_kes_multiprocess` (1). See
`PENDING_ITEMS.md` (verified state) and `AGENTS.md`'s "Ground truth"
section. This revision adds the four test binaries that were
undocumented here entirely (`test_kes_bitmap_full.c`,
`test_kes_storage_full.c`, `test_kes_cache_full.c`,
`test_kes_multiprocess.c`) and rewrites the cache test section to
match reality.

## Overview

This document describes the comprehensive test suite and example 
programs included with KANEK Extents Storage (KES). The tests validate 
all core functionality and demonstrate real-world usage patterns.

## Test Suite Organization

### Core Tests (`test_kes_minimal.c`) - ✅ 9/9 Passing

The core test suite validates fundamental KES storage operations with 
100% success rate. These tests ensure production-ready reliability.

#### 1. Bitmap Creation Test (`test_bitmap_creation`)

**Purpose**: Validates bitmap memory management and initialization

**What it tests:**
- Memory allocation for block tracking bitmap
- Initial state verification (all blocks free)
- Parameter validation and error handling
- Resource cleanup and destruction

**Test scenario:**
```c
// Creates bitmap for 1,000 blocks
kes_bitmap_create(1000, &bitmap);
// Verifies: total=1000, free=1000, used=0
```

**Real-world importance**: Bitmap is the foundation for tracking which 
storage blocks are available vs. occupied - like a parking lot 
occupancy system.

---

#### 2. Bitmap Operations Test (`test_bitmap_basic_operations`)

**Purpose**: Validates individual bit manipulation operations

**What it tests:**
- Setting bits (marking blocks as used)
- Clearing bits (marking blocks as free)
- Testing bit status (checking if block is occupied)
- Range operations (bulk set/clear operations)

**Test scenario:**
```c
kes_bitmap_set(bitmap, 42);          // Mark block 42 as used
assert(kes_bitmap_test(bitmap, 42)); // Verify it's marked
kes_bitmap_clear(bitmap, 42);        // Mark as free
assert(!kes_bitmap_test(bitmap, 42)); // Verify it's clear
```

**Real-world importance**: These are the atomic operations for managing 
storage allocation - every file write depends on these working correctly.

---

#### 3. Bitmap Find Free Test (`test_bitmap_find_free`)

**Purpose**: Validates free space allocation algorithms

**What it tests:**
- Finding contiguous free blocks
- Handling fragmented storage scenarios
- Search hints and optimization
- "No space available" conditions

**Test scenario:**
```c
// Create fragmentation pattern
kes_bitmap_set_range(bitmap, 10, 5);  // Occupy blocks 10-14
kes_bitmap_set_range(bitmap, 20, 3);  // Occupy blocks 20-22

// Find space for 3 contiguous blocks
kes_bitmap_find_free(bitmap, 3, 0, &found_start);
// Should find space at 0-2, 15-17, or 23-25
```

**Real-world importance**: Like finding available hotel rooms when some 
are booked - the system must efficiently locate contiguous free space 
in a fragmented storage device.

---

#### 4. Storage Creation Test (`test_storage_creation`)

**Purpose**: Validates storage system initialization

**What it tests:**
- Storage descriptor creation and validation
- Magic number and version verification
- Configuration parameter validation
- Resource allocation and setup

**Test scenario:**
```c
kes_storage_config_t config = {
    .device_path = "/tmp/test_storage",
    .device_size = 10 * 1024 * 1024,  // 10MB
    .block_size = 8192,               // 8KB blocks
    .flags = KES_STORAGE_CREATE
};
kes_storage_create(&config, &storage);
```

**Real-world importance**: Like formatting a new hard drive - establishes 
the fundamental storage structure that all other operations depend on.

---

#### 5. Storage Open/Close Test (`test_storage_open_close`)

**Purpose**: Validates storage persistence and session management

**What it tests:**
- Reopening existing storage
- Metadata persistence across sessions
- Corruption detection and recovery
- Proper resource cleanup

**Test scenario:**
```c
// Create storage and close it
kes_storage_create(&config, &storage);
kes_storage_close(storage);

// Reopen and verify metadata intact
kes_storage_open("/tmp/test_storage", 0, &storage);
// Verify magic numbers, block counts, etc. preserved
```

**Real-world importance**: Like safely closing and reopening a database - 
ensures data integrity survives program restarts and system reboots.

---

#### 6. Extent Allocation Test (`test_extent_allocation`)

**Purpose**: Validates block group allocation and management

**What it tests:**
- Multi-extent allocation
- Unique ID assignment
- Allocation statistics tracking
- Memory leak prevention

**Test scenario:**
```c
// Allocate 5 extents of 10 blocks each
for (int i = 0; i < 5; i++) {
    kes_extent_request_t request = {.block_count = 10};
    kes_extent_allocate(storage, &request, &extents[i]);
    // Verify unique IDs: extent[i].extent_id > previous
}
```

**Real-world importance**: Like allocating warehouse space for different 
products - must track multiple allocations without conflicts or leaks.

---

#### 7. Extent I/O Test (`test_extent_io`)

**Purpose**: Validates data read/write operations with integrity

**What it tests:**
- Writing data to allocated extents
- Reading data back with verification
- Offset-based operations
- Data corruption detection

**Test scenario:**
```c
char write_data[] = "Hello, KES! Test data for extent I/O.";
kes_extent_write(storage, &extent, write_data, 
                 strlen(write_data) + 1, 0);

char read_data[100];
kes_extent_read(storage, &extent, read_data, 
                strlen(write_data) + 1, 0);

// Verify: strcmp(write_data, read_data) == 0
```

**Real-world importance**: The core of any storage system - writing files 
to disk and reading them back must be byte-perfect.

---

#### 8. Storage Persistence Test (`test_storage_persistence`)

**Purpose**: Validates data survival across system restarts

**What it tests:**
- Data persistence after storage closure
- Recovery from clean shutdown
- Cross-session data integrity
- Metadata consistency

**Test scenario:**
```c
// Session 1: Write data and close
{
    kes_storage_create(&config, &storage);
    kes_extent_allocate(storage, &request, &extent);
    kes_extent_write(storage, &extent, test_data, size, 0);
    kes_storage_close(storage);  // Shutdown
}

// Session 2: Reopen and verify data survived
{
    kes_storage_open(device_path, 0, &storage);
    kes_extent_read(storage, &extent, read_data, size, 0);
    // Verify: data matches exactly
}
```

**Real-world importance**: Like saving a document, rebooting your 
computer, and finding the document unchanged - fundamental reliability.

---

#### 9. Utility Functions Test (`test_utility_functions`)

**Purpose**: Validates supporting functions and calculations

**What it tests:**
- Version information retrieval
- Error message generation
- Block/byte size calculations
- Configuration validation

**Test scenario:**
```c
// Test version info
kes_get_version(&major, &minor, &patch);
assert(major == 1 && minor == 0 && patch == 0);

// Test calculations
uint32_t blocks = kes_calculate_blocks_needed(8192, 4096);
assert(blocks == 2); // 8KB data needs 2 4KB blocks
```

**Real-world importance**: Like testing a calculator's basic functions - 
ensures all supporting utilities work correctly.

---

### Bitmap Full Tests (`test_kes_bitmap_full.c`) - 10/10 Passing

One direct test per `kes_bitmap.h` function -- create/destroy, set,
clear, test, set_range, clear_range, find_free, get_stats, load, save
-- satisfying `KES_HARDENING_PLAN.md` §6.A's per-function coverage
requirement for the bitmap layer.

---

### Storage Full Tests (`test_kes_storage_full.c`) - 15/15 Passing

One direct test per `kes_storage.h` function (create/open/close/sync,
extent allocate/free/read/write, get_stats/get_descriptor, the
utility functions) plus targeted edge cases, including
`test_kes_extent_read_write_32bit_overflow` (the storage-layer
`start_block * block_size` overflow check).

---

### Cache Tests (`test_kes_cache.c`) - 23/23 Passing

Edge-case and concurrency coverage for the cache layer, beyond the
one-test-per-function coverage in `test_kes_cache_full.c` below. This
document previously claimed "4/6 Passing" with two "known issues" in
basic operations and concurrent access -- that was stale; every test
in this file currently passes. Notable tests:

- **Cache Lifecycle**: creation, `kes_cache_start()`/`kes_cache_stop()`
  (including `Cache Start Background Flush`, which proves automatic
  flushing actually happens, and `Cache Start Rejects Zero Threads`).
- **Concurrent Miss No Duplicate Entry**: proves the P0 race fix
  (concurrent cache-miss on the same extent ID no longer creates
  duplicate hash-table entries) -- see `PENDING_ITEMS.md`'s "Resolved"
  section.
- **Concurrent Access** / **Concurrent Sync vs Get/Put**: multi-thread
  regression tests against a `max_entries`-constrained cache, run
  clean under ASan+UBSan and TSan during development (5+ consecutive
  runs each) -- these are the tests that actually found the
  concurrency hazards documented in `src/kes_cache.c`'s
  `try_evict_entry_locked()` doc comment.
- **Cache Eviction Respects Max Entries** / **Cache Pinned Entries
  Never Evicted** / **Cache Get Extent Busy When Full And Pinned**:
  capacity enforcement and eviction correctness.
- **Cache Create Rejects Unimplemented Policy**: confirms
  `KES_CACHE_LFU`/`KES_CACHE_CUSTOM` are rejected with `NULL` while
  `KES_CACHE_LRU` is accepted.
- **Cache Sync** / **Cache Sync Keeps Referenced Entries** /
  **Cache Invalidate** / **Cache Invalidate Discards Dirty Data** /
  **Cache Reset Stats**: the four functions added in Phase 3 --
  semantics per `PENDING_ITEMS.md`'s "Resolved" section.
- No-read/write-extent-callback paths and ref_count-leak-on-load-
  failure tests.

---

### Cache Full Tests (`test_kes_cache_full.c`) - 12/12 Passing

One direct test per `kes_cache.h` function, including
`test_kes_extent_hash`/`test_kes_extent_equal` (the hash/equality
utility functions) -- the per-function counterpart to
`test_kes_cache.c`'s edge-case/concurrency coverage above.

---

### Multi-Process Test (`test_kes_multiprocess.c`) - 1/1 Passing

`Cross Process Sync IO` -- a real `fork()` (two OS processes, not
threads), each independently calling `kes_storage_open()` on the same
backing file, taking turns writing/reading a distinguishable payload
through `kes_extent_write()`/`kes_extent_read()` with turns strictly
ordered by two POSIX semaphores in an anonymous `MAP_SHARED` mapping.
This is the only test binary that exercises the storage layer's raw
on-disk I/O path across independent processes rather than threads
within one process -- see `AGENTS.md`'s "Ground truth" section for
exactly what this does and does not prove (it deliberately never lets
the two processes race each other; unsynchronized concurrent access
remains an open, documented, unguarded gap).

---

## Example Program (`example_kes_usage.c`)

### Purpose

Demonstrates complete real-world KES usage in a single program that 
showcases every major feature and typical usage patterns.

### Program Flow

#### Phase 1: Storage System Setup
```c
// Configure 50MB storage with 8KB blocks
kes_storage_config_t config = {
    .device_path = "/tmp/example_storage.kes",
    .device_size = 50 * 1024 * 1024,  // 50MB
    .block_size = KES_DEFAULT_BLOCK_SIZE,  // 8KB
    .flags = KES_STORAGE_CREATE | KES_STORAGE_TRUNCATE,
    .strategy = KES_ALLOC_FIRST_FIT
};

kes_storage_create(&config, &storage);
```

**Output demonstrates:**
```
Storage Information:
  Total blocks: 6400
  Free blocks:  6398 (100.0%)
  Used blocks:  0 (0.0%)
```

#### Phase 2: Multiple Extent Allocation
```c
// Allocate 4 extents of 8 blocks each (64KB total)
kes_extent_request_t request = {.block_count = 8};
for (int i = 0; i < 4; i++) {
    kes_extent_allocate(storage, &request, &extents[i]);
}
```

**Output demonstrates:**
```
Allocated extent 1: start_block=0, count=8, id=2
Allocated extent 2: start_block=8, count=8, id=3
Allocated extent 3: start_block=16, count=8, id=4
Allocated extent 4: start_block=24, count=8, id=5

Storage Information:
  Free blocks:  6366 (99.5%)
  Used blocks:  32 (0.5%)
  Allocated extents: 4
  Fragmentation: 9%
```

#### Phase 3: Data I/O Operations
```c
// Write unique data to each extent
for (int i = 0; i < 4; i++) {
    snprintf(data, sizeof(data), 
        "This is test data for extent %d (ID: %lu). "
        "The extent starts at block %lu and contains %u blocks.",
        i + 1, extents[i].extent_id, 
        extents[i].start_block, extents[i].block_count);
    
    kes_extent_write(storage, &extents[i], data, 
                     strlen(data) + 1, 0);
}

// Read all data back and verify
for (int i = 0; i < 4; i++) {
    kes_extent_read(storage, &extents[i], read_buffer, 
                    data_size, 0);
    printf("  Extent %d data: %.100s...\n", i + 1, read_buffer);
}
```

**Output demonstrates:**
```
Extent 1 data: This is test data for extent 1 (ID: 2). The extent 
               starts at block 0 and contains 8 blocks...
Extent 2 data: This is test data for extent 2 (ID: 3). The extent 
               starts at block 8 and contains 8 blocks...
```

#### Phase 4: Memory Management and Fragmentation
```c
// Free extents 2 and 4 to create fragmentation
kes_extent_free(storage, &extents[1]);  // Free extent 2
kes_extent_free(storage, &extents[3]);  // Free extent 4
```

**Output demonstrates:**
```
Storage Information:
  Free blocks:  6382 (99.7%)
  Used blocks:  16 (0.2%)
  Allocated extents: 2
  Fragmentation: 6%
```

#### Phase 5: Large Allocation with Reuse
```c
// Allocate larger extent that efficiently reuses freed space
kes_extent_request_t large_request = {.block_count = 20}; // 160KB
kes_extent_allocate(storage, &large_request, &large_extent);
```

**Output demonstrates:**
```
Large extent: start_block=24, count=20, id=6
Large extent size: 163840 bytes (160.0 KB)

Storage Information:
  Free blocks:  6362 (99.4%)
  Used blocks:  36 (0.6%)
  Allocated extents: 3
  Fragmentation: 5%
```

**Key insight**: The large allocation reuses the freed space starting 
at block 24, demonstrating efficient space management.

#### Phase 6: System Operations and Cleanup
```c
// Force synchronization to storage
kes_storage_sync(storage);

// Get version information
kes_get_version(&major, &minor, &patch);
printf("KES Library Version: %u.%u.%u\n", major, minor, patch);

// Clean shutdown with resource cleanup
kes_storage_close(storage);
```

**Output demonstrates:**
```
KES Library Version: 1.0.0
✅ Example completed successfully!
```

### Real-World Applications Demonstrated

#### File System Implementation
- Each extent represents a file or file fragment
- Shows allocation, I/O, and deallocation patterns
- Demonstrates fragmentation handling

#### Database Storage Engine
- Each extent could be a database page
- Shows transaction-like operations
- Demonstrates persistence requirements

#### Object Storage System
- Each extent could be an object chunk
- Shows multi-object management
- Demonstrates space efficiency

#### Virtual Memory System
- Each extent could be a memory region
- Shows allocation/deallocation patterns
- Demonstrates memory pressure handling

### Performance Insights from Example

**Storage Efficiency:**
- 50MB storage = 6,400 blocks of 8KB each
- Metadata overhead: ~0.03% (2 blocks for descriptor/bitmap)
- Space utilization: >99.9% for user data

**Fragmentation Management:**
- Initial allocation: 0% fragmentation
- After mixed operations: 5% fragmentation  
- Efficient space reuse demonstrated

**Memory Safety:**
- All allocations properly tracked
- Clean shutdown with resource cleanup
- No memory leaks demonstrated

## Running the Tests and Examples

### Prerequisites
```bash
# Extract package
tar -xzf kanek_extents_storage-complete.tar.gz
cd kanek_extents_storage/

# Build everything
make all
```

### Test Execution

#### Core Tests (Recommended)
```bash
# Run guaranteed-passing core functionality tests
make test-core

# Expected output:
# === KES Minimal Implementation Test Suite ===
# Tests run: 9, Tests passed: 9, Tests failed: 0
# ✅ All tests PASSED!
```

#### All Tests (6 binaries)
```bash
# Run the complete test suite
make test

# Expected (verified baseline as of this writing -- rerun and
# reconcile if this number ever looks different, per PENDING_ITEMS.md):
# 70/70 across test_kes_minimal (9), test_kes_bitmap_full (10),
# test_kes_storage_full (15), test_kes_cache (23),
# test_kes_cache_full (12), test_kes_multiprocess (1)
```

#### Individual Test Execution
```bash
# Build all test binaries
make tests

# Run any one binary manually the same way "make test" does --
# copy to /tmp first (existing, intentional Makefile behavior)
for t in test_kes_minimal test_kes_bitmap_full test_kes_storage_full \
         test_kes_cache test_kes_cache_full test_kes_multiprocess; do
    cp build/tests/$t /tmp/$t
    chmod +x /tmp/$t
    /tmp/$t
done
```

### Example Execution

#### Using Makefile
```bash
# Build and run example automatically
make run-example

# Expected output: Complete demo of all KES features
```

#### Manual Execution
```bash
# Build example
make examples

# Run manually
cp build/examples/example_kes_usage /tmp/example
chmod +x /tmp/example
/tmp/example
```

## Test Coverage Analysis

### Functional Coverage
- ✅ **Block allocation/deallocation**: 100%
- ✅ **Data I/O with integrity**: 100%  
- ✅ **Storage persistence**: 100%
- ✅ **Error handling**: 100%
- ✅ **Resource management**: 100%

### Scenario Coverage
- ✅ **Fresh storage creation**: Covered
- ✅ **Storage reopening**: Covered
- ✅ **Fragmented allocation**: Covered
- ✅ **Large vs small extents**: Covered
- ✅ **System restart recovery**: Covered

### Platform Coverage

**Only actually verified on Linux/WSL2** (`AGENTS.md`'s "Building and
Testing" section) -- the code is portable C99/POSIX with no
platform-specific paths, but macOS and ARM64 are not independently
tested by this project's own suite; treat those as "should work,"
not "covered by CI" (there is no CI -- see
`PROJECT_OVERVIEW_KES.md`'s corrected "Continuous Integration"
section).
- ✅ **Memory constraints**: exercised across the test binaries'
  various cache/storage sizes.
- ✅ **Storage sizes**: from small (`test_kes_minimal`) to the
  larger allocation-exhaustion scenarios in `test_kes_storage_full.c`.

## Troubleshooting

### Common Test Issues

#### Permission Denied Errors
**Problem**: Tests built but won't execute
**Solution**: Copy to /tmp directory with executable permissions
```bash
cp build/tests/test_kes_minimal /tmp/test && /tmp/test
```

#### Storage Creation Failures
**Problem**: Cannot create test storage files
**Solution**: Ensure write permissions to /tmp directory
```bash
ls -la /tmp/  # Should show write permissions
```

#### Memory Allocation Errors
**Problem**: Tests fail with "Out of memory"
**Solution**: Check available system memory
```bash
free -h  # Should show available RAM
```

### Example Program Issues

#### Storage File Access
**Problem**: Cannot create /tmp/example_storage.kes
**Solution**: Check /tmp directory permissions and space
```bash
df -h /tmp  # Check available space
touch /tmp/test_file  # Test write permissions
```

#### Large Extent Allocation Failures  
**Problem**: Large extent allocation fails
**Solution**: Increase storage size in example configuration

## Validation and Quality Assurance

### Code Quality
- **Compiler warnings**: `-Wall -Wextra -Werror` -- any new warning
  fails the build, so zero warnings is enforced, not just observed.
- **Static analysis**: **not run.** There is no `cppcheck`/
  `clang-analyzer` step anywhere in this repository -- see
  `PROJECT_OVERVIEW_KES.md`'s corrected "Quality Assurance" section.
- **Memory safety**: 0 leaks/errors under `make valgrind` and
  `make asan` as of the last verified check-in (`PENDING_ITEMS.md`).
- **Thread safety**: verified via `make tsan` (0 races as of the last
  check-in) plus the dedicated concurrency-regression tests in
  `test_kes_cache.c` and the fork()-based test in
  `test_kes_multiprocess.c`.

### Performance Characteristics

Not benchmarked (see `PROJECT_OVERVIEW_KES.md`'s corrected
"Performance Benchmarks" section -- no benchmark suite exists in this
repository). One correction worth calling out here specifically:
`kes_bitmap_find_free()` (`src/kes_bitmap.c`) is a **linear scan**, not
O(1) -- "O(1) for first-fit" was never accurate. Best-fit is not
implemented at all, so there is nothing to give a complexity for.

### Reliability Metrics
- **Test success rate**: 70/70 (100%) across all 6 binaries as of the
  last verified `make test` run -- see the "Standing note" at the top
  of this document and `PENDING_ITEMS.md`.
- **Data integrity**: verified via byte-level read/write round-trips
  in `test_kes_storage_full.c`/`test_kes_cache_full.c`.
- **Recovery success**: verified for the clean-shutdown-then-reopen
  case in `test_kes_storage_full.c`; crash-consistency (mid-write
  failure, truncated/corrupted descriptor) is **not yet covered** --
  see `plan_phase5.md` Track A.5.
- **Resource cleanup**: 0 leaks verified via `make valgrind`.

## Conclusion

The KES test suite and examples provide substantial validation of the
storage and cache layers as they actually exist today -- 70/70 tests
passing across 6 binaries as of the last verified `make test` run,
clean under ASan+UBSan/TSan/Valgrind (`make check-all`) as of the last
verified check-in (`PENDING_ITEMS.md`). This document previously
described only the 9-test `test_kes_minimal` suite as "the" test
suite; that framing undercounted the actual current coverage by a
wide margin and has been corrected throughout this file.

The example program showcases real-world usage patterns that directly 
translate to file systems, databases, and other storage applications. 
The combination provides confidence in KES's readiness for production 
deployment.
