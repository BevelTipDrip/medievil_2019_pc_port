---
id: S1-delta-draw
title: S1 delta-draw fast path: a draw equal to the previous reuses descriptors, render state, dynamic state, vertex input (not built)
area: stage-b
status: rejected
platforms: all
default_windows: n/a
default_deck: n/a
origin: ours
class: G
commits:
files: gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp
symbols: BindResources, PrepareRenderState, UpdateDynamicState, MakeBeginSignature, EmitVertexBuffers
switches:
tests: N-28
docs: study-stage-b-native.md#inside-stage-b-no-game-hooks, study-frame-cost-2026-10-29.md#3-sampler-handle-cache-in-the-textureset-memo-entry-qw-then-s1-delta-draw--goal-3, study-stage-b-per-draw.md#4-ranked-cuts, study-deck-rescore-rejected.md#c-analysed-gated-or-ranked-never-built
depends_on: lineage-texture-set-memo, sampler-memo
related: record-skip-identical, S3-split-descriptors, bulk-descriptor-copy, own-target-texsets, stage-b-counters
checked: 2026-11-07
---
## What it does

Not built. If a draw's pipeline, prepared image hashes of every stage, `BeginSignature` (targets, generation) and the
dirty register blocks outside user data equal the previous draw's, reuse its `image_infos`, sampler writes,
`RenderState`, vertex attribute / binding descriptions and dynamic state; rebuild only the buffer bindings and push,
and skip recording `bindPipeline` / `pushConstants` / `setVertexInputEXT` whose bytes did not change. The code it
would change: `BindResources`, `PrepareRenderState`, `UpdateDynamicState`, `EmitVertexBuffers` in `vk_rasterizer.cpp`.

## Invariants

- The predicate must cover everything the skipped work reads: registry and scene generation, image `needs_rebind` /
  `is_target`, layouts, the clean-register-block check `UpdateDynamicState` would need. A missed input binds the
  previous draw's textures or state (wrong image, no error).
- What must stay per draw (study-frame-cost item 3): the texset emit loop, `Transit`, and the upscaler / camera-motion
  hooks in `PrepareRenderState`. `BeginRendering` is already memoized.
- Pipeline bind elision gains nothing on stage B (bind and draw share one Record; per-draw study C9): keep it out.

## Interactions

- Builds on lineage-texture-set-memo and sampler-memo (shipped since): their hits already took part of the per-draw
  texture cost the 2026-10-20 estimate counted.
- Overlaps record-skip-identical (the record skips), S3-split-descriptors (descriptor reuse) and the per-draw study's
  C4 + C7 + C8; the rescore maps S1 = C4 + C7 + C8 together. bulk-descriptor-copy already cut the descriptor copy.
- own-target-texsets: sets with the draw's own target are not memoized, so those draws would fail the predicate.

## Evidence

- Native study (PC, 2026-10-20): ~0.25-0.3 us per packet on ~75% of draws, ranked 1 (+11-14% heavy view, GPU cap).
- Frame-cost study (2026-10-29), corrected: 0.25-0.35 ms a frame with stats off. Gate: the Step-0 joint predicate
  >= 55-60% and >= 0.25 ms a frame. "If the sampler cache alone (~0.1 ms) doesn't move the lows, S1's ~0.3 ms won't
  move them much either."
- N-28 built the other Step-0 counters; "Not built: the joint S1 repeat predicate".
- Rescore C1 (Deck): **-0.6 to -1.2 ms** a frame (draw section x4.2 at 1,414 draws; 77% of pipeline binds equal the
  previous on the Deck vs 71% on the PC). "**Likely wrong for the Deck**: the gate was about the PC's lows; on the Deck
  stage B is the mean-FPS wall". Rank 8 on the Deck shortlist. Nothing measured; not verified in game.

## History and pitfalls

- Never built: gated on a counter nobody built, then deprioritised on the PC because it would not move the lows.
- To retry: build the joint predicate counter (stats only), read it in one Deck run, then build behind a toggle bit
  and A/B live. Medium risk is all in invalidation; reuse the memo's invalidation, do not invent a second one.
