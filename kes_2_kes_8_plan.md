# KES-2 + KES-8 Implementation Plan — Storage-Layer Error-Code Correctness

**Read this whole file before touching any code.** Both fixes here are
small and mechanical compared to KES-3/4/5/6 — the point of this
document is precision (exact locations, exact code, exact tests to
add), not managing a subtle design tradeoff. Follow it exactly.

## What these fix

Both from `PENDING_FIXES_SEP2026.md`, both in `src/kes_storage.c`:

- **KES-2** (Medium): `kes_allocation_strategy_t` declares
  `KES_ALLOC_BEST_FIT`/`WORST_FIT`/`NEXT_FIT` as public, settable enum
  values with no runtime guard anywhere. A caller who configures
  `KES_ALLOC_BEST_FIT` silently gets `FIRST_FIT` placement instead
  (`kes_extent_allocate()`'s dispatch `switch` only has an explicit
  `case` for `KES_ALLOC_FIRST_FIT`; every other value falls through
  `default:` to the same function, no error, no log line).
- **KES-8** (Low, informational): `load_storage_descriptor()` returns
  `KES_ERROR_IO` for a truncated file (fewer bytes than
  `sizeof(kes_storage_descriptor_t)`) but `KES_ERROR_CORRUPT` for an
  intact-size file with a bad magic number — two different codes for
  two different-looking failures, undocumented. Decision made for this
  plan (not asked as a question — low-stakes, matches
  `PENDING_FIXES_SEP2026.md`'s own default framing): **document the
  distinction, do not change the return codes.** Changing a truncated
  file to also report `KES_ERROR_CORRUPT` would be a real, if small,
  behavior change to an existing, working distinction that at least
  one existing test already asserts on — not worth the risk for a
  low-severity, already-non-crashing case.

These two are grouped because they are both about
`kes_storage.c`/`kes_storage_open()`/`kes_storage_create()`'s error
reporting, not because they interact with each other — unlike
KES-3/KES-4, there is no dependency between them. Do them in either
order.

## Files you will touch

- `src/kes_storage.c` — `validate_config()` (KES-2); a code comment in
  `load_storage_descriptor()` (KES-8)
- `include/kes/kes_storage.h` — doc-comment updates for
  `kes_storage_create()`/`kes_config_validate()` (KES-2) and
  `kes_storage_open()` (KES-8)
- `tests/test_kes_storage_edge.c` — one new test (KES-2)

---

## Step 1 (KES-2) — reject unimplemented allocation strategies

**Where to put the guard, and why this is simpler than it first
looks**: `kes_storage_open()` never lets a caller choose a strategy at
all — it always hardcodes `sto->strategy = KES_ALLOC_FIRST_FIT;`
(confirmed by reading it, `src/kes_storage.c`). The *only* function
that can set a `kes_storage_t`'s `strategy` to anything else is
`kes_storage_create()`, via `kes_storage_config_t.strategy`. That
means the single correct place to reject an unimplemented strategy is
`validate_config()` — the function `kes_storage_create()` already
calls first, and which is also the body of the public
`kes_config_validate()` wrapper. Fixing it there closes the gap
completely; nothing else needs to change. (This mirrors
`kes_cache_create()`'s existing pattern for the exact same category of
gap — see `src/kes_cache.c`, which rejects `KES_CACHE_LFU`/
`KES_CACHE_CUSTOM` at construction time rather than silently falling
back to LRU.)

Current code:

```c
static int validate_config( const kes_storage_config_t *config) {
    if ( config == NULL || config->device_path == NULL) {
        return(KES_ERROR_INVALID);
    }

    /* Validate block size */
    if ( !KES_IS_POWER_OF_2(config->block_size) ||
        config->block_size < KES_MIN_BLOCK_SIZE ||
        config->block_size > KES_MAX_BLOCK_SIZE) {
        return(KES_ERROR_INVALID);
    }

    /* Validate device size */
    if ( config->device_size < config->block_size * 10) {
        return(KES_ERROR_INVALID);  /* Too small */
    }

    return(KES_SUCCESS);
}
```

Replace with:

```c
static int validate_config( const kes_storage_config_t *config) {
    if ( config == NULL || config->device_path == NULL) {
        return(KES_ERROR_INVALID);
    }

    /* Validate block size */
    if ( !KES_IS_POWER_OF_2(config->block_size) ||
        config->block_size < KES_MIN_BLOCK_SIZE ||
        config->block_size > KES_MAX_BLOCK_SIZE) {
        return(KES_ERROR_INVALID);
    }

    /* Validate device size */
    if ( config->device_size < config->block_size * 10) {
        return(KES_ERROR_INVALID);  /* Too small */
    }

    /* KES-2: allocate_extent_first_fit() is the only allocation
     * strategy actually implemented -- kes_extent_allocate()'s
     * dispatch switch (below, in this same file) only has a real case
     * for KES_ALLOC_FIRST_FIT; every other kes_allocation_strategy_t
     * value falls through its "default:" to the same function
     * silently. Reject the declared-but-unimplemented strategies here
     * instead, the same way kes_cache_create() already rejects
     * KES_CACHE_LFU/_CUSTOM (src/kes_cache.c) rather than silently
     * substituting a different policy than what was asked for. */
    if ( config->strategy != KES_ALLOC_FIRST_FIT) {
        TRACE_ERR( "unimplemented allocation strategy %d requested "
                   "(only KES_ALLOC_FIRST_FIT is implemented)",
                   (int)config->strategy);
        return(KES_ERROR_INVALID);
    }

    return(KES_SUCCESS);
}
```

Note: this is the only `TRACE_ERR` call added to `validate_config()`
in this plan. The function's three pre-existing checks above stay
exactly as they are (silent, no logging) — per `AGENTS.md`'s "no
drive-by rewrites" rule, only the new hunk this plan adds gets Rule 11
treatment, not the whole function. (If KES-9's logging sweep is ever
done later, those three existing checks are exactly the kind of thing
it would pick up — not this plan's job.)

### 1a. Update `kes_extent_allocate()`'s dispatch comment

Current code (`src/kes_storage.c`, inside `kes_extent_allocate()`):

```c
    /* Use allocation strategy */
    switch ( storage->strategy) {
        case KES_ALLOC_FIRST_FIT:
        default:
            result = allocate_extent_first_fit( storage, request, extent);
            break;
    }
```

Leave the code exactly as-is (do not remove the `default:` case — it
is harmless, defensive, and removing it would make the switch warn
under `-Wswitch-enum`-style scrutiny for no benefit). Add a one-line
comment above it noting the guard now makes this effectively
unreachable for anything but `FIRST_FIT`:

```c
    /* Use allocation strategy. KES-2: validate_config() now rejects
     * any storage->strategy other than KES_ALLOC_FIRST_FIT before a
     * kes_storage_t can ever be constructed with one, so the
     * "default:" case below is defensive/unreachable in practice, not
     * a silent substitution anymore. */
    switch ( storage->strategy) {
        case KES_ALLOC_FIRST_FIT:
        default:
            result = allocate_extent_first_fit( storage, request, extent);
            break;
    }
```

### 1b. `include/kes/kes_storage.h` doc comment

`kes_storage_create()`'s doc comment currently says nothing about
strategy validation. Add one line:

```c
/**
 * Create a new KES storage instance
 * @param config Storage configuration. config->strategy must be
 *                KES_ALLOC_FIRST_FIT -- BEST_FIT/WORST_FIT/NEXT_FIT
 *                are declared but not yet implemented and are
 *                rejected with KES_ERROR_INVALID (KES-2).
 * @param storage Output parameter for created storage handle
 * @return KES_SUCCESS or error code
 */
int kes_storage_create( const kes_storage_config_t *config,
                         kes_storage_t **storage);
```

### 1c. New test — `tests/test_kes_storage_edge.c`

Add a new test function (follow this file's existing style — see
`test_exhaustion_then_free_and_reallocate` for the pattern) and
register it in the file's test-case array:

```c
/*
 * KES-2: kes_storage_create() must reject a config requesting an
 * unimplemented allocation strategy, not silently substitute
 * FIRST_FIT for it. See kes_2_kes_8_plan.md.
 */
static bool test_create_rejects_unimplemented_strategy(void) {
    kes_storage_t *storage = NULL;

    /* Sanity: FIRST_FIT itself must still work -- reuse this file's
     * existing make_storage() helper (it already sets
     * .strategy = KES_ALLOC_FIRST_FIT and the rest of a known-good
     * config), rather than duplicating its fields here. */
    cleanup();
    TEST_ASSERT( make_storage( TEST_FILE, &storage) == KES_SUCCESS,
                "KES_ALLOC_FIRST_FIT is still accepted");
    kes_storage_close( storage);
    storage = NULL;
    cleanup();

    kes_storage_config_t base_cfg = {
        .device_path = TEST_FILE,
        .device_size = TEST_SIZE,
        .block_size = 4096,
        .flags = KES_STORAGE_CREATE,
        .strategy = KES_ALLOC_FIRST_FIT,
    };

    kes_allocation_strategy_t unimplemented[] = {
        KES_ALLOC_BEST_FIT, KES_ALLOC_WORST_FIT, KES_ALLOC_NEXT_FIT
    };
    for ( size_t i = 0; i < sizeof(unimplemented) /
                            sizeof(unimplemented[0]); i++) {
        kes_storage_config_t cfg = base_cfg;
        cfg.strategy = unimplemented[i];

        TEST_ASSERT( kes_storage_create( &cfg, &storage) ==
                        KES_ERROR_INVALID,
                    "FIXED (KES-2): an unimplemented allocation "
                    "strategy is rejected with KES_ERROR_INVALID, "
                    "not silently substituted with FIRST_FIT");
        TEST_ASSERT( storage == NULL,
                    "*storage was not left pointing at a partially-"
                    "initialized handle on this rejection");
    }

    TEST_SUCCESS( "kes_storage_create() rejects "
                  "BEST_FIT/WORST_FIT/NEXT_FIT (unimplemented), "
                  "accepts FIRST_FIT (implemented)");
}
```

Use whatever this file's actual `TEST_FILE`/`TEST_SIZE`/block-size
constant names are (check the top of `tests/test_kes_storage_edge.c`
for the exact names already in use — do not invent new ones; match
its existing `make_storage()`-style helpers/constants instead of the
placeholder names above if they differ). Add the new test to this
file's `test_cases[]` array in the same style as its neighbors.

---

## Step 2 (KES-8) — document the truncated-vs-corrupted distinction

**No behavior change** (see the decision note at the top of this
file). Add a comment to `load_storage_descriptor()` explaining the
distinction precisely, and a note on `kes_storage_open()`'s public doc
comment so a caller doesn't have to read the implementation to learn
this.

### 2a. `src/kes_storage.c`

Current code:

```c
static int load_storage_descriptor( kes_storage_t *storage) {
    /* Seek to beginning of file */
    if ( lseek( storage->fd, 0, SEEK_SET) != 0) {
        return(KES_ERROR_IO);
    }

    /* Read descriptor */
    ssize_t bytes_read = read( storage->fd, &storage->desc,
                                sizeof(kes_storage_descriptor_t));
    if ( bytes_read != sizeof(kes_storage_descriptor_t)) {
        return(KES_ERROR_IO);
    }

    /* Validate descriptor */
    if ( storage->desc.magic != KES_MAGIC_NUMBER) {
        return(KES_ERROR_CORRUPT);
    }

    return(KES_SUCCESS);
}
```

Add a comment right above the byte-count check (do not change the
logic, only add the comment — if KES-6's plan has already been applied
to this function, this comment goes above the *same* byte-count check,
which KES-6 does not touch):

```c
static int load_storage_descriptor( kes_storage_t *storage) {
    /* Seek to beginning of file */
    if ( lseek( storage->fd, 0, SEEK_SET) != 0) {
        return(KES_ERROR_IO);
    }

    /* KES-8: a file shorter than sizeof(kes_storage_descriptor_t)
     * (e.g. truncated, or never fully written) returns KES_ERROR_IO
     * here, NOT KES_ERROR_CORRUPT -- deliberately left this way (see
     * kes_2_kes_8_plan.md; PENDING_FIXES_SEP2026.md's KES-8). A
     * caller distinguishing "corrupt" from "truncated/missing" by
     * return code alone should treat KES_ERROR_IO from
     * kes_storage_open() as "not even a full descriptor's worth of
     * bytes present" and KES_ERROR_CORRUPT (below) as "a full
     * descriptor was read, but its magic number is wrong." Either way
     * kes_storage_open() fails cleanly with no *storage output. */
    ssize_t bytes_read = read( storage->fd, &storage->desc,
                                sizeof(kes_storage_descriptor_t));
    if ( bytes_read != sizeof(kes_storage_descriptor_t)) {
        return(KES_ERROR_IO);
    }

    /* Validate descriptor */
    if ( storage->desc.magic != KES_MAGIC_NUMBER) {
        return(KES_ERROR_CORRUPT);
    }

    return(KES_SUCCESS);
}
```

### 2b. `include/kes/kes_storage.h`

Current doc comment:

```c
/**
 * Open an existing KES storage instance
 * @param device_path Path to storage device or file
 * @param flags Open flags (readonly, sync, etc.)
 * @param storage Output parameter for opened storage handle
 * @return KES_SUCCESS or error code
 */
int kes_storage_open( const char *device_path, uint32_t flags,
                       kes_storage_t **storage);
```

Add one line documenting the distinction:

```c
/**
 * Open an existing KES storage instance
 * @param device_path Path to storage device or file
 * @param flags Open flags (readonly, sync, etc.)
 * @param storage Output parameter for opened storage handle
 * @return KES_SUCCESS, KES_ERROR_IO if the file is too short to hold
 *         a full descriptor (e.g. truncated), or KES_ERROR_CORRUPT if
 *         a full descriptor was read but its magic number (and, once
 *         KES-6 lands, its bitmap checksum or format version) is
 *         invalid -- these are two different failure shapes with two
 *         different codes, not interchangeable (KES-8).
 */
int kes_storage_open( const char *device_path, uint32_t flags,
                       kes_storage_t **storage);
```

(The parenthetical about KES-6 is there so this doc comment stays
accurate regardless of whether KES-6 has landed yet when you apply
this plan — remove the "once KES-6 lands" qualifier and just state it
plainly if KES-6 is already applied in this tree.)

No test changes needed for this step — `tests/test_kes_crash_consistency.c`'s
`test_truncated_and_corrupted_descriptor` already asserts both codes
exactly as they are today; this plan does not change that behavior, so
that test needs no edits.

---

## Verification checklist

1. `make clean && make test` — all tests pass, including the new
   `test_create_rejects_unimplemented_strategy`.
2. `make asan` — clean.
3. `make tsan` — clean (neither change touches concurrency).
4. `make valgrind` — clean.
5. `make check-all` — clean, "ALL CHECKS PASSED".
6. Grep the test suite for any place that constructs a
   `kes_storage_config_t` with `.strategy` set to something other than
   `KES_ALLOC_FIRST_FIT` (or that omits `.strategy` and relies on it
   zero-initializing to `KES_ALLOC_FIRST_FIT == 0`, which is still
   fine) and confirm none exist — Step 1's research for this plan
   already did this check and found none, but re-verify against
   whatever the tree looks like when you actually apply this, since
   other work may have landed in between.

## Explicitly out of scope

- Actually implementing `BEST_FIT`/`WORST_FIT`/`NEXT_FIT` — that is
  the separate, larger "allocation strategies beyond first-fit" item
  `KES_HARDENING_PLAN.md` still describes as the main remaining
  allocator work, with no detailed implementation plan of its own yet.
  This plan only makes the current silent-substitution gap loud.
- Changing `KES_ERROR_IO` to `KES_ERROR_CORRUPT` for the truncated-file
  case, or vice versa — explicitly rejected above as the wrong
  tradeoff for this plan's scope.
- Any change to `kes_storage_open()`'s behavior for a corrupted
  descriptor beyond the doc-comment clarification — no code path here
  changes.

## Bookkeeping when done

Same convention as the other plans: move `PENDING_FIXES_SEP2026.md`'s
`## KES-2` and `## KES-8` sections to `CLOSED` (for KES-8, "closed" here
means "documented, deliberately not behavior-changed" — say that
explicitly, the way `PENDING_ITEMS.md`'s existing entries distinguish
a behavior fix from a documentation-only resolution) with the commit
hash, and add matching entries to `PENDING_ITEMS.md`'s `## Resolved`
section.
