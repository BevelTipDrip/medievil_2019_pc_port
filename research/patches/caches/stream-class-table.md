---
id: stream-class-table
title: Buffer cache small-buffer state in a fixed 65,536-entry direct-mapped table (was an unordered_map cleared at 262,144)
area: caches
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: de30a399f6
files: gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp, gpu/shadps4/video_core/buffer_cache/buffer_cache.h
symbols: BufferCache::ObtainBuffer, StreamClass, stream_class, stream_class_mutex
tests: N-8
docs: fix-inventory.md#2-draw-translation-cost-stage-b-per-draw, plan-frame-pacing.md#n-8-panning-hitches-the-stream-class-table-and-logging-cost-2026-10-28-launched-by-the-user
depends_on: S2
related: async-log-writer, sections, ring-reuse, narrow-image-stages
verified: 2026-11-07
---
## What it does

S2's per-address state (0 new, 1 uploaded once, 2 dynamic) was an `std::unordered_map<VAddr, u8>`, cleared when it
passed 262,144 entries. UE4's ring buffers bring new addresses every frame, so the clear and the rehashes as it grew
ran on stage B about once a second while panning. It is now `std::array<StreamClass, 1u << 16>` (`{address,
state}`, ~1 MB), indexed by `((device_addr >> 4) * 0x9E3779B97F4A7C15 >> 48) % size`. An address that finds another
in its slot replaces it and starts again as new. No switch.

## Invariants

- The table is only a heuristic for where to upload. Data correctness never depends on it: what gets uploaded is
  decided by `IsRegionCpuModified` / `SynchronizeMemory`. An evicted address costs at most one more upload of a
  small buffer outside a render pass (state 0 is promoted only when `!scheduler.IsRendering()`). A dynamic range
  can also be promoted again once, then becomes dynamic on its next CPU write.
- The table must never clear, rehash or allocate on stage B. That was the 8-12 ms hitch.
- The hash takes 16 bits (`>> 48`). If the table grows past 65,536 entries the shift must change too, or the upper
  slots go unused. `% stream_class.size()` keeps a smaller table in range.
- Accessed under `stream_class_mutex`, and only on S2's CPU-modified branch, so clean binds never touch it.

## Interactions

- It exists only for S2 (`GOW3_STREAM_CLEAN` / `perf_small_buffers`). With S2 off the table is never read.
- The same commit (`de30a39`) split `ObtainBuffer` into `GOW3_SECTIONS` sub-sections (`ObtReadbacks`,
  `ObtRegions`, `ObtStream`, `ObtResident`, `ObtSync`: the sections card) and moved slow-frame and perf-diag
  output to a background writer (async-log-writer). Those are separate cards.
- N-8's A/B without diagnostics measured "tonight's changes" together: this table, ring reuse, narrow image
  stages and async logging.

## Evidence

- N-8 `t-pan1`: gameplay hitches 13-15.5 ms, with `ObtainBuffer` at 10-12 ms in them (1.9 ms in ordinary frames).
  `t-pan2`: `obt_regions` 8-12 ms in the hitches [V]. Blaming the map's clear and rehash is inferred [I].
- `t-pan3`: `ObtainBuffer` ~0.5 ms in every slow frame [V]. The user: "a big improvement" (fix-inventory).
- A/B of the night's changes together, no diagnostics, same panning: FPS 197.2 -> 207.8, worst frame median 9.3 ->
  7.2 ms, windows with a frame > 12 ms 4 of 11 -> 1 of 16, p99 6.65 -> 6.15 ms.
- Deck: listed in the patch audit (on / on), no Deck measurement.

## History and pitfalls

- The first stall guess (cpu-osc-1, 2026-10-25) put stage B's first sample after a driver lock wait at the
  `stream_class` insert. The doc later corrected that: the lock wait came from `EnsureResident` (see
  driver-memory-off-stage-b).
- The large stutters in N-8's logged runs were mostly the logging itself. Judge this kind of change only from runs
  without `GOW3_SLOW_FRAMES` / `GOW3_SECTIONS` / `GOW3_PERF_DIAG`.
