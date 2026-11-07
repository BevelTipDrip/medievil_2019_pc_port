---
id: S1-v3-build
title: S1: opt-in x86-64-v3 Linux build flavour (GOW3_CPU_LEVEL=v3) and its AppRun CPU check
area: deck
status: active
platforms: linux
default_windows: n/a
default_deck: off
origin: ours
class: G
commits: c2ab154
files: gpu/CMakeLists.txt, build.sh, packaging/linux/AppRun, packaging/linux/appimage.sh
symbols: GOW3_CPU_LEVEL, cpu_level
switches:
tests:
docs: impl-deck-build-sched-pool.md#s1-the-x86-64-v3-build-flavour, study-deck-build-optimization.md#2-flags-one-by-one-for-a-branchy-pointer-chasing-thread-on-zen-2, study-deck-build-optimization.md#5-keeping-one-portable-appimage
depends_on: static-gpu-lib
related: pgo-build, thinlto, deck-preset
---
## What it does

`GOW3_CPU_LEVEL=v3 bash build.sh` (CMake cache `GOW3_CPU_LEVEL`) builds every CMake target (library, third-party
libraries built with it, runtime C, tests) with `-march=x86-64-v3 -mtune=znver2 -fno-math-errno -ffp-contract=off`
and links with lld, in its own tree `out/gpu-v3`. `build.sh` writes `out/cpu-level`; `appimage.sh` then writes
`dist/MediEvil-x86_64-v3.AppImage` with a marker `usr/lib/compat/cpu-level`, and AppRun refuses, with a message, a CPU
lacking `avx avx2 bmi1 bmi2 fma f16c movbe abm xsave` instead of dying with SIGILL. Unset: unchanged. A build
option, so it is not in `switches.tsv`.

## Invariants

- `-ffp-contract=off` must stay: without it clang contracts `a*b+c` into FMA on a v3 target, and host float results
  (shader recompiler constant folding, pacing and scaling math) differ from the default build. The v3 binary has 0
  `vfmadd*` (checked).
- v3 requires the static GPU library and `ld.lld` (`build.sh` stops otherwise); on WIN32 CMake stops (FATAL_ERROR).
- The flags are `add_compile_options` / `add_link_options` before any target, so every CMake target gets them; the
  link repeats `-march` so ThinLTO's code generator targets the same CPU. System libraries and
  `medievil-gpu-check` stay baseline.
- AppRun's gate runs only when the marker exists and `/proc/cpuinfo` is readable; the default AppImage has neither
  marker nor check. A default build must delete `out/cpu-level` (it does), or a baseline binary is packaged as v3.

## Interactions

- `static-gpu-lib`: required.
- `pgo-build` / `thinlto`: the other build-level items of study-deck-build-optimization.md; S1 is the only one built.
- `deck-preset`: left out of it (plan, "Deck Optimized preset": "Left out: ... the v3 build").

## Evidence

- Binary (impl S1, `a863482`): 71,646 VEX instructions, 12,401 using `%ymm`, BMI1/2, `lzcnt` and `movbe` used, 0 FMA,
  0 `pdep` / `pext`; instructions +3%, `.text` slightly smaller.
- Expected 0-2% of stage B (study table), inside one launch's noise. A v3 build was used for a 10 s `perf record`
  on the Deck (`research-data/perf/deck-v3-perf-report.txt`, impl-stage-a-decode.md), but **no v3 vs baseline A/B is
  recorded**. Not verified in game.

## History and pitfalls

- Status: built, opt-in, unmeasured. Later Deck builds were made with `GOW3_CPU_LEVEL=` (baseline) explicitly.
- The study recommended one v3 binary for everyone behind an AppRun gate; today v3 is a second artifact (the user's
  call, open).
