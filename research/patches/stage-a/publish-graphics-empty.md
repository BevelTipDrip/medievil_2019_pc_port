---
id: publish-graphics-empty
title: PublishGraphics returns at once when no pipeline compilation is pending
area: stage-a
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: b17f99b
files: gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp
symbols: PublishGraphics, pending_graphics, GetGraphicsPipeline
switches:
tests:
docs: impl-stage-a-decode.md#1-what-stage-a-does-per-draw-now, impl-stage-a-decode.md#3-the-cuts
depends_on:
related: A2, A3
verified: 2026-11-07
---
## What it does

`PipelineCache::GetGraphicsPipeline` calls `PublishGraphics()` for every draw on stage A. It publishes finished
asynchronous pipeline compilations from `pending_graphics` (a `tsl::robin_map`). `robin_map::begin()` walks the
buckets from 0 to the first full one, and the map keeps the buckets it grew to while compilations were queued (up to
64 queued: 128-256 buckets, each holding a key of a few hundred bytes). So in steady play every draw walked all of
them to find nothing. An `empty()` check now returns before the loop. With entries, the loop is unchanged.

## Invariants

- Same behaviour by construction: the loop did nothing for an empty map, with or without `wait`. That holds only
  while the loop is the whole function. Code added after the loop (stats, a step that must run every call) would
  be skipped by the early return, so put it before the check.
- `pending_graphics` is the only place finished builds wait to be published. `GetGraphicsPipeline` adds to it
  (`pending_graphics.emplace`) and `PublishGraphics` erases from it. The callers are `GetGraphicsPipeline`,
  `GetComputePipeline`, `ReplaceShader` (with `wait`) and `FinishCompilations` (with `wait`). The check reads the
  map without a lock, as the loop did, so it is only as thread-safe as those callers already are.

## Interactions

- Built and committed with A1-A3 (impl-stage-a-decode.md), but it has no switch and is on everywhere. That includes
  the Windows `deck-lab` build. It is not on `lab`.
- No interaction with the async compile path's results: the same builds are published at the same draw.

## Evidence

- None of its own. The impl doc's estimate is "unknown, below the profile's cut-off (< ~0.1 ms)". It was part of
  every A1-A3 run (round 1 launch 7, round 2, Launch 15) in both toggle states, since it has no bit.
- Read in the code: `robin_hash.h` `begin()` (impl doc section 1). Not verified in game on its own (no visible effect
  expected).

## History and pitfalls

- Found in the Deck `perf record` (v3 build) while profiling stage A's per-draw cost (`Draw` -> `GetGraphicsPipeline`:
  `SeedWorkerCaches`, `PublishShaders`, `PublishGraphics`, `PumpWarmUp`).
- A `robin_map` that shrinks on erase would make this moot. The map does not shrink, which is why it helps.
