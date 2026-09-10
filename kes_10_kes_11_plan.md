# KES-10 + KES-11 Implementation Plan — Documented Limitations, Deferred Work

**Read this whole file before touching anything.** Like
`kes_7_kes_9_plan.md`, this pairing is different in kind from
KES-2/3/4/5/6/8: both decisions here (confirmed with the project
owner) were to document a known limitation rather than change code.
KES-10 has a concrete doc-comment deliverable below; KES-11 has none
at all beyond a bookkeeping note — do not invent work for it.

## What these are

Both from `PENDING_FIXES_SEP2026.md`:

- **KES-10** (Low, design limitation): the `read_extent`/
  `write_extent` I/O callback contract (`include/kes/kes_cache.h`) is
  a plain `int` status code against a fixed `size` *input* parameter,
  with no bytes-actually-transferred *output* channel. A callback that
  reports `KES_SUCCESS` while only partially filling/consuming the
  buffer is silently trusted — confirmed structural, not a bug, by
  `test_partial_transfer_not_detected`
  (`tests/test_kes_fault_injection.c`). **Decision confirmed with the
  project owner: document only, do not extend the callback signature.**
  Even though fixing it now (before either downstream project
  integrates its own callback) is free in a way it won't be later,
  the two backends this project actually ships today don't do partial
  transfers, and an API-breaking change should wait until a concrete
  need (e.g. one of the two downstream projects actually building a
  network-backed callback) exists to design against, rather than
  guessing at the right shape now.
- **KES-11** (Low, intentionally deferred): performance smoke tests
  (`KES_HARDENING_PLAN.md` §6.G) were never implemented —
  `plan_phase5.md` explicitly marked this optional/stretch,
  non-blocking. **Decision confirmed with the project owner: keep
  deferring, do not implement even a minimal version now.** No real
  consumer workload exists yet to benchmark against; any target
  written today would be invented, not real, and would risk becoming
  a false signal (a "regression" against a made-up baseline nobody
  asked for, or false confidence from a synthetic benchmark that
  doesn't resemble either downstream project's actual access pattern).

## Files you will touch

- `include/kes/kes_cache.h` — doc-comment additions only (KES-10)
- `PENDING_FIXES_SEP2026.md` — reworded status/trigger condition for
  KES-11 (bookkeeping only, no plan content of its own)

---

## Step 1 (KES-10) — document the partial-transfer limitation

### 1a. `struct kes_cache`'s callback fields

Current code (`include/kes/kes_cache.h`):

```c
    /* I/O callback functions */
    int (*read_extent)( void *device, const kes_extent_id_t *id,
                         void *buffer, size_t size);
    int (*write_extent)( void *device, const kes_extent_id_t *id,
                          const void *buffer, size_t size);
    int (*sync_device)( void *device);
```

Replace with:

```c
    /* I/O callback functions. KES-10: read_extent/write_extent's
     * contract is a plain int status code against a fixed input
     * `size` -- there is no output parameter for bytes actually
     * transferred. A callback that returns KES_SUCCESS while only
     * partially filling/consuming `buffer` (e.g. a network-backed
     * implementation doing a short read/write) is silently trusted;
     * the cache layer has no way to detect this. A callback MUST
     * either fully transfer `size` bytes or return a non-KES_SUCCESS
     * error code -- confirmed structural (not just untested) by
     * test_partial_transfer_not_detected
     * (tests/test_kes_fault_injection.c). Known, accepted API
     * limitation (see kes_10_kes_11_plan.md / PENDING_FIXES_SEP2026.md
     * KES-10) -- fixing it would need a breaking change to these
     * signatures (an added bytes-transferred output parameter),
     * deliberately not done as of this writing. Matters most for a
     * callback backed by something other than a local file (e.g. a
     * network transport), where partial transfers are a real
     * possibility, not just a theoretical one. */
    int (*read_extent)( void *device, const kes_extent_id_t *id,
                         void *buffer, size_t size);
    int (*write_extent)( void *device, const kes_extent_id_t *id,
                          const void *buffer, size_t size);
    int (*sync_device)( void *device);
```

### 1b. `kes_cache_set_io_callbacks()`'s doc comment

Current code:

```c
/**
 * Set I/O callback functions
 * @param cache Cache handle
 * @param read_func Function to read extents from storage
 * @param write_func Function to write extents to storage
 * @param sync_func Function to sync storage device
 * @return KES_SUCCESS or error code
 */
int kes_cache_set_io_callbacks( kes_cache_t *cache,
    int (*read_func)( void *device, const kes_extent_id_t *id,
                       void *buffer, size_t size),
    int (*write_func)( void *device, const kes_extent_id_t *id,
                        const void *buffer, size_t size),
    int (*sync_func)( void *device));
```

Replace with:

```c
/**
 * Set I/O callback functions. KES-10: read_func/write_func must
 * fully transfer `size` bytes on success or return an error -- there
 * is no way for the cache layer to detect a partial transfer that
 * still reports KES_SUCCESS (see the struct kes_cache doc comment on
 * read_extent/write_extent above for the full explanation).
 * @param cache Cache handle
 * @param read_func Function to read extents from storage
 * @param write_func Function to write extents to storage
 * @param sync_func Function to sync storage device
 * @return KES_SUCCESS or error code
 */
int kes_cache_set_io_callbacks( kes_cache_t *cache,
    int (*read_func)( void *device, const kes_extent_id_t *id,
                       void *buffer, size_t size),
    int (*write_func)( void *device, const kes_extent_id_t *id,
                        const void *buffer, size_t size),
    int (*sync_func)( void *device));
```

No test changes for this step — `test_partial_transfer_not_detected`
already covers and asserts this exact behavior; this step only
documents it where a caller writing their own callback would actually
read it, without changing anything the test checks.

**If this decision is ever revisited** (a downstream project actually
needs partial-transfer detection): the shape to design toward, per
`PENDING_FIXES_SEP2026.md`'s own framing, is adding a
bytes-transferred output parameter to both callback typedefs — a
breaking signature change to `struct kes_cache`'s `read_extent`/
`write_extent` members and to `kes_cache_set_io_callbacks()`'s
parameters, touching every existing callback implementation in this
codebase (`fi_read`/`fi_write` in `tests/test_kes_fault_injection.c`,
and any others in the test suite/examples) plus `kes_cache_get_extent()`/
`kes_cache_flush_extent()`/`sweep_flush_and_maybe_evict()`'s call
sites. Not designed further here — this paragraph exists only so a
future implementer doesn't have to rediscover the blast radius from
scratch.

---

## Step 2 (KES-11) — reword the deferral, no implementation

**No code change.** Update `PENDING_FIXES_SEP2026.md`'s `## KES-11`
section (do this directly, it is the bookkeeping deliverable for this
step) to sharpen the trigger condition for when this should actually
get picked up, replacing its current "revisit once real consumers
exist" with something a future reader can act on without guessing:

> **Severity: Low. Status: OPEN, intentionally deferred.** Confirmed
> with the project owner on `<date you apply this>`: continue
> deferring rather than write synthetic smoke tests now. **Concrete
> trigger to revisit**: either downstream project (or this project's
> own maintainers) has a real, specific workload characteristic to
> benchmark against — e.g. "extent allocate/free must sustain N ops/
> sec under M concurrent callers" or "cache get/put p99 latency must
> stay under X ms at Y entries" — at which point write a smoke test
> against *that* number, not an invented one. Until such a number
> exists, a synthetic benchmark risks becoming a false signal: a
> "regression" against a baseline nobody asked for, or false
> confidence from a synthetic pattern that doesn't resemble either
> downstream project's actual access pattern. See `kes_10_kes_11_plan.md`.

Do not write any smoke-test code as part of this step. If you find
yourself drafting a benchmark "just to have something," stop — that is
exactly the outcome this decision was made to avoid.

---

## Verification checklist

1. `make clean && make test` — all tests still pass (both steps here
   make zero behavior changes).
2. Re-read the new `kes_cache.h` comments once more after writing them
   and confirm they match what `test_partial_transfer_not_detected`
   actually demonstrated, not a plausible-sounding extrapolation of
   it — same discipline as `kes_7_kes_9_plan.md` Step 1's verification
   note.

## Explicitly out of scope

- Extending the `read_extent`/`write_extent` callback signature —
  KES-10's decision above is explicit about this; Step 1's closing
  paragraph sketches the shape for later, but implementing it is not
  part of this plan.
- Any performance smoke test, minimal or otherwise — KES-11's decision
  above is explicit about this.

## Bookkeeping when done

- KES-10: move `PENDING_FIXES_SEP2026.md`'s `## KES-10` section status
  to reflect "documented" rather than "OPEN" with no recent decision
  behind it (same framing as KES-9's bookkeeping note in
  `kes_7_kes_9_plan.md`) — this is not a bug fix with a `CLOSED`
  commit hash the way KES-1 through KES-4 are, so don't mark it
  `CLOSED` in that sense; record the date and link to this plan
  instead. Add a short pointer entry to `PENDING_ITEMS.md` too, for
  consistency with how every other item in that file is tracked.
- KES-11: the reword in Step 2 above *is* the bookkeeping for this
  item — nothing further needed beyond actually making that edit.
