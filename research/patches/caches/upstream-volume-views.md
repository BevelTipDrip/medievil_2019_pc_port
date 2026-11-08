---
id: upstream-volume-views
title: Upstream volume 2D-array views and exact ClearVolumeSlices; attachment-only 2D-of-3D without the extension
area: caches
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream 1.3.4
class: G
commits: 4d72031, d4c9cf4, b7b78fb
files: gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/image_view.cpp, gpu/shadps4/video_core/texture_cache/image.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_runtime.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_instance.h
symbols: IsVolumeSliceTarget, ResolveOverlap, Runtime::ClearVolumeSlices, Runtime::ClearImage, Is2dViewOf3dSupported, image_2d_view_of_3d, GetBarriers
switches: GOW3_VOLUME_VIEWS, GOW3_NO_2D_VIEW_OF_3D
tests: N-27
docs: fix-inventory.md#from-upstream-medievil-133-138-bc160201983-and-contributors-merged-into-lab, plan-upstream-merge.md#2-the-five-conflicts-and-how-to-resolve-them, plan-upstream-merge.md#1-where-upstream-and-lab-overlap, patches-and-pr-proposal.md#not-proposed
depends_on: V3
related: V3, upstream-safe-gpu-mode
checked: 2026-11-07
---
## What it does

Upstream's half of the volume fix, merged over V3 (`b7b78fb`). 1.3.4 serves a 2D render target whose layers are
the depth slices of a same-sized 3D image from the 3D image (no `ExpandImage` rebuild per switch) and clears
render-target slices of a volume exactly (`Runtime::ClearVolumeSlices`: an empty render pass with `loadOp` clear on
a 2D array view of the addressed slices, routed from `Runtime::ClearImage`). Barriers on a volume cover the whole
image (`Image::GetBarriers`). 1.3.5: without `VK_EXT_image_2d_view_of_3d` (`Is2dViewOf3dSupported`) any 2D view
of a 3D image is attachment-only (`image_view.cpp`; AMD Polaris crash, issue #18).

## Invariants

- The slice test is ours: `IsVolumeSliceTarget` (render target binding only, a `Color3D` volume, same width /
  height, layers == depth, 1 level on both, compatible format). Upstream's own test (any binding, equal pixel
  format, tile mode and guest size; its sampled view) was dropped in the merge (`4d72031` vs `b7b78fb`).
- `GOW3_VOLUME_VIEWS=0` gates only the `ResolveOverlap` site; toggle bit 58 (`VolumeSliceViews`, inside
  `IsVolumeSliceTarget`) gates it and the "picked up wrong" check in `FindImage`. Off means the old expand path.
- `image_view.cpp` applies both rules in order: a 2D array view of a volume keeps only
  `eColorAttachment | eAttachmentFeedbackLoopEXT` (V3); then, without the extension, every non-3D view of a 3D
  image keeps only `eColorAttachment`. The volume must always be sampled through its own 3D view.
- `ClearVolumeSlices` needs a 2D array attachment view of the volume, which both rules allow; it clamps the
  slice range to the mip's depth. `Runtime::ClearImage` routes every clear of a `Color3D` volume there, not only
  render-target clears. A new usage restriction that forbids attachments breaks volume clears.
- Volumes are always created `e2DArrayCompatible` (`image.cpp`); `e2DViewCompatibleEXT` only with the extension.
  Both rules rely on the first flag.
- `image_2d_view_of_3d` is off with `GOW3_NO_2D_VIEW_OF_3D=1` or safe GPU mode (`VulkanCompat()`), as on drivers
  without the extension.

## Interactions

- V3: the same fix found independently (V3 2026-10-18, upstream 1.3.4 2026-10-19). The merge kept V3's view rule
  and slice test and took upstream's exact slice clears and its env switch. This card depends on V3's detection.
- upstream-safe-gpu-mode: `GOW3_VULKAN_COMPAT=1` turns the extension off, so the attachment-only rule applies.

## Evidence

- Upstream 1.3.4 commit: "~4700 texture rebuilds per 5 s -> ~90". V3's own numbers (T-V3) are on V3's card.
- N-27 merge test: no regression against pre-merge `lab` (fix-inventory.md). Not verified in game on AMD or Intel.

## History and pitfalls

- 1.3.4 sampled through the 2D array view; on Polaris drivers without the extension that was undefined behaviour
  and crashed in `amdvlk64.dll` (#18). 1.3.5 restricted it; V3 never sampled through it.
- Open (patches-and-pr-proposal.md): V3's rule "should" be valid without the extension (spec reading), but the
  validation-layer run with V3 on is still pending, and whether `eAttachmentFeedbackLoopEXT` is a valid usage on
  such views was not checked.
