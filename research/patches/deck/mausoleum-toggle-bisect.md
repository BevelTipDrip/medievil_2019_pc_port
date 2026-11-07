---
id: mausoleum-toggle-bisect
title: Hilltop Mausoleum lighting: suspects ruled out by toggle bisects (A2, Deck switches, culling, particles, 4K memory, undefined uploads, zerovram)
area: deck
status: rejected
platforms: linux
default_windows: n/a
default_deck: n/a
origin: ours
class: G
commits:
files:
symbols:
switches:
tests:
docs: plan-linux-deck.md#hilltop-mausoleum-lighting-pool-pages-leaked-on-linux-2026-11-0607-fixed-7246f88--f74e1a1
depends_on:
related: pool-kept-pages, M1, A2, deck-preset, finer-occlusion-culling, single-particle-pass, particle-flip-guard, 4k-memory, new-images-undefined, C3, tiny-target-readback, readback-first-wait, gpu-write-subresources, texcache-stats, mausoleum-diagnostics, renderdoc-capture
verified: 2026-11-07
---
## What it does

No code: the record of a method that failed. On the Deck the Hilltop Mausoleum was lit in wrong colours, surface
by surface ("like a normal map"), different every launch. Before any tool looked at the bad data, Deck launches
went to switching suspects off one at a time: A2 (`GOW3_PENDING_FILTER`), every switch of the Deck preset, the
finer culling eboot patch, the single particle pass, 4K memory, undefined-target uploads
(`GOW3_NEW_IMAGES_UNDEFINED=0`), RADV `zerovram`; then cache-side switches built for the hunt (stale first readbacks,
tiny target copy-back, GPU writes inside images). None of them was the cause; the cause was the Linux pool page leak
(pool-kept-pages, `7246f88` + `f74e1a1`).

## Invariants

- The suspects are cleared for this symptom only. `GOW3_POOL_KEEP_PAGES=0` (`pool_keep_pages()`,
  `src/runtime_memory.c`) restores the leak (`runtime-test --pool-pages` shows it; no Deck run with it off is written
  up), so a new lighting bisect must start with kept pages on, or it measures the leak again.
- A bug that depends on which pages a commit gets varies per launch. A wrong launch with a suspect off is conclusive
  (the suspect is not needed); a correct launch proves nothing. `GOW3_TEXCACHE_STATS=1` only adds counters, logs and
  bookkeeping maps (`CacheStatsEnabled`, `texture_cache.cpp`; nothing it gates changes an upload), yet one such run
  "repaired itself".
- Make such a bug deterministic by zeroing or poisoning freed memory: `GOW3_POOL_ZERO_DECOMMIT=1`
  (`pool_zero_decommit`, Linux only: returns 0 under `_WIN32`) also fixed the room (~231k page faults per 5 s,
  ~40x), so one launch confirmed the cause.
- Not covered (C): the bisect was a Deck (Linux) exercise. On Windows M1's kept views hide the leak until free pages
  run out, so a clean Windows run says nothing about these suspects either way.

## Interactions

- pool-kept-pages: the real fix; its history lists the same dead ends from the memory side.
- M1: Windows got kept views by accident (a performance patch), so the user's PC preset never showed the bug; the
  PC with the Deck preset did once (`pool_take_locked`'s second loop hands kept pages out un-zeroed).
- A2, deck-preset, finer-occlusion-culling, single-particle-pass, particle-flip-guard, 4k-memory,
  new-images-undefined, C3: ruled out here; each keeps its own card's default.
- tiny-target-readback, readback-first-wait, gpu-write-subresources: cache-side suspects built during the same hunt,
  now opt-in switches with their own rejected cards.
- texcache-stats, mausoleum-diagnostics, renderdoc-capture: the tools that ended it (image watch, texel statistics,
  RenderDoc on the Deck showed images holding guest memory nothing had written).

## Evidence

- plan-linux-deck.md, Mausoleum section: "Dead ends on the way (each ruled out by a run; kept as opt-in
  diagnostics): A2 / every Deck switch / the finer culling patch / particle single pass / 4K memory (bisect),
  undefined-target uploads (`GOW3_NEW_IMAGES_UNDEFINED=0`), RADV `zerovram`". No per-run numbers or logs are written
  up; about 15 Deck launches went to it (pool-kept-pages card, memory rule "trace data, not toggles").
- Logs, untracked and not written up (`research/docs/data/`): `deck-mausoleum-1.log` (Deck, 2026-11-06, Deck
  switches, before the fix); PC `t-mausoleum-pc-ref.log` (Deck switches, 1280x720 low, RenderDoc), `-clean.log` (4K
  console quality, no switches), `-a2off.log` (as ref but `GOW3_PENDING_FILTER=0`). The PC logs are 16-17 lines of
  start-up header with no frame or result lines.
- Verified in game was the fix, not this method: the user, "everything loaded in correctly".

## History and pitfalls

- Tried: one suspect off per launch, judged in the room. Result: the bug stayed under every switch; colours changed
  per launch regardless, and the one correct `GOW3_TEXCACHE_STATS=1` run was memory reuse luck (not reproducible).
- Dropped because the user stopped the bisects (memory rule). Tooling then named the bad objects (sky light capture
  cubemap with NaN and +-65504 in every mip, the 9x1 sky light SH target, luminance mips, reflection capture arrays),
  and reading the pool commit / decommit code found the cause.
- Worth retrying only for a bug that is the same every launch. For a varying render bug, trace the bad data to its
  source or make it deterministic first; after one or two inconclusive A/Bs, switch to instrumentation.
