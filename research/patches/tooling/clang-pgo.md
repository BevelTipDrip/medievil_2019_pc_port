---
id: clang-pgo
title: Clang PGO for the Windows release build (GOW3_BUILD=pgo-train, pgo-merge.sh, GOW3_PGO=use); off, the first profile made gameplay spikes and lows worse
area: tooling
status: rejected
platforms: windows
default_windows: off
default_deck: off
origin: ours
class: G
commits: ef5d9d2, d6bb436
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
  build's profiles do not merge). `release` uses `pgo/windows/medievil.profdata` only with `GOW3_PGO=use` (off by
  default since the A/B below; for a few hours on 2026-11-16 it was used whenever the file existed); `dev` never
  does. The build line says `PGO off|use|generate`.
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
- **A/B, confirmed: worse in gameplay** (2026-11-16 evening, 4K 75% DLSS, Reflex 237, flip trace on, logs off; release
  vs release; traces in `research-data/pgo-ab-20261116/`, not in git; graph `pgo-ab-frametimes.png`). 20 s windows
  grouped as in dev-tools-build. Heavy stretches (combat, busy views):

  | run | build | frames > 6 ms a minute | > 8 ms | worst | 0.1% low | average |
  |---|---|---|---|---|---|---|
  | 19:30 (dev-tools A/B) | no PGO | 10.1 | 0.4 | 12.6 ms | 157.7 | 226.5 |
  | pair 1, A (first) | no PGO | 163.7 | 0.9 | 13.3 ms | 131.3 | 222.7 |
  | pair 1, B | PGO | 38.9 | 1.5 | 17.2 ms | 133.0 | 226.0 |
  | pair 2, run 1 (first) | PGO | 46.9 | 1.0 | 14.3 ms | 137.3 | 226.6 |
  | pair 2, run 2 | no PGO | 1.3 | 0 | 7.3 ms | 172.3 | 225.6 |

  Pair 1's A is not clean: the save slot held the training session's Hilltop Mausoleum save, so that run started from
  another level (extra loads at 50-85 s) before the Graveyard; it also had waves of 6-8 ms frames at 150-230 s. Pair 2
  started both runs from the same Graveyard save (backup `user/savedata_backups_manual/pgo-ab-reverse-start-*`). The
  user, pair 2: the run without PGO had none of the 1-3 frame high spikes of the PGO run, at worst ~6.5 ms spikes in
  combat outside the Graveyard's first asset load; at the statue you hit to turn (bright lights, sparks) the 0.1% low
  fell to ~150 with PGO and stayed in the 190s without. Average unchanged; spikes and lows worse.
- **Profile shape** (`llvm-profdata show --detailed-summary`, `--topn`): 79 blocks (0.06%) hold 50% of all counts. The
  largest are thread loops: `Scheduler::RecorderThread` 5.3e10, `DrawPipe::Run` 2.0e10, `DrawPreparation::ScannerLoop`
  / `WorkerLoop` ~6e9: tens of millions of passes a second, most likely their wait / poll loops (not checked in the
  source), inflated further by the instrumented build being slower. Next come the per-draw paths (`HashRegisterPacket`,
  `ImageResource::Fetch`, `ApplyGraphicsRegisterPacket`, `BindBuffers`, ...) and atrac9 per sample. Loading and menu
  code does not dominate. Hot threshold (99% of counts) 2.2e7 per block, cold (99.9999%) ~980.
- **Likely mechanism** (inferred, not tested): PGO weighs code by how often it runs, not by its cost. Paths that run
  rarely or never in training (first use of an effect, pipeline, descriptor set or texture: the statue's sparks,
  combat effects) count as cold: lower inlining thresholds at cold call sites, profile-guided size optimization of cold
  code, laid out away from the hot code. Those rare paths are the 1-3 frame spikes, so the common case got no faster
  and the rare case got slower. A gameplay-only training would not change that by itself.
- Decision (the user, 2026-11-16): no PGO by default; record the results; move on to the x86-64-v3 / v4 builds. The
  PGO exe is kept as `out/medievil-runtime-pgo-e938b882.exe`, the profile stays in git (`d6bb436`).

## Retry ideas (the user would like PGO trained on gameplay only)

- Keep rare code from being treated as cold: e.g. LLVM's options for cold functions (`-pgo-cold-func-opt`, the PGSO
  switch), or a raised cold threshold. Check what clang 23 has before building.
- Trim the profile to gameplay: reset the counters after the level has loaded (a hotkey, or the first gameplay frame)
  and write before menus / loads, so menus, loads and start-up do not enter it; or merge only gameplay intervals
  (`llvm-profdata merge` takes weights per file).
- Keep the wait loops out of the weights: their counts depend on how long a thread waits, which the instrumented build
  changes (e.g. no instrumentation in the loop functions, `__attribute__((no_profile_instrument_function))`).
- Each retry: one training session plus a reverse-order A/B pair from the same save, judged by spikes and lows.

## History and pitfalls

- The inherited `__gcov_dump` writer linked under clang (compiler-rt has the symbol) and printed "PGO: profile
  written" without writing anything usable.
- An A/B run that starts from another save is not comparable: the training session saved into the slot the A/B
  loads (the game saves to the last-written slot), and pair 1's first run started from it. Start both runs of a pair
  from one save (back it up first, restore it if a run in between saves elsewhere).
- The run-log header's `Build:` line names `out/medievil-runtime.exe` even when `extra-env.txt`'s `GOW3_PROBE` starts
  another exe (the launcher writes it before run.py reads extra-env.txt). The runtime's first line is the reliable one.
