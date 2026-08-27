# KANEK Extents Storage (KES) - Continuation Prompt

## Standing rule

This file previously marked features "✅ COMPLETE" that did not
exist in the code (multi-policy LRU/LFU/Clock eviction, background
dirty-page sync, flash zones, garbage collection, wear leveling) --
see `KES_HARDENING_PLAN.md` §0/§1 for how that was discovered and
corrected. Do not repeat that failure mode: only mark something done
here after verifying it against the actual source and pasting
command output, per `KES_HARDENING_PLAN.md`'s standing rule.
`PENDING_ITEMS.md` is the current, maintained work tracker --
read it alongside this file, and prefer it when the two disagree.

## Project Status

### Core storage + bitmap layer: stable

- **Block Bitmap Management** (`kes_bitmap.c/.h`)
- **Storage Management** (`kes_storage.c/.h`) -- extent
  allocate/free, read/write, persistence via a descriptor in block 0
- **Type System** (`kes_types.h`)
- Extent allocation is **first-fit only**
  (`allocate_extent_first_fit()`) regardless of what
  `kes_storage_config_t.strategy` requests -- best-fit/worst-fit/
  next-fit/buddy-system are declared in
  `kes_allocation_strategy_t` but not wired up.
- Test coverage: `test_kes_minimal.c` (9/9) plus
  `test_kes_bitmap_full.c` (10/10) and `test_kes_storage_full.c`
  (15/15) for direct per-function coverage.

### Cache layer: functionally complete for LRU, not multi-policy

- **LRU cache with real eviction** (`kes_cache.c/.h`) --
  `kes_cache_get_extent()` evicts from the LRU tail as needed to
  respect `config.max_entries`/`config.max_memory`, returning
  `KES_ERROR_BUSY` if it can't free enough room (every cached entry
  referenced or pinned).
- **`KES_CACHE_LFU`/`KES_CACHE_CUSTOM` are NOT implemented** --
  `kes_cache_create()` rejects a config requesting either, rather
  than silently behaving as LRU.
- **Background dirty-page sync** (`kes_cache_start()`/
  `kes_cache_stop()`) -- one or more background threads wake every
  `config.sync_interval_ms` (timed against `CLOCK_MONOTONIC`) and
  run the same flush-then-evict sweep `kes_cache_sync()` runs
  manually.
- **`kes_cache_sync()`, `kes_cache_invalidate()`,
  `kes_cache_reset_stats()`** are implemented per
  `KES_HARDENING_PLAN.md` §4's specified semantics (`invalidate()`
  discards dirty data unconditionally, no implicit flush;
  `reset_stats()` zeros only the cumulative counters, not
  `memory_used`/`entries_cached`/`entries_dirty`/`entries_pinned`).
- Concurrency: a documented, deliberate hazard exists in
  `make_room_for_new_entry()`'s check-then-evict-then-insert
  sequence being non-atomic across concurrent misses on *different*
  ids (bounded overshoot of `max_entries`/`max_memory`, not a
  correctness bug) -- see that function's doc comment in
  `src/kes_cache.c`.
- Test coverage: `test_kes_cache.c` (23/23, including several
  targeted concurrency-regression tests -- duplicate-insert-on-race,
  sync-vs-get/put under contention) and `test_kes_cache_full.c`
  (12/12) for direct per-function coverage.
- Verified as of this writing: `make check-all` (normal build +
  ASan+UBSan + TSan + Valgrind) passes clean, 69/69 tests across all
  five test binaries, 0 leaks, 0 races. **Do not assume this stays
  true without rerunning it.**

### Not implemented (see Known Limitations below for the full list)

- LFU / Clock eviction policies
- Best-fit / worst-fit / next-fit / buddy-system allocation
- Flash zone management (hot/warm/cold data separation)
- Garbage collection
- Wear leveling
- Multi-device / multi-writer support
- Compression, encryption

None of the "Enterprise Features" / "Flash-Aware Storage" work
described in earlier versions of this document has been started.

---

## Architecture (as actually implemented)

```
┌─────────────────────────────────────────────┐
│               Application Layer              │
├─────────────────────────────────────────────┤
│            KES Cache Layer                   │
│  • LRU eviction (LFU/Clock: rejected, not   │
│    implemented)                             │
│  • Background dirty-page sync (real)        │
│  • Thread-safe cache operations             │
│  • Capacity-based eviction on miss          │
├─────────────────────────────────────────────┤
│            KES Storage Layer                │
│  • Extent allocation (first-fit only)       │
│  • Bitmap management                        │
│  • Thread-safe storage operations           │
│  • Storage persistence                      │
├─────────────────────────────────────────────┤
│            Storage Device/File               │
│  [Descriptor][Bitmap][User Data]             │
└─────────────────────────────────────────────┘
```

Note: `kes_cache.c` does NOT call into `kes_storage.c` -- it is an
independent layer that takes caller-supplied I/O callbacks via
`kes_cache_set_io_callbacks()`. A caller wires the two together
itself; there is no example of that wiring in this repo yet
(`examples/example_kes_usage.c` exercises the storage layer only).

---

## Build Commands & Status Verification

```bash
make all              # build build/libkes.a + build/libkes.so
make test             # build + run all 5 test binaries
make test-core        # build + run test_kes_minimal only
make check-all        # normal + ASan+UBSan + TSan + Valgrind, all binaries
make run-example      # build and run examples/example_kes_usage.c
```

There is no `kes_cache_create(size, policy, &cache)` three-argument
constructor -- an earlier version of this file showed that signature
and it never existed. The real signature is:

```c
#include <kes/kes_cache.h>

kes_cache_config_t config;
kes_cache_get_default_config(&config, false /* is_edge_device */);
config.max_memory = 64 * 1024 * 1024;   /* 64MB */
config.policy = KES_CACHE_LRU;          /* the only implemented policy */

kes_cache_t *cache = kes_cache_create(&config);
kes_cache_set_io_callbacks(cache, my_read_extent, my_write_extent,
                            my_sync_device);
kes_cache_start(cache);   /* optional: automatic background flush */

void *buffer;
kes_cache_get_extent(cache, &id, &buffer);
kes_cache_mark_dirty(cache, &id);
kes_cache_put_extent(cache, &id);

kes_cache_stop(cache);
kes_cache_destroy(cache);
```

---

## Known Limitations

See `PENDING_ITEMS.md` for the maintained, priority-ordered work
list. Summary:

- LFU/Clock eviction: not implemented, rejected at
  `kes_cache_create()`.
- Allocation strategies other than first-fit: not implemented.
- No multi-device/multi-writer protection at the storage layer.
- `make_room_for_new_entry()`'s capacity check is not strictly
  atomic under concurrent misses on different ids (documented,
  bounded overshoot -- see the function's doc comment).
- Test coverage does not yet include the full matrix described in
  `KES_HARDENING_PLAN.md` §6: systematic edge-case sweeps per
  parameter, fault injection (I/O failures, allocation failures,
  partial I/O), storage-layer crash-consistency tests, randomized/
  fuzz-adjacent testing, and a long-run soak test.
- Flash zone management, garbage collection, wear leveling,
  multi-device RAID-like features, compression/encryption: none of
  this exists. Treat any mention of it elsewhere in `docs/` as
  design-intent, not implemented behavior.

---

## Suggested Next Steps

In rough priority order (see `PENDING_ITEMS.md` for the definitive,
up-to-date list):

1. **Test expansion** (`KES_HARDENING_PLAN.md` §6.A-H) -- the
   largest remaining gap: systematic edge cases, fault injection,
   storage crash-consistency, fuzzing, soak testing.
2. **Allocation strategies** -- best-fit/worst-fit/next-fit are
   straightforward extensions of the existing first-fit code in
   `kes_storage.c`; buddy-system is a bigger design change.
3. **Multi-device/multi-writer protection** at the storage layer, if
   needed by a downstream consumer -- currently explicitly
   unguarded/undefined behavior.
4. Flash-aware features (zones, GC, wear leveling) remain
   appropriate future work, but should not be started before the
   above -- they would be built on an insufficiently-tested
   foundation otherwise.

Before starting any of the above, rerun `make check-all` and paste
the output as the baseline -- don't trust this file's or
`PENDING_ITEMS.md`'s cached claims about test status without
verifying.
