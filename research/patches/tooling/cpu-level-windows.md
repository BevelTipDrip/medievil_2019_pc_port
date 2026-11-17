---
id: cpu-level-windows
title: x86-64-v3 (AVX2) and x86-64-v4 (AVX-512) release exes on Windows (GOW3_CPU_LEVEL=v3|v4), beside the baseline exe
area: tooling
status: active
platforms: windows
default_windows: off
default_deck: off
origin: ours
class: G
commits:
files: gpu/CMakeLists.txt, build.sh, src/probe.c, launcher/medievil_launcher.py
symbols: GOW3_CPU_LEVEL, GOW3_CPU_LEVEL_NAME, extra_env_probe, run_log_header
switches: GOW3_CPU_LEVEL, GOW3_PROBE
tests: launcher-controls
docs: study-compiler-optimizations.md, plan-work-queue.md
depends_on: dev-tools-build
related: S1-v3-build, clang-pgo, thinlto
checked: 2026-11-16
---
## What it does

Candidate 2 of study-compiler-optimizations.md for the PC (the user, 2026-11-16: AVX2 / AVX-512 with a CPU check at
start). Build-only so far: two extra release exes for an A/B; nothing picks them by CPU yet.

- `gpu/CMakeLists.txt`: on WIN32, `GOW3_CPU_LEVEL=v3|v4` compiles and links every target with
  `-march=x86-64-v3|v4 -mtune=generic -fno-math-errno -ffp-contract=off` (was a `FATAL_ERROR`). Linux keeps its v3
  flavour (`-mtune=znver2`, lld) unchanged. Any level defines `GOW3_CPU_LEVEL_NAME="x86-64-<level>"`.
- `build.sh` (Windows, release build only): `out/gpu-release-<level>` -> `out/medievil-runtime-<level>.exe`, log
  `out/gpu-build-release-<level>.log`; `out/medievil-runtime.exe` stays the baseline.
- `src/probe.c`: the runtime's first line adds `, x86-64-v3` / `, x86-64-v4`.
- Launcher `run_log_header`: the `Build:` line names the exe `extra-env.txt`'s `GOW3_PROBE` starts
  (`extra_env_probe`, "(extra-env.txt)"); before, it named `out/medievil-runtime.exe` for such A/B runs.

What changes in our code (not the game's, which runs natively): AVX2 / BMI2 instruction selection, fixed-size
`memcmp ==` inlined (up to 64 bytes v3, 128 v4), `roundss` for `floorf` etc., and `common/bit_array.h`'s `__AVX2__`
path (buffer-cache page bits), never compiled on Windows before. Not changed: ucrtbase `memcpy` / `memcmp` (own
dispatch), SRW locks, the driver.

## Invariants

- The exe cannot test the CPU itself: AVX2 code can run in static constructors before `main` (ThinLTO with
  `-march` on every unit). Whatever starts it must check first (run.py / the launcher if this becomes a default).
- `-ffp-contract=off` must stay: without it a*b+c becomes FMA on v3 and float results (shader recompiler constant
  folding, pacing and scaling math) differ from the baseline build.
- A v3 / v4 build is a build type of its own for A/Bs (the runtime's first line names it).

## Interactions

- S1-v3-build (Deck): the same idea for Linux; the user tested it on the Deck and it was worse, before the recent
  CPU-side fixes; to re-test.
- clang-pgo: off; v3 / v4 are measured without PGO.

## Evidence

- Expected 0-2% (study-compiler-optimizations.md): wider SIMD speeds up loops on one thread, not waits between threads.
- Fix built (2026-11-16 21:38 / 21:39, the same 6 old warnings each). `llvm-objdump` of `.text` (baseline / v3 / v4):
  VEX or EVEX instructions 0 / 69,927 / 75,721; `%ymm` 0 / 13,160 / 5,217; `%zmm` 0 / 0 / 12,924 (clang uses 512-bit
  vectors for v4); mask registers 0 / 0 / 5,407; BMI2 35 / 1,183 / 1,177; **FMA 0 in all three**. 136.10 / 136.15 /
  136.21 MB.
- Not yet measured. A/B plan: baseline, v3, v3, baseline (run order balanced), each from the same Graveyard save; v4
  only if v3 shows a difference.

## History and pitfalls

- Until 2026-11-16 CMake stopped with `FATAL_ERROR` for `GOW3_CPU_LEVEL=v3` on WIN32 (a scope choice).
