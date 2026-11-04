# Implemented: Deck build flavour, scheduling switches, guest pool prefill (2026-11-04)

Status: built (Windows and Linux), not run on the Deck or in game. Branch `deck-lab`: `c2ab154` (S1), `24e461b`
(#7 / #8), `a863482` (pool prefill / populate). Not done: the pool switches on Windows (not trivial there), the
study's optional eboot opcode scan for S1. The limits of what could be checked off the Deck are listed per item.
Inputs: [study-deck-build-optimization.md](study-deck-build-optimization.md) (S1),
[study-deck-apu-deep-dive.md](study-deck-apu-deep-dive.md) (ranked actions 7 and 8; sections 1.3, 4.1),
[study-deck-memory-budget.md](study-deck-memory-budget.md) (ranked item 1; section 2.5 (a) and (b)).

| Item | Change | Where | Switch | Default |
|---|---|---|---|---|
| S1 | `-march=x86-64-v3 -mtune=znver2 -fno-math-errno -ffp-contract=off` + lld for every CMake target; own tree `out/gpu-v3`; `dist/MediEvil-x86_64-v3.AppImage`; AppRun refuses an older CPU | `gpu/CMakeLists.txt`, `build.sh`, `packaging/linux/appimage.sh`, `packaging/linux/AppRun` | build: **`GOW3_CPU_LEVEL=v3`** (env for `build.sh`; CMake `GOW3_CPU_LEVEL`) | empty: baseline x86-64, default linker, `dist/MediEvil-x86_64.AppImage` |
| #7 | Stage B (`gow3:DrawRec`) gets its own core-scheduling cookie (`prctl(PR_SCHED_CORE)`) | `gpu/shim/gow3_linux_sched.{h,cpp}`, `vk_draw_pipe.h` | **`GOW3_STAGE_B_CORE_SCHED=1`** at start | off |
| #8 | Stage B and the vblank thread (`shadPS4:PresentThread`) at nice -10: `setpriority`, else rtkit `MakeThreadHighPriority`; vblank timer slack 1 ns | `gpu/shim/gow3_linux_sched.{h,cpp}`, `vk_draw_pipe.h`, `videoout/driver.cpp` | **`GOW3_THREAD_NICE=1`** (nice -10) or **`=-N`** (nice -N, -1 to -20) at start | off |
| Pool (a) | After a pool expand, a background thread `fallocate`s the first N MiB of the pool's physical pages in the guest memfd | `src/runtime_memory.c` | **`GOW3_POOL_PREFILL_MB=N`** at start | off (0) |
| Pool (b) | One `madvise(MADV_POPULATE_WRITE / _READ)` per pool commit run, outside the locks | `src/runtime_memory.c` | **`GOW3_POOL_POPULATE=1`** at start | off |
| Pool stats | `Guest pool faults (window): process page faults N minor, N major; pool commit populates N (N pages, minor faults too; ms, N failed); prefilled N MiB` | `src/runtime_memory.c` (`runtime_memory_lock_report`), `research/tools/deck_summary.py` | `GOW3_FRAME_STATS=1` (Linux) | - |

All switches are Linux only and read once at start; Windows builds and behaves as before (the sched functions are
empty there, the pool switches are compiled out). With every switch off the only difference is the new
`Guest pool faults` line under `GOW3_FRAME_STATS=1`. All keys start with `GOW3_`, so `run.py` forwards them from
`~/.local/share/medievil-pc/extra-env.txt` (`deck.sh env`); no launcher change.

## S1: the x86-64-v3 build flavour

What the study asked for (section 2, "S1"; section 5): one build with `-march=x86-64-v3 -mtune=znver2
-fno-math-errno` and lld, and an AppRun gate. What was done:

- **Targets:** `add_compile_options` / `add_link_options` at the top of `gpu/CMakeLists.txt`, before any target or
  `add_subdirectory`, so they reach the GPU library, the third-party libraries built with it (sirit, FSR-Vulkan,
  miniz when built), atrac9, the runtime C (`gow3_runtime_executable`) and the test executables. Not covered: the
  system's shared and static libraries (SDL3, FFmpeg, fmt, Zydis, xxHash: Debian packages, baseline) and
  `medievil-gpu-check` (built by `build.sh`'s own `$CC` line, kept baseline: it is the AppRun `--vulkan-info` helper).
  The study's static-only premise holds: `GOW3_CPU_LEVEL=v3` with `GOW3_STATIC_GPU=OFF` stops with a message.
- **The link** gets `-fuse-ld=lld` plus `-march` / `-mtune` again (ThinLTO's code generator runs at the link; the
  bitcode's per-function target attributes already carry the CPU, the link flags make the driver pass it too).
  `build.sh` checks `ld.lld` is installed. Confirmed in the binary: `.comment` has `Linker: Debian LLD 19.1.7`.
- **FMA contraction / float determinism:** the study does not discuss it. clang contracts `a*b+c` into an FMA by
  default (`-ffp-contract=on`) as soon as the target has FMA, which changes rounding in host float code: the shader
  recompiler's constant folding, pacing and scaling math, the upscalers' CPU-side matrices. `-ffp-contract=off` keeps
  every float result bit-identical to the default build, so a v3-vs-default difference is instruction selection and
  tuning only. Cost: none worth measuring (stage B is not float code). Confirmed: **0 `vfmadd`/`vfmsub`/`vfnmadd`**
  in the v3 binary.
- **`-fno-math-errno`:** `sqrt` and friends inline without the errno branch; nothing reads `errno` after a math call
  (study). `strtod` and the like are not affected.
- **Separate tree:** `out/gpu-v3` (the default stays `out/gpu`), so switching flavours does not rebuild the other
  from scratch. `build.sh` writes `out/cpu-level` (`x86-64-v3`) after copying a v3 runtime to `out/` and deletes it
  after a default build; `appimage.sh` reads it, names the output `dist/MediEvil-x86_64-v3.AppImage` and puts the
  marker into the image (`usr/lib/compat/cpu-level`).
- **AppRun gate:** with the marker, AppRun reads the first `flags` line of `/proc/cpuinfo` and requires
  `avx avx2 bmi1 bmi2 fma f16c movbe abm xsave` (the x86-64-v3 set as Linux names it; `abm` = LZCNT). Missing ones are
  listed in a plain message (stderr, and zenity / kdialog like the glibc check), exit 1, before Python or the runtime
  starts: no SIGILL. No `/proc/cpuinfo`: no check. Tested with dash against this PC's flags (passes) and a pre-AVX2
  flag set (lists `avx2 bmi1 bmi2 fma f16c movbe abm`). The default AppImage has no marker and no check.
- **Not done (study section 5, optional):** the eboot opcode scan that would show whether the guest code already
  needs BMI1 (it decides how many extra CPUs v3 excludes; irrelevant for the Deck), and the study's "one v3 binary
  for everyone" decision (the user's call; today the v3 image is a second artifact).

Binary notes (WSL, clang 19.1.7, ThinLTO, static GPU library; objdump of `out/gpu-v3/medievil-runtime` vs
`out/gpu/medievil-runtime`, both at `a863482`):

| | default | v3 |
|---|---|---|
| File size (with debug info) | 78,451,472 | 79,579,720 |
| `.text` (size) | 12,584,596 | 12,565,641 |
| Instructions (objdump) | 1,090,576 | 1,125,933 |
| VEX-encoded (`v*`) / legacy SSE on `%xmm` | 0 / 73,429 | 71,646 / 0 |
| Instructions using `%ymm` (AVX/AVX2) | 0 | 12,401 |
| BMI1/2: `shlx` / `shrx` / `sarx` / `rorx` / `andn` / `bzhi` / `blsr` | all 0 | 368 / 314 / 6 / 66 / 93 / 38 / 123 |
| `lzcnt` / `popcnt` / `movbe` | 0 / 0 / 0 | 116 / 25 / 76 |
| `pdep` / `pext` (slow on Zen 2) | 0 / 0 | 0 / 0 |
| `vpbroadcast*` / `vpcmpeq*` / `vpermq` / `vinserti128` | 0 | 303 / 469 / 60 / 79 |
| `vzeroupper` | 0 | 4,048 |
| FMA (`vfmadd*` etc.) | 0 | 0 (`-ffp-contract=off`) |
| PLT call/jmp sites / calls through the GOT | 4,054 / 29,810 | 4,022 / 29,672 |
| Linker (`.comment`) | bfd (no LLD entry) | `Linker: Debian LLD 19.1.7` |

The instruction count rises ~3% (AVX2 loops are unrolled and vectorised wider, plus `vzeroupper` at function exits);
`.text` is slightly smaller (VEX three-operand forms save moves). The PLT count is the static build's residual
(libc, libstdc++, SDL, Vulkan loader) and does not change with lld. `tzcnt` appears in both (`rep bsf` decodes as
`tzcnt`).

Expected (study table): **0-2% of stage B**, within one launch's noise (~2%). Judge by stage B us per draw and busy %,
and the CPU clock beside it.

## #7: core scheduling cookie on stage B (GOW3_STAGE_B_CORE_SCHED=1)

- At stage B's start (`DrawPipe::Run`, after the thread is named and `GOW3_STAGE_B_CORE` ran): `prctl(PR_SCHED_CORE,
  PR_SCHED_CORE_CREATE, 0, PR_SCHED_CORE_SCOPE_THREAD, 0)`, then `PR_SCHED_CORE_GET` for the log. As the study
  proposes (1.3): stage B's own cookie, so while it runs its SMT sibling runs only tasks with that cookie (none) and
  idles; when stage B sleeps, the sibling runs anything. No other thread's affinity changes (unlike `GOW3_STAGE_B_CORE`).
- One line: `Stage B: core scheduling cookie 0x... (GOW3_STAGE_B_CORE_SCHED), tid N: its SMT sibling runs only this
  cookie while it runs`, or `Stage B: no core scheduling cookie (GOW3_STAGE_B_CORE_SCHED): <error> (<why>)`: EINVAL =
  kernel without `CONFIG_SCHED_CORE` or before 5.14, ENODEV = no SMT / SMT off.
- Checked off the Deck: the WSL2 kernel (6.18, no `CONFIG_SCHED_CORE`) gives the EINVAL line. The success path needs
  the Deck's kernel; the line says which.
- Differences from the study: an env switch at start, not a live toggle bit (no live A/B in one launch; one launch
  per state).
- Caveats: stage B's spin before it sleeps (50 us default, `GOW3_STAGE_B_SPIN` longer) counts as running, so the
  sibling idles through it too. Threads stage B creates later inherit the cookie (none known). Whether a
  sched_ext scheduler (`scx_lavd`) honours the cookie is not confirmed: read `deck.sh checks`' `sched_ext` line
  beside the result.
- Expected (study): stage B -5-10% per draw; FPS anywhere from -5% to +5% (one of 8 CPUs idles ~90% of the time).

## #8: nice for stage B and the vblank thread (GOW3_THREAD_NICE)

- At the start of stage B and of the vblank thread (`VideoOutDriver::PresentThread`, after `BoostPresentingThread`):
  `setpriority(PRIO_PROCESS, tid, nice)` first (works when `RLIMIT_NICE` allows it or with `CAP_SYS_NICE`); if that
  fails, rtkit `org.freedesktop.RealtimeKit1.MakeThreadHighPriority(tid, nice)` over the system bus, through the
  host's `libdbus-1.so.3` (dlopen'd as SDL does; a private connection, closed after the call, that does not exit the
  process if the bus drops). `1` = nice -10 (the study's value); `-N` = nice -N (rtkit's default floor is -15).
  The vblank thread also gets `PR_SET_TIMERSLACK` 1 ns (the study's #8 includes it). Never realtime scheduling.
- One line per thread: `Thread priority (GOW3_THREAD_NICE): stage B (gow3:DrawRec), tid N, nice -10 by setpriority`,
  `... nice -10 through rtkit (setpriority: Permission denied, RLIMIT_NICE 0)`, or `... unchanged, not nice -10:
  setpriority: <error> (RLIMIT_NICE n); rtkit: <D-Bus error name (message)>`; the vblank line ends with
  `; timer slack 1 ns`.
- Checked in WSL: as root, both threads at -10 by `setpriority`, slack 1 ns; as `nobody` (RLIMIT_NICE 0) the
  rtkit path reached the system bus and printed `org.freedesktop.DBus.Error.ServiceUnknown` (no rtkit in WSL). An
  rtkit that answers was not available here: the Deck's line is the first real test of that path.
- Caveats: rtkit refuses without an active local session in polkit's view (Game Mode's gamescope session should be
  one; Desktop mode is) and rate-limits callers (2 calls here). Threads created later by stage B or the vblank thread
  inherit the nice level. `scx_lavd`, if active, reinterprets nice. The vblank thread (`shadPS4:PresentThread`) is
  the one `BoostPresentingThread("vblank")` names; the pacer and the swap thread are untouched.
- Expected (study): run-queue delay off the critical chain; lows rather than averages. Decided by the run delay in
  `/proc/<pid>/task/<tid>/schedstat` (field 2) before and after.

## Pool prefill and populate (GOW3_POOL_PREFILL_MB, GOW3_POOL_POPULATE)

- **(a) `GOW3_POOL_PREFILL_MB=N`:** after each `sceKernelMemoryPoolExpand`, a detached thread (`gow3:PoolFill`)
  `fallocate`s (mode 0: allocate and zero) the part of the expanded physical range the remaining budget covers, from
  its lowest page, in 16 MiB steps (a guest direct-memory release's hole punch waits for one step at most). The
  budget is N MiB in total (capped at 16384). Commits take the lowest free pages first (`pool_take_locked`), so they
  land in prefilled pages. Line: `Guest pool prefill (GOW3_POOL_PREFILL_MB): N MiB at 0x... allocated in N ms` (or
  where it stopped and why). RAM: N MiB of shmem from boot. The study suggests N = 2560-3072 (the pool's peak was
  2.3-2.4 GiB committed).
- **(b) `GOW3_POOL_POPULATE=1`:** each pool commit run is populated with one `madvise(MADV_POPULATE_WRITE)` (for a
  read-only commit `MADV_POPULATE_READ`; no-access commits are skipped) **after** the commit has released
  `pool_map_mutex` and the table lock and **before** it runs the GPU hooks. Locking is unchanged in both the locked
  and unlocked Linux paths (the call sits between the unlock and `flush_hooks`). A populate follows the view's
  current protection: if another thread's hook or a racing decommit took access away first it fails harmlessly
  (counted). The first commit probes support once (Linux 5.14+) and prints `Guest pool populate
  (GOW3_POOL_POPULATE): on, ...` or `off, madvise(...) failed: ...`.
- **Stats** (`GOW3_FRAME_STATS=1`, every 5 s, beside the `Region table per frame` line; `deck_summary.py` keeps it):
  process minor / major page faults in the window (`getrusage`; the first window counts from the start), populate
  calls, pages, time, failures, and MiB prefilled. **The kernel counts each page a populate enters as a minor fault
  too**, so with (b) on, the process's minor faults do not fall; the game's own faults are minor faults less the
  pages populated. Judge (b) by the hitch frames and the populate ms, not the fault count.
- Checked in WSL with a scratch test against the runtime's exports (expand 256 MiB, reserve, commit 4 MiB, touch
  every 4 KiB page; WSL2 on the 7800X3D, 3 runs each):

  | Switches | commit (4 MiB) | first touches of 1,024 pages | per page |
  |---|---|---|---|
  | off | 39-54 us | 1,771-2,305 us | ~2.0 us |
  | prefill 64 MiB | 38-42 us | 922-1,219 us | ~1.1 us |
  | populate | 2,044-2,383 us | 8-9 us | ~2.1 us (moved into the commit) |
  | prefill + populate | 783-864 us | 7-8 us | ~0.8 us |

  Prefill alone halves a first touch (no allocation and zeroing in the fault). Populate alone only moves the same
  allocation cost into the commit, and it populates pages the game may never touch; with prefill it saves the trap
  per page on top. Data written before a decommit reads back after a recommit; read-only commits populate with
  `_READ`. The Deck's numbers will differ (the study: ~4 us per new page, ~1 us per held page).
- Caveats: populate costs CPU on the committing thread for pages never touched (UE4 commits 64 KiB, ~6,000 a second
  in play); the populate ms per window shows it. Prefill fallocate rate here: 64 MiB in 27-31 ms, so 2.5 GiB is ~1.2 s
  of one core on this PC, longer on the Deck (study: 0.3-0.6 s estimated), during the logos. Windows: not done (the
  pagefile-backed section has no cheap equivalent that is clearly safe).
- Expected (study): ~50-200 ms off each area hitch (68-86k faults now), ~0.2-0.5 ms of game-thread CPU a frame;
  the study's order is (a) first, then (b).

## Build and test

- Windows: `research/tools/build-clang64.sh` at `a863482`: `build.sh exit: 0`, no warnings in the incremental
  build. `draw-classes-test` and `readback-queue-test` pass. `pipeline-binary-cache-test` does not link on Windows
  (undefined `runtime_*` symbols such as `runtime_ue4_request_setres`, `runtime_memory_lock_report`: the test target
  links the static library without the runtime; not caused by these commits); it passes on Linux (below).
- Python unit tests (`py -3.14 -m unittest discover -s tests`): 127 run, the 8 known failures (2 failures, 6 errors).
- Linux (WSL Debian `/root/src/medievil` at `a863482`, `CC=clang CXX=clang++`):
  - `GOW3_CPU_LEVEL=v3 bash build.sh` + `appimage.sh`: **`dist/MediEvil-x86_64-v3.AppImage`** (133,782,008 bytes,
    sha256 `3467a665...`); the image holds `usr/lib/compat/cpu-level` = `x86-64-v3` and its runtime has 12,401
    `%ymm` instructions; `--help` runs (this PC's CPU passes the gate).
  - Then the default `bash build.sh` + `appimage.sh` (incremental in `out/gpu`; `out/cpu-level` removed):
    **`dist/MediEvil-x86_64.AppImage`** (131,197,432 bytes, sha256 `5c1c16e7...`), no marker, 0 `%ymm`. `out/` is the
    default flavour again. Both images: "Needs glibc 2.39+".
  - `draw-classes-test`, `readback-queue-test`, `pipeline-binary-cache-test`: pass. `runtime-test` / `sema-test` do
    not link (`runtime_image_base` / `_size` live in `probe.c`; known since impl-deck-cpu-bundle.md, not these commits).
  - Scratch checks (not in the tree): the pool test above; the sched functions in a small program (the #7 and #8
    sections).
- `src/runtime_memory.c` also compiles with gcc 14 `-std=c11 -Wall -Wextra -Werror` (the shared-library path's
  compiler).
- Not run in game, not run on the Deck.

## The Deck test (one change per launch, each with the user's go)

Same spot and settings as the previous Deck runs (720p, 67%, Low, graveyard heavy view, stand still, auto clocks;
`GOW3_FRAME_STATS=1`), same toggle file as the current runs. `deck.sh env` replaces `extra-env.txt`: carry over any
keys the current runs use into every command below. Before the first launch: `deck.sh checks` (SMT, RLIMIT_NICE,
`sched_ext` state) and, once, `ssh deck 'zcat /proc/config.gz | grep SCHED_CORE; busctl --system status
org.freedesktop.RealtimeKit1 | head -3'`.

| # | Deploy | `deck.sh env` | During the run | Read |
|---|---|---|---|---|
| 0 | `deck.sh deploy` (default `MediEvil-x86_64.AppImage`) | `"baseline" GOW3_FRAME_STATS=1` | `deck.sh clocks 20`; `deck.sh threads 10`; `deck.sh mem 60` across an area load | the reference for 1-5, including the new `Guest pool faults` line |
| 1 (S1) | `deck.sh deploy /root/src/medievil MediEvil-x86_64-v3.AppImage` | `"S1 v3" GOW3_FRAME_STATS=1` | `deck.sh clocks 20` | it starts (no CPU message); stage B us per draw, busy %, FPS vs 0; CPU clock beside it |
| 2 (#7) | `deck.sh deploy` (default) | `"#7 core sched" GOW3_FRAME_STATS=1 GOW3_STAGE_B_CORE_SCHED=1` | `deck.sh threads 10`; run delay (below) | the `Stage B: core scheduling cookie` line; stage B us per draw, busy %, FPS; game threads' run delay vs 0 |
| 3 (#8) | default | `"#8 nice" GOW3_FRAME_STATS=1 GOW3_THREAD_NICE=1` | run delay (below) | the two `Thread priority` lines (setpriority, rtkit or failed); stage B's and the vblank thread's run delay; 1% / 0.1% lows, pacing p99 |
| 4 (pool a) | default | `"pool prefill" GOW3_FRAME_STATS=1 GOW3_POOL_PREFILL_MB=2560` | `deck.sh mem 60` across the same area load as 0 | the prefill line (MiB, ms); `Stall:` frames' length and "process" faults at the same turn; `Guest pool faults` minor per window; RSS Shmem +2.5 GiB from boot, MemAvailable |
| 5 (pool b) | default | `"pool populate" GOW3_FRAME_STATS=1 GOW3_POOL_PREFILL_MB=2560 GOW3_POOL_POPULATE=1` (b on top of a, the study's order; drop the prefill key to test b alone) | as 4 | the populate line; populate ms per window (its cost) vs the stall frames; minor faults less populated pages |

After each run: `deck.sh pull <name>` and `deck.sh summary <name> --min-draws 1000` (FPS, stage B busy, us per draw
per window; the `Guest pool faults` line is kept with the `Region table` one). Run delay for 2 and 3 (twice, 10 s
apart, during the heavy view; light: tell the user it runs):

```sh
ssh deck 'p=$(pgrep -x medievil-runtim); for t in /proc/$p/task/*; do
  printf "%s nice %s %s\n" "$(cat $t/comm)" "$(cut -d" " -f19 $t/stat)" "$(cat $t/schedstat)"; done' | sort -k5 -n -r | head -20
```

(`schedstat` field 2 = ns runnable but waiting for a CPU; `stat` field 19 = nice.)

Pass / keep:
- S1: starts on the Deck; stage B us per draw not worse. Within noise is the expected outcome ("not measurable"); keep
  the v3 image as the Deck's artifact only if it is not worse (no downside otherwise).
- #7: keep if FPS or lows do not drop while stage B us per draw falls; if FPS drops (the game wants the 8th CPU), off.
- #8: keep if the lines say the nice level applied and lows or pacing p99 improve; the run delay of stage B and the
  vblank thread should fall.
- Pool: (a) keep if the area-load stall frames shorten at the same turn and memory stays comfortable (MemAvailable,
  no swap); (b) keep only if it shortens them further by more than its own populate ms.
