---
id: lineage-dropped-stage-b
title: Lineage stage B ideas built and dropped: batched small copies, no barrier tracking for read-only ranges, futex Commit, shader specialization memo
area: stage-b
status: rejected
platforms: all
default_windows: n/a
default_deck: n/a
origin: bloodborne 21b7214
class: G
commits: 21b7214, 413cd5c
files: gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h, gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp
symbols: Commit, SmallGuestCopy
switches:
tests:
docs: study-deck-rescore-rejected.md#b-built-measured-and-reverted-or-dropped-no-switch-left, docs/parallel_gpu.md, fix-inventory.md#10-tried-and-dropped
depends_on:
related: pool-small-copies, lineage-draw-pipe, stage-b-spin, L4, R1-stale-eop, L1-region-buckets, O2-O3, lineage-constant-ring, ring-reuse
checked: 2026-11-07
---
## What it does

Groups the stage B ideas the Bloodborne lineage built, measured on its own game (16 threads) and removed, so no code
or switch is left (rescore section B). Not MediEvil measurements.
- **B1** small host copies: a lock-free MPMC queue run by the idle recording thread (the rest by the GPU thread at the
  wait), and separately one recorded command per 32 small copies.
- **B2** no barrier tracking for read-only stream buffer ranges.
- **B3** a release store plus a timed futex sleep in `DrawPipe::Commit` instead of the sequentially consistent store.
- **B4** shader specialization memo (`21b7214`, reverted `413cd5c`): reuse the last permutation when the
  address-free resource inputs match.

## Invariants

- Nothing of these is in the code. `DrawPipe::Commit` keeps `published.store(head, seq_cst)` followed by the
  `sleeping` load; stage B's wait stores `sleeping` and re-reads `published` (both seq_cst) before `wake.wait`. With a
  weaker store one side can miss the other and stage B sleeps on a published packet (the reason B3 went).
- Small copies still run one closure each on the recorder (`SmallGuestCopy` -> `RecordHostCopy`); B1's batching is
  not there.

## Interactions

- pool-small-copies is the same lever as B1 and still exists as a live toggle bit: the rescore says measure it first,
  and rebuild B1 behind a toggle only if it gains.
- lineage-draw-pipe, stage-b-spin: stage B's packet wait (and #4's spin-then-sleep) depends on `Commit`'s store
  order (B3). The hybrid drain (drain-hybrid) pairs `Consumed` with `drain_target` instead; `commit_ends` is stage A's
  own data, so B3 does not touch it.
- Our own reverts, listed in rescore B5 and fix-inventory section 10: L4 (re-kick, replaced by L5), R1-stale-eop
  (black torch boxes), L1-region-buckets (re-applied as L2), per-epoch constant chunks (replaced by the constant ring
  and ring-reuse), O2-O3 (replaced by the HZB remap). Each has its own card.

## Evidence

- B1: "blocked 9.5% -> 4.8%, but 86.5 vs 87.2 FPS: the copying moved onto the GPU thread"; batched "86.4 vs 86.2 FPS"
  (docs/parallel_gpu.md, "Other attempts").
- B2: "85.9 vs 87.4 FPS, both phase orders (cause not found)".
- B3: "+0.4%, not worth a possible missed wake-up; reverted".
- B4: "re-read every sharp and cost more than the StageSpecialization it replaced (A/B: 54.7 FPS with it, 57.1
  without)" (`413cd5c`).
- Rescore Deck estimates: B1 0-0.3 ms, B2-B4 ~0 for stage B. None run on MediEvil. Not verified in game.

## History and pitfalls

- `21b7214` / `413cd5c` are in upstream's history (before this branch range), so the commit lookup lands here.
- B1 is the only one the rescore keeps open (fewer recorder commands shorten its catch-ups on the Deck). B2's loss
  was never explained: do not retry it without finding the cause. B3 and B4 hold.
