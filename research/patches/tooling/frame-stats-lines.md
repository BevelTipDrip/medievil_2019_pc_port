---
id: frame-stats-lines
title: GOW3_FRAME_STATS lines: upload rates, WriteData targets, fault handler time, frame pacing lows
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: 55146e6e4d, 90d7604154, d67ac70444, 56e323b3a4, 0ba768be2a
files: gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shadps4/video_core/amdgpu/liverpool.cpp, research/tools/deck_summary.py
symbols: frame_stats, longest_run, pacing_median, t_write_faults, t_read_faults, NoteWriteData, RunWriteData, lows
switches: GOW3_FRAME_STATS
tests: T-S1c, Native run 3, Native run 7, N-8, N-23, N-32
docs: plan-stage-b-split.md#t-s1c--t-w3-texture-memo-two-way-the-write_data-wait-2026-10-20-2331-2345-launched-by-claude-with-the-users-go-builds-2492874-d67ac70-logs-datat-s1c-two-waylog-datat-w3-writedata-targetslog, plan-linux-deck.md#drain-wake-ahead-6-vs-2-live-2026-11-06-build-d2f145f-lows-log-datadeck-wakeahead-1log, plan-linux-deck.md#native-run-3-stage-b-by-part-2026-11-03-appimage-of-56e323b-gow3_frame_stats1-gow3_sections1-gow3_slow_frames14-log-datadeck-native-3log, plan-linux-deck.md#native-run-7-gate-2-stats-2026-11-03-appimage-3efee7a-gow3_frame_stats1-by-file-log-datadeck-native-7-crashlog, plan-frame-pacing.md#n-8-panning-hitches-the-stream-class-table-and-logging-cost-2026-10-28-launched-by-the-user
depends_on:
related: W3, B1, drain-wake-ahead-6, slow-frames, fence-stats, async-log-writer, uffd
---
## What it does

Small additions to the inherited 5 s frame-stats report in `VideoOutDriver::Flip` (block under `frame_stats`,
`driver.cpp`). "Uploads: images / buffers MB/s" (`55146e6`). "Frame pacing" also gives the longest run of frames
over 1.5x the median (`longest_run`, `90d7604`). Since `0ba768b` each line ends with "; N frames, slowest a b c ms",
the window's slowest 2% (at least one). `deck_summary.py` `lows()` pools those into a state's 1% / 0.1% lows.
"Fault handlers: read faults/s, ms/s, write faults ms/s" (`56e323b`) is the time inside the port's handlers
(`Gow3Stats::t_read_faults` / `t_write_faults`, `page_manager.cpp`). "WriteData targets (5 s)" (`d67ac70`,
`NoteWriteData` in `liverpool.cpp`): up to 16 WRITE_DATA addresses with size, first word, count and wait time.

## Invariants

- Nothing here may change behaviour: every addition only reads counters. `NoteWriteData` runs only under
  `Gow3Stats::enabled` and only times the wait `RunWriteData` already makes (`WaitHostCopiesFor`,
  `WaitDeferredSignals`). The wait itself must not move inside the `if`.
- The lows are exact only while no window has more than 2% of its frames in the state's slowest 1% / 0.1% (class C).
  A window much worse than the rest drops frames from its list, so the pooled low reads too high. `lows()` returns
  nan when a log has no slowest lists (builds before 2026-11-06).
- `pacing_median` is written here and read by the slow-frame line (slow-frames). The pacing report must keep setting
  it, or `GOW3_SLOW_FRAMES` never prints.
- `NoteWriteData` keeps 16 slots per window: more targets are dropped silently.

## Interactions

- slow-frames depends on this block: it lives inside the same `if (frame_stats)` and takes its median from here.
- fence-stats prints inside the "Uploads" scope (`Gow3FenceStats::Report`, `runtime_memory_fault_report`).
- W3 came from the WriteData target list (T-W3); B1's WRITE_DATA waits are what the list times.
- `GOW3_FRAME_STATS` is checked three ways: `EmulatorSettingsImpl::Flag` here (`1`, `y`, `t`), `Gow3Stats::enabled`
  (`1`), and `getenv != nullptr` in `PrintPipeStats` and others. `GOW3_FRAME_STATS=0` turns on the pipe stats.

## Evidence

- T-S1c / T-W3: `RunWriteData` ~260 us a call twice a frame. Targets `0x6d2a4c88` and `0x6d2a4c90` (4 B, value 1)
  waited ~0.45 ms each, `0x30000000a0` ~0.03 ms. That led to W3 (rejected).
- Deck native run 3: handler time 4.9-6.6 ms/s (~0.1 ms a frame), on game threads, none on stage B.
- Drain wake-ahead 6 vs 2 (Deck, 2026-11-06), first use of the pooled lows: 56.5 / 51.8 vs 56.6 / 51.0 FPS.

## History and pitfalls

- Judge smoothness only from runs without `GOW3_SLOW_FRAMES` / `GOW3_SECTIONS` / `GOW3_PERF_DIAG` (N-8). N-23: the
  8-9 ms view-switch frames disappeared with diagnostics on. N-32: the user saw no spikes "without logging".
- The 5 s report is still synchronous `printf` on the flip path (stdout unbuffered, `setvbuf` in `probe.c`).
  Only the slow-frame lines and the perf-diag report go through async-log-writer.
- "write faults/s" in the inherited "Frame stats" line counts pages marked CPU-modified, not faults (Deck native
  run 7: ~1 real fault a frame against 44-115k/s "write faults"). `56e323b`'s message ("~760 write faults a frame")
  repeats that misreading. The handler-time line itself is correct.
- Not verified in game: diagnostics only.
