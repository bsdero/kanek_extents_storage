# macOS Port Work Plan

**Status: DONE — Phases 0-5 executed and verified on the actual
target machine, see §7 for the per-phase bookkeeping.** This was an
execution plan, not a status report while in progress; §7 below is
now the record of what was actually done and re-verified, not a
prediction.

**Was blocked on:** the open items in `PENDING_FIXES_SEP2026.md`
(`KES-2` through `KES-12`; `KES-1` is already closed) landing on the
primary development platform (Linux) first, before this work started.
By the time this pass began, KES-1 through KES-8 and KES-12 were all
CLOSED, and KES-9/10/11 were confirmed low-severity/deferred and not
touching the cache/storage/multiprocess-test files this port touches
in a way that would need rework — so the blocking condition below was
satisfied.

Original rationale for the block, kept for context: several of those
items touch the same files this plan touches (`src/kes_cache.c`,
`src/kes_storage.c`, `tests/test_kes_multiprocess.c`), and fixing them
first, on the platform where `make check-all` (ASan/TSan/Valgrind)
already ran reliably, avoided doing this port's platform-specific work
twice or against a moving target. As anticipated, this plan did need
some rework once those fixes landed and once real execution surfaced
gaps neither the `KES-*` fixes nor this plan's original drafting had
anticipated — see §7 for exactly what came up beyond the "Confirmed
blockers" below (a `USER_SPACE`/`crc32c.h` compile blocker,
`pthread_barrier_t`, the 200GiB `aligned_alloc()` NOMEM case, and one
non-platform-specific UBSan-confirmed bug).

**Audience:** any LLM coding agent or human picking up this port.
Assume zero prior context beyond this repo: read `AGENTS.md` first
(module layering, error-handling convention, build system,
`CODING_STYLE.md` binding rule) — this plan only covers what changes
to make this codebase build and pass its test suite on macOS, not
what the codebase does. Everything under "Confirmed blockers" below
was hit or reproduced directly on the target machine, not inferred
from general macOS/Linux portability knowledge — see each item's
evidence.

---

## 0. Ground rules

1. **No rounding up.** A task here is done when you have a pasted,
   reproducible command's output proving it (a green `make test`,
   `make asan`, `make tsan`, etc. on Darwin) — not because the diff
   "looks portable." This repo's own history
   (`AGENTS.md`'s "Ground truth" section, `docs/CONTINUATION_PROMPT.md`)
   is a specific warning about exactly this failure mode; it applies
   here too.
2. **Don't touch the Linux build path.** This repo still needs to
   build on Linux (see `AGENTS.md`'s WSL2-specific `setarch` notes in
   the Makefile). Every Makefile change in this plan must branch on
   `uname -s` / `$(UNAME_S)` so the Linux path is byte-for-byte
   unchanged, not "fixed" in a way that happens to still work there.
3. **One logical change per commit**, same convention as
   `plan_phase5.md` §0.2 — e.g. the Makefile platform split is one
   commit, the `pthread_condattr_setclock` fix is another, the
   `test_kes_multiprocess.c` semaphore rework is another.
4. **`CODING_STYLE.md` is binding** for any new/edited line in `src/`
   or `include/kes/`, per `AGENTS.md`'s style section — including any
   `#ifdef __APPLE__` blocks added to existing functions. Do not use
   the platform-split work as cover for a broader style sweep of
   surrounding code.
5. **Verify empirically before recommending as settled** — several
   items below (TSan on Apple Silicon in particular) are genuinely
   unknown until run on this machine; don't assume based on this
   plan's guesses once real output exists.

---

## 1. Environment (confirmed)

Captured directly on the target machine, not assumed:

- Hardware: MacBook Air, Apple Silicon, `arm64-apple-darwin25.6.0`
  (`uname -a`: `Darwin ... 25.6.0 ... RELEASE_ARM64_T8142`).
- Toolchain: `gcc`/`cc`/`clang` all resolve to Apple clang 21.0.0
  (Xcode's `XcodeDefault.xctoolchain`) — there is no separate real GCC
  installed.
- `/usr/bin/make` parses this repo's Makefile without syntax errors
  (GNU-Make-specific constructs like `ifdef`, `$(wildcard ...)`,
  automatic variables all work) — confirmed by running `make all` and
  getting through dependency resolution and three source-file
  compiles before hitting the first real C-level error (see §3.1).
- `valgrind` is not installed and has no Apple-Silicon build path at
  all (Valgrind has never shipped arm64-macOS support) — confirmed
  absent via `which valgrind`.
- `gmake` does not exist as a separate binary; `make` is what's used
  everywhere already, no change needed there.
- `../kanek_foundations` (KFL sibling repo, provides `trace.h`) is
  already checked out next to this repo and its `trace.h` includes
  only `<stdio.h> <stdlib.h> <string.h> <errno.h> <stdint.h>` — no
  Linux-specific dependency found there.

---

## 2. Scope

**In scope:** whatever is required for `make all`, `make test`,
`make asan`, and `make tsan` to run and pass on this machine, plus a
documented, deliberate decision on what `make valgrind`/`make
check-all` mean on a platform with no Valgrind.

**Explicitly out of scope for this plan** (tracked elsewhere, do not
pull in):
- Any feature work — allocation strategies beyond first-fit, LFU/Clock
  eviction, etc. `PENDING_ITEMS.md` already tracks these as unrelated
  to porting.
- Reformatting existing code to `CODING_STYLE.md` beyond the specific
  lines this port touches — see Ground rule 4.
- Windows or any other new platform — Darwin/arm64 only, this pass.

---

## 3. Confirmed blockers

Each of these was reproduced directly, with the exact command and
output, not inferred from generic "Linux vs macOS" knowledge.

### 3.1 `pthread_condattr_setclock` does not exist on Darwin

**Evidence:** `make all` fails at compiling `src/kes_cache.c` with:

```
src/kes_cache.c:736:5: error: call to undeclared function
'pthread_condattr_setclock'; ISO C99 and later do not support
implicit function declarations [-Wimplicit-function-declaration]
```

**Where:** `src/kes_cache.c:724-738` sets up `cache->bg_cond` bound to
`CLOCK_MONOTONIC` deliberately (comment explains: avoid wall-clock-step
vulnerability in the background thread's timed wait). The matching
deadline computation is `src/kes_cache.c:1495`
(`clock_gettime(CLOCK_MONOTONIC, &deadline)` inside
`cache_bg_thread_func()`).

**Why it's a real gap, not just a missing declaration:** macOS's
`pthread_cond_timedwait()` has no clock-selection mechanism at all —
there is no Darwin equivalent of `pthread_condattr_setclock`. The
wait is always relative to wall-clock time.

**Fix:** guard `src/kes_cache.c:734-738` with `#ifdef __APPLE__` /
`#else` — on Darwin, skip the `condattr`/`setclock` calls entirely
(`pthread_cond_init(&cache->bg_cond, NULL)` only) and use
`clock_gettime(CLOCK_REALTIME, &deadline)` instead of
`CLOCK_MONOTONIC` at `kes_cache.c:1495` on that platform. Comment
should explain *why* (no clock-selection API on Darwin) and note the
tradeoff this reintroduces (wall-clock-step risk the original
Linux-side comment was specifically written to avoid) so a future
reader doesn't "fix" it back to the broken call.

### 3.2 Shared-library link flags are Linux-only

**Evidence:** direct reproduction —

```
$ cc -shared -Wl,-soname,libfoo.so.1 -o /tmp/libfoo.so.1 /tmp/f.o
ld: unknown options: -soname
```

**Where:** `Makefile`'s `$(SHARED_LIB)` rule
(`$(CC) -shared -Wl,-soname,$(notdir $(SHARED_LIB)) ...`).

**Fix:** Darwin needs `-dynamiclib -install_name
@rpath/libkes.dylib` (or similar) instead of `-shared
-Wl,-soname,...`, and the artifact name itself needs to be
`libkes.1.0.0.dylib` / `libkes.dylib`, not `libkes.so.1.0.0` /
`libkes.so` — Apple's dyld doesn't use the `.so`/soname-symlink
convention at all. This has to branch on `uname -s` per Ground rule 2.

### 3.3 `tests/test_kes_multiprocess.c` uses unnamed process-shared semaphores

**Where:** `sync_state_t` holds `sem_t parent_turn; sem_t child_turn;`
inside a struct placed in an anonymous `MAP_SHARED` mapping
(`tests/test_kes_multiprocess.c:64-65, 208-213`), initialized via
`sem_init(&sync_mem->parent_turn, 1, 1)` (the `1` = process-shared).

**Why it's a real gap:** macOS never implemented process-shared
(`pshared=1`) unnamed POSIX semaphores — `sem_init` with a nonzero
`pshared` argument is not supported on Darwin (it's a long-standing,
well-documented platform gap, not a missing header/flag). This will
fail at runtime on macOS, not at compile time, which makes it easy to
miss if only `make all`/`make test-core` are checked.

**Fix:** rework the two-process handshake in
`test_kes_multiprocess.c` to use named semaphores (`sem_open()` with a
unique name, `sem_unlink()` in cleanup) instead of `sem_t` fields
inside the shared mapping. The `mmap`/`MAP_SHARED` region itself
(`tests/test_kes_multiprocess.c:208-209, 392-393`) is fine as-is for
the data payload — only the semaphore storage/initialization needs to
change. Do not touch the semaphore-based race test's actual
turn-taking logic beyond what's needed to swap the primitive.

### 3.4 `ldconfig` doesn't exist on macOS

**Where:** `Makefile`'s `install`/`uninstall` targets both call
`sudo ldconfig` at the end.

**Fix:** guard these calls out on Darwin (branch per Ground rule 2).
There is no equivalent needed — dyld resolves shared libraries via
`install_name`/`@rpath` at link time and load time, not a system-wide
cache the way glibc's dynamic linker uses one.

### 3.5 `setarch $(uname -m) -R` is Linux-only

**Where:** used in the `tsan`, `soak`, and `stress SANITIZER=tsan`
targets — per the Makefile's own comments, this is purely a WSL2
workaround ("TSan binaries must run under `setarch ... -R` or they
crash with an unrelated 'unexpected memory mapping' error").

**Fix:** make this a no-op on Darwin (branch per Ground rule 2) —
run the TSan test binaries directly there.

### 3.6 No Valgrind on Apple Silicon

**Evidence:** `which valgrind` finds nothing; Valgrind has never
shipped arm64-macOS support and there is no Homebrew formula providing
it for this architecture.

**Where:** the `valgrind` target, and `check-all`'s `[4/4]` stage.

**Fix:** this needs a decision, not just a code change — see §5
Phase 4. Options: (a) skip the Valgrind stage entirely on Darwin and
document `check-all` there as normal+ASan+TSan only, relying on
ASan's LeakSanitizer for the leak-checking role Valgrind plays on
Linux; (b) substitute Xcode's `leaks` tool / `MallocStackLogging=1` +
`MallocScribble=1` env vars around the plain test binaries. Whichever
is chosen must be spelled out in the Makefile's `help`/`check-all`
output and in `AGENTS.md`'s Ground Truth section once implemented, so
it doesn't silently look like parity with Linux when it isn't.

### 3.7 `tar --transform` in `make package` is GNU-tar-only

**Where:** the `package` target's `tar -czf ... --transform 's,^,...'`.
macOS's bundled `tar` is bsdtar, which has no `--transform`.

**Fix:** lowest priority of this list (only affects `make package`,
not build/test). Either use bsdtar's `-s` flag (similar
sed-pattern substitution, different syntax) behind the Darwin branch,
or depend on Homebrew's `gtar` there. Decide when this target is
actually needed — not blocking for Phases 0-4 below.

---

## 4. Verified as *not* a problem (checked directly, no change needed)

- `-lpthread` link: `cc /tmp/t.c -lpthread -o /tmp/t` succeeds on this
  toolchain — no Makefile change needed for `LIBS = -lpthread`.
- `-std=c99 -Wall -Wextra -Werror -fPIC`: all accepted by Apple clang
  as used already (the Phase-1 compile got past flag parsing and
  through `kes_storage.c`/`kes_bitmap.c` cleanly).
- No `/proc`, `setrlimit`/`RLIMIT_*`, `malloc_trim`/`mallopt`,
  `O_DIRECT`, `statfs`/`statvfs`, or raw `syscall()` usage anywhere in
  `src/`, `include/kes/`, or `tests/` — checked by grep across all
  three, nothing found.
- `#define _GNU_SOURCE` at `src/kes_cache.c:1` (for `aligned_alloc`,
  `clock_gettime`) is a harmless no-op on Darwin's libc — both
  functions are standard there without it. Not a blocker, but worth
  stripping in the same commit as the §3.1 fix since it's directly
  adjacent and misleadingly implies a GNU-only dependency that isn't
  real.
- `kanek_foundations`'s `trace.h` — plain standard-C includes only,
  no portability issue (see §1).

## 5. Needs verification once §3.1-3.2 are fixed (not yet run)

- **TSan on Apple Silicon arm64**: historically had gaps on this
  platform; may simply work with this recent an Xcode/clang, may not.
  Run `make tsan` (after the Darwin Makefile branch lands) and record
  the actual result — do not assume either way going in.
- **ASan on Apple Silicon arm64**: same treatment — run `make asan`
  once it can build, record the actual result.
- **`aligned_alloc(64, 0)` behavior on Darwin's libc**: `AGENTS.md`
  documents glibc returning non-NULL for a zero-size
  `aligned_alloc()` call (Track A.1.3, now fixed by rejecting
  `block_count == 0` before the allocation). Confirm this fix already
  covers Darwin too (it should, since the guard is before the
  allocation call regardless of platform) rather than assuming
  glibc-specific behavior was the only path that mattered.

---

## 6. Phased task list

**Phase 0 — Makefile platform split.** Add a `UNAME_S :=
$(shell uname -s)` branch. Darwin gets: `.dylib` shared-lib naming
and `-dynamiclib -install_name` link flags (§3.2), no `ldconfig` in
install/uninstall (§3.4), no `setarch` wrapper in `tsan`/`soak`/stress
(§3.5). Linux path unchanged (Ground rule 2). Acceptance: `make info`
prints sane values on both platforms; no behavior change on Linux
(spot-check by eye, this repo's CI/other-platform testing is out of
this machine's reach).

**Phase 1 — Make it compile.** Fix `pthread_condattr_setclock` per
§3.1, strip the stale `_GNU_SOURCE` per §4. Acceptance:
`make all` and `make test-core` both green on this machine, pasted
output.

**Phase 2 — Make the full suite pass.** Port
`test_kes_multiprocess.c`'s semaphores per §3.3. Acceptance: `make
test` reaches 93/93 across all 12 binaries on Darwin, pasted output.

**Phase 3 — Sanitizer parity.** Run `make asan` and `make tsan` as-is
after Phases 0-2 land; fix whatever each run actually reports (don't
pre-guess). Acceptance: both green, or a specific documented Darwin
gap with evidence if not.

**Phase 4 — Valgrind decision.** Resolve §3.6 with the repo owner's
input (not a unilateral technical call) and implement whichever
option is chosen, updating `check-all`'s Darwin behavior and its
`help` text to match. Acceptance: `make check-all` on Darwin does
something well-defined and documented, even if that "something" is
explicitly "no Valgrind stage here, see AGENTS.md."

**Phase 5 — Docs/tracker sync.** Once the above is real and
re-verified (not before — see Ground rule 1), update
`PENDING_ITEMS.md` and `AGENTS.md`'s "Ground truth" section to record
Darwin as a supported/verified build target, same convention this
repo already uses for Linux state. `make package`'s `tar --transform`
gap (§3.7) can be picked up here or deferred further, it's not
build/test-blocking.

---

## 7. Bookkeeping

Mark a phase's tasks done in this file only by replacing this
section's relevant bullet with a "(DONE — see commit `<hash>`,
verified `<command>` output <date>)" note, same convention
`PENDING_ITEMS.md` uses elsewhere in this repo. Do not mark anything
done based on code review alone — every acceptance criterion above
names a command whose actual output is the proof.

**All phases DONE, 2026-09-11, on this plan's own target machine
(MacBook Air, Apple Silicon, `arm64-apple-darwin25.6.0`).** Per-phase
record:

- **Phase 0 (Makefile platform split, §3.2/3.4/3.5)**: DONE — commit
  `b00b159`. Verified: `make info` prints `Platform: Darwin` and sane
  values; `make all` produces `build/libkes.1.0.0.dylib` via
  `-dynamiclib -install_name` (Apple's `ld` confirmed to reject
  `-shared -Wl,-soname,...` outright first). Linux path not touched
  (no Linux machine available to re-verify directly here — every
  change is `ifeq ($(UNAME_S),Darwin)`-gated, non-Darwin branches are
  byte-for-byte what they were before).
- **Phase 1 (compile, §3.1 + stale `_GNU_SOURCE`)**: DONE — commit
  `6905361`. Also required an unplanned prerequisite fix, commit
  `b28dc34` (`USER_SPACE` never defined for this repo's own `CFLAGS`,
  so `src/kes_bitmap.c`'s `crc32c.h` include failed outright on
  Darwin with "`linux/types.h` file not found" before Phase 1's own
  blocker was even reached — not anticipated by §3/§4 above, found by
  the first real `make all` attempt). Verified: `make all` and `make
  test-core` (9/9) both green, pasted output captured during this
  session.
- **Phase 2 (full suite, §3.3)**: DONE — commit `a01e542`
  (`test_kes_multiprocess.c` named-semaphore port). Two further
  unplanned fixes surfaced only once every test binary actually built
  and ran, neither anticipated by this plan: `pthread_barrier_t`
  doesn't exist on Darwin at all (`tests/test_kes_cache.c`, commit
  `8d0aed9`, a mutex/condvar shim), and the 200GiB `aligned_alloc()`
  NOMEM case in `tests/test_kes_fault_injection.c` doesn't reproduce
  on Darwin (measured up to the ~256TiB representable maximum — none
  of it fails; commit `5ac31b5`, skipped with a documented reason
  rather than forced). Verified: `make test` reaches **95/95** across
  all 12 binaries (grown past the 93/93 this plan and `AGENTS.md`
  cited at drafting time — re-verify the actual count going forward,
  don't trust either number).
- **Phase 3 (sanitizer parity)**: DONE, both green with no fixes
  needed on the sanitizer/platform side — genuinely unknown going in,
  per Ground rule 5, and turned out clean: `make asan` and `make tsan`
  both ran to completion with zero ASan/UBSan/TSan reports of any kind
  on Apple Silicon with this Xcode/clang version, no workaround
  needed for either. One non-platform-specific bug was found along
  the way by `make asan` reaching a state apparently not exercised by
  prior sessions' runs — `kes_storage_get_stats()`'s
  `allocated_extents - 1` underflow, a real UBSan-confirmed
  undefined-behavior bug, unrelated to Darwin and reproducible on any
  platform; fixed in commit `82aedd4`. Re-ran both `make asan` and
  `make tsan` clean after that fix.
- **Phase 4 (Valgrind decision, §3.6)**: DONE — commit `d08578f`.
  Decision (repo owner, not a unilateral call): skip the Valgrind
  stage entirely on Darwin; `make check-all` there means
  normal+ASan+TSan only, relying on ASan's LeakSanitizer for the
  leak-checking role Valgrind plays on Linux. Verified: `make
  check-all` exits 0 with "ALL CHECKS PASSED" and the documented skip
  message in its `[4/4]` slot.
- **Phase 5 (docs/tracker sync)**: DONE. `AGENTS.md`'s "Ground truth"
  and "Building and Testing" sections updated; `PENDING_ITEMS.md` got
  a matching "macOS (Darwin/arm64) port" entry under `## Resolved`
  citing every commit above. §3.7 (`make package`'s `tar --transform`
  gap) was picked up here rather than deferred further — commit
  `821c3f6` (bsdtar's `-s` flag), verified `make package` produces a
  correctly-prefixed `libkes-1.0.0.tar.gz` on this machine.
