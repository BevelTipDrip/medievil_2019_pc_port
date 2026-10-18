# PR: Windows: write a minidump when the runtime reports a fatal fault

Status: draft (lab change L1, `b01661e` on `lab`; not proposed). Branch: (later) `feat/crash-dump` from `main`.
Plan: [plan-baseline-issues.md](plan-baseline-issues.md) C1. Checklist: [pr-checklist.md](pr-checklist.md).

## What

A fatal fault on Windows ends the process from inside the runtime's first-chance handler: `vectored_fault` →
`fatal_exception` prints a report and calls `TerminateProcess` (`src/probe.c`, `fatal_exception`). Windows Error
Reporting never sees the exception, so no dump exists. Rare, timing-dependent crashes (here: a null read in the NVIDIA
driver under a recorded draw, run 2, not reproduced in run 3) cannot be examined beyond the printed report.

## How

`write_crash_dump(info)` runs after the report is printed and flushed, before `TerminateProcess`. It loads
`dbghelp.dll` at run time and calls `MiniDumpWriteDump` with the exception information into
`<user dir>\crashdumps\medievil-<date>-<time>-<pid>.dmp`, then prints the path (or why it could not).
Default dump type: private read-write memory (host heaps and stacks), data segments, handles, thread info, full memory
info, unloaded modules, indirectly referenced memory. Guest memory is a mapped section view, so it is not included.
`GOW3_CRASH_DUMP=0` turns it off; `GOW3_CRASH_DUMP=full` adds all memory.

## How it fits in

| File | Change | Why |
|---|---|---|
| `src/probe.c` | `#include <dbghelp.h>` (types only); `crash_dump_dir` + `write_crash_dump()` before `fatal_exception`; one call in `fatal_exception`; dir set in `main` next to `AddVectoredExceptionHandler` from the existing `--user` option | Same place as the existing report; `dbghelp` loaded like `GetThreadDescription` already is, so no link change. Windows-only code path (inside `#ifdef _WIN32`). |

Existing behaviour unchanged: same report, same exit code 3. Linux is untouched.

## Proof

| Check | Result | Evidence |
|---|---|---|
| Build (Windows, CLANG64) | clean, no new warnings | `out/gpu-pdb/build.log`, 2026-10-18 02:49 |
| Injected fault (cdb: break at the line after `AddVectoredExceptionHandler`, `rip=0`) | report + `Crash dump: ...medievil-20261018-025025-41252.dmp` (193 MB), exit 3 | console output 2026-10-18 02:50 |
| Dump opens (cdb `-z`, `.ecxr`) | faulting context restored (`rip=0`) | |
| Real crash in game | not yet | (to add) |
| `build.sh --test` | not run yet | |

## Risks and limits

- Writing a dump in-process from the faulting thread while other threads run is not guaranteed safe (a lock held by a
  crashed thread can block it). A hang there would replace an exit; acceptable for diagnostics, and `GOW3_CRASH_DUMP=0`
  turns it off. An out-of-process writer would be the robust alternative.
- Dump size in game unknown (host heaps can be GBs); to measure on the first real crash.
- Disk use: dumps accumulate in `user\crashdumps`; no rotation.

## Attribution

Ours. Uses the Windows `dbghelp` API.
