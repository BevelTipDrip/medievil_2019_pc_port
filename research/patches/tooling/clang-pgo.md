---
id: clang-pgo
title: Clang PGO for the Windows release build (GOW3_BUILD=pgo-train, research/tools/pgo-merge.sh, pgo/windows/medievil.profdata)
area: tooling
status: active
platforms: windows
default_windows: off
default_deck: off
origin: ours
class: G
commits:
files: gpu/CMakeLists.txt, gpu/shim/gow3gpu.cpp, build.sh, src/probe.c, research/tools/pgo-merge.sh, .gitignore, .gitattributes
symbols: GOW3_PGO, GOW3_PGO_DIR, GOW3_PGO_GENERATE, GOW3_PGO_PROFILE, StartProfileWriter, WriteProfile, DumpProfile, __llvm_profile_write_file, __llvm_profile_reset_counters
switches: GOW3_PGO, GOW3_BUILD, GOW3_PROBE
tests:
docs: study-compiler-optimizations.md, plan-work-queue.md
depends_on: dev-tools-build
related: pgo-build, thinlto, S1-v3-build
checked: 2026-11-16
---
## What it does

Clang IR profile-guided optimization for the Windows release build (study-compiler-optimizations.md finding 1,
candidate 1). The inherited `GOW3_PGO` path was GCC-only and never worked with clang (`use` stopped on
`-fprofile-partial-training`; `generate` linked but `__gcov_dump` wrote no `.profraw`, and the game ends through
`_exit` / `_Exit`, which skips clang's own write at exit).

- `gpu/CMakeLists.txt`: with clang, `GOW3_PGO=generate` compiles and links `-fprofile-generate=<GOW3_PGO_DIR>/raw
  -fprofile-update=atomic` (atomic: many threads run the same code); `use` reads `<GOW3_PGO_DIR>/medievil.profdata`
  (`-fprofile-use`, stops if the file is missing) with the stale / missing-profile warnings off
  (`-Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled -Wno-profile-instr-missing -Wno-backend-plugin`;
  the runtime C has `-Werror`). The profile's MD5 (12 hex digits) becomes `GOW3_PGO_PROFILE` on every command line,
  and the file is a configure dependency, so a new profile rebuilds everything. GCC keeps the old branch (Linux).
- `gow3gpu.cpp`: under `GOW3_PGO_GENERATE` a `gow3:pgo` thread writes the profile every 30 s
  (`__llvm_profile_write_file`, then `__llvm_profile_reset_counters`), and the window-close path writes it once more
  before `_Exit`. `-fprofile-generate=<dir>` names the file `default_<id>.profraw` and merges every write into it, so
  the file sums the session. A guest exit loses at most the last 30 s. The log shows `PGO: profile written (30 s)`.
- `build.sh` (Windows): `GOW3_BUILD=pgo-train` builds the release configuration (`GOW3_DEV_TOOLS=OFF`) instrumented in
  `out/gpu-pgo-train` -> `out/medievil-runtime-pgo-train.exe` and clears `pgo/windows/raw/` (an older instrumented
  build's profiles do not merge). `release` uses `pgo/windows/medievil.profdata` when it exists (`GOW3_PGO=off`:
  without); `dev` never does. The build line says `PGO off|use|generate`.
- `research/tools/pgo-merge.sh [windows]`: `llvm-profdata merge` of `raw/*.profraw` -> `medievil.profdata`.
- `src/probe.c`: the runtime's first line adds `, PGO profile 0x<md5>` or `, PGO training (instrumented)`.
- git: `pgo/windows/medievil.profdata` is tracked (binary); the rest of `pgo/` is ignored (the user, 2026-11-16).

Training session: `extra-env.txt` `GOW3_PROBE=C:\Users\Ty\MediEvil\medievil_2019_pc_port\out\medievil-runtime-pgo-train.exe`
(overrides the launcher's choice, run.py), then removed. A broad session (menus and saves, several levels, loads,
combat), wider than the A/B route.

## Invariants

- Train on the release configuration, since that is what uses the profile. A profile from another configuration
  matches fewer functions (code under `#if GOW3_DEV_TOOLS` differs), silently.
- A function changed after the training gets no profile (hash mismatch), compiled as without PGO: no crash, no wrong
  result, only the gain fades. Retrain before a release.
- The instrumented build is slower and changes timing: its frame times mean nothing; a crash there can be a real race
  (Bloodborne's training run found one).
- `gow3gpu_init` differs between the training and the use build (`StartProfileWriter`), so it is never profiled.

## Interactions

- pgo-build (rejected, "not built, gated"): this settles its PGO part for Windows with clang. BOLT and the Deck's
  clang / GCC PGO are still open there.
- dev-tools-build: a PGO release build is a build type of its own for A/Bs (the runtime's first line names it).
- thinlto: PGO data is attached before ThinLTO; lld's call-graph sort (COFF default) gets real weights from it.

## Evidence

- Mechanism checked before coding (2026-11-16, scratch program): generate -> `__llvm_profile_write_file` ->
  `llvm-profdata merge` -> `-fprofile-use` round trip with clang 23.1.3.
- Fix built (2026-11-16, `ef5d9d2`): the training exe has ~10,700 instrumented functions and the profile runtime;
  the same 6 old warnings.
- Training (2026-11-16, the user, 4K 75% DLSS): two sessions on the instrumented build, 4 min 48 s (the A/B route)
  and 17 min 49 s (a longer level), 10 + 36 writes, no failed write, no crash. Merged: 10,818 functions, 122,477
  blocks, total count 1.27e12 -> `pgo/windows/medievil.profdata` (MD5 prefix e938b8820701). The PGO release exe:
  141.5 MB (136.1 without), no counters, the same 6 warnings.
- Not yet measured: A/B `out/medievil-runtime-nopgo-cb36368.exe` vs the PGO release build.

## History and pitfalls

- The inherited `__gcov_dump` writer linked under clang (compiler-rt has the symbol) and printed "PGO: profile
  written" without writing anything usable.
