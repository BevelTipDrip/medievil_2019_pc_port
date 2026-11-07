---
id: gpu-write-twins
title: GPU write twins: guest writes into a page with unfinished GPU bytes keep a twin, no download (limit 16 KiB)
area: caches
status: active
platforms: all
default_windows: on
default_deck: on
origin: bloodborne 0.4
class: G
commits: 2a40666, 94b31c3
files: gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp, gpu/shadps4/video_core/buffer_cache/buffer_cache.h, gpu/shim/core/memory.h, src/runtime_memory.c
symbols: TwinsEnabled, TwinsMaxBytes, twins, twins_mutex, TwinRun, ForEachTwinRun, EraseTwins, ReadMemory, WriteBackDownload, SynchronizeMemory, ProcessCpuWritesLocked, ReportTwins, ReadBacking, runtime_memory_read_backing
switches: GOW3_GPU_WRITE_TWINS, GOW3_GPU_WRITE_TWINS_MAX, perf_gpu_data_no_waits
tests: T-D3b, T-T1, N-13
docs: fix-inventory.md#3-memory-sync-uploads-and-readbacks, plan-stage-b-split.md#t-t1-gpu-write-twins-2026-10-19-2310-2316-launched-by-claude-with-the-users-go-build-2a40666-log-datat-t1-twinslog, plan-frame-pacing.md#n-13-the-map-oscillations-waits-2026-10-28-launched-by-the-user--by-claude-at-the-users-request
depends_on: D3
related: fault-page-download, D2, D1
verified: 2026-11-07
---
## What it does

On a guest write fault (`BufferCache::ReadMemory`, `is_write`) into a 4 KiB page whose GPU-written bytes total at most
`TwinsMaxBytes` (16 KiB) and whose newest write tick (D3) is not finished, the guest's current copy of those bytes is
saved as a twin (`twins`, read through the backing view: `ReadBacking`) and the page becomes CPU-modified at once:
no download, no wait. Uploads (`SynchronizeMemory`) leave the GPU's bytes in the arena where the guest bytes still
match their twin and upload (and un-GPU-mark) the ones the guest changed; downloads (`WriteBackDownload`) do not
overwrite guest bytes that differ from their twin. D2's writes and unmaps erase twins.

## Invariants

- "Guest wrote it" is decided by byte comparison against the twin (`ForEachTwinRun`: Unchanged / Changed). A guest
  write that stores the same value as the twin is taken as unwritten and the GPU's byte wins; correct only while
  that byte is equal on both sides or the GPU's later value is what the guest wants [inferred].
- Existing twin parts keep their older snapshot when a page faults again (`TwinRun::None` only is snapshotted).
- Guest bytes are read through the backing view while `twins_mutex` is held: a normal read could fault and wait for a
  readback that needs the mutex (deadlock).
- Every path that ends the GPU's claim on bytes must erase their twins: `SynchronizeMemory` (changed runs),
  `WriteBackDownload`, `ProcessCpuWritesLocked` (D2), the unmap branch of `ReadMemory` (D1).
- Off when `GOW3_READBACKS` is Precise (`TwinsEnabled`), and only with a known, unfinished tick; tick 0 or a finished
  tick go to the download paths (fault-page-download).

## Interactions

- depends_on D3: `WriteTicks().Newest` is the "still in flight" test. The tick table is kept even with
  `GOW3_READBACK_QUEUE=0`, so twins work without the readback queue.
- fault-page-download (`94b31c3`) handles the cases twins do not: finished pages, pages with no GPU bytes; the same
  commit raised the limit from 1 KiB (Bloodborne's value) to 16 KiB.
- perf_gpu_data_no_waits=0 sets `GOW3_GPU_WRITE_TWINS=0` with D3, E1 and W2.

## Evidence

- T-D3b: the game's direct writes into pages a shader writes in the same frame (9-20/s, light view) each waited
  ~2.8 ms (max 3.3 ms) for that frame's submission. T-T1: light view (~1,150 draws) 213 FPS, stddev 0.06-0.14 ms, p99 4.8-5.2 ms (T-D3b ~0.56 ms, ~7 ms),
  write-fault downloads 0. The user: "Much better … It's not perfect, there's a few spikes here or there, especially
  panning past heavier areas." Verified in game.
- N-13 `t-map3` (16 KiB): the level-select map's ~35 faults/s twinned (33.8/s), 0.1-0.6 ms of waits a second.
- Deck (study-deck-patch-audit row 54): rarely fires (2 twin entries).

## History and pitfalls

- Port of the Bloodborne port 0.4 (`BB_GPU_WRITE_TWINS`, GPL-2.0-or-later); the 16 KiB limit is ours.
- The comment above `TwinsEnabled` still says "(1024)"; the default is 16384 (`TwinsMaxBytes`).
- `ReportTwins` (added in `31a6c1a`, the ring-reuse commit) prints the table size each frame-stats window; its code
  comment cites t-nostats6 (panning slower
  the longer a session ran; every fault and upload searches the map). N-14's bumps were ring reuse, not twins.
