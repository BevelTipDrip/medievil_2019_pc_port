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
files: gpu/shadps4/video_core/renderer_vulkan/vk_ue4_fsr.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shim/gow3_overlay.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_upscaler.h, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_upscaler.cpp, launcher/medievil_launcher.py
symbols: FsrBackend, MakeFsrBackend, SharpensItself, sharpness, Describe, ffxVkPortableUpscaleContextCreate, ffxVkPortableUpscaleContextRecordDispatch, near_plane, fov_y
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
- No reactive mask (step 4). The menu's sharpening is FSR's own RCAS (`sharpness` = strength / 100, the same
  look as the presenter's RCAS at that strength), the presenter's RCAS off while FSR runs (F2b).
- Available where the device enables `shaderStorageImageWriteWithoutFormat` (FSR 3's accumulate shaders); `GOW3_FSR=0`
  leaves it out.

## Invariants

- `ffx_vk_portable.h` after our Vulkan headers (vk_common.h): first, its `<vulkan/vulkan.h>` came from the
  vendored headers and clashed with MSYS2's vulkan.hpp (build error in vulkan_enums.hpp).

## Interactions

- ue4-dlaa: the same front end and hooks; switching between DLSS and FSR resets the history ("upscaler changed").
- The presenter's RCAS stays off while FSR runs (`Ue4Upscaler::Running`), as with DLSS.
- The God of War III upscaler (vk_temporal_upscaler, off for MediEvil) links the same library; both can exist, only
  this one runs for MediEvil.

## Evidence

- Fix built (plan-fsr.md step 2), not run.

## History and pitfalls

- 2026-11-12: built for step 2 of plan-fsr.md.
