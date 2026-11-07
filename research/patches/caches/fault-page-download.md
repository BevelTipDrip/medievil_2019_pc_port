---
id: fault-page-download
title: Write faults without unrelated waits: a finished page downloads alone, a page with no GPU bytes needs none (N-13)
area: caches
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 94b31c3
files: gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp, gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shim/gow3_toggles.h
symbols: ReadMemory, page_download, DownloadMemory, page_downloads, fault_no_gpu_bytes, fault_unknown_tick, fault_too_big_for_twin
switches: GOW3_FAULT_PAGE_DOWNLOAD
tests: N-11, N-12, N-13
docs: fix-inventory.md#3-memory-sync-uploads-and-readbacks, plan-frame-pacing.md#n-12-why-the-maps-bone-page-counts-as-gpu-written-2026-10-28-launched-by-the-user-t-map1-gpu-writerlog-gow3_perf_diag1, plan-frame-pacing.md#n-13-the-map-oscillations-waits-2026-10-28-launched-by-the-user--by-claude-at-the-users-request
depends_on: D3
related: gpu-write-twins, D2, perf-diag
verified: 2026-11-07
---
## What it does

In `BufferCache::ReadMemory`, after the twin path, a write fault (`GOW3_FAULT_PAGE_DOWNLOAD`, on) looks at the
faulting 4 KiB page only: if it holds GPU-written bytes whose newest write tick (D3) is finished, it downloads that
page alone (`DownloadMemory` on the page: no wait). If the page holds no GPU-written bytes at all (protected for a
neighbour's), it is unmarked GPU-modified and marked CPU-modified with no download. Only the remaining cases (no tick
known, or more unfinished GPU bytes than a twin takes) fall to the old 512 KiB window, which waits for the newest
GPU write anywhere in it. The frame stats count each case.

## Invariants

- The page test sums `gpu_modified_ranges` over the page; "no GPU bytes" must mean exactly that (no range in the
  page). Unmarking a page that still holds GPU-only bytes would lose them on the next upload.
- A finished-page download must only cover the page (`page_begin` .. `page_end`): taking a wider range brings back
  waits for unrelated writes.
- The twin path runs first; this path only sees pages with a finished tick, no GPU bytes, an unknown tick, or more
  than `TwinsMaxBytes` unfinished.
- With `GOW3_FAULT_PAGE_DOWNLOAD=0` every write fault not twinned takes the window, as before.
- "No wait" needs D3's readback queue: without it (`GOW3_READBACK_QUEUE=0`, `perf_gpu_data_no_waits=0`, or no
  compute-only family) `DownloadMemory` copies on the graphics queue and calls `Finish()`, a full GPU wait, even
  for a finished page.
- With twins off (`GOW3_GPU_WRITE_TWINS=0`, or `GOW3_READBACKS` Precise) a small unfinished page is counted
  "too big for a twin" and takes the window.

## Interactions

- depends_on D3: a page counts as finished by `scheduler.IsFree(WriteTicks().Newest(...))`, and its download is
  wait-free only on D3's readback queue.
- gpu-write-twins: `94b31c3` also raised `GOW3_GPU_WRITE_TWINS_MAX` 1 KiB -> 16 KiB; in t-map3 that, not the page
  download, took the map's faults.
- perf-diag's write-fault report (who made the page GPU-written) found the case (N-12).

## Evidence

- N-11 / N-12: the level-select map oscillated 100 <-> 200 FPS; write-fault downloads 14-31/s, 127-195 ms of GPU waits
  a second, from `guest+0x20da4d0` (UE4 GPU-skinning bone matrices [inferred]) writing a page that holds a 20-byte
  block a shader writes (range 0x2015060c00 +0x14, an indirect-draw argument block [inferred]).
- N-13 `t-map2`: the oscillation came back with 0 finished-page downloads (not the case). `t-map3` (all of `94b31c3`):
  no oscillation, ~35 faults/s twinned (33.8/s), the rest finished-page downloads, 0 in the other cases, 0.1-0.6 ms
  of waits a second (127-158 ms when it oscillated). Confirmed by counters, not a user A/B; the oscillation is
  intermittent, so one flat run does not prove it [I in the write-up].
- Deck (study-deck-patch-audit row 55): 0 such downloads.

## History and pitfalls

- First the finished-page download alone (`t-map2`): no hit. The no-GPU-bytes case and the larger twin limit were
  added after; the oscillating runs' page most likely held more than 1 KiB of unfinished GPU bytes [inferred].
- N-14 `t-nostats4` turned this off among five changes and still saw bumps: not the cause there (ring reuse).
