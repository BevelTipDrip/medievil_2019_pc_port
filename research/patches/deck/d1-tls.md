---
id: D1-tls
title: Linux: initial-exec TLS model for libgow3gpu.so (Deck D1)
area: deck
status: active
platforms: linux
default_windows: n/a
default_deck: on
origin: spacepepperoni 7800f32
class: C
commits: 02c15bf
files: gpu/CMakeLists.txt
symbols: gow3gpu
switches:
tests:
docs: impl-deck-cpu-bundle.md#d1-static-tls-for-the-gpu-library, study-deck-rescore-rejected.md#d-linux-only-items-from-the-spacepepperoni-review-verdict-adopt-not-ported
depends_on:
related: static-gpu-lib, sampler-memo
---
## What it does

`target_compile_options(gow3gpu PRIVATE -ftls-model=initial-exec)` on non-Windows builds: the GPU library's
`thread_local`s (stage B's `DrawPipe::on_stage_b`, section state, user-data snapshots, the sampler memo,
`record_data_chunk`) are read at a fixed offset from `%fs` instead of a `__tls_get_addr` call per access. Not to be
confused with `D1` (unmap discard, caches).

## Invariants

- Class C, assumption: the library is in the initial set of loaded objects (DT_NEEDED of `medievil-runtime` and the
  tests), never `dlopen`ed. Its TLS is then sized into the initial static block and glibc's small dlopen surplus does
  not apply. A `dlopen` of it would fail loudly (the linker marks it `STATIC_TLS`).
- The library's TLS segment must fit the initial block (0x4b48 bytes measured: fine).
- With `static-gpu-lib` ON (default) the option is still passed and the static link makes it local-exec; removing
  it changes only an OFF build.

## Interactions

- `static-gpu-lib` makes this mostly moot: default builds have no `__tls_get_addr` call at all (5 -> 0).
- Hot TLS users that gain in an OFF build: `sampler-memo` (per-thread memo), the section flags, `on_stage_b`.

## Evidence

- `objdump -d out/gpu/libgow3gpu.so | grep -c __tls_get_addr`: 524 before (`f03ee52`), 5 after (`ff54428`); the 5
  are in the FFX library's context creation (cold). `readelf -d`: `FLAGS STATIC_TLS`.
- Their number (spacepepperoni commit message): `__tls_get_addr` "~3% of the draw recording thread, ~+1.5 FPS".
  Rescore D1 estimate: -0.3 to -0.45 ms of stage B [inferred]. No Deck A/B of D1 alone (it shipped with D2 and
  recorder lean mode, then static-gpu-lib replaced it). Not verified in game on its own.

## History and pitfalls

- Credit: Porticus (upstream PR #57, `92b4d51`); spacepepperoni's `7800f32` is that fork's reapply
  (impl-deck-cpu-bundle.md credit note).
- Never use initial-exec for a library that is `dlopen`ed (for example a future plugin split): it fails at load.
