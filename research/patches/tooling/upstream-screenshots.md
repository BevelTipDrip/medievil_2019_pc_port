---
id: upstream-screenshots
title: Upstream F12 PNG screenshots (Shift+F12 raw dump)
area: tooling
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream 1.3.3
class: G
commits: 8a70032
files: gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shim/gow3_overlay.cpp, launcher/medievil_launcher.py
symbols: SaveScreenshot, DumpRaw, screenshot_requested, TakeScreenshotRequest, screenshots_dir
switches: GOW3_SCREENSHOT_DIR, GOW3_FRAME_DUMP_TRIGGER, GOW3_FRAME_DUMP_COUNT
tests:
docs: fix-inventory.md#from-upstream-medievil-133-138-bc160201983-and-contributors-merged-into-lab
depends_on:
related: renderdoc-capture, upstream-frame-analyzer-fix, upstream-launcher-features
verified: 2026-11-07
---
## What it does

F12 (overlay key handler, no Shift) sets `screenshot_requested`; the presenter's next frame
(`TakeScreenshotRequest`) copies the game's picture into a host buffer (with the temporal upscaler on, its output
`display.image` at the output size; otherwise the guest display buffer at its own size; no overlay) and
`SaveScreenshot` converts it to RGB in a deferred priority operation, then encodes the PNG (miniz) on a detached
thread into `GOW3_SCREENSHOT_DIR` (default `screenshots`; the launcher sets its screenshots folder) as
`MediEvil_YYYYMMDD_HHMMSS[_n].png`. Shift+F12 creates the `GOW3_FRAME_DUMP_TRIGGER` file instead: `DumpRaw` writes
`GOW3_FRAME_DUMP_COUNT` raw frames (default 1, clamped 1-64; the launcher: 8, one folder per session) for bug
reports. The raw dump and its trigger file are older (inherited); 1.3.3 moved them from F12 to Shift+F12.

## Invariants

- The copy runs inside the present command buffer: the image must be moved to `eTransferSrcOptimal` and back to
  the layout the present pass expects (the `to_copy` / `back` barriers, or `runtime.Transit` on the texture-cache
  path). A missing transition is a validation error or a corrupt present.
- Only BGRA8 / RGBA8 (UNORM / SRGB) and A2R10G10B10 / A2B10G10R10 are converted; other formats print "picture
  format ... is not supported" and save nothing (10-bit is cut to 8 bits per channel).
- The host buffer is freed after the GPU is done (deferred priority operation), never on the frame path; the
  4K PNG encode stays off the present thread (the reason for the detached thread). The per-pixel RGB conversion
  (w x h pixels) does run inside the deferred operation, on the thread that runs the draw scheduler's priority
  operations [code].

## Interactions

- renderdoc-capture: F12 is also RenderDoc's capture key; under `GOW3_RENDERDOC` one press probably does both
  (inferred).
- upstream-frame-analyzer-fix: the frame map (F11, `GOW3_FRAME_CAPTURE_KEY`) is the other key-driven capture.
- upstream-launcher-features: the launcher sets the screenshot and frame-dump folders (`setdefault`, so an
  environment variable already set wins).

## Evidence

- Upstream 1.3.3 (`8a70032`), merged on `merge-1.3.5` (`b7b78fb`), in `lab` through `e6bf93d` (2026-10-30); the
  merge test N-27 found no regression. No port-side measurement; a screenshot costs one copy on the frame it is taken.

## History and pitfalls

- Upstream `4ec0013` made the 1.3.6 screenshot and scheduler diagnostics build on Linux.
- The two branches give different sizes: a shot at the render size means the upscaler path was not taken, not
  that the screenshot is broken.
