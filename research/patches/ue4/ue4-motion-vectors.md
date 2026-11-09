---
id: ue4-motion-vectors
title: UE4 motion vectors (U2): the game's velocity, else camera motion from depth and ClipToPrevClip, with a debug view (F10)
area: ue4
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits: daeb5fd, 461f249
files: gpu/shadps4/video_core/renderer_vulkan/vk_ue4_motion.h, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_motion.cpp, gpu/shadps4/video_core/host_shaders/ue4_motion.comp, gpu/shadps4/video_core/host_shaders/ue4_motion_view.comp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shim/gow3_overlay.cpp, gpu/CMakeLists.txt
symbols: Ue4Motion, OnVelocityDraw, OnSceneDraw, OnTaa, Compose, CycleViewMode, ue4_motion, GetUe4Motion
switches: GOW3_UE4_MOTION_VIEW
tests: u2-motion
docs: plan-ue4-motion-vectors.md#u2-motion-vectors-and-their-debug-view
depends_on: ue4-frame-map
related:
---
## What it does

Step U2 of the DLSS / FSR plan. With `GOW3_UE4_MOTION_VIEW` set (1-4, or 0 to start with the picture unchanged),
the first TAA pass of each frame (`fs e88c8fcbe08ea0b6` / `fs e5588ed466d8f2f3` draws, or the TAAU dispatch
`cs b373f39908ae979f`) is preceded by a compute pass (`ue4_motion.comp`) that writes the motion vectors UE4's TAA
uses into an RGBA16F image at the velocity target's size: the game's velocity where it wrote one (encoded x > 0,
decoded `v * 4.008016 - 2.0039775`), else the camera's motion from scene depth and `ClipToPrevClip` (row vector
times matrix). Units: render pixels, previous position minus current (DLSS / FSR's convention); b = 1 where the game
wrote velocity. The game's frame is not changed.

At each flip with fresh vectors the presenter shows a debug view (`ue4_motion_view.comp`) of the display buffer in a
port image instead: 1 colours over the picture, 2 colours only (red / green = x / y, grey = still, 64 px to full;
blue tint = the game's velocity), 3 last frame moved by the vectors, 4 the difference of 3 and this frame (x4: black
where the vectors are right). F10 cycles 1-4 and 0.

## Invariants

- The pass runs once per frame: the View's `FrameNumber` of the first TAA pass is remembered and the second draw
  skipped.
- Velocity is used only when this frame's TAA pass samples the velocity target's address (gameplay frames without
  velocity draws too, U1; not menus, whose TAA samples none): else camera motion only. Depth: the velocity pass's,
  else the TAA draws' depth buffer, else the last G-buffer pass's (3+ colour targets). Images are kept
  by id and `image_uid` (`TryGetImage`), so a freed slot is never used.
- The pass is recorded before the TAA pass binds its resources: `EndRendering`, then `runtime.Transit` of velocity
  and depth to General (tracked, so TAA's own binding moves them back).
- `Compose` runs on the presenter's open command buffer of the draw scheduler: no `Finish` there (a resized own image
  is destroyed by `DeferOperation`). It reads the display buffer through a UNORM view; only RGBA8 display buffers.
- Off (`GOW3_UE4_MOTION_VIEW` unset): `ue4_motion` is null, F10 does nothing.

## Interactions

- ue4-frame-map: the same per-pass observation (`ObserveFrameMap`) feeds both; either switch turns it on.
- The view replaces the presented image before FSR 1 / RCAS, so those run on the debug picture.

## Evidence

- Run `u2-motion` (PC, 2026-11-08, build `357ef5d`, 1.0 game, render scale 80, `data/u2-motion.log`, the user's
  screenshots): the pass ran every gameplay frame (126,000 passes). Mode 2 panning (camera orbiting Dan), measured
  in the screenshot: Dan (game velocity) 0.3 / -0.3 px; camera pixels -24.8 px far top left to +8.8 near bottom
  right, smooth; tombstones (game velocity) +9.8 vs +5.3 / +6.3 beside them and -6.3 vs -12.8 / -18.3 behind: same
  signs, sizes by depth. Still camera: background exactly 0, only actors, the sword (gradient) and breakables
  marked. The user: "I believe we're looking at the right data". Mode 4 screenshot (camera still): black but for
  torch flares and tree edges.
- Found in that run: the drawn colours came out red / blue swapped (the presenter views the RGBA display buffer as
  BGRA, `GetFrameViewFormat`); menus and the title have no velocity pass, so the pass was skipped there. Fix built
  (this card's second commit): `swap_red_blue`, and a camera-only pass when TAA does not sample the velocity target
  (scene depth from the G-buffer pass). Not run.

## History and pitfalls

- First version.
