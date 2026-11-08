---
id: renderdoc-capture
title: RenderDoc and frame-map capture helpers: run under renderdoccmd, capture at a pass into a WxH target, texel statistics
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: d560f43720, b5c0be242c, b153cab657, c5377560e9, f495797b26
files: run.py, gpu/shadps4/video_core/renderer_vulkan/vk_frame_capture.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_frame_capture.h, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h, research/tools/renderdoc/trigger_capture.py
symbols: TargetWatchSpec, WatchSpec, FrameCapture::TargetWatch, FrameCapture::StartForTarget, FrameCapture::TargetRecording, target_recording, BeginPass, BeginRenderingFull, LogTexelStats
switches: GOW3_CAPTURE_ON_TARGET, GOW3_CAPTURE_ON_TARGET_COUNT, GOW3_CAPTURE_TRIGGER, GOW3_FRAME_CAPTURE_KEY
tests:
docs: plan-linux-deck.md#hilltop-mausoleum-lighting-pool-pages-leaked-on-linux-2026-11-0607-fixed-7246f88--f74e1a1, plan-baseline-issues.md#v1-shadows-sometimes-drawn-as-a-big-box-the-user-2026-10-18, fix-inventory.md#9-diagnostics-worth-taking
depends_on:
related: mausoleum-diagnostics, pool-kept-pages, mausoleum-toggle-bisect, V4, upstream-frame-analyzer-fix, upstream-screenshots, extra-env-file
checked: 2026-11-07
---
## What it does

`GOW3_RENDERDOC=<capture path>` (run.py, not a C switch) wraps the game command in `renderdoccmd capture -w -c`
(`GOW3_RENDERDOC_CMD` overrides the exe path); RenderDoc's key saves `<capture>_frameN.rdc`. On the Deck the
layer is loaded with `VK_LOADER_LAYERS_ENABLE=VK_LAYER_RENDERDOC_Capture` in extra-env.txt and
`trigger_capture.py` (inside qrenderdoc, SSH tunnel to port 38920) triggers a capture remotely. The frame map
(upstream's `FrameCapture`, `GOW3_CAPTURE_TRIGGER` / F11) gained `GOW3_CAPTURE_ON_TARGET=WxH[:Format]`: a pass whose
colour target has that size (and `vk::to_string` format) starts a recording, at most
`GOW3_CAPTURE_ON_TARGET_COUNT` times (default 2), for load-time work no flip-armed capture reaches (UE4's sky light
capture). While such a recording runs, `BindTextures` logs `LogTexelStats` for each sampled float texture. The
replay scripts (`pass_images.py`, `pixel_draw_textures.py`, `debug_pixel_values.py`, `resource_names.py`) came
with `e383f7e` (mausoleum-diagnostics).

## Invariants

- `StartForTarget` runs only in `BeginRenderingFull` and only while the capture is `Idle` (not Armed or
  Recording): a memo hit (`BeginRendering` returns `begin_memo.state` early) is never checked, which is safe only
  because a hit needs the render pass already open with the same targets. A watched pass that begins while a
  flip-armed capture is Armed or Recording is not checked, and does not start one later in the same pass.
- A target-started recording may see no frame boundary during a load: `BeginPass` writes and stops any recording at
  400 entries (otherwise at the first display pass after a scene pass, as a flip-armed one). Removing that cap lets
  a load-time recording grow without bound.
- `LogTexelStats` copies through a staging buffer and reads the result in a deferred priority operation (no GPU
  wait); only float formats up to 256x256, depth 1, once per (address, format, width), up to 16 mips.
- Windows: renderdoccmd injection. Deck: injection "kills the runtime"; use the loader variable only.

## Interactions

- mausoleum-diagnostics: same hunt; the frame map end rule (past the UI passes) and `GOW3_CAPTURE_BUFFER_BYTES`
  are in that card. pool-kept-pages: these tools showed the Mausoleum images held never-written guest memory.
- mausoleum-toggle-bisect: the method these tools replaced.
- V4: the first RenderDoc captures (`GOW3_RENDERDOC`, 2026-10-18) found the stencil-plane cause of V1.
- upstream-frame-analyzer-fix: the frame map's buffer dump these captures rely on.
- extra-env-file: carries the RenderDoc layer variable on the Deck (`VK_` prefix allowed).
- Any capture changes timing, and while the frame map is Armed or Recording the render-state memo, the
  texture-set memo and the bind helper are off (`FrameCapture::Active()` checks): no frame-time numbers from
  captured runs.

## Evidence

- V1 (PC, 2026-10-18, launched by Claude with the user's go; `_frame22745.rdc`, `_frame24679.rdc`): pixel history
  showed projection eid 6479 writing R = 0.000 inside the box; the stencil was already 0x80 before the volume.
- Mausoleum (Deck), victims found with these tools: the sky light capture cubemap (128x128 FP16, "NaN and
  +-65504 in every mip"), the 9x1 RGBA16F SH target, the luminance mips. Tooling; the fix was verified in game, not
  these tools.

## History and pitfalls

- F12 is RenderDoc's capture key (run.py: "F12 / Print Screen") and also the port's PNG screenshot key
  (upstream-screenshots): under `GOW3_RENDERDOC` one press probably does both (inferred, not tested); Print Screen
  avoids it.
- Remote replay on the Deck: `renderdoccmd remoteserver` in the SSH session (tunnel 39931 -> 39920); in Git Bash set
  `MSYS2_ENV_CONV_EXCL=RD_CAPTURE` so the path is not rewritten.
