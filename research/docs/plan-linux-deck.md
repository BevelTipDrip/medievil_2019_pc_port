# Plan: Linux build, Steam Deck first, shared-memory fixes, remote dev loop

Status: research only, 2026-11-03. Nothing was built, launched or committed. Parent: [START-HERE.md](START-HERE.md),
work queue items 9 and 10 ([plan-work-queue.md](plan-work-queue.md)). Related: [study-guest-memory-direct.md](study-guest-memory-direct.md),
[study-frame-cost-2026-10-29.md](study-frame-cost-2026-10-29.md), [fix-inventory.md](fix-inventory.md),
[gpu-threading-reference.md](gpu-threading-reference.md). Bloodborne reference: `bloodborne_pc` at `3c57da4` (0.51).

Marks: **[code]** read in this repo's code or scripts at `lab` `8db8193`. **[report]** from an upstream issue or PR
(other people's hardware; not the user's). **[web]** from a web source (Sources at the end). **[inferred]** reasoning,
not checked. "Verified" in the project's sense (the user confirmed it in game) applies to nothing on this page yet.

## Answer

1. **The Linux build exists, and other people have run it on the Steam Deck. On `lab` it is untested.**
   - Upstream merged a Linux build and an AppImage script in 1.3.7 (#13). Testers ran that build on a Steam Deck and
     on desktop Linux.
   - `lab` has ~2,900 changed lines in `src/` and `gpu/shim/` since upstream 1.3.8, all built and run only on
     Windows. Every Windows call in them reads as guarded [code]. Whether `lab` compiles on Linux is unknown.
   - Two known Linux bugs are not fixed on `lab`:
     - the AvPlayer join hang after the first movie (upstream PR #68, open);
     - AppImage bundling that can clash with Mesa (#13 follow-up 2).
2. **Deck numbers so far are poor, and the evidence points at RADV rendering slowly before memory copies.**
   - Upstream builds on a Deck LCD: graveyard 7-13 FPS under Proton, 13-18 FPS with the native AppImage [report].
   - On one desktop RX 5700 XT, the same scene took 3.8x the GPU time per frame on RADV that it took on AMD's Windows
     driver. About eight passes took 10-25x longer [report].
   - Finding and fixing those RADV slow passes is likely worth more than any shared-memory change.
3. **Build environment: a Debian 13 distrobox on the Deck itself.** It needs no install on this PC. Podman and
   distrobox ship with SteamOS 3.5+ [web], and the container lives in `/home`, so SteamOS updates leave it alone.
   WSL2 on this PC is the faster alternative if the user wants it later.
4. **Quick baseline: yes.** The current `lab` Windows package (`dist/medievil-windows`) can run under Proton as a
   non-Steam game. It runs on the same RADV driver through Wine, so treat it as a lower bound (section 4).
5. **Shared-memory (UMA) fixes: real but second-order. Top three:**
   - U1: check which memory heaps the Deck exposes and size the port's fixed GPU buffers to them.
   - U2: CPU writes straight into host-visible arena memory instead of staging plus a GPU copy.
   - U5: in the long term, guest memory as GPU-visible memory (Bloodborne's AMD model).

   Three Linux CPU items probably matter more on a 4-core Deck than any of them (section 5.4):
   - the 960 Hz vblank tick spin;
   - pool views kept across a decommit, which is Windows-only today;
   - spin-yield waits.
6. **Dev loop: SSH with key login.**
   - Deploy: tar over ssh with Git Bash's own `tar` and `ssh` (rsync optional, through MSYS2).
   - Build: `ssh deck distrobox enter ...`.
   - Runs: in Game Mode the user presses Play on a Steam shortcut that runs the next prepared experiment. That press
     is the user's go for that launch.
   - Logs come back with `scp`, or are streamed with `ssh tail -F`.

## 1. Current state of Linux

### 1.1 What exists [code]

| Piece | State | Where |
|---|---|---|
| Build script | `build.sh` has a Windows branch (MSYS2, CMake builds the exe) and a Linux path: CMake builds `out/gpu/libgow3gpu.so` (shared library), then the C runtime is compiled and linked by a direct `$CC` call with `-no-pie -rdynamic` | `build.sh:8-35` (Windows), `:36-52` deps check, `:73-80` GPU library, `:91` runtime link, `:94` gpu-check, `:95-112` `--test` unit tests |
| Compiler | Linux default is `cc` (often GCC). **GCC 14 fails on `src/actor_hook.h:26`** (`_Static_assert` in a header included from C++); Clang builds it [report, PR #70]. README says `CC=clang CXX=clang++` | `build.sh:36-43`, `packaging/linux/README-Linux.md` "Building it yourself" |
| Vulkan headers | Need **1.4.363+** (`VK_DRIVER_ID_MESA_KOSMICKRISP`, `VK_VALVE_shader_mixed_float_dot_product`); 1.4.328 fails [report, PR #70] | `vk_instance.h/.cpp` |
| GPU library linkage | Shared on Linux, static on Windows; runtime symbols resolve against the executable (`--allow-shlib-undefined`) | `gpu/CMakeLists.txt:147-151`, `:172-174` |
| LTO / PGO | LTO on by default on both; PGO `use` when `pgo/*.gcda` exist (GCC-style flags) | `build.sh:56-62`, `:73-74`; `gpu/CMakeLists.txt:30-40` |
| Guest memory | memfd + `mmap` (Linux) vs pagefile section + placeholders (Windows) | `src/runtime_memory.c:436-461` (Linux), `:163-435` (Windows) |
| Write tracking | Signals + `mprotect` by default; **userfaultfd write-protect opt-in** (`GOW3_UFFD=1`), Linux only | `gpu/shadps4/video_core/page_manager.cpp:234-371`, `:395-409` |
| Packaging | `packaging/linux/appimage.sh` (pinned, SHA-256-checked downloads of appimagetool, runtime, python-build-standalone; bundles SDL3/FFmpeg/xxHash + Python/Tk), `AppRun` (`--play`, `--run`, `--vulkan-info`, glibc check), `build-tk-xft.sh`, `README-Linux.md` | `packaging/linux/*` |
| AppImage data | `~/.local/share/medievil-pc` (`GOW3_DATA_DIR`), launcher settings `~/.config/medievil-launcher` | `AppRun:49-56` |
| Launcher / run.py | Linux branches present: locale language, `xdg-open`, `.desktop` shortcut, process-group stop, launcher stays alive while the game runs | `launcher/medievil_launcher.py:36`, `:143-147`, `:1992-1996`, `:2104-2120`, `:2133-2188`, `:2290`; `run.py:25-49`, `:124`, `:275` |
| Unit tests | `build.sh --test` builds and runs 8 C tests on Linux; the C++ GPU tests are CMake targets for both | `build.sh:95-112`, `gpu/CMakeLists.txt:237-375` |

### 1.2 Windows-only code paths that Linux lacks [code]

Every one below is behind `_WIN32` with a POSIX branch or a stub, so none blocks a Linux build. Some are performance
fixes that Linux therefore does not get:

| Windows-only | Effect on Linux | Where |
|---|---|---|
| **M1 + "decommit keeps access"**: pool views kept across a decommit | **Linux unmaps and remaps on every pool decommit and commit.** `pool_keep_views()` returns 0 off Windows. Each change takes the address-space lock for writing and sends a TLB shootdown to every core. See L2 (5.4) | `src/runtime_memory.c:999-1003`, `:1055-1084` |
| Host-map statistics (`GOW3_HOST_MAP_STATS`) | Not available on Linux (the code sits inside the Windows host-mapping block) | `src/runtime_memory.c:200-204` within `:163-435` |
| Sampling profiler (`GOW3_SAMPLE_FILE`) | Stub on Linux; use `perf` instead | `gpu/shim/gow3_sampler.cpp:4-160` |
| Dedicated core / CPU sets, `THREAD_PRIORITY_ABOVE_NORMAL` | Not on Linux (nice values only) | `gpu/shim/gow3_threads.h:13-92` |
| Crash minidumps (`<user>\crashdumps`) | Linux: systemd-coredump (`coredumpctl`) instead | `src/probe.c:130-170`, `:799-801` |
| NVIDIA Reflex limiter | NVIDIA only; falls back to the Precise limiter on AMD/Intel, either OS | `scripts/graphics_options.py:275-283` |
| DLSS bridge (MSVC DLL) | Not built on Linux | `packaging/windows/build_dlss.sh` |
| `medievil-play.exe`, `.rc` resources, the updater | Linux uses `AppRun --play`; the Update button opens the release page | `gpu/CMakeLists.txt:197-235` |
| Research tools: `run-game.ps1`, ETW/WPR, PresentMon, nsys, ngfx | Linux counterparts: `perf`, RGP via `MESA_VK_TRACE=rgp`, MangoHud / Steam overlay | `research/tools/` |
| Linux-only, for contrast: "GPU thread us/draw" per-thread rusage | Prints on Linux, 0.00 on Windows | `amdgpu/liverpool.cpp:1468` |

### 1.3 Stale or dead pieces [code]

- `run.sh` is the God of War III script: `../CUSA01623` and `gow3-probe` (`run.sh:37`, `:58-62`). Linux runs go through
  `run.py` (AppRun `--run`). Do not use `run.sh`.
- `shell.nix` lists `gcc`, not clang, and its first comment points at `native_probe/` (GoW3 era). Today's tree does not
  build with GCC.
- `launcher/vulkan_check.py` is Bloodborne's driver picker for an AppImage that bundles Mesa. Nothing in MediEvil calls
  it. `tests/test_packaged_vulkan.py` still tests it. This AppImage uses the host's driver (`appimage.sh:160`).

### 1.4 Known Linux problems on `lab`

| Problem | Evidence | Status on `lab` |
|---|---|---|
| **Hang after the first movie (black screen, audio continues)**. `Kernel::Thread::Join` detaches a self-joining AvPlayer thread and races the game's `sceAvPlayerStop` join. "Most starts hung here", any driver | [report] upstream PR #68 (open, by dvilelaf, tested on RADV and llvmpipe) | **Not fixed.** `gpu/shim/core/libraries/kernel/threads.h:29-46` still has the self-detach. Port #68 first (GPL, credit the author) |
| AppImage bundles every library `ldd` finds (FFmpeg pulls libzstd, libexpat, liblzma, libwayland, libgbm, gnutls, Samba libraries ~150 MB). Host Mesa (RADV on the Deck) can then bind the older bundled copies | [report] #13 owner review item 2; #30 "Samba private libraries (150 MB)" | Not fixed (`appimage.sh:153-176`) |
| `appimage.sh` calls `/opt/mdeps/bin/patchelf` without `MEDIEVIL_DEPS`; libXft/fontconfig/freetype dev files needed | [report] #30 | Not fixed (`appimage.sh:146`) |
| Mesa 25.0 RADV shows a black picture; 25.3 works | [report] PR #69 | Check the Deck's Mesa version (S0) |
| Light flickers into black boxes in white-lit levels | [report] #40: Steam Deck **and** Xbox Ally X on AMD's Windows driver, so AMD-specific rather than Linux-specific | Unknown. Bloodborne sets `RADV_DEBUG=nohiz` for Polaris/Vega/APUs for black halos (`bloodborne_pc/docs/CHANGES_0.5.md:65`). The Deck is RDNA2, so this is a diagnostic only |
| Map softlock (a level does not load from the map) | [report] #50, Windows build under Proton on a Deck | Unknown |
| Shadow issues on RADV; a depth-bias change (`VK_EXT_depth_bias_control`) was split out of #13 | [report] #13 discussion | Not in upstream or `lab` |

### 1.5 What was tested, by whom

| Test | Result | Source |
|---|---|---|
| Native build + run, Fedora 44, RX 5700 XT, RADV Mesa 26.2.5, upstream 1.3.7 | Menus, gameplay, PipeWire audio, gamepad work. 25.7 FPS vs 60 on Windows, same PC and scene | [report] #30 |
| AppImage on a Steam Deck LCD, SteamOS 3.9.0 | Graveyard 13-18 FPS (Windows build under Proton: 7-13) | [report] #13 comments |
| Windows build under Proton GE 11.7, Deck LCD, 1.3.5, 720p | Graveyard 8-12 FPS, 76 ms frames; GPU 44%, CPU 58%, no thread at 100%; cutscenes steady 30 | [report] #21, #16 |
| Ubuntu 22.04 + Nix, RX 6800 XT and a Ryzen 7000 iGPU, Mesa 25.3 | Runs past the intro with the #68 fix | [report] PR #68, #69 |
| `lab` on Linux | **Never built or run** | — |

**Not verified here:** whether `lab` compiles on Linux; whether the 1.3.7 Linux fixes survived the `lab` merge in
working form (they are in the tree, `cc8da6c` is an ancestor of `lab`); the Deck's SteamOS, Mesa and glibc versions.

### 1.6 What #30 says about where Linux loses time [report]

Same PC, same dump (CUSA12982 01.02), same settings, 1080p (the full report is in upstream #30):

|  | Windows | Linux (RADV 26.2.5) |
|---|---|---|
| FPS | 60 | 25.7 |
| GPU busy per profile frame | ~4.95 ms | ~18.75 ms (3.8x) |
| "between submissions: GPU idle" | 2.85 ms | 0.09 ms |
| Host-copy wait per frame | ~1.5 ms | 8-16 ms |
| Draws / copies / write faults per frame | 870 / 90 / ~760 | identical |

- About eight passes cost 10-25x more on RADV, adding up to ~14.7 of the 18.8 ms:
  - 1080p passes with a D32S8 depth target (vs `864df900`, `2eefc6cc`, `43ac1669`);
  - 64x64 two-target RGBA16F passes (vs `f7457b58`);
  - a 120x68 pass;
  - cs `986dfb36`;
  - "image upload R8Unorm 1920x1088" at 1.24 ms.
- Most other passes cost the same on both.
- Things tried without any change: `GOW3_UFFD=1`, 8 copy threads, `FRAMES_AHEAD` 2/3, GPU clocks forced high, readback
  modes, LTO off, GCC vs clang, feedback-loop layouts off. Linux shows those layouts on, Windows off.
- The CPU waits for the GPU, so the GPU work itself is slower on RADV.
- This port's per-pass profiler (`GOW3_GPU_PROFILE=1`) is the tool to repeat on the Deck. RGP is the tool to explain it
  (step S7).

## 2. Hardware facts that shape the plan

| Fact | Value | Source |
|---|---|---|
| Deck CPU | Zen 2, 4 cores / 8 threads, up to 3.5 GHz | the user |
| Deck GPU | RDNA2 "VANGOGH", 8 CU, RADV | [web] Geekbench entry |
| Memory | 16 GB LPDDR5, shared | the user |
| Display | 1280x800, 60 Hz LCD (40-60 Hz selectable in Game Mode); not VRR | [web] |
| UMA frame buffer (BIOS) | 1 GB default, 4 GB maximum. SteamOS balances RAM and VRAM regardless of the setting, so it matters less than on Windows | [web] |
| RADV memory types (general) | Type 0 device-local, 1 host-visible coherent, 2 device-local + host-visible, 3 host-visible cached, across VRAM, CPU-visible VRAM and GTT heaps. **The Deck's exact table is unknown: read it with `vulkaninfo` first (S0)** | [web] Mesa commit "radv: Use enum for memory heaps" |
| `radv_enable_unified_heap_on_apu` | driconf option, default false, enabled per game (RDR2): one heap on APUs | [web] Phoronix, Mesa MR 18884 |
| RADV host-pointer import of a memfd | **Fails** (`ErrorInvalidExternalHandle`); Bloodborne uses Vulkan memory exported as dma-buf on AMD instead | `bloodborne_pc/docs/CHANGES_after_0.4.md:35-43` |
| SteamOS glibc | 2.41 on 3.7 per one forum post. Check with `ldd --version` (S0) | [web] Godot forum |
| SteamOS root | read-only; `/home` survives updates; updates can reset `/etc` (re-enable sshd) | [web] |

The port's thread heuristics already account for a 4c/8t CPU. Copy threads are a quarter of the hardware threads
(2 on the Deck, `gpu/shim/gow3_copy.cpp:26-30`). The two-stage draw pipe turns on at 8+ threads, so it is on for the
Deck (START-HERE knobs). The texture cache's garbage collector uses the driver's current budget on integrated GPUs, a
Bloodborne Deck fix this port inherited (`texture_cache/texture_cache.cpp:1975-2000`, `vk_instance.cpp:1024-1035`).

## 3. Build environment

### 3.1 Options

| Option | Fits? | Pros | Cons |
|---|---|---|---|
| **A. Debian 13 distrobox on the Deck** (podman, preinstalled) | **Recommended** | Nothing to install on this PC. glibc 2.41 should match SteamOS 3.7 (check). Clang 19, SDL3 and FFmpeg 7 come from Debian. Lives in `/home`, so updates keep it. Builds run where the binary runs: no copy of ~100+ MB per iteration | First full build slow on 4 cores (estimate 20-40 min, [inferred]); incremental builds fine. Vulkan-Headers 1.4.363 must be fetched (header-only). Building heats the Deck: plug it in |
| B. WSL2 (Debian 13) on this PC | Good second option | 7800X3D builds fast; Git Bash can reach the files. Same Debian recipe as A | WSL is not installed: admin + reboot + Virtual Machine Platform. Ship ~150 MB of binaries per iteration over Wi-Fi |
| C. Docker / Podman Desktop on this PC | No | — | Both need WSL2 (or Hyper-V) underneath, so B with more layers |
| D. Arch distrobox on the Deck | For experiments | Newest packages, nothing to fetch by hand | Arch glibc is newer than SteamOS's, so binaries run only inside the container. Then the container's Mesa is used, not SteamOS's, and measurements would not match what players get |
| E. Nix | No | `shell.nix` exists | GCC-based and stale (1.3); `/nix` on a read-only root needs workarounds. Nix-built binaries point into `/nix/store` (Bloodborne had to patchelf them) |
| F. Cross-compile from MSYS2 CLANG64 | No | — | Needs a Linux sysroot with every dependency (SDL3, FFmpeg, Boost, Zydis...); fragile |
| G. Steam Runtime "sniper" SDK | No | Valve's recommended native target | Debian 11 base: too old for this C++23 / Vulkan 1.4 tree without rebuilding most dependencies [web] |

### 3.2 Recommended: A, concrete steps

The user does steps 1-2 once (section 6 has the SSH part). Everything else Claude can run over SSH.

1. Check the host: `ldd --version | head -1` (needs ≥ 2.41 for a Debian 13 build to run on the host), `cat /etc/os-release`,
   `which distrobox podman`.
2. Create the container. Distrobox shares `$HOME` and gives the container passwordless `sudo`:
   ```bash
   distrobox create --name mdev --image docker.io/library/debian:trixie --yes
   distrobox enter mdev -- true
   ```
3. Dependencies (README-Linux list + PR #70 corrections; package names [inferred] for Debian 13, check each):
   ```bash
   distrobox enter mdev -- sudo apt-get update
   distrobox enter mdev -- sudo apt-get install -y clang lld cmake ninja-build pkgconf python3 git curl file patchelf \
     binutils g++ glslang-tools spirv-headers libvulkan-dev libsdl3-dev libavcodec-dev libavformat-dev libavutil-dev \
     libswscale-dev libswresample-dev libboost-dev libfmt-dev libtsl-robin-map-dev libxxhash-dev libzydis-dev \
     libvulkan-memory-allocator-dev libx11-dev libxft-dev libfontconfig-dev libfreetype-dev ccache
   # Vulkan headers 1.4.363 (header-only), into a prefix the build finds first:
   distrobox enter mdev -- bash -c 'git clone --depth 1 -b v1.4.363 https://github.com/KhronosGroup/Vulkan-Headers ~/mdeps-src/vh &&
     cmake -S ~/mdeps-src/vh -B ~/mdeps-src/vh/b -DCMAKE_INSTALL_PREFIX=$HOME/mdeps && cmake --install ~/mdeps-src/vh/b'
   ```
   Any package that is missing or too old goes into `~/mdeps` the same way. `MEDIEVIL_DEPS` is the README's
   hook for such a prefix (`appimage.sh:19-20`, `:34-36`).
4. Build (dev builds without LTO, much faster to link; release and measurement builds with LTO on):
   ```bash
   distrobox enter mdev -- bash -c 'cd ~/medievil-dev/src && CC=clang CXX=clang++ GOW3_LTO=OFF \
     CMAKE_PREFIX_PATH=$HOME/mdeps bash build.sh 2>&1 | tail -40'
   distrobox enter mdev -- bash -c 'cd ~/medievil-dev/src && CC=clang CXX=clang++ bash build.sh --test'
   ```
   `CMAKE_PREFIX_PATH` reaches `find_package(Vulkan)` [inferred: may also need `-DVulkan_INCLUDE_DIR`]. The runtime's
   own `$CC` line takes Vulkan includes from `pkg-config` (`build.sh:53`): put `~/mdeps/share/pkgconfig` first in
   `PKG_CONFIG_PATH` or headers from two versions mix.
5. Package as an AppDir (the first run downloads and checks the pinned tools; later runs reuse them):
   ```bash
   distrobox enter mdev -- bash -c 'cd ~/medievil-dev/src && PATCHELF=$(command -v patchelf) bash packaging/linux/appimage.sh'
   ```
   For dev runs use `out/MediEvil.AppDir/AppRun` directly on the **host** (SteamOS's Mesa, glibc, PipeWire). It carries
   its own Python, SDL3 and FFmpeg. The squashed `dist/MediEvil-x86_64.AppImage` is for releases.
6. `build.sh` applies `gpu/patches/fsr-vulkan/*.patch` with `git -C <submodule> apply` (`build.sh:68-72`). A source
   tree deployed by tar has no `.git`. `git apply` works outside a repository, the reverse check succeeds and the
   patch is skipped [inferred; confirm on the first build]. If it does not, deploy with `.git` (section 6.4, option b).

## 4. Quick baseline: the Windows build under Proton

**Yes, do it first.** The current `lab` package is `dist/medievil-windows/` (built 2026-11-03 14:35;
`MediEvil.exe` = frozen launcher, `Play MediEvil.exe` = `--play`). Upstream testers already run upstream builds this
way on the Deck [report]. `lab` carries the CPU fixes measured on this PC (fix-inventory: lean barriers, HZB remap,
S2, ring reuse, R2/R3, M1...). So this baseline also answers whether `lab` already beats upstream on a Deck.

Steps (the user, once; Claude copies the files):
1. Claude: `tar -cf - -C dist medievil-windows | ssh deck 'mkdir -p ~/Games && tar -xf - -C ~/Games'`, and the game
   folder the same way (section 6.4).
2. The user, Desktop Mode: Steam > Add a Non-Steam Game > `~/Games/medievil-windows/MediEvil.exe`; Properties >
   Compatibility > force Proton Experimental (or the GE build the testers used).
3. Game Mode: start it. In the launcher choose the game folder (`Z:\home\deck\Games\CUSA11227`), output 1280x720 or
   1280x800, VSync on, limit 30. Then Play: the user's go.
4. The launcher writes `<saves folder>/last_run.log` (`launcher/medievil_launcher.py:1921-1925`). With the default
   saves folder that is inside `~/Games/medievil-windows/` [inferred], reachable over ssh.

What differs from a native build:

| Area | Under Proton | Native |
|---|---|---|
| GPU driver | The same RADV, through winevulkan (thin 64-bit thunks) | RADV directly |
| Guest memory | Windows placeholders + section views, emulated by Wine (on mmap). M1 keep-views active | memfd + mmap, no keep-views |
| Write faults | SIGSEGV > Wine's exception dispatch > the port's vectored handler (slower per fault [inferred]) | SIGSEGV handler directly (`src/probe.c:313-320`) |
| Locks / waits | SRW locks, `WaitOnAddress`, waitable timers through Wine (NTSync on recent SteamOS [web]) | pthreads / futex, `sleep_until` |
| Thread placement | CPU sets / priorities mostly ignored by Wine [inferred] | nice values |
| Limiter | Reflex absent, Precise used (same as native AMD) | same |
| Paths, saves | `Z:\` paths, launcher in Wine Tk | native paths, `~/.local/share/medievil-pc` |

It is a lower bound for native performance, and a fair comparison of `lab` against upstream on the Deck, but not a
measure of Linux-specific fixes.

## 5. Shared-memory (UMA) and handheld fixes

### 5.1 What UMA changes, and what it does not

On the Deck, "VRAM" is a carve-out of the same LPDDR5 as system memory. A GPU copy from staging into a device-local
buffer moves bytes within one DRAM, using ~2x the bytes of shared bandwidth (88 GB/s total, [inferred] typical
figure). There is no PCIe hop.

**What gets cheaper:**
- GPU reads of host memory. This is the cost that made Bloodborne keep hot data in VRAM and measure 48 vs 210 FPS
  on a discrete card (study-guest-memory-direct.md §1).
- CPU writes into device memory.

**What stays the same:**
- Every reason the port copies at all: tiled images, write tracking, and fences that signal before the GPU runs the
  work (study-guest-memory-direct.md §2, "the precondition").
- The CPU time of every memcpy. On the Deck the CPU, not memory bandwidth, is the scarce resource [inferred from
  #21: "CPU 58%, threads 50-70%" with low FPS].

Caveat [inferred]: on AMD APUs, GPU access to *cacheable* (snooped) system memory goes through the CPU's coherence
fabric and is slower than to the carve-out or to write-combined GTT. That is why U5 below needs a bandwidth probe
first.

### 5.2 Every copy or wait between guest memory and GPU memory [code]

| # | Site | What happens today | Memory type today (Deck: [inferred] which heap) | UMA / host-visible device memory changes |
|---|---|---|---|---|
| C1 | **Buffer uploads** `SynchronizeMemory` > `UploadCopies` (`buffer_cache/buffer_cache.cpp:1335-1386`, `:1388-1469`) | Dirty pages: guest > staging memcpy on the copy threads (`Gow3Copy::Async`, 1 MiB groups) or in the caller's batch (< 1 MiB); then `vkCmdCopyBuffer` staging > arena; submission and guest fences wait for the host copies (`Scheduler::WaitHostCopies`) | staging `HostUncached` (VMA prefer host, sequential write: write-combined GTT); arena = sparse, blocks from the first `eDeviceLocal` type (`:497-505`) | If arena blocks come from a DEVICE_LOCAL\|HOST_VISIBLE type and are mapped, the CPU can write guest bytes **straight into the arena**. No staging, no GPU copy, no render-pass end. Needs a guard: the GPU may still read the old bytes (U2) |
| C2 | **Precise CPU writes** `ProcessCpuWritesLocked` (`:1136-1185`, staging + copy at `:1156-1160`) | D2: guest libc writes into GPU-written pages queued as exact bytes, then staging + `CopyBuffer` before the next draw | as C1 | Same as C1: write into the mapped arena when the block's last GPU use is complete |
| C3 | **Small read-only buffers / constants** `ObtainBuffer` stream path (`:1026-1043`), constant ring (`renderer_vulkan/vk_constant_ring.h:32-42`) | CPU memcpy per bind into a ring the GPU reads directly; S2 binds clean ones from the arena | `Stream`: VMA prefer device + host access (`buffer_cache/buffer.cpp:34-63`): DEVICE_LOCAL\|HOST_VISIBLE when present. Stream 128 MiB, constant ring 96 MiB | **Already zero GPU-copy.** On the Deck the cost is footprint: 224 MiB of a possibly 1 GB carve-out (U1) |
| C4 | **Arena residency** `EnsureResident` (`:1262-1333`) | New 64 KiB blocks [inferred AMD sparse alignment] from 32 MB chunks allocated ahead (`ResidencyPool`); BDA page-table entries via staging + copy | `arena_memory_type_index` = first DEVICE_LOCAL type; BDA table `DeviceLocal` (`:508-511`), [inferred] ~128 MiB with 64 KiB blocks over 2^40 | Choose a type that is also HOST_VISIBLE and map chunks persistently (enables C1, C2, C5); the page table could be written by the CPU directly |
| C5 | **Buffer downloads** `DownloadMemory` (`:800-878`) | D3: wait for the newest write's submission, copy arena > `HostCached` staging on the readback queue (compute-only family, `vk_instance.cpp:517-528`), then memcpy into guest memory. Fallback: graphics-queue copy + `scheduler.Finish()` (`:859-864`) | `HostCached` staging (cached GTT) | With mapped arena chunks: wait for the tick, then memcpy **from the arena mapping** into guest memory. No GPU copy, no second queue, no semaphore |
| C6 | **Image uploads** `RefreshImage` (`texture_cache/texture_cache.cpp:1617-1734`) | guest > `HostUncached` staging (`ObtainBufferForImage`, `buffer_cache.cpp:1079-1111`, copy threads) > detile compute into `DeviceLocal` staging (`texture_cache/tile_manager.cpp:227-276`) > `CopyBufferToImage` | WC GTT, then device-local | Images still need detiling and an optimal layout. UMA saves nothing unless the detiler reads guest memory in place (U5). Gameplay uploads are ~0 after C3/N-11/the intro-movie fix (fix-inventory), so this mostly affects level loads |
| C7 | **Image readbacks** `ProcessDownloadImages` / `DownloadImageMemory` (`texture_cache.cpp:303-460`, `:489-520`); HZB, luminance, stale readbacks | image > `HostCached` staging copy, later memcpy into guest; `Finish()` on sync paths (`:370-376`, `:451`, `:518`) | cached GTT | Optimal-tiled images cannot be read in place: no change. The `Finish()` sync paths are the costly ones on any GPU |
| C8 | **Write tracking** `PageManager` (`page_manager.cpp`), runtime `runtime_memory_gpu_protect`, fault windows (`buffer_cache.cpp:555`) | `mprotect` + SIGSEGV (Linux default) or userfaultfd WP (`GOW3_UFFD=1`); 66k-200k pages/s unprotected on the 4090 machine (study-guest-memory-direct.md §3) | — | **UMA alone removes nothing**: tracking exists because the arena is a copy. Only U5 (GPU reads guest memory in place) removes it for in-place ranges |
| C9 | **Staging pool** (`renderer_vulkan/vk_staging_buffer_pool.cpp:23-31`, `:41-53`) | 16 MiB blocks, 512 MiB kept and pre-faulted (`GOW3_STAGING_KEEP_MB`) | WC GTT | Footprint: 512 MiB of 16 GB shared. Halve on the Deck (U1) |
| C10 | **Pool commit / decommit** (`src/runtime_memory.c:1055-1084`) | Linux: unmap + remap reserved per decommit (no keep-views) | — | Not UMA. Linux-specific CPU cost (L2) |

### 5.3 UMA fixes, ranked

The 4090 numbers (study-guest-memory-direct.md §3, study-frame-cost §1) are the only cost data so far. On the Deck the
same work happens at ~30-40 FPS on a CPU 2-3x slower per thread [inferred]. So per-frame costs that are small at
200 FPS matter more there. Every gain below is **[inferred] until measured on the Deck**.

| Rank | Fix | Expected gain on the Deck | Evidence | Effort | Risk | Low-end desktop too? |
|---|---|---|---|---|---|---|
| **U1** | **Heap audit and footprint.** Log the memory types and heaps at start, plus which heap each fixed allocation lands in: stream 128 MiB, constant ring 96 MiB, BDA table, residency chunks, staging keep 512 MiB. Size the fixed buffers by heap size on integrated GPUs: stream / ring 32-64 MiB if the device-local heap is ≤ 1 GB; staging keep 128-256 MiB (`GOW3_STAGING_KEEP_MB`, `GOW3_CONSTANT_RING_MB` exist). Test BIOS UMA 1 GB vs 4 GB once | Prevents the worst case. If the arena + images overflow a 1 GB device-local heap, the kernel evicts and refills buffers behind the driver: stutter, not a few %. The upside is avoiding a cliff, not a steady gain | Bloodborne's Deck GC bug (budget counted against the wrong heaps: constant evict/re-upload, `bloodborne_pc/docs/CHANGES_2026-10-03.md` "Steam Deck: запуск и сборщик текстур"); this port has that fix but not for its fixed buffers. 32 MiB constant ring ran full on the PC (N-14), so do not go below ~48 MiB without checking the "ring full" line | S (1-2 days) | L | iGPU laptops and handhelds on Windows too (Ally, 780M) |
| **U2** | **Direct arena writes.** Residency chunks from a DEVICE_LOCAL\|HOST_VISIBLE type when one exists, mapped persistently. C1 / C2 write guest bytes straight into the arena when the target blocks' last GPU-use tick is complete; otherwise the old staging path. Behind a toggle bit for A/B in one launch | Removes upload copies, their barriers, and the ~3 render-pass ends a frame they cause (study-guest-memory-direct.md §3: 2,388 per 5 s). On RADV every barrier is a cache flush / wait (cost per barrier [inferred] higher than NVIDIA). Also removes one memcpy hop (guest > staging > GPU copy becomes guest > arena). Plausibly a few % of frame time | 4090: uploads 380-585 MB/s, ~2-3 MB a frame; pass ends from uploads ~3/frame. The tick bookkeeping exists (`WriteTicks()` per 64 KiB page for D3, `buffer_cache.cpp:42`) | M (1-2 weeks) | M (data races if the guard is wrong: wrong vertices, flicker) | Desktops with ReBAR / SAM (the 4090 has a 23.6 GiB DEVICE_LOCAL\|HOST_VISIBLE heap): measure there too. Write-combined writes over PCIe cost about what staging writes cost |
| **U3** | **Downloads from the mapped arena** (C5) | Small in steady play (downloads 0/s after D2, fix-inventory); helps the fault and load spikes that still download | T-E1 "Buffer downloads/s" 0 | S once U2 maps chunks | L | ReBAR desktops (CPU reads of WC VRAM are slow, so probably not) |
| U4 | Write tracking A/B: `GOW3_UFFD=1` vs signals (C8) | Unknown. #30 saw no change on an 8-core desktop. On 4 cores the `mprotect` address-space lock and TLB shootdowns weigh more [inferred] | #30 perf: `mprotect` 7% of the draw-recording thread | none (one launch) | L | — |
| **U5** | **Guest memory as GPU-visible memory** (Bloodborne 0.5 AMD model: guest direct memory allocated as Vulkan host-visible *cached* memory, exported as dma-buf, mapped by the runtime at guest addresses, bound in place into the sparse arena; hot blocks rebound to device memory). On UMA the in-place read penalty mostly disappears, so fewer blocks need moving | The only structural change that removes uploads (C1, C6's CPU copy) and write tracking (C8) for in-place ranges. Potentially large on the Deck, but **only with GPU-ordered fences**: the guest rewrites per-frame data while the GPU still reads it (Bloodborne built a translated command processor for this; `GOW3_ASYNC_FENCES` here measured slower: the guest waits on fences) | study-guest-memory-direct.md §1-4; `bloodborne_pc/gpu/shim/bbport_guest_memory.cpp:46-65` (picks HOST_VISIBLE\|HOST_CACHED, not device-local), `:495-536`; RADV cannot import a memfd host pointer (§2) | L (weeks to months; Linux only, since Windows cannot alias Vulkan memory at guest addresses) | H | Linux AMD desktops; on discrete GPUs Bloodborne needed the VRAM rebinding |

**Gate for U5:** two standalone probes on the Deck, run before any design work. They are separate GPU programs, not
the game, but the user is told before they run.
- `bloodborne_pc/tools/sparse_host_probe.c`: can dma-buf-exported host memory back a sparse buffer, and how fast does
  the GPU read it compared with device-local memory?
- `bloodborne_pc/tools/host_import_probe.c`: confirms the memfd import failure on this Mesa.

If cached host memory reads at ≥ ~70% of carve-out speed, U5 is worth a design doc. Below that, U2 is the ceiling.

### 5.4 Linux and 4-core items (not UMA, likely bigger on the Deck)

| # | Item | Why | Where | Effort / risk |
|---|---|---|---|---|
| L1 | **Vblank tick = cap, no 960 Hz spin** | The default is a 960 Hz precise tick (launcher default `present_tick_hz=960`, `present_precise_tick=1`). Each tick spins 700 us of every 1.04 ms period (fix-inventory "Precise vblank tick"; `PreciseWait`), about 2/3 of one core. The Deck has 4 cores. Linux `sleep_until` is precise to ~50-100 us [inferred], so the spin can be short. Handheld preset: tick = cap (30 or 40), the rule found on PC (START-HERE "a cap holds when the tick is the cap") | `gpu/shadps4/core/libraries/videoout/driver.cpp:1083-1101`, `:1193-1205`; `launcher/medievil_launcher.py:343-346`; `scripts/graphics_options.py:265-273` | S / L |
| L2 | **Keep pool views across a decommit on Linux** (M1 + keep-access) | UE4 commits and decommits 64 KiB pool pages thousands of times a second (T-H1). On Linux each is `munmap` + `mmap` under the table lock (`pool_decommit_impl`). That takes the address-space lock for writing and sends TLB shootdown IPIs to all 8 threads. On Windows the fix cut decommit time 320 to 41 ms per 5 s. Linux version: the memfd mapping stays (physical pages unchanged), so a commit back at the same address is a table update only | `src/runtime_memory.c:999-1003`, `:1055-1084`, `:436-461` | S-M / M (pool page accounting is shared with Windows) |
| L3 | **Spin-yield waits** | #30's perf profile: `sched_yield` 15% and `DRM_SYNCOBJ_TIMELINE_WAIT` 10% of the draw-recording thread on Linux. Yield spins steal a hardware thread from guest threads when there are only 8 | draw pipe waits (`gpu/shim/gow3_threads.h`, `vk_draw_pipe.h`) | M / M |
| L4 | **Thread budget** | Guest threads + stage A + stage B + recorder + 2 copy threads + draw-prep workers + async shader and pipeline warm-up (`WarmupThreadCount(hardware_concurrency)`, `vk_pipeline_serialization.cpp:1068`) + vblank + audio on 8 hardware threads. Count runnable threads in a Deck run (`top -H` / perf sched) before tuning | — | S to measure |
| L5 | **AvPlayer join hang** (port PR #68) | Correctness: most starts hang after the first movie without it [report] | `gpu/shim/core/libraries/kernel/threads.h:29-46` | S / L |
| L6 | **RADV slow passes** (section 1.6) | Likely the largest single gain for the Deck and all Linux AMD users. Unknown cause: candidates are D32S8 HTILE / layout decompressions around sampled depth, feedback-loop layouts, tiny MRT passes with many barriers, the R8 upload path [inferred] | per-pass `GOW3_GPU_PROFILE=1` + RGP | M-L / M |
| L7 | **Black-box light flicker** (#40, also Windows AMD) | Correctness on all AMD | — | ? |

### 5.5 Handheld settings

| Setting | Deck value to start from | Why | Helps low-end desktop? |
|---|---|---|---|
| Output | `1280x800`. Fallback `1280x720` (gamescope letterboxes) if the game mishandles 16:10 (untested [inferred risk]) | native panel; 1.0 MP vs 8.3 MP at 4K | — |
| Render scale | 100 at first; then 67-75 (UE4 temporal upsampling) or output `960x600` + gamescope FSR 1 upscaling (no port work) | GPU load | yes (render scale) |
| Quality | `console`, then `medium` / `low` (shadow sizes `scripts/ue4_override.py:236-240`) | 8 CUs | yes |
| Effects resolution | 50 (`r.SeparateTranslucencyScreenPercentage`) | translucency / particles are fill-heavy | yes |
| Frame rate | 30, or 40 with the panel at 40 Hz (gamescope refresh slider) | 60 is out of reach; 40 is the usual Deck middle | 30 cap yes |
| Limiter | the port's own cap (`fps_limit` 30/40), **tick = cap** (L1), VSync on, `display_vrr=0`. Gamescope's limiter off: Steam's limiter added ~110 ms latency at 30 FPS in 2022 tests, and an in-game cap did not [web] | today's defaults are VRR-desktop values. "Auto: half refresh" with `display_vrr=1` gives 29 on a 60 Hz panel (`refresh/2 - 1`, fix-inventory "Auto frame limits") | yes (tick = cap on any fixed-refresh display) |
| `frames_queued` | 1 (default) | latency; 2 hurt the lows on PC | — |
| Background warm-up | `parallel_warmup` off, or fewer threads, while playing | shares 8 threads with the game | yes, ≤ 8 threads |
| Staging keep | 128-256 MiB (U1) | shared memory | iGPU |
| Measurement runs | fixed GPU clock (Game Mode performance menu "manual GPU clock") and fixed TDP; plugged in | repeatable numbers on a power-limited APU | — |

A **"Handheld" preset** in the launcher bundles these. Detect the Deck by DMI (`/sys/class/dmi/id/product_name`
"Jupiter" / "Galileo" [inferred]) or by `IsIntegrated()` with a ≤ 1280-wide display, and offer the preset rather than
forcing it.

## 6. Remote dev loop for the Deck

### 6.1 Options

| Option | Deploy | Logs back | Start / stop | Verdict |
|---|---|---|---|---|
| **SSH (sshd on SteamOS, key login)** | `tar \| ssh tar -x` with Git Bash's tools today; rsync optional (below) | `scp`, or live `ssh tail -F` into a local file | `ssh` + `systemd-run --user` (Desktop Mode) or a Steam shortcut (Game Mode) | **Recommended**: everything Claude's Bash tool can drive, no daemon, nothing installed on the read-only root |
| Syncthing | continuous both ways | continuous | none | No: rescans and hashes during runs (skews the Deck's frames), no atomic deploy of a binary + libs, no way to start anything |
| SMB share | the Deck mounts a Windows share (`sudo mount -t cifs` each boot) | same share | none | No: needs root mounts and Windows sharing; running the game from the network is slow; no start/stop |
| Tiny HTTP log server (`python3 -m http.server` on the Deck) | — | `curl` | none | Logs only. Adds nothing SSH lacks |

**rsync:** SteamOS appears to ship it (guides use it with no install step, and Valve's devkit tooling uploads with
rsync [web]; check with `which rsync`). This PC has no rsync. Git Bash and MSYS2 (`C:\msys64\usr\bin`) have
`ssh`, `scp`, `tar` and `zstd` [code: checked]. To get rsync on this side, add MSYS2's package
(`pacman -S rsync` in the MSYS2 shell: the user's decision). Then call it from MSYS2's bash with MSYS2's own ssh
(`-e /usr/bin/ssh`), not Windows' `C:\Windows\System32\OpenSSH`. Tar over ssh is enough to start.

### 6.2 One-time setup on the Deck (the user)

1. Desktop Mode (Steam button > Power > Switch to Desktop) > Konsole.
2. `passwd`: set a password for `deck` (needed for `sudo`).
3. `sudo systemctl enable --now sshd`.
4. `ip -4 addr show wlan0`: note the address. A DHCP reservation in the router keeps it fixed.
   (`steamdeck.local` through mDNS may work from Windows too [inferred].)
5. On this PC, in a terminal where the password can be typed (Claude cannot type it):
   ```bash
   ssh-keygen -t ed25519 -f ~/.ssh/id_ed25519_deck -N ""          # Claude can run this one
   cat ~/.ssh/id_ed25519_deck.pub | ssh deck@<ip> 'mkdir -p ~/.ssh && chmod 700 ~/.ssh &&
     cat >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys'   # the user types the password once
   ```
6. `~/.ssh/config` on this PC. Git Bash and Windows OpenSSH both read `C:\Users\Ty\.ssh\config`:
   ```
   Host deck
       HostName <ip>
       User deck
       IdentityFile ~/.ssh/id_ed25519_deck
       IdentitiesOnly yes
       BatchMode yes
       ConnectTimeout 5
       ServerAliveInterval 15
   ```
7. Optional, after `ssh deck true` works without a password: `PasswordAuthentication no` in `/etc/ssh/sshd_config`
   on the Deck.
8. Power: in Desktop Mode, System Settings > Power Management: no sleep on AC. A sleeping Deck drops SSH.
9. After a SteamOS update, if `ssh deck` is refused, repeat step 3. Updates can reset `/etc`; `/home` (keys, distrobox,
   game, builds) stays [web].

### 6.3 Layout on the Deck

```
~/medievil-dev/src/          source tree (deployed from lab)
~/medievil-dev/data/         GOW3_DATA_DIR for dev runs: medievil.ini, out/, user/ (dev saves), mods
~/medievil-dev/logs/<tag>.log  one log per run
~/medievil-dev/next-run.env  the next experiment (tag + variables), written by Claude, read by deck-play.sh
~/medievil-dev/bin/deck-run.sh, deck-play.sh
~/Games/CUSA11227/           game files (01.00 or 01.02: open question)
~/Games/medievil-windows/    the Proton baseline package
```
Dev runs use their own data directory and saves. The user's save folder on this PC is never copied or touched
(START-HERE rule). Copying a save to the Deck is the user's call. `medievil.ini` on the Deck follows the PC's rules:
back it up before an edit, re-read it before a launch.

### 6.4 Commands Claude runs from here (Git Bash)

**Check the connection:** `ssh deck 'uname -r; cat /etc/os-release | head -3'`.

**Deploy the source:**
- (a) Default: tracked files from the working tree, uncommitted edits included, submodules with their applied
  patches:
  ```bash
  cd /c/Users/Ty/MediEvil/medievil_2019_pc_port
  git ls-files -z --recurse-submodules | tar --null -T - -czf - |
    ssh deck 'mkdir -p ~/medievil-dev/src && tar -xzf - -C ~/medievil-dev/src'
  ```
  - New untracked files are not included until they are added to git.
  - Deleted files stay on the Deck: now and then run `ssh deck 'rm -rf ~/medievil-dev/src'` and deploy again.
- (b) With git history on the Deck, if needed (step 3.2.6): `git bundle create <scratch>/lab.bundle lab`, then `scp`
  it, then `ssh deck 'cd ~/medievil-dev/src && git fetch ~/lab.bundle lab && git checkout -f FETCH_HEAD'`.
  This is not a push.
- (c) With rsync (after `pacman -S rsync` in MSYS2):
  `C:/msys64/usr/bin/bash -lc "cd /c/Users/Ty/MediEvil/medievil_2019_pc_port && git ls-files --recurse-submodules | rsync -az --files-from=- -e /usr/bin/ssh . deck:medievil-dev/src/"`.

**Game files, once** (several GB over Wi-Fi; USB-C Ethernet is faster):
`tar -cf - -C "/c/Users/Ty/MediEvil/Game Files" CUSA11227 | ssh deck 'mkdir -p ~/Games && tar -xf - -C ~/Games'`.

**Build** (never while a run is going on the Deck):
```bash
ssh deck "distrobox enter mdev -- bash -lc 'cd ~/medievil-dev/src && CC=clang CXX=clang++ GOW3_LTO=OFF bash build.sh > out/build.log 2>&1; echo exit \$?; tail -30 out/build.log'"
ssh deck "distrobox enter mdev -- bash -lc 'cd ~/medievil-dev/src && bash packaging/linux/appimage.sh > out/appimage.log 2>&1; echo exit \$?'"
```
Long builds: run them in the background (Bash `run_in_background`) and wait for the exit notice.

**Prepare the next experiment (no launch):**
```bash
ssh deck 'cat > ~/medievil-dev/next-run.env' <<'EOF'
TAG=deck-s5-baseline-800p
GOW3_FRAME_STATS=1
GOW3_GPU_PROFILE=1
EOF
```

**Launch, only after the user's explicit go for this launch:**
- Game Mode (normal case): the user presses Play on the "MediEvil dev" shortcut. That press is the go.
  `deck-play.sh` reads `next-run.env`, refuses to start while a build lock exists or the game already runs, and writes
  `logs/$TAG.log`.
- Desktop Mode (quick checks), Claude after the go:
  `ssh deck '~/medievil-dev/bin/deck-run.sh'`. It wraps `systemd-run --user --unit=medievil-dev --collect` with
  `DISPLAY=:0`, `WAYLAND_DISPLAY=wayland-0` and `XDG_RUNTIME_DIR=/run/user/1000`, so the game survives the ssh session
  ending.

**Watch and pull logs:**
- Live: `ssh deck 'tail -n +1 -F ~/medievil-dev/logs/<tag>.log' > research/docs/data/<tag>.log` (in the background;
  light).
- After the run: `scp deck:medievil-dev/logs/<tag>.log research/docs/data/<tag>.log`.
- Use the `deck-` tag prefix so Deck logs never mix with PC logs. The existing scripts (`flip_lows.py`, `series.py`,
  ...) read them unchanged.

**Stop (ask the user first, START-HERE rule):** `ssh deck 'systemctl --user stop medievil-dev'` (Desktop Mode), or
`ssh deck 'pkill -TERM -f medievil-runtime'`.

**Crash information:** `ssh deck 'coredumpctl list medievil-runtime | tail -3'`. For a backtrace,
`coredumpctl debug` inside the distrobox, where gdb lives.

**Profiling:**
- CPU: `perf` from the distrobox (it shares the host's PID namespace) on the running game. Check whether SteamOS's
  `kernel.perf_event_paranoid` needs lowering (needs sudo).
- GPU: `MESA_VK_TRACE=rgp MESA_VK_TRACE_TRIGGER=/tmp/rgp` in `next-run.env`. `touch /tmp/rgp` captures a frame. Pull
  the `.rgp` file and open it in Radeon GPU Profiler on this PC. The permissions RGP capture needs on SteamOS are
  [inferred], not checked.

The two scripts (`deck-run.sh`, `deck-play.sh`, ~40 lines each) belong in `research/tools/deck/` when step S1 starts.
This page only specifies them.

## 7. Proposed order, with tests

Each launch needs the user's go and changes one thing. Builds and deploys never overlap a run on the Deck. A run on the
PC is not affected by Deck builds, but Deck builds over SSH should not overlap the user's PC measurement runs either
(memory rule).

| Step | What | Test / exit criterion |
|---|---|---|
| **S0** | Deck facts, no launch: `cat /etc/os-release`, `uname -r`, `ldd --version`, `which rsync python3 distrobox podman vulkaninfo`, `vulkaninfo --summary` (Mesa version, VANGOGH), `vulkaninfo` memory heaps / types, BIOS UMA size, free space | The table in §2 filled with Deck values. Decides: glibc for the build container, Mesa ≥ 25.3 (PR #69), the heap sizes U1 works with |
| **S1** | SSH + keys + layout (§6) | `ssh deck true` with no prompt; deploy and pull back a test file; `next-run.env` written and read back |
| **S2** | Proton baseline with the current `lab` Windows package (§4), 1280x800 (or 720), console quality, limit 30. Two spots: crypt spawn, graveyard | `last_run.log` pulled. FPS, 1% / 0.1% lows from the per-second lines, stage-B busy, host-copy waits; then one run with `GOW3_GPU_PROFILE=1`. Compare with the upstream numbers (§1.5) |
| **S3** | Build environment: distrobox `mdev`, deps, `build.sh`, `build.sh --test`, AppDir | Builds with clang; unit tests pass; `out/MediEvil.AppDir/AppRun --vulkan-info` on the host lists RADV VANGOGH. Fix `lab` compile breaks (expected small, all behind `_WIN32` guards), one commit each |
| **S4** | Must-have fixes: port PR #68 (AvPlayer join); `appimage.sh` bundles an allowlist (SDL3, FFmpeg libs, xxHash and their non-system dependencies), the #13 follow-ups and PR #70 README corrections; drop or repair `vulkan_check` and its test | Unit tests; `ldd` of the AppDir shows no bundled libzstd / libexpat / libwayland / libdrm. First native launch (the user's go): five starts in a row pass the logo into the crypt |
| **S5** | Native baseline, same spots and settings as S2 | Native vs Proton, one launch each. If native is not ahead, look at the faults and the decommits first (L2, U4) |
| **S6** | Handheld settings, one per launch: tick = cap (L1) first, then `display_vrr=0` / fixed cap, effects 50, render scale, quality | CPU usage drop from L1 (`/proc/stat` sampled by the run script, or the Steam overlay), lows and frame time at 30/40. Ends in the Handheld preset |
| **S7** | RADV GPU diagnosis (L6): per-pass profile at 800p compared with the same passes on the PC; RGP capture of a slow pass; one-launch diagnostics (`RADV_DEBUG=nohiz`, feedback-loop layouts off, `GOW3_SKIP_READ_BARRIERS=0` A/B) | Name the cause of the 10-25x passes; a fix shows up as GPU ms per frame in the profile. Also check #40's flicker |
| **S8** | U1 heap audit and sizing; L2 Linux keep-views; L3 waits; U4 uffd A/B | Heap log line and no "memory pressure" lines in a 10-minute run; decommit cost (add a Linux counter); toggle A/B within one launch where a toggle exists |
| **S9** | Probes for U2/U5 on the Deck (`sparse_host_probe`, `host_import_probe`, standalone) | Can mapped DEVICE_LOCAL\|HOST_VISIBLE memory back the arena? Cached-host read bandwidth vs carve-out |
| **S10** | U2 behind a toggle bit; U3 after it | A/B in one launch through `GOW3_TOGGLE_FILE`: render-pass ends from uploads → ~0, stage-B µs per draw, lows. Then the same A/B on the PC (ReBAR) |
| **S11** | U5 decision | Only if S9 passes and S10 leaves uploads or faults as a measured limit. Its own design doc first |
| Later | Release: AppImage on the fork's releases, README-Linux "tested on Steam Deck" with numbers | the user's go to publish |

## 8. Open questions

1. Which game build goes on the Deck: 01.00 (the reference: patches and addresses checked against it) or the merged
   01.02 (`E:\MediEvil\CUSA11227-0102`, best performance so far)? Maybe both, for comparisons.
2. Deck model: LCD (60 Hz, 40-60 range) or OLED (90 Hz, 45-90)? It decides the caps worth offering.
3. SteamOS version and channel; BIOS UMA setting (1 GB default or 4 GB). (S0 answers both.)
4. Is the user fine with a Debian distrobox in `/home` on the Deck (~3-5 GB with toolchain and build tree [inferred])?
   Or would they rather install WSL2 on this PC for faster builds later?
5. Should Claude add rsync to MSYS2 (`pacman -S rsync`), or stay with tar over ssh?
6. Game Mode runs through a "MediEvil dev" Steam shortcut that runs whatever `next-run.env` says: is that the way the
   user wants to give the go? Or should every launch be started by Claude in Desktop Mode after a written go?
7. Should a copy of the user's save go to the Deck (dev data directory only), or should runs start from new saves?
8. Is it fine to port upstream PR #68 (open, not merged; GPL-2.0, by dvilelaf) into `lab` with credit?
9. Does MediEvil render correctly at 16:10 (1280x800), or does it need 1280x720 letterboxed? Test in S5/S6.

## Sources

- Upstream issues and PRs (bc160201983/medievil_2019_pc_port):
  - #13 [Linux build and AppImage](https://github.com/bc160201983/medievil_2019_pc_port/pull/13) (review follow-ups, Deck AppImage numbers)
  - #16 [Steam deck performance](https://github.com/bc160201983/medievil_2019_pc_port/issues/16)
  - #21 [Low FPS in 2nd stage on Steam Deck](https://github.com/bc160201983/medievil_2019_pc_port/issues/21)
  - #30 [Linux (Fedora 44 / RADV) ~2.5x slower](https://github.com/bc160201983/medievil_2019_pc_port/issues/30)
  - #40 [Steam Deck graphics flickering](https://github.com/bc160201983/medievil_2019_pc_port/issues/40)
  - #50 [Map softlock on Steam Deck](https://github.com/bc160201983/medievil_2019_pc_port/issues/50)
  - #68 [AvPlayer hang fix](https://github.com/bc160201983/medievil_2019_pc_port/pull/68)
  - #69 [Mesa 25.0 RADV black picture](https://github.com/bc160201983/medievil_2019_pc_port/pull/69)
  - #70 [README-Linux accuracy](https://github.com/bc160201983/medievil_2019_pc_port/pull/70)
- Distrobox and Podman preinstalled on SteamOS 3.5+: [distrobox Steam Deck guide](https://distrobox.it/posts/steamdeck_guide)
- SSH on the Deck (`passwd`, `systemctl enable --now sshd`, updates reset `/etc`): [pimylifeup](https://pimylifeup.com/steam-deck-ssh/), [gist](https://gist.github.com/andygeorge/eee2825fa6446b629745ea92e862593a)
- rsync used on SteamOS without install, Valve devkit upload by rsync: [overkill.wtf backup guide](https://overkill.wtf/how-to-clone-backup-your-steam-deck), [rubenwardy devkit post](https://blog.rubenwardy.com/2022/10/16/steam-deck-upload-game/), [Steamworks: load games](https://partner.steamgames.com/doc/steamdeck/loadgames)
- RADV memory heaps / types: [Mesa commit "radv: Use enum for memory heaps"](https://android.googlesource.com/platform/external/mesa3d/+/4ae84ef%5E%21/); VANGOGH device name: [Geekbench entry](https://browser.geekbench.com/v5/compute/5462115)
- Unified heap on APUs: [Phoronix](https://www.phoronix.com/news/RADV-RDR2-APU-Fix), [Mesa MR 18884](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/18884)
- `VK_EXT_external_memory_host` rules: [Vulkan refpage](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_external_memory_host.html)
- UMA frame buffer size: [Windows Central](https://www.windowscentral.com/gaming/pc-gaming/how-to-increase-vram-on-steam-deck), [pimylifeup](https://pimylifeup.com/steam-deck-increase-vram/)
- SteamOS glibc 2.41: [Godot forum](https://forum.godotengine.org/t/steam-deck-cant-launch-linux-build-due-to-outdated-glibc-version-build-targeting-newer-glibc-version/143218)
- Steam Linux Runtime sniper SDK: [steam-runtime](https://github.com/ValveSoftware/steam-runtime)
- Deck frame limiter latency and 40 Hz: [Tom's Hardware](https://www.tomshardware.com/uk/news/steam-deck-framerate-limiter-egregiously-raises-input-latency), [GamingOnLinux](https://gamingonlinux.com/2023/11/valve-refines-new-steam-deck-unified-frame-limit-slider-to-show-refresh-rate), [overkill.wtf settings guide](https://overkill.wtf/how-to-graphic-settings-steam-deck)
- NTSync in SteamOS 3.7.20: [pulse.bot](https://www.pulse.bot/gaming/news/steamos-3720-released-with-ntsync-driver-plus-big-new-steam-client-update-for-all-f9741ba6-4bb8-47ed-9b68-92cdfb4d26e9)

## Decisions and answers (the user, 2026-11-03)

| Question | Answer |
|---|---|
| Game build on the Deck | **01.02** (as on the PC) |
| Deck model / UMA frame buffer | **OLED**; the BIOS VRAM setting is unknown: U1's heap log reads it |
| Where to build | **WSL2 on this PC** (the user installs it; admin + reboot). Building on the Deck only if it would perform better, which it should not: same compiler and flags. Use an older-glibc userland in WSL (or a container in it) so the binary runs on SteamOS |
| Steam shortcut press as the go for a launch | **yes** |
| Copy the user's save to the Deck | **yes** (copy, never move; the PC save folder is untouched) |
| Port upstream PR #68 (dvilelaf, movie hang) | **yes, with credit, but first check it is not the same as our fix** (the intro movie that kept playing, T-M1) and write down what it fixes and how |
| Proton baseline | **yes**, set up after the cursor test (`4bb5565`) |

Notes from the user:
- **Other Linux forks to read before porting anything:** `spacepepperoni/medievil_ps4_Linux` ("for Linux/Steam
  Deck"; branches `linux-steamdeck`, `steam-deck-wayland`, `skinning-fix`, `stencil-draw-clear`, `main`, ...; pushed
  2026-11-03), `TheRhysWyrill/medievil_2019_pc_port-linux`, `dvilelaf/medievil_2019_pc_port` (PR #68's author).
- **The upstream Deck numbers (issues #21, #13) are probably upstream's problem, not the Deck's:** a video of
  spacepepperoni's build shows Deck gameplay near 30 FPS. Measure our build before drawing conclusions.
- **Tick rate on the Deck:** lower than 960, a balance between long frames getting longer and spin CPU raising frame
  times. First try the fix already noted for the spin (plan-stage-b-split.md, after T-P120: a shorter
  `GOW3_TICK_SPIN_US`, or the precise tick off at high rates since the precise limiter times the slots; L1 above).
- The user is setting up SSH on the Deck (step S0).

## The Deck as found (2026-11-03, read over SSH)

- Host alias `deck` (`~/.ssh/config`, key `~/.ssh/medievil_deck`), 192.168.8.193, user `deck`; key login works.
- **SteamOS 3.6.20** (build 20241030.1, kernel 6.5.0-valve22), **glibc 2.39**: a WSL Debian 13 build (glibc 2.41)
  would not start there. Build in an older userland (Ubuntu 22.04 / Debian 12 container in WSL), which also suits
  other distributions. The user updates SteamOS after the file copy (newer Mesa / RADV; measure on the updated one).
- 8 threads, 14 GiB visible RAM; **UMA frame buffer 1 GB** (`mem_info_vram_total`), GTT 8 GB.
- 378 GB free on the internal drive; SD card 477 GB at `/run/media/deck/9C33-6BBD` (slower, not used).
- `rsync`, `tar`, `distrobox` present on the Deck.
- Copied: `~/MediEvil/medievil-windows` (the v2.0.0 package, `0d0097a`) with a copy of the user's saves in its
  `user/savedata`; `~/MediEvil/CUSA11227-0102` (01.02, 44 GB) on the internal drive.
- WSL on the PC: Debian 13.5 (glibc 2.41), 16 threads, 31 GB, 956 GB free.

## The user's direction on performance (2026-11-03)

- **Linux + AMD slowness is treated as fixable**, not as a hardware limit: spacepepperoni's Deck build is shown near a
  stable 30 FPS, against upstream's 7-18 FPS claims. Read that fork's changes first (branches `skinning-fix`,
  `stencil-draw-clear`, `linux-steamdeck`, `steam-deck-wayland`) for what they fixed on RADV.
- **Target: 60 FPS** on the OLED Deck (panel up to 90 Hz; gamescope can set the panel to 60 Hz, so tick = cap = 60, the
  PC rule). The precise tick likely off and / or a lower tick rate; decide from measurements (spin CPU vs long frames).
- **GPU memory:** on Linux the driver allocates from shared memory (GTT) on demand, so the BIOS UMA size matters most
  on Windows. Two things to look at: (1) claiming the port's GPU memory up front at boot instead of during gameplay
  (allocations / frees mid-game are a known stutter source, START-HERE "Stage B's GPU memory allocations"), and
  (2) memory use at 1280x800, which should be far below 4K: retail direct memory (5,056 MB, not the 4K 7,104), smaller
  render targets, smaller port caches and ring buffers sized for the resolution (U1).

## Proton run 1 (2026-11-03, v2.0.0 package, SteamOS 3.6.20 before the update; log `data/deck-proton-1.log`)

- Black screen in both **fullscreen and borderless** (the user tried both): audio plays, the logo and intro movies
  decode (149 / 159 frames), presents at a steady 60 FPS; skipping to the main menu: audio loops, no picture, then
  "The game exited (code 1)" right after a 4096x4096 image create. Not the display mode.
- `HLE thread: join failed (No such device or address)` is also in every PC run: harmless here.
- Upstream 1.3.8 displayed under Proton (slowly, issue #21): suspect our presenter changes (precise vblank tick,
  presents on the vblank thread, present-mode choice, Reflex path). Ranked suspects and tests:
  `study-fork-spacepepperoni.md` section 2 (in progress).
- Next run: `PROTON_LOG=1 %command%` in the Steam launch options (the exit reason).
- The launcher offers no **1280x800** output: add 16:10 sizes for Deck mode, then check the game at 16:10.

**Cause found (kernel log of that boot, read 2026-11-03):** a **GPU hang** in both launches. `ring gfx_0.0.0 timeout`
(the Deck's `amdgpu.lockup_timeout` 5 s for gfx), blamed on `medievil-runtime`, thread `shadPS4:Present` (pids 6542 at
18:25:46 and 6797 at 18:26:51, ~36 s after the second launch started), then a MODE2 GPU reset and `Skip scheduling IBs`
for the guilty context: the game ran on without a GPU (audio, movie decoding, presents counted) and exited with
code 1 later. The job named is the one on the hardware when the ring stopped, not necessarily the one that hung it.
That run was on SteamOS 3.6.20 / RADV 24.0.99.

**The Deck after the update (18:47):** SteamOS **3.9.5**, glibc **2.43**, Mesa / RADV **26.2.0-devel** (vulkan-radeon
26.2.0_devel, SteamOS 26.05.19). A Debian 13 (glibc 2.41) build now runs on it; spacepepperoni's AppImage too.

Next Deck launches, one change each:
1. Same package and settings on the updated SteamOS, `PROTON_LOG=1 %command%` (does the new RADV still hang?).
2. If it hangs: `RADV_DEBUG=hang` added (RADV writes a dump of the hung command buffer to `~/radv_dumps_*`; slow).
3. Then the suspects from [study-fork-spacepepperoni.md](study-fork-spacepepperoni.md) section 2 (1080p output with
   a 67% render scale; `GOW3_DEPTH_SAMPLE_IN_PLACE=0`; presenter timing switches).

## Proton run 2 (2026-11-03, after the update; v2.0.0 package; log `data/deck-proton-2.log`)

- **Runs, no GPU hang** (kernel log clean). SteamOS 3.9.5, `RADV VANGOGH` radv 26.1.99, Vulkan 1.4.350, pageable
  memory on, maintenance8 on. The hang of run 1 went with the driver update (the log says Borderless both times).
- Settings: 1280x720 borderless, render scale 100, Console preset, effects 75, `fps_limit=60`, vsync, tick 120 precise,
  limiter precise, every performance patch default.
- 136 five-second windows (~11 min): FPS median **52**, min 29, max 60; Dan's Crypt partly a steady 60, map screen
  30-60, graveyard slower (the user). CPU and GPU both ~80% (the user).
- Under Proton: `wineserver` ~19% and `xalia` (Proton accessibility helper, hooks the Tk launcher) ~24% of a core in
  a sample taken after the game exited.
- Next (one change per launch, same route): Graphics quality Low; render scale 67%; preshadows off; then CPU
  switches (precise tick off, draw workers); then the native build.
