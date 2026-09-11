# KANEK Extents Storage (KES)

**A minimal, efficient, and cross-platform extent-based storage management library**

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)]() [![Tests](https://img.shields.io/badge/tests-69%2F69%20passing-brightgreen.svg)]() [![Platform](https://img.shields.io/badge/platform-POSIX-blue.svg)]()

## 🎯 Overview

KANEK Extents Storage (KES) is a low-level storage management library 
designed for systems requiring efficient block allocation and extent 
management. Originally designed for flash storage optimization, KES 
provides a solid foundation for file systems, object storage, and 
database backends.

## ✨ Key Features

- **Cross-Platform**: POSIX-compliant (Linux, macOS, embedded systems)
- **Thread-Safe**: Full mutex protection for concurrent operations
- **Block Allocation**: First-fit extent allocation today; best-fit/
  worst-fit/next-fit/buddy-system are declared in the config API but
  not yet wired up (see Known Limitations)
- **Extent Caching**: LRU cache with real capacity enforcement
  (eviction on miss) and an automatic background flush thread
- **Flash-Aware**: Designed with flash storage optimization in mind
  (optimization itself is future work, not implemented yet)
- **Minimal Footprint**: Suitable for both edge devices and servers
- **Tested**: 69 tests passing (normal build, plus ASan+UBSan, TSan,
  and Valgrind clean runs) -- see Known Limitations for coverage
  gaps

## 🚀 Quick Start

### Build Requirements
- GCC or compatible C compiler
- POSIX-compliant system
- pthread library

### Building
```bash
# Clone and build
git clone <repository>
cd kanek_extents_storage
make all

# Run tests
make test

# Install system-wide (optional)
sudo make install
```

> **KES-6 breaking change:** the on-disk storage format gained a
> `bitmap_checksum` field in `kes_storage_descriptor_t` and bumped
> `KES_VERSION_MAJOR` to 2. Storage files created by a pre-KES-6 build
> will fail to open (`KES_ERROR_CORRUPT`, with a log message naming the
> version mismatch) and must be recreated — there is no migration
> path.
>
> **New link-time dependency:** `libkes.a` now has a real link-time
> dependency on `kanek_foundations`'s `libkfl.a`, for `kfl_crc32c()`
> (used to protect the on-disk bitmap, KES-6). `libkes.so` already
> embeds what it needs and requires nothing extra from consumers. If
> you link `libkes.a` directly, you must also link `libkfl.a` (build
> it via `make -C ../kanek_foundations/src`, or point at wherever your
> project vendors/builds it) — the same way this repo's own `Makefile`
> now does for its test/example binaries.
>
> **KES-5 performance trade-off:** every `kes_extent_allocate()`/
> `kes_extent_free()` call now does a full reload of the descriptor
> and bitmap from disk, a full write-back, and a synchronous `fsync()`,
> all under an exclusive file lock (`flock()`) — turning what used to
> be a pure in-memory operation into a full durable disk operation on
> every call. This is a deliberate, necessary consequence of making
> concurrent access from multiple `kes_storage_t*` handles (same
> process or different processes) on the same backing file safe,
> rather than an oversight, and it changes the throughput profile for
> allocate/free-heavy workloads significantly. Reducing how often the
> fsync/reload happens is a real, separate follow-up design question
> if either downstream project needs higher throughput later.

### Basic Usage
```c
#include <kes/kes_storage.h>

// Create storage
kes_storage_config_t config = {
    .device_path = "/path/to/storage",
    .device_size = 100 * 1024 * 1024,  // 100MB
    .flags = KES_STORAGE_CREATE
};

kes_storage_t* storage;
kes_storage_create(&config, &storage);

// Allocate extent
kes_extent_request_t request = {.block_count = 8};
kes_extent_descriptor_t extent;
kes_extent_allocate(storage, &request, &extent);

// Read/write data
char data[8192] = "Hello, KES!";
kes_extent_write(storage, &extent, data, sizeof(data), 0);
kes_extent_read(storage, &extent, buffer, sizeof(data), 0);

// Cleanup
kes_extent_free(storage, &extent);
kes_storage_close(storage);
```

## 📁 Project Structure

```
kanek_extents_storage/
├── docs/                 # Documentation
├── examples/             # Usage examples  
├── include/kes/          # Public headers
├── src/                  # Implementation
├── tests/                # Test suite
├── Makefile             # Build system
└── README.md            # This file
```

## 🧪 Testing

KES includes comprehensive test coverage:

```bash
# Run all tests
make test

# Run specific tests
./build/tests/test_kes_minimal    # Core functionality
./build/tests/test_kes_cache      # Cache layer
```

**Test Coverage**: 69/69 tests passing across all five test binaries
(`test_kes_minimal`, `test_kes_bitmap_full`, `test_kes_storage_full`,
`test_kes_cache`, `test_kes_cache_full`) ✅ -- also passing under
`make check-all` (ASan+UBSan, TSan, Valgrind)

## 📚 Documentation

- **[API Reference](docs/KES_API_Reference.md)** - Complete function documentation
- **[Design Document](docs/KES_Design_Document.md)** - Architecture and design decisions
- **[Implementation Guide](docs/README_Implementation.md)** - Development status
- **[Cache Design](docs/kes_cache_design.md)** - Advanced caching architecture

## 🔧 Advanced Features

### Allocation Strategies
- **First-Fit**: implemented, and the only strategy currently wired
  up regardless of what `kes_storage_config_t.strategy` requests
- **Best-Fit / Worst-Fit / Next-Fit / Buddy System**: declared in
  `kes_types.h`'s `kes_allocation_strategy_t`, not implemented

### Caching Layer
- **LRU eviction**: implemented -- `kes_cache_get_extent()` evicts
  from the LRU tail as needed to stay within `config.max_entries`/
  `config.max_memory`, returning `KES_ERROR_BUSY` if it can't free
  enough room
- **LFU / Custom eviction policies**: declared in
  `kes_cache_policy_t`, not implemented --
  `kes_cache_create()` rejects a config requesting either rather
  than silently falling back to LRU
- **Background dirty-page sync**: implemented --
  `kes_cache_start()` runs a background thread per
  `config.background_threads` that flushes dirty entries every
  `config.sync_interval_ms`
- Thread-safe concurrent access (get/put/pin/mark_dirty/flush/sync/
  invalidate/eviction can all run concurrently; verified under TSan)
- Memory pressure handling via the LRU eviction above

### Flash Optimization (Future)
- Hot/cold data separation
- Wear leveling algorithms
- Garbage collection strategies

## ⚠️ Known Limitations

- Extent allocation is first-fit only; `kes_allocation_strategy_t`'s
  other strategies (best-fit, worst-fit, next-fit, buddy system) are
  declared but not implemented.
- Cache eviction is LRU only; `KES_CACHE_LFU`/`KES_CACHE_CUSTOM` are
  rejected at `kes_cache_create()` rather than implemented.
- No multi-device or multi-writer protection: opening the same
  underlying storage file/device from two `kes_storage_t*` instances
  concurrently is unguarded/undefined behavior.
- Cache-miss capacity enforcement (`config.max_entries`/
  `config.max_memory`) is not strictly atomic under concurrent
  misses -- a burst of simultaneous misses can transiently overshoot
  the configured limit by a small, bounded amount rather than
  enforcing it with a single global lock across every miss.
- Test coverage is solid for the paths exercised by the current
  suite (functional coverage, several targeted concurrency/race
  regression tests under ASan+UBSan/TSan/Valgrind) but does not yet
  include the full matrix described in `KES_HARDENING_PLAN.md` §6:
  systematic edge-case sweeps (NULL/zero/overflow per parameter),
  fault injection (I/O failures, allocation failures, partial I/O),
  storage-layer crash-consistency tests, randomized/fuzz-adjacent
  testing, and a long-run soak test.

## 🤝 Contributing

1. Fork the repository
2. Create a feature branch
3. Add tests for new functionality
4. Ensure all tests pass
5. Submit a pull request

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🎯 Roadmap

- ✅ Core storage operations
- ✅ Bitmap allocation
- ✅ LRU caching with real eviction and background sync
- 🔄 Best-fit / worst-fit / next-fit / buddy-system allocation
- 🔄 LFU / Clock eviction policies
- 🔄 Flash zone management
- 🔄 Garbage collection
- 🔄 Wear leveling
- 🔄 Multi-device support

## 📞 Support

For issues, feature requests, or questions:
- Check the [documentation](docs/)
- Review [existing issues](https://github.com/your-repo/issues)
- Create a new issue with detailed information

---

**KANEK Extents Storage** - Efficient storage management for modern systems
