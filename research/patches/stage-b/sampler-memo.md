---
id: sampler-memo
title: Sampler handle memo: per-thread direct-mapped memo reuses GetSampler handles within a GC period
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 5c752e887f, ffd5d90fbc
files: gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h, gpu/shim/gow3_toggles.h, gpu/shim/gow3_settings.h
symbols: BindSamplers, SamplerMemo, sampler_memo, GcTick, gc_tick, GetSampler, PerfLeanBinding
switches: toggle2:SamplerMemo, perf_lean_binding, GOW3_LEAN_BINDING
tests: N-31
docs: fix-inventory.md#2-draw-translation-cost-stage-b-per-draw, plan-frame-pacing.md#n-31-sampler-handle-memo-ab-inside-one-launch-2026-10-30-launched-by-claude-with-the-users-go-t-sampler-1log-build-5c752e8-lto-bulk-copy-on
depends_on: upstream-texture-gc
conflicts_with:
supersedes:
related: bulk-descriptor-copy, force-aniso, lineage-texture-set-memo, texture-helper
verified: 2026-11-07
---
## What it does

`Rasterizer::BindSamplers` called `TextureCache::GetSampler` for every sampler of every draw: an XXH3 hash of the S#
and the sampler mutex. A `thread_local std::array<SamplerMemo, 256>` (direct-mapped by a hash of the S#'s two words
and the border colour base) keeps the last handle per slot with its key (S#, `ta_bc_base` address, depth, LOD bias)
and the GC tick it was obtained in. A slot whose key matches and whose tick equals `texture_cache.GcTick()` returns
its handle; otherwise `GetSampler` runs (touching the sampler's LRU entry) and the slot is refilled.

## Invariants

- A handle is reused only within the GC period it was obtained or touched in (`memo.tick == gc_tick`).
  `GarbageCollectSamplers` (texture_cache.cpp) destroys only samplers whose LRU tick is below
  `gc_tick - ticks_to_destroy` (16 / 80 / 160, at most `gc_tick`) and defers the destruction past submitted work
  (`DeferOperation`). A GC that could destroy a sampler touched in the current period (`ticks_to_destroy` 0, or an
  eviction not based on the LRU tick) would let the memo bind a destroyed handle.
- The memo key must hold every input of `GetSampler`'s own key (S#, depth, extra LOD bias); it adds the border
  colour base, so it is stricter. A new sampler-creation input that can change while running (a live anisotropy or
  bias switch) must join the memo key or bump the tick.
- Per thread (`thread_local`): every thread that binds samplers (the texture helper too, through `BindTextures` ->
  `BindSamplers`) has its own memo; never share one without a lock.
- The memo is a function-level `static thread_local`, not a `Rasterizer` member: it outlives a `TextureCache`. A new
  texture cache on the same thread (a renderer restart in-process, none today) restarts `gc_tick` at 0 and could
  match old slots with destroyed handles; such a restart must clear the memo or never reuse the thread.
- Off when `PerfOn(PerfLeanBinding)` is false or second-word bit 2 is set, live: `GetSampler` every time.

## Interactions

- Shares `perf_lean_binding` / `GOW3_LEAN_BINDING=0` with bulk-descriptor-copy (`ffd5d90`); toggle bits separate.
- Relies on the sampler GC's age rule (upstream-texture-gc: GC limits, sampler destruction fixes).
- force-aniso and the scene mip bias shape the samplers: anisotropy is read once at start (`GOW3_FORCE_ANISO`); the
  scene LOD bias is part of the key (`lod_bias`), so a live bias change misses as it should.

## Evidence

- N-31 (PC, A/B in one launch, second word bit 2 every 20 s, heavy frames ~1,490 draws, 188 / 208 frames, bulk copy
  on): stage B samplers 0.041 vs 0.126 us a draw median (-0.085 us, ~-0.13 ms a heavy frame); draw section 1.86 vs
  1.97 us. No in-game confirmation recorded as a separate item.
- Deck: on / on in study-deck-patch-audit.md (row 29); no Deck A/B of its own.

## History and pitfalls

- The sampler half of study-frame-cost-2026-10-29.md item 3 ("sampler handles in the texture-set memo"), built as a
  memo of its own so dispatches and unmemoized sets gain too.
- `GetSampler`'s own hash leaves out the border colour base: two S# differing only in `ta_bc_base` share one
  sampler there. The memo does not change that (it is stricter), but its key is not the sampler's identity.
