---
id: draw-classes
title: N0: recorded draws per frame by class; particle sim on the previous run's target
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits: 853877d873, 3123d44c6c
files: gpu/shim/gow3_draw_classes.h, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h, gpu/shadps4/core/libraries/videoout/driver.cpp, tests/test_draw_classes.cpp, research/tools/deck_summary.py
symbols: Gow3DrawClasses, Classify, Shape, Scope, Note, Report, sim_runs, sim_runs_after_inject, sim_runs_same_target, last_run_target, sim_shaders, Rasterizer::DrawClass, DrawRecord, DrawIndirectRecord
switches: GOW3_FRAME_STATS
tests:
docs: impl-draw-reduction.md#2-n0-draw-classes-gpushimgow3_draw_classesh, plan-linux-deck.md#particle-1-results-2026-11-05-1946-build-d20f010-test1105-log-datadeck-particle-1log, plan-linux-deck.md#particle-fix-1-2026-11-05-2110-2128-build-49291f3-particlefix-log-datadeck-particlefix-1log, study-particle-fixed-step.md
depends_on: frame-stats-lines
related: single-particle-pass, particle-flip-guard, particle-single-fixed, particle-identity-skip, finer-occlusion-culling, sections
verified: 2026-11-07
---
## What it does

With `GOW3_FRAME_STATS=1`, `DrawRecord` and `DrawIndirectRecord` open a `Gow3DrawClasses::Scope` (rdtsc) and,
after `PrepareRenderState`, `Rasterizer::DrawClass` fills a `Shape` from the bound targets (colour count, colour 0
RGBA32F / RGBA16F / RG16 / display buffer, depth, D16, depth writes). `Classify` maps it to one of 12 classes
(particle sim, particle inject, depth prepass, shadow depth, ..., post, display). The 5 s report (`Report`, after
"Thread ids per frame" in `driver.cpp`) prints draws per frame and stage B us per draw by class, particle sim
runs per frame, runs after an inject draw, runs on the previous run's colour target 0 (`3123d44`) and the first
four sim pixel shader hashes.

## Invariants

- Off costs one untaken branch: `Scope(false)` sets `start = 0` and `DrawClass` returns at once on `!scope.start`.
  A new early return in `DrawRecord` before `DrawClass` leaves `cls = Count`, which `~Scope` skips (no count).
- `last_class`, `inject_since_run` and `last_run_target` are plain globals: correct only while one thread records
  draws at a time (stage B, or stage A without the draw pipe). Parallel recording would need them per thread.
- `DrawClass` must stay after `PrepareRenderState` in both record paths: it reads the `cb_descs` / `db_desc`
  images and `depth_control` that call bound for this draw; before it, a draw is classed by the previous draw's targets.
- `scope.target` is the guest address of the first colour slot with a bound image (normally slot 0); a run counts
  as "same target" only when it equals the previous run's (`Note`). With the single particle pass it should read
  0.00 (it did with the flip guard); non-zero means a state flip went missing or doubled.
- Classes come from target shapes seen in PC 4K captures (class M): another game or preset can put a pass in a
  different class. Dispatches are not counted. Every counter is reset by `Report` (exchange); the four sim
  shader hashes and `last_run_target` are not, so the hashes are the first four seen in the launch [code].

## Interactions

- frame-stats-lines: the line lives in the `GOW3_FRAME_STATS` 5 s block; with stats off nothing is collected.
- single-particle-pass, particle-flip-guard, particle-single-fixed: "particle sim runs" (1.00 vs 1.98 a frame) and
  "on the previous run's target" were the measures in their Deck A/Bs.
- particle-identity-skip: the "after an inject draw" count is its gate (a reset every run leaves nothing to skip).
- finer-occlusion-culling: built in the same pass (impl-draw-reduction.md); N0 gave the class sizes it cuts.
- sections: the per-class us are inclusive of the whole record (stage B's task work inside it too), overlapping the
  `DrawRecord` section.

## Evidence

- Particle 1 (Deck, build `d20f010-test1105`): single pass 1.00 runs (193 sim draws) vs fixed step 1.98 (386),
  1,122 vs 1,316 draws, 79.4 vs 77.1 FPS. It showed the second run fires on nearly every frame at ~78 FPS (the
  "fires less near 60 FPS" guess was wrong).
- Particle fix 1 (build `49291f3-particlefix`): "sim runs 1.00 a frame, 0.00 on the previous run's target" with
  the guard. Diagnostic only; nothing to verify in game.
- `tests/test_draw_classes.cpp` (`draw-classes-test`, `gpu/CMakeLists.txt`) covers `Classify` and the run counters.

## History and pitfalls

- With the default fixed step the next frame's first run writes the previous frame's second run's pair: "on the
  previous run's target" reads about 1 a frame there by design, not a bug.
- Toggle bit 64 of the second word stays reserved for the particle-sim skip this was built to gate.
