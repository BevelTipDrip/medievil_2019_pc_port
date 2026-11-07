---
id: R1-readback
title: A readback copy recorded at an end of pipe is submitted once the readback locks are released
area: caches
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 088d8c2b72, 9dea0af12d
files: gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h
symbols: Rasterizer::ProcessDownloadImages, TakeReadbackSubmitWanted, readback_submit_wanted, stale_readbacks
switches: GOW3_READBACK_SUBMIT
tests: T-HZ2b, T-HZ3
docs: fix-inventory.md#3-memory-sync-uploads-and-readbacks, plan-stage-b-split.md#t-hz2b-hzb-with-precise-readbacks-2026-10-20-0028-0033-launched-by-claude-with-the-users-go-build-a968802-log-datat-hz2b-hzb-preciselog, plan-stage-b-split.md#t-hz3-hzb--precise-readbacks--r1-2026-10-20-0037-0041-launched-by-claude-with-the-users-go-build-9dea0af-log-datat-hz3-readback-submitlog
depends_on: lineage-deferred-readbacks
related: hzb-remap, R1-stale-eop, C5
verified: 2026-11-07
---
## What it does

`TextureCache::ProcessDownloadImages` sets `readback_submit_wanted = !stale_readbacks` when it queues a readback copy
(`pending_readbacks.Push`). After the texture cache returns (all readback locks released),
`Rasterizer::ProcessDownloadImages` calls `TakeReadbackSubmitWanted()` and, if it was set, `scheduler.Flush()`. The
copy is then submitted right after the work it reads instead of at the frame's end. `9dea0af` only made
`TakeReadbackSubmitWanted` public (build fix).

## Invariants

- It acts only with precise readbacks (`stale_readback=0`). In stale mode the flag is never set, so the code is
  inert. `stale_readback=1` is the default in `graphics_options.py` `DEFAULTS` and in `DECK_PRESET`, but the
  launcher's own `INI_DEFAULTS` has `stale_readback=0` (since the import `ac900f8`): a launcher save of an ini
  without the key writes 0, so a fresh Windows launcher install runs precise readbacks and this card acts there.
- The flush must happen outside `readback_mutex` and `download_images_mutex`. It runs after
  `texture_cache.ProcessDownloadImages()` has returned, and a submission can wait for host copies whose fault
  paths take `readback_mutex`.
- `TakeReadbackSubmitWanted` reads and clears the flag only in `Rasterizer::ProcessDownloadImages` (end of pipe,
  end of shader and release-mem events, after `DrainDrawPipe`). The frame-end `OnSubmitWork` also calls
  `texture_cache.ProcessDownloadImages()` and can set the flag without taking it; the frame-end `Flush` covers that
  copy, and the stale flag only costs one extra flush at the next event. It is a plain bool, ordered by the
  draw-pipe drain between the threads; another caller would need its own ordering.
- `GOW3_READBACK_SUBMIT=0` (read once, static) keeps the old behaviour: the copy waits for the frame-end submit.

## Interactions

- It was built for UE4's HZB results with precise readbacks (T-HZ2b). The kept HZB route is hzb-remap, which
  runs with stale readbacks and serves the newest finished copy, so this card does nothing with stale readbacks on (the user's ini, the Deck preset).
  A switch-on/off A/B in default play would measure nothing.
- Each flush adds a submission. On the Deck an extra submission per end of pipe would cost more. That only
  matters if someone turns stale readbacks off.
- Not R1-stale-eop (the reverted wait skip) and not R1-region-index. The letter was reused.

## Evidence

- T-HZ2b (precise readbacks, HZB): culling was correct (the user: "that fixes the culling glitches"), but there was
  ~1 demand wait a frame for the results, 1.47 s per 5 s, max 3.1 ms (~2.6 ms a frame); heavy view 110-116 FPS.
- T-HZ3 (with this card, build `9dea0af`): 623-627 waits per 5 s, 1.04 s per 5 s (~1.7 ms a frame, max 2.25 ms);
  ~125 FPS, p99 8.5 ms with the camera still. The user: "the heavy area is about 125 with 118 1% lows when the cam is
  static, but moving the camera has a heavy penalty."
- fix-inventory: "no gain measured in normal play". Never measured on the Deck.

## History and pitfalls

- Why it could not remove the wait: the game reads frame N's HZB results at the start of N+1, while the GPU is still
  about a frame behind on N. However early the copy is submitted, it has not run yet. The fix was hzb-remap, which
  double-buffers the results outside UE4 and serves the newest finished copy.
- Kept because it is harmless and helps any precise-readback consumer. To remove it, check first that nothing runs
  with `stale_readback=0` (`GOW3_STALE_READBACK=0` tests do).
