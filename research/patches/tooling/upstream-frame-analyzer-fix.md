---
id: upstream-frame-analyzer-fix
title: Upstream frame analyzer crash on flatbuf ring bindings (address 0 passed IsValidGpuMapping)
area: tooling
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream PR #28 (Porticus)
class: G
commits: 3a240c1, 8a7abb9
files: gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_frame_capture.cpp
symbols: BindBuffers, CollectRingBindings, FlatUserData, FrameCapture::Buffer, IsValidGpuMapping, OnConstants
switches: GOW3_CAPTURE_TRIGGER, GOW3_CAPTURE_DIR
tests:
docs: fix-inventory.md#from-upstream-medievil-133-138-bc160201983-and-contributors-merged-into-lab
depends_on:
related: renderdoc-capture, mausoleum-diagnostics, lineage-constant-ring, upstream-screenshots
verified: 2026-11-07
---
## What it does

`CollectRingBindings` copies a stage's flattened user data (the flatbuf) into the constant ring with guest address
0. `IsValidGpuMapping(0, 0)` only bounds the range, so with a frame capture active `BindBuffers` passed a null
pointer to `FrameCapture::Buffer` (SIGSEGV). The fix: a ring binding of size 0 is skipped, an address-0 binding is
recorded from `stage.FlatUserData()`, a real address must pass `IsValidGpuMapping(address, size)`, `Buffer` guards
against null data, and flatbuf dwords print in hex (descriptors and bitfields, not floats). The same edit guards
the camera-motion constants read (`ring->size == 864 && gbuffer_draw`) against address 0.

## Invariants

- Every consumer of a ring binding must treat `address == 0` as "flatbuf, data in `FlatUserData()`", never as a
  guest pointer. Today: the capture branch and the `camera_motion->OnConstants` check in `BindBuffers`. A new
  reader of `ring->address` that skips the check faults when a stage binds its flatbuf.
- `FrameCapture::Buffer` only reads `data` when non-null; `hex` keys on `address == 0`. The byte limit
  (`min(size, GOW3_CAPTURE_BUFFER_BYTES)`, rounded down to 4) is ours, from mausoleum-diagnostics (`e383f7e`).
- Only active while the frame map is Armed or Recording (`FrameCapture::Active()`), except the camera-motion
  guard, which is on the normal draw path (G-buffer draws with an 864-byte ring binding).

## Interactions

- renderdoc-capture, mausoleum-diagnostics: build on this frame map (target-started captures, the larger buffer
  dump of the ~3.3 KB view constants).
- lineage-constant-ring: the ring whose flatbuf bindings carry address 0.
- upstream-screenshots: the other key-driven capture (F12; the frame map is F11 with `GOW3_FRAME_CAPTURE_KEY`, or
  the `GOW3_CAPTURE_TRIGGER` file the launcher sets, polled 4 times a second).

## Evidence

- Upstream (Porticus, `3a240c1`): "Verified on Linux with GOW3_CAPTURE_TRIGGER: 43 passes written, the flatbuf
  slots show their descriptors, and the game keeps running." Merged as PR #28 (`8a7abb9`); in `lab` with the upstream
  1.3.8 merge (`e308c4c`, 2026-10-30; fix-inventory; merge test N-27: no regression). No port-side measurement.

## History and pitfalls

- Before the fix any frame-map capture that reached a draw with a flatbuf binding crashed the game; the frame-map
  captures of the Mausoleum hunt (2026-11-06/07) need it.
