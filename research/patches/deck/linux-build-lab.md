---
id: linux-build-lab
title: Linux build: lab compiles natively again; AppImage uses the host's libwayland and libgbm
area: deck
status: active
platforms: linux
default_windows: n/a
default_deck: on
origin: ours (runtime_memory.c); appimage.sh part spacepepperoni 22d6f92
class: G
commits: 8072f54
files: src/runtime_memory.c, packaging/linux/appimage.sh
symbols: host_stats_enabled, host_now_ns, HOST_STAT_ADD, decommit_keep_access, runtime_memory_set_precise_write_hook, gpu_written, GPU_WRITTEN_LIMIT, host_lib
switches:
tests: Native run 1
docs: plan-linux-deck.md#1-current-state-of-linux, plan-linux-deck.md#native-run-1-2026-11-03-appimage-of-8072f54-gow3_frame_stats1-log-datadeck-native-1log-partial, plan-linux-deck.md#branch-deck-lab-2026-11-03-the-user
depends_on:
related: upstream-linux-build, host-map-stats, D2, decommit-keep-access, static-gpu-lib
checked: 2026-11-07
---
## What it does

Makes `lab` build on Linux (it had never been built there, plan 1.5). `src/runtime_memory.c`: the Linux branch gets
stubs `host_stats_enabled()` = 0, `host_now_ns()` = 0 and a no-op `HOST_STAT_ADD`, so the shared code's host-map
counters compile away (`GOW3_HOST_MAP_STATS` stays Windows-only); the GPU-written page bitmap `gpu_written`
(`runtime_memory_set_precise_write_hook`) is an anonymous `MAP_NORESERVE` mmap instead of `VirtualAlloc`;
`decommit_keep_access` (Windows host views) is marked unused. `appimage.sh` `host_lib()`: `libgbm*` and
`libwayland-*` are the host's, never bundled.

## Invariants

- `host_lib()` must keep returning 0 (do not bundle) for `libwayland-*` and `libgbm*`: the host's Mesa Vulkan drivers
  need the host's newer libwayland-client (`wl_display_dispatch_queue_timeout`); a bundled older copy loaded first by
  SDL3 made every driver fail to load on SteamOS ("doesn't implement VK_KHR_surface").
- The bitmap is `GPU_WRITTEN_LIMIT >> 15` bytes (32 MiB for 1 TiB of guest space, one bit per 4 KiB page), zero on
  first touch; if the mmap fails `gpu_written` stays NULL and the precise-write hook is not installed (D2 off), never
  a crash. Guest addresses at or above `GPU_WRITTEN_LIMIT` are ignored by the marking code.
- Anything new in the Windows host-mapping block that the shared code calls needs a Linux stub here, or Linux stops
  compiling (the build was broken exactly this way).
- Binaries need glibc 2.38 (built in WSL Debian 13, clang 19); the AppImage needs 2.39+; AppRun's glibc check says so.

## Interactions

- `upstream-linux-build` is the base this repairs; `static-gpu-lib` later changed the same `appimage.sh` (shared
  library installed only when DT_NEEDED).
- `D2` (precise CPU writes) needs the bitmap: before this it was `VirtualAlloc` only and `lab` did not compile on
  Linux; `runtime_memory_precise_write` is a no-op until the hook is installed.
- `host-map-stats`: Linux reports nothing for it (stubs); the "Guest pool faults" line is the Linux counterpart.
- `decommit-keep-access` / `M1` have no Linux effect (Linux unmaps on decommit; see `copy-thread-backing-view`,
  `pool-kept-pages`).

## Evidence

- Native run 1 (AppImage of `8072f54`, Deck): mostly 60 FPS, worst 21-25 ms in the graveyard against Proton's 48-54
  FPS / worst 34-35 ms; "Native is clearly ahead of Proton". Windows build verified on the same commit (plan,
  "Branch deck-lab").
- Not verified in game beyond the native runs (no visual check recorded for this commit).

## History and pitfalls

- The libwayland fix is spacepepperoni/medievil_ps4_Linux `22d6f92` (GPL-2.0). Plan 1.4 had listed the over-bundling
  (FFmpeg's dependencies, libwayland and libgbm among them, ~150 MB with Samba's libraries) as a known Linux problem.
- GCC 14 fails on `src/actor_hook.h:26` (plan 1.1, from an upstream report; not re-tested); build with
  `CC=clang CXX=clang++`.
