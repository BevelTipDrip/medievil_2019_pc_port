---
id: reflex-markers-r1
title: "Reflex R1 (tabled, no gain): per-frame IDs from stage A as present IDs, every queue submission tagged, render-submit markers at stage A and `queued`"
area: present
status: active
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits:
files: gpu/shim/gow3_reflex.h, gpu/shadps4/video_core/amdgpu/liverpool.cpp, gpu/shadps4/video_core/amdgpu/liverpool.h, gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shadps4/core/libraries/videoout/driver.h, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.h, gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.h, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h, gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp, tests/test_reflex_ids.cpp
symbols: FrameIds, OpenReflexFrame, TakeReflexFrame, SetPresentFrame, SetSubmitFrame, TakeFlipFrame, SetMarker, LatencySubmissionPresentIdNV, SignalFlip, SubmitFlipInternal
switches: GOW3_REFLEX_MARKERS
tests:
docs: study-sv-critical-path.md, study-sv-gpu-gaps.md, study-sv-vs-other-level.md
depends_on: reflex
related: reflex, display-trace, lineage-draw-pipe, inline-present, menu-idle-redraw, newest-frame
---
## What it does

`GOW3_REFLEX_MARKERS=1` (needs `GOW3_REFLEX=1` and `VK_NV_low_latency2`; start-up line "Vulkan: Reflex frame markers
R1 ..."). Stage A opens a frame at its first top-level graphics submission after the previous flip
(`Liverpool::OpenReflexFrame`): `FrameIds::Reserve` gives its ID, RENDERSUBMIT_START is set, and an in-order task
tells stage B (`SetSubmitFrame`). The flip NOP ends the frame and carries the ID to `SignalFlip` (packet payload),
which hands it to the flip handler (`TakeFlipFrame`) -> `Request::reflex_frame`; RENDERSUBMIT_END at `queued`. The
present uses it as its present ID (`Swapchain::Present(reflex_frame)` -> `FrameIds::Present`). Every
`vkQueueSubmit` (scheduler) and the readback queue's `vkQueueSubmit2` carry `VkLatencySubmissionPresentIdNV`.
`=2`: SIMULATION_START / END at stage A's frame start, before RENDERSUBMIT_START (else as before: after the Reflex
sleep / at the present, with the frame's ID).

## Invariants

- Present IDs rise strictly: `FrameIds::Present` returns the frame's ID if it is above the last present ID, else last
  + 1 (non-game presents: frame 0) and raises the reservation counter to last + 1 + `Window` (64). Both branches are
  above the previous result. `Present` runs under `Scheduler::submit_mutex` (both `swapchain.Present` callers).
- A frame keeps its ID unless a non-game present (menu redraw, loading / blank frame) or a direct flip came between
  its reservation and its present, provided fewer than 64 frames are in flight (`flip_pending_num` caps at 16).
  `tests/test_reflex_ids.cpp` checks both (random mixes, drops, two threads).
- Frames reach the present in reservation order (one request deque). A second present queue or reordering
  breaks the "keeps its ID" claim, not monotonicity.
- Off: `Gow3Reflex::On()` is false after one static load (`Mode()`), no atomic is read; present IDs are
  `++present_id` as before. The flip packet's payload is 8 bytes (was the 4-byte header, unused): same aligned size.
- Every marker goes through `Gow3Reflex::SetMarker` under the sink lock; `Swapchain::Create` / `Destroy` replace the
  handle under it, so no marker names a destroyed swapchain.
- `Gow3Reflex::SetActive` must run before the first frame (Instance constructor); `On()` reads it relaxed.

## Interactions

- reflex: needs it (`low_latency2`). The sleep is unchanged (`ReflexSleepBeforePresent`); with the sleep before
  the present, SIMULATION_START is for that present's ID (`Peek`), with it after, for the last present ID + 1.
- display-trace: present-wait IDs are the same IDs (still increasing).
- lineage-draw-pipe: the frame-start task is a pipelined task (`PipelinedTasks`); with that toggle off it drains
  stage A once a frame (switch on only).
- menu-idle-redraw / inline-present: idle redraws take fresh IDs and raise the counter (gaps of ~64 IDs).
- newest-frame: dropped frames leave gaps (their RENDERSUBMIT markers have no present).
- Untagged: ImGui's font / texture uploads (third-party `vkQueueSubmit`), FFX / NGX internals, `bindSparse`.
  Submissions before stage A's first frame carry none; load-time uploads after it carry the current frame's ID.

## Evidence

- Fix built: release build 2026-11-25 (`build.sh exit: 0`); `reflex-ids-test` passed (commit `c06d354`).
- **Run, no gain; tabled (the user, 2026-11-25).** A/B in the Sleeping Village, same route (steady, wall swinging,
  steady), flip traces in `user/logs/flip-traces`: A off (21:42), B `=1` (22:06), C `=2` (22:17, graphs in
  `research-data/reflex-r1-20261125`). No Vulkan errors, the cap held, menus / loads without stutter. Pacing
  unchanged: wall swinging 166-218 FPS in all three with 8-79 frames over 6 ms a second; in those seconds the
  release -> present_start wait (Reflex's sleep) falls to ~0.1 ms in every run: frames arrive late from the game
  thread, which markers cannot change. Steady stretches: the same banding (late frames 4.3-4.45 ms from the
  game's frame start, catch-up frames after them) with and without. The code stays, off by default; branch
  `reflex-markers` keeps it for a later look (e.g. together with R2 or a faster game thread).

## History and pitfalls

- Proposed as R1 in study-sv-vs-other-level.md ("Where this left off"); R2 moves SIMULATION_* to the game thread.
- Markers before R1 all sat at the present (study-sv-critical-path.md section 3): Reflex saw ~0 ms of CPU work.
- Mode 1 sets SIMULATION_START / END after the frame's RENDERSUBMIT_START / END (they stay at the present), out
  of order for one ID; mode 2 is there in case the driver rejects that.
- Markers from stage A / B run beside the presenter's `vkQueuePresentKHR` / `vkAcquireNextImageKHR` on the same
  swapchain: `vkSetLatencyMarkerNV` is not externally synchronised in vk.xml, but the validation layer's threading
  check may flag it. Not serialised with the present on purpose (stage A would wait on it).
