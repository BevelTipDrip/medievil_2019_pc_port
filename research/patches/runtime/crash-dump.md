---
id: crash-dump
title: Minidump on any fatal fault, with fallbacks to smaller dump types and faults inside winpthreads
area: runtime
status: active
platforms: windows
default_windows: on
default_deck: n/a
origin: ours
class: G
commits: b01661e60c, 5247265690, 513ca7f1f4
files: src/probe.c
symbols: write_crash_dump, crash_dump_dir, fatal_exception, vectored_fault
switches: GOW3_CRASH_DUMP
tests: T-Z4, T-V120f
docs: fix-inventory.md#9-diagnostics-worth-taking, pr-crash-dump.md#how, plan-roadmap.md#progress, plan-baseline-issues.md#run-i2-profile-4k-2-2026-10-18-0347-0352-c1-again-first-profile, plan-baseline-issues.md#s1-silent-exit-with-code-5-at-startup-2026-10-18-1253-once
depends_on:
related: upstream-safe-gpu-mode
verified: 2026-11-07
---
## What it does

On Windows `vectored_fault` sends faults in guest code, the loader or the runtime to `fatal_exception`, and the
pre-existing `unhandled_fault` filter sends it the unhandled rest (the C1 driver fault in `nvoglv64.dll` of run
`i2-profile-4k-2` was reported that way [inferred]). `fatal_exception` prints the report and calls
`TerminateProcess` from inside the first-chance handler, so Windows Error Reporting never writes a dump.
`write_crash_dump` now runs between the report and the exit. It loads `dbghelp.dll` at run time and writes
`<user dir>\crashdumps\medievil-<date>-<time>-<pid>.dmp`. It tries four dump types, from largest to smallest, and
keeps the first that works (`5247265`). Since `513ca7f` a fault inside `libwinpthread-1.dll` also counts as ours
(S1), so it gets the report and a dump instead of going straight to WER.

## Invariants

- `write_crash_dump` runs after the report is printed and `fflush(NULL)`. A dump that hangs or fails must never cost
  the text report. The exit code stays 3.
- Order of `types[]`: (1) private read-write memory + full memory info + indirectly referenced memory; (2) indirectly
  referenced memory; (3) the base (data segments, handles, thread info, unloaded modules); (4) `MiniDumpNormal`.
  Before each try the file is truncated (`SetFilePointer` + `SetEndOfFile`). The file is deleted if every type fails.
- Guest memory is a mapped section view, so private read-write memory leaves it out; only the pages that stack
  values point at can come in through indirectly referenced memory (types 1 and 2). Only `GOW3_CRASH_DUMP=full` adds
  `MiniDumpWithFullMemory`, and only to the first type. Any value other than `0` or `full` is the default.
- `crash_dump_dir` is set in `main` before `AddVectoredExceptionHandler`. An empty directory means no dump.
- `fatal_reported` lets only the first faulting thread report. A second fatal fault sleeps forever, so two dumps never
  race on one file.
- The winpthreads rule sits after `gow3gpu_handle_fault` (write-protected guest pages) and `runtime_fault_recover`
  (speculative reads) in `vectored_fault`, so those faults are resolved first even inside winpthreads. Moving it
  above them would make a tracked-page write a fatal crash. It covers only
  the fault codes `vectored_fault` lists (access violation, illegal / privileged instruction, stack overflow, divide
  by zero, misalignment).
- The winpthreads rule assumes winpthreads raises no exceptions of its own (class C: true for the MSYS2 CLANG64
  build). A library that handles its own access violations must not be added to `ours`.

## Interactions

- upstream-safe-gpu-mode: `fatal_exception` calls `gow3gpu_dump_recording_barriers` / `gow3gpu_dump_guest_writes`
  before the dump, so the barrier batch is in the log as well as in the dump.
- Linux and the Deck: `probe.c`'s signal handler prints a report without a dump. Use systemd-coredump
  (`coredumpctl`) there (plan-linux-deck.md).
- `GOW3_STRICT_HANDLES=1` sends invalid-handle exceptions to `fatal_exception` too, so they get a dump as well.

## Evidence

- Injected `rip=0` fault under cdb (2026-10-18): the report, a 193 MB dump, exit 3. The dump opens with the faulting
  context (`.ecxr`). Not a crash in game.
- In game the first type fails every time: `type 0x1a65 failed (0x80070057)` (E_INVALIDARG) or `(0x8007012b)`. The
  fallback then writes `type 0x1065`: `data/t-z4-crash-pthread.log:295` (T-Z4, 2026-10-21),
  `data/t-v120f-gsync-off.log:188` (T-V120f), `data/t-o2-occlusion.log:850`. Before `5247265` the run
  `i2-profile-4k-2` left a 0-byte file.
- No S1 fault has been written up with a dump since `513ca7f` (plan-work-queue.md: S1 still open).

## History and pitfalls

- plan-roadmap.md calls this patch L1; plan-baseline-issues.md (run `i2-profile-4k-2`) calls the fallback L1b. Those IDs are not fix-inventory's L1 / L2 or the
  Deck plan's L1-L7.
- The memory-walking types fail in game, likely because of the placeholder reservations and mapped views in the
  address space. A test outside the game does not show this. Check a real crash log after any change to the type list.
- The dump is written in-process from the faulting thread while the other threads run. A lock held by a crashed
  thread could hang it (pr-crash-dump.md "Risks"). `GOW3_CRASH_DUMP=0` turns it off. Dumps accumulate in
  `user\crashdumps` with no rotation.
- Draft upstream PR: pr-crash-dump.md (not proposed).
