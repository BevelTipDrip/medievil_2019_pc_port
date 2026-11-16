# Compiler-level optimizations: what is left (research, 2026-11-16, no launch)

Parent: [plan-work-queue.md](plan-work-queue.md) "Compiler optimizations". Read-only research by one agent (the user
asked: unused instruction sets, other flags, OpenTuner). Builds into a scratch folder only. Tags: **[V]** verified
(build files, disassembly, a test compile), **[I]** inferred. Linux was studied earlier in
[study-deck-build-optimization.md](study-deck-build-optimization.md) and [impl-deck-build-sched-pool.md](impl-deck-build-sched-pool.md)
(cards `launcher/pgo-build`, `deck/s1-v3-build`). This adds the Windows side, the eboot scan, the clang test of
`GOW3_PGO` and the PC profile breakdown.

## What flags can reach

- **Not the game.** The eboot and the game's libc.prx run natively; there is no recompiler (README, `runtime_sse4a.c`)
  **[V]**. Flags change host code only: stage A / B, the recorder, prep workers, the runtime C, atrac9, imgui.
- **Much of the host hot time is in code we don't compile** (PC WPR profile 2026-10-25,
  `research-data/wpr/cpu-osc-1-functions.txt`): stage B ucrtbase `memcpy` 3.3%, `memcmp` 2.7%, ntdll SRW locks ~6%;
  stage A `memcpy` 8%; guest threads mostly game code (69-77%), libwinpthread, the NVIDIA driver.
- What we compile is flat and branchy (no dominant loop; the Deck `perf` report agrees): code layout (PGO) matters
  more than instruction sets.

## Today's flags (Windows) [V]

- RelWithDebInfo = `-O2 -g -DNDEBUG`, ThinLTO, `-fno-omit-frame-pointer`; no `-march` / `-mtune` (x86-64, generic).
  The runtime C adds `-std=c11 -Wall -Wextra -Werror` (and `-DNDEBUG`: asserts off on Windows, `-UNDEBUG` on Linux).
- atrac9: `-O2`, **LTO off** (`gpu/CMakeLists.txt` `INTERPROCEDURAL_OPTIMIZATION OFF`).
- MSYS2 DLLs (SDL3, FFmpeg, libxxhash, Zydis, libc++, libwinpthread) and ucrtbase are baseline and not ours.
- Linker: lld (`ld.lld -m i386pep`); `--gc-sections` / `--icf` off by default under MinGW.
- libc++ hardening NONE (no hidden bounds checks).
- Linux has `GOW3_CPU_LEVEL=v3` (`-march=x86-64-v3 -mtune=znver2 -fno-math-errno -ffp-contract=off`, lld); CMake
  stops with `FATAL_ERROR` on WIN32 (a scope choice, not a limit).
- The user's CPU: Ryzen 7 7800X3D (Zen 4, AVX-512).

## Findings

1. **`GOW3_PGO` has never worked with clang** [V]. Inherited from the Bloodborne port (GCC: LTO+PGO -4.8% per draw
   vs LTO alone -1.8%, `docs/parallel_gpu.md`). With clang 23.1.3: `use` fails (`unknown argument
   -fprofile-partial-training`); `generate` links (compiler-rt has a `__gcov_dump` symbol), but `__gcov_dump` + `_exit`
   writes no `.profraw`, and the game often ends through `_exit`, so a training run would silently write nothing.
   `__llvm_profile_write_file()` + `__llvm_profile_reset_counters()` works; `llvm-profdata` and the profile runtime
   are installed; a full generate -> merge -> `-fprofile-use=x.profdata` round trip worked in the scratch folder.
   Windows `build.sh` hard-codes `-DGOW3_PGO=off`. Fix: a clang branch in CMake (`-fprofile-use=<dir>/medievil.profdata
   -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled -Wno-profile-instr-missing`, reaching the `-Werror`
   runtime C), the writer in `gow3gpu.cpp:279-306` on `__llvm_profile_*`, build.sh detecting `.profdata`. lld's
   call-graph ordering (`/call-graph-profile-sort`, on by default for COFF) gets real weights only with PGO [I].
2. **x86-64-v3 drops almost nobody** [V]: a scan of the eboot's code (23.2M instructions) finds 1.4M VEX instructions,
   BMI1 (`andn` 4,953, `bextr` 5,834, `blsi` 7,018), `lzcnt` 7,128, `movbe` 80, no FMA, no BMI2. Every CPU that runs the
   game has AVX + BMI1 + LZCNT: on Intel that is Haswell+, which is all v3; only AMD Piledriver / Steamroller (FX era)
   lack AVX2, and Piledriver also lacks MOVBE. (Answers the open item in the Deck study.)
3. **Where v3 helps our code** (mechanisms checked by test compiles; gains [I]): `common/bit_array.h:10-11`'s
   `__AVX2__` path (buffer-cache page bits) is compiled out on Windows today; fixed-size `memcmp ==` inlines up to 64
   bytes with v3 (128 with v4), stays a call at baseline; `floorf` etc. become `roundss` from v2. Unaffected:
   ucrtbase `memcpy` / `memcmp` (own dispatch), SRW locks, the driver, texture detiling (GPU shaders). XXH3 is in a
   baseline `libxxhash.dll` (could use `xxh_x86dispatch.c`), but it appears in no profile.
4. **atrac9 without LTO**: `Round`, `Clamp16`, `PeekInt`, `SignExtend32` are separate per-sample calls on the audio
   thread (atrac9 ~17% of 0.21 cores on the PC) [V for the calls, I for the gain].
5. **OpenTuner does not fit** [I]: it needs a cheap, repeatable, low-noise objective over tens to hundreds of
   evaluations. Here an evaluation is a ThinLTO rebuild plus a game launch with the user present, and launch-to-launch
   noise (3-7%, N-29) is larger than the 0-5% effects tuned. The offline benches (`flip-trace-bench`, `mutex-test
   --bench`, `rwlock-test --bench`) cover a small part of the frame and not stage B's code layout. No frame replay
   exists. A PM4 capture / replay harness (record N frames of command buffers and the guest memory they read, replay
   through `Liverpool` and the rasterizer, time per draw) would enable automated PGO training and regression checks;
   a large project. With ~4 real knobs (PGO, v3, -O3 under PGO, icf / gc-sections), manual A/Bs are simpler.

## Rejected

`-ffast-math` (changes the shader recompiler's constant folding and the pacing / scaling math; stage B is not float
code); `-fomit-frame-pointer` (the Windows fault dump walks the rbp chain, `probe.c:251-259`; the Linux sampler too);
full LTO (ThinLTO vs none was within noise, N-29; full links far slower); AutoFDO / CSSPGO / Propeller (no LBR on the
Deck, no clang AutoFDO tooling on Windows); a malloc replacement (NT heap <= 0.4%); `target_clones` (many small hot
functions: an indirect call and no inlining); static libc++ (removes only the import thunk); `--icf=all` (breaks
function-pointer identity).

## Ranked candidates

| # | Candidate | Platform | Expected [I] | Effort | Risk | Measurement |
|---|---|---|---|---|---|---|
| 1 | Clang IR PGO: fix `GOW3_PGO`, train, `use` (call-graph sort comes with it) | Win + Deck | stage B -2 to -5% PC, -3 to -7% Deck | M, then S per retrain | low (build only; the instrumented build changes timing; profiles go stale) | a training session per platform (labelled), then `use` vs no PGO, same build type: stage B us/draw (stats run) and a separate lows run |
| 2 | x86-64-v3 by default (Deck AppImage first; Windows: unblock CMake, CPU gate in run.py / gpu-check / `play.c`, since `-march` on the ThinLTO link can run AVX2 in constructors before `main`) | Deck, then Win | 0-2% stage B, a little less CPU power on the Deck | S Deck (flavour exists, never A/B'd), S-M Win | low (`-ffp-contract=off` keeps float results) | one launch each, same spot |
| 3 | ThinLTO for atrac9 | both | small on PC, more on the Deck's 15 W | S | low (card: re-test audio) | offline at9 decode bench (needs a game file), then a listen check |
| 4 | BOLT after PGO | Deck | 2-5% on top | M | medium (dumps, `dladdr`, retrain per release) | training + one A/B |
| 5 | lld for the Deck default build | Deck | 0-1% | S | low | with 2 or 4 |
| 6 | `-Wl,--gc-sections,--icf=safe` | Win | 0-1%, smaller `.text` | S | low | binary diff; bundle with 1 |
| 7 | `-O3` under PGO | both | -1 to +2% | S | low | one A/B after 1 |
| 8 | XXH3 inline / x86 dispatch | both | < 0.5% | S-M | low | only if a profile shows XXH3 |
| 9 | `-march=znver4` personal build | the user's PC | < 1% | S | never for release | its own build type |

All A/Bs: one change per launch, same build type (release vs release: LANDMINES 8c), lows and worst frames.

## Scratch material

Agent scratch (session folder, not kept): `scan/isa_scan.py` (capstone eboot scan), `pgo2.cpp` / `pgo3.cpp` (the
clang PGO tests), `mc.c` (memcmp inlining per `-march`).
