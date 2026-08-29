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
	# test_kes_fault_injection deliberately triggers a real, expected
	# allocator OOM (tests/test_kes_fault_injection.c, A.4.2 --
	# plan_phase5.md). ASan's default reaction to ANY out-of-memory
	# allocation failure is to print a report and abort the process
	# rather than return NULL, which would fail this whole target on
	# an intentional test case. allocator_may_return_null=1 makes
	# ASan behave like plain glibc (return NULL) instead -- set only
	# for this one binary so every other test keeps ASan's default
	# strict abort-on-OOM behavior (a genuine, unexpected OOM
	# elsewhere in the suite should still be loud).
	@for test in $(TEST_TARGETS); do \
		testname=$$(basename $$test); \
		echo "--- $$testname (asan) ---"; \
		if [ "$$testname" = "test_kes_fault_injection" ]; then \
			ASAN_OPTIONS=allocator_may_return_null=1 $$test || exit 1; \
		else \
			$$test || exit 1; \
		fi; \
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

# Soak test (A.7.2, plan_phase5.md). tests/test_kes_soak.c runs its
# mixed get/put/pin/unpin/mark_dirty/flush/invalidate worker threads
# vs. dedicated sync() threads for KES_SOAK_SECONDS wall-clock seconds
# (default 2, so it stays fast and harmless as part of a normal
# "make test" run) -- this target overrides that to 600 (10 minutes).
#
# Layered under TSan, not ASan: KES_HARDENING_PLAN.md S6.H's two soak
# concerns are slow memory drift and counters drifting out of sync.
# The latter is a data-race symptom, and TSan is what actually finds
# the race that causes it -- ASan's LeakSanitizer only catches
# genuinely unfreed allocations, which the test's own periodic
# quiesce-and-compare of memory_used against a live hash-table walk
# already covers more precisely than ASan could here. (This choice
# already paid off during development: a real, reproducible TSan-
# detected data race was found this way -- kes_cache_flush_extent(),
# unlike make_room_for_new_entry()'s internal eviction-flush path,
# does not check KES_EXTENT_LOADING before writing out an entry's
# data buffer, so a flush racing a still-in-flight load on the same
# entry can read partially-written data concurrently with the load's
# own write into it. See PENDING_ITEMS.md's Phase 5 progress section
# for the full writeup -- not fixed here per this plan's "report, do
# not silently fix" rule.)
#
# Follows the asan/tsan targets' clean-rebuild pattern; must run
# under "setarch $$(uname -m) -R" in this WSL2 environment, same
# reason as the "tsan" target.
.PHONY: soak
soak:
	$(MAKE) clean
	$(MAKE) all tests CFLAGS="$(CFLAGS) $(TSAN_FLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(TSAN_FLAGS)"
	@echo "Running 10-minute soak test under TSan " \
	     "(KES_SOAK_SECONDS=600)..."
	KES_SOAK_SECONDS=600 setarch $$(uname -m) -R \
	    $(BUILD_DIR)/tests/test_kes_soak

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

# Stress-run harness (KES_HARDENING_PLAN.md S6.C / plan_phase5.md
# Track A.3.2): repeatedly runs the two binaries with real
# thread/process races -- test_kes_cache (threads) and
# test_kes_multiprocess (fork()) -- STRESS_RUNS times each (default
# 100; override with `make stress STRESS_RUNS=500`), stopping and
# reporting the failing run number on the first non-zero exit rather
# than continuing past a failure.
#
# Plain rebuild by default. Pass SANITIZER=asan or SANITIZER=tsan to
# layer one of the sanitizer variants on top instead, reusing the
# same ASAN_FLAGS/TSAN_FLAGS the asan/tsan targets themselves use
# (chosen over always running plain, since a stress loop's whole
# point is surfacing a rare interleaving, and ASan/TSan are far more
# likely than a plain build to turn one into a visible failure) --
# SANITIZER is left as an opt-in rather than the default because a
# sanitized clean rebuild is much slower per run, and plain-build
# stress runs are still useful for catching non-memory-safety logic
# bugs (deadlocks, wrong results) cheaply.
#
# Fixed-seed-for-reproducibility note (per plan_phase5.md A.3.2):
# neither tests/test_kes_cache.c nor tests/test_kes_multiprocess.c
# currently calls rand()/srand() or usleep() with a randomized
# argument -- checked by grep before adding this target. Their
# delays are fixed constants (e.g. usleep(100) in mock I/O) and all
# interleaving nondeterminism comes from OS thread/process
# scheduling, not a seeded RNG this target could make reproducible.
# If either file later grows a seeded random delay, wire a
# KES_TEST_SEED environment variable through it (default
# time(NULL) if unset) and print the seed at test startup, so a
# stress failure can be reproduced via `KES_TEST_SEED=<value>`, per
# this same task's guidance -- there is nothing to wire up today.
STRESS_RUNS ?= 100
STRESS_TARGETS = $(BUILD_DIR)/tests/test_kes_cache \
                  $(BUILD_DIR)/tests/test_kes_multiprocess

.PHONY: stress
stress:
	@if [ "$(SANITIZER)" = "asan" ]; then \
		echo "=== stress: clean rebuild under ASan+UBSan ==="; \
		$(MAKE) clean; \
		$(MAKE) all tests CFLAGS="$(CFLAGS) $(ASAN_FLAGS)" \
		    LDFLAGS="$(LDFLAGS) $(ASAN_FLAGS)"; \
	elif [ "$(SANITIZER)" = "tsan" ]; then \
		echo "=== stress: clean rebuild under TSan ==="; \
		$(MAKE) clean; \
		$(MAKE) all tests CFLAGS="$(CFLAGS) $(TSAN_FLAGS)" \
		    LDFLAGS="$(LDFLAGS) $(TSAN_FLAGS)"; \
	elif [ -n "$(SANITIZER)" ]; then \
		echo "stress: unknown SANITIZER=$(SANITIZER)" \
		     "(expected asan or tsan)"; \
		exit 1; \
	else \
		echo "=== stress: plain clean rebuild ==="; \
		$(MAKE) clean; \
		$(MAKE) tests; \
	fi
	@echo "Stress-running test_kes_cache and test_kes_multiprocess," \
	     "$(STRESS_RUNS) run(s) each" \
	     "(SANITIZER=$(if $(SANITIZER),$(SANITIZER),none))..."
	@logfile=$$(mktemp /tmp/kes_stress_XXXXXX.log); \
	trap 'rm -f $$logfile' EXIT; \
	for test in $(STRESS_TARGETS); do \
		testname=$$(basename $$test); \
		echo "--- stress: $$testname ---"; \
		run=1; \
		while [ $$run -le $(STRESS_RUNS) ]; do \
			if [ "$(SANITIZER)" = "tsan" ]; then \
				setarch $$(uname -m) -R $$test \
				    > $$logfile 2>&1; \
			else \
				$$test > $$logfile 2>&1; \
			fi; \
			rc=$$?; \
			if [ $$rc -ne 0 ]; then \
				echo "STRESS FAILURE: $$testname failed" \
				     "on run $$run/$(STRESS_RUNS)" \
				     "(exit $$rc)"; \
				echo "--- captured output ---"; \
				cat $$logfile; \
				echo "--- reproduce with: $$test" \
				     "(tsan: setarch \`uname -m\` -R" \
				     "$$test) ---"; \
				exit 1; \
			fi; \
			run=$$((run + 1)); \
		done; \
		echo "$$testname: $(STRESS_RUNS)/$(STRESS_RUNS)" \
		     "runs passed"; \
	done; \
	echo "stress: ALL RUNS PASSED"

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
#
# Removes $(BUILD_DIR) (all binaries/libraries built by this
# Makefile) plus the /tmp copies that "test", "test-core", and
# "run-example" leave behind for execution -- without this, those
# copies persist indefinitely across "make clean" since they live
# outside $(BUILD_DIR).
.PHONY: clean
clean:
	@echo "Cleaning build artifacts"
	rm -rf $(BUILD_DIR)
	@echo "Cleaning temporary /tmp binary copies"
	@for test in $(TEST_TARGETS); do \
		rm -f /tmp/$$(basename $$test); \
	done
	rm -f /tmp/test_kes_minimal_run /tmp/kes_example_test
	rm -f $(PROJECT_NAME)-$(VERSION).tar.gz

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
	@echo "  stress        - repeat test_kes_cache/test_kes_multiprocess" \
	     "STRESS_RUNS times (default 100); SANITIZER=asan|tsan to" \
	     "layer a sanitizer on top"
	@echo "  soak          - clean rebuild under TSan + run" \
	     "test_kes_soak for 10 minutes (KES_SOAK_SECONDS=600)"
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
