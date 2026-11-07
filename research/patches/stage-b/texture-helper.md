---
id: texture-helper
title: Texture binding helper: stage B forks texture binding to a spinning helper (GOW3_TEXTURE_HELPER)
area: stage-b
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: bloodborne efb428d
class: C
commits: efb428d
files: gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_bind_helper.h, gpu/shim/gow3_toggles.h
symbols: BindHelperWanted, HelperEligible, TexturesBindableOnHelper, RunTextureTask, JoinBindHelper, BindHelper, TextureBindHelper
switches: GOW3_TEXTURE_HELPER, toggle:TextureBindHelper
tests:
docs: study-deck-rescore-rejected.md#a-switches-that-already-exist-no-build, docs/parallel_gpu.md, plan-baseline-issues.md#i2-frame-times-oscillating-50--120-fps-gpu-not-saturated
depends_on:
related: lineage-texture-set-memo, texset-region-stamps, T1, T2, sampler-memo, lineage-draw-pipe, stage-b-core
verified: 2026-11-07
---
## What it does

With `GOW3_TEXTURE_HELPER=1` (`BindHelperWanted`, read once at start), `Rasterizer::BindResources` forks the
texture binds of an eligible graphics draw to a spinning `BindHelper` thread (`RunTextureTask`) while stage B binds
the buffers and resolves vertex / index buffers, then joins at the end of `BindResources`, before the descriptors are
pushed (so before `BeginRendering`). Stages the helper gives up on are bound after the join. Toggle word 1 bit 34 (`17179869184`) switches it off live; it cannot switch it on.

## Invariants

- Eligibility (`HelperEligible`): graphics only, no frame capture, no mip-array images, no written storage buffers,
  and the texture-binding memo and update-image fast path not toggled off. Per stage, `TexturesBindableOnHelper`
  refuses written images, an image-description entry from an older registry generation, `needs_rebind`, an image
  not up to date, a scene-target proxy or an upscaler redirect: anything that would record commands.
- Both sides know their descriptor slots up front: `set_writes` is resized before `Fork`, buffers first, then
  images, then samplers per stage. A new per-stage descriptor kind or ordering breaks this split.
- Stage B joins before it changes image state: `JoinBindHelper` is the runtime's image access hook
  (`SetImageAccessHook`), run by `Runtime::BeforeImageAccess` in `Transit`, barrier flushes and the buffer cache's
  image sync. Code added later in `BindTextures` or its callees runs on two threads at once with buffer binding: it
  must not touch state stage B touches during the fork.
- [inferred from code] The helper calls `BindTextures(..., on_helper = true)`, which skips `BindTexturesFromSet`:
  stages bound on the helper never hit the texture-set memo and never refill it.

## Interactions

- lineage-texture-set-memo, texset-region-stamps, T2: masked by the helper for every stage it binds (the memo is
  skipped on the helper), so on MediEvil, where the memo hits 92-99%, the helper would trade memo hits for full slow
  binds [inferred].
- T1, sampler-memo: added after the helper was measured; they run inside `BindTextures` on the helper thread too.
  The sampler memo is `thread_local`, so the helper keeps its own copy. Never tested together.
- lineage-draw-pipe: the helper was measured before stage B was a separate thread from stage A. Never run with the
  draw pipe, lean binding, the sampler memo or region stamps (rescore A5: race risk, wrong textures).
- stage-b-core: both assume a spare core; the Deck has none.

## Evidence

- Bloodborne (16 threads): 68% of draws bound in parallel, 85.9 FPS with the helper vs 85.8 without; the helper's
  task ~6200 cycles per fork against ~4600 on the GPU thread, and the GPU thread still waited ~2500 cycles per fork
  in the join (docs/parallel_gpu.md).
- MediEvil: never run. Baseline logs show "Texture helper: 0 binds in parallel, ... 976896 serial"
  (plan-baseline-issues.md:120). Rescore A5 (Deck): -1.4 to +1.0 ms a frame, most likely <= 0. Not verified in game.

## History and pitfalls

- Dropped because per-draw fork / join over ~7 us of cache-bound work does not pay: the descriptor infos and image
  states the helper writes are read by stage B right after, so each draw moves them between cores.
- docs/parallel_gpu.md's heading still names the switch-off bit "toggle 1 << 22"; today it is bit 34.
- Retry only with a spare physical core and after checking the race list above; first make the helper use the
  texture-set memo, or it starts from a loss. Test: `GOW3_TEXTURE_HELPER=1 GOW3_FRAME_STATS=1`, bit 34 on / off every
  30 s, the "Texture helper:" line, screenshots for wrong textures.
