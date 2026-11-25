---
name: native-diagnostics
description: The MediEvil port's own logging, trace and stats switches (GOW3_* variables, medievil.ini keys) - which exist, what each writes and where, whether it is periodic or costs anything, and how to turn one on safely for a run. Use before enabling any diagnostic, before judging performance from a run, and when a log line needs explaining.
---

# Native diagnostics (the runtime's own)

## Find, don't remember

Every diagnostic has a card. The catalogue is the "diagnostics, tracing, capture, stats" section of
`research/patches/INDEX.md` (status `diagnostic`). From a switch or a log line:

```
py -3.14 research/tools/patchmap.py switch GOW3_FLIP_TRACE     # its card, default, code sites
py -3.14 research/tools/patchmap.py find "Copy fault"           # search the cards by a log text
py -3.14 research/tools/patchmap.py file gpu/shim/gow3_flip_trace.h
```

Read the card's Invariants before using one: they say what it costs while on and off.

## Rules

- **A run with periodic log lines is not a run to judge performance by** (the user, 2026-11-15; the mouse
  diagnostics' lines caused frame-time waves). Before a performance run, check the last run's log for repeating
  lines: `sed -E 's/[0-9]+(\.[0-9]+)?/N/g' user/last_run.log | cut -c1-60 | sort | uniq -c | sort -rn | head`.
- **A switched-off diagnostic must cost nothing**: read its switch once into a constant; no atomics, clock reads,
  thread-local writes or string building on hot paths while off (work queue: "Logging and trace code with a cost
  when off"). New diagnostics follow that.
- Turn switches on in `extra-env.txt` (one `NAME=value` per line, `#` comments; LF or CRLF as found, append
  byte-safely, back up first with `cp -p`). `medievil.ini` is CRLF: edit it with a Python bytes replace that asserts
  exactly one match, never Git Bash `sed -i`; diff against the backup afterwards. Re-read both before each launch
  (the user edits them; the game's menu writes the ini too).
- The launcher writes its in-memory settings back on Play and on close: edit the ini only while the launcher is
  closed (or close and reopen it), else the edit is lost.

## The ones used most

| Switch / key | What it gives | Where | Periodic / cost |
|---|---|---|---|
| `present_frame_trace=1` (ini; `GOW3_FLIP_TRACE`) | every flip's stages with ns; `start` and `wall` rows for alignment | `user/logs/flip-traces/flip-trace-<date-time>.csv` (a new file each start, newest 60 kept) | per frame (~8 rows) kept in memory, written once at exit (a killed process writes nothing); not the FPS log; tools `flip_*.py` |
| `GOW3_FPS_LOG=0` | silences the periodic `Perf:` FPS line | log | keep 0 for perf runs |
| `GOW3_FRAME_STATS=1` | 5 s stats blocks (uploads, faults, region cache, backing copies, fences ...) | log | **periodic**: diagnostic runs only |
| `GOW3_COPY_FAULT_LOG` (dev build only: default on, `0` off; not in the release build since P5) | a guest fault inside a host copy, before / after handling | log | event only (first 20, then powers of two); off = no per-copy work |
| `GOW3_WAIT_SITES` | every guest wait by call site, requested vs actual time | log | see card `tooling/wait-sites` |
| `GOW3_MOUSE_LOG=1` (dev build only since P5) | mouse read / look diagnostics | log | **periodic**, caused frame-time waves |
| AvPlayer trace (always on) | movie player calls and events | log | events only |
| Save trace (always on) | every save call with the guest caller | log | events only |
| `GOW3_CRASH_DUMP` (`0` off, `full`) | a minidump on a host fault | `user/crashdumps/` | on crash only; "Crash dump: type 0x1a65 failed" then 0x1065 written is normal |

Logs: `user/last_run.log` (the run in progress or last), `user/last_run.1.log` (the one before), and every
finished run archived in `user/logs/runs/run-<time>[-v<ver>].log` (newest 200, 500 MB cap). The header names the
build (`Build: medievil-runtime.exe <mtime>, <size>; source <commit> (<branch>)`): the branch is git's current
branch at launch, not proof of which exe ran; the exe's mtime and size are.

## Reading crash lines

`Fault: read access to <addr> at guest offset 0x... (thread N)` + `#0 guest offset ...` frames: a fault in game
code (offsets into the eboot). `LowLevelFatalError ... Ran out of memory` / `PS4 OOM:` is UE4's own error (e.g. the
512 MB framebuffer pool at 4K 200%). `GameThread timed out waiting for RenderThread after 120.00 secs` is UE4's
watchdog ending a hang: the render side was stuck for 2 minutes (see the hang-crash-capture skill).
