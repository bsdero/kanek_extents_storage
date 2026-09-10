# KES-6 Implementation Plan — Bitmap Checksum

**Read this whole file before touching any code.** It is written for an
implementer with zero prior context on this decision — every step is
spelled out, including exact code to write and exact tests that will
break and why. Do not improvise around the parts that seem verbose;
the verbosity is there because the obvious shortcuts were checked and
found to break something.

## What this fixes

`KES-6` in `PENDING_FIXES_SEP2026.md`: `kes_bitmap_load()`/
`kes_bitmap_save()` have no integrity check on the on-disk bitmap
region. A single silently bit-flipped byte (disk corruption, a stray
write, cosmic ray, whatever) goes completely undetected at
`kes_storage_open()` time and causes a *real* double-allocation: the
allocator hands out a block that is still live and holds another
extent's data, silently overwriting it. `tests/test_kes_crash_consistency.c`'s
`test_bitflipped_bitmap_block` demonstrates this concretely today.

## Decision already made (do not re-litigate)

**Breaking on-disk format change, no migration path.** Confirmed with
the project owner. The on-disk descriptor struct gains a field and
bumps its version. Storage files created before this change will fail
to open afterward (`KES_ERROR_CORRUPT`, with a clear log message
explaining why) and must be recreated. There is no migration tool and
none should be added as part of this work.

## Design

Add a `uint32_t bitmap_checksum` field to `kes_storage_descriptor_t`
(which already lives in block 0 of the file, alongside the existing
`magic`/`version_major`/`version_minor` fields used for the same kind
of validation). It holds the CRC-32C of the bitmap's on-disk byte
representation (`bitmap->data`, `bitmap->total_bytes`), recomputed and
stored every time the descriptor is saved, and verified every time the
bitmap is loaded.

**Why the descriptor, not appending bytes after the bitmap on disk:**
the descriptor already has 32 unused reserved bytes and is already the
place this codebase validates structural integrity (`magic`). Storing
the checksum there needs no change to the bitmap's on-disk size or
layout (`bitmap_start_block`/`bitmap_blocks` math in
`calculate_bitmap_blocks()` stays untouched). Appending 4 bytes after
the bitmap region instead would require re-deriving that layout math
and is strictly more invasive for no benefit.

**Why CRC-32C specifically, and why via `kanek_foundations` instead of
a new implementation:** `../kanek_foundations/src/crc32c.h` (the
sibling KFL repo this project already depends on for `trace.h`)
provides `kfl_crc32c()` and documents the convention explicitly:

> Convention used throughout the KANEK stack: the last 4 bytes of
> every on-disk struct are the little-endian CRC-32C of all preceding
> bytes.

`AGENTS.md` in this repo's own root separately confirms `crc32c.h`'s
output format is meant to be a stable interface used to protect
on-disk structs across the whole KANEK stack, not just KFL. Reusing it
here (a) avoids a second, incompatible checksum implementation in the
same organization's stack, and (b) means any future KANEK tool that
already speaks `kfl_crc32c` (e.g. a repair/fsck utility) can verify
this bitmap without reimplementing anything. This is a real consequence
worth knowing before you start: **this is the first time anything in
`src/` actually links `libkfl.a`**, not just includes its headers (see
"Build system changes" below for what that requires).

## Files you will touch

- `include/kes/kes_types.h` — descriptor struct, version bump
- `include/kes/kes_bitmap.h` — new function declaration
- `src/kes_bitmap.c` — new function implementation, two new includes
- `src/kes_storage.c` — version check on load; checksum set/verify on
  every save/load path
- `Makefile` — actually link `libkfl.a` (currently header-only)
- `README.md` — document the breaking change and the new build
  dependency
- `tests/test_kes_crash_consistency.c` — `test_bitflipped_bitmap_block`
  must be rewritten; its entire premise (undetected corruption) is
  what this fix removes
- `PENDING_ITEMS.md` / `PENDING_FIXES_SEP2026.md` — bookkeeping once
  done, per this repo's own convention (see the end of this file)

---

## Step 1 — `include/kes/kes_types.h`

### 1a. Bump the version

```c
/* Version information */
#define KES_VERSION_MAJOR    2   /* v2: KES-6 added bitmap_checksum */
#define KES_VERSION_MINOR    0
#define KES_VERSION_PATCH    0
```

(Currently `KES_VERSION_MAJOR` is `1`. This is the only place the
version number is defined — `kes_get_version()` in `src/kes_storage.c`
just returns these macros, no change needed there.)

### 1b. Add the field, shrink `reserved`

Current struct (`kes_storage_descriptor_t`):

```c
    /* Statistics */
    uint64_t free_blocks;            /* Current free blocks */
    uint64_t used_blocks;            /* Current used blocks */
    uint64_t next_extent_id;         /* Next extent ID to allocate */

    /* Reserved for future use */
    uint8_t reserved[32];
} kes_storage_descriptor_t;
```

Replace with:

```c
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
```

Confirmed safe: grepped every `tests/*.c` for `.reserved` usage against
`kes_storage_descriptor_t` — the only `.reserved = 0` hits in the test
suite are on an unrelated struct (`kes_extent_id_t`, the cache layer's
own type, which has its own separate `reserved` field). Nothing
touches this descriptor's `reserved` bytes directly.

---

## Step 2 — `include/kes/kes_bitmap.h`

Add this declaration after `kes_bitmap_save()`'s declaration, matching
the existing `/** ... */` doc-comment style already used for every
other declaration in this file (this file predates `CODING_STYLE.md`
and stays in that style throughout — see `AGENTS.md`'s "don't silently
reformat surrounding code" rule; matching this file's own existing
convention for one new declaration dropped into the middle of fifteen
others in that same style is the right call here, not a violation of
that rule):

```c
/**
 * Compute the CRC-32C checksum of the bitmap's on-disk byte
 * representation (bitmap->data over bitmap->total_bytes).
 * @param bitmap Target bitmap
 * @param checksum Output parameter for the computed checksum
 * @return KES_SUCCESS or error code
 */
int kes_bitmap_checksum( kes_bitmap_t *bitmap, uint32_t *checksum);
```

---

## Step 3 — `src/kes_bitmap.c`

### 3a. Add two includes at the top

Current top of file:

```c
#include <kes/kes_bitmap.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
```

Add `trace.h` (for `TRACE_ERR`, per this new function following
`CODING_STYLE.md` Rule 11 from the start — see `AGENTS.md`'s "apply it
by default to any new function") and `crc32c.h` (for `kfl_crc32c()`):

```c
#include <kes/kes_bitmap.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "trace.h"
#include "crc32c.h"
```

Both resolve via the existing `-I../kanek_foundations/src` include
path already in this project's `Makefile` (`INCLUDES` variable) — no
include-path changes needed for this step, only for actually linking
(Step 6).

### 3b. Implement the function

Add at the end of the file, after `kes_bitmap_save()`:

```c
int kes_bitmap_checksum( kes_bitmap_t *bitmap, uint32_t *checksum) {
    if ( bitmap == NULL || checksum == NULL) {
        TRACE_ERR( "invalid arguments (bitmap=%p, checksum=%p)",
                   (void *)bitmap, (void *)checksum);
        return(KES_ERROR_INVALID);
    }

    *checksum = kfl_crc32c( 0, bitmap->data, bitmap->total_bytes);
    return(KES_SUCCESS);
}
```

---

## Step 4 — `src/kes_storage.c`: version check

In `load_storage_descriptor()`, right after the existing magic check:

```c
    /* Validate descriptor */
    if ( storage->desc.magic != KES_MAGIC_NUMBER) {
        return(KES_ERROR_CORRUPT);
    }

    return(KES_SUCCESS);
}
```

Insert the version check **before** the `return(KES_SUCCESS);`:

```c
    /* Validate descriptor */
    if ( storage->desc.magic != KES_MAGIC_NUMBER) {
        return(KES_ERROR_CORRUPT);
    }

    /* KES-6: the on-disk format gained a bitmap_checksum field and
     * shrank kes_storage_descriptor_t's reserved bytes accordingly --
     * a breaking change. Reject anything not written by this exact
     * major version rather than silently trusting a stale layout
     * (whose reserved bytes would read as zero, not a real checksum,
     * and would then always spuriously fail the checksum check in
     * kes_storage_open() with a confusing KES_ERROR_CORRUPT instead
     * of this precise one). */
    if ( storage->desc.version_major != KES_VERSION_MAJOR) {
        TRACE_ERR( "incompatible on-disk format version %u.%u "
                   "(library is %u.%u) -- storage files created "
                   "before the KES-6 bitmap-checksum change must be "
                   "recreated",
                   storage->desc.version_major,
                   storage->desc.version_minor,
                   KES_VERSION_MAJOR, KES_VERSION_MINOR);
        return(KES_ERROR_CORRUPT);
    }

    return(KES_SUCCESS);
}
```

`load_storage_descriptor()` already runs on every `kes_storage_open()`
call, so this check is automatically exercised everywhere it needs to
be. `trace.h` is already `#include`d at the top of `kes_storage.c` —
no include changes needed here.

---

## Step 5 — `src/kes_storage.c`: set/verify the checksum

There are four places that touch `kes_storage_descriptor_t` and/or the
bitmap on disk. Each needs a small, precise addition. Do not
paraphrase these — copy them exactly; the ordering of the checks
matters (each must run and update `result`/`storage->desc.bitmap_checksum`
before the *next* step that depends on it).

### 5a. `kes_storage_create()` — set the initial checksum

Current code (inside `kes_storage_create()`):

```c
    /* Create bitmap */
    result = kes_bitmap_create( sto->desc.user_blocks, &sto->bitmap);
    if ( result != KES_SUCCESS) {
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }

    /* Save initial descriptor to storage */
    result = save_storage_descriptor( sto);
```

Insert a new block between those two, so the descriptor `create()` is
about to write already carries the correct checksum for the freshly
created (all-zero) in-memory bitmap:

```c
    /* Create bitmap */
    result = kes_bitmap_create( sto->desc.user_blocks, &sto->bitmap);
    if ( result != KES_SUCCESS) {
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }

    /* KES-6: record the checksum of the freshly-created (all-zero)
     * in-memory bitmap into the descriptor now, so the descriptor
     * this function is about to write is self-consistent from the
     * start rather than carrying a stale/zero checksum until the
     * first real sync. Note this does NOT itself write the bitmap
     * bytes to disk -- kes_storage_create() still only writes block 0
     * (the descriptor) here, same as before this change; see
     * kes_5_plan.md if that is also being applied, since KES-5 adds
     * that missing bitmap write for an unrelated reason (making the
     * file safe to reload-from-disk immediately after create()). The
     * checksum set here stays correct either way, since it is always
     * computed from the in-memory bitmap, not from whatever has
     * physically reached disk. */
    uint32_t initial_checksum;
    result = kes_bitmap_checksum( sto->bitmap, &initial_checksum);
    if ( result != KES_SUCCESS) {
        kes_bitmap_destroy( sto->bitmap);
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }
    sto->desc.bitmap_checksum = initial_checksum;

    /* Save initial descriptor to storage */
    result = save_storage_descriptor( sto);
```

(`kes_bitmap_checksum()` can only fail here on a NULL argument, which
cannot happen given `kes_bitmap_create()` just succeeded — but every
other call in this function checks its result the same defensive way,
so this matches existing local style.)

### 5b. `kes_storage_open()` — verify on load

Current code:

```c
    /* Load bitmap from storage */
    off_t bitmap_offset = sto->desc.bitmap_start_block *
                           sto->desc.block_size;
    result = kes_bitmap_load( sto->bitmap, sto->fd, bitmap_offset);
    if ( result != KES_SUCCESS) {
        kes_bitmap_destroy( sto->bitmap);
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }

    *storage = sto;
    return(KES_SUCCESS);
}
```

Insert a verification block between the successful load and the final
`*storage = sto;`:

```c
    /* Load bitmap from storage */
    off_t bitmap_offset = sto->desc.bitmap_start_block *
                           sto->desc.block_size;
    result = kes_bitmap_load( sto->bitmap, sto->fd, bitmap_offset);
    if ( result != KES_SUCCESS) {
        kes_bitmap_destroy( sto->bitmap);
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }

    /* KES-6: verify the loaded bitmap against the checksum recorded
     * in the descriptor at the last successful save, catching silent
     * on-disk bit flips that kes_bitmap_load()'s own free-bit recount
     * cannot (a flip that preserves the total population count -- one
     * bit 1->0, another 0->1 -- is invisible to that recount alone,
     * but not to a real checksum). */
    uint32_t computed_checksum;
    result = kes_bitmap_checksum( sto->bitmap, &computed_checksum);
    if ( result == KES_SUCCESS &&
        computed_checksum != sto->desc.bitmap_checksum) {
        TRACE_ERR( "bitmap checksum mismatch: on-disk 0x%08x, "
                   "computed 0x%08x -- refusing to open corrupted "
                   "storage %s",
                   sto->desc.bitmap_checksum, computed_checksum,
                   device_path);
        result = KES_ERROR_CORRUPT;
    }
    if ( result != KES_SUCCESS) {
        kes_bitmap_destroy( sto->bitmap);
        close( sto->fd);
        pthread_mutex_destroy( &sto->lock);
        free( sto);
        return(result);
    }

    *storage = sto;
    return(KES_SUCCESS);
}
```

### 5c. `kes_storage_sync()` — refresh on every sync

Current code:

```c
int kes_storage_sync( kes_storage_t *storage) {
    if ( storage == NULL || storage->readonly) {
        return(KES_ERROR_INVALID);
    }

    pthread_mutex_lock( &storage->lock);

    /* Save bitmap */
    off_t bitmap_offset = storage->desc.bitmap_start_block *
                           storage->desc.block_size;
    int result = kes_bitmap_save( storage->bitmap, storage->fd,
                                   bitmap_offset);

    if ( result == KES_SUCCESS) {
        /* Save descriptor */
        result = save_storage_descriptor( storage);
    }
```

Insert the checksum refresh between the bitmap save and the descriptor
save:

```c
int kes_storage_sync( kes_storage_t *storage) {
    if ( storage == NULL || storage->readonly) {
        return(KES_ERROR_INVALID);
    }

    pthread_mutex_lock( &storage->lock);

    /* Save bitmap */
    off_t bitmap_offset = storage->desc.bitmap_start_block *
                           storage->desc.block_size;
    int result = kes_bitmap_save( storage->bitmap, storage->fd,
                                   bitmap_offset);

    /* KES-6: refresh the descriptor's checksum from the bitmap bytes
     * that were just written, before persisting the descriptor itself
     * -- otherwise the on-disk checksum would keep describing whatever
     * bitmap contents were current at the LAST sync, not this one. */
    if ( result == KES_SUCCESS) {
        uint32_t checksum;
        result = kes_bitmap_checksum( storage->bitmap, &checksum);
        if ( result == KES_SUCCESS) {
            storage->desc.bitmap_checksum = checksum;
        }
    }

    if ( result == KES_SUCCESS) {
        /* Save descriptor */
        result = save_storage_descriptor( storage);
    }
```

(The rest of the function — the `fsync()` block and the
unlock/return — is unchanged.)

### 5d. `kes_storage_close()` — refresh before the final save

Current code:

```c
    pthread_mutex_lock( &storage->lock);

    /* Save bitmap before closing */
    if ( !storage->readonly) {
        off_t bitmap_offset = storage->desc.bitmap_start_block *
                               storage->desc.block_size;
        kes_bitmap_save( storage->bitmap, storage->fd, bitmap_offset);

        /* Save descriptor with updated statistics */
        save_storage_descriptor( storage);

        /* Sync file system */
        fsync( storage->fd);
    }
```

Replace with:

```c
    pthread_mutex_lock( &storage->lock);

    /* Save bitmap before closing */
    if ( !storage->readonly) {
        off_t bitmap_offset = storage->desc.bitmap_start_block *
                               storage->desc.block_size;
        kes_bitmap_save( storage->bitmap, storage->fd, bitmap_offset);

        /* KES-6: refresh the descriptor's bitmap checksum from what
         * was just written, same reasoning as kes_storage_sync(). */
        uint32_t checksum;
        if ( kes_bitmap_checksum( storage->bitmap, &checksum) ==
            KES_SUCCESS) {
            storage->desc.bitmap_checksum = checksum;
        }

        /* Save descriptor with updated statistics */
        save_storage_descriptor( storage);

        /* Sync file system */
        fsync( storage->fd);
    }
```

Note: this code already ignores `kes_bitmap_save()`/
`save_storage_descriptor()`/`fsync()`'s return values (pre-existing —
`kes_storage_close()` always returns `KES_SUCCESS` once it reaches
this point). That is a separate, pre-existing looseness, not something
this plan changes — do not add new error propagation here, only the
checksum refresh shown above. (If you are also applying KES-5, its
plan revisits this same block again — see `kes_5_plan.md` Step 5d for
the combined final version; apply that version instead if doing both.)

---

## Step 6 — Build system: actually link `libkfl.a`

Read this section carefully. This is the first code in this repo that
calls a function whose *implementation* (not just declaration) lives
in `kanek_foundations` — up to now only `trace.h` (header-only macros)
was used. `AGENTS.md`'s own "Infrastructure notes" section says
outright: "**Nothing in `src/` includes or links it yet.**" That
sentence stops being true after this step, and every place that
referenced it needs updating to match.

### 6a. Consolidate the `foundations`/`foundations-fetch` targets

Current:

```makefile
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
```

Replace with (keep `foundations-fetch` exactly as-is; only
`foundations` changes, and a new real file-target,
`$(FOUNDATIONS_LIB)`, is added so other rules can depend on it
normally):

```makefile
.PHONY: foundations-fetch
foundations-fetch:
	@if [ ! -d $(FOUNDATIONS_DIR) ]; then \
		echo "kanek_foundations not found at $(FOUNDATIONS_DIR)," \
		     "cloning..."; \
		git clone $(FOUNDATIONS_REPO) $(FOUNDATIONS_DIR); \
	fi

# libkfl.a is now actually linked (KES-6 needs kfl_crc32c()), not just
# needed for header resolution -- this is a real file-based Make
# target so other rules (SHARED_LIB, test/example binaries) can depend
# on it normally. Always does a clean rebuild: this project already
# forces full clean rebuilds for asan/tsan/valgrind for the same
# reason (a stale, differently-instrumented libkfl.a silently linked
# into a sanitized libkes build/binary would be a real, hard-to-spot
# bug) -- accept the small extra build time on every invocation as the
# simple, safe default rather than tracking a CFLAGS fingerprint.
$(FOUNDATIONS_LIB): foundations-fetch
	$(MAKE) -C $(FOUNDATIONS_SRC) clean all \
	    CFLAGS="$(FOUNDATIONS_CFLAGS)"

# Convenience alias for manually building/rebuilding libkfl.a.
.PHONY: foundations
foundations: $(FOUNDATIONS_LIB)
```

### 6b. Link `libkfl.a` into the shared library

Current:

```makefile
# Build shared library
$(SHARED_LIB): $(OBJECTS)
	@echo "Creating shared library $@"
	$(CC) -shared -Wl,-soname,$(notdir $(SHARED_LIB)) \
		$(LDFLAGS) -o $@ $^ $(LIBS)
	ln -sf $(notdir $(SHARED_LIB)) $(SHARED_LIB_LINK)
```

Change only the prerequisite list (the recipe body is unchanged — `$^`
already expands to include the new prerequisite, in the right order:
objects first, then the archive that resolves their undefined
symbols):

```makefile
# Build shared library
$(SHARED_LIB): $(OBJECTS) $(FOUNDATIONS_LIB)
	@echo "Creating shared library $@"
	$(CC) -shared -Wl,-soname,$(notdir $(SHARED_LIB)) \
		$(LDFLAGS) -o $@ $^ $(LIBS)
	ln -sf $(notdir $(SHARED_LIB)) $(SHARED_LIB_LINK)
```

Linking a static archive (`libkfl.a`) into a shared object (`-shared`)
statically embeds the object code it actually needs (`crc32c.o`) into
`libkes.so` itself — consumers of `libkes.so` get no new runtime
dependency from this.

**`$(STATIC_LIB)` (the `.a` target) is deliberately NOT changed** —
`ar rcs` just archives `$(OBJECTS)` as before. This means
`libkes.a` alone still has an unresolved `kfl_crc32c` symbol in
`kes_bitmap.o`; see the README note in Step 7 for why this is the
right call and what it means for consumers.

### 6c. Link `libkfl.a` into test and example binaries

Current:

```makefile
# Build tests
$(BUILD_DIR)/tests/%: $(TEST_DIR)/%.c $(STATIC_LIB) | $(BUILD_DIR)
	@echo "Building test $@"
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(STATIC_LIB) $(LIBS)

# Build examples
$(BUILD_DIR)/examples/%: $(EXAMPLES_DIR)/%.c $(STATIC_LIB) | $(BUILD_DIR)
	@echo "Building example $@"
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(STATIC_LIB) $(LIBS)
```

Replace both with:

```makefile
# Build tests
$(BUILD_DIR)/tests/%: $(TEST_DIR)/%.c $(STATIC_LIB) $(FOUNDATIONS_LIB) | $(BUILD_DIR)
	@echo "Building test $@"
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(STATIC_LIB) $(FOUNDATIONS_LIB) $(LIBS)

# Build examples
$(BUILD_DIR)/examples/%: $(EXAMPLES_DIR)/%.c $(STATIC_LIB) $(FOUNDATIONS_LIB) | $(BUILD_DIR)
	@echo "Building example $@"
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(STATIC_LIB) $(FOUNDATIONS_LIB) $(LIBS)
```

(Link order matters here: `$(STATIC_LIB)` before `$(FOUNDATIONS_LIB)`
before `$(LIBS)` — each archive resolves symbols needed by what
precedes it on the command line, left to right, which is exactly the
order these three need to be in. No `--start-group`/`--end-group` is
needed since there is no circular dependency between them.)

### 6d. Thread sanitizer flags through to `libkfl.a` for `asan`/`tsan`/`soak`

`FOUNDATIONS_CFLAGS` (used inside the `$(FOUNDATIONS_LIB)` rule from
6a) is a separate variable from KES's own `CFLAGS`, and does not
automatically pick up `$(ASAN_FLAGS)`/`$(TSAN_FLAGS)`. Since
`$(FOUNDATIONS_LIB)` is now actually linked into sanitized test
binaries, an uninstrumented `libkfl.a` linked into an ASan/TSan build
of KES risks an instrumentation mismatch. Fix this by overriding
`FOUNDATIONS_CFLAGS` on the same recursive `$(MAKE)` line that already
overrides `CFLAGS`/`LDFLAGS` for KES itself, in three targets.

`asan` — current:

```makefile
.PHONY: asan
asan:
	$(MAKE) clean
	$(MAKE) all tests CFLAGS="$(CFLAGS) $(ASAN_FLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(ASAN_FLAGS)"
```

New:

```makefile
.PHONY: asan
asan:
	$(MAKE) clean
	$(MAKE) all tests CFLAGS="$(CFLAGS) $(ASAN_FLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(ASAN_FLAGS)" \
	    FOUNDATIONS_CFLAGS="$(FOUNDATIONS_CFLAGS) $(ASAN_FLAGS)"
```

(Only the `FOUNDATIONS_CFLAGS=...` line is new; everything after it in
the target, including the per-binary loop, is unchanged.)

`tsan` — same pattern, using `$(TSAN_FLAGS)`:

```makefile
.PHONY: tsan
tsan:
	$(MAKE) clean
	$(MAKE) all tests CFLAGS="$(CFLAGS) $(TSAN_FLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(TSAN_FLAGS)" \
	    FOUNDATIONS_CFLAGS="$(FOUNDATIONS_CFLAGS) $(TSAN_FLAGS)"
```

`soak` — same pattern (it also builds under TSan):

```makefile
.PHONY: soak
soak:
	$(MAKE) clean
	$(MAKE) all tests CFLAGS="$(CFLAGS) $(TSAN_FLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(TSAN_FLAGS)" \
	    FOUNDATIONS_CFLAGS="$(FOUNDATIONS_CFLAGS) $(TSAN_FLAGS)"
```

**`valgrind`, `check-all`, `test`, `tests`, `test-core`, `examples`,
`run-example`, `debug` need no changes** — they get a correctly
plain-flags `libkfl.a` automatically through the new
`$(FOUNDATIONS_LIB)` prerequisite chain (Step 6a), which always
rebuilds with the default `FOUNDATIONS_CFLAGS` unless overridden.

### 6e. Known trade-off, stated plainly

Because `$(FOUNDATIONS_LIB)` always does a clean rebuild of a sibling
repo (Step 6a), **every** `make` invocation that reaches it (`make
all`, `make test`, etc., not just `asan`/`tsan`) now pays a small,
constant extra build cost — a full rebuild of `kanek_foundations`,
even when nothing in it changed. This is a deliberate simplicity/
correctness-over-speed trade-off, consistent with how this project
already treats `asan`/`tsan`/`valgrind` (always `$(MAKE) clean`
first). If this becomes annoying in practice, a future improvement
could fingerprint `FOUNDATIONS_CFLAGS` to skip the rebuild when
unchanged — explicitly out of scope for this plan.

---

## Step 7 — `README.md`

Add a short, clearly-labeled note (near wherever this repo's own build
instructions live) covering two things a downstream consumer needs to
know and would otherwise discover the hard way:

1. **Breaking on-disk format change.** Storage files created by a
   library build before this change will fail to open
   (`KES_ERROR_CORRUPT`) and must be recreated. There is no migration
   path.
2. **`libkes.a` has a real link-time dependency on `libkfl.a`
   (`kanek_foundations`) now**, specifically for `kfl_crc32c()`, used
   to protect the on-disk bitmap (KES-6). Anyone linking `libkes.a`
   directly (not `libkes.so`, which already embeds what it needs — see
   Step 6b) must also link `kanek_foundations`'s `libkfl.a` (build it
   via `make -C ../kanek_foundations/src` or point at wherever your
   project vendors/builds it), the same way this repo's own
   `Makefile` now does for its test/example binaries.

---

## Step 8 — Test changes

### 8a. `tests/test_kes_crash_consistency.c`: `test_bitflipped_bitmap_block`

This test's entire premise is "no corruption is detected." After this
fix, corruption IS detected — the test must be rewritten to prove
that, not patched around it. Everything from the reopen at line 456
onward (in the pre-fix file) depended on `kes_storage_open()`
succeeding despite the corruption; none of that is reachable anymore.

Replace the whole function body from the `/* Reopen -- no corruption
is detected... */` comment through the end (i.e. everything from what
was line 455 through line 526 in the pre-fix file) with:

```c
    /* Reopen -- KES-6's bitmap checksum now catches this. */
    TEST_ASSERT( kes_storage_open( TEST_FILE, 0, &st) ==
                    KES_ERROR_CORRUPT,
                "FIXED (KES-6): a single-bit-flipped bitmap block is "
                "now detected at open() time via the descriptor's "
                "bitmap_checksum field, and kes_storage_open() "
                "correctly refuses to proceed rather than silently "
                "trusting corrupted bytes");
    TEST_ASSERT( st == NULL,
                "*storage was not left pointing at a partially-"
                "initialized handle on this failure");

    cleanup();
    TEST_SUCCESS( "bit-flipped bitmap block: detected at open() time "
                  "via KES-6's bitmap checksum, refusing to open "
                  "rather than risking the double-allocation/silent-"
                  "corruption hazard this test used to demonstrate");
}
```

The rest of the function (the setup through the clean close and the
byte-flip itself — everything before the `/* Reopen */` comment) stays
exactly as-is; only the back half changes. Also update the block
comment above the function (originally describing "no corruption is
detected... this is characterized precisely" as the finding) to say
the corruption is now detected and cite this plan/KES-6 instead — it
should read as history (`PENDING_ITEMS.md`'s "Resolved" section
convention: describe what was true, then what fixed it), not as a
still-open finding.

### 8b. Full test suite

Run `make test` after every step above (not just at the end) and read
every failure before changing anything. For any test other than
`test_bitflipped_bitmap_block` that starts failing, ask: does this
assertion encode the OLD, buggy "corruption is silently accepted"
behavior, or is this a genuine regression from a mistake in the steps
above? Fix genuine regressions in the code; update assertions that
encode old, now-intentionally-changed behavior — do not "fix" the new
code to make an old, wrong assertion pass again.

`tests/test_kes_storage_full.c`, `tests/test_kes_storage_edge.c`,
`tests/test_kes_minimal.c` were all grepped for direct
`kes_storage_descriptor_t` field access during this plan's own
research and found to touch only `magic`/`block_size`/`total_blocks` —
none reference `reserved` or the struct's exact size — so none are
expected to need changes, but verify by actually running them, not by
trusting this note.

---

## Verification checklist (run every one, paste the actual output
before considering this done — do not assume, per `AGENTS.md`'s
standing rule about pasted evidence)

1. `make clean && make test` — expect all tests to pass except the
   rewritten `test_bitflipped_bitmap_block`'s NEW assertions passing
   (not the old ones — those are gone).
2. `make asan` — clean, no new findings. Confirms the new
   `kfl_crc32c()` call path and the new checksum-mismatch error path
   are both memory-safe.
3. `make tsan` — clean. (This change adds no new concurrency, so this
   is mostly confirming the build/link changes didn't break anything
   under TSan's instrumentation.)
4. `make valgrind` — clean.
5. `make check-all` — clean, "ALL CHECKS PASSED".
6. Manually confirm the breaking-change behavior once: build the
   library, create a storage file, then (with a debugger or a small
   throwaway program) revert `KES_VERSION_MAJOR` to `1` and confirm
   `kes_storage_open()` on a file written by the reverted build now
   fails with `KES_ERROR_CORRUPT` and a log line naming the version
   mismatch — this is the concrete proof the "breaking change, no
   migration" decision actually takes effect, not just a unit test
   proving it in isolation.

## Explicitly out of scope for this plan

- Anything from `KES-5` (unsynchronized cross-process access) — a
  separate plan, `kes_5_plan.md`. Note that KES-5's plan *depends* on
  this one (`kes_bitmap_checksum()` and `bitmap_checksum` must already
  exist) — implement this plan first if doing both.
- A migration/repair tool for pre-KES-6 storage files.
- Checksumming anything other than the bitmap region (e.g. the
  descriptor's own statistics fields, or user data itself) — out of
  scope, not what KES-6 asked for.
- The `kes_storage_create()`-time full bitmap write covered in
  `kes_5_plan.md` Step 5's "kes_storage_create()" section — that
  belongs to KES-5, not this plan, even though it touches the exact
  same function. If you are doing KES-6 only, do not add it.

## Bookkeeping when done

Per this repo's existing convention (see how KES-1's fix was recorded
in both files): once verified, move `PENDING_FIXES_SEP2026.md`'s
`## KES-6` section status to `CLOSED` with the commit hash, and add a
matching evidence-first entry to `PENDING_ITEMS.md`'s `## Resolved`
section (what it was, what the fix does, and the verification output
from the checklist above) — follow the exact shape already used there
for KES-1/the `block_count == 0` fix/the `kes_cache_flush_extent()`
race, so the two files stay in the format future readers already
expect.
