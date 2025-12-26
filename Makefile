# Makefile for KANEK Extents Storage (KES) - Minimal Implementation
# Builds core functionality for POSIX systems

# Project information
PROJECT_NAME = libkes
VERSION = 1.0.0

# Directories
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
TEST_DIR = tests

# Compiler and flags
CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -Werror -fPIC
LDFLAGS = 
LIBS = -lpthread

# Include paths
INCLUDES = -I$(INC_DIR)

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
               $(SRC_DIR)/kes_bitmap.c

# Object files
OBJECTS = $(CORE_SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# Library targets
STATIC_LIB = $(BUILD_DIR)/$(PROJECT_NAME).a
SHARED_LIB = $(BUILD_DIR)/$(PROJECT_NAME).so.$(VERSION)
SHARED_LIB_LINK = $(BUILD_DIR)/$(PROJECT_NAME).so

# Test sources and targets
TEST_SOURCES = $(wildcard $(TEST_DIR)/test_*.c)
TEST_TARGETS = $(TEST_SOURCES:$(TEST_DIR)/%.c=$(BUILD_DIR)/tests/%)

# Default target
.PHONY: all
all: $(STATIC_LIB) $(SHARED_LIB)

# Create build directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/tests

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

# Test targets
.PHONY: tests
tests: $(TEST_TARGETS)

.PHONY: test
test: tests
	@echo "Running tests..."
	@for test in $(TEST_TARGETS); do \
		echo "Running $$test"; \
		$$test || exit 1; \
	done

# Debug build
.PHONY: debug
debug:
	$(MAKE) DEBUG=1

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

# Help target
.PHONY: help
help:
	@echo "Available targets:"
	@echo "  all      - Build static and shared libraries"
	@echo "  debug    - Build with debug symbols"
	@echo "  tests    - Build test suite"
	@echo "  test     - Build and run tests"
	@echo "  install  - Install library system-wide"
	@echo "  uninstall- Uninstall library"
	@echo "  clean    - Remove build artifacts"
	@echo "  info     - Show build configuration"
	@echo "  help     - Show this help"
	@echo ""
	@echo "Environment variables:"
	@echo "  DEBUG=1  - Build with debug symbols"

# Force rebuild
.PHONY: rebuild
rebuild: clean all

# Dependencies
$(BUILD_DIR)/kes_storage.o: $(INC_DIR)/kes/kes_storage.h $(INC_DIR)/kes/kes_types.h $(INC_DIR)/kes/kes_bitmap.h
$(BUILD_DIR)/kes_bitmap.o: $(INC_DIR)/kes/kes_bitmap.h $(INC_DIR)/kes/kes_types.h
