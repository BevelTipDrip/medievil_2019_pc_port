---
id: pool-kept-pages
title: Kept pool pages on Linux: a recommit gets its own bytes back (Hilltop Mausoleum lighting fix)
area: memory
status: active
platforms: linux
default_windows: n/a
default_deck: on
origin: ours
class: G
commits: b42c0a8, 7246f88, f74e1a1
files: src/runtime_memory.c, tests/test_runtime.c
symbols: pool_keep_pages, pool_keep_locked, pool_take_kept_locked, pool_zero_foreign_locked, kept_table, pool_owner, pool_pages
switches: GOW3_POOL_KEEP_PAGES, GOW3_POOL_ZERO_DECOMMIT
tests: Launch 15
docs: plan-linux-deck.md#hilltop-mausoleum-lighting-pool-pages-leaked-on-linux-2026-11-0607-fixed-7246f88--f74e1a1
depends_on:
related: M1
verified: 2026-11-07
---
## What it does

A pool decommit marks its pages `POOL_KEPT` for the guest address (`kept_table`, `pool_owner` per chunk)
instead of `POOL_FREE`. A commit at that address takes the same physical pages back, so the memfd still
holds the bytes the guest left there, as on the PS4 and as Windows' kept views (M1) do. A commit that has to
take pages kept for another address zeroes them first (`pool_zero_foreign_locked`, hole punch). A failed commit
zeroes the pages it gives back (`f74e1a1`).

## Invariants

- No commit may receive pages holding another allocation's bytes: kept pages go to their own address, or are
  zeroed (`kept_foreign_zeroed` counts it). Breaking this brings back the Mausoleum bug: the texture cache
  uploads leaked bytes into the sky capture, reflection arrays and luminance mips (wrong, per-launch colours).
- A recommit at the same address sees its old bytes (UE4's pool reuse relies on it, like the PS4).
- Every path that frees pool pages on Linux goes through `pool_keep_locked` or `host_discard`: decommit
  (`pool_decommit_unlocked` and the locked path), a failed commit. A new free path must do the same.
- The tables exist only when `pool_expand` allocated them; without `kept_table` a decommit frees as before. If only
  one chunk's `pool_owner` calloc failed, `pool_keep_locked` skips that chunk's pages and they stay `POOL_TAKEN`
  (never reused: a leak, not wrong bytes) [code].

## Interactions

- M1 is the Windows mechanism (kept views); this card is its Linux equivalent without views. Windows still
  hands kept pages to another address un-zeroed once free pages run out (second loop of `pool_take_locked`,
  medium confidence, fix pending for `lab`).
- The texture cache's undefined-image paths (C3, new images undefined, park unmapped) skip uploads only for
  render-target first uses; every other first use uploads guest memory, so they depend on this invariant.
- `GOW3_PS4_MEMORY` (Deck preset, stock 5056 MiB) means more reuse, so the leak showed sooner there.

## Evidence

- Deck, 2026-11-07 (build `bd1a00d-mausoleumfix`): the user, "It looks correct, everything is loading in"
  (plan-linux-deck.md records it as "everything loaded in correctly"); 60 FPS. About 64,000 kept pages taken back per
  5 s, 0 commits without, 0 foreign pages zeroed, minor faults ~7-35k per 5 s (broken baseline ~6-33k). No in-game
  run with `GOW3_POOL_KEEP_PAGES=0` after the fix is recorded; the leak with it off is shown by the unit test.
- `test_runtime --pool-pages`: kept on, own bytes back, no leak; kept off, leaked.

## History and pitfalls

- The leak existed since the import (`ac900f8`): no patch removed pool zeroing, it never existed. D4 (pool
  locking) is unrelated.
- About 15 Deck launches went to toggle bisects (A2, fog, scale, sky light) before tooling showed the images
  held never-written guest memory. `GOW3_POOL_ZERO_DECOMMIT=1` (`b42c0a8`) was the deterministic test: it
  zeroes on decommit and makes the bug appear every launch.
- `GOW3_POOL_KEEP_PAGES=0` restores the leak. Under memory pressure a recommit can get zeros instead of its own
  bytes (safe, not identical to the PS4); watch `kept_misses` in the "Guest pool faults" line.
