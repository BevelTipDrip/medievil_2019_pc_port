---
id: upstream-linux-build
title: Upstream Linux build and AppImage script
area: deck
status: active
platforms: linux
default_windows: n/a
default_deck: on
origin: upstream 1.3.7
class: G
commits: cc8da6c, 60230fd, 4ec0013, 2c20e99
files: packaging/linux/appimage.sh, packaging/linux/AppRun, packaging/linux/build-tk-xft.sh, packaging/linux/README-Linux.md, gpu/shadps4/video_core/renderer_vulkan/vk_platform.cpp, gpu/CMakeLists.txt, src/probe.c, src/runtime_extra.c, run.py, launcher/medievil_launcher.py
symbols: host_lib, collect, fetch, check_local, newest, CreateInstance, DynamicLoader, GOW3_SYSTEM_XBYAK, runtime_restart, kernel_nanosleep, wait_for, linux_shortcut, start_new_session
switches: GOW3_DATA_DIR, GOW3_SCREENSHOT_DIR, GOW3_RESTART_COMMAND
tests:
docs: fix-inventory.md#from-upstream-medievil-133-138-bc160201983-and-contributors-merged-into-lab, plan-linux-deck.md#11-what-exists-code, plan-linux-deck.md#13-stale-or-dead-pieces-code, plan-linux-deck.md#14-known-linux-problems-on-lab, plan-linux-deck.md#15-what-was-tested-by-whom
depends_on:
related: linux-build-lab, static-gpu-lib, S1-v3-build, thinlto, avplayer-join-hang, upstream-screenshots, deck-preset, extra-env-file, D1-tls
checked: 2026-11-07
---
## What it does

Upstream PR #13 (nydal91, merged in 1.3.7 as `2c20e99`) makes the port run natively on Linux and packages it as one
AppImage. It builds on `build.sh`'s Linux path, which it does not change (inherited from the Bloodborne base: CMake
GPU library, runtime linked with `$CC`). Code: `CreateInstance` opens `libvulkan.so.1` explicitly; the submodule's
xbyak when the system has none (`GOW3_SYSTEM_XBYAK`); `kernel_nanosleep` uses `nanosleep`; `runtime_restart` execs
`GOW3_RESTART_COMMAND`. `run.py` and the launcher get Linux branches (exec restart, `xdg-open`, `.desktop` shortcut,
process-group stop, fontconfig fonts). `appimage.sh` copies the built programs and every library `ldd` finds that
`host_lib` does not exclude, plus a standalone Python with Tk; `AppRun` adds `--play`, `--run`, `--vulkan-info` and a
glibc check.

## Invariants

- One Vulkan loader, the host's. In `CreateInstance` the `__linux__` branch must keep `DynamicLoader dl{"libvulkan.so.1"}`:
  vulkan.hpp's default tries `libvulkan.so` first, which can be a second loader whose interposed functions crash the
  instance. `host_lib` must keep `libvulkan*` and the GPU stack (`libGL*`, `libEGL*`, `libdrm*`, `libnvidia*`) out of
  the bundle for the same reason.
- `host_lib` decides what is bundled: everything `ldd` finds that it does not list (`collect`). A library the host
  GPU driver also links must be on the list, or a bundled older copy loads first. That is how libwayland broke every
  Mesa driver on SteamOS until linux-build-lab added `libwayland-*` and `libgbm*`.
- glibc comes from the host. `newest GLIBC` writes `usr/lib/compat/glibc`, and `AppRun` refuses older systems.
  libstdc++ is bundled only as a fallback, used when the host's lacks the `GLIBCXX` version in `compat/needs`. The
  floor is the build machine's glibc (`8072f54`'s message: binaries 2.38, the AppImage 2.39+).
- The image is read-only and `AppRun` runs from `PORT`. It sets `GOW3_DATA_DIR` (default
  `${XDG_DATA_HOME:-~/.local/share}/medievil-pc`) and `GOW3_SCREENSHOT_DIR` under it: the presenter's fallback is a
  relative `screenshots`, inside the image. Bytecode is compiled at build time (`PYTHONDONTWRITEBYTECODE=1`).
- Pinned downloads: `fetch` refuses an appimagetool, type2 runtime or python-build-standalone file whose SHA-256
  differs from its pin (`60230fd`), and `build-tk-xft.sh` refuses a moved Tcl/Tk tag. A version bump needs the new hash.
  The launcher's wheels are pinned by version only; SDL3, FFmpeg and xxHash come from the build machine, unpinned.
- Restart on Linux: `run.py` sets `GOW3_RESTART_COMMAND` with `shlex.join` and no `--after <pid>`. `runtime_restart`
  closes descriptors 3+ (`close_range`, the old GPU device goes) and execs it via `/bin/sh -c`; exec keeps the PID, so
  a `--after` wait would wait for itself. Without the variable it falls back to `run.sh` (stale, see below); the
  overlay's "Apply and restart" is disabled then.
- Windows behaviour is unchanged: the changes sit behind `_WIN32` / `__linux__` / `WIN32` / `WINDOWS`, except two
  equivalent lines (`global_key = vk::PipelineBinaryKeyKHR{}`, run.py's "not found" message).

## Interactions

- linux-build-lab (`8072f54`): `lab` no longer compiled on Linux after the merge (Windows-only host map stats and
  GPU-written bitmap). It restored the build and added `libwayland-*` / `libgbm*` to `host_lib` (spacepepperoni
  `22d6f92`).
- static-gpu-lib: replaced the inherited shared `libgow3gpu.so` (`-rdynamic`, `--allow-shlib-undefined`) as the
  default; it taught `appimage.sh` to read `NEEDED` with `objdump` and handle both. With the static library,
  D1-tls's TLS model no longer matters.
- S1-v3-build extends this script (`out/cpu-level`, `-v3` AppImage, AppRun's CPU flag check); thinlto changes the
  `build.sh` build whose output it packages.
- avplayer-join-hang: the hang after the first movie ("most starts hung here") is the main Linux problem on this
  base. Its port (upstream PR #68, `b182a97325`) is only on branch `fork-ports`, not in `lab` or `deck-lab`.
- upstream-screenshots: `4ec0013` builds the 1.3.6 screenshot and scheduler diagnostics on Linux and sets
  `GOW3_SCREENSHOT_DIR` in `AppRun`.
- deck-preset, extra-env-file: launcher and `run.py` features that run through these Linux branches (extra-env.txt
  lives in `GOW3_DATA_DIR`).

## Evidence

- Reports (plan-linux-deck.md 1.5, not ours): Fedora 44, RX 5700 XT, upstream 1.3.7: menus, gameplay, audio and
  pad work, 25.7 FPS against 60 on Windows (#30). AppImage on a Deck LCD: graveyard 13-18 FPS, against 7-13 for the
  Windows build under Proton (#13 comments).
- Ours: native run 1 (2026-11-03, AppImage of `8072f54` on this script): graveyard mostly 60 FPS, worst 21-25 ms,
  against 48-54 under Proton (the plan: "Native is clearly ahead of Proton"). Not a user quote; the Deck runs
  since use this script's AppDir / AppImage.

## History and pitfalls

- Compiler: PR #70 reports GCC 14 failing on the `_Static_assert` in `src/actor_hook.h:26` included from C++ (plan
  1.1; plan 1.3: "Today's tree does not build with GCC"), yet README-Linux still says "GCC 14+ also works". Use Clang
  (README: Clang 18+). Vulkan headers: PR #70 reports 1.4.363+ needed (1.4.328 fails), but `8072f54` built with 1.4.344.
- Still open: the bundle still takes every other library `ldd` finds (FFmpeg pulls gnutls and Samba libraries,
  ~150 MB; #13 owner review, #30).
- Stale: `run.sh` is the God of War III script (`../CUSA01623`, `gow3-probe`), and `shell.nix` lists GCC.
  `launcher/vulkan_check.py` (Bloodborne's bundled-Mesa picker) is called only by its test. Linux runs go through
  `run.py` (`AppRun --run`).
