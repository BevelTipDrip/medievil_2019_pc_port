---
id: light-grid-scale
title: UE4 forward light grid lookup scaled in separate translucency below 100% (Dan's Crypt water)
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: C
commits:
files: gpu/shadps4/shader_recompiler/ir/passes/light_grid_scale_pass.cpp, gpu/shadps4/shader_recompiler/recompiler.cpp, gpu/shadps4/shader_recompiler/runtime_info.h, gpu/shadps4/shader_recompiler/info.h, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.h, gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.h, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp
symbols: LightGridScalePass, FragCoordComponent, uses_light_grid, light_grid_scale_x, light_grid_scale_y, light_grid_scale, light_grid_scene, LightGridScale, LightGridViewport, RecordLightGridScene, g_light_grid_scene
switches:
tests: T-LG1
docs: study-light-grid-scale.md#t-lg1-the-fix-in-game-2026-11-10-launched-by-claude-with-the-users-go-log-datacrypt-light-grid-fix-0102log
depends_on: effects-resolution
related: lineage-draw-preparation
checked:
---
## What it does

UE4 4.21 finds a pixel's forward light grid cell as `uint(SvPosition.xy - ViewRectMin) >> LightGridPixelSizeShift`.
The grid is built for the full view, but separate translucency below 100% (`effects_resolution`) draws smaller and
does not scale the position, so it reads the grid's top-left part: reflections with hard edges on 64-px lines. A
recompiler pass marks shaders with the lookup; draws at the effects-resolution size of the frame's full view get a
fragment variant that multiplies the position by full / small per axis before the conversion.

## Invariants

- The pattern is `ShiftRightLogical32(ConvertU32F32(f), non-immediate)` where `f` comes from FragCoord x or y through
  at most 4 float add / sub / neg / mul / fma / min / max (`FragCoordComponent`). A constant shift is never matched.
- The full view is the viewport of the last draw with >= 5 colour targets and depth (the G-buffer pass,
  `RecordLightGridScene`), stored in `g_light_grid_scene` on the GPU thread for every draw before
  `TryPreparedPipeline`. A scale is used only when the draw's viewport is within 2 px of full x the
  `effects_resolution` percentage (`LightGridScale`); 100% or an unknown value: no scale.
- The scene is not in the registers: `PreparedDraw::light_grid_scene` records the one the worker picked for, and
  `TryPreparedPipeline` rejects a prepared draw when it differs (lineage-draw-preparation's rule).
- The scale lives in `HwFragmentRuntimeInfo` (part of `operator==`), so each scale is its own permutation; the cache
  versions were bumped (shader binary 8, meta 8, pipeline key 6), one rebuild of the shader cache.

## Interactions

- effects-resolution: only active below 100; the live switch picks the variant on the next draw.
- lineage-draw-preparation: see the third invariant.
- Render scale, DLSS: the scale is a ratio of viewports, independent of the render size.

## Evidence

- RenderDoc at 75% / 100% (study-light-grid-scale.md): edges on 64-px lines, the shader's grid lookup, the 31x17 of
  41x23 cells read at 75%.
- T-LG1 (PC, 01.02): verified. The user, at 50, 75 and 100 live: "Yes, all 3 now look correct!". Six shaders matched,
  scaled variants at 2.0008 x 2.0000 and 1.3339 / 1.3335 x 1.3333.

## History and pitfalls

- First read as a refraction / DLSS problem; the user found `effects_resolution` was the cause.
- Recording the scene inside `RefreshGraphicsKey` missed draws that took a prepared pipeline; moved to
  `GetGraphicsPipeline` before the prepared check.
- No switch of its own: it only changes draws that were wrong; the A/B is `effects_resolution=100`.
- Not checked: the forest level's cauldron fog (suspected same cause).
