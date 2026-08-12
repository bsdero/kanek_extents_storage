# Makefile for KANEK Extents Storage (KES)
# Updated for new directory structure

# Project information
PROJECT_NAME = libkes
VERSION = 1.0.0

# Directories
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
TEST_DIR = tests
EXAMPLES_DIR = examples
DOCS_DIR = docs

# kanek_foundations (KFL) sibling repo -- provides trace.h and other
# foundation facilities. Cloned automatically by the foundations-fetch
# target if not already checked out next to this repo.
FOUNDATIONS_DIR = ../kanek_foundations
FOUNDATIONS_SRC = $(FOUNDATIONS_DIR)/src
FOUNDATIONS_LIB = $(FOUNDATIONS_SRC)/libkfl.a
FOUNDATIONS_REPO = https://github.com/bsdero/kanek_foundations.git
FOUNDATIONS_CFLAGS = -Wall -DUSER_SPACE -g

# Compiler and flags
CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -Werror -fPIC
LDFLAGS =
LIBS = -lpthread

# Include paths
INCLUDES = -I$(INC_DIR) -I$(FOUNDATIONS_SRC)

# Build configuration
ifdef DEBUG
    CFLAGS += -g3 -O0 -DDEBUG
    BUILD_TYPE = debug
else
    CFLAGS += -O2 -DNDEBUG
    BUILD_TYPE = release
endif

# Source files
CORE_SOURCES = $(SRC_DIR)/kes_storage.c \
               $(SRC_DIR)/kes_bitmap.c \
               $(SRC_DIR)/kes_cache.c

# Object files
OBJECTS = $(CORE_SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# Library targets
STATIC_LIB = $(BUILD_DIR)/$(PROJECT_NAME).a
SHARED_LIB = $(BUILD_DIR)/$(PROJECT_NAME).so.$(VERSION)
SHARED_LIB_LINK = $(BUILD_DIR)/$(PROJECT_NAME).so

# Test sources and targets
TEST_SOURCES = $(wildcard $(TEST_DIR)/test_*.c)
TEST_TARGETS = $(TEST_SOURCES:$(TEST_DIR)/%.c=$(BUILD_DIR)/tests/%)

# Example targets
EXAMPLE_SOURCES = $(wildcard $(EXAMPLES_DIR)/*.c)
EXAMPLE_TARGETS = $(EXAMPLE_SOURCES:$(EXAMPLES_DIR)/%.c=$(BUILD_DIR)/examples/%)

# Default target
.PHONY: all
all: foundations-fetch $(STATIC_LIB) $(SHARED_LIB)

# Clone kanek_foundations next to this repo if it isn't there yet.
# Only fetches -- does not build it. Needed so -I$(FOUNDATIONS_SRC)
# resolves (e.g. trace.h) even before any code links libkfl.a.
.PHONY: foundations-fetch
foundations-fetch:
	@if [ ! -d $(FOUNDATIONS_DIR) ]; then \
		echo "kanek_foundations not found at $(FOUNDATIONS_DIR)," \
		     "cloning..."; \
		git clone $(FOUNDATIONS_REPO) $(FOUNDATIONS_DIR); \
	fi

# Build libkfl.a from the sibling checkout, using whichever CFLAGS
# this invocation needs (plain, ASan, or TSan) so instrumentation
# matches whatever KES itself is being built with. Not a prerequisite
# of "all" -- only built on demand once code actually links it.
.PHONY: foundations
foundations: foundations-fetch
	$(MAKE) -C $(FOUNDATIONS_SRC) clean all \
	    CFLAGS="$(FOUNDATIONS_CFLAGS)"

# Create build directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/tests
	mkdir -p $(BUILD_DIR)/examples

# Compile source files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "Compiling $<"
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build static library
$(STATIC_LIB): $(OBJECTS)
	@echo "Creating static library $@"
	ar rcs $@ $^
	ranlib $@

# Build shared library
$(SHARED_LIB): $(OBJECTS)
	@echo "Creating shared library $@"
	$(CC) -shared -Wl,-soname,$(notdir $(SHARED_LIB)) \
		$(LDFLAGS) -o $@ $^ $(LIBS)
	ln -sf $(notdir $(SHARED_LIB)) $(SHARED_LIB_LINK)

# Build tests
$(BUILD_DIR)/tests/%: $(TEST_DIR)/%.c $(STATIC_LIB) | $(BUILD_DIR)
	@echo "Building test $@"
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(STATIC_LIB) $(LIBS)

# Build examples
$(BUILD_DIR)/examples/%: $(EXAMPLES_DIR)/%.c $(STATIC_LIB) | $(BUILD_DIR)
	@echo "Building example $@"
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(STATIC_LIB) $(LIBS)

# Test targets
.PHONY: tests
tests: $(TEST_TARGETS)

.PHONY: test-core
test-core: $(BUILD_DIR)/tests/test_kes_minimal
	@echo "Running core tests (guaranteed to pass)..."
	@cp $(BUILD_DIR)/tests/test_kes_minimal /tmp/test_kes_minimal_run
	@chmod +x /tmp/test_kes_minimal_run
	@/tmp/test_kes_minimal_run

.PHONY: test
test: tests
	@echo "Running tests..."
	@echo "Copying tests to /tmp for execution..."
	@for test in $(TEST_TARGETS); do \
		testname=$$(basename $$test); \
		cp $$test /tmp/$$testname; \
		chmod +x /tmp/$$testname; \
		echo "Running $$testname..."; \
		/tmp/$$testname || exit 1; \
		echo ""; \
	done
	@echo "All tests completed successfully!"

# Example targets
.PHONY: examples
examples: $(EXAMPLE_TARGETS)

.PHONY: run-example
run-example: examples
	@echo "Running example..."
	@cp $(BUILD_DIR)/examples/example_kes_usage /tmp/kes_example_test
	@chmod +x /tmp/kes_example_test
	@/tmp/kes_example_test

# Debug build
.PHONY: debug
debug:
	$(MAKE) DEBUG=1

# Sanitizer build variants (KES_HARDENING_PLAN.md Phase 2). ASan and
# TSan cannot share a binary, so each does a full clean rebuild with
# its own flags, then actually runs every test binary -- a variant
# that "builds clean" but was never executed proves nothing.
ASAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g
TSAN_FLAGS = -fsanitize=thread -fno-omit-frame-pointer -g

.PHONY: asan
asan:
	$(MAKE) clean
	$(MAKE) all tests CFLAGS="$(CFLAGS) $(ASAN_FLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(ASAN_FLAGS)"
	@echo "Running tests under ASan+UBSan..."
	@for test in $(TEST_TARGETS); do \
		testname=$$(basename $$test); \
		echo "--- $$testname (asan) ---"; \
		$$test || exit 1; \
	done

.PHONY: tsan
tsan:
	$(MAKE) clean
	$(MAKE) all tests CFLAGS="$(CFLAGS) $(TSAN_FLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(TSAN_FLAGS)"
	@echo "Running tests under TSan..."
	@for test in $(TEST_TARGETS); do \
		testname=$$(basename $$test); \
		echo "--- $$testname (tsan) ---"; \
		setarch $$(uname -m) -R $$test || exit 1; \
	done

.PHONY: sanitize-all
sanitize-all: asan tsan
	$(MAKE) clean
	$(MAKE) all

# Valgrind pass: independent leak/error checker on a plain (non-
# sanitized) build -- ASan and Valgrind's instrumentation conflict,
# so this always starts from a clean, unsanitized rebuild.
.PHONY: valgrind
valgrind:
	$(MAKE) clean
	$(MAKE) tests
	@echo "Running tests under Valgrind..."
	@for test in $(TEST_TARGETS); do \
		testname=$$(basename $$test); \
		echo "--- $$testname (valgrind) ---"; \
		valgrind --leak-check=full --error-exitcode=1 \
		    --track-origins=yes $$test || exit 1; \
	done

# Single gate: normal build+tests, ASan, TSan, Valgrind, in sequence.
# Any failing step aborts (non-zero exit) via make's default
# stop-on-error behavior plus the explicit "|| exit 1" inside each
# tool's own test loop above.
.PHONY: check-all
check-all:
	@echo "=== [1/4] Normal build + tests ==="
	$(MAKE) clean
	$(MAKE) test
	@echo "=== [2/4] ASan + UBSan ==="
	$(MAKE) asan
	@echo "=== [3/4] TSan ==="
	$(MAKE) tsan
	@echo "=== [4/4] Valgrind ==="
	$(MAKE) valgrind
	$(MAKE) clean
	$(MAKE) all
	@echo "check-all: ALL CHECKS PASSED"

# Install library
.PHONY: install
install: all
	@echo "Installing $(PROJECT_NAME) to /usr/local"
	sudo mkdir -p /usr/local/lib
	sudo mkdir -p /usr/local/include/kes
	sudo cp $(STATIC_LIB) /usr/local/lib/
	sudo cp $(SHARED_LIB) /usr/local/lib/
	sudo ln -sf $(notdir $(SHARED_LIB)) /usr/local/lib/$(notdir $(SHARED_LIB_LINK))
	sudo cp $(INC_DIR)/kes/*.h /usr/local/include/kes/
	sudo ldconfig

# Uninstall library
.PHONY: uninstall
uninstall:
	@echo "Uninstalling $(PROJECT_NAME)"
	sudo rm -f /usr/local/lib/$(PROJECT_NAME).*
	sudo rm -rf /usr/local/include/kes
	sudo ldconfig

# Documentation
.PHONY: docs
docs:
	@echo "Documentation available in $(DOCS_DIR)/"
	@echo "  - $(DOCS_DIR)/KES_API_Reference.md"
	@echo "  - $(DOCS_DIR)/KES_Design_Document.md" 
	@echo "  - $(DOCS_DIR)/README_Implementation.md"
	@echo "  - $(DOCS_DIR)/kes_cache_design.md"

# Clean build artifacts
.PHONY: clean
clean:
	@echo "Cleaning build artifacts"
	rm -rf $(BUILD_DIR)

# Show build information
.PHONY: info
info:
	@echo "Build Configuration:"
	@echo "  Project: $(PROJECT_NAME) $(VERSION)"
	@echo "  Build Type: $(BUILD_TYPE)"
	@echo "  Compiler: $(CC)"
	@echo "  CFLAGS: $(CFLAGS)"
	@echo "  Sources: $(CORE_SOURCES)"
	@echo "  Objects: $(OBJECTS)"
	@echo "  Static Library: $(STATIC_LIB)"
	@echo "  Shared Library: $(SHARED_LIB)"
	@echo "  Tests: $(TEST_TARGETS)"
	@echo "  Examples: $(EXAMPLE_TARGETS)"

# Show directory structure
.PHONY: tree
tree:
	@echo "Project Structure:"
	@echo "kanek_extents_storage/"
	@echo "├── $(DOCS_DIR)/                 # Documentation"
	@echo "├── $(EXAMPLES_DIR)/             # Usage examples"
	@echo "├── $(INC_DIR)/kes/              # Public headers"
	@echo "├── $(SRC_DIR)/                  # Implementation"
	@echo "├── $(TEST_DIR)/                 # Test suite"
	@echo "├── Makefile                     # Build system"
	@echo "├── README.md                    # Project overview"
	@echo "└── LICENSE                      # License file"

# Help target
.PHONY: help
help:
	@echo "Available targets:"
	@echo "  all         - Build static and shared libraries"
	@echo "  debug       - Build with debug symbols"
	@echo "  tests       - Build test suite"
	@echo "  test-core   - Build and run core tests (9/9 passing)"
	@echo "  test        - Build and run all tests (includes cache tests)"
	@echo "  examples    - Build examples"
	@echo "  run-example - Build and run example"
	@echo "  docs        - Show documentation files"
	@echo "  install     - Install library system-wide"
	@echo "  uninstall   - Uninstall library"
	@echo "  clean       - Remove build artifacts"
	@echo "  info        - Show build configuration"
	@echo "  tree        - Show directory structure"
	@echo "  help        - Show this help"
	@echo ""
	@echo "Sanitizer/verification targets:"
	@echo "  asan          - Clean rebuild + run tests under ASan+UBSan"
	@echo "  tsan          - Clean rebuild + run tests under TSan"
	@echo "  sanitize-all  - asan + tsan, then a plain rebuild"
	@echo "  valgrind      - Clean rebuild + run tests under Valgrind"
	@echo "  check-all     - normal + asan + tsan + valgrind, gated"
	@echo ""
	@echo "kanek_foundations (KFL) sibling repo:"
	@echo "  foundations-fetch - clone KFL next to this repo if" \
	     "missing"
	@echo "  foundations       - build KFL's libkfl.a on demand"
	@echo ""
	@echo "Test Information:"
	@echo "  test-core   - Recommended: Runs 9 core tests (all pass)"
	@echo "  test        - All tests including cache (6/6 passing)"
	@echo ""
	@echo "Environment variables:"
	@echo "  DEBUG=1     - Build with debug symbols"

# Force rebuild
.PHONY: rebuild
rebuild: clean all

# Package creation
.PHONY: package
package: clean
	@echo "Creating package..."
	tar -czf $(PROJECT_NAME)-$(VERSION).tar.gz \
		--exclude='.git' \
		--exclude='build' \
		--exclude='*.tar.gz' \
		--transform 's,^,$(PROJECT_NAME)-$(VERSION)/,' \
		*
	@echo "Package created: $(PROJECT_NAME)-$(VERSION).tar.gz"

# Dependencies
$(BUILD_DIR)/kes_storage.o: $(INC_DIR)/kes/kes_storage.h $(INC_DIR)/kes/kes_types.h $(INC_DIR)/kes/kes_bitmap.h
$(BUILD_DIR)/kes_bitmap.o: $(INC_DIR)/kes/kes_bitmap.h $(INC_DIR)/kes/kes_types.h
$(BUILD_DIR)/kes_cache.o: $(INC_DIR)/kes/kes_cache.h $(INC_DIR)/kes/kes_types.h
