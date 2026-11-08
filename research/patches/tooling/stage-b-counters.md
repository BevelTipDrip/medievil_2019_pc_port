---
id: stage-b-counters
title: Stage B counters: texture sets not kept and why, descriptor writes, FindImage time, sleep ms by site, ordered tasks by kind
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: ae09fcc08b, 513ca7f1f4, 320af9b534
files: gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_common.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shim/gow3_sections.h, gpu/shim/gow3_toggles.h, gpu/shadps4/video_core/amdgpu/liverpool.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_gpu_signals.cpp, research/tools/sections_frames.py
symbols: texture_set_unkept, image_generation_misses, descriptor_pushes, descriptor_writes, RsTarget, FindImage, CacheLock, TaskEop, TaskEos, TaskWriteData, WriteDataWait, TaskDma, TaskFlip, TaskSubmit, SubmitFlush, TaskRetire, TaskSignals, TaskOcclusion, sleep_ns, site_ns, BeginRendering
switches: GOW3_FRAME_STATS, GOW3_SECTIONS
tests: N-28
docs: plan-frame-pacing.md#n-28-stage-b-counters-the-studys-step-0-2026-10-30-launched-by-claude-with-the-users-go-t-sb0-counterslog-build-ae09fcc-gow3_sections1, impl-texset-index.md#1-instrumentation, plan-linux-deck.md#spin-compact-texture-memo-and-the-task-split-2026-11-04, study-deck-rescore-rejected.md
depends_on: sections, frame-stats-lines
related: own-target-texsets, findimage-generation-misses, bulk-descriptor-copy, sampler-memo, W3, W3b, stage-b-spin, lineage-texture-set-memo, crash-dump
verified: 2026-11-07
---
## What it does

Counters for study 2026-10-29 Step 0 (`ae09fcc`) and the Deck's unnamed task time (`320af9b`). Frame stats:
why `BindTextures`' slow path did not keep a set (`texture_set_unkept`: storage, written, own colour / depth
target, rebind, incomplete, registry changed; not memoizable), descriptor pushes and writes per push ("Binds per
frame"), FindImage misses only on the registry generation (`image_generation_misses`, slow-frame lines), and
"Stage B sleeps per frame" with each site's ms (packet ring, recorder, GPU waits, present). Sections: `rs_target`,
`find_image`, `cache_lock`, and one section per ordered-task kind (`task_eop` ... `task_occl`, with `wdata_wait`
and `submit_flush` nested). `513ca7f` holds the N-28 logs and `sections_frames.py`'s per-frame split (its `probe.c` part is crash-dump's).

## Invariants

- Counting only: every new counter is behind `Gow3Stats::enabled` or a `GOW3_SECTION` scope, except the
  `texture_set_unkept` increments (a plain array add on the slow path, printed only with stats).
- The "texture sets not kept" line prints at the top of `BeginRendering` (before the memo check) when
  `(hits + misses) & 0x3FFFF == 0x3FFFF`, i.e. every 262,144 render-state calls, not every 5 s, and then resets the
  array: divide by the frames in that span, not by a 5 s window.
- Each `Gow3Sections::Id` needs a name in `Names` (the `static_assert` on the last name catches a missing one).
  Task sections are inclusive and nest (`task_flip` contains `flip_signals` / `flip_present`); "end of pipe" runs
  inside EOP, EOS and submit tasks. Summing nested sections double counts.
- The generation-only test compares the whole `FindImageCacheEntry` key and the resolved view mip / slice; a key
  field added to the entry must be added to `same_key` too, or the count overstates.

## Interactions

- sections, frame-stats-lines: the lines these extend; off without `GOW3_SECTIONS` / `GOW3_FRAME_STATS`.
- own-target-texsets, findimage-generation-misses: N-28's gates (passed, then parked for the Deck; dropped).
- bulk-descriptor-copy, sampler-memo: N-28's "descriptors" and "samplers" numbers gave them their go.
- W3, W3b: the Deck task split named WRITE_DATA's wait (`wdata_wait`) as the next design item.
- stage-b-spin: shares the "Stage B sleeps per frame" line (spin state, spins); the per-site ms read the same
  `Gow3StageBWait::sleep_ns` counters.
- crash-dump: owns `513ca7f`'s `probe.c` change (winpthreads faults reported with a dump).

## Evidence

- N-28 (PC, `t-sb0-counters.log`, build `ae09fcc`): ~900 pushes a frame at 14.7-15.2 writes each; samplers
  0.12 ms a frame; own colour target 7,430 / own depth 6,541 against 14,076 "other key" misses; generation-only
  misses 3 per ordinary heavy frame (gate >= 50: dropped); render-state spikes >= 0.5 ms in 7 of 886 heavy frames,
  all `rs_target` -> FindImage, cache lock 0.00-0.06 ms ("no lock problem").
- Deck task split (heavy frames, 13.6 ms, 1,213 draws): tasks 3.09 ms = flip 1.30, WRITE_DATA 0.85 "all waiting",
  EOP 0.50, submit 0.23 (flush 0.13), EOS 0.09. Diagnostic; nothing to verify in game.

## History and pitfalls

- The per-draw study first divided the "not kept" counts by 5 s and read ~5 own-target sets a frame on the Deck;
  the rescore corrected it to ~9-11 (the 262,144-call span, ~2.3 s there).
- Sections change timing: judge smoothness only from runs without them (fix-inventory).
