---
id: force-aniso
title: 16x anisotropic filtering for every filtered, linearly mipmapped, non-depth sampler
area: caches
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 6fa68196f5
files: gpu/shadps4/video_core/texture_cache/sampler.cpp
symbols: Sampler::Sampler, force_aniso, trilinear_texture, anisotropy_enable, max_anisotropy
switches: GOW3_FORCE_ANISO
docs: fix-inventory.md#5-correctness-and-stability, plan-linux-deck.md#round-1-results-gpu-clock-auto-2026-11-05-ini-logging-67-uncapped-launched-by-the-user, study-deck-patch-audit.md#short-answer
related: sampler-memo, live-graphics-options, deck-preset
verified: 2026-11-07
---
## What it does

When a guest S# becomes a Vulkan sampler (`Sampler::Sampler`), a sampler that is not depth-compare, whose minify
filter is not Point and whose mip filter is Linear (`trilinear_texture`) gets `anisotropyEnable` with
`maxAnisotropy` 16. It is clamped to `instance.MaxSamplerAnisotropy()` and applies only if the device supports
anisotropy. Point-sampled textures (UI, lookup tables) and depth-compare samplers keep the game's own filtering.
UE4's `r.MaxAnisotropy` reaches only textures created after it changes, which is why it is done here.

## Invariants

- Only samplers that already filter linearly between mips change. Point / nearest-mip samplers must stay as the
  game set them: forcing anisotropy there would blur UI and lookup-table reads. A sampler left unforced whose S#
  asks for an aniso filter still gets the S#'s own `MaxAniso()` (the upstream path).
- Depth-compare samplers (`is_depth`, which also sets `compareEnable`) are excluded and keep the game's filtering
  (the code comment). If that test is widened, shadow-map sampling changes as well.
- The decision is made once per created sampler, and `GOW3_FORCE_ANISO` is a static read. It is restart-only, not
  live, so samplers cached in the texture cache and in the sampler memo never mix two modes.

## Interactions

- sampler-memo and the texture cache's sampler cache reuse handles keyed on the S#. `GetSampler` hashes the S#,
  `is_depth` and the extra LOD bias, and the forced value depends only on the S# and `is_depth`, so they work
  unchanged.
- Not one of the 13 live graphics options (live-graphics-options shipped in the same commit). plan-live-switches:
  "No setting: the user wants 16x always".
- Kept in the Deck preset (no key sets it to 0). The audit named it a Deck GPU cost suspect.

## Evidence

- PC: never measured (fix-inventory: "a quality choice of the user").
- Deck round 1 (deck-aniso-1, `GOW3_FORCE_ANISO=0`, auto GPU clock, 67% scale): FPS median 79.3. Launch 2 of
  the same round, a neutral main-thread switch, ran 79.4. The plan: "neutral (CPU-gated view); 16x stays (the user's choice)".
- Patch audit: it adds texture bandwidth on a GPU 85-95% busy at 0.8-1.2 GHz that shares 88 GB/s and ~15 W with
  the CPU. Its planned clean number (100% render scale, GPU-bound) is not written up in plan-linux-deck.

## History and pitfalls

- Changes the look, so judge it by screenshots, not only FPS.
- To re-test on the Deck, use a GPU-bound setting (100% scale or a fixed low GPU clock). In the CPU-gated view it
  cannot show a cost.
