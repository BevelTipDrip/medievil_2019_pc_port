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
commits: eeadcb3, 9d2f49a, 464877a, d60e380, ac5e385
files: gpu/shadps4/shader_recompiler/ir/passes/refraction_scale_pass.cpp, gpu/shadps4/shader_recompiler/ir/passes/ir_passes.h, gpu/shadps4/shader_recompiler/recompiler.cpp, gpu/shadps4/shader_recompiler/runtime_info.h, gpu/shadps4/shader_recompiler/info.h, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.h, gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.h, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp, gpu/shim/gow3_settings.h, gpu/shim/gow3_settings.cpp, gpu/shim/gow3_overlay.cpp
symbols: RefractionScalePass, FindParams, ConstantSide, DistortionConstant, uses_refraction_scale, refraction_ref_width, refraction_setting, refraction_ps4, RefractionReferenceWidth, refraction_ripples, water_ripples_always, RefractionSetting, refraction_edge_stretch, SoftEdge, EdgeBand, ChainCopy, StoredProduct, FallbackLean
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

Optional, not the PS4's look: the same shaders carry the material's time pulse
`saturate(sin(fract(material[52] * time)))` (0.1: a 10 s cycle, rippling 5 s, still 5 s). Still water shows the
apply pass's off-screen fallback as a hard line near the top edge at steep angles. ini `water_ripples_always`
(default 1 for testing; 0 = the game's cycle), live as "Water ripples" (Always / Game cycle): the saturate becomes
abs, so the second half mirrors the first. Only the refraction ripples through; the water surface's own shading
keeps the game's cycle.

Optional, not the PS4's look either: where the shifted spot leaves the screen the apply pass falls back to no shift,
a hard line with the pond bed past it and no water effect (the ripples ride on the shift). ini
`refraction_edge_stretch` (default 1 for testing), live as "Refraction edge" (Stretch / Game (hard line)): the
spot is squeezed into the screen before the depth test, `edge + 0.15 * exp((t - edge - 0.15) / 0.15)` within 15% of
an edge (value and slope continuous): near the edge an almost exact copy, stretching further from it.
Modes 2 and 3 ("Stretch + ripple", "Stretch + stepped ripple", ini 2 / 3; default 3 for testing) also fill the
spots whose depth test drops the shift (a closer object at the shifted spot, the arch over the pond): a vertical
ripple, `clamp(0.012 + 6 * sideways shift, 0, 0.03)` down, sideways unchanged; mode 3 first tries 2/3, then 1/3
of the shift with copies of the shader's own depth test (up to two more depth reads per water pixel).

## Invariants

- The pattern: an immediate +-0.00023 (bit exact) in an `FPMul32` whose other operand reaches, through at most 4
  `FPMul32` levels, `BitCastF32U32(ReadConstBuffer(#handle, #index))` with index 0 and with index 2 or 3, all one
  handle. Lookup and output use the same constant, so the depth test and the stored shift stay consistent.
- The width read is a new `ReadConstBuffer(handle, #2)` with the found read's flags, emitted right before the multiply
  (no dominance questions); scale = 1920 / max(width, 1).
- Ripples: only in shaders where the constant was found, only `FPSaturate32(FPSin(FPFract32(x)))`, replaced in place
  by `FPAbs32` (same operand). The pond's other sine (unclamped) is not touched.
- Edge stretch: per axis the two multiplies share their factor operand; the one with an `FPAdd32` user is the depth
  test's spot (`uv + shift`), the other the stored shift and must come after that add in its block (else skipped).
  The add's users get the squeezed spot, the stored shift becomes `spot - uv`, so the depth test and the sample
  agree. Bounds 0.001-0.999 of the buffer UV: the port's views fill their render targets (2880 x 1620 view and
  buffer at 75%); the apply pass's own bounds test stays as the backstop.
- Fallback: the stored shift's multiply by the depth test (`FPMul32` user of the stretched shift) is replaced; x and
  y must use the same test. Mode 3 copies every instruction from the stretched spot to the test (`ChainCopy`) in front
  of the first stored multiply; a phi or a side effect on that chain falls back to mode 2 (logged). The ripple is
  vertical on purpose: any sideways or upward unchecked shift pulled the arch's dark columns into the water.
- The variants live in `HwFragmentRuntimeInfo::refraction_ref_width` and `refraction_ripples`, `refraction_edge_stretch` (part of `operator==`);
  cache versions bumped to shader binary 11, meta 11, pipeline key 9 (9 / 9 / 7 was the apply-side limit, 10 / 10 / 8
  the scale alone, 11 / 11 / 9 with ripples; 12 / 12 / 10 the edge stretch; now 13 / 13 / 11 with the fallback modes).
- The setting is not in the registers: `PreparedDraw::refraction_setting` records the one a worker picked for, and
  `TryPreparedPipeline` rejects a prepared draw when it changed (`RefractionSetting`: bit 0 scale, bit 1 ripples, bits 2-3 edge mode) (lineage-draw-preparation's rule).

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
- Scale run in game (2026-11-15, 4K DLSS 100%): the log found the constant in 7 shaders (4 each) and built the 1920
  variants. The user: "it does appear to work", the copy of the money bag sits about 1/6 of the screen below it like
  the PS4's lanterns, no Dan or sword in the water, the town-centre flicker "does appear to be gone". The hard line
  stays at steep angles, higher up the screen; it stands out in the still half of the pulse.
- Ripples: replay of 32266 (a still phase, pulse/replay-32266.txt) with abs(sin): the copied image ripples. No capture
  at the steep angle. Run in game: "ripple pulses 1" on the pond's 7303e465cd16f8c4 only; the user: the hard line
  stays, above it no ripple at all, "like there's no water there" (the fallback drops the shift and its ripples).
- Edge stretch: GLSL replay of 32266 in the pond shader (pulse/stretch-apply.png, s05stretch-apply.png): at the game's
  scale the horizontal line is gone, a stretched copy of the stones, posts and money bag above it. Run in game: "stretched
  axes 2" in every water shader; the user: "significantly better"; the arch over the pond still breaks it (54% of the
  pond's pixels under the arch dropped by the depth test, gate/dbg7342-mask.png, capture 22245).
- Fallback: GLSL replays of the arch frame (gate/*-final.png): sideways ripple (x6, x12) and an unchecked 25% copy pull
  black patches from the columns; vertical x6 lean 1.2% clean; x15 lean 2% pulls in the near bank; stepped + vertical
  the most water-like. The user picked vertical x6 and stepped + vertical. Not run in game yet.

## History and pitfalls

- First tried as a limit on the apply pass's shift (eeadcb3, ini refraction_limit, 2%): it removed the copy but put a
  second Dan in the water, because the accumulate shader had tested the depth at the full shift. Replaced by this.
- The copied scene in the water is the game's: PS4 footage shows the same; only the overflow above 1080p is the port's.
