---
id: ue4-fsr
title: AMD FSR 3.1 in place of UE4's TAA - native AA at render scale 100, upscaling below it; an Ue4Upscaler backend
area: ue4
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits:
files: gpu/shadps4/video_core/renderer_vulkan/vk_ue4_fsr.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shim/gow3_overlay.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_upscaler.h, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_upscaler.cpp, launcher/medievil_launcher.py, gpu/shim/gow3_settings.cpp, gpu/shim/gow3_graphics.h, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_motion.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_motion.h, gpu/shadps4/video_core/host_shaders/ue4_motion.comp, gpu/shadps4/video_core/host_shaders/ue4_motion_view.comp
symbols: FsrBackend, MakeFsrBackend, SharpensItself, FsrRunning, sharpness, fsr_sharpness, ParseFsrSharpness, fsr_reactive, ParseFsrReactive, reactive_pipeline, reactive_written, REACTIVE, Describe, ffxVkPortableUpscaleContextCreate, ffxVkPortableUpscaleContextRecordDispatch, near_plane, fov_y
switches: GOW3_FSR
tests:
docs: plan-fsr.md
depends_on: ue4-dlaa, ue4-motion-vectors
related: ue4-frame-map
---
## What it does

`ue4_upscaler=fsr` (menu Image tab, launcher Graphics, F9) runs AMD FSR 3.1 where UE4's TAA would run, through the
generic front end of ue4-dlaa (`Ue4Upscaler`): native anti-aliasing at the first native TAA draw (render scale 100),
upscaling at the TAAU dispatch below it. The backend (`FsrBackend`, vk_ue4_fsr.cpp) uses the vendored FSR-Vulkan
portable upscaler (gpu/third_party/fsr-vulkan, FSR 3.1.4 from AMD's MIT source, already linked for the God of War
III upscaler):

- Context flags: HDR colour, auto exposure, depth inverted, depth infinite; max render = input size, max output =
  output size (a new context on any size change, as DLSS).
- Inputs as the front end gives them: the RGBA16F colour TAA samples, the D32S8 depth (depth aspect), U2's RGBA16F
  motion vectors (scale 1: render pixels, previous minus current, FSR's convention), the jitter DLSS uses (F8 cycles
  its sign), `cameraNear` = View near plane (cm), the far plane infinite (FLT_MAX), the vertical FOV from
  `ViewToClip[1][1]` (View offset 384 + 20), `viewSpaceToMeters` 0.01, pre-exposure 1.
- Every image in VK_IMAGE_LAYOUT_GENERAL: FidelityFX's Vulkan backend maps GENERIC_READ and UNORDERED_ACCESS to
  GENERAL and returns each image to its state after the dispatch.
- Reactive mask (step 4, fix built 2026-11-14, not run): `fsr_reactive` (0-100, 0 = off, default 50 provisional;
  menu Display tab and launcher "AMD FSR reactive mask"). While FSR runs with it above 0, U2's motion pass runs its
  REACTIVE variant (`ue4_motion_reactive_comp`): a stencil-aspect view of the same D32S8 depth-stencil, bit 3 (UE4's
  responsive AA, the bit the game's TAA tests) -> an R8_UNORM mask, strength / 100 there and 0 elsewhere, at render
  size; `LastPass::reactive` -> `Ue4UpscaleFrame::reactive` -> `reactiveMask`. Without a stencil (D32 only), R8
  storage or the setting, the plain pass runs as before and FSR gets its own 1x1 blank mask. Motion alpha = the
  bit, shown by the motion debug view's mode 5 (`GOW3_UE4_MOTION_VIEW=5`, F10). Cost estimate: ~2 bytes a pixel in
  the motion pass plus one byte a pixel in FSR's first pass (its blank mask was 1x1).
- Sharpening is FSR's own RCAS at its own strength, `fsr_sharpness` (0-100, 0 = off,
  default 25; menu Image tab and launcher "AMD FSR sharpening"): `sharpness` = strength / 100, the same look as the
  presenter's RCAS at that strength. The presenter's RCAS and its strength stay off while FSR runs (F2b, F2c).
- Available where the device enables `shaderStorageImageWriteWithoutFormat` (FSR 3's accumulate shaders); `GOW3_FSR=0`
  leaves it out.

## Invariants

- `ffx_vk_portable.h` after our Vulkan headers (vk_common.h): first, its `<vulkan/vulkan.h>` came from the
  vendored headers and clashed with MSYS2's vulkan.hpp (build error in vulkan_enums.hpp).

## Interactions

- ue4-dlaa: the same front end and hooks; switching between DLSS and FSR resets the history ("upscaler changed").
- The presenter's RCAS stays off while FSR runs (`Ue4Upscaler::SharpensItself`); over DLSS it runs (F2b).
- The God of War III upscaler (vk_temporal_upscaler, off for MediEvil) links the same library; both can exist, only
  this one runs for MediEvil.

## Evidence

- T-F2 (plan-fsr.md step 2): FSR 3.1 runs at 75%, 50% and native.
- T-F6: FSR's own sharpening from the menu's strength verified (the user). F2c, its own slider apart from RCAS
  (`fsr_sharpness`): verified in T-F7 (menu and launcher).

## History and pitfalls

- 2026-11-12: built for step 2 of plan-fsr.md.
