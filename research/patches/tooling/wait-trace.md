---
id: wait-trace
title: GOW3_WAIT_TRACE: every guest wait over a threshold as one CSV row (thread, site, requested, actual), lined up with the flip trace
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits:
files: src/runtime_waitsites.c, src/runtime.h, src/runtime_thread.c, research/tools/wait_frames.py
symbols: trace_wait, trace_path, trace_min_ns, sites_on, wall_ns, runtime_thread_current_name
switches: GOW3_WAIT_TRACE, GOW3_WAIT_TRACE_MIN_US
tests:
docs: plan-work-queue.md
depends_on: wait-sites
related: flip-trace, cond-precise-timeout, wpr-capture
checked:
---
## What it does

`GOW3_WAIT_TRACE=<file.csv>` turns on wait-sites' measurement (without its 5 s report, which stays
`GOW3_WAIT_SITES=1`) and writes each guest wait that took at least `GOW3_WAIT_TRACE_MIN_US` (default 500) us:
`w,<monotonic ns at the end>,<thread id>,<guest thread>,<kind>,<site>,<requested ns>,<actual ns>`. One
`wall,<Unix ns>,<monotonic ns>` row comes first. `research/tools/wait_frames.py <flip-trace.csv> <wait-trace.csv>
<from_s> <to_s> [thread] [slow_ms]` joins it with a flip trace (through both files' `wall` rows): long waits per
thread, the waits of one thread in each slow game frame, sites that cover slow frames more than normal ones, and
periodic long waits.

## Invariants

- Rows are written under one mutex on the waiting thread, buffered (1 MB) and flushed every 1,024 rows: the trace
  is part of the timing it measures. MediEvil at 237 FPS wrote ~8,600 rows a second at 500 us (all threads).
- Off: no change to wait-sites' cost (one cached load per wait).
- `runtime_thread_current_name` is the guest thread's name (`main` is UE4's game thread), NULL on host threads.

## Evidence

- 2026-11-15 run: sized the timed-wait rounding of cond-precise-timeout (main: requested median 0.50, actual 1.01 ms).
- The 1.36 s periodic stalls: every game thread waits at once (parked, plan-work-queue.md).

## History and pitfalls

- At ~8,600 rows a second it may itself cause the process-wide stalls it records: raise the threshold
  (`GOW3_WAIT_TRACE_MIN_US`) before relying on it for such events.
- Not verified in game: diagnostics only.
