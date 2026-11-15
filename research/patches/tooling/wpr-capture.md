---
id: wpr-capture
title: WPR capture scripts: hotkey ring buffer and timed capture, junction-safe trace folder, failed stops reported, a light jitter profile
area: tooling
status: diagnostic
platforms: windows
default_windows: off
default_deck: n/a
origin: ours
class: G
commits: 2ff26df, e4b9ccb
files: research/tools/wpr-hotkey.ps1, research/tools/wpr-capture.ps1, research/tools/wpr-common.ps1, research/tools/wpr-jitter.wprp, research/tools/etw_common.py, research/tools/etw_export.py, research/tools/etw_frame_threads.py, research/tools/etw_thread_profile.py, research/tools/etw_long_waits.py, research/tools/flip_plot.py
symbols: Get-RealPath, Get-TraceFreeGB, Start-Ring, flip_to_etw, runtime_base, etl_span
switches:
tests:
docs: plan-work-queue.md
depends_on:
related: flip-trace, profiler-support, slow-frames
checked:
---
## What it does

`wpr-hotkey.ps1` (elevated) records into WPR's in-memory ring and saves the seconds before a key press
(`research-data\wpr\<tag>-N.etl` + `.times.txt`); `-Forward` starts a timed capture (`wpr-capture.ps1`) instead.
`wpr-common.ps1` (dot-sourced) resolves the trace folder through junctions (`Get-RealPath`), checks free space on
that drive and picks the ADK's WPR. `wpr-jitter.wprp!Jitter`: context switches and ready-thread events without
stacks, 1 kHz samples with stacks, DPC and interrupts, 3 GB of buffers (about 90 s at 237 FPS). Analysis:
`etw_export.py`, `etw_frame_threads.py` (per frame and thread), `etw_thread_profile.py`, `etw_long_waits.py`,
`flip_plot.py`; procedure in the `frame-jitter-capture` skill.

## Invariants

- WPR's `-stop` rejects a results path through a junction (0xc5600600, nothing saved): always pass the real path.
- A failed `-stop` leaves the profile running: cancel before restarting (else 0xc5583001 on every later start).
- Never F9 or F10 as the key with MediEvil (the game uses them: UE4 upscaler, motion vector view).
- For frame-time jitter use the jitter profile: the built-in CPU profile walks the stack on every context switch.

## Interactions

- flip-trace: its `wall` row lines a slow frame up with the ETL (the ETL's start time is wall clock).
- The ring keeps only the last ~75-90 s of events although the ETL header spans the whole session; during a
  `-stop` the samples continue without switches or stacks (about 20-25 s with the CPU profile).

## Evidence

- 2026-11-15 spikes-1..4: every stop failed (junction); fixed, then spikes-2/3 and static-1..3 saved.
- static-1/2 (CPU profile, 4K 75% DLSS, static view): 10.5% of the game's CPU in ETW stack walks (16-37% on
  threads that switch often); the frames went flat at 4.22 ms the moment the stop began, game-frame sd 0.47-0.50 ->
  0.18-0.25 ms. jitter-1 (this profile): 230.0 vs 230.9 FPS, game-frame sd 0.23 vs 0.20 ms, frames over 6 ms
  6 vs 2 per minute against no profiler.

## History and pitfalls

- The earlier analyses of spikes-2 (game thread "saturated", push-pull) measured the CPU profile's own cost.
