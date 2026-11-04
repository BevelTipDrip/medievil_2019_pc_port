# Study: compiler, linker and build-level optimisation of the Linux build for the Steam Deck (2026-11-04)

Status: read-only research (code, the WSL build tree's binaries and CMake cache, web). Nothing built, launched or
committed; no SSH. Branch `deck-lab` (`5aac209` + the uncommitted `GOW3_STATIC_GPU` work another agent is doing in
`build.sh` / `gpu/CMakeLists.txt`). This plan assumes that work lands as it stands in the working tree today.
Parent: [study-deck-platform-optimization.md](study-deck-platform-optimization.md) section 5 (B1-B5). Related:
[study-deck-rescore-rejected.md](study-deck-rescore-rejected.md) (C14 PGO, C15 `-march`, D1 TLS),
[impl-deck-cpu-bundle.md](impl-deck-cpu-bundle.md) (D1 done), [study-stage-b-per-draw.md](study-stage-b-per-draw.md).

Marks: **[code]** read in this tree. **[bin]** read from the WSL build at `ff54428` (`/root/src/medievil`, read-only
`objdump` / `nm` / `readelf` / CMake cache). **[log]** a measured number from a doc or log. **[web]** a source in the
Sources list. **[inferred]** reasoning or estimate, not measured. Nothing here is "verified" in the project's sense.

## Answer

The yardstick (Deck, graveyard heavy view, 67%, auto clocks): ~65 FPS (15.45 ms), stage B 91% busy (~14 ms a frame),
~1,400 draws, 1 us per draw = 1.4 ms a frame. Every flag below changes only **host** code (stage A/B, recorder,
prep workers, the HLE runtime). The game's own code (UE4, guest threads) is the PS4 binary and no compiler flag
touches it.

| Step | Build | Deck launch | Expected on stage B (all [inferred] unless marked) | Effort | Risk |
|---|---|---|---|---|---|
| S0 | **Static GPU library** (other agent: `GOW3_STATIC_GPU`, `-fno-plt`, local-exec TLS, runtime C in ThinLTO) | measure | **1-4%** (0.15-0.55 ms) | in progress | L |
| S1 | `-march=x86-64-v3 -mtune=znver2 -fno-math-errno` + lld, one AppImage, AppRun CPU gate | measure | **0-2%** (0-0.3 ms) | S | L |
| S2 | clang IR PGO, **instrumented** build | **training run** (no measurement) | - | M | L |
| S3 | PGO **use** + lld call-graph ordering + hot/cold text sections | measure | **3-7%** (0.4-1.0 ms) | S after S2 | L |
| S4 | BOLT **instrumented** binary (of S3's build, linked `--emit-relocs`) | **training run** | - | M | L |
| S5 | BOLT-optimised binary | measure | **2-5%** on top (0.25-0.65 ms) | S after S4 | M |
| S6 | optional, one each: `-hugify`; `-O3` under PGO; CS-PGO; `LD_PRELOAD` mimalloc | measure | 0-2% each | S | L-M |

S0 + S1 + S3 + S5 together: **~6-15% of stage B, ~0.9-2.3 ms a frame**. While stage B is the wall that is
+5-12% FPS (65 → ~69-73), but the GPU is ~90% busy at auto clocks, so part of it may show only as lower stage B busy
and lower CPU power (which the shared 15 W budget hands to the GPU). Judge by **stage B us per draw and busy %**, not
FPS alone.

Not worth a launch: `-fomit-frame-pointer` (the watchdog and the sampler walk `rbp`), `-fno-exceptions` (not
possible), `-fstrict-aliasing` (already on), `-fvisibility=hidden` / `-fno-semantic-interposition` (moot once
static), `-ffunction-sections` + `--gc-sections` alone (size only), `-falign-*` (BOLT does layout), `-mllvm`
inliner thresholds (PGO does hot-call-site inlining), full LTO instead of ThinLTO (0-1%, much slower link),
function multiversioning (stage B is not SIMD code), AutoFDO / CSSPGO (Zen 2 has no LBR), and an allocator swap
before a profile shows malloc (stage B allocates almost nothing per draw, section 4).

## 1. What the build does today [code, bin]

- **Compiler and linker:** Debian clang 19.1.7. `CMAKE_LINKER=/usr/bin/ld.lld` is in the cache, but the link
  driver uses the default `ld` (bfd) with the LLVMgold plugin: no `-fuse-ld=lld` in `build.ninja`, and there is no
  "Linker: LLD" in `.comment` [bin]. So lld's call-graph ordering is not used today.
- **Flags:** `RelWithDebInfo` = `-O2 -g -DNDEBUG`, `-fno-omit-frame-pointer` (`gpu/CMakeLists.txt:12`), ThinLTO
  (`-flto=thin` on 319 compile lines), `-fPIC` (243 lines: `CMAKE_POSITION_INDEPENDENT_CODE ON`), no `-march`
  (comment at `:16-18`: "the same build runs on the Steam Deck"). Runtime C: `-O2 -g`, no LTO, `-no-pie`
  (`build.sh`, the shared variant).
- **The shared library's cost, measured from the binary [bin]:** `libgow3gpu.so` (`ff54428`) has **47,409 call/jmp
  sites through the PLT, 16,000 of them to the library's own exported functions** (7,055 exported symbols:
  default visibility plus `-fPIC` means every non-inlined call between translation units can be interposed). The top
  ones: `Common::Log::ShouldLog` (810 sites), `Common::Log::Write` (829), `assert_fail_impl`, `unreachable_impl`,
  shader IR/Sirit helpers. `.text` is 4.4 MB (0x43dacf). After D1, `__tls_get_addr` is down to 5 references
  ([impl-deck-cpu-bundle.md](impl-deck-cpu-bundle.md)).
- **What S0 changes (working tree, not committed) [code]:** `GOW3_STATIC_GPU` (default ON) makes `gow3gpu` a static
  library linked into `medievil-runtime` with the runtime C (`-no-pie -rdynamic`, `POSITION_INDEPENDENT_CODE OFF` for
  the executable), `-fno-plt` on both, `-ftls-model=initial-exec` (relaxed to local-exec at the link). The library's
  objects stay `-fPIC`; in a non-PIE executable the linker relaxes their GOT/PLT references to direct ones, and
  ThinLTO knows every symbol is final in the link, so that costs little [inferred].
- **PGO path:** GCC-only. `GOW3_PGO=use` passes `-fprofile-partial-training` and `-Wno-coverage-mismatch` (GCC
  options), `build.sh` looks for `*.gcda`, and `gow3gpu.cpp:240-256` calls `__gcov_dump` / `__gcov_reset` every 30 s.
  None of it works with clang (C14).
- **Allowed environment:** `run.py` takes only `GOW3_`, `RADV_`, `MESA_`, `AMD_`, `VK_`, `SDL_` keys from
  `extra-env.txt` (`run.py:72`). `LLVM_PROFILE_FILE`, `LD_PRELOAD` and `GLIBC_TUNABLES` are not among them.
- **glibc:** the WSL userland is Debian 13, glibc 2.41 [bin], so today's AppImage needs glibc ≥ 2.41 (AppRun checks
  it). SteamOS 3.9.5 has 2.43: fine. **Hosts with 2.39-2.40 (Ubuntu 24.04) cannot run it.** That is a userland
  question, not a flag: build in an Ubuntu 24.04 container with clang 19 from apt.llvm.org. Everything below
  (flags, PGO profile, BOLT) carries over unchanged.

## 2. Flags, one by one, for a branchy pointer-chasing thread on Zen 2

Stage B's time is in `DrawRecord` → `BindResources` / `BindTextures` / `ObtainBuffer` / descriptor copies / ordered
tasks ([study-stage-b-per-draw.md](study-stage-b-per-draw.md) section 1): hash and map lookups, small copies,
mutexes, many functions per draw. Every part costs 4-5x the PC's, which points at memory latency and the front end
(4 MB L3, 32 KB L1i, 512-entry L2 iTLB = 2 MB of code), not at arithmetic.

| Flag | Today | Effect on stage B | Verdict |
|---|---|---|---|
| `-O2` → `-O3` | `-O2` | clang's `-O3` adds a higher inline threshold, more unrolling and loop unswitching: mostly larger code. On a front-end-bound thread with a 32 KB L1i that can be negative. Phoronix O2/O3 comparisons are mixed (wins and losses within a few %) | Only under PGO (which keeps the growth to hot paths), as S6: -1% to +2% |
| `-march=x86-64-v3` | none (x86-64 v1, SSE2) | AVX2 paths of `common/bit_array.h` (buffer-cache page bits, `region_definitions.h`) compile in; BMI1/2 (`andn`, `shlx`, `tzcnt`/`lzcnt` without fallbacks), `movbe`, VEX encoding everywhere (no legacy-SSE / VEX mixing next to the guest's VEX code). Stage B is not SIMD code: **0-2%** | S1, one AppImage (section 5) |
| `-march=znver2` | - | Same ISA as v3 for this code plus SHA/CLZERO nobody uses; ties the binary to Zen 2+ only for no measurable gain | No: `-march=x86-64-v3 -mtune=znver2` gives the same code generation for the Deck |
| `-mtune=znver2` | generic | Scheduling model, alignment and instruction-choice tuning for Zen 2. Harmless elsewhere | With S1 |
| BMI2 caveat | - | `pdep`/`pext` are microcoded and slow on Zen 2. The compiler never emits them by itself; no intrinsic uses them in `gpu/shim`, `gpu/shadps4`, `src` [code] | No action |
| `-fno-plt` | off | Calls into libc, libstdc++, SDL, FFmpeg become `call *GOT` instead of a PLT stub's extra jump. Remaining hot ones: `pthread_mutex_lock/unlock`, `memcpy`, `operator new/delete` | Done in S0 |
| `-fno-semantic-interposition`, `-fvisibility=hidden` | off | What they fix in a shared library (16,000 interposable PLT calls) disappears with the static link: an executable's symbols cannot be interposed | Only for the `GOW3_STATIC_GPU=OFF` fallback (one line, no launch of its own) |
| `-fPIC` on the static library | on | Residual cost after linker relaxation and ThinLTO is small (PC-relative jump tables, some GOT loads the linker relaxes to `lea`) | Optional: `POSITION_INDEPENDENT_CODE OFF` on `gow3gpu` when static; check with `objdump` (section 6), no launch |
| `-fomit-frame-pointer` | `-fno-omit-frame-pointer` on purpose | ~0-2% on x86-64 (Fedora measured 2% on Blender, 2.4% on a kernel build; "usually <1%" in production studies) [web]. The watchdog dump (`src/probe.c:374-395`) and the SIGUSR2 sampler walk `rbp`, and `perf --call-graph fp` needs it on the Deck | **Keep frame pointers** |
| `-fno-exceptions` | off | Not possible: Vulkan-Hpp throws (`vk::SystemError`), `std::mutex` throws, 1,973 `_Unwind_Resume` landing pads in the library [bin]. Zero-cost on the happy path anyway | No |
| `-fstrict-aliasing` | on (clang default at `-O2`) | - | Nothing to do |
| `-ffunction-sections` + `--gc-sections` | off | Size only. Useful as an enabler: with lld and PGO, per-function sections let lld order functions by the call graph (S3) | In S3, not alone |
| `-mllvm -inline-threshold=...` | default | Raises inlining everywhere, cold code too: more I-cache pressure | No; PGO raises it only at hot call sites (hot-callsite threshold) |
| Full LTO vs ThinLTO | ThinLTO | Full LTO: one module, single-threaded optimisation, link several times slower, typically within ±1% of ThinLTO. Windows measurement: ThinLTO ≥ no LTO on stage B per draw (t-lto-a1/b, `build.sh:5-6`) [log] | Stay on ThinLTO |
| `-falign-functions=32`, `-falign-loops=32` | 16 | Noise-level and unstable from build to build | No; BOLT lays out code |
| `-fno-math-errno` | clang on Linux keeps `errno` for math | Lets `sqrt` etc. inline without the errno branch. Nothing reads `errno` after math calls. Negligible for stage B | With S1, free |
| Function multiversioning (`target_clones`) | - | An `ifunc` indirect call per call and no inlining across it: wrong for many small hot functions | No |

## 3. PGO with clang

### 3.1 Which kind

- **Instrumented IR PGO** (`-fprofile-generate` → `llvm-profdata merge` → `-fprofile-use=file.profdata`): exact
  counts, works on any CPU. Gives block layout, hot/cold splitting, hot-call-site inlining, **indirect-call
  promotion** (the recorder's `CommandBase::Execute` virtual calls, `std::function` signals) and memcpy size
  specialisation. **The route for the Deck.**
- **AutoFDO** (sampling, `perf record -b` → `create_llvm_prof`): needs branch records. Zen 2 has neither LBR nor
  BRS; AMD branch sampling (BRS) starts with Zen 3 and Linux 5.19 [web]. Without branch stacks the profile is poor.
  **Not viable on the Deck.**
- **CSSPGO** (pseudo-probes + LBR sampling): same LBR requirement. **Not viable.**
- **CS-IRPGO** (`-fcs-profile-generate` on top of a PGO build, then a second merge): context-sensitive counts after
  inlining, usually a further 1-3% [inferred]. A second training run; optional (S6).

**Is `perf` usable unprivileged on SteamOS?** Not verified. The read-only root probably has no `perf` binary
[inferred]; `deck.sh checks` prints `which perf` and `perf_event_paranoid`. With the usual Arch/upstream default of 2
a user can sample user space of their own processes (`-e cycles:u`), which is enough for the checks in section 6
(top symbols, IPC, iTLB / L1i misses). Source of a binary: the Debian distrobox's `linux-perf` (shares the host PID
namespace, plan-linux-deck.md "Profiling") or a static `perf` copied to `~/bin`. PGO itself does not need perf.

### 3.2 The workflow (fits: WSL build → AppImage → Deck run → profiles back over SSH)

Changes needed in the tree (to make when the static work has landed; none made here):

1. **CMake, clang branch of `GOW3_PGO`** (keep the GCC branch for Bloodborne-style GCC builds):
   - `generate`: `-fprofile-generate -fprofile-update=atomic` on compile and link, for `gow3gpu`, the static
     third-party libraries and **the runtime C** (`gow3_runtime_executable`: the HLE functions stage B and the guest
     threads call). Keep `GOW3_PGO_GENERATE`.
   - `use`: `-fprofile-use=<pgo>/medievil.profdata -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled
     -Wno-backend-plugin` (the runtime C is `-Werror`: these must reach it too). Drop the GCC-only
     `-fprofile-partial-training`, `-fprofile-correction`, `-Wno-coverage-mismatch`.
   - Linker: `-fuse-ld=lld` (it is installed: `/usr/bin/ld.lld` [bin]), `-ffunction-sections`, and
     `-Wl,-z,keep-text-section-prefix` so the PGO-marked `.text.hot.*` / `.text.unlikely.*` functions land in
     contiguous ranges. lld then orders functions from the `.llvm.call-graph-profile` the PGO build emits;
     cdsort is lld's default since LLVM 18, "up to 1.5%" on large front-end-bound binaries [web].
2. **`build.sh`:** detect `pgo/*.profdata` (today `*.gcda`) for the automatic `use`.
3. **`gow3gpu.cpp:240-256`:** under `__clang__`, the 30-s writer calls `__llvm_profile_write_file()` then
   `__llvm_profile_reset_counters()` (compiler-rt; declared in `<profile/instr_prof_interface.h>` since LLVM 18
   [inferred: check the header in clang 19]). The game often ends through `_exit`, which skips the at-exit write,
   as the GCC comment says. **The file name must contain `%m`** (online merging): without it each write would
   overwrite the previous one after the reset. The writer sets it itself with
   `__llvm_profile_set_filename("<GOW3_DATA_DIR>/pgo/medievil-%m.profraw")`, because `LLVM_PROFILE_FILE` is not an
   allowed `extra-env.txt` key and the compiled-in default (`/root/src/medievil/pgo`) does not exist on the Deck.

Run:

| # | Where | What |
|---|---|---|
| a | WSL | `GOW3_PGO=generate bash build.sh && bash packaging/linux/appimage.sh`; `deck.sh deploy` (the previous AppImage is kept) |
| b | Deck, **the user's go** | A training run, ~10 min, in the settings of the measurement runs (67%, auto clocks): boot and menu, Dan's crypt, the graveyard heavy view (stand, pan, walk), one fight with kill effects, one area not visited before (shader and pipeline compilation, so the recompiler is not treated as cold). Expect it slower than normal (atomic counters shared by stage A/B and the 4 prep workers; 2-4x on the hot path [inferred]); counts do not depend on FPS. If it is unplayably slow, rebuild with `-fprofile-update=single` (racing increments lose a few counts, nothing worse) |
| c | WSL | `scp deck:.local/share/medievil-pc/pgo/*.profraw pgo/raw/`; `llvm-profdata-19 merge -o pgo/medievil.profdata pgo/raw/*.profraw` (same LLVM major as the compiler). Check: `llvm-profdata-19 show --topn=40 pgo/medievil.profdata` lists `DrawRecord`, `BindTextures`, `ObtainBuffer`, `Scheduler::Record`... |
| d | WSL | `GOW3_PGO=use bash build.sh` + AppImage. In `out/gpu-build.log`, count `profile-instr-out-of-date` / `unprofiled` warnings (should be ~0 right after training) |
| e | Deck, **the user's go** | The measurement run (section 6) |

### 3.3 Expected gain, and staleness

- **Basis:** Bloodborne (same code base, GCC, Linux): LTO -1.8%, LTO + PGO **-4.8% per draw** on the GPU thread, 3
  runs each [log, `bloodborne_pc/docs/parallel_gpu.md:247-252`]: PGO ≈ 3% on top of LTO. Chrome's clang PGO: up to
  10% faster page loads, 7-11% on JS benchmarks [web]. lld call-graph ordering: up to 1.5% [web]. The Deck's smaller
  caches and iTLB favour layout work more than the 7800X3D does [inferred].
- **Estimate for stage B on the Deck: 3-7% (0.4-1.0 ms a frame)** [inferred]. Also the game threads' HLE calls and
  stage A.
- **Staleness:** clang matches profiles per function by name and CFG hash. A changed function loses its profile
  (compiled as without PGO) and warns `profile-instr-out-of-date`; nothing breaks. Policy: re-train when stage B's
  files change materially (`vk_rasterizer.cpp`, `vk_scheduler.*`, `vk_draw_pipe.*`, `texture_cache`,
  `buffer_cache`) or the out-of-date count passes ~5% of profiled functions, and before a release. Old and new can
  be merged with weights (`llvm-profdata merge -weighted-input=1,old.profdata -weighted-input=3,new.profdata`).
  Where the profile lives (repo, release asset) is the user's call; ~5-20 MB [inferred].
- **Risks:** low (build only). Training content bias: code not exercised is optimised for size where the profile
  says cold, hence the new-area segment in (b). The instrumented build changes timing; Bloodborne's instrumented
  build exposed a real race (`parallel_gpu.md:300-303`): a crash there is a finding, not a PGO problem.
- **Windows:** MSYS2 CLANG64 ships the same profile runtime; the same clang path would work there later, trained
  on the PC (different `#ifdef`s and driver paths). Not in scope.

## 4. Allocators

Guest `malloc` is the game's own (guest code); a host allocator affects only the port, SDL, FFmpeg and Mesa
(study-deck-platform-optimization B4). What allocates on stage B [code]:

- **Per draw: almost nothing.** `Scheduler::Record` closures and `RecordData` go into recycled 128 KiB chunks
  (`vk_scheduler.h:600-640`, `AcquireChunk` reuses `free_chunks`, `vk_scheduler.cpp:330-338`); binding lists are
  `boost::container::small_vector`s; `std::vector<u32> flat` in `vk_rasterizer.cpp:825` is a diagnostic capped at 32
  reports.
- **Per end-of-pipe / end-of-shader event (~200 a frame):** `SignalAfterHostCopies` takes a `std::function`, wraps
  it with a 64-bit fence and a `shared_ptr` in a second `std::function` for `Gow3Copy::AfterCopies`
  (`vk_scheduler.cpp:340-367`, `gow3_copy.cpp:64-75`): both exceed libstdc++'s 16-byte small buffer, so 2-4 heap
  allocations per event, freed on the recorder or copy thread (cross-thread frees). ~400-800 malloc/free pairs a
  frame at ~50 ns = 20-40 us, **~0.2% of stage B** [inferred].
- **Upload batches:** `make_shared<Group>` per batch of CPU-modified ranges (`buffer_cache.cpp:1526`, `:1948`): a
  few per frame.
- **Shader / pipeline compilation** (other threads, during stutters) allocates heavily: 2,454 `operator new` call
  sites, mostly in the recompiler and Sirit [bin]. A faster allocator could shorten compile hitches, not stage B.

**Verdict:** mimalloc / jemalloc / tcmalloc would save ≤ 0.2% on stage B. Test one (S6, `LD_PRELOAD` of a bundled
`libmimalloc.so`, which needs `LD_PRELOAD` admitted by `run.py` or set in AppRun) **only if** a `perf` profile of
`gow3:DrawRec` or `gow3:DrawPrep` shows `malloc`/`free`/`_int_free` above ~2%. The code fix for the per-event
allocations (a move-only callback with a 48-byte inline buffer, or the closure stored in the record chunk) is cheaper
and portable. glibc tunables worth one free test each: `glibc.malloc.hugetlb=1` (M3 in the platform study);
`glibc.malloc.tcache_count` higher (fewer arena-lock frees when the recorder frees stage B's allocations) only if
the profile shows `_int_free` / arena locks. Do not fix `glibc.malloc.mmap_threshold`: setting it disables glibc's
dynamic threshold.

## 5. Keeping one portable AppImage

- **The game already needs a Jaguar-class CPU.** The PS4 code runs natively; Jaguar code is VEX/AVX with BMI1 and
  F16C [inferred: compiled for btver2]; only SSE4a is patched for Intel (`src/runtime_sse4a.c`). A CPU without AVX
  cannot run it today, and AVX-only CPUs without BMI1 (Sandy/Ivy Bridge) probably not either. x86-64-v3 further
  drops only AVX1-only CPUs with BMI1 (AMD Piledriver/Steamroller) and AVX-less Pentium/Celeron parts. shadPS4
  upstream already builds with `-march=x86-64-v3` [web]. Check before S1 (no launch): count VEX and BMI1 opcodes in
  the eboot's text with an objdump script; if the guest code already needs BMI1, the v3 cost is the
  Piledriver family only.
- **Recommended: one binary, `-march=x86-64-v3 -mtune=znver2`, plus a gate in AppRun** next to the glibc check:
  `/lib64/ld-linux-x86-64.so.2 --help | grep -q 'x86-64-v3 (supported'` (glibc ≥ 2.33 prints it), else a plain
  message like the glibc one. Same artifact for the Deck, other distros and the PC's Zen 4.
- **If pre-AVX2 CPUs must stay supported:** two executables in one AppImage (`bin/medievil-runtime` built v3,
  `bin/v1/medievil-runtime` built baseline), AppRun picks through `GOW3_PROBE` (AppRun only sets it when unset,
  `run.py:149` reads it). The PGO profile applies to both builds (IR-level, same source). Cost: a second build and
  ~10-15 MB more in the image with debug info split off (the 77 MB library today is mostly DWARF) [inferred]. The
  glibc-hwcaps directory scheme (`glibc-hwcaps/x86-64-v3/`) would only work for a shared library, which S0 removes.
- **Debug info:** keep building with `-g` (BOLT and `coredumpctl` backtraces want symbols), ship stripped or with
  the DWARF split into a local `.debug` file matched by build-id. Not a speed item; it keeps a two-binary image small.

## 6. Measurement per step (one build, one launch)

- Same spot and settings as the C1 / lean-mode tests ([impl-deck-cpu-bundle.md](impl-deck-cpu-bundle.md), "The
  Deck test"): graveyard heavy view, 67%, auto clocks, `GOW3_FRAME_STATS=1`, sections off, stand still; drop the
  first window; 8+ five-second windows.
- Compare with the previous step's AppImage in the same spot: stage B busy %, GPU-thread us per draw, draws a frame,
  FPS; plus the CPU clock (P0 logger) because CPU and GPU share the power budget and the clock moves between runs.
- Run-to-run spread: Bloodborne measured ±1.5 FPS on one machine (`parallel_gpu.md:305-308`) [log]. **A single
  launch resolves ~2% at best.** Hence S1 bundles its small flags, and S0/S3/S5 are the steps sized to be seen. If
  a step reads within the spread, keep it only if it has no downside (all of S0-S3) and say "not measurable".
- **No-launch checks after each build [bin-level, WSL]:**
  - S0: `objdump -d out/gpu/medievil-runtime | grep -c '@plt>'` and calls through `*...(%rip)` to symbols the
    executable defines: expect ~0 internal (from 16,000); `grep -c __tls_get_addr` ~0.
  - S1: `objdump` shows `shlx`/`andn`/`vpcmpeqq ymm` in `BitArray` users; no `pdep`/`pext`.
  - S3: `llvm-profdata show --topn`; `readelf -S` shows `.text.hot` / `.text.unlikely` ranges; the hot range size.
  - S5: `llvm-bolt -dyno-stats` before/after (taken branches, executed instructions in hot functions).
- **Ride-along `perf` checks** (on any launch already planned, read-only, if `perf` is available): `perf stat -t
  <DrawRec tid> -e cycles:u,instructions:u,L1-icache-load-misses:u,iTLB-load-misses:u -- sleep 10` (low IPC with
  high L1i / iTLB misses says S3 and S5 are worth more); `perf record -t <tid> -e cycles:u --call-graph fp -- sleep
  10` for the `malloc`/`free`, `@plt` and `memcpy` shares (decides S6's allocator test).

## 7. BOLT

- **Tooling:** Debian 13 has `bolt-19` (`llvm-bolt-19`, `merge-fdata-19`, `perf2bolt-19`), not installed in WSL
  yet [bin: `apt-cache policy`].
- **Requirements:** link with `-Wl,--emit-relocs` (add it in S3's build so S4 needs no relink); a non-PIE static
  executable is BOLT's best-supported case (one more reason S0 comes first; shared-library support is "added
  recently" [web]). Must not use GCC's `-freorder-blocks-and-partition` (clang: not applicable).
- **Profile without LBR: instrumentation.** `llvm-bolt-19 medievil-runtime -instrument -o medievil-runtime.inst
  --instrumentation-file=/home/deck/.local/share/medievil-pc/bolt/prof.fdata --instrumentation-sleep-time=30` (writes
  every 30 s, since the game ends through `_exit`). Instrumentation counts are exact; the README's "gains ... expected
  to be lower" applies to sample-only (`-nl`) profiles from `perf` without branch records [web], which is the
  fallback if instrumentation fails.
- **Optimise:** `llvm-bolt-19 medievil-runtime -o medievil-runtime.bolt -data=prof.fdata -reorder-blocks=ext-tsp
  -reorder-functions=cdsort -split-functions -split-all-cold -split-eh -dyno-stats -update-debug-sections`, then
  the AppImage from the result.
- **Expected:** the BOLT paper: up to 7% on top of PGO + LTO for data-center services, up to 20.4% for clang/GCC
  binaries (front-end bound) [web]. Our hot text is a few MB at most and PGO + lld ordering already did part of the
  layout: **2-5% of stage B** [inferred]. The S0-S3 `perf stat` front-end numbers decide whether to expect the top
  or the bottom of that range.
- **`-hugify` (S6):** remaps the hot text onto a 2 MiB page at start (needs anonymous THP at least `madvise`; the
  Deck's setting is section 0 of the platform study). The text comes from the AppImage's squashfs over FUSE, which is
  never huge-page backed otherwise. 0-2% [inferred]; its own launch.
- **Risks (M):** BOLT must keep the `-rdynamic` dynamic symbols and `dladdr` names right (fault and watchdog dumps,
  `src/probe.c:341-366`), the frame-pointer chain (it does not remove frame pointers unless `-frame-opt` is given),
  C++ exception tables, and function pointers the runtime hands to guest code (relocations cover them). Check with
  one boot to the menu as part of S5's launch; the previous AppImage stays on the Deck (`deck.sh deploy`).
- **Staleness:** a BOLT profile is tied to one binary's addresses: every release needs a new S4 training run. If
  that is too costly, `-fsplit-machine-functions` (clang, uses the S3 IR profile, x86-64 ELF) gives the hot/cold
  split without retraining per binary, at a smaller gain [inferred]. BOLT's stale-profile matching
  (`-infer-stale-profile`, YAML profiles) is the other option; not tried anywhere here.

## 8. What the emulators ship (checked)

- **shadPS4 (upstream):** `-march=x86-64-v3` on x86-64, `Release`, PIC on Linux, no LTO / PGO in the top-level
  CMakeLists [web].
- **PCSX2:** clang only on Linux, lld in its documented build; the AppImage CI job is named "sse4" (SSE4.1 baseline);
  LTO in the `clang-release` preset [web]. Its internal per-ISA dispatch was not confirmed by this search.
- **Bloodborne port:** GCC, LTO (`-flto=auto`) + PGO, no `-march` [code, `bloodborne_pc/gpu/CMakeLists.txt:16-40`].
- RPCS3, Dolphin, Ryujinx: not checked (B5 in the platform study stays open).

## 9. Order, and what each step needs from the user

1. **S0** (other agent): one measurement launch against `ff54428`'s numbers.
2. **S1**: one build, one launch. Before it: the eboot opcode scan (section 5) and the user's choice between one v3
   binary and two binaries.
3. **S2**: the training launch (~10 min of play in the listed spots). **S3**: one build, one launch.
4. **S4**: install `bolt-19` in WSL, the training launch. **S5**: one build (BOLT pass + AppImage), one launch.
5. **S6**: only what the checks justify, one per launch: `-hugify`, `-O3` under PGO, CS-IRPGO (one more training
   launch), mimalloc / glibc tunables.

Every launch needs the user's go; training launches are play sessions, not measurements, and are labelled so in the
log tag (`deck-pgo-train-1`, `deck-bolt-train-1`).

## Sources

- shadPS4 CMakeLists.txt (`-march=x86-64-v3`): https://github.com/shadps4-emu/shadPS4/blob/main/CMakeLists.txt
- LLVM BOLT README (`--emit-relocs`, no-LBR profiles, instrumentation, PIE/.so support):
  https://github.com/llvm/llvm-project/blob/main/bolt/README.md
- BOLT, CGO 2019 (up to 7.0% on top of PGO + LTO; 20.4% for GCC/Clang): https://arxiv.org/abs/1807.06735
- lld: cdsort as the default `--call-graph-profile-sort` (up to 1.5%):
  https://lists.llvm.org/pipermail/llvm-commits/Week-of-Mon-20231009/1221710.html
- Chrome 85 clang PGO (up to 10% page load; 7-11% JS): https://www.phoronix.com/news/Chrome-85-Clang-PGO
- AMD branch sampling (BRS, Zen 3, Linux 5.19): https://www.phoronix.com/news/Linux-5.19-Perf
- Frame-pointer cost (Fedora): https://lwn.net/Articles/919940
- x86-64-v3 package benchmarks (small, mixed gains): https://www.phoronix.com/review/cachyos-x86-64-v3-v4/4
- PCSX2 Linux build (clang, lld): https://github.com/PCSX2/pcsx2/wiki/10-Building-on-Linux ; AppImage CI:
  https://github.com/PCSX2/pcsx2/actions/runs/9224003454/workflow
- perf_event_paranoid levels: https://sysctl-explorer.vstack.com/kernel/perf_event_paranoid/
