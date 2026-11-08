---
id: lean-barriers
title: Fewer render pass breaks: skip read-after-read barriers, sample depth in place, re-bind stencil without a barrier
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 4b5d65c, 63cfac3, 48bede5, 575c160, 5ecf53b
files: gpu/shadps4/video_core/texture_cache/image.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_runtime.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_runtime.h, gpu/shadps4/video_core/texture_cache/image_view.cpp, gpu/shadps4/video_core/texture_cache/image_view.h, scripts/graphics_options.py, gpu/shim/gow3_settings.cpp
symbols: SkipReadBarriers, IsReadOnly, GetBarriers, SampleDepthInPlace, KeepStencilAttachment, view_aspect, depth_aspect, BeginRenderingFull, BindTextures, BindTexturesFromSetIn, PERF_SWITCHES
switches: GOW3_SKIP_READ_BARRIERS, GOW3_DEPTH_SAMPLE_IN_PLACE, perf_lean_barriers
tests: T-F1, T-F2, T-F3, T-F4, T-F5, T-F6, T-F7
docs: fix-inventory.md#1-gpu-sync-barriers-and-render-pass-breaks, plan-stage-b-split.md#t-f1-what-makes-the-fights-frames-slow-2026-10-25-logs-t-p960-gpu-profile-5-t-p960-slow-frames, plan-stage-b-split.md#t-f2-which-barriers-break-the-passes-log-t-f2-barrier-breaks-trace-33, study-deck-patch-audit.md#2-scores
depends_on:
related: narrow-image-stages, depth-write-effective, V4, precise-limiter
checked: 2026-11-07
---
## What it does

Three parts, each removing a barrier that broke a render pass:
- (a) `GOW3_SKIP_READ_BARRIERS`: in `Image::GetBarriers`, a read after a read in the same layout records no
  barrier; the access mask and stages merge into the tracked state. Read-only depth attachments are recorded as
  `eDepthStencilAttachmentRead`; they were writes, so (a) never applied (`63cfac3`).
- (b) `GOW3_DEPTH_SAMPLE_IN_PLACE`: a depth-aspect sample (`ImageView::view_aspect`, `TextureSetEntry::depth_aspect`)
  of an image in DepthReadOnlyStencilAttachment keeps that layout (`Runtime::SampleDepthInPlace`).
- (c) same switch: re-binding it as that attachment after only attachment use and in-place reads records no barrier
  (`Runtime::KeepStencilAttachment`).
Both are on by default since `5ecf53b`; `perf_lean_barriers=0` sets both to 0.

## Invariants

- (a) merges reads, it never drops them: `access_mask |= dst_mask` and `pl_stage |= dst_stage`. The next write or
  layout change then has every earlier reader in its source scope. Replacing the state instead would race those
  readers.
- (a) skips only when both masks pass `IsReadOnly` and `ReadCovered` holds. With narrow-image-stages a stored stage
  can be fragment-only, and a compute read must still get its own barrier. Removing `ReadCovered` from the skip
  would leave compute reads unordered after a graphics-scope barrier.
- (b) needs a whole-image state: no `subresource_states`, layout exactly `eDepthReadOnlyStencilAttachmentOptimal`.
  It adds `eShaderRead` with stage `eAllCommands`, so a later write waits for it.
- (c) accepts only DepthStencilAttachmentRead / Write and ShaderRead in the stored mask, a whole-image state and
  the layout already DepthReadOnlyStencilAttachment, and only for native targets (`!reduced` in
  `BeginRenderingFull`). Its premise: depth is never written in that layout and stencil attachment writes between
  draws are ordered by rasterization order. Any other access (transfer, storage) must take the `Transit` path.
- (b)'s two call sites are the slow path (`BindTextures`) and the memo's hit path (`BindTexturesFromSetIn` on
  deck-lab, `BindTexturesFromSet` on lab). The hit path decides from the stored `depth_aspect`; a new sampling path
  that transitions depth images must make the same check or it brings the layout switch back.
- Vulkan allows depth-aspect sampling while the image is a DepthReadOnlyStencil attachment (separate aspects).

## Interactions

- narrow-image-stages: `ReadCovered` (same `GetBarriers`) is the guard (a) depends on.
- depth-write-effective: fewer draws mark depth written, so more binds end in the read-only layouts (a)-(c) use.
- V4: its copy moves the image to TransferDst, so (c) correctly does not apply after it.
- `5ecf53b` also changed the precise limiter's slot wait to spin `GOW3_PACER_SPIN_US` (700 us); see precise-limiter.
- The in-game menu reads the patch state from `GOW3_SKIP_READ_BARRIERS` only (`gow3_settings.cpp` perf_off), so
  `GOW3_DEPTH_SAMPLE_IN_PLACE=0` alone still shows it as on.

## Evidence

- T-F1 (fight): kills' effects nearly doubled pass ends (326 -> 560-660 a frame), stage B 92-97% busy.
- T-F4 (a): DepthStencilReadOnly -> same barriers gone, reopens 20-48k -> 5.7-17k calm. T-F5 (a + b): the barrier
  moved (~43k a kill window), which (c) removed.
- T-F6 (all three): barrier breaks 89-131k / 157k -> 48-70k / 84k per 5 s (calm / kill); slow frames' stage B
  busy 92-97% -> 48%. The user: no artefacts.
- T-F7 (fight, 120 cap, no stats, last 45 s): 1% / 0.1% lows 119.7 / 117.4, no frame over 10 ms, against T-P960
  before the barrier changes (107.6 / 100.5, 34 frames of 9-12 ms; another run, not a same-launch A/B). The user
  (paraphrased in the doc): no issues, the fights' dips gone, GPU usage steady instead of 60 -> 100% spikes.
  Verified in game.

## History and pitfalls

- T-F3 changed almost nothing until read-only depth was recorded as reads (`63cfac3`).
- Patch audit: never A/B'd on RADV. It has been on in all Deck runs without reported artefacts, but it has no Deck
  measurement.
- What is left is depth written by one draw and sampled by the next (~50k a kill window), a real dependency.
