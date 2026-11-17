---
id: mutex-holders
title: GOW3_MUTEX_HOLDERS: who holds a guest mutex while a thread waits on it, and the holder's stack (sampled)
area: tooling
status: diagnostic
platforms: windows
default_windows: off
default_deck: off
origin: ours
class: G
commits:
files: src/runtime_mutex.c, src/runtime_waitsites.c, src/runtime.h, tests/test_mutex.c, research/tools/mutex_holders.py
symbols: runtime_mutex_holders_enabled, runtime_wait_trace_line, runtime_wait_format_site, runtime_wait_thread_name, traced_lock, holder_taken, holder_releasing, holder_sampler, return_after_call, cond_release, cond_retake
switches: GOW3_MUTEX_HOLDERS, GOW3_WAIT_TRACE, GOW3_WAIT_TRACE_MIN_US
tests: mutex-test
docs: plan-work-queue.md
depends_on: wait-trace, dev-tools-build
related: wait-sites, wait-trace
checked: 2026-11-16
---
## What it does

For the queue row "RHIThread waits ~2 ms a frame on one guest mutex" (guest+0x6795a1, RHI `EndFrame`; wait trace
2026-11-15: 227 waits a second, median 2.1 ms, 57% of RHIThread's time). The wait trace says who waited; this says
who held the lock and what the holder was doing. Dev build only (`#if GOW3_DEV_TOOLS`); with
`GOW3_MUTEX_HOLDERS=1` and `GOW3_WAIT_TRACE=<file>` the trace gets three more row types:

- `m,<end ns>,<waiter tid>,<waiter thread>,<waiter site>,<holder tid>,<holder thread>,<holder lock site>,<held when
  the wait began ns>,<waited ns>`: a guest mutex wait of `GOW3_WAIT_TRACE_MIN_US` (500) or more. Guest mutexes
  record their holder (thread id and name, the guest call site of the lock, the time); a lock tries first and reads
  the holder only when that fails. Condition waits release and retake the hold (site `cond_wait`); recursive depth
  is counted.
- `h,<end ns>,<tid>,<thread>,<lock site>,<unlock site>,<held ns>`: every hold of that length or more.
- `s,<ns>,<holder tid>,<pc>,<caller>...` (Windows): while a thread waits on a held mutex, a sampler thread
  (ABOVE_NORMAL priority, a high-resolution timer asking 200 us, ~0.5 ms in practice) suspends the holder,
  reads its registers and copies up to 16 KB of its stack (bounded by `VirtualQuery`; syscalls only, no lock of
  ours or the loader's while it is suspended), resumes it, then unwinds the copy: `RtlVirtualUnwind` with rsp / rbp
  moved into the copy (exe and DLL frames, exact), the rbp chain for game code (each return address must follow a
  call), and, if that gives fewer than 3 frames, return addresses found by scanning the copy, marked `~` (can be
  stale). Frames are `exe+0x<rva>`, `guest+0x<offset>` or `<dll>+0x<offset>`.
- An `exe,<path>,<base>` row after `wall` (Windows) names the exe for `llvm-symbolizer`.

`research/tools/mutex_holders.py <trace> [--site guest+0x6795a1]`: waits by site with their holders, the chosen
site's holders and lock sites, long holds, and the holder samples by module / pc / first exe frame / first game
frame (exe offsets named from the exe's debug info).

## Invariants

- Compiled out of the release build (the `GuestMutex` owner fields too); off in the dev build: one call per lock to
  a cached switch.
- The sampler must never take a lock (heap, loader, trace, stdio) between `SuspendThread` and `ResumeThread`: the
  holder may hold it, and the sampler would never resume it. Everything after resuming reads only the copy.
- Owner fields are written by the holder while it holds the lock and read by waiters without it (relaxed): a
  snapshot can name the previous holder if the lock changed hands at that moment.
- `wait_frames.py` reads only `w` rows, so the new rows do not disturb it.

## Interactions

- wait-trace: shares its file, threshold and writer (`runtime_wait_trace_line`); the trace's `w` rows are unchanged.
- The run is slower (a trylock and clock reads per lock, the sampler's suspensions): its frame times mean nothing.

## Evidence

- `mutex-test` (2026-11-16): a 20 ms hold waited on gives one `m` row naming the holder, one `h` row (20.0 ms), and
  ~40 samples, all unwound to the holder's function (`hold_20ms`) through ntdll / winpthread; a 2 ms timed condition
  wait gives no hold row. Three runs, all passed. `--handoff` and the semantics tests unchanged.
- First game run (2026-11-16 22:45, dev build, 4 min 49 s, 311 MB trace: 61,712 m, 284,273 h, 297,652 s rows; no crash or
  hang): RHIThread's lock holder named (SubmitGPUCommandsAsyncTaskThread) and its wait found (`sceKernelWaitEqueue`, the
  per-frame EOP wait); 92% of the samples unwound to the same three exe frames. Details: plan-work-queue.md, RHIThread row.

## History and pitfalls

- First version walked only the rbp chain, which gives the callers of the function that owns rbp, never that
  function: from inside ntdll it skipped exactly the port function that called it. A plain stack scan picked up a
  stale `mutex_lock` return address in 80% of samples. Hence the unwinder on a copy.
- A fixed 16 KB stack read fails near a thread's stack top: the copy is bounded by the stack's region.
