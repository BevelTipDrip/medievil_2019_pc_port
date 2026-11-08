---
id: particle-curve-sync-compile
title: Compile UE4 particle curve injection synchronously (torch flames on a cold cache, fork port #52)
area: stage-b
status: active
platforms: all
default_windows: n/a
default_deck: n/a
origin: upstream PR #52 (Porticus 40d105e)
class: M
commits: f130b8de78
files: gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp
symbols: GetGraphicsPipeline, GetProgram, AsyncCompilation, TryPreparedPipeline
docs: study-forks-survey.md#9-worth-porting-shortlist, study-forks-survey.md#unverified--open
related: lineage-draw-preparation, particle-bounds, particle-flip-guard, single-particle-pass, publish-graphics-empty
checked: 2026-11-07
---
## What it does

This exists only on branch **`fork-ports`** (`f130b8d`, local, not in `lab` or `deck-lab`). Its write-up is
`research/docs/impl-fork-ports.md` section 2, which is on that branch only. `MustCompileSync(pgm_hash)` is true
for UE4's particle curve injection programs (vs `0x113f0c20873a7659`, ps `0x1b1e7c76d6a281d3`). For them,
`GetProgram` compiles on the spot instead of queueing, and `GetGraphicsPipeline` skips the async path for any
selection that uses one. Everything else stays async. `GOW3_CURVE_SYNC=0` (on that branch) puts both back on the
async path. Each run prints `gow3: curve injection program 0x... ready synchronously (N ms)` once per program
compiled that way.

## Invariants

- The injection draws run once at level load into `FParticleCurveTexture` (512x512), and nothing redraws it. A
  draw skipped while its pipeline compiles (`GetGraphicsPipeline` returns null) is lost for good. Any program of
  that "drawn once" kind needs the same exception: the list is closed.
- The hashes come from the shader code in the paks and match the user's CUSA11227 cache
  (`0x113f0c20873a7659_0.spv`, `0x1b1e7c76d6a281d3_0.spv`). A game build with other shaders silently loses the
  fix.
- The prepared-draw path (`TryPreparedPipeline`) hands out only pipelines already built. A missing curve pipeline
  falls through to the regular path, where the exception sits. Both paths are covered only while that stays true.
- It acts only while `AsyncCompilation()` is true (the in-game / launcher async shader setting on, compile workers
  present) and the draw is not tessellated or under shader collection: those cases already compile synchronously.
- The sync path is `async_shaders=0`'s path: it waits for a pending program. The warm-up keeps the first pipeline
  published for a key (`PublishFirst`), so a pipeline built here wins over a warm-up copy.

## Interactions

- Defaults: n/a in the front matter because `lab` and `deck-lab` builds (the user's Windows and Deck builds) do
  not contain it; in a `fork-ports` build it is on (`GOW3_CURVE_SYNC` unset or anything but `0`). Whether the bug shows
  there on a cold cache is unverified (survey, "Unverified / open"). Cherry-pick onto `deck-lab`: the file differs
  from `fork-ports` only by publish-graphics-empty's early return in `PublishGraphics` (git diff, 2026-11-07), so
  it should apply cleanly. Not tried.
- particle-bounds, particle-flip-guard and single-particle-pass are the other GPU-particle fixes. Flames missing
  on a cold cache is a separate cause from the single pass's frozen torch.

## Evidence

- Porticus, Linux RADV with the pipeline cache off: 80 of 84 injection draws skipped. They showed the crypt torch
  without a flame (a dark smudge) before and with a flame after, in PR screenshots. Not tested on Windows.
- Ours: built on Windows, **not run in game, not verified** (impl-fork-ports.md status, 2026-11-05). Test plan R2:
  cold cache (rename `user\cache`, never the rest of `user\`), crypt start, `GOW3_CURVE_SYNC=0` then the default.

## History and pitfalls

- spacepepperoni took it (`c758207`) and reverted it (`7f69812`, 2026-10-28) with no reason given. The inference
  is that it was one of an evening's reverts while they hunted stale GPU data, whose cause was their buffer path.
  The six runtime commits reverted with it were reapplied; this one was not.
- Risk: a level-load hitch of a few to tens of ms on a cold cache only. On the Deck every new build starts cold
  (inferred), so the Deck is where it matters.
- `GOW3_CURVE_SYNC` is not in `_facts/switches.tsv` (facts are taken from `deck-lab`), so it is named here and
  not in the switches field.
