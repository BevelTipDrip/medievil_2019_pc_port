---
id: depth-write-effective
title: A depth target counts as written only with the depth test on or a pending clear (N-6)
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: d6a1285
files: gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp
symbols: BeginRenderingFull, effective_writes, depth_written, is_depth_clear
switches: GOW3_DEPTH_WRITE_EFFECTIVE
tests: N-5, N-6
docs: fix-inventory.md#1-gpu-sync-barriers-and-render-pass-breaks, plan-frame-pacing.md#n-6-depth-writes-only-with-the-depth-test-on-2026-10-28
depends_on:
related: lean-barriers, narrow-image-stages, V4
verified: 2026-11-07
---
## What it does

In `BeginRenderingFull` the depth target's "written" flag decides its layout. It used to be
`desc.view_info.is_storage` (DB_DEPTH_CONTROL `depth_write_enable`) alone. It is now
`is_storage && (depth_enable || is_depth_clear)`, so a draw with the write bit but the depth test off no longer
switches the image to a writable layout. The stencil-only layout follows: `stencil_write` is now `!depth_written`
where it was `!is_storage`. `GOW3_DEPTH_WRITE_EFFECTIVE=0` restores the write bit alone.

## Invariants

- The pipeline has to agree: `vk_depth_stencil_state.h` sets the depth write to `depth_enabled &&
  control.depth_write_enable`. If a pipeline wrote depth with the test off, the read-only layout chosen here would be
  wrong: a Vulkan error, and depth writes possibly lost. Keep the two conditions identical.
- `is_depth_clear` (the register clear, which itself requires test and write on, or `IsMetaCleared` from HTILE)
  still makes the target writable. Only the HTILE case can arrive with the test off, and it must still get a
  writable layout: the clear is applied through the attachment (`depth_clear`).
- A draw with the write bit, the test off and the stencil test on now gets DepthReadOnlyStencilAttachment, so the
  stencil can be written there. The same draw used to get DepthStencilAttachment (from the diff; not looked at in
  N-6). With the write bit and both tests off it now gets the read-only depth layout, whose access lean-barriers
  part (a) records as a read.
- On the PS4, Z_ENABLE gates Z_WRITE_ENABLE, so a draw with the test off writes no depth there either.

## Interactions

- lean-barriers: more binds land in the read-only depth layouts whose reads (a) merges and whose stencil re-binds
  (c) keeps without a barrier.
- V4 runs in the same block, before the layout choice.
- Same commit as narrow-image-stages and the first ring reuse (`d6a1285`).

## Evidence

- N-6 (`t-dw1-depth-effective.log`, `GOW3_PERF_DIAG=1`): no visual problems. The depth ping-pong is unchanged
  (29.7 + 29.2 a frame): those draws really test and write depth. Kept as correct and free.
- The barrier keys named the ping-pong image: 512x512 D16Unorm, a shadow map reused for each shadowed light, not the
  4K scene depth.

## History and pitfalls

- N-5 assumed that the ~60 depth transitions a frame came from draws with the test off. N-6 showed they did not.
  The idea that followed, renaming the shadow map's backing, was dropped on analysis: N-6's premise was wrong, since
  there are 26-34 separate maps (frame-cost-dropped-ideas).
- `ImageViewInfo::is_storage` means "depth writable" here, not a storage image. Keep that in mind when reading the
  condition.
