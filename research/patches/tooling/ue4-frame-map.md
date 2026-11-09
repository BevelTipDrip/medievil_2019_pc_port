---
id: ue4-frame-map
title: UE4 frame map (U1): logs the velocity pass, TAA (draws or compute), the View buffer, tonemap and first UI draw each frame
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits: ba013b4
files: gpu/shadps4/video_core/renderer_vulkan/vk_ue4_frame_map.h, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_frame_map.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h, tests/test_ue4_frame_map.cpp, gpu/CMakeLists.txt
symbols: Ue4FrameMap, ObserveFrameMap, frame_map
switches: GOW3_UE4_FRAME_MAP
tests:
docs: plan-ue4-motion-vectors.md#u1-run-time-frame-map-log-only, study-taau-frame-map.md#5-shader-identity-the-port-can-match-at-run-time
depends_on:
related:
---
## What it does

Step U1 of the DLSS / FSR plan. With `GOW3_UE4_FRAME_MAP=1` the rasterizer hands every direct draw, indirect draw
and direct dispatch (before its resources are bound) to `Ue4FrameMap`: colour target (address, size, R16G16_UNORM
or not, display buffer or not), depth buffer, sampled and storage image addresses, and a bound 3,296-byte buffer.
The map finds by role or by the captures' shader hashes:

- velocity: draws into one R16G16_UNORM target with a depth buffer (per-material shaders: by target, not hash);
- TAA: `fs e88c8fcbe08ea0b6` / `fs e5588ed466d8f2f3` draws, or the `cs b373f39908ae979f` dispatch (TAAU, inferred),
  its output, whether it samples velocity and depth, and the View buffer bound to it (jitter, view and buffer
  size, frame number, jitter index, camera cut, mip bias, pre-exposure, near plane, ClipToPrevClip vs identity);
- `cs a080f599034de78e` (compiled with the TAAU CS, role unknown): count and groups;
- display buffer draws: shaders in order, tonemap `fs 3632d4c4faaa85d2`, first UI `fs 4fe20af683865713`, other
  draws between them;
- every shader that samples the velocity target (once each, the first time).

Log lines (`UE4 frame map:`): one when the frame's shape changes (roles, hashes, sizes), camera cuts, new velocity
readers (at most 10 change / cut lines per summary), and every 300 frames a count of frames with each role plus
the last frame in full. Nothing in the frame changes.

## Invariants

- Read only: `ObserveFrameMap` runs before `BindResources` and changes no rasterizer state. `FindPreparedStage`
  needs `bind_prepared`, still set at that point (direct draws); dispatches and indirect draws read the sharps
  from user data, as `BindTextures` / `BindBuffers` do without a prepared stage.
- Off: `frame_map` is null and each draw / dispatch costs one pointer test.
- The View buffer is read from guest memory when the TAA pass is recorded (stage B), like `CameraMotion::OnConstants`.
  If the game had already rewritten it, the frame number would jump: the summary's "View frame gaps" counts that.
- A frame closes at a draw into a display buffer other than the frame's, or a second tonemap draw. A game with one
  display buffer and no tonemap (menus) would show no lines for that time.

## Interactions

- None with the upscaler path (off for MediEvil, `GOW3_UPSCALER=none`); the hook sits next to its `OnDraw` /
  `OnDispatch` calls and does not use them.

## Evidence

- `ue4-frame-map-test` (synthetic native and TAAU frames, the View buffer, a camera cut, the summary) passes, built
  standalone with clang 23 (`-Wall -Wextra`, no warnings). Not run in the game.

## History and pitfalls

- First version. The TAAU compute hash comes from the shader cache only (study-taau-frame-map.md): a TAAU run
  with this map shows whether that dispatch is TAA.
