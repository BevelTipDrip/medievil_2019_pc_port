---
id: stdio-one-write-per-line
title: "Windows: formatted stdout / stderr lines in one write each (link-wrapped printf families; streams stay unbuffered)"
area: runtime
status: active
platforms: windows
default_windows: on
default_deck: n/a
origin: ours
class: S
commits: 6352211
files: src/stdio_lines_win.c, gpu/CMakeLists.txt
symbols: __wrap_printf, __wrap_fprintf, __wrap_vprintf, __wrap_vfprintf, __wrap___mingw_printf, __wrap___mingw_fprintf, __wrap___mingw_vprintf, __wrap___mingw_vfprintf
switches:
tests:
docs: study-perf-deepdive-20261123.md, plan-work-queue.md (P6)
depends_on:
related: quiet-log, dev-tools-build
---
## What it does

`main()` (src/probe.c) makes stdout unbuffered: crash lines must reach the launcher at once, and stdout and stderr
share the launcher's pipe in order. On an unbuffered stream both printf implementations in the exe write the
formatted text one character at a time:
- UCRT's `printf` / `fprintf` / `vprintf` / `vfprintf` (C++ code: the GPU log's `fprintf(stderr)`, `std::printf`;
  C files without `_GNU_SOURCE`; `__USE_MINGW_ANSI_STDIO` is 0 for both);
- mingw's `__mingw_printf` / `__mingw_fprintf` / `__mingw_vprintf` / `__mingw_vfprintf` (runtime C files with
  `_GNU_SOURCE`, where `printf` is an asm alias to them).

The link wraps all eight (`-Wl,--wrap=...`, `medievil-runtime` only). For stdout / stderr a wrapper formats the line
into a 2 KB stack buffer (heap when longer) with the same family's formatter (`vsnprintf` / `__mingw_vsnprintf`) and
writes it with one `fwrite`. Every stream goes this way (files too: one `fwrite` into the stream's buffer). The
streams stay unbuffered.

## Invariants

- Same bytes as before (checked: md5 of 5,000 lines through both families, wrapped and not).
- Crash lines, order between stdout and stderr: as before (no buffering added).
- A line is written whole: two threads printing at once no longer interleave mid-line.
- `puts` / `fputs` / `fwrite` already wrote once per call (measured); not wrapped.
- Linux / Deck: not built (glibc writes once per call).

## Evidence

Measured 2026-11-24 (scratch `p6/stdio_test.c`, `stdio_calls.c`, stdout on a pipe read like the launcher's):
- unbuffered `printf` / `fprintf`, both families: 60-70 us a line, ~35 writes per 88-byte line;
- `puts`, `fputs`, `fwrite`, or `vsnprintf` + one `fwrite`: 1.2-1.6 us a line;
- with the wrappers, linked with LTO and lld like the runtime: 1.3-1.6 us a line, identical output.
- Release exe after the build: 0 calls to the unwrapped functions, 564 to the wrappers.

Where it shows in release: each save (~50 save-trace lines, ~3 ms on the game thread), loads and movie events, a GPU
warning on a GPU thread (~70 us each). The dev build's periodic diagnostics paid it on every line.

## History and pitfalls

- **Crash in the first version (`6352211`, found 2026-11-25):** other streams went to `__real___mingw_vfprintf` /
  `__real_vfprintf`. In the game's LTO link lld resolved that back into the wrapper: its "other stream" branch
  restored its registers and jumped into its own body (disassembly of `__wrap___mingw_vfprintf`). The first
  `fprintf` to a file crashed: the wait trace's first row (`GOW3_WAIT_TRACE`, dev run, SceFiosIO thread, fault
  in `__mingw_pformat` reading 0xffffffffffffffff); the flip trace's writer would have too. Every run until then
  printed to the console only; the first test programs wrote stdout only, and a small LTO program with the old
  wrapper did not reproduce it. Fixed by having no `__real_*` call at all. Check after any change here: both exes'
  wrappers call only the formatters, `fwrite`, `malloc` / `free` and the stdout lookup, and no `jmp` follows an
  epilogue (`llvm-objdump --disassemble-symbols=__wrap_...`); test programs for stdout, stderr and a buffered file
  in both families, LTO and the same `--wrap` flags, byte-identical to unwrapped builds.

- Defining `__USE_MINGW_ANSI_STDIO=0` alone does not help: UCRT's printf on an unbuffered stream is per character too.
