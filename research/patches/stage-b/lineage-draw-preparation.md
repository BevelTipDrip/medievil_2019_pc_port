---
id: lineage-draw-preparation
title: Draw preparation workers and scanner (pipelines, sharps, vertex inputs prepared ahead of stage B)
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream (inherited lineage, docs/parallel_gpu.md)
class: G
files: gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.h, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, launcher/medievil_launcher.py
symbols: DrawPreparation, DefaultWorkerCount, ScannerLoop, WorkerLoop, PrepareResources, BeginSubmission, EndSubmission, Enqueue, NextDraw, Collect, Count, PreparedDraw, TryPreparedPipeline, KeepCurrent, RetireSubmission, PREP_WORKERS
switches: toggle:DrawPreparation, GOW3_PREP_WORKERS
docs: study-deck-other-threads.md#32-gow3drawprep-4-19-each-17k-involuntary-switchess-each-code--log, study-deck-patch-audit.md#11-draw-pipeline-and-helper-threads-inherited-lineage-docsparallel_gpumd-measured-on-the-base-ports-game-not-medievil, study-deck-rescore-rejected.md#a-switches-that-already-exist-no-build, docs/parallel_gpu.md
related: prep-priority, prep-workers-2, lineage-texture-set-memo, lineage-constant-ring, lineage-draw-pipe, T2, A3, particle-curve-sync-compile, stage-b-core
checked: 2026-11-07
---
## What it does

The guest's submit thread `Enqueue`s each submission. One scanner (`gow3:DrawScan`, `ScannerLoop`) replays its
register writes and stores, per buffer, the start checksum and a register delta. Workers (`gow3:DrawPrepN`,
`WorkerLoop`) claim the nearest scanned buffer ahead of stage A, reach its state from the deltas and prepare each
direct draw (`PreparedDraw`): the pipeline key, each stage's image, sampler and buffer sharps with T# hashes
(`PrepareResources`), the vertex inputs and `reg_checksum`. Stage A takes the next one (`NextDraw`), and
`TryPreparedPipeline` uses it when it is `Ready`, the checksum equals `liverpool->gfx_reg_checksum`, and the
programs and flat user data match. The default worker count is half the CPUs in the mask, 1-8
(`DefaultWorkerCount`: 8 on a 16-thread PC, 4 on the Deck). `GOW3_PREP_WORKERS` (0-16, launcher "Draw
preparation helpers") overrides it. All helpers are idle-priority (`MakeBackground`; see prep-priority). Toggle
word 1 bit 13 (`8192`) stops stage A using prepared draws. Bit 12 (`4096`, `PreparedResources`) stops their sharps
being used.

## Invariants

- A prepared draw is only a hint. It is used only when the register checksum and the per-stage program hash, base
  and flat data equal the live state (`TryPreparedPipeline`). A new prepared field computed from anything else
  must be added to that check, or stage B binds stale resources.
- `TryPreparedPipeline` hands out only pipelines already built (`graphics_pipelines.find`). Missing ones fall
  through to `GetGraphicsPipeline`'s regular path.
- A submission's prepared draws stay alive until stage B has passed them: at the end of each submission stage A's
  `Rasterizer::RetireSubmission` keeps a reference (`KeepCurrent`, `pipe_keepalive`) until the pipe reaches that
  point. `EndSubmission` marks it decoded (`gpu_done`) and `Collect` then drops the queue's own reference. No drain
  at the end of a submission; dropping the keepalive would leave stage B reading freed `PreparedDraw`s (the
  texture-set memo's T# hashes).
- If the scanner falls more than 64 buffers behind, stage A rebases it from its own registers ("scanner rebases"
  in the stats) instead of queueing without bound.
- Bit 13 stops only consumption. The workers keep running (`NextDraw` checks it after claiming).

## Interactions

- lineage-texture-set-memo: keyed by the prepared T# hashes, so an unprepared draw misses the memo unless T2's
  wide memo is on. lineage-constant-ring uses the prepared V#s.
- A3 (register-packet checksum, two words per step) changes `reg_checksum` for stage A, the scanner and the workers
  together. All three must use the same hash.
- prep-priority (Windows priority) and prep-workers-2 (rejected, Deck count) tune it. stage-b-core moved the other
  threads off stage B's core.

## Evidence

- Inherited, base port's game (docs/parallel_gpu.md, toggle 8192 A/B in one run): about 74 vs 57.7 FPS on 16
  threads with 8 workers, and about 68 vs 53 on 4 cores / 8 threads with 4 (+28%). About 98% of direct draws
  prepared.
- Deck (study-deck-other-threads.md 3.2): 4 workers at about 19% each, 1.7k involuntary switches/s each, about 8 us
  of worker CPU per prepared draw. 462,257 prepared vs 28,239 not per 5 s (~94%). 97.8% on the Deck vs 99.6% on
  the PC (patch audit, E1).

## History and pitfalls

- The first design had 4 replaying workers, was off below 12 threads and gave none on 8. The scanner + delta design
  replaced it.
- `SCHED_IDLE` on the Deck: SMT siblings of stage B, preemption churn, and one mutex shared with stage A and the
  submit thread (lock-holder preemption, inferred). See prep-workers-2 for the count A/B.
