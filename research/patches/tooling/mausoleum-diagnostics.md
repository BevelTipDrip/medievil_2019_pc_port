---
id: mausoleum-diagnostics
title: Image watch and readback logs: every event that can set or lose a watched image's contents, skipped targets, first copies
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: e383f7ec49, be8ee73696, 892a801ef4, 3d4afb280f
files: gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h, gpu/shadps4/video_core/renderer_vulkan/vk_frame_capture.cpp
symbols: ImageWatchSpec, ImageWatch, TextureCache::Watched, TextureCache::WatchNote, SkippedTargetLog, LogSkippedTarget, HalfToFloat, NonZeroTexels, readback_log, readback_published, RecordImageDownload, RetireReadbacks, FindRenderTarget, FindTexture, RefreshImage, FrameCapture::Buffer
switches: GOW3_IMAGE_WATCH, GOW3_IMAGE_WATCH_LINES, GOW3_READBACK_LOG, GOW3_SKIPPED_TARGET_LOG, GOW3_CAPTURE_BUFFER_BYTES
tests:
docs: plan-linux-deck.md#hilltop-mausoleum-lighting-pool-pages-leaked-on-linux-2026-11-0607-fixed-7246f88--f74e1a1
depends_on:
related: pool-kept-pages, readback-first-wait, tiny-target-readback, gpu-write-subresources, renderdoc-capture, mausoleum-toggle-bisect, texcache-stats, lineage-deferred-readbacks, new-images-undefined
---
## What it does

Tracing for the Hilltop Mausoleum lighting (Deck). `GOW3_IMAGE_WATCH=WxH[:min_layers]` gives images of that size a
timed "Image watch" line (flags, scheduler tick) for every event that can set or lose their contents: created (for
which binding), CPU / maybe-CPU / GPU dirty, uploaded from guest memory (caller file:line, "was undefined"), render
target and storage binds (mips, layers), expanded, mip merged in, unmapped, freed; `GOW3_IMAGE_WATCH_LINES` caps
the lines (default 4000). `GOW3_SKIPPED_TARGET_LOG=1` logs once each GPU-written target or storage image of at most
64 KiB that is never copied back (tiled, wider than 8 px) with the guest bytes a CPU read would see, as halves.
`GOW3_READBACK_LOG=1` logs first copies (queued, published with their non-zero texels, the GPU's linear texels, a CPU
read). The frame map now ends at a display pass after a scene pass (not at the UI pass) and prints
`GOW3_CAPTURE_BUFFER_BYTES` of each buffer (default 1024; the view constants are ~3.3 KB).

## Invariants

- Logging only: no path changes what is uploaded, copied or freed. Off, each site costs a `Watched` call (a static
  spec and a width compare) or a static bool test.
- `WatchNote` takes a line from the budget by CAS, so the cap holds across threads; a new event site must call
  `Watched` first (the format work is not free) and add its own note, or the watch misses that way of losing data.
- The "read by the CPU" readback line prints only when the read faults, i.e. with `GOW3_READBACK_FIRST_WAIT=1`
  (readback-first-wait); "first copy published" only in stale mode (`stale_readbacks`). `readback_published` grows
  by one entry per distinct readback address and is never pruned.
- The linear GPU copy for the log is an extra staging download per first copy, freed in a deferred priority
  operation; it adds GPU and staging work while `GOW3_READBACK_LOG` is on.
- The frame map's end rule (`saw_scene` in `BeginPass`) assumes MediEvil's two display-only passes per frame
  (composite, then UI); a game with no depth pass before its display pass would never end a recording that way.

## Interactions

- pool-kept-pages: the root cause these logs led to: watched images "uploaded from guest memory" that nothing had
  written (Linux pool pages from another allocation).
- readback-first-wait, tiny-target-readback, gpu-write-subresources: suspects built in the same commits or hunt,
  each rejected with its own card; the image watch's "GPU write inside" note belongs to gpu-write-subresources.
- renderdoc-capture: target-started captures and texel statistics; mausoleum-toggle-bisect: the method before.
- texcache-stats: one run with `GOW3_TEXCACHE_STATS=1` "repaired itself" (memory reuse luck, not reproducible).

## Evidence

- Mausoleum section: the reflection capture arrays (128x128 L8 x 128/512/1024) were "uploaded once from guest
  memory 'was undefined'"; the view constants (words 624-651) held garbage on the Deck, exact zeros on the PC.
  The fix (pool-kept-pages) was verified by the user: "everything loaded in correctly". These logs carry no
  performance numbers; not verified in game as anything but tracing.

## History and pitfalls

- Before these tools about 15 Deck launches went to toggle bisects (pool-kept-pages card); tracing what set each
  image's contents ended it (memory rule "trace data, not toggles").
- The watch keys on size only: common sizes (e.g. 128x128) match many images; use `:min_layers` to narrow it.
