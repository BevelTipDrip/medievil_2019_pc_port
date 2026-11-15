---
name: frame-jitter-capture
description: Find where individual slow frames, spikes, streaks or frame-time "push-pull" come from in MediEvil, frame by frame - the flip trace plus a light WPR capture, exact alignment, and the etw_* / flip_* analysis tools. Use for any frame pacing, 0.1% low, stutter or spike investigation on Windows.
---

# Frame jitter capture and analysis

Goal: for each slow frame, what every thread of the game did in that frame (ran, waited on whom, was
starved, was interrupted). Data, not toggles: averages over windows and static screenshots led us in circles.
Cards: `research/patches/tooling/flip-trace.md`, `tooling/wpr-capture.md`.

## Rules that cost us hours (2026-11-15)

- **Never the built-in WPR `CPU` profile for jitter.** It walks the stack on every context switch: 10.5% of the
  game's CPU at 237 FPS (16-37% on threads that switch often). The frames it "showed" bumping and alternating went
  flat the moment the recording stopped. Use `research/tools/wpr-jitter.wprp!Jitter` (switches without stacks,
  1 kHz samples with stacks, DPC/ISR). Even it adds a few larger spikes: compare against a profiler-free stretch.
- **Profiler-free stretch first**, in the same session: the user stands still ~60 s with no WPR running, then starts
  the hotkey script. That stretch is the truth the capture is judged against.
- **The ring keeps only the last ~75-90 s** of events, although the ETL header spans the whole session. During a
  `-stop` (~20-25 s) the samples continue without switches or stacks. Exports before the data start are empty.
- **Exact alignment needs the flip trace's `wall` row** (build `d032cd4` or later): ETW time = flip time mapped
  through the wall clock and the ETL's start time (`etw_common.flip_to_etw`). Without it the offset is +-0.5 s and
  per-frame matching is impossible.
- **Never F9 or F10** as the hotkey (the game: UE4 upscaler, motion-vector view). F8 works.
- Alt-tab makes large spikes: the user tabs back in and waits ~1 min before the still period.
- The flip trace itself fsyncs nothing but flushes every 2,048 lines (~1.2 s): it was present in flat stretches
  too, so it is not the cause of spikes; keep it in mind for very fine effects.
- No heavy work (exports, builds) while the game runs; the user's frames are the measurement.

## Setup (user's go for the launch; the user runs WPR, it needs admin)

1. `medievil.ini`: `present_frame_trace=1` (writes `user\logs\flip-trace.csv`, replaced each start). Byte-safe
   CRLF edit with a backup (see the build-ab skill). `extra-env.txt`: no periodic logging
   (`GOW3_FPS_LOG=0`, no `GOW3_FRAME_STATS`, `GOW3_COPY_FAULT_LOG=0` if the copy-fault trace is not wanted).
2. The user, in an elevated PowerShell, after the profiler-free stretch:
   `powershell -ExecutionPolicy Bypass -File C:\Users\Ty\MediEvil\medievil_2019_pc_port\research\tools\wpr-hotkey.ps1 -Key F8 -Tag <tag> -WprProfile "C:\Users\Ty\MediEvil\medievil_2019_pc_port\research\tools\wpr-jitter.wprp!Jitter"`
   It prints `Traces go to D:\MediEvil-archive\research-data\wpr` (the junction's real path: WPR refuses a path
   through a junction). Press F8 right after seeing the effect; two beeps = saved, a low beep = failed (message).
3. Record the time of each user message as it arrives (`date +%H:%M:%S >> <scratchpad>/run-notes.txt`): the user's
   messages carry no timestamps on our side. Ctrl+C the script when done (the ring restarts after each save).

## Analysis (game closed; scratchpad for exports, delete them after)

```
cd research/tools
py -3.14 etw_export.py <etl>                                    # span (local start / end)
py -3.14 flip_plot.py <flip-trace.csv> <a> <b> <out.png> F8:<t>-<t+0.5>   # look first; send the PNG to the user
py -3.14 etw_export.py <etl> <from_s> <to_s> <out.csv>          # 2-5 s around the slow frames (~60 MB/s)
py -3.14 etw_frame_threads.py <out.csv> <etl> <flip-trace.csv> <from_s> <to_s>   # per frame, per thread
py -3.14 etw_thread_profile.py <out.csv> <tid> before:a-b slow:c-d               # where a thread's time went
py -3.14 etw_long_waits.py <out.csv> <tid> <from_s> <to_s> 0.8                   # its long waits and wakers
```

Times for the etw_* tools are ETW seconds (from the ETL start). Convert flip-trace times of slow frames with the
`wall` row (`etw_common.flip_to_etw`), or list them from `etw_frame_threads.py` over a wider range. The tools find
the game's PID and the runtime's load address themselves (`etw_common.runtime_base`: hot addresses must resolve to
pipeline functions; the outermost frames of sample stacks are often truncated and mislead). Stage roles come from
sampled functions: `DrawPipe::Run` = stage B, `RecorderThread` = VkRecorder, `Liverpool::Process` /
`ProcessGraphics` = stage A, `PrepareGraphicsPipeline` = draw-prep workers, mostly guest code = game threads,
`nvoglv64.dll` only = an NVIDIA driver thread.

Reading `etw_frame_threads.py`: `r` = ms the thread ran in that game frame, `d` = ms between being readied and
running (CPU starvation). DPC/ISR steady at 0.5-0.8 ms a frame on this PC is normal. A game thread at ~100% of
a slow frame: look at `etw_thread_profile.py` (runtime lock time, guest call sites). A game thread mostly idle:
`etw_long_waits.py` (a run of ~1 ms waits "readied by Idle" is a timer-polling loop; the last waker is what it
waited for).

## What 2026-11-15 found (static view, 4K 75% DLSS, ~230 FPS) - work queue row "Small frame-time spikes"

- Streaks of 5-6 ms frames: the game thread spends ~17% in our runtime's guest rwlock / mutex HLE (global
  `registry_lock`, a `calloc` per acquire) and 16% in other runtime code.
- Single 8-14 ms spikes: the game thread sleeps in 1 ms timed waits until stage B signals.
- Not the cause: OS / drivers, CPU starvation, the flip trace.

## Pitfalls in the tooling itself

- Bash heredocs here eat backslashes (`'\\'` in Python became `'\'`): write analysis scripts with the Write tool.
- `flip_toggle_ab.py` turns the start row into wall time with the current machine's clock offset (valid only
  before a reboot or sleep); prefer the `wall` row.
- An xperf export re-reads the whole ETL each time (~1 min per 3 GB): export a few short windows, not the run.
