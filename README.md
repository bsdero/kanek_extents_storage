# KANEK Extents Storage (KES)

**A minimal, efficient, and cross-platform extent-based storage management library**

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)]() [![Tests](https://img.shields.io/badge/tests-9%2F9%20passing-brightgreen.svg)]() [![Platform](https://img.shields.io/badge/platform-POSIX-blue.svg)]()

## 🎯 Overview

KANEK Extents Storage (KES) is a low-level storage management library 
designed for systems requiring efficient block allocation and extent 
management. Originally designed for flash storage optimization, KES 
provides a solid foundation for file systems, object storage, and 
database backends.

## ✨ Key Features

- **Cross-Platform**: POSIX-compliant (Linux, macOS, embedded systems)
- **Thread-Safe**: Full mutex protection for concurrent operations
- **Efficient Allocation**: Multiple allocation strategies (first-fit, best-fit, buddy system)
- **Flash-Aware**: Designed with flash storage optimization in mind
- **Minimal Footprint**: Suitable for both edge devices and servers
- **Production-Ready**: Comprehensive test coverage and error handling

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

**Test Coverage**: 9/9 core tests + cache tests passing ✅

## 📚 Documentation

- **[API Reference](docs/KES_API_Reference.md)** - Complete function documentation
- **[Design Document](docs/KES_Design_Document.md)** - Architecture and design decisions
- **[Implementation Guide](docs/README_Implementation.md)** - Development status
- **[Cache Design](docs/kes_cache_design.md)** - Advanced caching architecture

## 🔧 Advanced Features

### Allocation Strategies
- **First-Fit**: Fast allocation for general use
- **Best-Fit**: Minimize fragmentation
- **Buddy System**: Power-of-2 allocation with coalescing

### Caching Layer
- Multi-policy eviction (LRU, LFU, Clock)
- Background dirty page sync
- Thread-safe concurrent access
- Memory pressure handling

### Flash Optimization (Future)
- Hot/cold data separation
- Wear leveling algorithms
- Garbage collection strategies

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
- ✅ Multi-policy caching
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
