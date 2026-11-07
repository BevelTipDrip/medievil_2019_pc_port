---
id: narrow-image-stages
title: Image transitions wait for the reading shader stages instead of eAllCommands (N-4)
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: d6a1285
files: gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/texture_cache/image.cpp, gpu/shim/gow3_toggles.h
symbols: ImageReadStages, ReadCovered, GetBarriers, BindTextures, BindTexturesFromSet, BindTexturesFromSetIn, NarrowImageStages
switches: GOW3_NARROW_IMAGE_STAGES, toggle:NarrowImageStages
tests: N-3, N-4, N-5, N-16
docs: fix-inventory.md#1-gpu-sync-barriers-and-render-pass-breaks, plan-frame-pacing.md#n-4-change-1-image-transitions-to-the-reading-shader-stages-2026-10-2728-launched-by-the-user, plan-frame-pacing.md#n-5-same-spot-ab-of-change-1-and-the-pcie-split-2026-10-28-launched-by-the-user-research-datangfxngfx-ab-1, plan-frame-pacing.md#n-16-research-where-heavy-frames-lose-time-constant-ring-stamp-fix-2026-10-29, study-frame-cost-2026-10-29.md#gpu-work-goal-1, plan-linux-deck.md#baseline-1-and-live-abs-2026-11-04-appimage-a863482-67-helpers-auto-no-dedicated-core-auto-clocks-mailbox-log-datadeck-baseline-1-ablog
depends_on:
related: lean-barriers, depth-write-effective, M1, ring-reuse
verified: 2026-11-07
---
## What it does

The transitions `BindTextures` and the memo's hit path (`BindTexturesFromSet`; `BindTexturesFromSetIn` on deck-lab)
make for sampled or storage images now wait for the
reading shader stages (`ImageReadStages`: compute, or pre-rasterization + fragment) instead of `eAllCommands`. That
stage is stored as the image's last use, so the next transition (for example back to a render target) waits only
for those shaders. In `Image::GetBarriers`, `ReadCovered` gives a read by a shader stage outside the stored stages
its own read-after-read barrier, chained onto the first. `GOW3_NARROW_IMAGE_STAGES=0` restores `eAllCommands`, and
toggle bit 31 does the same live.

## Invariants

- The stored `pl_stage` must cover every stage that read the image since the last barrier. `ReadCovered` returns
  false when a new reader's shader stage is not in the stored shader-only mask; that is how a compute read after a
  fragment read (or the reverse) gets a barrier. A new image read site that passes a narrower stage than the shader
  really runs in would race the next write.
- `ImageReadStages` maps every non-compute stage to `ePreRasterizationShaders | eFragmentShader`.
  `ReadCovered` expands `ePreRasterizationShaders` to the vertex / tessellation / geometry bits before comparing. A
  stage outside both groups (task / mesh, ray tracing) would need its own case.
- Stored stages that are not only shader stages (depth tests, attachments, all commands) skip as before. That is
  how lean-barriers' merged reads keep working.
- Storage images (`eGeneral`, read + write) also take the narrow stage. Any access after a storage use still gets a
  barrier, because the stored mask holds a write (`is_write`).
- Not narrowed: render targets bound for a feedback loop keep `eAllGraphics`. `ReadCovered` itself is not behind
  the switch; with the switch off every stored stage is `eAllCommands`, which it always treats as covered.
- A read-after-read barrier that `ReadCovered` forces keeps the earlier readers in the stored stage (`keep_stages`
  in `GetBarriers`), so the next write waits for both shader kinds. Storing only the new stage would race the
  first reader.

## Interactions

- lean-barriers part (a) skips read-after-read barriers only when `ReadCovered` holds, so it depends on this
  card's check being exact.
- M1 (memory, Windows) shares toggle bit 31: writing 2147483648 switches off both narrow stages and the kept pool
  views (`POOL_KEEP_VIEWS` in `runtime_memory.c`, read live since `40a416a`, 2026-10-18, `_WIN32` only). N-5's
  toggle-file A/B on Windows therefore also flipped M1 (from the code; the docs do not say it). On the Deck the
  bit flips narrow stages only.
- `d6a1285` also carries depth-write-effective, the first constant-ring reuse (ring-reuse), Nsight support
  (profiler-support) and perf-diag barrier counts.

## Evidence

- N-4 (`ngfx-narrow-1`, fight): 163 barriers a frame (174 before), now narrow; 0 read-after-read barriers added. The
  user: no visual problems in a fight, but a "persistent oscillation on the frame-time graph that was not there
  before" (open).
- N-5 (same spot, 5 frames a side): GPU frame 5.71 -> 5.22 ms; drains 208-214 vs 204-211, unchanged (the driver
  drains at layout changes and pass breaks).
- N-16 / study-frame-cost correction: GPU active time 4.876 -> 4.836 ms (-0.8%). The ~9% was idle variance.
- Deck baseline 1 (live `2147485696 0`, bit 31 + coarse read tracking off): 74.4 vs 74.2 FPS, neutral. No visual
  issues reported on RADV.

## History and pitfalls

- Kept for the correctness of the stored stages and because it costs nothing, not for speed. The barrier count, not
  the masks, is what costs (fix-inventory section 1).
- The N-4 oscillation the user saw was never explained. It was not read-after-read barriers (0 added).
