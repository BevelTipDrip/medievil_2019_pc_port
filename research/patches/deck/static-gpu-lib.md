---
id: static-gpu-lib
title: Linux: the GPU library linked statically into medievil-runtime (GOW3_STATIC_GPU, default ON)
area: deck
status: active
platforms: linux
default_windows: n/a
default_deck: on
origin: ours
class: G
commits: 98dca62
files: gpu/CMakeLists.txt, build.sh, packaging/linux/appimage.sh, tests/test_gpu_runtime_stubs.cpp
symbols: GOW3_STATIC_GPU, GOW3GPU_KIND, gow3_runtime_executable, shared_gpu, runtime_disabled_optimizations2
switches:
tests:
docs: impl-deck-cpu-bundle.md#g-static-gpu-library-on-linux-gow3_static_gpu-98dca62, plan-linux-deck.md#static-vs-shared-gpu-library-2026-11-06-build-d2f145f-shared2-log-datadeck-shared-1log
depends_on:
related: D1-tls, S1-v3-build, thinlto, pgo-build, linux-build-lab
checked: 2026-11-07
---
## What it does

CMake option `GOW3_STATIC_GPU` (default ON; `build.sh` reads the env of the same name): on Linux `gow3gpu` is a
STATIC library (`GOW3GPU_KIND`) and CMake links `medievil-runtime`, `runtime-test` and `sema-test` itself
(`gow3_runtime_executable`), as Windows always did. The runtime C joins the library's ThinLTO unit, calls between
them and inside the library are direct (no PLT), thread_locals become local-exec, and `-fno-plt` sends the rest into
libc / libstdc++ through the GOT. `appimage.sh` installs `libgow3gpu.so` only when the runtime has it as DT_NEEDED
(`shared_gpu`). A build option, so it is not in `switches.tsv`.

## Invariants

- `medievil-runtime` must keep `-no-pie` (the brk heap stays below 1 TiB, `src/probe.c`) and `-rdynamic` (`dladdr`
  names in fault and thread dumps); `gow3_runtime_executable` sets both. Losing `-no-pie` breaks the guest layout.
- Runtime flags match `build.sh`'s shared-path `$CC` line: C11, asserts on (`-UNDEBUG`), `-Wall -Wextra -Werror`,
  plus `-fno-plt`. A flag added to one path must go into the other (`gow3_runtime_executable`).
- With ON, `build.sh` deletes a stale `out/gpu/libgow3gpu.so`; `appimage.sh` decides by `objdump -p` NEEDED, not by
  the file's presence, so an old shared library is never packaged beside a static runtime.
- Statically linked tests need every runtime symbol the library uses (`runtime_disabled_optimizations2` stub in
  `tests/test_gpu_runtime_stubs.cpp`); a new runtime global read by the library needs a stub there too.
- `GOW3_STATIC_GPU=OFF` keeps the old shared build (checked bit-identical to the build before, impl G). On Windows
  the option is ignored (`WIN32 OR GOW3_STATIC_GPU`: always static).

## Interactions

- `D1-tls`: its `-ftls-model=initial-exec` stays in `gpu/CMakeLists.txt` and the static link relaxes it to
  local-exec, so D1 only matters for an OFF build.
- `S1-v3-build` requires ON (`build.sh` stops otherwise).
- `thinlto`: part of the gain is LTO across the runtime / library boundary.

## Evidence

- Binary counts (impl G, WSL clang 19): PLT call/jmp sites 49,039 -> 4,055; interposable calls into the library's own
  exports 16,000 -> 0; `__tls_get_addr` 5 -> 0; AppImage 131,209,720 -> 131,152,376 bytes.
- Deck A/B at 1,400 MHz with the Deck preset's switches (plan "Static vs shared", `data/deck-shared-1.log` against the
  wake-ahead launch of the same commit): shared 74.7 FPS, stage B 76.8%; static 76.5 FPS, 75.1%. -1.8 FPS for shared
  is past the ~1.5 FPS between-launch noise; the user saw the shared build "more sensitive to camera movement induced
  drop". Static-view lows matched; the camera spot differed (HZB hidden 43k vs 35k), so the gap is approximate.

## History and pitfalls

- From study-deck-platform-optimization.md B1. Fault dumps now name frames `medievil-runtime+0x...` instead of
  `libgow3gpu.so+0x...`; the library's code sits at low addresses, as on Windows.
- `build.sh --test` was already broken before this change (`tests/test_pad.c`; the runtime-test link), per impl G.
