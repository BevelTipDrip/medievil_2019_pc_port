---
id: own-target-texsets
title: Texture sets that contain the draw's own target made memoizable (study item 4, not built)
area: stage-b
status: rejected
platforms: all
default_windows: n/a
default_deck: n/a
origin: ours
class: C
commits:
files: gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp
symbols: BindTextures, BindTexturesFromSetIn, KeepTextureSet, DropTextureSet, texture_set_unkept
switches:
tests: N-28
docs: study-frame-cost-2026-10-29.md#4-make-sets-that-contain-the-current-target-memoizable--goal-3, plan-frame-pacing.md#n-28-stage-b-counters-the-studys-step-0-2026-10-30-launched-by-claude-with-the-users-go-t-sb0-counterslog-build-ae09fcc-gow3_sections1, study-stage-b-per-draw.md#4-ranked-cuts, study-deck-rescore-rejected.md#c-analysed-gated-or-ranked-never-built
depends_on: lineage-texture-set-memo
related: texset-region-stamps, T2, lean-barriers, stage-b-counters, S1-delta-draw
verified: 2026-11-07
---
## What it does

Not built. In `BindTextures`' slow path a set is not kept when an image is a storage image, `force_general` or the
draw's own target (`binding.is_target`): `set_ok` turns false, the reason goes to `texture_set_unkept` (2 own colour,
3 own depth), and `DropTextureSet` clears the slot, so the same set misses again every frame. The idea: keep such sets
with a per-entry target / feedback flag and redo the depth-in-place or feedback-loop transition on a hit.

## Invariants

- A memo hit must still do what the slow path does for a target image: for colour the feedback-loop / general layout
  and its barrier (the `force_general || is_target` branch, colour only); for depth the read-only layout or
  `SampleDepthInPlace`. A hit that skips it samples a target in the wrong layout.
- The memo hit check refuses an entry whose image is a target now (`image.binding.is_target` in
  `BindTexturesFromSetIn`): it would have to be relaxed together with the keep side, never one without the other.
- Assumption (class C): the count of own-target sets per frame depends on the scene and the settings (translucency,
  soft particles, fog, depth sampled in place); the Deck's Low preset binds few.

## Interactions

- lineage-texture-set-memo, texset-region-stamps, T2: the memo this would extend; region stamps decide validity.
- lean-barriers: samples depth in place, one source of own-depth sets.
- stage-b-counters: `texture_set_unkept` (N-28's "texture sets not kept") is the gate counter.
- S1-delta-draw: draws with own-target sets fail its predicate too.

## Evidence

- N-28 (PC, build `ae09fcc`): own colour target 7,430 and own depth 6,541 against 14,076 "other key" misses per stats
  window; "78 misses a heavy frame at ~2.3 us (texslow 0.18 ms): ~0.1-0.18 ms. Gate passed." Study item 4: no effect
  on the lows (constant floor across fast and slow frames).
- Deck (`deck-decode-D2.log`): own colour 187 + own depth 1,496 per window of 262,144 render-state memo calls, ~9 a
  frame vs ~82 on the PC: ~0.13 ms. Per-draw study C10: "park for the Deck", < 0.05 us/draw. Dropped from the Deck
  shortlist. Not built, not verified in game.

## History and pitfalls

- Corrected T-S1c: the steady ~104 "other key" misses a frame were not rotating images but these drops. A hash change
  or a per-image cache would gain ~0 (`ImageDescHash` is stable).
- The per-draw study first read ~5 a frame by dividing by a 5-s window; the line prints every 262,144 calls (~2.3 s),
  so it is ~9-11 (rescore). The big Deck slow-bind class is "not memoizable" (unprepared draws, dispatches, > 16
  images), which T2 partly addressed, not this one.
- Retry only on a scene / preset where own-target sets are >= ~50 a frame at >= 1 us each.
