---
id: upstream-safe-gpu-mode
title: Upstream safe GPU mode for older AMD drivers; GPU / driver log line; crash reports list the barrier batch
area: stage-b
status: active
platforms: all
default_windows: off
default_deck: off
origin: upstream 1.3.6
class: G
files: gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_runtime.cpp, gpu/shadps4/video_core/texture_cache/image.cpp, src/probe.c, launcher/medievil_launcher.py
symbols: VulkanCompat, EnvOn, maintenance_5, maintenance_8, attachment_feedback_loop, image_2d_view_of_3d, recording_barriers, gow3gpu_dump_recording_barriers, DescribeImageHandle
switches: GOW3_VULKAN_COMPAT, GOW3_NO_MAINTENANCE5, GOW3_NO_MAINTENANCE8
tests: N-27
docs: fix-inventory.md#from-upstream-medievil-133-138-bc160201983-and-contributors-merged-into-lab
related: upstream-barrier-batch-limit, upstream-volume-views, V3, lean-barriers, crash-dump
verified: 2026-11-07
---
## What it does

`GOW3_VULKAN_COMPAT=1` (launcher Advanced "Safe GPU mode (older AMD GPUs)", setting `vulkan_compat`) makes
`Instance::CreateDevice` run as on older drivers (RX 400/500, Vega; issues #18, #22). It turns off
`VK_KHR_maintenance5`, `VK_KHR_maintenance8`, `VK_EXT_attachment_feedback_loop_layout` and
`VK_EXT_image_2d_view_of_3d`. `GOW3_NO_MAINTENANCE5=1` / `GOW3_NO_MAINTENANCE8=1` turn off only one of them. Two
parts are always on. One is the log lines `GPU: <model>, driver <name>, Vulkan <ver>[, safe GPU mode]` and
`GPU: maintenance5 on|off, ...`. The other: `FlushBarriers` sets the thread-local `recording_barriers` around each
`pipelineBarrier2`, and the fault handlers (`src/probe.c`) call `gow3gpu_dump_recording_barriers`, which prints the
batch with each image named by `DescribeImageHandle` (destroyed images are marked).

## Invariants

- Every feature the switch removes must have a working fallback, because device creation simply skips the
  extension:
  - no maintenance5 also means no pipeline binary cache (`pipeline_binaries` requires `maintenance_5`);
  - no feedback-loop layouts means `IsAttachmentFeedbackLoopLayoutSupported()` is false (`vk_rasterizer.cpp`,
    `vk_scheduler.cpp` and `image.cpp` take the `eGeneral` path; the dynamic-state extension goes with it);
  - no 2D views of 3D means volume slices stay attachment-only views (V3's rule).
- The feature structs of switched-off extensions are unlinked from the device chain (`device_chain.unlink`).
  Leaving one linked is invalid Vulkan.
- `recording_barriers` points to a stack `DependencyInfo` only while `pipelineBarrier2` runs, and it is cleared
  right after. The dump reads it from a fault handler on the same thread, and at most 64 images are printed.

## Interactions

- upstream-barrier-batch-limit: the other half of the AMD crash work. The dump shows which batch crashed; the
  limit caps its size.
- V3 / upstream-volume-views: without `image_2d_view_of_3d` both depend on attachment-only 2D array views
  (`GOW3_NO_2D_VIEW_OF_3D=1` tests that alone).
- lean-barriers: sampling depth in place uses the depth read-only layouts, and `BindTextures` excludes depth
  images from the feedback-loop path (`!image.info.props.is_depth`), so safe mode should leave it unchanged
  (from the code, not tested).
- crash-dump: the minidump path runs beside this text dump.

## Evidence

- Upstream: issue reports from RX 400/500 / Vega users (#18, #22). Never run on our hardware with the switch set
  (RTX 4090 PC, Deck RADV). N-27 covers the merge with it off. Not verified in game.

## History and pitfalls

- It is a compatibility switch, not a speed item. With it on, the pipeline binary cache is lost (slower cold
  starts).
- The Deck (RADV) does not need it. If a Deck user sets it, VRAM or volume paths change; check the `GPU:` lines
  in their log first.
