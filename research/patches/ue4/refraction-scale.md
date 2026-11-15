---
id: refraction-scale
title: UE4 distortion (refraction) shift scaled to the PS4's 1080p share of the screen (Sleeping Village water)
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: C
commits: eeadcb3
files: gpu/shadps4/shader_recompiler/ir/passes/refraction_scale_pass.cpp, gpu/shadps4/shader_recompiler/ir/passes/ir_passes.h, gpu/shadps4/shader_recompiler/recompiler.cpp, gpu/shadps4/shader_recompiler/runtime_info.h, gpu/shadps4/shader_recompiler/info.h, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.h, gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.h, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp, gpu/shim/gow3_settings.h, gpu/shim/gow3_settings.cpp, gpu/shim/gow3_overlay.cpp
symbols: RefractionScalePass, FindParams, ConstantSide, DistortionConstant, uses_refraction_scale, refraction_ref_width, refraction_setting, refraction_ps4, RefractionReferenceWidth
switches:
tests:
docs: plan-work-queue.md
depends_on:
related: light-grid-scale, lineage-draw-preparation
checked:
---
## What it does

UE4 4.21's distortion accumulate shaders (one per refracting material; the pond's are `bd2d070ea5f6989d`,
`7303e465cd16f8c4`, `d935aa3f72509ee9`, `74412f6e3a71d886`) compute the screen shift as
`normal.xy * (IOR - 1) * ... * DistortionParams * 0.00023`, where DistortionParams is (proj00, aspect,
UnscaledViewW, UnscaledViewH): the output size (3840 x 2160 at a 4K output with a 2880 x 1620 render). The shift as a
share of the screen therefore grows with the output resolution. The shader tests the scene depth at the shifted spot
(a closer object drops the shift), then stores the shift x4 in RGBA8, which holds at most 0.25 of the screen; the
apply pass (`6a5757e32a9f7ad1`) samples there.

At a 4K output the Sleeping Village water's shift is 0.31-0.34: stored as 0.25, so the apply pass samples a spot the
depth test never saw. Dan's sword appears in the water, with a hard line and flicker. The PS4 (the copied scene in the
water is the game's own look, PS4 footage) shifts 0.13-0.21 at 1080p and samples what it tested.

A recompiler pass finds `FPMul32(x, +-0.00023)` where `x` is a product of DistortionParams reads (index 0 and 2 or 3
of one constant buffer). The variant multiplies the constant by `1920 / UnscaledViewW`, read again from the same
buffer right before the multiply, so every output size (display, custom, the Deck) gets the PS4's 1080p share.
ini `refraction_ps4` (default 1; 0 = the game's), live in the in-game menu ("Refraction scale": PS4 scale / Game,
under Effects resolution).

## Invariants

- The pattern: an immediate +-0.00023 (bit exact) in an `FPMul32` whose other operand reaches, through at most 4
  `FPMul32` levels, `BitCastF32U32(ReadConstBuffer(#handle, #index))` with index 0 and with index 2 or 3, all one
  handle. Lookup and output use the same constant, so the depth test and the stored shift stay consistent.
- The width read is a new `ReadConstBuffer(handle, #2)` with the found read's flags, emitted right before the multiply
  (no dominance questions); scale = 1920 / max(width, 1).
- The variant lives in `HwFragmentRuntimeInfo::refraction_ref_width` (part of `operator==`); cache versions bumped to
  shader binary 10, meta 10, pipeline key 8 (9 / 9 / 7 was the apply-side limit, whose cached flags are wrong for this).
- The setting is not in the registers: `PreparedDraw::refraction_setting` records the one a worker picked for, and
  `TryPreparedPipeline` rejects a prepared draw when it changed (lineage-draw-preparation's rule).

## Interactions

- light-grid-scale: the same mechanism (pass + runtime info variant + prepared-draw check).
- Render scale and effects resolution do not change the shift (DistortionParams carries the output size).
- r.RefractionQuality ("Refraction" in the menu, restart): off skips distortion altogether.

## Evidence

- RenderDoc, the offset buffer at the apply draw (research-data/sleeping-village-refraction-20261114/an/disthist-*.txt):
  4K captures 40-99.8% of water pixels at the 0.25 limit; 1080p capture 18794 |dy| p50 0.128, max 0.208, none.
  DistortionParams: 1080p (1.42815, 1.77778, 1920, 1080); 4K output at 75% (1.42815, 1.77778, 3840, 2160)
  (an/params-1080p.txt, an/params-48493.txt).
- Replay with the constant x0.5 in the four accumulate shaders (scale/replay-48493.txt, replay-34189.txt): saturation
  99.5% -> 0%, |dy| p50 0.168 / 0.154 (PS4 footage about 0.16); the copy of Dan's sword is gone, the money bag
  (behind the water) is still copied, closer to the bag (scale/48493-before.png, 48493-after.png).
- Not run in game yet.

## History and pitfalls

- First tried as a limit on the apply pass's shift (eeadcb3, ini refraction_limit, 2%): it removed the copy but put a
  second Dan in the water, because the accumulate shader had tested the depth at the full shift. Replaced by this.
- The copied scene in the water is the game's: PS4 footage shows the same; only the overflow above 1080p is the port's.
