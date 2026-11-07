---
id: thinlto
title: ThinLTO on by default (GOW3_LTO=ON in build.sh), plus toggle A/B and per-draw tools
area: launcher
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: ca5636f7ed
files: build.sh, research/tools/build-pdb.sh, gpu/CMakeLists.txt, research/tools/sections_per_draw.py, research/tools/toggle-ab.ps1
symbols: GOW3_LTO, CMAKE_INTERPROCEDURAL_OPTIMIZATION, check_ipo_supported, min_draws
switches:
tests: N-29
docs: plan-frame-pacing.md#n-29-thinlto-on-by-default-2026-10-30-launched-by-claude-with-the-users-go-t-lto-a1log-t-lto-blog-same-code-a855b18, study-deck-build-optimization.md#1-what-the-build-does-today-code-bin
depends_on:
related: pgo-build, static-gpu-lib, S1-v3-build, bulk-descriptor-copy
---
## What it does

`build.sh`'s Windows branch (MSYS2 CLANG64) and `research/tools/build-pdb.sh` now pass `-DGOW3_LTO=ON` by default
(`GOW3_LTO=OFF` turns it off). `gpu/CMakeLists.txt` then sets `CMAKE_INTERPROCEDURAL_OPTIMIZATION` after
`check_ipo_supported`. With clang that is `-flto=thin` on every compile line, linked by lld. The Linux branch already
defaulted to `GOW3_LTO=ON` upstream. With the Deck build's clang 19 it is ThinLTO too (319 `-flto=thin` lines,
study-deck-build-optimization.md), and since static-gpu-lib the runtime C joins the same ThinLTO unit. The commit
also added `sections_per_draw.py` (stage B microseconds per draw in heavy frames from a `GOW3_SECTIONS` log) and
`toggle-ab.ps1` (alternates one toggle-file bit every N seconds in one launch). A build variable, not a runtime
switch: nothing in the game reads it.

## Invariants

- Host code only. The game's own code is the PS4 binary, and no compiler flag touches it.
- `check_ipo_supported` failing must only warn ("LTO unavailable") and build without LTO. A toolchain without LTO
  support still builds, slower.
- Symbols keep resolving with llvm-nm / llvm-symbolizer under LTO (checked in N-29). The minidump and watchdog paths
  need that, and `build-pdb.sh` builds the same configuration plus CodeView for WinDbg.
- In the static build, `atrac9` (third-party C) is built with `INTERPROCEDURAL_OPTIMIZATION OFF`. The comment
  gives no reason. Re-test audio before turning it on.
- A per-draw comparison between builds needs the same spot and the same toggles. The N-29 noise band is a few
  percent: median 4-7% better, mean 3% worse in the same pair.

## Interactions

- pgo-build: PGO was held until LTO paid. N-29 was neutral, so PGO stays out (rescore C14).
- static-gpu-lib (Deck S0) extends LTO across the runtime C, and S1-v3-build adds `-march=x86-64-v3` + lld on top.
  Both change code generation, so compare them only against an LTO baseline.
- bulk-descriptor-copy's N-30 A/B was the first use of `toggle-ab.ps1`, on build `ca5636f` (LTO).

## Evidence

- N-29 (same code `a855b18`, LTO off vs on, heavy view, sections on): stage B per draw, median / mean. Packet
  2.62 / 2.66 us off vs 2.44 / 2.74 on. Draw section 2.07 / 2.15 vs 1.99 / 2.21. The user: "run-to-run variance,
  no downside to keeping it". The exe is 129 MB with LTO, 150 MB without.

## History and pitfalls

- t-lto-a1 is partly invalid: Claude built the next change during the run (20:06:07-44), and the heavy view fell to
  139-146 FPS for exactly that time (the user: "particularly bad for the first 30 seconds ... then it cleared up").
  Only frames after 20:06:45 count. The rule since then: no builds or trace exports while a run is in progress.
- Until `ca5636f` the Windows build had `GOW3_LTO=OFF` while the CMake default was ON, with no reason recorded
  (`0af6cd4`, study-frame-cost-2026-10-29.md). The third run (A2) was skipped.
- Bloodborne (GCC, Linux) measured LTO -1.8% per draw. That did not reproduce clearly here.
