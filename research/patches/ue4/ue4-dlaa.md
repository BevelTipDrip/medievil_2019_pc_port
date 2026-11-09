---
id: ue4-dlaa
title: DLAA in place of UE4's TAA at render scale 100 (U3), F9 switches live
area: ue4
status: diagnostic
platforms: windows
default_windows: off
default_deck: n/a
origin: ours
class: M
commits:
files: gpu/shadps4/video_core/renderer_vulkan/vk_ue4_dlss.h, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_dlss.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_motion.h, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_motion.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_dlss.h, gpu/shadps4/video_core/renderer_vulkan/vk_dlss.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_frame_map.h, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shim/gow3_overlay.cpp
symbols: Ue4Dlss, OnColorTarget, OnTaaDraw, Running, GetPass, depth_inverted, ue4_dlss, target_rgba16f
switches: GOW3_UE4_DLSS
tests:
docs: plan-ue4-motion-vectors.md#u3-design-dlaa-at-render-scale-100, plan-dlss.md#d0-2026-11-08-bridge-built-support-checked-offline
depends_on: ue4-motion-vectors
related: ue4-frame-map
---
## What it does

Step U3. With `GOW3_UE4_DLSS=1` (and `gow3_dlss.dll` + `nvngx_dlss.dll` beside the exe), at the first native TAA
draw (`fs e88c8fcbe08ea0b6`) DLSS runs as DLAA (quality 0, input = output = the View size) on:
- colour: TAA's colour input, the last single RGBA16F target drawn before TAA (the separate translucency composite),
  accepted only if the TAA draw samples its address;
- depth: the scene depth U2's motion pass read, `depth_inverted` (new field of `Dlss::FeatureDesc`);
- motion: U2's image (render pixels, previous minus current, scale 1);
- jitter: View `TemporalAAJitter.xy` as pixels (`x * W / 2`, `-y * H / 2`); HDR with auto exposure; sharpness 0.

Its output (an RGBA16F image of the port) is copied into TAA's colour target, and both TAA draws are skipped. Resets:
the first frame after UE4's TAA ran, a View frame number that is not the last + 1, a ClipToPrevClip element more than
1.0 from identity, a new feature. F9 switches DLAA and UE4's TAA live. RCAS is off while DLAA ran in the last 100 ms
(the user's decision). Under TAAU (the compute TAA) and without the DLLs nothing changes.

## Invariants

- Skipping a TAA draw returns from `DrawRecord` after `PrepareRenderState`, clearing `bind_prepared` and calling
  `ResetBindings(false)`, before `BindResources`: nothing of the draw is recorded.
- The second TAA draw of a frame is skipped only when DLSS ran for that View frame number.
- DLSS is recorded on `scheduler.CommandBuffer()` (direct mode, after U2's recorded motion pass); feature creation
  waits for submitted work first (`WaitSubmitted`, as Bloodborne's `RecordDlss`).
- `Dlss::FeatureDesc::depth_inverted` defaults to false: Bloodborne's upscaler path is unchanged.
- The NVIDIA DLLs never go in git. For testing, NVIDIA's development `nvngx_dlss.dll` replaces the release one in
  `out\` (the release copy kept as `out\nvngx_dlss.dll.release`): Ctrl+Alt+F12 debug overlays, Ctrl+Alt+F9 / F10
  jitter sign / swap.

## Interactions

- ue4-motion-vectors: `GOW3_UE4_DLSS` creates the motion pass too (view mode 0); its `GetPass` hands over the image
  and depth of the same View frame.
- RCAS (presenter): off while DLAA runs.

## Evidence

- Builds (clang 23, PC). Not run.

## History and pitfalls

- First version. CameraCut is never set by this game (U1, U2), hence the frame number / camera jump resets.
