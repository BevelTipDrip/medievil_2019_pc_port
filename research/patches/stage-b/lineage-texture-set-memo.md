---
id: lineage-texture-set-memo
title: Texture-set and render-state memos (inherited), keyed per draw
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream (inherited lineage, docs/parallel_gpu.md)
class: G
files: gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h, gpu/shim/gow3_texset_table.h
symbols: BindTexturesFromSet, BindTexturesFromSetIn, TextureSet, TextureSetEntry, BeginMemo, BeginSignature, MakeBeginSignature, BeginRendering, BeginRenderingFull, RegistryGeneration, Slots
switches: toggle:TextureSetMemo, toggle:RenderStateMemo
tests: T-S1b, T-texset-both-off
docs: fix-inventory.md#2-draw-translation-cost-stage-b-per-draw, study-deck-patch-audit.md#12-stage-b-per-draw-caches-and-memos, docs/parallel_gpu.md
depends_on: lineage-draw-preparation
related: texset-region-stamps, texset-compact, T1, T2, own-target-texsets, S3-split-descriptors, S1-delta-draw, prep-priority, findimage-generation-misses
verified: 2026-11-07
---
## What it does

**Texture-set memo** (toggle word 1 bit 45 `TextureSetMemo`): `Rasterizer::BindTexturesFromSet` keys a stage's
sampled textures by `XXH3` of the prepared T# hashes, seeded with the stage's `Shader::Info` address. The table has
`Gow3TexSet::Slots` entries (32,768 now; 8,192 inherited) and holds sets of up to 16 images. On a hit
(`BindTexturesFromSetIn`) the resolved images are reused: image after the depth redirect, view, backing, range,
scene-proxy flag. Only the per-draw effects are redone (binding flags, transitions, descriptor writes).
**Render-state memo** (bit 44 `RenderStateMemo`): `BeginRendering` reuses the previous `RenderState` when the
scheduler still has that exact pass open (`IsRenderingWith`), the `BeginSignature` matches (targets, views,
attachment key, scaling and upscaler state, registry generation, depth control), no clear is requested and no
target is bound for sampling. States with clears are not stored.

## Invariants

- A texture-set hit is valid only if nothing it resolved has changed. The inherited rule was the same image
  registry generation (`RegistryGeneration`). texset-region-stamps relaxed that to 4 MiB region stamps, checked
  only after a registry change. Each entry must also keep its backing (`image.backing == entry.backing`), and no
  image may need a rebind, be a target (`binding.is_target`) or be out of date (`IsUpToDate`). A stale hit binds
  freed or old image views: wrong textures, or a device loss.
- Not memoized: written or storage images, mip fallbacks, more than 16 images (32 with T2), stages without a
  prepared draw (unless T2's wide memo is on; dispatches never, even with T2), and anything during a frame
  capture. Render-state memo states with attachment feedback loops are not stored either.
- The key includes the `Shader::Info` address, so two programs with the same T#s never share a slot.
- A render-state hit requires the pass still open. Any barrier, copy or dispatch that ended the pass makes it miss.
  A clear or a target also sampled by the draw must take the full path.

## Interactions

- lineage-draw-preparation: the prepared T# hashes are the key, so an unprepared draw falls to the slow path (the
  per-draw study's ~15 us slow bind on the Deck). prep-priority and prep-workers-2 tune the workers.
- texset-region-stamps (validity), texset-compact (layout), T1 (stages without images skip setup) and T2 (17-32
  images, unprepared draws; Deck preset `GOW3_TEXSET_WIDE=1`) all modify this memo. `perf_texture_memo=0` turns
  off only the stamps and two-way slots (`GOW3_TEXSET_REGIONS=0`, `GOW3_TEXSET_TWO_WAY=0`), not the memo.
- own-target-texsets and S3-split-descriptors were never built. They would extend the memo.

## Evidence

- Inherited, base port's game (docs/parallel_gpu.md): render-state memo 90% hits, 122.1 vs 111.8 FPS (+9%);
  texture-set memo 84% hits, 128.6 vs 121.8 (+5.6%). Screenshots on and off were within the scene's noise.
- MediEvil: with region stamps, T-S1b hit rate 83.6 -> 92.3%. T-texset-both-off (plan-stage-b-split.md, build
  `61440d7`): the user, 1% and 0.1% lows worse without stamps and two-way slots, "too much run to run variance at
  this level, but it looks like the memo work is worth it". Both measure texset-region-stamps, not the memo.

## History and pitfalls

- The stats line "texture sets not kept: ..." (`texture_set_unkept`) gives the reasons for a miss. Read it before
  widening the memo.
- Toggling bit 45 or 44 live is safe: a miss takes the full path, which is always correct.
