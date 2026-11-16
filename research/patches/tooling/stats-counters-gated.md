---
id: stats-counters-gated
title: Stats counters, packet timing and their switches cost nothing without GOW3_FRAME_STATS=1 (no locked adds per draw or per guest memcpy / lock; switches on cache lines of their own)
area: tooling
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits:
files: gpu/shim/gow3_toggles.h, gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h, gpu/shadps4/video_core/amdgpu/liverpool.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp, gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp, gpu/shadps4/video_core/buffer_cache/region_manager.h, gpu/shadps4/video_core/page_manager.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_constant_ring.h, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h, gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_occlusion.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shim/gow3gpu.cpp, src/runtime.c, src/runtime_mutex.c, src/runtime_memory.c, tests/test_mutex.c
symbols: Gow3Stats::Add, Gow3Stats::Sub, Gow3Stats::Switch, Gow3Stats::enabled, Gow3Stats::WaitTimer, DrawPipe::Run, count_memory_call, COUNT, runtime_fault_stats_enabled, runtime_mutex_report
switches: GOW3_FRAME_STATS
tests: mutex-test
docs: study-tooling-off-cost.md, plan-work-queue.md
depends_on:
related: flip-trace, wait-sites, perf-diag, draw-classes, stage-b-counters, frame-stats-lines, slow-frames
checked: 2026-11-16
---
## What it does

Step 1 of the off-cost work (study-tooling-off-cost.md). Counters the frame-stats lines report were bumped whatever
`GOW3_FRAME_STATS` said, although `gow3_toggles.h` documented them as collected only with it. Now, without
`GOW3_FRAME_STATS=1`:
- GPU: every `Gow3Stats` add outside the report goes through `Gow3Stats::Add` / `Sub` (an `enabled` test first):
  `draws`, `dispatches`, `constant_ring_*`, `small_*`, `pipeline_binds`, `render_pass_ends`, downloads, faults,
  protects, ... (~50 sites). `DrawPipe::Run` times a packet (`__rdtsc` twice, two locked adds) only with stats;
  stage A's idle time and `WaitTimer` read no clock. LongWaits (`GOW3_LONG_WAITS`) takes its own start time.
- `Gow3Stats::enabled` is a 64-byte `Switch` (`operator bool`): it sat on the line that stage A wrote every draw
  (`draws`, `constant_ring_reused_bytes`), read many times a draw on stage B.
- Runtime: `memory_calls` (every guest memcpy / memset / memmove / memcmp), the mutex / condition counters
  (`locks`, `unlocks`, `waits`, `wakeups`), `queries` and the pool counters (`kept_*`, `foreign_handed`,
  `view_pages_back`) only with `runtime_fault_stats_enabled()`, whose flag is a `volatile` 64-byte struct (without
  `volatile` LLVM splits it into a bare int and drops the alignment). The exit report leaves the uncounted lines out
  (mutexes: "created" only).
- The six sites that tested `getenv("GOW3_FRAME_STATS") != nullptr` (the pipe stats, the drain waits, draw prep,
  occlusion, the texture cache's frame stats) use `Gow3Stats::enabled`: `GOW3_FRAME_STATS=0` no longer half-enables
  them.

## Invariants

- No behaviour depends on these counters: outside the frame-stats block (driver.cpp 401-1163, the pipe stats, the exit
  report) only `gpu_frames` (once a frame, still unconditional), `recorder_lean` (written on change) and
  `buffer_upload_bytes` are read; `buffer_upload_bytes` counts with `GOW3_FRAME_STATS=1` **or** `GOW3_BUFFER_STATS=1`
  (the per-region upload figure is a delta of it).
- `Gow3Stats::enabled` and the runtime flag are fixed for the process, so gauges (`hot_pages` add / sub) stay
  consistent.
- `Gow3Stats::enabled` is used only as a condition or a `bool` argument (`Scope{...}`, `Gow3SignalWait::Wait`): a new
  use that needs a real `bool` (varargs, template deduction) must write `bool(Gow3Stats::enabled)`.
- Check the placement after a change: `llvm-nm -C -n out/medievil-runtime.exe | grep -A1 " Gow3Stats::enabled$"`
  (next symbol 64 bytes on) and `grep -A1 runtime_fault_stats_enabled.enabled`.
- The functional pieces near these counters stay unconditional: `Gow3Sections::recording_thread`, `download_reason`,
  `pool_owner[]` stores, `hot_since`.

## Interactions

- With `GOW3_FRAME_STATS=1` everything is counted as before (and the runtime's exit report prints the old lines).
- The mutex counters still sit beside the region table's `vma_seq` / `vma_count` / `vmas` (linker order), but are
  no longer written without stats.
- Step 2 (`GOW3_DEV_TOOLS`) makes these switches compile-time constants in a release build.

## Evidence

- Inventory (2026-11-16, binary built 06:57): `lock incq` for `memory_calls` in `guest_memcpy`; `lock` adds for
  `draws`, `dispatches`, `constant_ring_bytes`, `constant_ring_reused_bytes`, `stage_b_busy_cycles`, `small_*`,
  `buffer_upload_bytes`, `render_pass_ends`; `Gow3Stats::enabled` at 0x1433baf20 between `draws` (baf18) and
  `hot_pages` (baf28); `unlocks` / `waits` / `wakeups` beside `vma_count` / `vma_seq` / `vmas`. Estimated 2-6% of
  stage B, not measured.
- Fix built (2026-11-16 08:16): `guest_memcpy` tests the flag, then skips the `lock incq`; `Gow3Stats::enabled` at
  0x1433baf80, next symbol at bafc0; the runtime flag at 0x140b723c0, next at 72400. `mutex-test`: all passed.
- A/B: to run (baseline `out/medievil-runtime-baseline-aa4c74b.exe` against this build, same settings).

## History and pitfalls

- A run with `GOW3_FRAME_STATS=1` is a different program now from one without (as it always was in intent): compare
  lows only between runs with the same setting.
