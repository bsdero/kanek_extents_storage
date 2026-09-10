# KES Pending-Fixes Master Plan

This is the index and application order for every currently open item
in `PENDING_FIXES_SEP2026.md`. Each item has its own detailed,
copy-pasteable implementation plan in a separate file — this file does
not repeat that detail, it sequences it. Read the linked plan file in
full before implementing that item; do not implement from this
summary alone.

`KES-1` (`kes_cache_destroy()` use-after-free) is already fixed —
commit `8d40167`, see `PENDING_ITEMS.md`'s `## Resolved` section. It
is not part of this plan.

`KES-12` (`AGENTS.md` Ground Truth bookkeeping) turned out to already
be resolved — commit `94e5868` already removed the stale language
`PENDING_FIXES_SEP2026.md` still describes as open.
`PENDING_FIXES_SEP2026.md`'s own `## KES-12` entry just needs a
one-line status correction to `CLOSED`; no implementation plan exists
or is needed for it.

## Application order

Apply the plans in this order. The order is driven by two things: a
hard dependency (KES-6 before KES-5), and putting the two highest-
severity, most load-bearing fixes first, since two downstream projects
are about to start building directly on top of the storage layer both
of them touch.

| # | Plan file | Items | Severity | Why this position |
|---|---|---|---|---|
| 1 | [`kes_6_plan.md`](kes_6_plan.md) | KES-6 | High | Hard dependency: KES-5's plan requires `kes_bitmap_checksum()` and the descriptor's `bitmap_checksum` field, both added here. Also the higher-severity half of the pair on its own — silent bitmap corruption. |
| 2 | [`kes_5_plan.md`](kes_5_plan.md) | KES-5 | High | Depends on step 1. Do it immediately after, same session if possible — both touch the same functions in `src/kes_storage.c`, so one combined `make check-all`/stress verification pass at the end is more efficient than two separate ones. |
| 3 | [`kes_3_kes_4_plan.md`](kes_3_kes_4_plan.md) | KES-3, KES-4 | Medium | Different file (`src/kes_cache.c`), no conflict with steps 1–2. The most intricate of the remaining work (the ERROR/DIRTY interaction between the two fixes) — do it while still in a careful-concurrency mindset rather than context-switching back into it later. |
| 4 | [`kes_2_kes_8_plan.md`](kes_2_kes_8_plan.md) | KES-2, KES-8 | Medium / Low | Small, mechanical, no dependencies on anything above. |
| 5 | [`kes_7_kes_9_plan.md`](kes_7_kes_9_plan.md) | KES-7, KES-9 | Low | Docs-only (KES-7) / no-code policy reaffirmation (KES-9). Zero risk, zero dependencies — could actually go anywhere, including first, but a couple of its notes read more naturally once steps 1–2 have landed (KES-7's note explicitly references "if KES-5 has been applied"). |
| 6 | [`kes_10_kes_11_plan.md`](kes_10_kes_11_plan.md) | KES-10, KES-11 | Low | Same profile as step 5 — docs-only / deferred-by-design, zero risk, zero dependencies. |

Steps 5 and 6 have no dependency on steps 1–4 or on each other and can
be done at any point, including interleaved as downtime between the
harder sessions, without affecting correctness of anything else in
this table. Steps 1–4 should be done in the order shown; step 2
specifically must not be started before step 1 is complete and
verified.

## What "done" means for each step

Every linked plan file has its own "Verification checklist" and
"Bookkeeping when done" section — follow those exactly, per plan, not
a single verification pass at the very end of all six. In particular:

- Steps 1, 2, 3, 4 change code and must each pass their own full
  `make test` / `make asan` / `make tsan` / `make valgrind` /
  `make check-all` sequence before being considered done, with pasted
  output as evidence (per `AGENTS.md`'s standing rule — do not assume
  a step is clean without actually re-running it).
- Steps 5 and 6 change no code; "done" for them means the doc comments
  are written and `make test` still passes unchanged (confirming
  nothing was accidentally broken), plus the bookkeeping edits each
  plan specifies.
- After every step, update `PENDING_FIXES_SEP2026.md`'s status line
  for the item(s) that step covers, and add the matching evidence-first
  entry to `PENDING_ITEMS.md`'s `## Resolved` section — each plan file
  says exactly what that entry should contain. Do this per step, not
  in one batch at the end, so the tracker files never drift out of
  sync with actual code state for long (the exact failure mode
  `KES_HARDENING_PLAN.md` §0 and `AGENTS.md` both warn about).

## Also needed, not a plan of its own

Correct `PENDING_FIXES_SEP2026.md`'s `## KES-12` entry to `CLOSED`
(citing commit `94e5868`) at whatever point is convenient — it is not
gated on any of the six steps above.
