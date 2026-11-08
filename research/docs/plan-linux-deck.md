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

## Proton runs 3-4 (2026-11-03, settings changed by the user while exploring)

- Run 3: "previous frame's copy" for readbacks on (`stale_readback=1`), VSync off (Mailbox), frame limit 0, VRR off:
  the user, "seemed to help a lot" (several changes at once; not a clean A/B).
- Run 4: **precise vblank tick off** (tick 120): the frame-time graph on the main menu becomes **completely flat at
  60 FPS** (the user). Under Proton the high-resolution waitable timer + 700 us spin goes through Wine; likely the
  Deck default regardless (to recheck natively).
- Native build ready (`8072f54`): `dist/MediEvil-x86_64.AppImage` from WSL Debian 13, copied to the Deck after the run.
- Run 4 log (`data/deck-proton-4.log`): mostly 60 FPS (worst 17-20 ms), graveyard 48-54 FPS with 34-35 ms worst
  frames and 0.5-1 s hitches (first visit); the user: performance "falls apart" in heavy graveyard views with the GPU
  at ~80%: CPU / sync bound.
- **Native deployed** (2026-11-03): `~/MediEvil/MediEvil-x86_64.AppImage` (WSL build of `8072f54`); data
  `~/.local/share/medievil-pc` with a copy of the saves and the Proton run's `medievil.ini` (same settings);
  `~/.config/medievil-launcher/settings.json` with the game folder. `--vulkan-info`: RADV VANGOGH, submission +
  readback PASS; note "lacks blit support for D32S8" (scene scaling).

## Branch `deck-lab` (2026-11-03, the user)

Deck / Linux work happens on `deck-lab`, branched from `lab` at `c863886` (Linux build fixes `8072f54` already on
`lab`, Windows build verified). `lab` stays in its known good state; changes go back by merge or cherry-pick later.

## Native run 1 (2026-11-03, AppImage of `8072f54`, `GOW3_FRAME_STATS=1`; log `data/deck-native-1.log`, partial)

Same settings as Proton run 4 (720p borderless, VSync off, limit 0, precise tick off, stale readbacks). 56 windows:
mostly **60 FPS, worst 21-25 ms** in the graveyard (Proton: 48-54, worst 34-35 ms); a stretch at 93-99 FPS (so 60
is not a gamescope cap); heaviest windows 43-53 FPS with 0.6-1.1 s hitches (first visit: loads / shader builds).
Frame stats: stage B busy median 61%, **max 92%** (the PC's bottleneck, per-draw recording); GPU thread idle median
37%; draws median 1,192 / frame; **write faults 25-55k / s** (to compare with the PC); frame-time stddev median
2.2 ms, p99 median 21 ms. **Native is clearly ahead of Proton**; Wine overhead was a large part of the Proton drop.
Next: where 60 caps without a limit; stage B per-draw cost on Zen 2; write-fault cost; the Deck defaults.

## Native run 2 (2026-11-03, same AppImage; log `data/deck-native-2.log`)

Settings (the user, toward a Deck preset): Low preset, effects 50, preshadows off, shadows 256 / short, view 0.7, LOD
low, no fog / motion blur / DoF / lens flares, engine cap 240 (the earlier 60 cap was the **engine frame rate** setting),
tick 240, no frame caps, VSync off, precise tick off, stale readbacks. `GOW3_SECTIONS=1` did not take effect (no
"stage B ms" lines): repeat for the per-part breakdown.
- Menu ~235 FPS (p99 4-8 ms, odd 16.7 ms frames: the "spikes on a static menu"); Dan's Crypt ~82 FPS at ~372 draws,
  stage B 92%; **graveyard ~58 FPS steady at ~1,180 draws, stage B 94%**, pacing median 16.68 ms, p99 ~21 ms.
  Stage B (draws x per-draw cost) is the limit.
- **Write faults per frame equal the PC's:** Deck graveyard ~44k/s at 58 FPS = ~760 / frame; PC logs 530-760 / frame
  (t-z5, wpr-graveyard, wait-sites ...). The game rewrites the same GPU-read pages every frame; the rate scales with
  FPS (115k/s on the menu). Unknown: cost per fault on Linux / Zen 2 (SIGSEGV + mprotect) vs Windows. "0 hot pages".
- **"Below 60 it locks to 30"**: not the game (frame times 17-21 ms, never 33); gamescope shows frames on the 60 Hz
  panel for one or two refreshes. Options: panel at 90 Hz (11.1 ms steps) or hold 60. Menu cap for the Deck preset.

## Native run 3: stage B by part (2026-11-03, AppImage of `56e323b`; `GOW3_FRAME_STATS=1 GOW3_SECTIONS=1 GOW3_SLOW_FRAMES=1.4`; log `data/deck-native-3.log`)

(The breakdown prints only with `GOW3_SLOW_FRAMES`: run 2 had `GOW3_SECTIONS=1` alone, so nothing printed.)
117 heavy frame samples (>= 1,000 draws, graveyard): frame 16.70 ms, 1,414 draws, stage B busy 93%, packet 15.6 ms.
Per draw, Deck vs the PC's `t-sampler-1` (4K heavy, ~1,466 draws), us:

| Part | PC | Deck | x |
|---|---|---|---|
| packet | 2.35 | 11.04 | 4.7 |
| draw | 1.90 | 7.96 | 4.2 |
| bind | 0.65 | 3.05 | 4.7 |
| textures | 0.43 | 2.02 | 4.7 |
| ObtainBuffer | 0.33 | 1.41 | 4.3 |
| vertex | 0.27 | 1.46 | 5.4 |
| index | 0.12 | 0.44 | 3.7 |
| descriptors | 0.23 | 0.86 | 3.7 |
| texset | 0.19 | 0.94 | 4.9 |
| texslow | 0.12 | 0.76 | 6.3 |
| render state | 0.08 | 0.27 | 3.4 |
| begin render | 0.15 | 0.45 | 3.0 |
| obt_sync | 0.08 | 0.42 | 5.3 |
| **tasks** | 0.31 | **2.36** | **7.6** |
| **flip_present** | 0.19 | **1.24** | **6.5** |

- Draw recording is uniformly 4-5x slower (Zen 2 3.5 GHz / 4 MB L3 vs 7800X3D 96 MB L3); memory-heavy lookups
  (textures, vertex, texslow) worst. Per-draw cuts help both machines; the Deck magnifies them.
- **Out of proportion: flip_present 1.75 ms / frame and the other stage B tasks ~1.6 ms / frame** (PC ~0.3 and ~0.2):
  ~3.3 ms of 16.7 ms in the limiting stage. Suspect the X11 / XWayland present path under gamescope (spacepepperoni
  has a `steam-deck-wayland` branch).
- **Write faults:** our handler time 4.9-6.6 ms/s (~0.1 ms / frame), on game threads, none on stage B. The kernel's
  signal delivery per fault (~3 us of faultprobe's 7.1 us, scratchpad `faultprobe.c`; 24 us with 4 threads, 57 us
  with 8) is outside it: ~13% of a core of game-thread time at 45k/s. Lower priority than stage B.

Order on `deck-lab`: (1) present / tasks in stage B (native Wayland first); (2) per-draw cost (the PC stage B queue
items, measured here); (3) write faults (`GOW3_UFFD=1`, hot pages).

## Native run 4: flip trace (2026-11-03, `GOW3_FRAME_STATS=1 GOW3_FLIP_TRACE`; log `data/deck-native-4.log`, trace `research-data/flip/deck-native-4.csv`)

Same settings as run 3. 13,621 frames, 175 s; graveyard windows 55-66 FPS. Frames of 14-40 ms (6,704), medians, vs
the PC's `t-lab-flip-1` (same filter, 134 frames):

| Stage | PC | Deck |
|---|---|---|
| frame | 16.88 | 16.69 |
| submit -> irq (game submit to stage A) | 0.26 | **12.53** |
| irq -> queued (recorded) | 0.06 | 1.79 |
| queued -> gpu_ready | 0.44 | **15.62** |
| present_start -> presented | 0.15 | 0.94 |

The Deck's GPU finishes each frame ~15.6 ms after its recording ends (the PC's GPU: 0.4 ms): **the GPU is busy for
almost the whole frame**, and submit -> irq is the backlog behind it. With run 3's ~11 ms of stage B recording CPU,
both sides are near 16 ms in the graveyard (the user's overlay: CPU and GPU both ~80-90%). flip_present's 1.75 ms
(run 3) is mostly waiting (present itself 0.94 ms). Next: render scale 75% with the trace on (GPU limit first?).

## Native run 5: render scale 67% (2026-11-03; log `data/deck-native-5.log`, trace `research-data/flip/deck-native-5.csv`)

Graveyard, last ~60 s, vs run 4 (100%): avg 64.4 -> **69.0** FPS, 1% low 44.5 -> **47.0**, 0.1% low 35.9 -> **43.2**,
median frame 16.1 -> 14.2 ms. The user: the frame-time graph still oscillates, many long frames, GPU use ~90% with
dips during them; the problem is the lows, as on the PC. **Long frames come from the game's own frame**
(`flip_slow_stages.py`, 1,064 frames over 1.4x the local median: mean excess game +5.32 ms, stage A +1.23, recording
+0.41, GPU +0.30, present +0.23; largest stage: A 307, game 299, recording 268).
Suspect: write-fault cost on the game threads (~760 / frame; faultprobe 7 us alone, 24-57 us with 4-8 threads, each
mprotect takes the address-space lock for writing). Next: `GOW3_UFFD=1` (userfaultfd write-protect, the lock for
reading); the Deck's kernel 7.2.7 has CONFIG_USERFAULTFD / UFFD_WP / PTE_MARKER_UFFD_WP; `vm.unprivileged_userfaultfd`
is 0 but the port asks for UFFD_USER_MODE_ONLY, allowed unprivileged; falls back to signals if refused.

## Test settings by file (2026-11-03, the user's request; `a15b5cb`)

Steam launch options stay plain. Claude writes each run's settings over SSH to
`~/.local/share/medievil-pc/extra-env.txt` (one `KEY=VALUE` a line, `#` comments; keys starting `GOW3_`, `RADV_`,
`MESA_`, `AMD_`, `VK_`, `SDL_`); `run.py` applies them on every start (Steam, launcher, `--play`), overriding the
environment, and logs `Extra environment (...): ...`. The same file works on Windows in the port's data folder.
Deployed AppImage `e91d5b79` (`a15b5cb`); backups `.56e323b`, `.8072f54` beside it.

## Native run 6: `GOW3_UFFD=1` (2026-11-03, AppImage `a15b5cb`, settings by extra-env.txt; log `data/deck-native-6.log`, trace `research-data/flip/deck-native-6.csv`)

userfaultfd took effect ("GPU: memory tracking with userfaultfd write-protection"). Graveyard, last ~60 s, vs run 5
(signals, same 67%): avg 69.0 -> **62.4** FPS, 1% low 47.0 -> **38.0**, 0.1% low 43.2 -> **26.3**, frame stddev 2.46
-> 2.84 ms; slow frames' game excess **+5.32 -> +8.93 ms** (A +1.85, recording +0.59). Faults unchanged (~47.5k/s);
handler time 2.0-2.2 ms/s. **Worse:** a faulting thread sleeps until the handler thread wakes it. But changing only
the fault path moved the game's share of the long frames: **the ~760 faults a frame are what stretch the game's
frames.** Fix: fewer faults, not a cheaper fault. Reverted to signals (extra-env.txt).

Existing opt-in `GOW3_HOT_PAGES=1` (region_manager.h:220-283; toggle bit 32768) was rejected on the PC: 2 faults made
a page hot for 20 s, re-uploaded on every binding; the set grew to ~14k pages (56 MB), 33 FPS, a GPU ring timeout.
Next: a fault map (which pages and guest sites fault, how many distinct pages a frame, how many every frame), then a
targeted version (pages that fault every frame stay writable and are uploaded once a frame, not per binding).

## Direction (the user, 2026-11-03): build for the Deck's architecture, not around the PC's

- **Unified memory first.** The port's buffer copies, write tracking and faults exist for split pools. On the Deck
  the target is U5 (guest memory as GPU-visible memory, buffers read in place), not patches on the copy model. The
  fault fix ("hot pages once a frame") is a **PC** item (`plan-work-queue.md`, on `lab`), not needed on the Deck.
- **Deck audit of the PC fixes.** Fixes that spend CPU to save PCIe traffic or GPU waits (worker threads, readback
  copies, precise timing, memos) may cost more than they save on 4 Zen 2 cores with shared memory. A/B each switch
  on the Deck (`GOW3_TOGGLE_FILE` switches many live within one launch; the rest by launch), turn off what does not
  pay, and replace with what suits UMA. Result: the Deck preset's patch set.
- U5 order: (1) probes on the Deck, no game launch (`bloodborne_pc/tools/sparse_host_probe.c`,
  `host_import_probe.c`): cached host memory as sparse backing, GPU read bandwidth vs the carve-out; (2) the fence
  precondition: how long the game waits on its fences, what GPU-ordered fences (`VK_AMD_buffer_marker`, on RADV) add;
  (3) a design doc, then buffers-only U5 behind a switch.

## U5 gate 1: probes on the Deck (2026-11-03, SteamOS 3.9.5, RADV 26.1.99; built from `bloodborne_pc/tools`, run over SSH, not the game)

`sparse_host_probe 256`: arena block 4 KiB; guest memory type 5 (flags 0xe: HOST_VISIBLE | HOST_COHERENT |
HOST_CACHED) **backs the sparse arena** (128 MiB bound), CPU write -> GPU read -> GPU write -> CPU read OK; **GPU reads
128 MiB: guest memory 41.2 GB/s, device-local 40.8 GB/s** (gate: >= 70%: passes, no penalty).

`host_import_probe 512`:
1. import of the memfd backing view: **fails** (-1000072003, VK_ERROR_INVALID_EXTERNAL_HANDLE); anonymous memory
   imports (type 5, 7.9 ms).
2. 64 MiB: GPU reads imported memory 2.37 ms (28.3 GB/s); today's path CPU copy to staging 5.08 ms + GPU copy 2.84 ms.
3. coherence CPU write -> GPU read: OK. 4. 2000 mprotect on the guest view 12.4 ms.
5. **Vulkan-allocated guest memory exported as dma-buf** (type 5, 512 MiB, 57.5 ms): two CPU views, MAP_FIXED at an
   offset, aliasing, coherence, write faults (256 / 256) and read() into the view all OK; GPU reads 22.6 GB/s.
   CPU write bandwidth: ordinary 7.7 GB/s, **memfd (today's guest memory) 1.0 GB/s, dma-buf view 4.9 GB/s**; reads
   ~14.5 GB/s for all three.

**Gate 1 passes.** The route is Bloodborne's AMD model: guest direct memory from Vulkan (type 5), exported as dma-buf,
mapped by the runtime at guest addresses, bound in place into the arena. On the Deck no hot-block moves to device
memory are needed (same bandwidth). The memfd cannot be imported. Next: gate 2, the fence cost.

## Native run 7: gate 2 stats (2026-11-03, AppImage `3efee7a`, `GOW3_FRAME_STATS=1` by file; log `data/deck-native-7-crash.log`)

**Crash** after the main menu: `Host fault (signal 11) in libc memcpy, address 0x2005500000` from
`Core::MemoryManager::CopySparseMemory` on a copy thread: a pool page read through the guest mapping while unmapped
(Linux unmaps a pool page on every decommit; Windows keeps the view). Same crash as spacepepperoni `97f0ac9`; fixed
the same way (copies read through the backing view under the region lock, Linux only).

**Correction: "write faults/s" in the frame stats counts pages, not faults.** `Gow3Stats::tracker_faults` is counted
in `RegionManager::CountWriteFaults` for every protected page marked CPU-modified, from any source: a real fault
marks a whole fault window (`BufferCache::ExtendWriteFault`), and the runtime's invalidation hooks (pool commits /
decommits, file reads) and precise CPU writes mark pages too. The new fault map counts actual signals: 59-233/s
(about **1 per frame**, all Garlic-WC pages) against 44-115k/s "write faults". So the "~760 faults a frame" were ~760
pages, the signal cost is negligible, and runs 5-6's conclusion is wrong about the mechanism. What uffd did make worse
is still unexplained. **New suspect:** every invalidation goes through `Rasterizer::InvalidateMemory`
(`vk_rasterizer.cpp:3546`), which **drains the draw pipe first** on the calling thread; if UE4's pool recommits (5-6 a
frame, study-bloodborne-07-memory.md) invalidate, game threads wait for stage B (93% busy) several times a frame. To
measure: invalidations by source and the drain time they cost, on which thread.

Other first readings (menu and early game, 92-235 FPS; not the graveyard):
- Guest event waits: `RHIFrameOffsetThread` waits on VideoOut events (filter -13, idents 6 and 7) ~once a frame,
  **~4.1-4.2 ms each**; `SubmitGPUCommandsAsyncTaskThread` waits on GraphicsCore (filter -14, ident 64, EOP)
  ~once a frame, **2.2-2.5 ms avg**, max 8-35 ms; immediate returns there had 6-15 ms slack.
- Submission lock: armed on ~99% of SubmitDone calls, held 0.3-0.6 ms avg (max 11.7); submits waited 0.3-0.6 ms
  avg, 32-128 ms/s.
- Cache actions per frame: EOP ~36 type 4 + 2 type 20 (flush+inv) + 62 type 40 (bottom of pipe), 1 with an IRQ;
  EOS 8 (CS_DONE 47); EVENT_WRITE 5 type 44 + 15 type 46; compute RELEASE_MEM 2 (tc_wb / tcl1 / tc on half);
  ACQUIRE_MEM ~150 a frame, 3 whole-space.
- Mapped by type: Onion ~783 MiB, Garlic-WC ~940-1000 MiB, flexible 22 MiB; no Onion faults.

## Native run 8: gate 2 in the graveyard (2026-11-03, AppImage `aec1592`, 67%; log `data/deck-native-8.log`)

No crash (copy-thread fix `8e7bb25`). Graveyard windows 64-67 FPS, frame median ~16.5 ms, p99 ~21 ms (one window 29 ms):
- **Real write faults ~1 a frame**, Garlic-WC only (25-81 distinct pages per 5 s, none every frame); **Onion 0** with
  ~1.2 GB Onion and ~1.6 GB Garlic-WC mapped. Faults are not a Deck cost; the PS4 type does not matter for them.
- **Invalidations ruled out:** ~80/s (RenderThread 1 mostly), ~0 MB, draw-pipe drain ~0 ms (max 0.06 ms).
- Submission lock: submits waited ~330 times per 5 s (~once a frame), 0.8-1.2 ms avg, max ~10 ms, 52-75 ms/s.
- **The game's critical wait:** `SubmitGPUCommandsAsyncTaskThread` waits on GraphicsCore EOP (filter -14, ident 64)
  **once a frame, 5.8-6.8 ms avg, never already signalled**, max 28 ms in the window with long frames.
  `RHIFrameOffsetThread`: VideoOut ident 6 once a frame 4.4 ms; ident 7 240/s (the vblank tick) ~4.0 ms.
- Reading: the EOP fires when stage B reaches the frame's end, so the game's submit thread is paced by our draw
  recording (stage B 93%); its long waits are stage B's slow frames. **The Deck lever is stage B's per-draw cost**,
  and U5's value there is removing copy work inside stage B (host-copy waits 4-5%, WRITE_DATA 3.7%, uploads).
- **U5 warning:** if ident 64's EOP is the newest frame's, GPU-ordered fences add ~the GPU lag (~15.6 ms) per frame
  unless Bloodborne 0.7's "first 2 submissions free" pacing applies. Which frame it belongs to is the open gate-2
  item: the fence-lag probe (study-fence-waits.md 5.1, part 2).

## Native run 9: fence lag (2026-11-03, AppImage `579c9d2`, 67%; log `data/deck-native-9.log`)

Graveyard windows 67-69 FPS (~14.6 ms frames). Lag = GPU done (the fence's timeline tick free) - our signal today,
level 1 (upper bound, command-buffer granularity), p50 / p90 / max:
- **EOP with IRQ** (wakes the game's GraphicsCore event, ident 64): **14.2-14.7 / 16.2-17.5 / ~20-25 ms**, ~343 per
  5 s (once a frame). The game's `SubmitGPUCommandsAsyncTaskThread` waits on it ~5.6 ms a frame, never already
  signalled (no slack).
- EOP label only: 16.4-17.0 / 23.4-24.6 ms, ~61k per 5 s (~180 a frame). EOS: 16.3-16.9 / 21-22 ms, ~6.9k per 5 s.
- "GPU done first": 0 of every kind (our signal always precedes GPU completion).

**Gate 2 reading:** a guest wait on the newest frame's EOP, no slack, lag ~14 ms: GPU-ordered fences would add ~14 ms
to that wait each frame; the game would stop overlapping its next frame with the GPU (estimate: ~11 ms recording +
~15 ms GPU ~ 26 ms, high 30s FPS vs 68). By the decision rule (> +4 ms, newest frame, no slack): **NO-GO for U5 as
designed**, pending a direct measurement (the rule's "measure directly" step): a switch that signals EOP / EOS only
after the GPU finished, under today's copy model (safe), one Deck run. If it collapses as estimated: U5 needs another
fence answer (early signal plus a snapshot of the per-frame ring the game rewrites), or the Deck work goes to U2 and
stage B's per-draw cost.

## Gate 2 A/B attempt (2026-11-03, `f97479f`, reverted in `ba95907`)

`GOW3_GPU_ORDERED_FENCES=1` delayed EOP / EOS label writes and interrupts until the GPU finished the fence's command
buffer. **Black screen at boot** (log stops after RHIThread creation, no GPU error): it waited for a command buffer
only submitted at the end of the guest's submission, while the guest waited on the fence first. Also **unsafe**: GPU
idle still fired at the old time, and the guest frees the objects holding its fence labels at GPU idle
(`liverpool.cpp` submission-end comment), so labels would have landed in freed memory (Bloodborne's heap-corruption
class). A correct A/B is design-u5-deck-memory.md's step U5-2 / Bloodborne 0.7's model together: submit at the
fence, GPU idle after the labels, and 0.7's pacing (the first submissions of a frame go ahead). The deployed
AppImage still contains the inert switch (no effect without the variable).

## U5-2 implemented (2026-11-03, `0771a23`; not run yet)

`GOW3_GPU_FENCES=1` (`GOW3_SUBMIT_AHEAD`, default 2): Bloodborne 0.7's model under the copy model, with the pieces the
attempt above lacked (bounded signal-thread waits with flush requests, decoder idle flush, submission batching, GPU
idle and frames retired after the labels, 0.7's `decode` pacing, WAIT_REG_MEM flush + yield, watchdogs). Design,
differences from 0.7, risks and the A / B0 / B2 / soak test plan: [impl-gpu-ordered-fences.md](impl-gpu-ordered-fences.md).

## U5-2 A/B on the Deck (2026-11-04, AppImage `0771a23`, 67%, GPU clock auto; logs `data/deck-u5-2-{A,B0,B2}.log`)

| Graveyard (last windows) | A: fences off | B0: GPU fences, ahead 0 | B2: GPU fences, ahead 2 |
|---|---|---|---|
| FPS | **64.0** | 39.4 | **41.2** |
| frame stddev / p99 (ms) | 2.25 / 20.8 | 2.52 / 29.2 | 2.49 / 29.2 |
| game's EOP wait (ident 64) | 6.2 ms | 17.5 ms | 17.6 ms |
| WAIT_REG_MEM wait / frame | ~0 | 6.2 ms | 5.9 ms |
| stage B busy | 80% | 55% | 55% |
| submits / frame | 2.0 | 10.8 | 10.8 |

- Correct: boots, fence lag 15 ms -> 0.4 ms, no crash / watchdog, "labels to unmapped memory" 0, guest memory flat.
- **Fails the pass rule** (-2%): -36% at ahead 2. The ahead-2 overlap is on ("decode, ahead 2") but barely used
  (1-4 of ~850 submissions waited per 5 s): MediEvil's UE4 waits on the newest frame's EOP once a frame (and one
  WAIT_REG_MEM), which now ends at GPU completion (~17.5 ms), so CPU and GPU alternate. The run-9 estimate held.
- The user: frame generation steadier, CPU use about halved, VRAM ~1 GB lower with fences on (inferred: fewer frames
  in flight, fewer staging copies alive).
- **Open variable:** the GPU runs at ~800 MHz on auto (DPM levels 200 / 1100 / 1600; ~90% busy at 800 = ~50% at
  1600, the user). The 17.5 ms wait is GPU time at that clock. Next: Manual GPU Clock 1600 (QAM), B2 then A; if B2
  approaches A, U5 is viable with a pinned clock; if not, U5 closes and the Deck work goes to U2 + stage B per draw.

## Pinned GPU clock (2026-11-04, the user; log `data/deck-u5-2-B2-1600.log`, partial)

- B2 (GPU fences, ahead 2) at Manual GPU Clock 1600: ~+20 FPS over B2 at auto; the ~80 FPS windows were the main
  menu; **the heavy graveyard view is still behind A** (the user). GPU utilisation ~40% at 1600 MHz: with GPU-ordered
  fences the frame is a relay (CPU records, GPU runs, the game waits for that frame's EOP, then the next), so frame time
  is CPU + GPU, not max(CPU, GPU); the clock only shortens the GPU leg.
- A (copy model) with a pinned GPU clock (the user): the shared power budget caps the CPU at ~2 GHz instead of
  boosting: worse for the CPU-bound copy model. **No pinned GPU clock in a Deck preset.**
- **Direction (the user):** remove the wait and restore the asynchronous CPU / GPU overlap; then the fence model can
  come out ahead (it also cut CPU use ~half and VRAM ~1 GB). Next: find what the per-frame EOP (GraphicsCore ident 64,
  `SubmitGPUCommandsAsyncTaskThread`) protects: a `GOW3_WAIT_SITES=1` run for the guest call site, then static
  analysis of what UE4's PS4 RHI recycles after it (command buffers only: early signal is safe as is, since stage A
  decodes them on the CPU; dynamic buffer rings: keep those copied, everything else in place: a hybrid U5).

## Wait sites and per-thread CPU (2026-11-04, AppImage `0771a23`, fences off, auto clocks except the last ~30 s; log `data/deck-waitsites-1.log`)

The user, heavy graveyard view: GPU 85-95% at ~900 MHz, CPU 70-80% on all threads, ~65 FPS: "the GPU is starved, the
CPU usage is high, and that CPU work is likely fake". Per-thread CPU over 3 s (`/proc/<pid>/task`, sampled while the
clock was already pinned): `gow3:DrawRec` (stage B) ~1 core, **`shadPS4:GpuComm` (stage A) ~1 core with 8.4k
voluntary switches/s while it waits for stage B 53% of the time (yield-spin)**, the game's main thread ~1 core,
`gow3:VkRecorder` 63% (7.3k switches/s), `AK::EventManager` (Wwise) 58%, `RHIThread` 42% (6.6k switches/s), four
`gow3:DrawPrep` at ~19% each with ~1.7k involuntary switches/s each.

Guest wait sites (20 windows at 63-67 FPS, auto clocks): the game's waits are blocking, not spinning (task workers'
`cond_timedwait` guest+0x70df3a 5.2k/s and `cond_wait` guest+0x70e02c 6.5k/s; `usleep` 8 ms guest+0x8da727 356/s; three
33 ms timers; mutexes ~70k/s uncontended at 1-3 us). **The per-frame EOP wait is guest+0x68af47** (`equeue_wait`,
~68/s = the frame rate, 6.4 ms avg, 60 ms timeout: GraphicsCore ident 64 on SubmitGPUCommandsAsyncTaskThread);
guest+0x69eb0c (~307/s, 3.2 ms) is the VideoOut flip + 240 Hz vblank waits. Wwise's wait is an event flag
(guest+0x3517baf, 155/s, 4.8 ms): its CPU is likely real work.

Reading: the "fake" CPU is mostly the port's (stage A's spin, recorder churn, four preempted prep workers). Next:
port the stage A Drain sleep (spacepepperoni `6ef7566`, from the Uncharted Linux fork); prep workers 2; static analysis
of what UE4 recycles after the EOP wait at guest+0x68af47 (hybrid U5).

## Drain sleep (2026-11-04, AppImage `ea8f4c1`, fences off, auto clocks; log `data/deck-drain-sleep-1.log`)

`GOW3_DRAIN_SLEEP` (stage A sleeps on stage B's progress after a short spin; spacepepperoni `6ef7566`). Graveyard:
**60.7 FPS** vs run A 64.0, frame stddev 2.19 / p99 21.0 ms (A 2.25 / 20.8), stage A waited 22.7% (A 22.6%),
stage B 78% (A 80%). The user: CPU clocks boost higher (less spin power), FPS the same or lower. **No gain**: stage A
sleeps instead of spinning but waits as much, and the freed core does not become frames (the chain is stage B and the
game's per-frame EOP wait). Made opt-in (`GOW3_DRAIN_SLEEP=1`) for later A/Bs.

**The user's overlay: the ~62 FPS is 10 ms and 20 ms frames ping-ponging.** Our frame-generation stats are even
(median 16.7 ms, stddev ~2.2 ms), so the alternation is in presentation (present thread, the 240 Hz vblank tick,
gamescope). Next: a flip-trace run (`GOW3_FLIP_TRACE`) to find where it enters.

## What the per-frame EOP wait is (2026-11-04, [study-eop-wait-site.md](study-eop-wait-site.md), static, 01.02)

guest+0x68af47 is in the work function of `SubmitGPUCommandsAsyncTaskThreadPS4` (0x68acc0), equeue "SubmitEndOfFrameQueue"
(`sceGnmAddEqEvent(eq, 64, 0)`, string verified in `out/eboot.elf`). It holds the RHI manager mutex while it waits
(EndFrame blocks on it: wait site guest+0x6795a1) until frame **N-1's** EOP label (64-bit frame number, two labels by
parity) arrives, then calls `sceGnmSubmitDone` and submits frame N's command buffers: **it gates submission, it does
not recycle memory.** Recycling is frame-counted: UE4's transient pool (guest 0x5805e70; per-draw constants, LCUE
descriptor tables, texture staging; read by the GPU) has 4 frame slots, reused 4 frames later; labels reused 3 frames
after release; two rare flush-and-rewind paths spin on an in-buffer label (0x679190, 0x68baf0).
- Hybrid U5: early signalling is safe for most memory, **not for the transient pool** (and the rewind paths): keep the
  pool copied (hook the allocator return at guest+0x697e54) or hold the label until stage B passes the next flip.
- **New idea for the copy model (untested):** the game's ~6 ms a frame waits for our stage B to finish frame N-1. If
  the label were signalled earlier, the game would submit sooner and overlap more with stage B, bounded by the 4-slot
  pool (stage B must have copied a slot's data before the game reuses it). After U2; same safety checks.
- Runtime checks (one stats run): pool chunks and memory types, GPU / stage B lag at each slot reuse, transient
  allocations by thread, rewind-path calls, only the two parity labels on IRQ EOPs.

## U2 implemented (2026-11-04; not run yet)

Direct arena writes (section 5.3 U2): with `GOW3_ARENA_HOST_VISIBLE` (start; default 1 on Linux integrated GPUs,
0 elsewhere) the arena's residency chunks come from a `DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT` type and are mapped;
C1 uploads and C2 precise CPU writes into pages the GPU is done with (per-page use ticks noted at recording, so
recorded-but-unsubmitted draws count; DMA blocks all) are copied straight into the mapping, else staged as before.
Live A/B: toggle file second word bit 8 (`0 8` off, `0 0` on). Stats: the `Arena writes per frame` line. Design,
guard, memory types, risks and the one-launch Deck test: [impl-u2-direct-arena-writes.md](impl-u2-direct-arena-writes.md).

## U2 A/B, early frame EOP, and the submission gates (2026-11-04)

- **U2 A/B** (`ab4861a`, one launch, toggle every 30 s; log `data/deck-u2-ab-1.log`): memory type 3
  (DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT, heap 1, 6144 MiB); 4.9 of 5 uploads a frame direct (~3.15 MB), 2 of ~92
  pass ends saved; **FPS 62-63 both ways**, stage B ~80.7% both, host-copy waits unchanged: correct, no gain (uploads
  are not where stage B's time goes). The user's culling report from the next run (objects missing at distance, FPS
  unchanged) came with HZB "hidden" ~55-61k per 5 s vs ~20k in earlier runs, with no experimental switch on and the
  view distance changed live to Console without effect; gone in the run after (14k hidden). Watch the HZB hidden count.
- **Early frame EOP** (`8600b80`, `GOW3_EARLY_FRAME_EOP=1`; log `data/deck-early-eop-1.log`): every frame EOP
  signalled at decode, 0 fallbacks, **max lead 1 frame**; FPS 63.7-64.4, flip-release pacing stddev 1.3-1.5 ms (was
  ~2.2), p99 ~19 ms (was ~21); **the game's ident-64 wait still ~5.6 ms a frame**. The user: still unstable, a small
  gain. The user's overlay (tearing allowed) shows uneven frame starts and presents, unlike our flip-release stat:
  frame-time consistency is a separate, unexplained problem (not display cadence).
- **Why the wait stays:** two gates serialise the game with stage B. (1) At the end of queued work stage A drains the
  draw pipe before raising GPU idle, so it cannot decode the game's next submission (holding the next frame label)
  until stage B catches up. (2) sceGnmSubmitDone arms the submission lock every frame, so the next submit waits for GPU
  idle, which follows the drain. Next: `GOW3_DECODE_PACING` (impl-decode-pacing.md): idle deferred to stage B in
  stream order, Bloodborne 0.7's `decode` pacing adapted to the copy model (a frame retires when stage B and its label
  writes pass its end), frame lead cap 1-2 (label free list 3 frames, transient pool 4).

## Decode pacing D0-D2 (2026-11-04, AppImage `fb62432`, 67%, auto clocks, helpers Auto, no dedicated core; logs `data/deck-decode-D{0,1,2}.log`)

| Heavy view | D0: early frame EOP | D1: + decode pacing, lead 1 | D2: lead 2 |
|---|---|---|---|
| FPS | 64-65.5 | 63.5-64.8 | 64-65 |
| frame stddev / p99 (ms) | 1.28 / 18.9 | 1.48 / 20.0 | 1.48 / 19.2 |
| game's ident-64 wait | 5.9 ms | 6.1 ms | 6.6 ms |
| submission lock / pacing waits | 83 ms/s / - | 0 / 78 ms/s | 0 / 0 |
| stage A waits: submission end / OnSubmit / WAIT_REG_MEM (VO label) / compute | 8.4 / 1.5 / 10.1 / 4.6% | (idle drains avoided 571 / 5 s) | 0 / **12.2** / 8.2 / 3.9% |
| max frame lead | - | 1 | 1 |

No visual issues in D1 or D2 (the user). Decode pacing works (idle drains avoided, lock gone, no stalls), but the drain
moved to `Rasterizer::OnSubmit` (once a frame at sceGnmSubmitDone), so stage A still reaches the frame label late and
the game never gets 2 frames ahead. Next (agent): the VO-label-met fix (spacepepperoni `6ef7566`, second half) and
OnSubmit's work in stream order on stage B (texture-cache GC vs prepared draws is the safety question).

Earlier the same session: 2 helpers + dedicated stage B core in one run (`data/deck-helpers-run1.log`, by accident
both at once): 63.2 FPS vs 64.4, stage B busy 79 -> 70%, game's wait 5.5 -> 4.6 ms: stage B did more per frame but
the frame rate did not follow (the game's threads on 6 of 8 CPUs; and the OnSubmit / VO drains above). Linux pinning
works ("Stage B: dedicated core 3 (CPU 6; sibling CPU 7 kept free), 23 other threads moved").

**Occlusion on the Deck vs the PC** (the user: Deck draw counts look high): heavy view, Deck ~1,430-1,460 draws a
frame vs PC `t-sampler-1` (4K, high settings) ~1,480; occlusion queries ~490 vs ~655 a frame; **objects hidden by
the HZB ~56 a frame on the Deck vs ~171 on the PC.** Either expected (the HZB is built from a 858x482 depth at 67% of
720p, much coarser than 4K; Low settings change the object set) or a Linux / Deck problem (cf. the one over-culling
run). Decisive check: the PC with the Deck's exact settings in the same spot (back up medievil.ini first; a PC launch
needs the user's go).

## HZB resolution and culling (2026-11-04; runs 4 and 5, same spot and settings but render scale)

| Graveyard heavy view | 100% (run 4) | 67% (run 5) |
|---|---|---|
| HZB hidden / frame | ~122 | ~63 |
| draws / frame | ~1,290 | ~1,430 |
| FPS (auto clocks) | 64.3 | 69.0 |

Resolution doubles culling (~145 fewer draws, ~10%), but at auto clocks the GPU cost outweighed stage B's saving. The
user's hunch confirmed: the Deck culls less because its HZB is coarse. UE4 builds the HZB at half the depth buffer's
resolution (power of two): a pyramid at the depth's full resolution would be 2x finer per axis with no extra scene
rendering, by intercepting the game's HZB build / test passes (where the HZB remap and the small-mip fix already work).
**Order (the user):** (1) finish the OnSubmit and VO-label drains; (2) re-test render scale 67 / 80 / 100% on top;
(3) only if the curve shows culling pays: research the full-resolution HZB build.

## E1 and the fix hit rates (2026-11-04)

**E1** (`c4a0147`: D2 + OnSubmit on stage B + met display-label waits without a drain; log on the Deck): FPS
64.5-64.9, stddev 1.42 / p99 18.9 ms, **stage B 91% busy** (D2 83%), stage A waits 30% all on the compute drain,
OnSubmit drains avoided 320 per 5 s (294 ms/s of stage A waiting saved), 975 display-label waits met without a
drain, game's ident-64 wait 6.5 ms, lead 1; no visual issues (the user). Stage B is now saturated: the stage A gates
were real and are gone; the wall is stage B's per-draw cost. The user's list: cut stage B cost first; improve culling
(native resolution: -3-4 FPS with the GPU at 1200 MHz, 90%, but more culled); fewer draws / heavy actions; CPU of other
threads; what stage B is busy doing; idle elsewhere; whether earlier fixes work as intended (HZB occlusion exonerated:
worse without it).

**Earlier fixes on the Deck vs the PC** (heavy view, medians per 5 s; Deck D2 vs PC `t-sampler-1` 4K):
texture-set memo hit 99.3% vs 92.1%; render-state memo 93.7% vs 81.5%; prepared draws used 97.8% vs 99.6%; small buffers
from the arena 59% vs 51%; pipeline binds 1,399 vs 1,462 a frame (77% vs 71% the same pipeline as the previous draw);
descriptor pushes 1,277 vs 1,362 a frame at 17.7 vs 15.1 writes (~22.6k writes a frame). **Every measured fix fires
on the Deck as well as or better than on the PC**; the Deck's stage B does the same work, each piece 4-5x slower.
Research running: `study-stage-b-per-draw.md` (ranked per-draw cuts, top one behind a toggle),
`study-deck-other-threads.md` (Wwise 58%, the main thread, recorder churn).

## Stage B per draw (2026-11-04, [study-stage-b-per-draw.md](study-stage-b-per-draw.md); C1 built in `f03ee52`, not run)

Draw parts cost 4-5x the PC's, but each end-of-pipe / end-of-shader event (~200 a heavy frame) costs 17-26x:
`ProcessDownloadImages` is 3.3 us there vs 0.13 us on the PC. Its readback retirement queries the timeline semaphore
at every event, and on RADV that query is an ioctl. C1 polls at most every 100 us (second toggle word 16, default
on). The study ranks the other cuts: C3 slow texture binds, C4 one recorded command per draw, C5 a lock-free
readback pre-check, C6 descriptor set updates off stage B. New `GOW3_FRAME_STATS` lines: "Stage B syscalls per
frame" (semaphore queries, recorder wakes) and "slow texture binds per 1000 draws". Deck test: study section 6 (one
launch, `0 0` / `0 16` every ~30 s).

## Deck test tooling (2026-11-04): `research/tools/deck.sh` + `deck_summary.py`

From Git Bash on the PC (host `deck` in `~/.ssh/config`; never launches the game):
`deck.sh status` (running?, deployed build from `~/MediEvil/DEPLOYED`, test settings, toggles, launcher and ini
settings, GPU clock mode); `deck.sh deploy [wsl_tree]` (refuses while the game runs, keeps the previous AppImage as
`.<commit>`, verifies the checksum); `deck.sh env "comment" K=V ...`; `deck.sh toggle "W1 W2"`;
`deck.sh ab "A" "B" [period] [cycles]` (alternates the toggle file during a run); `deck.sh pull NAME`;
`deck.sh summary NAME [--min-draws N] [--last N] [--windows]` (labels each 5 s window with the toggle state from the
log's "disabled optimizations" lines, drops windows that straddle a switch, medians per state: FPS, worst, draws,
pacing stddev / p99, stage B busy, stage A waited, the game's ident-64 wait, HZB hidden, plus the last
Recorder / Stage B syscalls / Decode pacing / OnSubmit / Arena / Early EOP / Thread ids lines);
`deck.sh threads [s]` (per-thread CPU and context switches from /proc during a run); `deck.sh checks` (the
read-only platform facts of study-deck-platform-optimization.md section 0). Checked on `deck-u2-ab-1.log`: 62.5 vs
62.7 FPS by state, 4 mixed windows dropped.

## Built and researched while the Deck was away (2026-11-04; nothing run yet)

**Built (deck-lab, not pushed):**
- Static GPU library on Linux, `98dca62` ([impl-deck-cpu-bundle.md](impl-deck-cpu-bundle.md) G): default ON,
  `GOW3_STATIC_GPU=OFF bash build.sh` for the old shared build. PLT calls 49,039 -> 4,055, `__tls_get_addr` 5 -> 0.
  WSL `dist/` holds `MediEvil-x86_64-static.AppImage` and `-shared.AppImage` from the same commit for the A/B.
- Stage B bundle 2 ([impl-stage-b-bundle-2.md](impl-stage-b-bundle-2.md), `50b965d`..`8dd5881`): C5 readback
  pre-check (2nd word 128), D4 Linux pool maps outside the table lock (256), D5 table lock spin + empty hook flush
  (512), #4 stage B spin before blocking (opt-in `GOW3_STAGE_B_SPIN=1`, 1024 off) with a "Stage B sleeps per frame"
  counter, #6 cache-line groups (build: `GOW3_CACHELINE_PADDING=OFF`; WSL `dist/MediEvil-x86_64-nopad.AppImage`).
- `deck.sh` gained `clocks [s] [names]` (the clock of the CPU stage B and other named threads run on, every 200 ms),
  `mem [s]` (meminfo, TTM limit, VRAM / GTT used, RSS, faults, I/O, pressure every 5 s) and
  `deploy [tree] [file]` (deploys a named `dist/` AppImage; DEPLOYED records `<commit>-<name>`).

**Studies:** [study-deck-apu-deep-dive.md](study-deck-apu-deep-dive.md) (stage B likely at ~2.4 GHz: EPP's sticky
plateau after sleeps over ~0.2 ms; the `performance` governor via `steamosctl` may give +25-30%),
[study-deck-build-optimization.md](study-deck-build-optimization.md) (static link, x86-64-v3 + lld, IR PGO, BOLT),
[study-native-render-paths.md](study-native-render-paths.md) (no separate renderer; particle-sim run replay after a
per-class draw counter), [study-deck-memory-budget.md](study-deck-memory-budget.md) (4.5-7.5 GiB free; bigger caches
gain nothing; pre-fill the guest pool for hitches; denser texture-set memo),
[study-ue4-draw-reduction.md](study-ue4-draw-reduction.md) (UE 4.21's HZB is `2^(ceil(log2 W) - 1)`: a
two-instruction patch gives 67% the 100% pyramid, ~120-145 fewer draws; no sun cascades in the heavy view; cvars
ranked).

**Deck test order when it is back** (one change per launch; each launch needs the user's go; updated after every
build of the day, deck-lab `671a81a`):
0. Baseline trap ([study-deck-patch-audit.md](study-deck-patch-audit.md)): every Deck run since U2 had
   `GOW3_ARENA_HOST_VISIBLE=0` in extra-env.txt; keep it in every `deck.sh env` until U2 itself is tested, or the
   baseline changes silently. Steps 1-7 below are followed by the audit's own order (its section 3): word-1 screening
   pairs, then `GOW3_PREP_WORKERS=3`, U2 at its default, `GOW3_FORCE_ANISO=0`, `GOW3_ONSUBMIT_STAGE_B=0`.
1. `deck.sh deploy` (default AppImage, `a863482`); `deck.sh checks`. Baseline in the heavy view, 67%,
   `GOW3_FRAME_STATS=1`, with `deck.sh clocks 20` (is stage B at ~2.4 GHz?) and `deck.sh mem 30`. Same launch, live:
   `deck.sh ab "0 0" "0 32"` (lean recorder), `"0 0" "0 16"` (C1), then the bundle 2 pairs C5 / D4 / D5
   ([impl-stage-b-bundle-2.md](impl-stage-b-bundle-2.md); bits 128 / 256 / 512 off-switches).
2. Full-size HZB patch (`93d1a58`, [impl-draw-reduction.md](impl-draw-reduction.md)): `GOW3_HZB_FULL_SIZE=1`,
   restart-only. Hidden ~63 -> ~120?, draws ~1,430 -> ~1,300? Check for pop-in. Cross-check without the patch: 81%.
3. Stage B spin #4: `GOW3_STAGE_B_SPIN=1`, live `deck.sh ab "0 1024" "0 0"`, with `deck.sh clocks`.
4. Governor `performance` via `steamosctl` (system change: the user's OK first), with `deck.sh clocks`.
5. Builds, one launch each against step 1: the v3 AppImage (`deck.sh deploy /root/src/medievil
   MediEvil-x86_64-v3.AppImage`); static vs shared and padding vs none (the `-shared` / `-nopad` images in WSL `dist/`
   are from older commits `98dca62` / `0c57f23`: rebuild them from the current commit first for a clean A/B).
6. Restart-only switches ([impl-deck-build-sched-pool.md](impl-deck-build-sched-pool.md)), one per launch:
   `GOW3_STAGE_B_CORE_SCHED=1`, `GOW3_THREAD_NICE=1`, `GOW3_POOL_PREFILL_MB=2560`, then `GOW3_POOL_POPULATE=1` on top
   (judge the pool by hitch frames and the "Guest pool faults" line).
7. Then: `GOW3_UE4_CVARS` items 4-5 of the UE4 study one per launch, `RADV_FORCE_VRS=2x2` at 100%, the render-scale
   curve 67 / 80 / 100; the particle-sim skip after the N0 line gives its shader hashes (impl-draw-reduction.md).

## Baseline 1 and live A/Bs (2026-11-04, AppImage `a863482`, 67%, helpers Auto, no dedicated core, auto clocks, Mailbox; log `data/deck-baseline-1-ab.log`)

Heavy view: **74.2 FPS at ~1,241 draws** (D2: 65.3 at 1,415). Draws x FPS is unchanged (~92k draws/s), and stage B is
busier (94% vs 83%) at ~10.2 vs 9.0 us per draw: the FPS gain lines up with ~170 fewer draws, not with a faster stage B.
The user could not find an angle with more draws; the old build in the same spot would tell view from build (open).
`deck.sh clocks`: stage B at **~2.98 GHz** mean (2.88-2.97), not ~2.4 (the APU study's guess); it migrates between CPUs
every sample. The governor's headroom is therefore ~15% at most.

Live pairs, 20 s x 3 cycles each, medians of 9 clean windows vs 284 baseline windows (each B switches a fix **off**):

| B | Fix off | FPS (base 74.2) | Stage B busy | Read |
|---|---|---|---|---|
| `0 32` | recorder lean mode | 74.3 | 93.4% | neutral in the heavy view |
| `0 16` | C1 semaphore poll interval | 73.7 | 94.1% | +0.5 for C1 (noise level) |
| `0 128` | C5 readback pre-check | 73.7 | 94.1% | +0.5 for C5 (noise level) |
| `0 256` | D4 pool maps outside the lock | 73.5 | 92.8% | +0.7 for D4 (noise level) |
| `0 512` | D5 table lock spin / empty hook queue | 72.9 | 92.0% | **+1.3 for D5** |
| `2147485696 0` | narrow image stages + coarse read tracking | 74.4 | 91.8% | neutral in the heavy view |
| `4295098368 0` | record prefetch + parallel copies | 74.0 | 94.4% | neutral in the heavy view (may help loads / transitions) |
| `5764607523034234880 0` | both targeted waits (W2 / B1) | 70.2 | 96.5% (stage A waited 42%) | **+4.0 for the targeted waits** |

The patch audit's suspects among these are cleared: the targeted waits are the largest single gain measured live on
the Deck, and the barrier / prefetch pairs cost nothing. No visual issues reported. Present mode: the game used Mailbox
(VSync off); the frame rate is stage-B-bound and not vblank-quantized, so the A/B holds; Immediate (the mode gamescope's
"allow tearing" acts on) is a separate launch (`GOW3_PRESENT_MODE=immediate` or the launcher's Present mode).

**Immediate present mode (2026-11-04, one launch):** `GOW3_PRESENT_MODE=immediate` logged "Swapchain: present mode
Fifo (GOW3_PRESENT_MODE=immediate)": the gamescope surface did not offer Immediate (with "allow tearing" on), so the
port fell back to Fifo and the frame rate was capped (the user saw it capped). Mailbox stays the uncapped mode under
gamescope; true tearing would need gamescope to expose Immediate. Removed from extra-env.txt.

## Finer occlusion culling on the Deck (2026-11-04, AppImage `a4250a2`, baseline settings + `GOW3_HZB_FULL_SIZE=1`, Mailbox; log `data/deck-hzb-1.log`)

Patch applied (17 writes). Same heavy view, medians of the steady windows vs baseline 1:

| | Baseline 1 | Full-size HZB |
|---|---|---|
| HZB hidden / frame | ~60 | **~119** (the 100% run's rate) |
| draws / frame | 1,241 | **~1,151** (base pass 523 -> 460, velocity 74 -> 57, depth prepass 232 -> 222) |
| FPS | 74.2 | **~77.6** (+4.6%) |
| pacing stddev / p99 / worst | 1.54 / 17.6 / 18.9 ms | **1.42 / 16.4 / 17.3 ms** |

Fewer draws than the study's 120-145 estimate (two hidden objects save about one and a half draws), but the gain is clean:
throughput unchanged (~89k draws/s), the heaviest frames shrink most (the user: "the spikes are lower amplitude on
the graph"), no glitches seen in the heavy view. Pending: walking and fast turns for late pop-in; then a Deck default.

## Decisions after the culling run (the user, 2026-11-04)

- Finer occlusion culling: no pop-in over most of the graveyard level, GPU use visibly higher; **default on for scenes up
  to 1280x800** (`8131ecb`). A finer-still HZB may be tried much later.
- "A locked 60 is on the table now" (the other Deck fork shows a locked 30).
- Test **every built-but-untested item** (menu redraw fix, stage B spin, static vs shared, v3 build, padding, core
  scheduling, nice, pool prefill / populate). The **performance governor stays on the list** despite stage B's ~2.98
  GHz; **present work comes back once the frame rate is good enough for a lock**.
- Logging runs use a fixed ini: `deck.sh ini logging` (fog off, sharpening off, frames queued 1); the user's own is
  `medievil.ini.play` (`deck.sh ini play`).
- Running in parallel: live render scale with 1% steps (impl agent, `impl-live-render-scale.md`); which restart-only
  settings could go live by a renderer restart that keeps the game state (`study-live-settings.md`).
- Next run: sections (`GOW3_SECTIONS=1 GOW3_SLOW_FRAMES=1.4`) to break down stage B's ~4-6 ms outside draws.

## Spin, compact texture memo and the task split (2026-11-04)

- **Stage B spin #4** (`cd1b1a8`, log `data/deck-spin-1.log`, `deck.sh ab "0 1024" "0 0" 20 4`): 77.1 vs 77.1 FPS.
  Sleeps 4.2 -> 1.7 a frame, 4.8 waits a frame ended by the spin, GPU waits (0.7 a frame) unchanged: neutral. The user
  keeps it on (`GOW3_STAGE_B_SPIN=1` stays in extra-env.txt). Live render scale and the menu redraw fix: verified by
  the user (works; pacing with the menu up normal).
- **Compact texture-set memo** (`1e4ca3d`, log `data/deck-texset-1.log`, `ab "0 0" "0 2048" 30 5`): 76.6 vs 76.5 FPS,
  texset 1.051 vs 1.046 ms, hit rates identical (98.25 / 98.26%), 44 MB in 21 huge pages. Neutral: the texset cost
  is not memory-bound. Kept on (8 MB less).
- **Task split** (heavy frame samples, 13.6 ms, 1,213 draws): tasks 3.09 ms = flip 1.30 (GPU wait 0.69, the rest the
  present work), **WRITE_DATA 0.85, all waiting** (`wdata_wait`: the two "done" flags a frame wait for the fences
  deferred to the Vulkan recorder, T-W3), EOP 0.50, submit 0.23 (flush 0.13), EOS 0.09. Stage B sleeps 0.87 ms a frame
  in all with the spin on (GPU waits 0.69). Next design item: WRITE_DATA without stage B waiting (W3 alone moved the
  wait to the flip; W3b deadlocked at boot).
- CPU clocks next (the user: boosting stage B's core and capping the others leaves power to the GPU; higher clocks
  move the bottleneck to the GPU): `research/tools/deck-cpu-clocks.sh` (`deck.sh cpu-script`), the user runs it with sudo.

## CPU clocks and the power cap (2026-11-04/05; log `data/deck-epp-perf-1.log`, `data/deck-dedicated-1.log`)

- **EPP performance on every CPU:** stage B 2,963 MHz (2,976 before), FPS 74-76 vs 76.5: no gain. Spin off/on under
  it: 75.6 / 75.9 (neutral; spin left off).
- **The APU is at its power cap:** `power1_cap` 15 W (default; max 29 W), 14-15 W used, GPU 860-1,090 MHz at ~100% busy.
  `gpu_metrics` (v2.4, offset 40: socket / cpu / soc / gfx mW, then core[4]; `~/MediEvil/apu_power.py`): **CPU 7.9 W,
  GPU 3.2 W** of 14.9 W.
- **Dedicated core alone:** 74.1 FPS at 1,155 draws (same draws/s), stage B busy 96 -> 89%, GPU 98% busy at 1,024 MHz.
- **Split** (stage B's CPU 6 at EPP performance / 3.5 GHz, the rest capped at 2.0 GHz): **~66 FPS**. Stage B 3,344 MHz,
  but stage A at ~1.9 GHz starves it (packet-ring waits 0.2 -> 1.8 ms a frame), and the GPU fell to 863 MHz (GPU waits
  0.7 -> 1.7 ms a frame): capped cores still drew ~1.7 W each, like stage B's core (inferred: one shared CPU voltage
  set by the fastest core, so capping frequency saves little). Boost-one / slow-the-rest does not work on this APU.
- **The user:** the GPU can already get more power with a fixed GPU clock (+1-2 FPS at a cost in frame stability); not
  worth chasing clocks further unless CPU use drops seriously. **Direction: cut total CPU work on every thread** (less
  CPU power = GPU headroom), measured per thread first (`deck.sh threads`).

## Cutting CPU work: stage A's wait and the game thread's top samples (2026-11-05, built, not run; [impl-cpu-waste.md](impl-cpu-waste.md))

- **Stage A's drain wait** (`6f4be46`): `GOW3_DRAIN_HYBRID=1` pauses up to 30 us, then sleeps on a futex that stage B
  ends two packets before the last one (one wake per sleep; `GOW3_DRAIN_SLEEP` woke stage A at every packet, likely
  why it lost 60.7 vs 64.0). Live A/B: `deck.sh ab "0 0" "0 4096" 20 4` with `deck.sh threads 5` and the APU power read;
  "Stage A drain waits per frame" line.
- **The game thread's "tight loop" at guest+0x2c94ba2-ba8 is not a wait:** UE 4.21's `ANavigationData::TickActor`
  purging `ActivePaths` every frame, with an `mfence` before and after each refcount read. Patch (`b98df73`, opt-in,
  restart-only): `GOW3_NAV_PURGE_NOFENCE=1` drops the two fences (the read stays).
- Recommendations without code: recorder wake batching, a lock-free label write path, `GOW3_PREP_WORKERS=3`, a lower
  vblank tick than 900 Hz on the Deck.

## Stage A hybrid drain wait (2026-11-05, AppImage `d6029de`, `GOW3_DRAIN_HYBRID=1`, frame stats without sections; log `data/deck-drain-hybrid-1.log`)

`deck.sh ab "0 2048" "0 6144" 20 4` (bit 4096 = today's wait): **78.7 vs 77.9 FPS**, stddev 1.32 vs 1.44 ms, p99 16.4 vs
16.6 ms, stage B busy 91.8 vs 93.7%. Stage A CPU **76% vs 112%** (`deck.sh threads`), sched_yield calls **0 vs 4,540 a
frame**, drain wait 3.26 vs 4.16 ms a frame; 1.04 of 2.89 drains a frame ended in the 30 us spin. Power samples too
noisy to separate (CPU 6.6 vs 6.2 W, socket 14.9 vs 13.3 W, GPU 4.5 vs 3.7 W). Verdict: keep; candidate default.
Next: the nav purge patch (`GOW3_NAV_PURGE_NOFENCE=1`, restart-only) on top.

## Stage B per draw, round 2 (2026-11-05, branch `stage-b-per-draw-2`; [impl-stage-b-per-draw-2.md](impl-stage-b-per-draw-2.md); not run)

Read from the code against `data/deck-texset-1.log`: `obt_resident` (0.32 ms) is a sorted-vector search for residency
that only ever grows; the clamp's region cache is filled only on the locked path, so since the lock-free search it is
nearly empty and every vertex range / buffer binary-searches ~13,000 regions; `obt_sync`'s 0.67 ms is mostly ~4
uploads a frame (~3 MB, the pages of ~750 write faults, re-protected each time); texslow is over-16-image sets (0.38
ms), unprepared draws (0.28) and stages without images (0.17); the ~16 texture-set misses a frame are all own-target
sets (C10). Built, each its own commit and live switch (second word): **R2** residency bitmap 65536 (on), **T1**
no-image texture binds 131072 (on), **T2** wide texture-set memo 262144 (`GOW3_TEXSET_WIDE=1`), **R1** region cache
index 524288 (`GOW3_REGION_INDEX=1`); new sections `obt_upload`, `clamp`; new lines "Stage B cuts per frame",
"Region cache per frame". All removed work, none moved to another thread. Expected -0.6 to -1.3 ms of stage B a heavy
frame. Deck test: one launch, `deck.sh ab "0 0" "0 983040"`, then one pair per bit (impl doc section 3).

## Per-draw cuts R1 / R2 / T1 / T2 on the Deck (2026-11-05, AppImage `c71e87c`, drain hybrid on, `GOW3_TEXSET_WIDE=1 GOW3_REGION_INDEX=1`, frame stats without sections; log `data/deck-perdraw-1.log`)

All four on (`0 0`) vs off (`0 983040`), 30 s x 4: **78.8 vs 78.2 FPS**, p99 16.4 vs 16.5 ms, **stage B busy 86.7 vs
92.6%** (~0.75 ms a frame freed, inside the -0.6 to -1.3 ms estimate), but stage B now waits on stage A's packets
(1.02 vs 0.52 ms a frame) and on the GPU (1.58 vs 0.96 ms); stage A waited 17.4 vs 26.9%. Single bits off (20 s x 3):
78.7-79.0, within noise. Counters: R1 14.3k index hits a frame (vs 13.6k lock-free searches off), R2 2,150 bitmap
answers, T1 ~920 binds, T2 46 + 175 sets at 100% hits. APU: socket 13.6 W, CPU 6.3 W, GPU 3.9 W. **The limit has
moved: stage A and the power-capped GPU now gate stage B about as much as its own work.** The user (same session):
capping the GPU at 1,000 MHz flattens the frame-time "waves" (spikes to ~20 ms with the GPU uncapped; 1,200 MHz
reduced, 1,100 more, 1,000 flat +-1 ms at ~76 FPS): the power controller shifting the 15 W between GPU and CPU.
Next: stage A's per-packet cost; total CPU watts.

## A flat 60 on the Deck (the user, 2026-11-05)

Render scale **80%**, **volumetric fog on**, **60 FPS cap**, GPU clock **1,200 MHz** (Quick Access Menu): a flat 60 FPS
in the heavy graveyard view (the other Deck fork shows a locked 30). Next: a "Deck Optimized" preset (patches, the
measured performance switches, graphics settings, the GPU / FPS recommendation; being built). The user: "cutting CPU
usage is still the big win here, because there's a lot of GPU headroom, we just can't use it" (the 15 W cap).
On the Deck's next start: the active medievil.ini holds these settings (written by the in-game menu into the logging
copy): save it as medievil.ini.deck60, restore medievil.ini.logging for test runs; extra-env.txt still has test keys.

## Deck Optimized preset (2026-11-05, launcher; built, not run)

The user's result (Deck OLED, SteamOS, AppImage): **a flat 60 FPS in the heaviest graveyard view** with the logging
ini plus render scale 80%, volumetric fog on and `fps_limit=60`, VSync off (Mailbox), the GPU clock fixed at 1,200 MHz
in the Quick Access Menu. Now a launcher preset: Graphics quality > **Deck Optimized** (Home and Performance).

- **Values** (`DECK_PRESET`, `launcher/medievil_launcher.py`): the logging ini as `deck.sh ini logging` writes it,
  with `render_scale=80`, `volumetric_fog=1`, `fps_limit=60` (720p borderless, Low with effects 50%, AO 2, reflections
  1, bloom 2, shadows 256 / short / 1, view 0.7, LOD low, `preshadows=0`, every performance patch on but 4K memory,
  `perf_particle_single_pass=1` as in the user's ini, engine 240 + no built-in caps, tick 900 not precise, Standard
  limiter, inline present, `display_vrr=0`). `ue4_quality` stays `low` for the game; "deck" exists only in the
  launcher (`graphics_preset` in settings.json).
- **The 60 cap:** with `present_no_frame_caps=1` run.py gives the engine a 1000 FPS bound and `GOW3_NO_FRAME_CAP=1`;
  the tick is 900 Hz (`GOW3_VBLANK_HZ`), not 480, so the present thread has no fixed limit of its own and `fps_limit`
  is the only clock (`driver.cpp` `current_limit`: kept because 60 < the tick; not zeroed because VSync is off; no
  Reflex with the Standard limiter). One flip per 1/60 s slot; the game waits on its flips (1 frame queued). Not used:
  `engine_fps=60` (UE4's own tick bound, `SmoothedFrameRateRange` below 62, would be a second clock beside the
  present limit: two limiters at one rate fight, T-F13), a 60 Hz tick (tick = cap holds on a fast PC, but a frame
  that misses a tick waits for the next: 33 ms on a Deck whose heavy frames are near 16 ms), Reflex / Precise (not
  what was measured; Reflex falls back to Precise on AMD).
- **Switches** (`DECK_SWITCHES`, launcher setting `deck_switches`, the checkbox "Steam Deck performance switches"
  under Graphics quality): `GOW3_EARLY_FRAME_EOP=1`, `GOW3_DECODE_PACING=1` + `GOW3_DECODE_PACING_LEAD=2`,
  `GOW3_ARENA_HOST_VISIBLE=0`, `GOW3_DRAIN_HYBRID=1`, `GOW3_TEXSET_WIDE=1`, `GOW3_REGION_INDEX=1`, and since
  2026-11-06 stage A's cuts `GOW3_BACKING_COPY=1` / `GOW3_PENDING_FILTER=1` / `GOW3_REG_HASH_PAIRED=1` (round 1
  and 2), `GOW3_PARTICLE_FLIP_GUARD=1` (particle fix 1 and 2: the single pass without its glitches; not the
  fixed-step patch) and `GOW3_THREAD_NICE=1` (thread nice, live: better lows at a fixed GPU clock), set by
  `game_environment` with `setdefault` (an environment variable already set wins; extra-env.txt still overrides).
  Preset-driven, not Linux defaults: measured on the Deck only; the PC / Windows defaults are unchanged. Left out:
  `GOW3_NAV_PURGE_NOFENCE` (neutral), `GOW3_STAGE_B_SPIN`, the v3 build, core scheduling (rejected), pool prefill.
  Picking another Graphics quality ends the preset and turns the switches off; the other values stay.
- **Recommendation**, shown under Graphics quality (Home and Performance) while Deck Optimized is chosen and in the
  first-run message: set a manual GPU clock of about 1,200 MHz in the Quick Access Menu (Performance) and keep the
  frame limit at 60; an uncapped GPU takes power from the CPU and frame times swing.
- **Detection** (`steam_deck`): `SteamDeck=1` (Steam), or DMI `board_vendor` (or `sys_vendor`) `Valve` with
  `product_name` (or `board_name`) `Jupiter` / `Galileo`. On a Deck, once (`deck_offered`): the first run (no
  settings.json, no medievil.ini) selects the preset and says so; an existing setup is asked yes / no. Never changed
  silently; `--play` never offers (it needs a game folder, so the window ran first).
- Tests: `tests/test_deck_preset.py` (exact values, every value valid, the single 60 clock, switches only with the
  preset, detection with mocked DMI files, the offer rule). Smoke-tested off-screen (Tk, scratch data and ini): first
  run, existing setup no / yes, leaving the preset, reopening.
- Deck check when it is back: the preset on a fresh data folder reproduces the flat 60 (`deck.sh status` shows the
  ini); the switches' log lines ("Decode pacing: on", "Early frame EOP", "Stage A drain waits") without extra-env.txt.

## Single GPU particle pass, live (2026-11-05, `d20f010`; built, not run)

The patch (`perf_particle_single_pass`, `r.GPUParticle.FixDeltaSeconds=0`; on in the user's Deck ini and in Deck
Optimized) is now switched live, so one session can A/B its cost at one camera spot and flip it while the user watches
a torch. Background: with the default fixed 1/30 s step UE 4.21 adds a second, empty GPU particle run of ~190 draws in
some frames ([study-heavy-frame-map.md](study-heavy-frame-map.md) H3); with `0` lone torches crawl and an ember /
golden stream shows in Dan's crypt, seen on the PC at 120-240 FPS ([plan-0102.md](plan-0102.md) T-0102-4), **not
known at 60**.

- **Switches.** In-game menu > Performance patches > "Single GPU particle pass" is in the live group now (saved to the
  ini as before; the launcher's checkbox still sets the start). **Toggle file second word 8192**: set = the game's
  fixed 1/30 s step whatever the menu says (the menu shows "held off by the toggle file" and keeps its own choice);
  clear = the menu's / ini's choice again. So `"0 0"` vs `"0 8192"` compares single pass vs fixed step **only if the
  ini (menu) has the patch on**; with it off both states are the fixed step. The runtime sets the cvar only when the
  effective value changes, from the main menu on, and logs `Particle single pass: off (live, toggle file second word
  8192)` / `Particle single pass: on (live)`; `deck_summary.py` labels windows by the toggle lines as usual. Off
  restores exactly the game's default (`0.033333335` = float `0x3d088889`). Both game builds (01.00, 01.02; addresses
  in [study-live-switches.md](study-live-switches.md)).
- **Build:** WSL `/root/src/medievil` at `d20f010`, `CC=clang CXX=clang++ GOW3_CPU_LEVEL= bash build.sh` (LTO, static,
  baseline CPU level, no warnings) + `packaging/linux/appimage.sh`: **`dist/MediEvil-x86_64.AppImage`** (131,357,176
  bytes, sha256 `d06c34f6465d58e192e09aebd881fc3b50bde21f2716aa673ba402f7ab3a770f`), `--help` runs. Windows build
  clean; `graphics-settings-test` passes; Python tests: the 8 known failures, 4 new pass. The poll's decision logic
  was also compiled alone with stubs (scratch harness, gcc `-Wall -Wextra -Werror`): start on / off, toggle set and
  cleared with the menu never opened, menu flips while the bit holds; one cvar write per change. Not deployed.

### Deck test (one launch, the user's go; the user presses Play)

Before (game closed):
1. `deck.sh deploy` (the AppImage above), `deck.sh status`.
2. The user's 60 FPS settings: `deck.sh ini deck60` if `medievil.ini.deck60` was saved, else Graphics quality > Deck
   Optimized in the launcher; GPU clock fixed at 1,200 MHz in the Quick Access Menu as for the flat 60. Check the patch
   is on: `ssh deck 'grep ^perf_particle_single_pass ~/.local/share/medievil-pc/medievil.ini'` gives `=1`.
3. `deck.sh env "particle pass live A/B" GOW3_FRAME_STATS=1` (plus any test keys the current runs keep), then
   `deck.sh toggle "0 0"`.
4. The user starts the game, loads the save, stands still in the heavy graveyard view. The switches apply from the
   main menu on (the log shows the first `Particle single pass` line only at a change).

**(a) Performance A/B.** No SSH sampling (`deck.sh threads`, `clocks`, `mem`, power reads) while `ab` runs: the
comparison windows must carry no extra load; power gets its own windows afterwards.
5. 60 cap: `deck.sh ab "0 0" "0 8192" 30 4` (fixed step first, ends on single pass), then at once
   `deck.sh pull deck-particle-1-cap60` and `deck.sh summary deck-particle-1-cap60 --min-draws 1000`.
6. Power at 60, in its own windows: `deck.sh toggle "0 8192"`, wait ~10 s, `ssh deck python3 ~/MediEvil/apu_power.py`
   (5 s), `deck.sh toggle "0 0"`, wait ~10 s, the same read; twice each.
7. Uncapped: in-game menu > Performance > Frame rate limit "Unlimited" (live), wait ~10 s, `deck.sh ab "0 0" "0 8192"
   30 4`, then at once `deck.sh pull deck-particle-1-uncapped` and `deck.sh summary deck-particle-1-uncapped --last
   48 --min-draws 1000` (about the 48 five-second windows of this `ab`), then the power reads of step 6 again. Set the limit back to 60.
8. Record per state (single pass `0 0` / fixed step `0 8192`), at 60 and uncapped: stage B busy, draws/frame, FPS
   (uncapped), socket / CPU / GPU W. Expected [I]: the fixed step adds the empty run's ~190 draws in some frames, so
   more draws/frame and stage B busy; at 60 the FPS stays at the cap and the gain shows as stage B busy and CPU watts
   (GPU headroom under the 15 W cap). How often the extra run happens at 60 FPS (1/30 s step, 1/60 s frames) is not
   known: the draws/frame difference answers it.

**(b) Visual check** (the user watches; flip with `deck.sh toggle` or the menu checkbox, toggle at `"0 0"`):
9. 60 cap: a lone torch (one torch in view, as in plan-0102.md shots 1-3): `deck.sh toggle "0 0"` (single pass), ~20
   s: slow or normal? `deck.sh toggle "0 8192"` (fixed step), ~20 s; once more each way. Then Dan's crypt: the ember on
   the floor / golden stream, in both states. Particles already alive may outlast a switch: turn away and back (or
   leave and re-enter the view) after each flip before judging.
10. Uncapped (Unlimited): the same torch and crypt, both states.
11. The user's words per state and cap go here with the log lines' times. Decision: no glitch at 60 and a measurable
    stage B / power gain: Deck Optimized keeps the patch; a glitch at 60: Deck Optimized turns it off
    (`DECK_PRESET['perf_particle_single_pass']='0'`); the PC default stays off either way unless the glitch is gone at
    the PC's rates too.

### Particle 1 results (2026-11-05 ~19:46, build `d20f010-test1105`, log `data/deck-particle-1.log`)

Launched by the user. ini: logging (67%, fog off, uncapped), GPU clock auto, the Deck switches as per-draw 1. Live A/B
`ab "0 0" "0 8192" 30 4` in the heavy graveyard view; all 8 switches logged (`Particle single pass: off/on (live)`).

| state | n | FPS (median) | draws | particle sim runs | stage B | p99 ms |
|---|---|---|---|---|---|---|
| `0 0` single pass | 33 | **79.4** | 1,122 | 1.00 (193 draws) | 86.3% | 16.30 |
| `0 8192` fixed 1/30 step | 20 | 77.1 | 1,316 | 1.98 (386 draws) | 89.1% | 16.74 |

- **+2.3 FPS (+3%), -193 draws a frame, stage B -2.8 points.** The second run fires on nearly every frame at ~78 FPS
  (1.98 runs/frame), so the "fires less near 60 FPS" guess (study-deck-rescore-rejected.md A6) is wrong.
- **Visual (the user, uncapped ~79 FPS): "the torch and the small glitch on the floor do return with it on."** The
  bug of T-0102-4 reproduces on the Deck. **At a 60 FPS lock: "made no difference"** (the user): the glitch is there
  at 60 too, so it does not scale with the frame rate. The preset cannot dodge it by capping.
- Next: a fix that keeps one run with correct torches (study-particle-fixed-step.md, in progress). Until then the
  Deck Optimized preset's `perf_particle_single_pass` is the user's call.

## Stage A per packet: A1 / A2 / A3 (2026-11-05, built, not run; [impl-stage-a-decode.md](impl-stage-a-decode.md))

Read from the code against the v3 profile and `data/deck-perdraw-1.log`: stage A costs ~8 us of CPU a draw (76% of a
core, 1,168 draws) against stage B's 6.45 us, so after each drain (2.75 a frame, all compute-queue drains) stage B
waits packet by packet (0.83 ms a frame on the packet ring): the wait is stage A's cost per draw, not batching
(`Commit` publishes each packet at once). The hybrid drain's wake latency (51 us average) exceeds its 2-packet early
wake: `GOW3_DRAIN_WAKE_AHEAD=6` is worth one restart-only launch. R1 already answers the clamp / region lookups;
still per constant-ring copy on Linux: the region table's read lock and a second 13,000-region search
(`runtime_memory_read_backing`), also on the recorder. The libc hotspot is almost certainly memmove (the ring copies,
2.18 MB a frame: the work itself). Built, each opt-in with a live off bit (second word):
**A1** backing copies without the table lock (Linux; `GOW3_BACKING_COPY=1`, 1048576), **A2** pending-write page
filter (`GOW3_PENDING_FILTER=1`, 2097152), **A3** register checksum two words per step (`GOW3_REG_HASH_PAIRED=1`,
4194304; the scanner and workers too), plus `PublishGraphics` skipping an empty map (no switch). All remove work.
Expected: stage A -0.5 to -0.9 ms of CPU a frame, the process -0.7 to -1.3 ms; FPS +0 to +2%. New lines "Stage A cuts
per frame", "Backing copies per frame". Tests: `runtime-test --backing-copies`, `pending-pages-test`,
`stage-a-hash-test` pass (Windows and WSL).

- **Build:** WSL `/root/src/medievil` switched from `deck-lab` `d20f010` to the local branch `stage-a-decode` (the
  code of `5e4e4b6`); `CC=clang CXX=clang++ GOW3_CPU_LEVEL= bash build.sh` (LTO, static, baseline, no warnings) and
  `packaging/linux/appimage.sh` with only its output name changed: **`dist/MediEvil-x86_64-stagea.AppImage`**
  (131,344,888 bytes, sha256 `718990178e7174e6e76a6d4c055b0dcf62a4254b4d638a921bd6c896de83b45e`), `--help` runs; every
  other `dist/` file checked unchanged (sha256). Not deployed.
- **Deck test** (one launch, the user's go): impl doc section 5: `deck.sh deploy /root/src/medievil
  MediEvil-x86_64-stagea.AppImage`, the three variables in extra-env.txt beside the current keys, `deck.sh toggle "0
  0"`, `deck.sh ab "0 0" "0 7340032" 30 5`, then one pair per bit; `deck.sh threads` and power in their own windows;
  look at the picture while toggling 1048576 (A1 carries the constants).

### Round 2 method: a fixed GPU clock for CPU-efficiency A/Bs (the user, 2026-11-05)

With the GPU clock on auto the 15 W controller moves power between GPU and CPU frame to frame: a CPU cut can boost the
cores on one frame and starve the next, and part of the saving becomes GPU clock instead of FPS. The user: test this
round unlocked (raw numbers), then **re-take the candidates with the GPU fixed a little above what the frame needs
(1,200-1,300 MHz, not 1,000)**, so the GPU's share is fixed and CPU savings stay on the CPU as boost. Round 2: a new
baseline at the fixed clock, then thread nice, pool populate, the stage A cuts (A1-A3, live bits) and the particle
single pass. For live-bit tests one launch can cover both clocks: the user changes the clock in the Quick Access Menu
between two `ab` runs and the log is split at that time. Note the clock with every result.

### Round 1 results, GPU clock auto (2026-11-05, ini logging 67% uncapped; launched by the user)

**Caveat (2026-11-07, found by the patch map, logs re-read):** the first live flip of A3 (second word 4194304, in
7340032) left draw preparation dead for the rest of the launch: "Draw preparation" 0 prepared from line 2646 of
`data/deck-stagea-1.log` (= `deck-stagea-1200.log`) to the end, in both states; the scanner is seeded from stage A
only at the first submission or a rebase (`DrawPreparation::BeginSubmission`). Rounds 1 and 2 below compared A1-A3
on / off with no prepared draws on either side. See `research/patches/LANDMINES.md` item 1.

| launch | log | change | FPS median | worst (median of 5 s) | p99 ms | verdict |
|---|---|---|---|---|---|---|
| 1 | deck-particle-1 | particle single pass (live, vs `0 8192`) | 79.4 vs 77.1 | 17.4 / 18.1 | 16.30 / 16.74 | +2.3 FPS, but the torch / floor glitch (also at 60) |
| 2 | deck-nav-1 | `GOW3_NAV_PURGE_NOFENCE=1` | 79.4 | 17.3 | 16.14 | neutral in FPS (main thread; power not measured) |
| 3 | deck-aniso-1 | `GOW3_FORCE_ANISO=0` | 79.3 | 17.1 | 15.98 | neutral (CPU-gated view); 16x stays (the user's choice) |
| 4 | deck-coresched-1 | `GOW3_STAGE_B_CORE_SCHED=1` | **66.4** | **35.4** | 22.79 | **rejected**: the sibling idles, stage A drain waits 2.1 -> 6.9 ms/frame |
| 5 | deck-nice-1 | `GOW3_THREAD_NICE=1` (rtkit: both threads nice -10) | **80.15** | 17.0 | **15.82** | small win (+0.8, sd 1.21): preset candidate |
| 6 | deck-populate-1 | `GOW3_POOL_POPULATE=1` | 78.4 | 17.95 | 16.54 | rejected in the static view: faults 9k -> 437k per 5 s, 537 ms per 5 s populating; motion-test candidate (streaming) |
| 7 | deck-stagea-1 | A1-A3 on vs off (live, `0 7340032`), build `d4145b5-stagea` | 78.1 vs 77.5 | 17.6 / 17.5 | 16.34 / 16.19 | +0.6 FPS (+0.8%), lows equal; A1 4,350 copies/frame without the lock, A2 filter answers 1,400 checks (scans 1,225 -> 0.4) |

Pool prefill dropped: demand faults are only ~1.8k/s. The user (2026-11-05): judge by the 0.1% lows and by pacing in
motion too; still-camera rejects may help in panning / combat. Round 2 (fixed GPU clock) re-takes nice, the stage A
cuts and the particle pass.

### Round 2 results, GPU fixed at 1,200 MHz (2026-11-05 ~20:51-21:06, same launch as stage A 1, log `data/deck-stagea-1200.log`)

The user locked the GPU at 1,200 MHz in the Quick Access Menu after the auto-clock A/B (stage A 1); the log was split
at the lock (the dip windows) and switch windows left out. Medians of 5 s windows (worst = median of the windows'
worst frames; max = the worst window).

| A/B @1,200 MHz | on | off | gain | worst median / max ms | p99 ms |
|---|---|---|---|---|---|
| stage A cuts A1-A3 (`0 0` vs `0 7340032`) | 79.25 | 78.60 | +0.65 | 17.8 / 20.5 vs 18.2 / 22.3 | 16.89 vs 16.96 |
| particle single pass (`0 0` vs `0 8192`) | 80.60 | 77.90 | +2.7 | 17.3 / **18.8** vs 18.5 / **36.0** | 16.20 vs 17.11 |

- The stage A cuts give the same +0.6 as at auto, with slightly better lows. Keep (Deck preset candidates).
- The particle pass also improves the lows (the worst window 36 -> 19 ms), but keeps the torch glitch until the fixed-step
  patch (study-particle-fixed-step.md) is verified.
- The fixed clock did not raise FPS here (79-81 as at auto): this view is CPU-gated either way.
- **Drift within one launch:** the two `0 0` segments at 1,200 MHz differ by 1.35 FPS (79.25 vs 80.60, same state).
  Between-launch results of under ~1.5 FPS (thread nice +0.8, populate -1) are inside that drift: only live A/Bs in one
  launch settle them. Thread nice should become a live switch before it is judged.

### Particle fix 1 (2026-11-05 ~21:10-21:28, build `49291f3-particlefix`, log `data/deck-particlefix-1.log`)

Launched by the user. Settings: stage A 1's plus `GOW3_PARTICLE_SINGLE_FIXED=1` and `GOW3_PARTICLE_FLIP_GUARD=1`; ini
logging (single pass on); GPU clock auto. The log: the patch applied ("GPU particle fixed step in the single pass"),
sim runs 1.00 a frame, 0.00 on the previous run's target; the guard skipped nearly every flip as "without a
simulation" (1,067-1,200 of each 1,200).
- **Visual (the user):** lone torch with the fix (`0 0`): "it's animating now"; guard off (`0 16384`): frozen again;
  Dan's crypt floor with the fix: "That glitch is also gone"; guard off: "Yup, it's back". **The flip guard fixes both
  glitches**: the lost update is a buffer flip without a simulation (study-particle-fixed-step.md's candidate).
- **Performance** (heavy view, `ab "0 0" "0 8192" 30 5`, GPU auto):

| state | n | FPS | draws | stage B | p99 ms | worst median / max ms |
|---|---|---|---|---|---|---|
| `0 0` fix (single pass + guard + fixed-step patch) | 27 | **81.2** | 1,102 | 85.9% | 15.64 | 17.0 / 19.7 |
| `0 8192` stock 1/30 step | 25 | 79.5 | 1,295 | 91.8% | 15.94 | 17.2 / 22.2 |

  +1.7 FPS (+2.1%), -193 draws, stage B -5.9 points, better lows.
- Next: one launch with the guard only (`GOW3_PARTICLE_SINGLE_FIXED` unset). If the torch and the floor are right, the
  fixed-step patch is not needed (it saves nothing measurable and costs flag-8 emitters their look-ahead step). Then
  the Deck preset gets the guard; the PC default stays as it is until regression-tested on `lab`.

### Particle fix 2: the guard alone (2026-11-06, build `49291f3-particlefix`)

Launched by the user. Particle fix 1's settings without `GOW3_PARTICLE_SINGLE_FIXED`. The user: "the torches still
worked"; Dan's crypt floor (same launch, the log: flip guard ready, no fixed-step line): "the crypt floor looks
fine". **The guard alone fixes both glitches**: `GOW3_PARTICLE_SINGLE_FIXED` is not needed and stays off (flag-8
emitters keep their look-ahead step). The Deck preset gets the guard (Deck Optimized preset, Switches).

### Thread nice, live (2026-11-06, build `5ab1a24-nicelive`, logs `data/deck-nicelive-1.log`, `data/deck-nicelive-1400.log`)

Launched by the user. Particle fix 2's settings (guard only, stage A cuts on) plus `GOW3_THREAD_NICE=1`. Stage B and
the vblank thread got nice -10 through rtkit at start (setpriority: Permission denied, RLIMIT_NICE 0). The toggle
file's second word 32768 puts them back at nice 0 live, and clearing it gives -10 again through rtkit. The log shows
both switches. Heavy view, `ab "0 0" "0 32768" 30 5`; medians of each block's non-switch windows.

| GPU clock | state | windows | FPS (block medians) | FPS median | worst median / max ms | sd ms | p99 ms | stage B | stage A wait |
|---|---|---|---|---|---|---|---|---|---|
| auto | nice -10 | 25 | 81.2, 81.7, 81.9, 81.8, 81.9 | 81.7 | 17.0 / 18.4 | 1.24 | 15.63 | 88-90% | 20-26% |
| auto | nice 0 | 25 | 81.5, 81.7, 82.2, 82.1, 82.4 | 82.0 | 16.7 / 19.2 | 1.27 | 15.41 | 91-92% | 28-32% |
| 1,400 MHz | nice -10 | 31 | 77.7, 77.3, 77.7, 78.1, 78.1, 78.0 | **77.8** | **18.4 / 20.4** | **1.82** | **17.38** | 74.6% | 4.4% |
| 1,400 MHz | nice 0 | 25 | 77.2, 76.6, 77.6, 77.6, 77.6 | 77.4 | 19.1 / 21.5 | 1.98 | 17.64 | 74.2% | 5.0% |

- **GPU auto:** neutral; nice 0 is 0.3 FPS ahead, inside the noise. Nice -10 does run the threads sooner (stage B
  busy 2-3 points less, stage A waits about 7 points less), but the frame is set by the GPU and the power cap.
- **GPU fixed at 1,400 MHz** (the user: where the next view starts to lose FPS while static): nice -10 wins every
  block. Each nice 0 block is below both of its nice -10 neighbours. +0.5 FPS (mean 77.82 vs 77.28), worst median
  -0.7 ms, sd 1.82 vs 1.98 (lower in every block), p99 -0.26 ms. The gain is mostly in frame pacing.
- Kept for the Deck preset (with the 1,400 MHz result); a motion test is still worth doing. It is Linux-only: nothing
  changes on Windows.

### Drain wake-ahead 6 vs 2, live (2026-11-06, build `d2f145f-lows`, log `data/deck-wakeahead-1.log`)

Launched by the user. The Deck preset's switches (stage A cuts, flip guard, thread nice) plus
`GOW3_DRAIN_WAKE_AHEAD=6`; the logging ini; GPU fixed at 1,400 MHz. New in this build: second toggle word 8388608 puts
the count back at the default 2 live ("wake N ahead" in the drain line follows it), and each "Frame pacing" line
ends with the window's frame count and slowest 2%, which `deck_summary.py` pools into a state's 1% and 0.1% lows.
Heavy view, `ab "0 0" "0 8388608" 30 5`.

| state | windows | FPS (block medians) | FPS median | low 1% / 0.1% FPS | worst ms | p99 ms | drain wait ms/frame | sleeps/frame |
|---|---|---|---|---|---|---|---|---|
| 6 ahead | 27 | 76.1, 77.0, 76.8, 76.8, 76.8 | 76.8 | 56.5 / 51.8 | 18.7 | 17.74 | 0.63 | 1.66 |
| 2 ahead | 25 | 76.2, 76.2, 76.8, 76.6, 76.4 | 76.5 | 56.6 / 51.0 | 19.1 | 17.64 | 0.62 | 1.50 |

- **Neutral; the default 2 stays.** +0.3 FPS is inside the noise, the lows and p99 match, and stage A's drain wait is
  the same over all windows. The earlier wake does not shorten the wait: stage A wakes before the end more often and
  sleeps again (1.66 vs 1.50 sleeps a frame). The 51 us wake latency is not what stage A waits on.

### Static vs shared GPU library (2026-11-06, build `d2f145f-shared2`, log `data/deck-shared-1.log`)

Launched by the user. `GOW3_STATIC_GPU=OFF` (the runtime loads `libgow3gpu.so` from the AppImage, checked in
`/proc/<pid>/maps`), the Deck preset's switches, the logging ini, GPU fixed at 1,400 MHz, 5 minutes in the heavy view
without toggling. Against wake ahead 1's static build of the same commit (the previous launch):

| build | windows | FPS median | low 1% / 0.1% FPS | worst ms | p99 ms | stage B | HZB hidden |
|---|---|---|---|---|---|---|---|
| shared | 62 | 74.7 | 55.6 / 51.3 | 19.2 | 17.87 | 76.8% | 42,999 |
| static (2 ahead) | 25 | 76.5 | 56.6 / 51.0 | 19.1 | 17.64 | 75.1% | 35,134 |

- **Static stays (it is the default).** -1.8 FPS is past the ~1.5 FPS between-launch noise, stage B is 1.7 points
  busier (calls across the library boundary through the PLT, no LTO across it), and the user saw the shared build
  "more sensitive to camera movement induced drop". The static-view lows match. The camera spot was not identical
  (HZB hidden 43k vs 35k), so the gap is approximate; the direction and the user's motion report agree.

### Two draw-preparation helpers (2026-11-06, build `d2f145f-lows`, log `data/deck-helpers2-1.log`)

Launched by the user. Wake ahead 1's settings without the wake-ahead count, plus `GOW3_PREP_WORKERS=2` (the Deck's
default is 4, half its 8 threads; the log: "draw preparation workers: 2 (+1 scanner, 8 hardware threads)"). The user
moved between views and between GPU auto and 1,400 MHz during the run, and the log does not record the clock, so only
the last 17 windows (the heavy spot of wake ahead 1, 1,142 draws, the clock at 1,400 MHz then) are compared:

| at 1,400 MHz | windows | FPS | low 1% / 0.1% FPS | sd ms | p99 ms | stage B | stage B packet-ring sleeps |
|---|---|---|---|---|---|---|---|
| 2 helpers | 17 | 76.3 | **57.8 / 52.6** | **1.66** | **17.20** | 68.8% | 3.25 ms/frame |
| 4 helpers (wake ahead 1, 2 ahead) | 25 | 76.5 | 56.6 / 51.0 | 1.88 | 17.64 | 75.1% | |

- **The user:** "1400mhz was a little ambitious, the CPU gets starved. However, in 'auto' mode, 2 helpers is worth a
  couple frames." At 1,400 MHz: the same FPS, better pacing and lows (fewer busy threads at the 15 W cap). Not yet
  measured at auto: launch 14 below.
- **The user on the clock range:** the Deck's GPU ceiling is already 1,600 MHz, so a range test uses a ceiling of
  ~1,200-1,300 MHz. A fixed 1,400 MHz in a fixed heavy view still has a lot of frame-time instability: the CPU work
  itself is uneven (perhaps some stages boost high in some frames and cannot in others).

## Clock push-pull at GPU auto (2026-11-06; the user's model; tooling `5ead842`, not run)

**The model (the user):** the game's work is cyclical (the game thread, then the translator, then the GPU). CPU idle
lets the GPU boost; the CPU then lacks power for the heavy part of the next frame; the GPU starves and drops its
clock; the CPU boosts and finishes early and waits; the GPU boosts again. Not 1:1, but heavy views alternate 16-20 ms
and ~10 ms frames. Some frames may also simply cost more with a still camera (seen on the 4090 too, where the
throughput and a proper frame pacer hide it). The two need different fixes, so the trace tells them apart first.

**Tooling** (`5ead842`):
- `apu_trace.py` (`deck.sh trace [s] [ms]`, no root): every 4 ms on CLOCK_MONOTONIC, the amdgpu metrics table
  (gpu_metrics 2.x: GPU current / average clock, busy, socket / CPU / SoC / GPU power, core clocks) and, for stage A,
  stage B, the recorder, the vblank thread and the 4 busiest other threads, the CPU each last ran on and its run time
  (schedstat ns). Prints how often the table really changed and its own CPU (3.4% of a core in a WSL test).
- `frame_clocks.py` (`deck.sh frames NAME`): per frame (between "presented" notes of `GOW3_FLIP_TRACE`, stages
  matched per flip from its submit): frame time, game submit interval, submit -> irq (recording), irq -> gpu_ready
  (GPU), the GPU clock over the frame and over its GPU window, power, each traced thread's run ms and its core's clock.
  Prints frame-time autocorrelation at lags 1-8 (negative at 1: alternation), the share of alternating pairs, means
  by frame-time band (thirds, spikes > 1.3x median) and correlations one frame before / same / after. Checked on a
  synthetic run with a planted alternation (autocorrelation -0.98, GPU clock 900 vs 1,500 MHz found by band).
- `deck.sh pull-trace NAME`: both CSVs to `research-data/deck/` (not in git). The toggle file's optional third word
  is a live frame cap in FPS ("Runtime: toggle file frame cap=N"; `deck_summary.py` labels the state `capN`), and
  `deck.sh steps PERIOD CYCLES "S1" "S2" ...` cycles any number of states: a cap sweep in one launch.

**What tells them apart:** push-pull = frame times alternate (lag-1 autocorrelation clearly negative) and the GPU
clock and the CPU / GPU power split swing with them, leading by about a frame. Uneven work = long frames carry more
recording or GPU time at steady clocks; what is left at a fixed clock is the CPU side.

**Launch 14** (build `5ead842-trace`; the Deck preset's switches + `GOW3_PREP_WORKERS=2` +
`GOW3_FLIP_TRACE=/home/deck/.local/share/medievil-pc/user/flip-trace.csv`; logging ini, uncapped; heavy view):
1. GPU auto: `deck.sh trace 60`, then 5 minutes of windows (2 helpers at auto against launch 10's 81.7 FPS).
2. The user fixes the GPU clock (1,200-1,400 MHz): `deck.sh trace 60` (the CPU side's own unevenness).
3. GPU auto again: cap sweep `deck.sh steps 30 3 "0 0" "0 0 75" "0 0 70" "0 0 65"`, with `deck.sh trace` beside it.
4. `deck.sh pull-trace deck-pushpull-1`, `deck.sh frames deck-pushpull-1 --skip S --seconds 60` per part.

**Fixes, if it is push-pull (cheapest first):** a cap just under the auto average (step 3); `frames_queued=2` (the
GPU command thread waits for the frame two flips back instead of one, `vk_presenter.cpp` PrepareFrame: a second
frame of slack, so a slow CPU frame does not leave the GPU without work; up to one more frame of latency; the in-game
menu's "Frames queued" switches it live, so it fits launch 14. Its comment: unbounded, the command thread ran ahead
and blocked in bursts, 12.5/25 ms alternation at 80 FPS, the reason the default is 1: 2 may bring some of it back); a GPU
clock range instead of a fixed clock (`power_dpm_force_performance_level` manual + `pp_od_clk_voltage` "s 0 <floor>",
"s 1 <1,200-1,300>", "c": a system change the user makes with sudo, lost at reboot; does Steam's QAM reset it?). If
it is uneven work: which frames cost more and on which stage, from the same per-frame table.

### Launch 14 results (2026-11-06 ~19:45-20:22, build `5ead842-trace`, 2 helpers, logging ini 67% uncapped; log `data/deck-pushpull-1.log`, traces `research-data/deck/deck-pushpull-{auto1,fixed1100,capsweep}-*.csv`)

- **2 helpers at auto:** 80.8 FPS mean over 5 clean minutes (median 80.95, low 1% 63.7, low 0.1% 56.4) against launch
  10's 81.7 with 4 helpers: inside the between-launch noise; the user's "+2 FPS" is not confirmed in this view.
- **Auto, 60 s trace** (sampler 23.7% of a core at 4 ms: too costly; the SMU table changes every ~8 ms, so 8 ms
  sampling from now on): socket power 14.8-15.0 W in every frame-time band, so the cap binds and frame time follows the
  split. Long frames: CPU +330 mW and +60 MHz, GPU -480 mW and -170 MHz over its own GPU window (1,112 -> 940 MHz),
  GPU busy 97 -> 91%; stage A / B are a smaller share of a long frame (they wait more). Frame time vs the GPU-window
  clock -0.41; lag-1 autocorrelation -0.23 (mild alternation, not every other frame).
- **What the user's graph shows is the present call** (`present_start` in the flip trace): p1 ~10.4 ms, p99.9 ~17.8 ms
  in the traced auto run, matching the overlay. The summary's median FPS and SD hide it; the lows columns and the
  count of present intervals over 15 ms per minute show it. The game's own submit cadence is rougher than what reaches
  the screen (the queue absorbs most of it).
- **Fixed 1,100 MHz (the auto run's average clock), same view, sampler 13.2% at 8 ms:** present intervals over 15 ms
  26/min (sampler-free auto: 57/min), p99 14.6 ms (15.4), GPU time SD 1.05 ms (1.45), same FPS in the windows
  (~80.9). **The CPU work is as uneven as at auto** (the user's point): per-frame run time CV stage A 16% (14),
  stage B 12% (13), recorder 13% (13), game main thread 11% (12.5), RenderThread 23% (18). Locked, the GPU stops adding
  its own swing on top, and the queue absorbs most of the CPU swing; what spikes remain carry the GPU idle (86% busy)
  and a long game submit gap (CPU side). Every traced thread runs a near-fixed share of each frame (game main thread
  93-94%, stage B 73-75%, stage A 55-56%), so per-thread ms vs frame time correlates by construction, and schedstat
  run time counts spin-waits too: which CPU work varies needs per-flip busy time without waits (not built).
- **Cap sweep at auto, live** (`deck.sh steps 30 3`, no sampler, first 3 s after each switch left out; present calls):

  | cap | FPS | median | SD | p99 | p99.9 | max | > 17 ms / min |
  |---|---|---|---|---|---|---|---|
  | none | 80.5 | 12.27 | 0.96 | 15.42 | 16.67 | 19.5 | 4 |
  | 75 | 74.9 | 13.33 | 0.22 | 13.56 | 14.14 | 26.3 (one hitch) | 2 |
  | 70 | 70.0 | 14.29 | 0.10 | 14.52 | 14.66 | 15.8 | 0 |
  | 65 | 65.0 | 15.38 | 0.06 | 15.55 | 15.68 | 16.2 | 0 |

  The game's submit SD falls 1.40 -> 0.67 ms at 75: with slack the work evens out. A cap a few FPS under the
  uncapped rate makes the pacing flat at auto; the push-pull needs no other fix.
- **The user's decision:** no finer sweep; a 60 cap is fine on this screen. **The goal stays cutting CPU work** for
  headroom (higher settings, fewer stutters). From now on CPU cuts are judged at a 60 cap by CPU power and per-thread
  CPU time (the saving shows as idle time and watts there, not as FPS).

## CPU cuts at a 60 cap (2026-11-06; tooling `7379aa0`, signal waits `2a6efea`; not run)

**Method (the user's goal: CPU headroom, judged at the 60 cap they play at):** at a fixed frame rate a cut shows as
idle time and watts, not FPS. The live cap word holds 60 (`"W1 W2 60"`) while the second word switches one cut;
`deck.sh trace 420 50` (sampling every 50 ms is enough for averages; the APU trace now keeps its files open, 1.2%
of a core at 8 ms in a WSL test, and logs the whole process's CPU) runs beside `deck.sh steps`, and `deck.sh power
NAME` (`state_power.py`) gives per state: FPS (did the cap hold), socket / CPU / SoC / GPU power, GPU clock and busy,
the process's CPU (% of one core) and each traced thread's.

**Signal waits** (`gow3_signal_wait.h`, `GOW3_SIGNAL_SLEEP=1`, second word **16777216** off live; opt-in): the
recording thread's waits for the recorder, `WaitDeferredSignals` (before WRITE_DATA, end-of-shader fences, the flip)
and `WaitHostCopies`, called `sched_yield` until done: "blocked: host copies" 7-8% of stage B's time in launch 14
(~0.9 ms a frame at 80 FPS). Now a pause spin of `GOW3_SIGNAL_SPIN_US` (30) then a futex sleep; the recorder / copy
threads raise their counters as before and then wake sleepers only if one is registered (a fence and a load
otherwise). Wake latency on the Deck ~70 us (stage A's drain line): a few a frame cost stage B time uncapped, none at
a cap with slack. "Recorder waits per frame" line on both paths. `signal-wait-test`: two waiters, a producer with
2 / 80 / 400 us gaps, watchdog on lost wakes.

**Launch 15** (build `2a6efea-signal`; the preset's switches + `GOW3_PREP_WORKERS=2` + `GOW3_SIGNAL_SLEEP=1` +
flip trace + frame stats; logging ini uncapped, the cap by the toggle file; GPU auto; heavy view, still):
1. Signal waits: `deck.sh trace 420 50` beside `deck.sh steps 30 6 "0 0 60" "0 16777216 60"`, then `deck.sh
   pull-trace deck-cap60-signal` and `deck.sh power deck-cap60-signal`.
2. The cuts already in the preset, in watts for the first time: `steps 30 4 "0 0 60" "0 4096 60" "0 7340032 60"
   "0 983040 60"` (drain hybrid off; stage A's A1+A2+A3 off; stage B's R1+R2+T1+T2 off) with a 540 s trace.
3. Uncapped, one pair (`"0 0" "0 16777216"`, 30 s x 3): what the signal sleep costs in FPS when stage B is the limit.

### Launch 15 results (2026-11-06 ~20:50-21:28, build `2a6efea-signal`, a new arena with a guaranteed heavy view (the old spot cannot be found under a cap), GPU auto; log `data/deck-launch15.log`, traces `research-data/deck/deck-{cap60-signal,cap60-cuts,uncap-cuts}-*`)

**Caveat (2026-11-07, found by the patch map, logs re-read):** the first live flip of A3 (part of 7340032) left draw
preparation dead for the rest of each launch segment (`data/deck-launch15.log`: 0 prepared from line 9198 to the
in-game restart at 17676, and from 20691 to the end). Signal sleep at 60: unaffected (before the flip). The cuts at
60: only round 1's "all on" and "drain hybrid off" had draw preparation; A1-A3 off, R1/R2/T1/T2 off and rounds 2-4
had none. **The uncapped table (rounds 2-4): no draw preparation in any state**, so it measures the cuts with stage B
doing all preparation itself, not the preset as played; R1/R2/T1/T2's share is likely overstated (T2 serves draws
without a prepared draw). To redo with A3 out of the live set (A1+A2 = 3145728) or A3 restart-only.
See `research/patches/LANDMINES.md` item 1.

- **Signal sleep at 60** (162 s each): FPS 60 / 60, socket 15.03 / 15.05 W, CPU 7.93 / 7.96 W, process CPU 362 / 363%,
  stage B 49.1 / 50.0%: neutral. It works (1.5 of 2.7 waits a frame slept; wait time 0.38 vs 0.62 ms a frame) but each
  slept wait is woken ~12 times (every host copy's Notify wakes it, its target or not: 42 wakes a frame). Uncapped
  (rounds 2-4): sleep off 78.8 vs on 78.3 FPS, present intervals > 15 ms 96 vs 120 / min: the wake latency's sign,
  inside the noise. **Not for the preset**; if picked up again, wake only at the waiter's target.
- **At the 60 cap the APU still draws its 15 W:** GPU 83-85% busy at ~1,040 MHz, 3.5 W; **CPU cores ~3.2-3.4 GHz
  (near the 3.5 GHz top), 7.7-7.9 W**; process CPU 3.6-3.7 cores. Uncapped in the same arena: GPU 96% busy at ~1,115
  MHz, 4.3 W; CPU 2.89 GHz avg, 6.7 W; process 5.5 cores. The cap frees GPU work and the power manager gives what
  is left to the CPU's boost, so the socket stays at 15 W.
- **The preset's cuts, each switched off (second word), at 60** (108 s each): process CPU +4.8 (drain hybrid),
  +9.9 (stage A's A1-A3), +14.6 (stage B's R1/R2/T1/T2) points of a core; CPU power +0.00-0.03 W, socket +0.01-0.06
  W: the saved CPU time becomes idle at full clock and voltage, not watts.
- **The same uncapped** (30 s x 4 rounds; round 1 left out: FPS rose 64.6 -> 76.9 through it while the arena
  settled, with 527 / 414 ms hitches; 81 s per state; GPU 96% busy, so FPS is GPU-bound here):

  | state | FPS | process CPU % | stage A % | stage B % | present > 15 ms / min | > 17 ms / min | p99.9 ms |
  |---|---|---|---|---|---|---|---|
  | all on | 78.3 | 553.5 | 70.7 | 76.0 | 120 | 3 | 16.84 |
  | drain hybrid off | 78.3 | 569.3 | 86.5 | 76.2 | 108 | 10 | 17.82 |
  | A1-A3 off | 78.1 | 557.5 | 74.1 | 77.9 | 112 | 4 | 16.89 |
  | R1/R2/T1/T2 off | 77.6 | 554.6 | 73.9 | 81.2 | 122 | 5 | 17.29 |
  | signal sleep off | 78.8 | 557.2 | 71.5 | 78.4 | 96 | 7 | 17.67 |

  The cuts keep their CPU savings uncapped (drain hybrid 16 points of a core on stage A, the per-draw cuts 5 on
  stage B) and the preset keeps them; FPS moves at most 0.7 here because the GPU is the limit.
- **Next (proposed):** a CPU clock ceiling at the 60 cap (`scaling_max_freq`, amd-pstate-epp, all cores; the user
  with sudo: 3.0 / 2.6 / 2.2 GHz, restore 3501250), a 60 s trace each: the lowest ceiling that holds 60 by the 0.1%
  lows, and the watts it gives back. Capping every core lowers the shared voltage (capping some did not, 2026-11-05).

## Hilltop Mausoleum lighting: pool pages leaked on Linux (2026-11-06/07; fixed `7246f88` + `f74e1a1`)

**Symptom (the user, Deck):** the Hilltop Mausoleum lit in wrong colours, surface by surface (pink, green, blue,
over-bright; "like a normal map"), different every launch, the same after a level reload, white splotches on pillars
and a sparkle on the silver shield. The user's own PC preset never showed it; the PC with the Deck preset did once.

**Root cause [code, test, Deck runs]:** UE4 decommits a render target's pool memory while the target is unused
(`sceKernelMemoryPoolDecommit`) and commits it again at the same address. Windows hands the same physical pages back
(M1 pool views kept across a decommit, `40a416a` / `3c5704e`, a performance patch): the target gets its own bytes.
Linux (`pool_keep_views()` = 0) put decommitted pages on the free list un-zeroed and a commit took any free pages:
the target came back holding another allocation's bytes. The original import `ac900f8` already did this on both
platforms; Windows stopped by accident with M1; D4 (`b7e947d`) only changed locking. Victims found: UE4's sky light
capture cubemap (128x128 FP16, 8 layers: NaN and +-65504 in every mip), the 9x1 RGBA16F sky light SH target, the
luminance chain's mips, the reflection capture arrays (128x128 L8 x 128/512/1024, uploaded once from guest memory
"was undefined"). The view constants (words 624-651, sky irradiance) held garbage on the Deck, exact zeros on the PC.
`runtime-test --pool-pages` reproduces it: with `GOW3_POOL_KEEP_PAGES=0` another range's bytes leak both ways.

**Fix (`7246f88`, Linux):** kept pages, Windows' behaviour without views. A decommit leaves its pages kept for their
address (`pool_owner`, `kept_table`); a commit takes back the pages kept for its address first; a kept page another
address takes is zeroed first (`pool_zero_foreign_locked`); `f74e1a1`: a failed commit zeroes the pages it frees.
`GOW3_POOL_KEEP_PAGES=0`: as before. Frame stats "Guest pool faults" line: kept pages taken back / commits without /
another address's zeroed. **Deck result (the user):** "everything loaded in correctly"; 60 FPS; ~64,000 kept pages
taken back per 5 s, 0 commits without, 0 zeroed; minor page faults ~7-35k per 5 s (broken baseline ~6-33k).
`GOW3_POOL_ZERO_DECOMMIT=1` (`b42c0a8`, test) also fixed it but cost ~40x the page faults (~231k per 5 s).

**Open (not changed here):**
- Windows still leaks once no never-used page is left: `pool_take_locked`'s second loop hands out kept pages
  un-zeroed, and a stale kept view can get another owner's bytes via `pool_take_at_locked` [code, research agent].
  Smaller pools (PS4 memory sizes, the Deck preset on the PC) reach that sooner: the one PC glitch. Fix: zero there
  (`host_discard` is a memset on Windows). Belongs to `lab` with its regression pass.
- Under pressure a Linux recommit can get zeros instead of its own bytes (pages taken by another address, or a
  `kept_table` slot replaced): safe but not PS4-identical; the counters show it.
- The spacepepperoni fork (Linux, same runtime lineage) very likely has the same leak (their "purple reflections,
  random colours per run").

**Dead ends on the way (each ruled out by a run; kept as opt-in diagnostics):** A2 / every Deck switch / the finer
culling patch / particle single pass / 4K memory (bisect), undefined-target uploads (`GOW3_NEW_IMAGES_UNDEFINED=0`),
RADV `zerovram`, stale first readbacks (`GOW3_READBACK_FIRST_WAIT`), tiny tiled target copy-back
(`GOW3_TINY_TARGET_READBACK`, off by default: frame cost), GPU writes inside images (`GOW3_GPU_WRITE_SUBRESOURCES`).
One run "repaired itself" with `GOW3_TEXCACHE_STATS=1`; not reproducible (memory reuse luck).

**Tools added (deck-lab):** RenderDoc on the Deck (`VK_LOADER_LAYERS_ENABLE=VK_LAYER_RENDERDOC_Capture` in
extra-env; `renderdoccmd` injection kills the runtime), remote trigger `research/tools/renderdoc/trigger_capture.py`
(SSH tunnel 38920), remote replay on the Deck (`renderdoccmd remoteserver` inside the SSH session, tunnel 39931 ->
39920; set `MSYS2_ENV_CONV_EXCL=RD_CAPTURE` in Git Bash), `pass_images.py`, `pixel_draw_textures.py`,
`debug_pixel_values.py`, `resource_names.py`, `disasm.py` remote; the frame map past the UI passes,
`GOW3_CAPTURE_BUFFER_BYTES`, `GOW3_CAPTURE_ON_TARGET=WxH[:Format]` (load-time passes) with texel statistics;
`GOW3_READBACK_LOG`, `GOW3_SKIPPED_TARGET_LOG`, `GOW3_IMAGE_WATCH=WxH[:min_layers]`.

## Cuts redo with draw preparation alive (2026-11-07, build `4f49400-a3restart`: A3 restart-only)

Launch 15's cut table had no draw preparation after its first A3 flip (caveat above). Redone in one launch, the same
arena and heavy view, still: the preset's switches + 2 prep helpers + frame stats + flip trace, logging ini (67%,
uncapped), GPU auto. `steps 30 4` with `"0 0"`, `"0 4096"` (drain hybrid off), `"0 3145728"` (A1 + A2 off; A3 stays
on), `"0 983040"` (R1/R2/T1/T2 off), first with the cap word 60 beside `trace 540 50`, then uncapped beside
`trace 520 50`. Log `data/deck-cutsredo.log`; traces `research-data/deck/deck-{cap60,uncap}-cuts2-*`.
**Draw preparation alive in all 287 windows** (73-97% prepared; the low end is the load).

Setup miss on the way: the first capped steps did nothing. The test env lacked `GOW3_TOGGLE_FILE`, so the game read
the launcher's default `user/frame-dumps/toggles.txt` while `deck.sh` wrote `toggles.txt` (LANDMINES item 2 on the
Deck). Fixed live with a link for this run; `deck.sh env` now always writes the line.

**At the 60 cap** (108 s per state; cap held at 60.0 in every state):

| state | process CPU % | stage A % | stage B % | CPU mW | socket mW | present > 17 ms / min |
|---|---|---|---|---|---|---|
| all on | 364.6 | 41.4 | 50.2 | 5,198 | 11,454 | 40 |
| drain hybrid off | 372.2 (+7.6) | 50.1 | 50.1 | 5,409 | 11,656 | 50 |
| A1 + A2 off | 366.6 (+2.0) | 43.0 | 50.6 | 5,362 | 11,627 | 50 |
| R1/R2/T1/T2 off | 368.4 (+3.8) | 42.5 | 53.2 | 5,365 | 11,573 | 49 |

**Uncapped** (108 s per state; GPU 97-98% busy, so FPS is GPU-bound):

| state | FPS | process CPU % | stage A % | stage B % | present > 15 ms / min | > 17 ms / min | p99.9 ms | worst ms |
|---|---|---|---|---|---|---|---|---|
| all on | 82.8 | 549.2 | 56.3 | 76.2 | 54 | 1.7 | 16.48 | 17.88 |
| drain hybrid off | 81.9 | 572.9 | 84.7 | 75.5 | 88 | 1.7 | 16.79 | 17.82 |
| A1 + A2 off | 82.8 | 553.8 | 61.0 | 76.0 | 57 | 2.2 | 16.43 | 17.56 |
| R1/R2/T1/T2 off | 81.7 | 551.9 | 59.4 | 80.8 | 89 | 3.3 | 16.60 | 20.40 |

Against Launch 15 (dead draw preparation; points of a core at 60): drain hybrid +4.8 -> **+7.6** (larger), A1-A3 +9.9
-> A1 + A2 **+2.0** (most of the old figure was the dead preparation, and A3 is now unmeasured: restart pair only),
R1/R2/T1/T2 +14.6 -> **+3.8** (the old figure was inflated: T2 serves unprepared draws, which were all of them).
Uncapped, drain hybrid and the stage B cuts each keep ~1 FPS and about 35 fewer presents over 15 ms a minute; A1 + A2
show nothing outside their CPU time. All cuts stay in the preset.

**Different from Launch 15: the APU was not at its 15 W.** At the cap the socket drew 11.5 W, the CPU 5.2 W with cores
at ~2.96 GHz (Launch 15: 15 W, 7.8 W, ~3.21 GHz), GPU 73% busy at ~1,008 MHz (83-85% at ~1,040). So each cut now
shows as watts too: +0.12 to +0.20 W socket, +0.16 to +0.21 W CPU. Why the clocks differ is open: EPP reads
`balance_performance` and `scaling_max_freq` is the full 3.5 GHz now; what Launch 15 had is not recorded (a reboot
resets any EPP change made with sudo). Uncapped both runs reach ~15 W.
