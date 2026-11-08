---
id: profiler-support
title: Profiling support: VkRecorder samples (GOW3_SAMPLE_FILE), Nsight (skip tool info, stdout file, run-game -Nsys / -Ngfx)
area: tooling
status: diagnostic
platforms: windows
default_windows: off
default_deck: n/a
origin: ours
class: G
commits: 151dadbf6d, d6a12857a9
files: gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp, gpu/shim/gow3_sampler.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp, src/probe.c, run.py, research/tools/run-game.ps1, research/tools/nsys-bisect.ps1
symbols: SampleCurrentThread, RecorderThread, getToolProperties, stdout_file, GOW3_NGFX
switches: GOW3_SAMPLE_FILE, GOW3_SKIP_TOOL_INFO, GOW3_STDOUT_FILE
tests: N-1, N-2, N-5
docs: fix-inventory.md#9-diagnostics-worth-taking, building-and-tools.md#profiling-and-crashes, plan-frame-pacing.md#n-1-nsight-systems-gpu-busy-vs-working-hard-in-the-fight-2026-10-27-launched-by-the-user-report-research-datansysnsys-fight-1-1nsys-rep-log-datansys-fight-1log
depends_on:
related: narrow-image-stages, ring-reuse, depth-write-effective, perf-diag, renderdoc-capture, reflex
checked: 2026-11-07
---
## What it does

- `151dadb` (roadmap L2): `Scheduler::RecorderThread` calls `Gow3Sampler::SampleCurrentThread("VkRecorder")`, so
  the inherited `GOW3_SAMPLE_FILE=<prefix>` sampler (~1,000 stack samples a second, `<prefix>-<thread>.txt`
  rewritten every 10 s, named by `scripts/symbolize_samples.py`) covers the Vulkan recording thread too, besides
  `GpuCmd` (stage A) and `DrawRec` (stage B).
- `d6a1285`: `GOW3_SKIP_TOOL_INFO=1` skips `physical_device.getToolProperties()` in `vk_instance.cpp`. Nsight
  Systems 2026.5.1's Vulkan layer forwards `vkGetPhysicalDeviceToolProperties` to a null pointer and the game
  crashes at boot. `GOW3_STDOUT_FILE=<file>` (`probe.c` `main`, Windows) reopens stdout unbuffered into the file
  and points stderr and the Win32 std handles at it, so a launch under ngfx keeps its log and crash report.
  `run.py` runs the game under Nsight Graphics when `GOW3_NGFX=<dir>` is set (`GOW3_NGFX_CMD`, `GOW3_NGFX_ARGS`).
  `run-game.ps1 -Nsys` sets `GOW3_SKIP_TOOL_INFO=1` and `GOW3_REFLEX=0`, and holds F9 itself around
  `nsys start` / `nsys stop`. `-Ngfx` sets `GOW3_NGFX`, `GOW3_SKIP_TOOL_INFO=1` and `GOW3_STDOUT_FILE`
  (`<tag>\game.log`), Reflex stays on. Both refuse to start without an administrator shell (GPU counters).
- The sampler (`gow3_sampler.cpp`, `GOW3_SAMPLE_FILE`) is upstream; only the VkRecorder hookup is ours.

## Invariants

- Each switch is read only at start-up and changes nothing when unset: `SampleCurrentThread` returns at once without
  `GOW3_SAMPLE_FILE`, the tool query runs as before without `GOW3_SKIP_TOOL_INFO=1`, stdout stays the console.
- The sampler is Windows only (`gow3_sampler.cpp` has an empty Linux stub). It suspends the target thread to read its
  context about 1,000 times a second, which adds stalls to that thread: never judge pacing with it on.
- The tool query is informational (it logs the tools found). Skipping it must not change any later code path.
- `GOW3_STDOUT_FILE` must be applied before anything prints (first lines of `main`), and stays unbuffered like the
  console path (`setvbuf _IONBF`): a crash loses nothing.
- Only the port's own threads can be sampled this way. The game's threads need WPR (building-and-tools.md).

## Interactions

- `d6a1285` also carries narrow-image-stages, depth-write-effective, ring-reuse and perf-diag's barrier counts
  (behaviour patches, their own cards).
- renderdoc-capture uses the same `run.py` launch wrapper (`GOW3_RENDERDOC`). The two are exclusive (`elif`).
- `-Nsys` turns Reflex off as a precaution (not a known conflict).

## Evidence

- N-1 (Nsight Systems, fight): in the dips the GPU works harder than in ordinary frames (corrected the same day: in
  absolute terms far from saturated, SMs Active 78%, SM Issue 45%). Tracing overhead was high (median frame 6.4 ms
  vs ~5 ms normally). Nsight's WDDM trace hung `nsys stop` forever, found with `nsys-bisect.ps1` on a
  dummy program and dropped.
- N-2 / N-5 (Nsight Graphics GPU Trace): the GPU waits for idle most of each frame; same-spot A/B of
  narrow-image-stages (~9% shorter GPU frame, `d6a1285`'s message).

## History and pitfalls

- ngfx's own timeout ended its session and closed the game (`ngfx-narrow-1`): `run.py` passes `--no-timeout`.
- Nsight's hotkey never reached the game; `run-game.ps1` handles F9 itself (a system-wide hotkey that runs
  `nsys start` / `nsys stop`).
- `GOW3_SKIP_TOOL_INFO` is also read on Linux (shared `vk_instance.cpp`), harmless there; `GOW3_STDOUT_FILE` is
  `#ifdef _WIN32` only.
- Not verified in game: diagnostics only.
