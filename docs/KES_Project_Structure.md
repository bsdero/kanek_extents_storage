# KES Project Structure & Organization
## Version 1.0

---

## Complete Directory Tree

```
kes/
├── README.md                          # Project overview and quick start
├── LICENSE                            # Project license (MIT/GPL/BSD)
├── CHANGELOG.md                       # Version history and changes
├── CONTRIBUTING.md                    # Contribution guidelines
├── .gitignore                         # Git ignore patterns
├── Makefile                           # Main build system
├── Kconfig                           # Feature configuration (optional)
├── 
├── docs/                             # Documentation
│   ├── KES_Design_Document.md        # Comprehensive design document
│   ├── KES_API_Reference.md          # Complete API documentation
│   ├── tuning_guide.md               # Performance tuning guide
│   ├── integration_guide.md          # Integration patterns
│   ├── flash_optimization.md         # Flash-specific optimizations
│   ├── troubleshooting.md            # Common issues and solutions
│   └── images/                       # Documentation images
│       ├── architecture_diagram.png
│       ├── storage_layout.png
│       └── zone_diagram.png
├── 
├── include/                          # Public header files
│   └── kes/
│       ├── kes_storage.h             # Main storage API
│       ├── kes_cache.h               # Cache management API
│       ├── kes_bitmap.h              # Bitmap operations API
│       ├── kes_flash.h               # Flash-specific API
│       ├── kes_zone.h                # Zone management API
│       ├── kes_serialization.h       # Descriptor serialization API
│       └── kes_types.h               # Common type definitions
├── 
├── src/                              # Source code implementation
│   ├── common/                       # Common utilities
│   │   ├── kes_common.c              # Common utility functions
│   │   ├── kes_error.c               # Error handling implementation
│   │   ├── kes_memory.c              # Memory management utilities
│   │   ├── kes_threading.c           # Threading primitives
│   │   └── kes_logging.c             # Logging framework
│   │
│   ├── core/                         # Core storage functionality
│   │   ├── kes_storage.c             # Main storage operations
│   │   ├── kes_descriptor.c          # Storage descriptor management
│   │   ├── kes_extent.c              # Extent management
│   │   ├── kes_allocator.c           # Allocation algorithms
│   │   ├── kes_allocator_first_fit.c # First-fit allocator
│   │   ├── kes_allocator_best_fit.c  # Best-fit allocator
│   │   ├── kes_allocator_buddy.c     # Buddy system allocator
│   │   ├── kes_allocator_slab.c      # Slab allocator
│   │   ├── kes_allocator_log.c       # Log-structured allocator
│   │   └── kes_allocator_hybrid.c    # Hybrid allocator
│   │
│   ├── bitmap/                       # Bitmap management
│   │   ├── kes_bitmap.c              # Basic bitmap operations
│   │   ├── kes_bitmap_full.c         # Full memory bitmap strategy
│   │   ├── kes_bitmap_window.c       # Sliding window bitmap strategy
│   │   ├── kes_bitmap_ondemand.c     # On-demand bitmap strategy
│   │   ├── kes_bitmap_compressed.c   # Compressed bitmap strategy
│   │   └── kes_bitmap_hierarchical.c # Hierarchical bitmap strategy
│   │
│   ├── cache/                        # Caching layer
│   │   ├── kes_cache.c               # Main cache implementation
│   │   ├── kes_cache_lru.c           # LRU eviction policy
│   │   ├── kes_cache_lfu.c           # LFU eviction policy
│   │   ├── kes_cache_arc.c           # ARC eviction policy
│   │   ├── kes_cache_hash.c          # Hash table implementation
│   │   └── kes_cache_background.c    # Background cache management
│   │
│   ├── flash/                        # Flash-specific functionality
│   │   ├── kes_flash_common.c        # Common flash operations
│   │   ├── kes_flash_detection.c     # Flash geometry detection
│   │   ├── kes_gc.c                  # Garbage collection
│   │   ├── kes_gc_greedy.c           # Greedy GC algorithm
│   │   ├── kes_gc_cost_benefit.c     # Cost-benefit GC algorithm
│   │   ├── kes_gc_wear_aware.c       # Wear-aware GC algorithm
│   │   ├── kes_wear_leveling.c       # Wear leveling implementation
│   │   ├── kes_wear_dynamic.c        # Dynamic wear leveling
│   │   ├── kes_wear_static.c         # Static wear leveling
│   │   └── kes_flash_zones.c         # Flash zone management
│   │
│   ├── zones/                        # Zone management
│   │   ├── kes_zones.c               # Zone management core
│   │   ├── kes_zone_creation.c       # Zone creation/destruction
│   │   ├── kes_zone_resize.c         # Zone resize operations
│   │   ├── kes_zone_hot.c            # Hot zone management
│   │   ├── kes_zone_warm.c           # Warm zone management
│   │   ├── kes_zone_cold.c           # Cold zone management
│   │   └── kes_multi_device_zone.c   # Multi-device zone support
│   │
│   ├── serialization/                # Descriptor serialization
│   │   ├── kes_serialization.c       # Main serialization interface
│   │   ├── kes_serialize_binary.c    # Binary serialization format
│   │   ├── kes_serialize_json.c      # JSON serialization format
│   │   ├── kes_serialize_validation.c# Schema validation
│   │   └── kes_migration.c           # Version migration support
│   │
│   ├── platforms/                    # Platform-specific code
│   │   ├── linux/
│   │   │   ├── kes_linux_io.c        # Linux I/O operations
│   │   │   ├── kes_linux_flash.c     # Linux flash detection
│   │   │   ├── kes_linux_memory.c    # Linux memory management
│   │   │   └── kes_linux_threading.c # Linux threading
│   │   ├── android/
│   │   │   ├── kes_android_io.c      # Android-specific I/O
│   │   │   ├── kes_android_flash.c   # Android flash handling
│   │   │   └── kes_android_power.c   # Android power management
│   │   ├── macos/
│   │   │   ├── kes_macos_io.c        # macOS I/O operations
│   │   │   └── kes_macos_flash.c     # macOS flash detection
│   │   ├── embedded/
│   │   │   ├── kes_embedded_io.c     # Embedded I/O operations
│   │   │   ├── kes_embedded_memory.c # Memory-constrained operations
│   │   │   └── kes_embedded_power.c  # Power-aware operations
│   │   └── generic/
│   │       ├── kes_generic_io.c      # POSIX I/O fallback
│   │       ├── kes_generic_flash.c   # Generic flash detection
│   │       └── kes_generic_threading.c # pthread implementation
│   │
│   ├── profiles/                     # Hardware profiles
│   │   ├── kes_profile_loader.c      # Profile loading system
│   │   ├── smartphone.c              # Smartphone profile
│   │   ├── tablet.c                  # Tablet profile
│   │   ├── laptop_linux.c            # Linux laptop profile
│   │   ├── laptop_macos.c            # macOS laptop profile
│   │   ├── server_ssd.c              # Server SSD profile
│   │   ├── server_nvme.c             # Server NVMe profile
│   │   ├── embedded_emmc.c           # Embedded eMMC profile
│   │   ├── embedded_nand.c           # Embedded NAND profile
│   │   ├── cloud_vm.c                # Cloud VM profile
│   │   └── custom.c                  # Custom profile template
│   │
│   └── internal/                     # Internal headers (private)
│       ├── kes_internal.h            # Internal common definitions
│       ├── kes_allocator_internal.h  # Allocator internal interface
│       ├── kes_cache_internal.h      # Cache internal interface
│       ├── kes_flash_internal.h      # Flash internal interface
│       ├── kes_zones_internal.h      # Zone internal interface
│       └── kes_threading_internal.h  # Threading internal interface
├── 
├── tests/                            # Test suite
│   ├── unit/                         # Unit tests
│   │   ├── test_storage.c            # Storage operations tests
│   │   ├── test_bitmap.c             # Bitmap operations tests
│   │   ├── test_allocator.c          # Allocation algorithm tests
│   │   ├── test_cache.c              # Cache functionality tests
│   │   ├── test_flash.c              # Flash operations tests
│   │   ├── test_zones.c              # Zone management tests
│   │   ├── test_serialization.c      # Serialization tests
│   │   └── test_profiles.c           # Profile system tests
│   │
│   ├── integration/                  # Integration tests
│   │   ├── test_full_lifecycle.c     # Complete lifecycle tests
│   │   ├── test_concurrency.c        # Multi-threading tests
│   │   ├── test_crash_recovery.c     # Crash recovery tests
│   │   ├── test_platform_compat.c    # Cross-platform compatibility
│   │   ├── test_large_storage.c      # Large storage tests
│   │   └── test_real_devices.c       # Real device tests
│   │
│   ├── performance/                  # Performance tests
│   │   ├── bench_allocation.c        # Allocation performance
│   │   ├── bench_io_throughput.c     # I/O throughput benchmarks
│   │   ├── bench_cache.c             # Cache performance
│   │   ├── bench_flash_gc.c          # Flash GC performance
│   │   ├── bench_memory_usage.c      # Memory usage benchmarks
│   │   └── bench_scalability.c       # Scalability tests
│   │
│   ├── flash_simulation/             # Flash device simulation
│   │   ├── kes_flash_simulator.c     # Flash simulator implementation
│   │   ├── simulator_nand.c          # NAND flash simulation
│   │   ├── simulator_emmc.c          # eMMC simulation
│   │   ├── simulator_nvme.c          # NVMe simulation
│   │   └── simulator_wear.c          # Wear simulation
│   │
│   ├── regression/                   # Regression tests
│   │   ├── test_regression_suite.c   # Automated regression testing
│   │   ├── performance_baseline.c    # Performance baseline tracking
│   │   └── memory_leak_detection.c   # Memory leak detection
│   │
│   └── common/                       # Test utilities
│       ├── test_framework.c          # Test framework implementation
│       ├── test_utilities.c          # Common test utilities
│       ├── test_data_generator.c     # Test data generation
│       └── test_platform_helpers.c   # Platform-specific test helpers
├── 
├── tools/                            # Command-line tools
│   ├── kes-mkfs/                     # Storage formatting tool
│   │   ├── kes_mkfs.c                # Main mkfs implementation
│   │   ├── mkfs_validation.c         # Parameter validation
│   │   ├── mkfs_layout.c             # Layout calculation
│   │   └── mkfs_formatting.c         # Actual formatting operations
│   │
│   ├── kes-info/                     # Storage information tool
│   │   ├── kes_info.c                # Main info tool
│   │   ├── info_display.c            # Information display formatting
│   │   └── info_analysis.c           # Storage analysis
│   │
│   ├── kes-fsck/                     # Storage check and repair
│   │   ├── kes_fsck.c                # Main fsck implementation
│   │   ├── fsck_validation.c         # Storage validation
│   │   ├── fsck_repair.c             # Repair operations
│   │   └── fsck_reporting.c          # Report generation
│   │
│   ├── kes-defrag/                   # Defragmentation tool
│   │   ├── kes_defrag.c              # Main defragmentation
│   │   ├── defrag_analysis.c         # Fragmentation analysis
│   │   ├── defrag_strategies.c       # Defragmentation strategies
│   │   └── defrag_progress.c         # Progress tracking
│   │
│   ├── kes-benchmark/                # Benchmarking tool
│   │   ├── kes_benchmark.c           # Main benchmark tool
│   │   ├── benchmark_suites.c        # Benchmark test suites
│   │   ├── benchmark_reporting.c     # Results reporting
│   │   └── benchmark_analysis.c      # Performance analysis
│   │
│   ├── kes-simulator/                # Flash simulation tool
│   │   ├── kes_simulator.c           # Main simulator interface
│   │   ├── simulator_config.c        # Simulator configuration
│   │   └── simulator_ui.c            # User interface
│   │
│   ├── kes-tune/                     # Runtime tuning tool
│   │   ├── kes_tune.c                # Main tuning interface
│   │   ├── tune_analysis.c           # Performance analysis
│   │   ├── tune_recommendations.c    # Tuning recommendations
│   │   └── tune_profiles.c           # Profile management
│   │
│   └── common/                       # Common tool utilities
│       ├── tool_common.c             # Common tool functionality
│       ├── tool_parsing.c            # Command-line parsing
│       ├── tool_output.c             # Output formatting
│       └── tool_validation.c         # Input validation
├── 
├── examples/                         # Usage examples
│   ├── basic_usage/                  # Basic usage examples
│   │   ├── simple_storage.c          # Simple storage operations
│   │   ├── extent_management.c       # Basic extent operations
│   │   └── cache_usage.c             # Basic cache usage
│   │
│   ├── advanced/                     # Advanced examples
│   │   ├── flash_optimized.c         # Flash-optimized usage
│   │   ├── multi_threaded.c          # Multi-threaded usage
│   │   ├── external_descriptor.c     # External descriptor management
│   │   ├── custom_allocator.c        # Custom allocation strategies
│   │   └── zone_management.c         # Zone management examples
│   │
│   ├── integration/                  # Integration examples
│   │   ├── fuse_filesystem.c         # FUSE file system integration
│   │   ├── database_backend.c        # Database integration
│   │   ├── object_storage.c          # Object storage implementation
│   │   └── distributed_storage.c     # Distributed storage example
│   │
│   └── platform_specific/            # Platform-specific examples
│       ├── android_app/              # Android app integration
│       ├── ios_app/                  # iOS app integration
│       ├── linux_kernel_module/      # Linux kernel module
│       └── embedded_system/          # Embedded system example
├── 
├── scripts/                          # Build and utility scripts
│   ├── build/                        # Build scripts
│   │   ├── configure.sh              # Build configuration script
│   │   ├── cross_compile.sh          # Cross-compilation helper
│   │   ├── profile_setup.sh          # Profile configuration
│   │   └── feature_selection.sh      # Feature selection helper
│   │
│   ├── testing/                      # Testing scripts
│   │   ├── run_tests.sh              # Test execution script
│   │   ├── run_benchmarks.sh         # Benchmark execution
│   │   ├── regression_test.sh        # Regression testing
│   │   ├── memory_check.sh           # Memory leak checking
│   │   └── coverage_analysis.sh      # Code coverage analysis
│   │
│   ├── packaging/                    # Packaging scripts
│   │   ├── create_release.sh         # Release packaging
│   │   ├── debian_package.sh         # Debian package creation
│   │   ├── rpm_package.sh            # RPM package creation
│   │   └── docker_build.sh           # Docker container build
│   │
│   └── utilities/                    # Utility scripts
│       ├── code_style_check.sh       # Code style validation
│       ├── header_check.sh           # Header file validation
│       ├── dependency_check.sh       # Dependency validation
│       └── documentation_build.sh    # Documentation generation
├── 
├── third_party/                      # Third-party dependencies
│   ├── json_parser/                  # JSON parsing library (if needed)
│   ├── compression/                  # Compression libraries
│   └── test_framework/               # Test framework dependencies
├── 
├── ci/                               # Continuous Integration
│   ├── github_actions/               # GitHub Actions workflows
│   │   ├── build_test.yml            # Build and test workflow
│   │   ├── cross_platform.yml       # Cross-platform testing
│   │   ├── performance.yml           # Performance regression
│   │   └── release.yml               # Release automation
│   │
│   ├── jenkins/                      # Jenkins configuration
│   │   ├── Jenkinsfile               # Jenkins pipeline
│   │   └── build_matrix.groovy       # Build matrix configuration
│   │
│   └── docker/                       # Docker configurations
│       ├── Dockerfile.ubuntu         # Ubuntu build environment
│       ├── Dockerfile.alpine         # Alpine build environment
│       ├── Dockerfile.android        # Android build environment
│       └── docker-compose.yml        # Multi-container setup
├── 
└── build/                            # Build output directory (generated)
    ├── debug/                        # Debug build artifacts
    ├── release/                      # Release build artifacts
    ├── coverage/                     # Coverage reports
    ├── documentation/                # Generated documentation
    ├── packages/                     # Generated packages
    └── tests/                        # Test executables and results
```

---

## Build System Design

### Main Makefile Structure

```makefile
# kes/Makefile - Main build system
PROJECT_NAME := libkes
VERSION := 1.0.0

# Profile and feature selection
PROFILE ?= auto
MEMORY_BUDGET ?= medium
BUILD_TYPE ?= release

# Include build configuration files
include scripts/build/profile_detection.mk
include scripts/build/platform_detection.mk
include scripts/build/feature_selection.mk
include scripts/build/compiler_setup.mk
include scripts/build/source_selection.mk

# Default target
.PHONY: all
all: lib tools tests

# Core build targets
.PHONY: lib shared tools tests docs
lib: $(STATIC_LIB)
shared: $(SHARED_LIB) 
tools: $(TOOLS_TARGETS)
tests: $(TEST_TARGETS)

# Profile-specific builds
.PHONY: smartphone tablet laptop server embedded
smartphone:
	$(MAKE) PROFILE=smartphone MEMORY_BUDGET=small
tablet:
	$(MAKE) PROFILE=tablet MEMORY_BUDGET=medium
laptop:
	$(MAKE) PROFILE=laptop_linux MEMORY_BUDGET=medium
server:
	$(MAKE) PROFILE=server_nvme MEMORY_BUDGET=large
embedded:
	$(MAKE) PROFILE=embedded_nand MEMORY_BUDGET=tiny

# Installation and packaging
.PHONY: install uninstall package
install: all
	$(SCRIPTS_DIR)/packaging/install.sh $(INSTALL_PREFIX)

package:
	$(SCRIPTS_DIR)/packaging/create_release.sh $(VERSION)

# Cleaning
.PHONY: clean distclean
clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -rf $(INSTALL_PREFIX)/include/kes
	rm -rf $(INSTALL_PREFIX)/lib/libkes*
```

### Profile Configuration System

#### Profile Detection (`scripts/build/profile_detection.mk`)
```makefile
# Auto-detect profile if not specified
ifeq ($(PROFILE),auto)
    # Check if we're on Android
    ifneq ($(shell getprop ro.build.version.sdk 2>/dev/null),)
        DETECTED_PROFILE := smartphone
    else
        # Check system memory
        TOTAL_MEM_MB := $(shell awk '/MemTotal/ {print int($$2/1024)}' \
                               /proc/meminfo 2>/dev/null || echo 2048)
        
        # Detect based on memory and CPU
        ifeq ($(shell test $(TOTAL_MEM_MB) -lt 2048; echo $$?),0)
            DETECTED_PROFILE := smartphone
        else ifeq ($(shell test $(TOTAL_MEM_MB) -lt 8192; echo $$?),0)
            DETECTED_PROFILE := laptop_linux
        else
            DETECTED_PROFILE := server_ssd
        endif
    endif
    
    PROFILE := $(DETECTED_PROFILE)
    $(info Auto-detected profile: $(PROFILE))
endif

# Load profile-specific configuration
include scripts/build/profiles/$(PROFILE).mk
```

#### Smartphone Profile (`scripts/build/profiles/smartphone.mk`)
```makefile
# Smartphone Profile Configuration
PROFILE_NAME := smartphone
OPTIMIZATION_TARGET := flash_life

# Feature selection - minimize code size and memory usage
ENABLE_FEATURES := storage bitmap allocator cache_basic flash_gc wear_leveling
DISABLE_FEATURES := cache_advanced multi_device compression encryption \
                   async_io debug_tools

# Memory constraints
MEMORY_BUDGET := small
MAX_CACHE_MB := 8
MAX_BITMAP_MB := 2
MAX_THREADS := 1
MAX_ZONES := 4

# Platform preferences
PREFERRED_PLATFORMS := android generic
FALLBACK_PLATFORM := generic

# Compiler optimization flags
PROFILE_CFLAGS := -DKES_PROFILE_SMARTPHONE \
                  -DKES_MEMORY_CONSTRAINED \
                  -DKES_FLASH_OPTIMIZED \
                  -DKES_MAX_CACHE_MB=$(MAX_CACHE_MB) \
                  -DKES_MAX_THREADS=$(MAX_THREADS) \
                  -Os -ffunction-sections -fdata-sections

PROFILE_LDFLAGS := -Wl,--gc-sections

# Platform-specific sources
PROFILE_SOURCES := src/platforms/android/kes_android_power.c \
                  src/platforms/android/kes_android_flash.c \
                  src/profiles/smartphone.c

# Test configuration
PROFILE_TESTS := unit integration
SKIP_TESTS := performance large_storage
```

#### Server NVMe Profile (`scripts/build/profiles/server_nvme.mk`)
```makefile
# Server NVMe Profile Configuration  
PROFILE_NAME := server_nvme
OPTIMIZATION_TARGET := performance

# Feature selection - enable all features for maximum performance
ENABLE_FEATURES := $(ALL_OPTIONAL_FEATURES)
DISABLE_FEATURES := 

# Memory configuration - generous memory allocation
MEMORY_BUDGET := large
MAX_CACHE_MB := 512
MAX_BITMAP_MB := 64
MAX_THREADS := 8
MAX_ZONES := 64

# Platform preferences
PREFERRED_PLATFORMS := linux generic
FALLBACK_PLATFORM := generic

# Compiler optimization flags - maximum performance
PROFILE_CFLAGS := -DKES_PROFILE_SERVER_NVME \
                  -DKES_HIGH_PERFORMANCE \
                  -DKES_FULL_FEATURES \
                  -DKES_MAX_CACHE_MB=$(MAX_CACHE_MB) \
                  -DKES_MAX_THREADS=$(MAX_THREADS) \
                  -O3 -march=native -flto \
                  -funroll-loops -finline-functions

PROFILE_LDFLAGS := -flto

# Platform-specific sources
PROFILE_SOURCES := src/platforms/linux/kes_linux_nvme.c \
                  src/platforms/linux/kes_linux_numa.c \
                  src/profiles/server_nvme.c

# Test configuration - run all tests including performance
PROFILE_TESTS := unit integration performance
SKIP_TESTS := 
```

### Feature Selection System

#### Feature Definition (`scripts/build/feature_selection.mk`)
```makefile
# Define all available features
CORE_FEATURES := storage bitmap allocator

CACHE_FEATURES := cache_basic cache_advanced cache_lru cache_lfu cache_arc

FLASH_FEATURES := flash_gc flash_wear_leveling flash_zones flash_controllers

STORAGE_FEATURES := multi_device zones serialization

UTILITY_FEATURES := compression encryption async_io debug_tools

# Combine all optional features
ALL_OPTIONAL_FEATURES := $(CACHE_FEATURES) $(FLASH_FEATURES) \
                        $(STORAGE_FEATURES) $(UTILITY_FEATURES)

# Generate feature flags based on enabled/disabled lists
FEATURE_FLAGS := $(addprefix -DKES_ENABLE_,$(ENABLE_FEATURES))
FEATURE_FLAGS += $(addprefix -DKES_DISABLE_,$(DISABLE_FEATURES))

# Validate feature dependencies
include scripts/build/feature_validation.mk
```

#### Source File Selection (`scripts/build/source_selection.mk`)
```makefile
# Core sources (always included)
CORE_SOURCES := src/core/kes_storage.c \
               src/core/kes_extent.c \
               src/bitmap/kes_bitmap.c \
               src/common/kes_common.c

# Feature-conditional sources
ifeq ($(filter cache_basic,$(ENABLE_FEATURES)),cache_basic)
    SOURCES += src/cache/kes_cache.c
endif

ifeq ($(filter cache_lru,$(ENABLE_FEATURES)),cache_lru)
    SOURCES += src/cache/kes_cache_lru.c
endif

ifeq ($(filter flash_gc,$(ENABLE_FEATURES)),flash_gc)
    SOURCES += src/flash/kes_gc.c \
               src/flash/kes_gc_greedy.c \
               src/flash/kes_gc_cost_benefit.c
endif

ifeq ($(filter wear_leveling,$(ENABLE_FEATURES)),wear_leveling)
    SOURCES += src/flash/kes_wear_leveling.c \
               src/flash/kes_wear_dynamic.c
endif

# Platform-specific sources
PLATFORM_SOURCES := $(wildcard src/platforms/$(TARGET_PLATFORM)/*.c)
SOURCES += $(PLATFORM_SOURCES)

# Profile-specific sources
SOURCES += $(PROFILE_SOURCES)

# Generate object file list
OBJECTS := $(SOURCES:%.c=$(BUILD_DIR)/%.o)
```

### Memory Budget System

#### Memory Budget Configuration (`scripts/build/memory_budget.mk`)
```makefile
# Memory budget definitions
ifeq ($(MEMORY_BUDGET),tiny)
    MAX_CACHE_MB := 2
    MAX_BITMAP_MB := 0.5
    MAX_METADATA_MB := 0.5
    MAX_THREADS := 1
    MEMORY_FLAGS := -DKES_MEMORY_TINY
endif

ifeq ($(MEMORY_BUDGET),small)
    MAX_CACHE_MB := 8
    MAX_BITMAP_MB := 2
    MAX_METADATA_MB := 2
    MAX_THREADS := 2
    MEMORY_FLAGS := -DKES_MEMORY_SMALL
endif

ifeq ($(MEMORY_BUDGET),medium)
    MAX_CACHE_MB := 64
    MAX_BITMAP_MB := 16
    MAX_METADATA_MB := 8
    MAX_THREADS := 4
    MEMORY_FLAGS := -DKES_MEMORY_MEDIUM
endif

ifeq ($(MEMORY_BUDGET),large)
    MAX_CACHE_MB := 512
    MAX_BITMAP_MB := 64
    MAX_METADATA_MB := 32
    MAX_THREADS := 8
    MEMORY_FLAGS := -DKES_MEMORY_LARGE
endif

# Generate memory limit flags
MEMORY_FLAGS += -DKES_MAX_CACHE_MB=$(MAX_CACHE_MB)
MEMORY_FLAGS += -DKES_MAX_BITMAP_MB=$(MAX_BITMAP_MB)
MEMORY_FLAGS += -DKES_MAX_METADATA_MB=$(MAX_METADATA_MB)
MEMORY_FLAGS += -DKES_MAX_THREADS=$(MAX_THREADS)

CFLAGS += $(MEMORY_FLAGS)
```

---

## Testing Infrastructure

### Test Framework Organization

#### Test Suite Structure
```makefile
# tests/Makefile.tests
TEST_FRAMEWORK_SOURCES := tests/common/test_framework.c \
                         tests/common/test_utilities.c

# Unit tests
UNIT_TEST_SOURCES := $(wildcard tests/unit/test_*.c)
UNIT_TEST_TARGETS := $(UNIT_TEST_SOURCES:tests/unit/%.c=$(BUILD_DIR)/tests/%)

# Integration tests  
INTEGRATION_TEST_SOURCES := $(wildcard tests/integration/test_*.c)
INTEGRATION_TEST_TARGETS := $(INTEGRATION_TEST_SOURCES:tests/integration/%.c=$(BUILD_DIR)/tests/%)

# Performance tests
PERFORMANCE_TEST_SOURCES := $(wildcard tests/performance/bench_*.c)
PERFORMANCE_TEST_TARGETS := $(PERFORMANCE_TEST_SOURCES:tests/performance/%.c=$(BUILD_DIR)/tests/%)

# Build rules for tests
$(BUILD_DIR)/tests/%: tests/unit/%.c $(TEST_FRAMEWORK_SOURCES) $(STATIC_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS) $(LIBS)

$(BUILD_DIR)/tests/%: tests/integration/%.c $(TEST_FRAMEWORK_SOURCES) $(STATIC_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS) $(LIBS)

# Test execution targets
.PHONY: test-unit test-integration test-performance
test-unit: $(UNIT_TEST_TARGETS)
	@echo "Running unit tests..."
	@for test in $(UNIT_TEST_TARGETS); do \
		echo "Running $$test"; \
		$$test || exit 1; \
	done

test-integration: $(INTEGRATION_TEST_TARGETS)
	@echo "Running integration tests..."
	@for test in $(INTEGRATION_TEST_TARGETS); do \
		echo "Running $$test"; \
		$$test || exit 1; \
	done
```

#### Test Framework Implementation (`tests/common/test_framework.c`)
```c
// Simple but effective test framework
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static int test_count = 0;
static int test_passed = 0;
static int test_failed = 0;

// Test assertion macros
#define ASSERT_TRUE(condition) \
    do { \
        test_count++; \
        if (condition) { \
            test_passed++; \
            printf("PASS: %s:%d - %s\n", __FILE__, __LINE__, #condition); \
        } else { \
            test_failed++; \
            printf("FAIL: %s:%d - %s\n", __FILE__, __LINE__, #condition); \
        } \
    } while(0)

#define ASSERT_FALSE(condition) ASSERT_TRUE(!(condition))

#define ASSERT_SUCCESS(result) ASSERT_TRUE((result) == KES_SUCCESS)

#define ASSERT_EQUAL(expected, actual) \
    ASSERT_TRUE((expected) == (actual))

#define ASSERT_MEMORY_EQUAL(expected, actual, size) \
    ASSERT_TRUE(memcmp((expected), (actual), (size)) == 0)

// Test suite management
void test_suite_begin(const char* suite_name) {
    printf("\n=== Test Suite: %s ===\n", suite_name);
    test_count = test_passed = test_failed = 0;
}

void test_suite_end(void) {
    printf("\n=== Results ===\n");
    printf("Tests run: %d\n", test_count);
    printf("Passed: %d\n", test_passed);
    printf("Failed: %d\n", test_failed);
    
    if (test_failed > 0) {
        exit(1);
    }
}
```

### Platform-Specific Testing

#### Android Testing (`tests/platform_specific/android_test.c`)
```c
// Android-specific test considerations
#include <android/log.h>

// Android logging integration
#define ANDROID_LOG_TAG "KES_TEST"
#define LOG_INFO(...) __android_log_print(ANDROID_LOG_INFO, ANDROID_LOG_TAG, __VA_ARGS__)
#define LOG_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, ANDROID_LOG_TAG, __VA_ARGS__)

// Test Android power management integration
TEST_CASE(test_android_power_management) {
    kes_storage_config_t config;
    kes_profile_load(KES_PROFILE_SMARTPHONE, &config);
    
    // Test battery-aware behavior
    android_set_battery_level(20);  // Low battery
    kes_storage_t* storage;
    ASSERT_SUCCESS(kes_storage_create(&config, &storage));
    
    // Verify power-saving mode is active
    kes_storage_info_t info;
    kes_storage_get_info(storage, &info);
    ASSERT_TRUE(info.power_saving_mode);
    
    kes_storage_close(storage);
}

// Test Android-specific flash detection
TEST_CASE(test_android_flash_detection) {
    kes_flash_geometry_t geometry;
    
    // Test with typical Android eMMC device
    int result = kes_flash_detect_geometry("/dev/block/mmcblk0", &geometry);
    ASSERT_SUCCESS(result);
    
    // Verify reasonable geometry values
    ASSERT_TRUE(geometry.page_size >= 512);
    ASSERT_TRUE(geometry.block_count > 0);
}
```

---

## Tool Development Framework

### Command-Line Tool Structure

#### Tool Common Framework (`tools/common/tool_common.c`)
```c
// Common functionality for all KES tools
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    const char* name;
    const char* description;
    const char* usage;
    int (*main_function)(int argc, char* argv[]);
} kes_tool_info_t;

// Common command-line options
typedef struct {
    bool verbose;
    bool quiet;
    bool help;
    bool version;
    const char* config_file;
    kes_hardware_profile_t profile;
} kes_common_options_t;

// Parse common options
int parse_common_options(int argc, char* argv[], 
                        kes_common_options_t* options) {
    static struct option long_options[] = {
        {"verbose", no_argument, 0, 'v'},
        {"quiet", no_argument, 0, 'q'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'V'},
        {"config", required_argument, 0, 'c'},
        {"profile", required_argument, 0, 'p'},
        {0, 0, 0, 0}
    };
    
    int option_index = 0;
    int c;
    
    while ((c = getopt_long(argc, argv, "vqhVc:p:", 
                           long_options, &option_index)) != -1) {
        switch (c) {
            case 'v': options->verbose = true; break;
            case 'q': options->quiet = true; break;
            case 'h': options->help = true; break;
            case 'V': options->version = true; break;
            case 'c': options->config_file = optarg; break;
            case 'p': 
                if (parse_profile_name(optarg, &options->profile) != 0) {
                    fprintf(stderr, "Invalid profile: %s\n", optarg);
                    return -1;
                }
                break;
            default:
                return -1;
        }
    }
    
    return 0;
}
```

#### kes-mkfs Tool (`tools/kes-mkfs/kes_mkfs.c`)
```c
// KES storage formatting tool
typedef struct {
    kes_common_options_t common;
    
    // mkfs-specific options
    const char* device_path;
    uint64_t size;
    uint32_t block_size;
    bool force;
    const char* label;
    bool enable_compression;
    bool enable_encryption;
    
    // Layout options
    bool custom_layout;
    uint64_t bitmap_location;
    uint64_t index_location;
    uint32_t max_extents;
} mkfs_options_t;

void print_mkfs_usage(void) {
    printf("Usage: kes-mkfs [OPTIONS] DEVICE\n\n");
    printf("Format a device for use with KES storage\n\n");
    printf("Options:\n");
    printf("  -s, --size=SIZE        Storage size (default: use full device)\n");
    printf("  -b, --block-size=SIZE  Block size in KB (4, 8, 16, 32, 64)\n");
    printf("  -f, --force           Force format even if device contains data\n");
    printf("  -L, --label=LABEL     Set storage label\n");
    printf("  --enable-compression  Enable data compression\n");
    printf("  --enable-encryption   Enable data encryption\n");
    printf("  --bitmap-at=BLOCK     Place bitmap at specific block\n");
    printf("  --index-at=BLOCK      Place index at specific block\n");
    printf("  --max-extents=N       Maximum number of extents (0=no index)\n");
    printf("\nCommon options:\n");
    printf("  -v, --verbose         Verbose output\n");
    printf("  -q, --quiet          Quiet output\n");
    printf("  -p, --profile=PROFILE Hardware profile\n");
    printf("  -h, --help           Show this help\n");
    printf("  -V, --version        Show version\n");
}

int mkfs_main(int argc, char* argv[]) {
    mkfs_options_t options = {0};
    
    // Set defaults
    options.block_size = 8192;  // 8KB default
    
    // Parse command line
    if (parse_mkfs_options(argc, argv, &options) != 0) {
        print_mkfs_usage();
        return 1;
    }
    
    if (options.common.help) {
        print_mkfs_usage();
        return 0;
    }
    
    if (options.common.version) {
        print_version_info();
        return 0;
    }
    
    // Validate options
    if (validate_mkfs_options(&options) != 0) {
        return 1;
    }
    
    // Perform formatting
    if (format_storage(&options) != 0) {
        fprintf(stderr, "Formatting failed\n");
        return 1;
    }
    
    if (!options.common.quiet) {
        printf("Storage formatted successfully\n");
    }
    
    return 0;
}
```

#### kes-info Tool (`tools/kes-info/kes_info.c`)
```c
// Storage information and analysis tool
typedef struct {
    kes_common_options_t common;
    
    // info-specific options
    const char* storage_path;
    bool show_detailed;
    bool show_zones;
    bool show_performance;
    bool show_fragmentation;
    bool machine_readable;
} info_options_t;

int info_main(int argc, char* argv[]) {
    info_options_t options = {0};
    
    if (parse_info_options(argc, argv, &options) != 0) {
        print_info_usage();
        return 1;
    }
    
    // Open storage for inspection
    kes_storage_t* storage;
    int result = kes_storage_open(options.storage_path, 
                                 KES_STORAGE_READONLY, &storage);
    if (result != KES_SUCCESS) {
        fprintf(stderr, "Failed to open storage: %s\n",
                kes_get_error_string(result));
        return 1;
    }
    
    // Display basic information
    display_basic_info(storage, &options);
    
    if (options.show_detailed) {
        display_detailed_info(storage, &options);
    }
    
    if (options.show_zones) {
        display_zone_info(storage, &options);
    }
    
    if (options.show_performance) {
        display_performance_info(storage, &options);
    }
    
    if (options.show_fragmentation) {
        display_fragmentation_analysis(storage, &options);
    }
    
    kes_storage_close(storage);
    return 0;
}

void display_basic_info(kes_storage_t* storage, info_options_t* options) {
    kes_storage_info_t info;
    kes_storage_get_info(storage, &info);
    
    if (options->machine_readable) {
        printf("TOTAL_BLOCKS=%lu\n", info.total_blocks);
        printf("FREE_BLOCKS=%lu\n", info.free_blocks);
        printf("USED_BLOCKS=%lu\n", info.used_blocks);
        printf("BLOCK_SIZE=%u\n", info.block_size);
        printf("FRAGMENTATION=%.2f\n", info.fragmentation_ratio);
    } else {
        printf("KES Storage Information\n");
        printf("======================\n");
        printf("Total Blocks:    %lu\n", info.total_blocks);
        printf("Free Blocks:     %lu (%.1f%%)\n", 
               info.free_blocks, 
               100.0 * info.free_blocks / info.total_blocks);
        printf("Used Blocks:     %lu (%.1f%%)\n", 
               info.used_blocks,
               100.0 * info.used_blocks / info.total_blocks);
        printf("Block Size:      %u bytes\n", info.block_size);
        printf("Total Size:      %.2f MB\n", 
               (double)(info.total_blocks * info.block_size) / (1024*1024));
        printf("Fragmentation:   %.2f%%\n", info.fragmentation_ratio);
        printf("Total Extents:   %lu\n", info.total_extents);
    }
}
```

---

## Continuous Integration Setup

### GitHub Actions Workflow (`.github/workflows/build_test.yml`)
```yaml
name: Build and Test

on:
  push:
    branches: [ main, develop ]
  pull_request:
    branches: [ main ]

jobs:
  build-test-matrix:
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest]
        profile: [smartphone, laptop_linux, server_ssd]
        memory_budget: [small, medium, large]
        exclude:
          # Exclude incompatible combinations
          - profile: smartphone
            memory_budget: large
          - profile: server_ssd
            memory_budget: small
    
    runs-on: ${{ matrix.os }}
    
    steps:
    - uses: actions/checkout@v3
    
    - name: Install dependencies
      run: |
        if [ "$RUNNER_OS" == "Linux" ]; then
          sudo apt-get update
          sudo apt-get install -y build-essential valgrind
        elif [ "$RUNNER_OS" == "macOS" ]; then
          brew install valgrind
        fi
    
    - name: Build
      run: |
        make clean
        make PROFILE=${{ matrix.profile }} MEMORY_BUDGET=${{ matrix.memory_budget }}
    
    - name: Run Unit Tests
      run: make test-unit
    
    - name: Run Integration Tests  
      run: make test-integration
    
    - name: Memory Leak Check
      if: runner.os == 'Linux'
      run: |
        valgrind --tool=memcheck --leak-check=full --error-exitcode=1 \
          $(BUILD_DIR)/tests/test_storage
    
    - name: Upload Build Artifacts
      uses: actions/upload-artifact@v3
      with:
        name: kes-${{ matrix.os }}-${{ matrix.profile }}
        path: build/release/

  performance-regression:
    runs-on: ubuntu-latest
    needs: build-test-matrix
    
    steps:
    - uses: actions/checkout@v3
      with:
        fetch-depth: 0  # Need history for comparison
    
    - name: Build Performance Tests
      run: |
        make clean
        make PROFILE=server_ssd performance-tests
    
    - name: Run Performance Benchmarks
      run: |
        make benchmark > current_performance.txt
    
    - name: Compare with Baseline
      run: |
        scripts/testing/compare_performance.sh \
          baseline_performance.txt current_performance.txt
    
    - name: Upload Performance Results
      uses: actions/upload-artifact@v3
      with:
        name: performance-results
        path: |
          current_performance.txt
          performance_comparison.html
```

### Cross-Platform Testing (`.github/workflows/cross_platform.yml`)
```yaml
name: Cross-Platform Testing

on:
  schedule:
    - cron: '0 2 * * *'  # Daily at 2 AM
  workflow_dispatch:

jobs:
  android-build:
    runs-on: ubuntu-latest
    
    steps:
    - uses: actions/checkout@v3
    
    - name: Set up Android NDK
      uses: nttld/setup-ndk@v1
      with:
        ndk-version: r25b
    
    - name: Build for Android ARM64
      run: |
        export ANDROID_NDK_ROOT=$ANDROID_NDK_LATEST_HOME
        make clean
        make PROFILE=smartphone PLATFORM=android \
             CC=$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang
    
    - name: Run Android Emulator Tests
      uses: reactivecircus/android-emulator-runner@v2
      with:
        api-level: 29
        target: google_apis
        arch: x86_64
        script: |
          adb push build/android/tests/test_smartphone /data/local/tmp/
          adb shell /data/local/tmp/test_smartphone

  embedded-simulation:
    runs-on: ubuntu-latest
    
    steps:
    - uses: actions/checkout@v3
    
    - name: Set up ARM Cross-Compiler
      run: |
        sudo apt-get update
        sudo apt-get install -y gcc-arm-linux-gnueabihf
    
    - name: Build for ARM Embedded
      run: |
        make clean
        make PROFILE=embedded_nand \
             CC=arm-linux-gnueabihf-gcc \
             CROSS_COMPILE=arm-linux-gnueabihf-
    
    - name: Test in QEMU
      run: |
        sudo apt-get install -y qemu-user-static
        qemu-arm-static build/embedded/tests/test_embedded_nand
```

---

## Documentation Generation System

### Automated Documentation Build (`scripts/utilities/documentation_build.sh`)
```bash
#!/bin/bash
# Generate comprehensive documentation

set -e

BUILD_DIR="build/documentation"
DOCS_DIR="docs"

# Create build directory
mkdir -p "$BUILD_DIR"

echo "Building KES Documentation..."

# Generate API documentation from headers
echo "Generating API documentation..."
doxygen Doxyfile 2>/dev/null || echo "Doxygen not found, skipping API docs"

# Convert markdown documents to HTML
echo "Converting documentation to HTML..."
for md_file in "$DOCS_DIR"/*.md; do
    base_name=$(basename "$md_file" .md)
    pandoc "$md_file" \
        --from markdown \
        --to html \
        --standalone \
        --css=style.css \
        --output="$BUILD_DIR/${base_name}.html"
done

# Generate PDF documentation
echo "Generating PDF documentation..."
pandoc "$DOCS_DIR"/KES_Design_Document.md \
    --from markdown \
    --to pdf \
    --output="$BUILD_DIR/KES_Design_Document.pdf"

pandoc "$DOCS_DIR"/KES_API_Reference.md \
    --from markdown \
    --to pdf \
    --output="$BUILD_DIR/KES_API_Reference.pdf"

# Generate man pages from tools
echo "Generating man pages..."
mkdir -p "$BUILD_DIR/man"
for tool in build/release/tools/kes-*; do
    if [ -x "$tool" ]; then
        tool_name=$(basename "$tool")
        help2man "$tool" > "$BUILD_DIR/man/${tool_name}.1" || true
    fi
done

# Create documentation index
cat > "$BUILD_DIR/index.html" << 'EOF'
<!DOCTYPE html>
<html>
<head>
    <title>KES Documentation</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; }
        .section { margin: 20px 0; }
        .file-list { margin-left: 20px; }
    </style>
</head>
<body>
    <h1>KANEK Extents Storage (KES) Documentation</h1>
    
    <div class="section">
        <h2>Core Documentation</h2>
        <div class="file-list">
            <a href="KES_Design_Document.html">Design Document</a><br>
            <a href="KES_API_Reference.html">API Reference</a><br>
            <a href="KES_Project_Structure.html">Project Structure</a><br>
        </div>
    </div>
    
    <div class="section">
        <h2>PDF Downloads</h2>
        <div class="file-list">
            <a href="KES_Design_Document.pdf">Design Document (PDF)</a><br>
            <a href="KES_API_Reference.pdf">API Reference (PDF)</a><br>
        </div>
    </div>
    
    <div class="section">
        <h2>API Documentation</h2>
        <div class="file-list">
            <a href="api/index.html">Generated API Documentation</a><br>
        </div>
    </div>
    
    <div class="section">
        <h2>Manual Pages</h2>
        <div class="file-list">
            <a href="man/">Tool Manual Pages</a><br>
        </div>
    </div>
</body>
</html>
EOF

echo "Documentation build complete in $BUILD_DIR"
```

---

This comprehensive project structure provides a solid foundation for implementing the complete KES system with proper organization, build system, testing infrastructure, and documentation. The modular design allows for easy maintenance and extension while supporting all the requirements we discussed.

**The project is now fully documented and ready for implementation!**
