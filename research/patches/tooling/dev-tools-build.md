---
id: dev-tools-build
title: Release and dev builds (GOW3_DEV_TOOLS): the release build has the diagnostics compiled out; the launcher runs it unless Development build is ticked
area: tooling
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 21886b6
files: gpu/CMakeLists.txt, build.sh, packaging/linux/appimage.sh, tests/test_launcher_controls.py, gpu/shim/gow3_dev_tools.h, gpu/shim/gow3_toggles.h, gpu/shim/gow3_sections.h, gpu/shim/gow3_draw_classes.h, gpu/shim/gow3_overlay.cpp, gpu/shim/gow3_overlay.h, gpu/shim/gow3_copy.cpp, gpu/shim/gow3_copy.h, gpu/shim/gow3_write_log.cpp, gpu/shadps4/common/logging/log.h, gpu/shadps4/video_core/renderer_vulkan/vk_platform.h, gpu/shadps4/video_core/renderer_vulkan/vk_frame_capture.h, gpu/shadps4/video_core/renderer_vulkan/vk_frame_capture.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_gpu_profiler.h, gpu/shadps4/video_core/renderer_vulkan/vk_gpu_profiler.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_wait_diagnostics.h, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_motion.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_upscaler.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.cpp, src/runtime.h, src/runtime_memory.c, src/runtime_waitsites.c, src/probe.c, launcher/medievil_launcher.py
symbols: DEV_DIAGNOSTICS, dev_diagnostics_off_reason, dev_build_missing, GOW3_DEV_TOOLS, Gow3Stats::enabled, Gow3Toggle::Disabled, Gow3Toggle2::Disabled, Gow3Sections::Enabled, WaitDiagnostics::Enabled, CacheStatsEnabled, Ue4Upscaler::StatsOn, GpuProfiler::Get, FrameCapture::Active, FrameCapture::OnFlip, Gow3Overlay::TakeFrameDumpRequest, Gow3Overlay::FpsLogOn, SetObjectName, LOG_DEBUG, runtime_fault_stats_enabled, runtime_wait_sites_enabled, RUNTIME_WAIT_BEGIN, dev_executable, runtime_executable
switches: GOW3_DEV_TOOLS, GOW3_BUILD, GOW3_APPIMAGE_DEV, GOW3_FPS_LOG, GOW3_COPY_FAULT_LOG, GOW3_FRAME_DUMP_TRIGGER, dev_build
tests: launcher-controls
docs: study-tooling-off-cost.md, plan-work-queue.md
depends_on: stats-counters-gated
related: flip-trace, sections, wait-sites, wait-trace, perf-diag, renderdoc-capture, mausoleum-diagnostics, lineage-toggle-file, stage-b-counters
checked: 2026-11-16
---
## What it does

Step 2 of the off-cost work (study-tooling-off-cost.md). CMake option `GOW3_DEV_TOOLS` (default ON) defines
`GOW3_DEV_TOOLS=1/0` for every target, C and C++ (`gpu/shim/gow3_dev_tools.h`: a compile outside CMake counts as
dev). `build.sh` on Windows (`GOW3_BUILD`, default `release`):
- `release`: `out/gpu-release` with `GOW3_DEV_TOOLS=OFF` -> `out/medievil-runtime.exe`, `medievil-gpu-check.exe`,
  `medievil-play.exe` (what run.py, the launcher, medievil-play and package.sh use).
- `dev`: `out/gpu` (the tests' tree, as before) with `GOW3_DEV_TOOLS=ON` -> `out/medievil-runtime-dev.exe`.
- `both`. Logs: `out/gpu-build-release.log`, `out/gpu-build-dev.log`.

The launcher's Advanced > Diagnostics > "Development build" (`dev_build`, app setting) sets `GOW3_PROBE` to the dev
exe when it exists; run.py starts `GOW3_PROBE` instead of `out/medievil-runtime.exe`. The run-log header names the
exe started. The runtime's first log line is `Runtime: release build (GOW3_DEV_TOOLS=0)` or `dev build`.

Compiled out of the release build (switch compile-time false, so its sites fold away):
- Frame stats (`Gow3Stats::enabled` constexpr false; the runtime's `runtime_fault_stats_enabled` returns 0): all
  counters, `WaitTimer`, draw-pipe timing, the stats lines, draw classes (empty `~Scope`, `DrawClass` not called),
  host-map / pool / mutex counters.
- `GOW3_SECTIONS`, `GOW3_PERF_DIAG` (`WaitDiagnostics`), `GOW3_TEXCACHE_STATS`, `GOW3_UPSCALER_STATS`,
  `GOW3_GPU_PROFILE` (`GpuProfiler::Get` null), wait sites and the wait trace (`RUNTIME_WAIT_BEGIN` is 0).
- The toggle file: no watcher thread (no 250 ms `fopen`), `Gow3Toggle::Disabled` / `Gow3Toggle2::Disabled` false.
  The runtime C's direct reads of the toggle words stay (loads of a word that stays 0).
- The F11 frame analyzer (`FrameCapture::Active` false, `OnFlip` returns at once: no 250 ms `filesystem::exists` on
  the present thread, no target watch), `GOW3_FRAME_DUMP_TRIGGER`'s file poll, the guest write log (`GOW3_WRITE_LOG`;
  ring 1 entry instead of 40 MiB), Vulkan object names (`SetObjectName` returns before formatting), `LOG_DEBUG`.

Both builds:
- Shift+F12 sets `Gow3Overlay::frame_dump_requested`; the presenter takes it at its next frame (a relaxed load per
  frame, the exchange only when set). Until now the key wrote the trigger file the presenter polled. The file still
  works in the dev build (outside tools).
- `GOW3_FPS_LOG` defaults **off** (`=1` turns it on); the launcher no longer sets 1. It also gates the UE4 motion /
  upscaler lines every 3,000 frames (`Gow3Overlay::FpsLogOn`).

Kept in release (functional or user-facing): the flip trace (`present_frame_trace`), F12 screenshots, Shift+F12
dumps, crash recorders and `GOW3_CRASH_DUMP`, the save trace, the overlay FPS readout, the AvPlayer map,
`IsDisplayBuffer` / `AddDisplayBuffer`, `recording_thread`, `download_reason`. The copy-fault trace stays but
defaults **off** in release (`GOW3_COPY_FAULT_LOG=1` on); in dev it stays on by default.

## Invariants

- A diagnostic that must not cost anything in release tests its switch so that the release value is a compile-time
  constant (`if constexpr (!GOW3_DEV_TOOLS)`, `#if GOW3_DEV_TOOLS`, or a `constexpr` switch). A runtime `getenv`
  check alone is not compiled out.
- Nothing functional may sit behind `GOW3_DEV_TOOLS`. The lists above ("Kept in release") are the pieces next to
  diagnostics that the game needs.
- `Gow3Stats::Switch::operator bool` is `constexpr`, so `if (Gow3Stats::enabled)` folds in release. `enabled` is
  still a 64-byte object in dev (stats-counters-gated).
- Every CMake tree is one build type (`GOW3_DEV_TOOLS` is a cache option): `out/gpu` dev, `out/gpu-release`
  release. A tree configured by hand without the option is dev.
- run.py honours `GOW3_PROBE` before `out/`; extra-env.txt can also set it.

## Interactions

- Every diagnostic launcher option (frame stats, GPU profile, perf diag, F11) does nothing without "Development build"
  (greyed out since step 3). `extra-env.txt` diagnostics (`GOW3_SECTIONS`, `GOW3_WAIT_TRACE`, toggle-file A/Bs) also
  need the dev build.
- Linux / AppImage / Deck (2026-11-16, step 3): `build.sh` on Linux has the same `GOW3_BUILD` (release default:
  `out/gpu-release` or `out/gpu-v3-release` -> `out/medievil-runtime`, `out/build-type` = release; dev: `out/gpu` /
  `out/gpu-v3` -> `out/medievil-runtime-dev`, `out/cpu-level-dev`; both). `--test` builds the dev tree too (the
  tests run there). The shared-library build (`GOW3_STATIC_GPU=OFF`) is dev only. `appimage.sh` packs the release
  build and refuses one without `out/build-type` = release; `GOW3_APPIMAGE_DEV=1` packs the dev build as
  `MediEvil-x86_64[-v3]-dev.AppImage` for the Deck test loop (frame stats, the toggle file, wait traces).
  Until then every AppImage and Deck run was a dev build.
- Launcher (step 3): frame stats, GPU profile, the perf report and the F11 capture key (`DEV_DIAGNOSTICS`) are
  greyed until Development build is ticked and the dev exe exists, with the reason below it
  (`dev_diagnostics_off_reason`); without a dev exe the Development build box is greyed too ("not built" in a
  source tree, "not in this package" in the zip / AppImage). The flip trace and validation layers stay.

## Evidence

- Inventory and estimates: study-tooling-off-cost.md. Fix built 2026-11-16 13:15 (both trees, the same 6 old
  warnings each). Release exe 136.1 MB vs dev 139.4 MB; `rdtsc` sites 52 vs 419; no `Gow3Stats::enabled`,
  `runtime_fault_stats_enabled.enabled` or `toggle_watcher` symbol in release, 0 calls to
  `runtime_fault_stats_enabled`; the strings `GOW3_SECTIONS`, `GOW3_PERF_DIAG`, `GOW3_TOGGLE_FILE`,
  `GOW3_CAPTURE_TRIGGER` absent. Launcher tests pass (Python 3.14).
- A/B, **verified** (the user: "clearly better than run B, by quite a bit"; 2026-11-16 evening, 4K 75% DLSS, Reflex 237, flip trace on, FPS log and copy-fault trace off, both logs
  quiet; release run first, then dev, the same route: save menu, the Graveyard's long view after the load, a Tome
  pickup, combat). 20 s windows grouped (steady >= 235 FPS, heavy < 235, loading = a frame > 50 ms), release / dev:
  steady 0.1% low 216.3 / 185.2 FPS, worst 5.2 / 6.9 ms, frames > 6 ms 0 / 1.4 a minute; heavy average
  226.5 / 224.3, 0.1% low 157.7 / 141.2, p99.9 5.94 / 6.52 ms, frames > 6 ms **10.1 / 68.6** a minute, > 8 ms
  0.4 / 1.0; loading p99.9 26 / 171 ms, frames > 8 ms 115 / 284 a minute. The dev run came second (warm file and
  shader caches), so run order favoured it. The user: in release, spikes only at the save menu (opening it, after
  it, the map screen load), a small series at the Graveyard's first long view (shrinking over recent patches) and
  one at a Tome pickup; in dev, deeper dips in combat, much larger and longer asset-load spikes, worse 0.1% lows in
  the static view. Traces and logs: `research-data/off-cost-step2-20261116/` (not in git); summary script
  `ab_windows.py` (session scratchpad).
- Linux (2026-11-17 00:07, WSL Debian, clang, `bda5f7f`): release `out/medievil-runtime` 0 tooling symbols, 50 `rdtsc`
  sites, no dev switch name; dev 428 `rdtsc`. `dist/MediEvil-x86_64.AppImage` (release) and `-dev.AppImage` packed;
  the 2.4.0 one kept as `MediEvil-x86_64-2.4.0.AppImage`. On the way: a new tree defaulted to GCC, which rejects
  `vk_instance.cpp`'s out-of-order `.occlusionQueryPrecise` (now in order; new trees use clang, `262bf6f`), and
  `runtime_waitsites.c` lacked `_GNU_SOURCE` for `syscall` since the wait trace (`599380c`): the Linux build had not
  compiled since 2026-11-15. `build.sh --test` still fails (tests/test_pad.c, queue). Not yet run on the Deck.

## History and pitfalls

- Until 2026-11-16 the user's exe and the dev exe were the same build: every earlier A/B and baseline is a dev-build
  run. Compare a release-build run only with release-build runs (LANDMINES 8c).
