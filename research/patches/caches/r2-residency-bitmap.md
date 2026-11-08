---
id: R2-residency-bitmap
title: Buffer cache residency check answered from a bitmap of resident blocks
area: caches
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 1f384e5582
files: gpu/shim/gow3_resident_blocks.h, gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp, gpu/shadps4/video_core/buffer_cache/buffer_cache.h, gpu/shim/gow3_toggles.h, tests/test_resident_blocks.cpp
symbols: Gow3Resident::Blocks, resident_bitmap, BufferCache::EnsureResident, resident_ranges, ResidencyBitmap, residency_checks
switches: toggle2:ResidencyBitmap
docs: impl-stage-b-per-draw-2.md#r2-residency-bitmap-1f384e5-second-word-65536-default-on, plan-linux-deck.md#per-draw-cuts-r1--r2--t1--t2-on-the-deck-2026-11-05-appimage-c71e87c-drain-hybrid-on-gow3_texset_wide1-gow3_region_index1-frame-stats-without-sections-log-datadeck-perdraw-1log
depends_on: driver-memory-off-stage-b
related: T1, T2, R1-region-index, S2, R2
checked: 2026-11-07
---
## What it does

Every `ObtainBuffer` outside the stream buffer asks `EnsureResident` whether its blocks are resident. The answer
used to come from a binary search of `resident_ranges` (a sorted vector of a few thousand backings) plus a walk
for gaps. `Gow3Resident::Blocks` keeps a bitmap of the resident blocks: chunks of 2^16 blocks (8 KiB of bits),
created on the first set, 1,024 chunk pointers. `EnsureResident` returns at once when `resident_bitmap.All(first,
last + 1)`. Any other answer runs the old search and binds as before.

## Invariants

- Residency only grows: nothing removes a range from `resident_ranges`, and `ResidencyPool` has no free. This is
  why "all bits set" means exactly "ForEachGap finds no gap". Releasing residency anywhere would make the bitmap
  answer wrongly that blocks are resident, and unbound sparse pages would be read: garbage or a device fault.
- Every backing added to `resident_ranges` must also be `Set` in the bitmap in the same place, whatever the
  toggle (in `EnsureResident`'s piece loop). A new path that adds residency must do both.
- `All` answers false for an empty range and for blocks past 2^26 (`MaxBlockBits`). False only costs the search,
  never correctness.
- Same thread rule as `resident_ranges` (the thread that owns the caches). There is no lock, and `Set` allocates a
  chunk on its first use.
- Toggle word 2 bit 65536 (live) brings back the search every time. The bitmap is still kept up to date.

## Interactions

- Builds on driver-memory-off-stage-b: one backing per pool piece, each piece set in the bitmap.
- S2 sends more small buffers down the arena path, so more of these checks.
- Part of round 2 of the per-draw cuts with T1, T2 and R1-region-index (`0 983040` turns all four off).
- Not R2 the rwlock handle hash set (runtime). The letter was reused.

## Evidence

- Cuts redo (2026-11-07, draw preparation alive): R1/R2/T1/T2 off (983040) +3.8 points of a core at the 60 cap (stage B 50 -> 53%), +0.17 W CPU; uncapped 82.8 -> 81.7 FPS, presents > 15 ms 54 -> 89 a minute, worst 20.4 ms. Launch 15's +14.6 was inflated: with no prepared draws every draw took T2's path.
- Study: `obt_resident` 0.32 ms of a heavy Deck frame at ~0.15 us a call, ~2,100 calls. Expected about -0.2 to
  -0.25 ms.
- deck-perdraw-1 (2026-11-05): all four cuts 78.8 vs 78.2 FPS, stage B busy 86.7 vs 92.6% (~0.75 ms a frame freed),
  2,150 bitmap answers a frame. Single bits off: 78.7-79.0 FPS (noise). Freeing stage B time did not lift FPS
  because stage A and the power-capped GPU then gated it. No user-visible change reported. Not measured on the PC.
- `resident-blocks-test`: 1.2M checks against `IntervalList` over random gap-style adds, plus word / chunk /
  top-of-bitmap edges.

## History and pitfalls

- An exact cut with no behaviour change, so its default is on. Its value shows only while stage B is the
  limit.
- `GOW3_FRAME_STATS` line "Stage B cuts per frame: residency bitmap (R2 on|off) N, searched N". A high "searched"
  count means new ranges or blocks past the bitmap.
