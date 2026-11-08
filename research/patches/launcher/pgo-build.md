---
id: pgo-build
title: PGO / BOLT / -march=znver2 builds (not built)
area: launcher
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits:
files: build.sh, gpu/CMakeLists.txt, gpu/shim/gow3gpu.cpp
symbols: GOW3_PGO, GOW3_PGO_DIR, GOW3_PGO_GENERATE, __gcov_dump, GOW3_CPU_LEVEL
switches:
tests: N-29
docs: study-deck-rescore-rejected.md#c-analysed-gated-or-ranked-never-built, study-deck-build-optimization.md#answer, study-deck-build-optimization.md#33-expected-gain-and-staleness, study-deck-build-optimization.md#7-bolt, study-deck-build-optimization.md#2-flags-one-by-one-for-a-branchy-pointer-chasing-thread-on-zen-2
depends_on:
related: thinlto, S1-v3-build, static-gpu-lib
checked: 2026-11-07
---
## What it does

Three build-level ideas that were analysed and never built or measured:
- **PGO** (rescore C14): clang IR PGO with an instrumented training build, then a "use" build with lld call-graph
  ordering. Steps S2 / S3 in study-deck-build-optimization.md.
- **BOLT** (S4 / S5): an instrumented binary, then a re-laid-out one.
- **`-march=znver2`** (C15): a Deck-only ISA build.
The inherited `GOW3_PGO` path (generate / use, `__gcov_dump` every 30 s under `GOW3_PGO_GENERATE`) is GCC-only:
`-fprofile-partial-training`, `*.gcda`, `__gcov_dump`. It was never tried with clang; expect it not to build or
not to write a usable profile. The Windows build passes `-DGOW3_PGO=off`, and on Linux `build.sh` uses a profile
only if `pgo/` holds `.gcda` files.

## Invariants

- Do not build `GOW3_PGO=use` with clang: its flags are GCC options and a `.gcda` profile is GCC's format. A Linux
  build picks `use` by itself when `pgo/` holds any `.gcda` file, so a stale GCC profile there changes the build.
  A clang PGO needs `__llvm_profile_write_file` in place of `__gcov_dump` (gow3gpu.cpp), plus
  llvm-profdata and `-fprofile-instr-use` (study-frame-cost: "Only after LTO pays").
- Profiles go stale per function: clang drops the profile of a changed function. A BOLT profile is tied to one
  binary's addresses, so every release needs a new training run.
- An instrumented build changes timing. Bloodborne's exposed a real race, so a crash there is a finding, not a PGO
  problem.
- `extra-env.txt` does not pass `LLVM_PROFILE_FILE` (prefix list in run.py). A Deck training run needs it allowed or
  set another way.

## Interactions

- thinlto: PGO was gated on LTO paying first. N-29 was neutral, so the gate never opened.
- S1-v3-build replaced C15: `-march=x86-64-v3 -mtune=znver2` gives the same code generation for the Deck without
  tying the binary to Zen 2 (opt-in `GOW3_CPU_LEVEL=v3`, with an AppRun CPU gate).
- static-gpu-lib (S0) comes first: BOLT's best-supported case is a non-PIE static executable.

## Evidence

- Not measured here. The only data is Bloodborne's (same code base, GCC, Linux, 3 runs each): LTO -1.8%, LTO + PGO
  -4.8% per draw on the GPU thread.
- Estimates for stage B on the Deck (inferred, study-deck-build-optimization.md): S3 PGO 3-7% (0.4-1.0 ms a frame),
  S5 BOLT 2-5% on top, S1 0-2%. All of S0 + S1 + S3 + S5 together: about 6-15% of stage B.
  Rescore C14: -0.3 to -0.5 ms; C15: 0-3%, "never measured".

## History and pitfalls

- N-29: "PGO stays out (needs clang profile code, study item 6)".
- `rejected` here means "not built, gated", not "measured and dropped": the README has no status for that.
  study-deck-rescore-rejected.md lists C14 as "Open" and C15 as "Never measured", and study-deck-build-optimization
  ranks S2-S5 as a pending plan. The `GOW3_PGO` CMake / build.sh plumbing is upstream's (already in `upstream/main`).
- What would make it worth building: stage B still the Deck's wall after the S0 / S1 measurements, and the user's
  go for two training launches (play sessions, labelled `deck-pgo-train-1` / `deck-bolt-train-1`), plus one
  measurement launch each. Judge by stage B us per draw and busy %, not FPS alone (15 W shared budget).
- Ruled out in the same study without a launch: full LTO instead of ThinLTO (0-1%, much slower link), AutoFDO /
  CSSPGO (Zen 2 has no LBR), `-fomit-frame-pointer` (the watchdog and the sampler walk `rbp`).
