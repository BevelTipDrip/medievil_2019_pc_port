---
id: refraction-limit
title: UE4 distortion (refraction) shift shortened to a limit (Sleeping Village water copy)
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: C
commits: eeadcb3
files: gpu/shadps4/shader_recompiler/ir/passes/refraction_limit_pass.cpp, gpu/shadps4/shader_recompiler/ir/passes/ir_passes.h, gpu/shadps4/shader_recompiler/recompiler.cpp, gpu/shadps4/shader_recompiler/runtime_info.h, gpu/shadps4/shader_recompiler/info.h, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.h, gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.h, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp, gpu/shim/gow3_settings.h, gpu/shim/gow3_settings.cpp, gpu/shim/gow3_overlay.cpp
symbols: RefractionLimitPass, OffsetAxis, ExtractedComponent, uses_refraction_offset, refraction_limit, refraction_setting, RefractionLimitDefault
switches:
tests:
docs: plan-work-queue.md
depends_on:
related: light-grid-scale, lineage-draw-preparation
checked:
---
## What it does

UE4 4.21's distortion apply pass (fs `6a5757e32a9f7ad1`) samples the scene at `uv + (texel.xy - texel.zw) * 0.25` from
the accumulated RGBA8 offset buffer and falls back to `uv` when that leaves the view. On the Sleeping Village water
every pixel is shifted 0.13 (1080p) to 0.25 (4K, the RGBA8 limit) of the screen up / down, sideways only 0.001-0.008,
so the water shows a translated, unflipped copy of the scene above it (Dan's sword, a money bag), cut by a hard line
where the fallback starts. The PS4 shows none of it; why is still open (plan-work-queue.md, Sleeping Village water).
A recompiler pass marks fragment shaders with that offset read; draws get a variant that shortens the offset to
`refraction_limit` percent of the screen, direction kept. Default 2%; 0 = as the game draws it. Live in the in-game menu
("Refraction limit" under Effects resolution: Off / 1 / 2 / 4%).

## Invariants

- The pattern is two `FPMul32(FPSub32(e[a], e[a + 2]), 0.25)` (a = 0 and 1, either operand order of the multiply),
  where every `e` is a `CompositeExtractF32x4` of the same sample. The capped offset is built from new extracts right
  after the sample, so it is defined before any user of either component; the old multiplies are replaced
  (`ReplaceUsesWith`).
- Offsets within the limit are unchanged: scale = min(1, limit / max(length, 1e-6)).
- The limit lives in `HwFragmentRuntimeInfo::refraction_limit` (part of `operator==`), so each limit is its own
  permutation; cache versions bumped (shader binary 9, meta 9, pipeline key 7), one rebuild of the shader cache.
- The setting is not in the registers: `PreparedDraw::refraction_setting` records the one a worker picked for, and
  `TryPreparedPipeline` rejects a prepared draw when the setting changed (lineage-draw-preparation's rule).

## Interactions

- light-grid-scale: the same mechanism (pass + runtime info variant + prepared-draw check); both can apply to one
  shader, each `RefreshGraphicsStages` compiles with every selection field.
- Effects resolution, render scale, output resolution: the limit is a fraction of the screen, the same at any size.
- Shader cache: see the key bug in plan-work-queue.md (output-size-dependent SPIR-V); this variant depends only on
  the setting.

## Evidence

- RenderDoc, the offset buffer at the apply draw (research-data/sleeping-village-refraction-20261114/an/disthist-*.txt):
  pond 32266 / 34189, sword 48493, town 19513 (4K) and pond 18794 (1080p). Sideways p90 0.004-0.024; up / down p50
  0.13 (1080p) and 0.24-0.25 (4K), 40-99.8% of water pixels at the 0.25 limit.
- Not run in game yet.

## History and pitfalls

- The offsets match UE4's formula with the game's material data (IOR 1.33); the near-constant up / down shift fits
  UE4's "Index of Refraction" mode on flat surfaces (from memory of UE4, not checked in this build). The cap is a
  port-side workaround, not the PS4's behaviour: it removes the copy, it does not make a mirrored reflection.
