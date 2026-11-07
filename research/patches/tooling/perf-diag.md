---
id: perf-diag
title: GOW3_PERF_DIAG additions: write-fault downloads by guest instruction and owner, host copy waits and pass ends by caller, pass-break barriers, long waits
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: 537d86e965, 057afc64a0, fde36243a2, 338e0a0353, 2155ba72f9, 450df36a32, 214a52eefb, d6a12857a9, 464d9258d6
files: gpu/shadps4/video_core/renderer_vulkan/vk_wait_diagnostics.h, gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp, gpu/shadps4/video_core/buffer_cache/buffer_cache.h, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_runtime.cpp, gpu/shadps4/core/libraries/videoout/driver.cpp
symbols: WaitDiagnostics, HostCopies, RenderPassEnds, PassSwitches, BarrierBreaks, PassReopens, ReportDownloadSites, NoteGpuWriterShader, download_sites, gpu_writers, SwitchReason, InternKey, LongWaits
switches: GOW3_PERF_DIAG, GOW3_LONG_WAITS, GOW3_FRAME_STATS
tests: T-D1b, T-H1, N-3, N-11, N-12, T-F1, T-F2, N-23
docs: fix-inventory.md#9-diagnostics-worth-taking, plan-frame-pacing.md#t-d1b-the-downloads-are-guest-write-faults-2026-10-19-1749-1753-launched-by-claude-with-the-users-go-build-057afc6-log-datat-d1b-unmap-discardlog, plan-frame-pacing.md#n-3-where-the-drains-come-from-by-code-site-2026-10-27-launched-by-the-user-gow3_perf_diag1-log-datat-b1-barrier-siteslog, plan-frame-pacing.md#n-12-why-the-maps-bone-page-counts-as-gpu-written-2026-10-28-launched-by-the-user-t-map1-gpu-writerlog-gow3_perf_diag1, plan-stage-b-split.md#t-f2-which-barriers-break-the-passes-log-t-f2-barrier-breaks-trace-33
depends_on:
related: D1, B1, W2, lean-barriers, narrow-image-stages, fault-page-download, gpu-write-twins, park-unmapped, async-log-writer, micro-mip-targets, ring-reuse, depth-write-effective
---
## What it does

The inherited `GOW3_PERF_DIAG` "Wait profile" (GPU waits by `std::source_location`) gains, in `WaitDiagnostics`:
- `HostCopies`: `Scheduler::WaitHostCopies` callers, recording-thread and copy-thread time (`338e0a0`).
- `RenderPassEnds`: `Scheduler::EndRendering` callers that closed an open pass (`fde3624`).
- `PassSwitches`: what differed at a pass switch (`SwitchReason`). `PassReopens`: a pass reopened on the same
  targets, keyed by the site that ended it (`450df36`).
- `BarrierBreaks`: every `Runtime::FlushBarriers` by caller, with stages, layouts and the image size / format,
  "(between passes)" when no pass was open (`2155ba7`, widened in `d6a1285`).
- "Write-fault downloads": guest instruction (`Gow3Stats::fault_rip`, `guest+0x...`) and 64 KiB page
  (`download_sites`, `537d86e`). It also names who made the page GPU-written: the `ObtainBuffer` caller, the shader
  (`NoteGpuWriterShader`), the range and the marks (`gpu_writers`, `214a52e`).
`GOW3_LONG_WAITS=1` (`464d925`, `LongWaits` in `vk_scheduler.cpp`) is separate: only GPU waits of 1 ms or more, by
site and thread name, every 5 s.

## Invariants

- Measuring only. Each table takes a mutex per event (`WaitDiagnostics::Add`, `download_sites_mutex`) on stage B,
  the GPU thread and faulting game threads. That changes timing: N-23 saw the 8-9 ms frames vanish with it on.
- The reports print only from `VideoOutDriver::Flip`'s frame-stats block (`WaitDiagnostics::Report`,
  `ReportDownloadSites`). So `GOW3_PERF_DIAG=1` without `GOW3_FRAME_STATS=1` collects and never prints, and the
  64-entry tables fill up into "other wait sites".
- `Entry::file` is a `string_view`: keys must outlive the table. Use `source_location` file names, string literals
  (`SwitchReason`) or `InternKey` (a never-freed `std::set`). A temporary `std::string` key dangles.
- `LongWaits` takes its start from `Gow3Stats::WaitTimer`, which always reads the clock (unlike `Gow3Stats::Timer`),
  so it works without `GOW3_FRAME_STATS`. Only waits of 1 ms or more take its lock.
- `gpu_writers` is filled in `ObtainBuffer` for every written binding while on and is never cleared.

## Interactions

- `057afc6` also carries D1 (unmap discard, toggle bit 56). `d6a1285` carries narrow-image-stages,
  depth-write-effective, ring-reuse and `GOW3_SKIP_TOOL_INFO` (profiler-support). `464d925` carries
  micro-mip-targets.
- The report goes through async-log-writer (`de30a39`).
- Findings fed B1 / W2 (host copy waits), lean-barriers and narrow-image-stages (barrier sites), D1,
  fault-page-download and gpu-write-twins (write-fault downloads), and park-unmapped (N-23 via `GOW3_LONG_WAITS`).

## Evidence

- T-D1b: every synchronous download was a guest write fault, 25.8-27.4/s, ~73 ms of GPU waits a second.
- N-3 (fight, per frame): 174 barriers, 310 pass ends (switches 168, barrier flushes 124), 11 wasted reopens. Depth
  ping-pong was the top site (~29 + 25).
- N-12: the bone page was GPU-written by shader `0x86c25cc15153c173`, range `+0x14`, ~800-1,000 marks a second.
- N-23 (`GOW3_LONG_WAITS`): in gameplay only `vk_presenter.cpp:560` on `gow3:DrawRec`.

## History and pitfalls

- `GOW3_LONG_WAITS` exists because the full wait profile hid the spikes it was meant to find (t-hzb5).
- The launcher sets `GOW3_PERF_DIAG` (`setdefault`, from the `perf_diag` setting, default 0) independently of frame
  statistics: turning on only `perf_diag` gives no output.
- Not verified in game: diagnostics only.
