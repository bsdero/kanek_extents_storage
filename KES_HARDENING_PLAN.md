# KES Hardening, Completion & Test Plan

**For:** any LLM-based coding agent (model- and tool-agnostic — Claude
Code, Codex, Cursor, or similar)
**Target repo:** `bsdero/kanek_extents_storage` (KES)
**Prepared by:** architecture session, grounded in direct source
inspection — every claim below was verified by building the repo
and reading the actual code, not inferred from the README.

---

## 0. How to use this document

This is a work order, not a suggestion list. It is organized into
phases in dependency order — do not start a later phase before an
earlier one is verifiably complete. "Verifiably complete" means:
you built it, ran it, and are pasting the actual terminal output
as evidence. A phase is not done because you believe the code is
correct; it is done because a sanitizer-clean test run says so.

**Standing rule, non-negotiable:** this repository already
contains a document (`docs/CONTINUATION_PROMPT.md`) that marks
the cache layer as "✅ COMPLETE" with "Background Sync -
Automatic dirty page writeback" and "LRU, LFU, and Clock
algorithms" implemented. None of that is true — see §1. Do not
repeat that failure mode. Never mark a task done in this document
or in any commit message, docstring, or status file without
pasted, reproducible command output backing the claim. If you are
not sure whether something works, say so explicitly rather than
rounding up.

---

## 1. Verified ground truth (do not re-derive — start from here)

Confirmed by building with `make all` (clean, zero warnings under
`-Wall -Wextra -Werror`) and running the existing test binaries
directly:

- `build/tests/test_kes_minimal` — bitmap + storage/extent
  allocator layer. **9/9 tests pass.** This layer is in good
  shape; treat it as a stable base, not a rewrite target, except
  where §5.E below asks you to add adversarial coverage it
  currently lacks.
- `build/tests/test_kes_cache` — cache layer. **4/6 tests pass,
  2 fail**, both traced to one root cause (§2).
- `kes_cache.h` declares four functions that **do not exist** in
  `kes_cache.c`: `kes_cache_sync`, `kes_cache_invalidate`,
  `kes_cache_reset_stats`, `kes_cache_start`. Confirmed by
  grepping every top-level function definition in the `.c` file.
  The source file itself says so, verbatim, at the bottom:
  `/* Background thread and other functions would be implemented
  here... This is a representative sample of the core
  functionality. */` — take that comment at face value.
- **No eviction or capacity enforcement exists anywhere.**
  `max_entries` / `max_memory` are validated once at
  `kes_cache_create()` and never checked again.
  `kes_cache_get_extent()` unconditionally grows the cache on
  every miss. The LRU list (`mru_head`/`lru_tail`) is correctly
  maintained but never consulted to evict anything.
- `kes_cache_policy_t` offers `KES_CACHE_LRU`, `KES_CACHE_LFU`,
  `KES_CACHE_CUSTOM`. `config.policy` is stored at creation and
  **never read again anywhere in the file**. There is no LFU
  logic and no Clock algorithm despite both being named in
  `docs/CONTINUATION_PROMPT.md`. Only LRU list bookkeeping
  exists, and per the point above, even that isn't acted on yet.
- `kes_cache_start()` not existing means `bg_threads` is never
  allocated or spawned. `kes_cache_stop()` guards this safely
  (`if (cache->bg_threads)`), so nothing crashes — but the
  entire background-sync story in the README and design docs is
  currently inert. Nothing runs in the background today.
- `extent_alloc_data()` / `extent_free_data()` already use
  `aligned_alloc()` and correctly maintain a running
  `stats.memory_used` counter. This is good, working
  infrastructure — reuse it for capacity checks in §4 rather
  than inventing a parallel accounting mechanism.
- The build has **no sanitizer or Valgrind targets** — `make
  debug` only sets `-g3 -O0 -DDEBUG`, nothing more.

Root cause of both current test failures (§2 has the fix):
`tests/test_kes_cache.c` defines `test_data_t` as
`{ uint8_t pattern[8192]; uint32_t checksum; }` — 8196 bytes.
The cache allocates the buffer for that extent as exactly
`block_count * block_size` = 8192 bytes
(`extent_data_size()`, `src/kes_cache.c:88-90`). The mock I/O
callbacks (`mock_read_extent`/`mock_write_extent`) copy exactly
`size` (8192) bytes. The test then casts the 8192-byte buffer to
`test_data_t*` and reads/writes the `checksum` field at byte
offset 8192 — **four bytes past the end of the actual heap
allocation.** This is a real out-of-bounds read (and, in the
write-back path, a real heap buffer overflow), not just a logic
bug. It happens to "only" read/write 4 bytes of adjacent heap
memory today, on this allocator, on this platform — that is luck,
not correctness, and is exactly the kind of latent bug
AddressSanitizer exists to catch before it becomes a real
corruption in a different allocator or build.

---

## 2. Phase 1 — Fix the confirmed memory-safety bug (P0)

**2.1 — Fix the buffer/struct size mismatch in the test harness.**

Do not "fix" this by silently enlarging the cache's internal
buffer allocation to 8196 bytes to match the test — that treats
a test-only convenience struct as if it were part of the real
extent contract, and it will re-break the moment block_size
changes. Instead, separate the two concerns cleanly:

- The cached buffer stays exactly `block_count * block_size`
  bytes — that is the real contract the cache library makes with
  its caller, and no test should quietly bend it.
- Move the checksum out of the buffer the cache manages. Keep
  `g_mock_storage[]` as the source of truth for expected data
  (it already is), and compute/compare the checksum **against
  the mock storage's copy directly**, not against a trailer field
  smuggled inside the cached buffer. Concretely: change
  `validate_test_data()` (and any call site using the
  buffer-cast-to-`test_data_t*` pattern) to take the raw
  `pattern` bytes and the extent's `start_block`, recompute the
  checksum from `g_mock_storage[start_block]`, and compare — no
  struct overlay on the cache's buffer at all.
- Grep the whole test file for every place that does
  `(test_data_t*)buffer` and fix each one the same way. There
  are at least two (`test_basic_operations` at
  `tests/test_kes_cache.c:225`, and
  `concurrent_access_thread` at `tests/test_kes_cache.c:402`) —
  find any others.

**2.2 — After the fix, re-run under sanitizers, not just
normal build.** This is important: fixing the checksum logic
error will very likely make both currently-failing tests report
PASS again, but that is not sufficient evidence that the
concurrent-access failure was *only* this bug. Do §3 (sanitizer
targets) before declaring Phase 1 done, then run
`test_kes_cache` under ThreadSanitizer specifically. If TSan
reports any data race, treat it as a new, separate P0 bug — root
cause it properly, do not assume it's "probably the same issue."

**Acceptance for Phase 1:** paste the full output of `make test`
showing all prior test binaries at 100% pass, plus the TSan run
from §3.2 showing zero race reports on `test_kes_cache`.

---

## 3. Phase 2 — Build tooling hardening (P0, blocks everything after)

Add these before writing new concurrency code, not after — you
want every subsequent phase automatically checked.

**3.1 — Add sanitizer build variants to the Makefile.** ASan and
TSan cannot be linked into the same binary; build them as
separate targets/output directories:

```makefile
# Add near the existing DEBUG conditional block
ASAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g
TSAN_FLAGS = -fsanitize=thread -fno-omit-frame-pointer -g

.PHONY: asan tsan sanitize-all
asan:
	$(MAKE) clean
	$(MAKE) all tests CFLAGS="$(CFLAGS) $(ASAN_FLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(ASAN_FLAGS)"
	@echo "Run: ./build/tests/test_kes_cache && \
	    ./build/tests/test_kes_minimal"

tsan:
	$(MAKE) clean
	$(MAKE) all tests CFLAGS="$(CFLAGS) $(TSAN_FLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(TSAN_FLAGS)"
	@echo "Run: ./build/tests/test_kes_cache && \
	    ./build/tests/test_kes_minimal"

sanitize-all: asan tsan
	$(MAKE) clean
	$(MAKE) all
```

(Adjust to fit the existing Makefile's variable names exactly —
read the current file before patching; don't assume the sketch
above matches variable names byte-for-byte.)

**3.2 — Verify Valgrind is available in the build environment**
and add a `valgrind` target running each test binary under
`valgrind --leak-check=full --error-exitcode=1
--track-origins=yes`. Valgrind and ASan overlap in purpose but
Valgrind's leak-check mode is still worth having as a second,
independent tool — different implementations catch different
things.

**3.3 — `make check-all`** — a single target that builds and
runs: normal build tests, ASan build tests, TSan build tests,
Valgrind pass. Fails loudly (non-zero exit) if any step fails.
This becomes the one command you run before claiming any phase
below is complete.

**Acceptance for Phase 2:** paste `make check-all` output,
currently expected to still show the Phase-1 fix's test results
clean under all four tool configurations.

---

## 4. Phase 3 — Implement the four missing functions (P0/P1)

Implement exactly the semantics below. These were derived
deliberately in the design process that preceded this plan —
follow them precisely rather than substituting a
seemingly-equivalent variant, because several of the choices
below (particularly in `sync()` and `invalidate()`) resolve real
correctness hazards that a plausible-looking alternative would
reintroduce.

**4.1 — `kes_cache_sync(kes_cache_t* cache)`**

For every entry currently in the cache:

1. If `state & KES_EXTENT_DIRTY`: call `cache->write_extent`
   under the **entry's own lock** (`entry->lock`), not the
   global `cache->cache_lock`. On success, clear the dirty flag,
   increment `stats.flushes`, decrement `stats.entries_dirty`.
   On failure, set `KES_EXTENT_ERROR`, leave dirty set, and
   record the failure (do not silently drop it) — this entry is
   not eligible for eviction below this round.
2. If `ref_count == 0 && pin_count == 0` (checked **after** the
   flush attempt, under the entry's lock): remove the entry from
   its hash bucket and from the LRU list, free its data buffer
   via `extent_free_data`, destroy its mutex/cond, free the
   entry struct, decrement `stats.entries_cached`.
3. Entries with `ref_count > 0` or `pin_count > 0` are flushed
   if dirty but **never freed** — they are still in active use.
   This is correct behavior for `sync()` being called while
   other callers hold live references; it is a durability
   checkpoint, not a full eviction pass.

**A concurrency hazard to design carefully, not hand-wave:**
holding `cache->cache_lock` for the entire duration of this
sweep — including while blocked inside the `write_extent` I/O
callback — would serialize every unrelated `get_extent`/
`put_extent` call on the whole cache behind however long the
disk write takes. That defeats the purpose of per-entry locking
that `kes_cache_get_extent` already uses elsewhere in this file.
Match that existing discipline: acquire `cache_lock` only for the
list/hash structural traversal and mutation steps; drop it before
calling into `write_extent`; use the target entry's own
`entry->lock` to guard its state during the write. Because
walking a linked list while releasing and reacquiring locks
between nodes creates a use-after-free risk if another thread
frees a node you're about to visit next, you must snapshot the
traversal safely — e.g. take a reference (bump `ref_count`) on
the current node before dropping `cache_lock` to do its I/O, and
release that reference before advancing, so no node you're
holding a pointer to can be freed out from under you mid-walk by
a concurrent operation. Write a short comment in the code
explaining this invariant; a future reader (human or otherwise)
needs to understand why the extra refcount bump exists at that
specific point or a "simplifying" edit will silently reintroduce
the race.

**4.2 — `kes_cache_invalidate(kes_cache_t* cache, const
kes_extent_id_t* id)`**

Look up the entry. If not found, return `KES_ERROR_NOTFOUND`.
If `ref_count > 0 || pin_count > 0`, return `KES_ERROR_BUSY`
(this error code already exists in the header specifically for
cases like this — use it, don't invent a new one). Otherwise,
remove and free the entry **unconditionally, discarding any
dirty data without writing it back** — that is the semantic
difference between `invalidate` (discard) and `sync`/
`flush_extent` (persist). Do not add an implicit flush-before-
discard; if the caller wanted the data persisted they should
call `flush_extent` first. Document this data-loss behavior
clearly in the function's doc comment so it isn't mistaken for a
gentler operation later.

**4.3 — `kes_cache_reset_stats(kes_cache_t* cache)`**

Lock `cache_lock`. Reset only the **cumulative counters**:
`hits`, `misses`, `evictions`, `flushes`, `bytes_read`,
`bytes_written`. Do **not** reset `memory_used`,
`entries_cached`, `entries_dirty`, `entries_pinned` — those
describe current state, not history, and zeroing them would
desynchronize the stats structure from the cache's actual
contents until the next operation happened to correct it. Getting
this distinction wrong is an easy, quiet bug — write a unit test
(§5.A) that specifically checks state counters survive a
`reset_stats()` call unchanged while counters reset to zero.

**4.4 — `kes_cache_start(kes_cache_t* cache)`**

Allocate `cache->bg_threads` for `config.background_threads`
threads. Each thread runs a loop: wait on `cache->bg_cond` with a
**timed** wait of `config.sync_interval_ms`, using the same
`cache->cache_lock` mutex that `kes_cache_stop()` already pairs
with `bg_cond` — do not introduce a second, different
synchronization primitive that `stop()` doesn't know about.

Important existing-code detail to check before writing this:
`pthread_cond_init(&cache->bg_cond, NULL)` currently uses the
default clock, which on most platforms is `CLOCK_REALTIME` for
timed waits. A timed wait against `CLOCK_REALTIME` is vulnerable
to system clock adjustments (NTP steps, manual clock changes)
causing the wait to fire early or absurdly late. Before adding
the first timed wait in this codebase, switch cond-var
initialization to use `pthread_condattr_t` with
`pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)`, and use
`clock_gettime(CLOCK_MONOTONIC, ...)` to compute the absolute
deadline passed to `pthread_cond_timedwait`. This is a small
change but get it right now — retrofitting it later means
auditing every timed wait added in between.

On each wake (whether by timeout or by `pthread_cond_broadcast`
from `stop()`), check `cache->shutdown` first; if set, exit the
loop without doing another sweep. Otherwise, run the same
flush-then-conditionally-free sweep logic as `kes_cache_sync()`
— factor the shared logic into one internal static function
that both `kes_cache_sync()` and the background thread call, so
behavior can't drift between the manual and automatic paths.

**Acceptance for Phase 3:** unit tests for each of the four
functions (see §5.A), passing under the normal build, ASan, and
TSan. Specifically for `kes_cache_start`: a test that starts the
background thread(s), dirties several entries, sleeps past
`sync_interval_ms`, and asserts the entries were flushed
automatically without any explicit `sync()`/`flush_extent()`
call — this is the one behavior the whole background-thread
design exists to deliver, so it needs a test that actually
proves it happens, not just that the thread doesn't crash.

---

## 5. Phase 4 — Capacity enforcement / real eviction (P1)

Currently the cache grows without bound. Implement actual
enforcement of `config.max_entries` and `config.max_memory`.

**5.1 — Eviction on miss.** In `kes_cache_get_extent()`'s
cache-miss path, before allocating and inserting the new entry,
check whether inserting it would exceed `config.max_entries`
(`stats.entries_cached + 1 > max_entries`) or
`config.max_memory` (using the already-correct
`stats.memory_used` running counter — reuse it, don't add a
second accounting path). If either limit would be exceeded, walk
the LRU list from `lru_tail` toward `mru_head`, evicting entries
with `ref_count == 0 && pin_count == 0` (flushing first if
dirty, same as the sync logic — reuse that factored-out internal
function from §4.1/4.4) until there is room, incrementing
`stats.evictions` for each one actually evicted.

**5.2 — Decide and document what happens when eviction can't
free enough room** (every cached entry is pinned or actively
referenced). My recommendation: return `KES_ERROR_BUSY` from
`kes_cache_get_extent()` rather than silently exceeding the
configured limit — a configured memory ceiling that can be
silently violated isn't really a ceiling, and on the edge
targets this project is aimed at, that's not a hypothetical
concern. If you implement it differently (e.g., allow a bounded
overshoot), that is a real design deviation from the spec here —
flag it explicitly rather than making the call silently.

**5.3 — `config.policy` currently does nothing.** Do not
implement LFU or a Clock algorithm speculatively. Wire up LRU
only (which the eviction walk above already does, by construc-
tion, since it walks the LRU list) and leave `KES_CACHE_LFU`/
`KES_CACHE_CUSTOM` unimplemented but **explicitly documented as
such** — add a doc comment on `kes_cache_policy_t` noting only
`KES_CACHE_LRU` is currently implemented, and make
`kes_cache_create()` return `KES_ERROR_INVALID` if a config
requests `KES_CACHE_LFU` or `KES_CACHE_CUSTOM`, rather than
silently accepting the config and behaving as if LRU were
selected. Silently ignoring a configuration option is worse than
refusing it outright — it hides a mismatch between what the
caller asked for and what they got.

**Acceptance for Phase 4:** a test that creates a cache with a
small `max_entries`, fills it past that limit through repeated
distinct-extent `get_extent`/`put_extent` calls, and asserts
`stats.entries_cached` never exceeds the configured maximum and
`stats.evictions` increments correctly. A second test confirming
pinned entries are never evicted even under memory pressure. A
third confirming `kes_cache_create()` rejects
`KES_CACHE_LFU`/`KES_CACHE_CUSTOM` per §5.3.

---

## 6. Phase 5 — Comprehensive test expansion (P1/P2)

This is the largest phase and the main point of this exercise —
build out real coverage, not just enough to turn the existing
suite green. Organize new tests into the categories below,
either as new files (`tests/test_kes_cache_edge.c`,
`tests/test_kes_storage_stress.c`, etc.) or appended to the
existing suites — match whichever the existing `tests/`
structure and Makefile test-target pattern makes cleaner, and
wire each new file into the Makefile's `TEST_TARGETS` the same
way the existing two are.

### 6.A — Functional coverage per public function

Every function in `kes_cache.h`, `kes_storage.h`, and
`kes_bitmap.h` needs at least one direct, isolated test beyond
whatever incidental coverage it gets from higher-level tests.
Build a checklist from the three headers and confirm each entry
has: a success-path test, and a failure-path test for every
documented error return in its doc comment (`KES_ERROR_INVALID`,
`KES_ERROR_NOTFOUND`, etc.). Where a function's doc comment
lists an error code that nothing in the test suite ever
triggers, that's a coverage gap — close it.

### 6.B — Edge cases

At minimum, cover all of the following. Where one doesn't apply
to a given function, skip it for that function, but check every
one against every relevant function rather than assuming it
doesn't apply:

- NULL pointer for every pointer parameter, individually (not
  just "pass NULL cache" — also NULL `id`, NULL `buffer`, NULL
  `stats`, etc., one at a time).
- Zero-sized requests: `block_count = 0` in an extent request or
  ID.
- Maximum values: `block_count = UINT32_MAX`,
  `start_block = UINT64_MAX`, and combinations that would
  overflow `start_block * block_size` in 64-bit arithmetic —
  confirm `extent_data_size()` and any offset math is checked
  for overflow rather than trusting it silently wraps safely.
- `block_size` values outside `KES_MIN_BLOCK_SIZE`..
  `KES_MAX_BLOCK_SIZE`, and values that are not a power of two
  (there's already a `KES_IS_POWER_OF_2` macro — confirm it's
  actually used to validate input somewhere, and add the check
  plus a test if it isn't).
- Double-free equivalents: calling `kes_cache_put_extent()` more
  times than the matching `get_extent()` calls (refcount
  underflow) — confirm the existing `if (ref_count > 0)` guard
  in `kes_cache_put_extent` is actually exercised by a test that
  calls `put_extent` on an already-zero-refcount entry and
  asserts it does not underflow to a huge unsigned value.
- Pin/unpin imbalance: unpinning more times than pinned; pinning
  the same entry many times and confirming a single unpin
  doesn't fully unpin it (this behavior isn't spelled out
  anywhere I could find in the current code or docs — read
  `kes_cache_pin_extent`/`unpin_extent`'s actual current
  behavior first, since I have not personally verified whether
  pin is refcounted or boolean, and write the test to match
  whichever the code actually does, then document that decision
  explicitly since it's currently undocumented).
- Getting an extent, never releasing it, then calling
  `kes_cache_destroy()` — confirm this doesn't leak or corrupt
  and produces a clean (if perhaps warned-about) shutdown rather
  than silently freeing memory a caller still logically holds a
  reference to.
- Calling any operation on a cache immediately after
  `kes_cache_stop()` but before `kes_cache_destroy()`.
- `kes_cache_sync()`/`invalidate()`/`flush_extent()` called on
  an empty cache (zero entries) — should be a clean no-op, not
  an error.
- I/O callback set to `NULL` (never call
  `kes_cache_set_io_callbacks`) and then triggering a cache
  miss — confirm this fails gracefully rather than
  null-pointer-calling through `cache->read_extent`.
- Extent identifiers that collide in the hash table (same hash,
  different actual id) — confirm `kes_extent_equal` is actually
  used to disambiguate on lookup and not just the hash value.
- For the storage/bitmap layer: allocating every single block
  (100% full), then requesting one more extent — confirm a
  clean `KES_ERROR_NOSPACE` rather than any corruption of the
  bitmap; then freeing one block and confirming allocation
  succeeds again.
- Storage layer: opening the same underlying file/device twice
  concurrently (two `kes_storage_t*` instances) — this is
  currently, as far as I can tell from reading the code, entirely
  unguarded; at minimum, write a test that documents current
  behavior (even if that behavior is "undefined/unsafe, do not
  do this") so it's a known, recorded limitation rather than a
  silent trap. Whether to actually add single-writer enforcement
  is a design decision beyond this plan's scope — flag it back
  rather than deciding unilaterally.

### 6.C — Concurrency & stress tests

- Re-run the existing concurrent-access test pattern but scale
  it up significantly: more threads than CPU cores, higher
  iteration counts, and — critically — mixed operation types per
  thread (some threads doing get/put, others doing
  pin/unpin/mark_dirty/flush_extent, others calling `sync()`
  repeatedly in a loop) all against the *same* small pool of
  extent IDs, to maximize contention and interleaving on the
  same entries rather than each thread working on disjoint data.
- A dedicated test for the exact hazard called out in §4.1: one
  thread calling `sync()` repeatedly in a tight loop while
  another thread continuously acquires and releases the same
  extent — this is the specific interleaving that would expose
  a use-after-free if the "hold a reference during the sweep's
  I/O" invariant from §4.1 is implemented incorrectly.
  Run this specific test under TSan and ASan every time, not
  just in the general suite — it is the highest-value single
  test in this entire plan given the bug class this project
  cares most about.
- `kes_cache_destroy()` called from one thread while another
  thread is mid-`get_extent()`/`put_extent()` — confirm this is
  either safely rejected/synchronized or explicitly documented
  as caller-must-quiesce-first, and write the test to match
  whichever contract you settle on.
- Two threads racing to be the one that populates a cache miss
  for the *same* extent ID simultaneously (both call
  `get_extent()` on an uncached id at nearly the same instant) —
  confirm only one actually issues the `read_extent` I/O call
  and the other correctly waits and receives the same data,
  rather than both racing to insert duplicate hash-table entries
  for the same id.
- Background-thread sync (§4.4) running concurrently with
  application threads calling `mark_dirty`/`flush_extent`/
  `invalidate` on the same entries the background sweep is
  currently visiting.
- Run every concurrency test at least 100 times in a loop (or
  under a stress-test target that repeats the whole suite N
  times) before considering it reliable — a single green run of
  a concurrency test proves very little; flaky concurrency bugs
  routinely need dozens of runs to surface. Use a **fixed random
  seed**, logged at the start of each run, for any test that
  uses randomized delays or randomized operation ordering, so
  that any failure that does surface can be reproduced
  deterministically by re-running with the same seed rather than
  chased as a one-off.

### 6.D — Fault injection

- Wrap the mock I/O callbacks with a configurable failure mode:
  return an I/O error on the Nth call, or on every call after
  some point, or with some probability. Confirm the cache
  transitions affected entries to `KES_EXTENT_ERROR` correctly,
  that `get_extent()` on a load failure doesn't leave a
  corrupted/half-inserted entry behind in the hash table, and
  that a subsequent retry (after the injected failure condition
  clears) succeeds cleanly rather than being permanently wedged
  by the earlier error state.
- Simulate `malloc`/`aligned_alloc` failure (e.g., via a
  test-only allocator wrapper or by requesting an
  unreasonably large `max_memory`/entry size) and confirm
  `KES_ERROR_NOMEM` propagates cleanly without leaking whatever
  partial state was allocated before the failure point.
- Partial writes/reads from the mock I/O layer (callback returns
  success but only "transfers" fewer bytes than requested,
  simulating what a real short read/write would look like at a
  lower layer) — confirm this is either detected and treated as
  an error, or document why it's considered out of scope at this
  layer (it may legitimately be a Layer-0 I/O concern rather
  than a Layer-1 cache concern — if so, say that explicitly
  rather than leaving it untested and unaddressed).

### 6.E — Persistence / crash-consistency (storage layer)

- Write data, close the storage handle without an explicit sync,
  reopen, and confirm the documented durability guarantee (or
  lack thereof) actually matches what's specified in
  `KES_STORAGE_SYNC` flag's doc comment.
- Simulate a crash mid-write to the storage descriptor (block 0)
  by writing a deliberately truncated or corrupted descriptor
  and confirming `kes_storage` detects the corruption (magic
  number / version mismatch) rather than proceeding with garbage
  geometry values.
- Bit-flip a single byte in a persisted bitmap block and confirm
  the failure mode on reopen is a clean, reported error rather
  than an out-of-bounds allocation later caused by trusting
  corrupted free/used bit data.

### 6.F — Randomized / fuzz-adjacent testing

- A test that issues a long random sequence of operations
  (get/put/pin/unpin/mark_dirty/flush/sync/invalidate) against a
  small, fixed pool of extent IDs, checking invariants after
  every single operation rather than just at the end: ref_count
  never negative, pin_count never negative, `entries_cached`
  matches the actual live hash-table population, `memory_used`
  matches the actual sum of live buffer sizes. Log the random
  seed used so any invariant violation is reproducible.
- Feed `kes_bitmap`'s bit-manipulation functions (the ported/
  reference equivalent of `map.h`, if present in this repo — if
  it isn't, note that as a gap, since `map.h` was designed
  as this library's bitmap primitives and should have direct
  test coverage here) randomized bit ranges, including ranges
  that start or end exactly at a byte boundary, span multiple
  bytes, and cover the entire bitmap — compare results against
  a naive, obviously-correct reference implementation (e.g. a
  loop that checks/sets one bit at a time) rather than only
  checking against itself.

### 6.G — Performance smoke tests (informational, not blocking)

- A simple throughput benchmark (sequential gets, random gets,
  mixed read/write) run before and after this plan's changes,
  to catch an accidental large regression introduced by, e.g.,
  the new eviction-on-every-miss check. These do not need to
  gate merges the way correctness tests do, but the numbers
  should be recorded in the PR/commit description so a future
  regression has a baseline to compare against.

### 6.H — Long-run soak test

- A test (can be opt-in / not part of the default fast suite,
  e.g. gated behind a `make soak` target or an environment
  variable) that runs the mixed-operation concurrent workload
  from §6.C continuously for a fixed long duration (start with
  10 minutes) under ASan or TSan, watching for anything that
  only surfaces after sustained operation — slow leaks, gradual
  fragmentation, counters drifting out of sync over many cycles.

**Acceptance for Phase 5:** `make check-all` (from §3.3) passing
with the full expanded suite, plus a short written summary (in
the PR description or a new `docs/TEST_COVERAGE.md`) of what
each new test file covers and any gaps intentionally left for a
future pass, clearly labeled as such.

---

## 7. Phase 6 — Documentation truth pass (P2)

- Correct `docs/CONTINUATION_PROMPT.md` and the README's status
  badges to reflect only what is actually verified at the end of
  this plan. Remove or correct the "LRU, LFU, and Clock
  algorithms" and "Background Sync - Automatic dirty page
  writeback" claims to match what was actually implemented in
  Phases 3-5.
- Add a "Known Limitations" section listing anything
  deliberately deferred: `KES_CACHE_LFU`/`KES_CACHE_CUSTOM`
  unimplemented (rejected at `create()` time, §5.3), no
  multi-device/multi-writer protection (§6.B storage-layer
  note), and anything else that came up during this work that
  wasn't in scope to fully resolve.
- Update the test-count badge to reflect the real, current
  total across all test binaries, not just `test_kes_minimal`.

---

## 8. Constraints for this work

- **Match existing style exactly**: the codebase is C99,
  4-space indent (or whatever the actual prevailing indent is —
  check `.editorconfig` or infer from surrounding code, don't
  assume), `kes_` prefix on all public symbols, existing error
  code enum — do not introduce new dependencies beyond `pthread`
  and the C standard library without flagging that decision
  explicitly first.
- **No drive-by rewrites.** If you notice something you believe
  is a bug or bad practice outside the scope of the current
  phase, note it (in a comment, or in a running "Findings" doc)
  rather than fixing it inline in an unrelated commit — keep
  each commit's diff readable and scoped to one phase/task.
- **One logical change per commit.** A reviewer (human or
  otherwise) needs to be able to look at a commit and its
  associated test-run evidence and evaluate it on its own.
- **Do not touch `kes_storage.c`/`kes_bitmap.c` public
  signatures** unless a bug found during §6.B/6.E genuinely
  requires it — and if so, call that out explicitly as an API
  change, not a quiet edit, since anything built on top of this
  library depends on that surface staying stable.
- **Every phase's acceptance criteria must be satisfied with
  pasted command output before moving to the next phase.** This
  is the direct countermeasure to the failure mode documented in
  §1 — this plan exists partly because a prior pass through this
  codebase marked things "complete" that were not, and that
  should not happen again.

---

## 9. Suggested execution order

Run this as a sequence of focused, independently-reviewable
passes rather than one large session — each phase below produces
its own commit(s) and its own pasted verification output, and
later phases should not start until earlier ones are confirmed
green under `make check-all`:

1. Phase 1 (bug fix) + Phase 2 (sanitizer tooling) together —
   the fix needs the tooling to be properly verified anyway.
2. Phase 3 (four missing functions).
3. Phase 4 (eviction/capacity enforcement) — depends on the
   shared sweep logic factored out in Phase 3.
4. Phase 5 (test expansion) — can partially overlap with Phase 3
   and 4 in practice (writing the test for a function as you
   implement it is fine and encouraged), but the full matrix in
   §6 should be treated as its own reviewable pass at the end to
   make sure nothing was skipped.
5. Phase 6 (docs) last, once everything it describes is actually
   true.
