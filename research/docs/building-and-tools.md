# Building and tools

Parent: [START-HERE.md](START-HERE.md). Do not re-install the toolchain or re-derive the build: it is here.

## Paths

| What | Where |
|---|---|
| Workspace | `C:\Users\Ty\MediEvil` (workspace `CLAUDE.md` here) |
| Port (this repo) | `C:\Users\Ty\MediEvil\medievil_2019_pc_port` |
| Bloodborne reference | `C:\Users\Ty\MediEvil\bloodborne_pc` |
| MGS2 reference (read only) | `C:\Users\Ty\MGS2-Lab` |
| MSYS2 | `C:\msys64` (CLANG64 environment) |
| Game (CUSA11227 01.00) | `C:\Users\Ty\MediEvil\Game Files\CUSA11227` (eboot sha256 matches `scripts/patches.py:72`); source `Game Files\MediEvil-CUSA11227_V0100.pkg` |

## Toolchain (installed 2026-10-18)

- MSYS2 via `winget install MSYS2.MSYS2`, then in CLANG64:
  ```bash
  pacman -S --needed git patch \
    mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
    mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
    mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
    mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
  ```
  Versions at install: clang 23.1.3, cmake 4.4.4, ninja 1.13.2.
- Python: **use 3.14** (`py -3.14`), which has pillow + cryptography. Plain `python` on PATH is 3.8 and breaks the
  launcher (`str | None`, `removesuffix`). **`py -3` / `pyw -3` also pick 3.8.10 here** (no py.ini, no PY_PYTHON:
  the legacy py launcher ranks the Python-install-manager 3.14 below 3.8), so the repo's `MediEvil Launcher.cmd`
  starts the launcher on 3.8 (corrected 2026-10-18; earlier notes said 3.14). Start the launcher with
  **`research\tools\launcher.cmd`** (pins `pyw -3.14`, sets `GOW3_UE4_BOOT_4K=1`, which the launcher passes to the
  game). 3.10 also exists without the packages.

## Build

From PowerShell (non-interactive), through the script (see Traps 1):
```powershell
$env:MSYSTEM='CLANG64'; & C:\msys64\usr\bin\bash.exe -l /c/Users/Ty/MediEvil/medievil_2019_pc_port/research/tools/build-clang64.sh
```
In an interactive CLANG64 shell: `bash research/tools/build-clang64.sh` (extra arguments go to `build.sh`, e.g. `--test`).
Log: `out/build.log` (ends with `build.sh exit: N`); GPU library log `out/gpu-build.log`.
Outputs `out/medievil-runtime.exe` (~140 MB), `out/medievil-gpu-check.exe`, `out/medievil-play.exe`.
`build.sh` applies `gpu/patches/fsr-vulkan/*.patch` to the submodule once. LTO is off on Windows.
First build 2026-10-18 at `783b25a`: ~1 minute, 3 harmless warnings (C23 empty initializer in fsr-vulkan; two
`DWORD` printf formats in `gpu/shim/gow3_sampler.cpp:75,77`).
`out/medievil-gpu-check.exe` (needs `C:\msys64\clang64\bin` on PATH) prints
"NVIDIA GeForce RTX 4090 supports live presets" on this machine.

## Run

```powershell
$env:GOW3_GAME_DIR = '<game folder or .pkg>'
py -3.14 run.py
```
or `MediEvil Launcher.cmd`. **Every launch needs the user's go.**

## Profiling and crashes

- `GOW3_FRAME_STATS`: per-frame stage timings (the per-second "Frame stats / Draw pipe / Stall" lines are in every
  log anyway).
- `GOW3_SAMPLE_FILE=<prefix>` + `scripts/symbolize_samples.py`: sampling profiler, ~1000 samples/s, rewrites
  `<prefix>-<thread>.txt` every 10 s. Threads: `GpuCmd` (stage A), `DrawRec` (stage B), and `VkRecorder` (lab change
  L2). **Not the game's own threads**: for those use Windows Performance Recorder (`wpr -start CPU` … `wpr -stop
  <file>.etl`, admin), which samples every thread.
- **Crash dumps (lab change L1):** a fatal fault writes `user\crashdumps\medievil-<date>-<time>-<pid>.dmp` and prints
  its path in the log. Open with `cdb -z <dmp> -y C:\Users\Ty\MediEvil\medievil_2019_pc_port\out` then `.ecxr` and
  `kv` (needs the PDB of the exe that crashed). `GOW3_CRASH_DUMP=0` off, `full` adds all memory.

## Research tools

Research data outside git (Ghidra project, RenderDoc captures, dumps, PresentMon CSVs):
`C:\Users\Ty\MediEvil\research-data\` (create per tool when first needed). **Never write into `C:\mgspf_tools\`**:
it holds the MGS2/MGS4 projects' data and Ghidra project (`ghidra_project`); only the Ghidra *install* there is shared.

| Tool | Where | Use here | Notes |
|---|---|---|---|
| **Ghidra 12.1.4 + GhidraOrbis 1.0156** (ours) | `C:\Users\Ty\MediEvil\tools\ghidra_12.1.4_PUBLIC` (`ghidraRun.bat`; headless `support\analyzeHeadless.bat`), GhidraOrbis in its `Ghidra\Extensions\`. JDK 21 Adoptium on PATH. | The game's `eboot.bin`: find r.SetRes / 4K pool code for pattern scans (V1), UE4 structures (View uniform buffer, TAA) for M0 | Installed 2026-10-18 as a separate install so the MGS Ghidra 12.1.3 (`C:\mgspf_tools\ghidra_12.1.3_PUBLIC`) and its settings stay untouched; GhidraOrbis's newest release is built for exactly 12.1.4. Ghidra zip sha256 `ddac49f9…d2d4db` = NSA's published hash; GhidraOrbis zip sha256 `4738f393…79501` (GPL-3.0, astrelsky). Downloads kept in `tools\downloads\`. `eboot.bin` (plaintext PS4 SELF, magic `4f 15 3d 1d`) imports directly: loader "Orbis SELF", x86:LE:64 gcc, 920 PS4 imports named from its NID database (headless smoke test 2026-10-18, `Import succeeded`; analyzeHeadless returns 255 under PowerShell even on success). Image base 0x400000, addresses as in `patches/MediEvil.xml`. Project: `research-data\ghidra\` (not MGS's). |
| RenderDoc 1.46 | `C:\Program Files\RenderDoc` (`renderdoccmd.exe`) | Vulkan captures of `medievil-runtime.exe`: frame map (depth, velocity, TAA pass, UI start) for M0; checking MV / FG inputs | Complements the port's own `GOW3_CAPTURE_TRIGGER` / `GOW3_FRAME_DUMP_TRIGGER`. A capture is a game launch: the user's go. |
| WinDbg 10.0.22621 (classic) | `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\windbg.exe` (+ `cdb`, `symchk`, `gflags`) | Crashes / hangs in the runtime | **Attach:** `research/tools/windbg.ps1` (`-Cdb` for console) runs `windbg-medievil.txt`: MS symbols cached in `research-data\symbols`, `sxd av` (first-chance AVs are the GPU page tracker, many per second), `sxd ii` (SSE4a emulation, Intel only), `sxi eh`, and **`bp medievil_runtime!fatal_exception`** printing `.exr` / `.cxr` / `kv`: the runtime terminates inside first-chance handling (`src/probe.c:120-178`), so a second-chance break never comes. **Symbols:** the normal build is DWARF only (WinDbg cannot read it); `research/tools/build-pdb.sh` builds DWARF + CodeView + PDB into `out/gpu-pdb/` without touching `out/`, `--install` copies exe + pdb into `out/` (game closed). Verified 2026-10-18 at `783b25a`: build ~80 s, same 3 warnings, `medievil-runtime.pdb` 48 MB, `symchk` PASSED, `cdb -z` resolves `medievil_runtime!fatal_exception (struct _EXCEPTION_POINTERS *)`, `vectored_fault`, `ue4_setres`. **Installed into `out/` 2026-10-18 02:20** (symchk PASSED on `out/medievil-runtime.exe`). A later normal
`build-clang64.sh` overwrites the exe without a PDB and leaves the old `.pdb` beside it: `windbg.ps1` warns when the
PDB is older than the exe; re-run `build-pdb.sh --install` before debugging. Guest (PS4) code has no symbols; the runtime's own fault report (stderr / log) walks guest frames by rbp. |
| IDA | not installed (maybe later) | Same as Ghidra | Ghidra ships `Extensions\IDAPro` import/export scripts. |
| Khronos validation layer 1.4.363 | `C:\msys64\clang64\bin\VkLayer_khronos_validation.{dll,json}` (pacman `mingw-w64-clang-x86_64-vulkan-validation-layers`, 2026-10-18) | Names bad handles / misuse (crash C1) | Port switch `GOW3_VK_VALIDATION=1` (`GOW3_VK_VALIDATION_SYNC=1` for sync validation, `gpu/shim/core/emulator_settings.h:70-72`); loader needs `VK_ADD_LAYER_PATH=C:\msys64\clang64\bin`. Much slower. |
| `research/tools/run-game.ps1` (+ `tee_run.py`) | ours | Test launches by the user: backs up `medievil.ini`, prints the settings and env of this launch, writes the game's output to `data\<tag>.log` through `tee_run.py` (run.py only prints; the launcher is what writes `last_run.log`, so a scripted run leaves it untouched). Launcher-only settings are not applied (defaults are). | `-Validation`, `-Samples` (into `research-data\samples\<tag>`), `-TextureHelper`, `-Env @{...}`, `-Tag`. Starts the game only when the user runs it. |

MGS2-Lab tools worth copying the *pattern* of (read only, `C:\Users\Ty\MGS2-Lab\MGSHDFix\research\tools\`):
- `sigmap.py`: scans every AoB pattern in the source over an image dump and lists hits per label (unique / multiple /
  missing). Model for checking the V1 pattern scans against 01.00 (and any other build) offline.
- `disasm.py`: disassembles a function by RVA from a dump, names calls from sigmap. Model for quick checks without Ghidra.
- `frame_capture.ps1` + `pm_pacing.py`: PresentMon (+ nvidia-smi) capture and summary of present / display gaps.
  Model for P1 and FG pacing measurements (displayed times, not Present times).
- `summarise.py`: reads the tool's log and prints only what answers the question. Model for `GOW3_FRAME_STATS` summaries.
- `game.ps1` / `config.ps1` / `deploy.ps1` / `close_game.ps1`: shared constants and A/B mode switching without
  deleting files. Model for a `medievil.ini` backup/diff + launch-mode helper.

## Traps

1. **Never pass the build command through PowerShell with inner double quotes** (`bash -lc 'CXXFLAGS="..." bash
   build.sh'`): PowerShell 5.1 strips them, bash runs nothing useful, and the wrapper still reports exit 0 with an
   empty log and no `out/` (two wasted builds, 2026-10-18). Use `research/tools/build-clang64.sh`, and check
   `out/build.log` for `build.sh exit: 0`, not the wrapper's exit code.
2. Plain `python` is 3.8: use `py -3.14`.
6. **The user's PowerShell blocks `.ps1` scripts** (no execution policy set at any scope = `Restricted`; admin does not
   help; Claude's own sessions run with `Bypass`, so they never see it). Give the user commands as
   `powershell -ExecutionPolicy Bypass -Command "& '<script.ps1>' <args>"` from a normal (non-admin) window; `-Command`,
   not `-File`, when an argument is a hashtable (`-Env @{...}`). Permanent alternative is the user's call:
   `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`.
5. **Never rewrite docs with Windows PowerShell `Get-Content -Raw | Set-Content`**: 5.1 reads UTF-8 without BOM as
   ANSI and writes a BOM, so `→` became `â†’` in two docs (2026-10-18, restored from git). Edit with the Edit tool or
   Python (`read_text/write_text(encoding='utf-8')`).
4. **PowerShell: `git commit -F - @'...'@` does not pipe the message**: the here-string becomes extra arguments
   (pathspecs) and nothing is committed. Write the message to a file and use `git commit -F <file> -- <paths>`.
3. **In bash, `exit` inside a `{ ... } > log` group ends the whole script**, not the group: `build-pdb.sh --install`
   silently skipped its copy step (2026-10-18). Logged groups in `research/tools/*.sh` use `( ... )` subshells.
