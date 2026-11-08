---
id: async-log-writer
title: Slow-frame lines and the perf-diag report formatted on a background log writer, not ~30 printf on the flip path
area: tooling
status: diagnostic
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: de30a399f6
files: gpu/shim/gow3_async_log.h, gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_wait_diagnostics.h
symbols: Gow3AsyncLog, Queue, Get, Write, Append
switches:
tests: N-8
docs: plan-frame-pacing.md#n-8-panning-hitches-the-stream-class-table-and-logging-cost-2026-10-28-launched-by-the-user, fix-inventory.md#9-diagnostics-worth-taking
depends_on:
related: slow-frames, perf-diag, stream-class-table, sections, frame-stats-lines, flip-trace, ring-reuse
checked: 2026-11-07
---
## What it does

`gpu/shim/gow3_async_log.h`: `Gow3AsyncLog::Append` formats printf-style text into a `std::string` on the calling
thread; `Write` pushes it onto a deque under a mutex and notifies a detached worker thread. The worker swaps the
whole deque out, `fwrite`s each string to stdout and calls `fflush`. Users: the slow-frame lines (driver.cpp, one
string per frame with its "stage B ms:" line), `WaitDiagnostics::Report` (one string per report, written by a
scope guard); later also `GOW3_LONG_WAITS` (`vk_scheduler.cpp`, `464d925`) and the constant-ring pressure report
(`ConstantRing::ReportPressure`, `4038564`, ring-reuse), which needs no switch: it prints at most every 5 s when the
ring ran full, so a default run can use the writer too. Always compiled in, no switch (default on/on in that sense).
The worker thread starts at the first `Write`.

## Invariants

- The caller only formats and takes a short mutex; the console write happens on the worker. A new diagnostic on the
  flip path or stage B should use `Append` + `Write`, not `printf` (stdout is unbuffered, `setvbuf _IONBF` in
  `probe.c`, so every `printf` is a console write).
- Order is kept among queued strings, not against direct `printf` output: a queued line can appear after later
  synchronous lines. Log readers must not rely on adjacency between the two kinds.
- Each `Write` call is printed whole (one `fwrite`), so a multi-line report stays together.
- The queue is never destroyed (`new Queue`, worker detached): lines written at exit may be lost if the process
  ends before the worker runs. Not suitable for a crash report.
- The queue is unbounded: a producer faster than the console grows memory without limit.

## Interactions

- slow-frames and perf-diag are its users. Their output must not be judged against the old printf timing (logs
  before `de30a39`).
- Shares `de30a39` with stream-class-table (the panning hitch fix) and the `obt_*` sections split.
- Not used by the 5 s frame-stats report (frame-stats-lines), perf-diag's "Write-fault downloads" list
  (`ReportDownloadSites`, `printf` in that block), the WriteData targets and "stage B us per packet" lines (stage B /
  stage A `printf`), the texture cache report (texcache-stats) or the flip trace (flip-trace, its own `fprintf` under
  a mutex): those still write on the calling thread. The commit's "the GOW3_PERF_DIAG report" means
  `WaitDiagnostics::Report` only.

## Evidence

- N-8 `t-pan1` (`GOW3_SLOW_FRAMES=1.6`, `GOW3_SECTIONS=1`): ~30 unbuffered printf a slow frame on the flip path,
  862 lines. The A/B without diagnostics showed the large stutters in logged runs were mostly the logging:
  "tonight's changes" off 197.2 FPS, worst frame median 9.3 ms, 4 of 11 windows with a frame over 12 ms; on 207.8
  FPS, 7.2 ms, 1 of 16. That A/B ran without diagnostics, so the writer was idle in it: it measured
  stream-class-table and the rest of that night's changes. No run measured logged pacing before and after the
  writer.

## History and pitfalls

- `WaitDiagnostics::Report` printed on the flip path delayed a frame every 5 s (comment in the report).
- Even with the writer, diagnostics change timing (formatting, counters): N-8's rule stands, judge smoothness only
  from runs without them.
- Not verified in game.
