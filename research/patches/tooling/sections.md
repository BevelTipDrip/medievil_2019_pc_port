---
id: sections
title: GOW3_SECTIONS: stage B time per packet by part (rdtsc scopes), Record calls, ordered tasks by function, texture-binding and ObtainBuffer splits
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: bloodborne 4d4d208
class: G
commits: c4bf2dc9c3, ba9c433974, 60d27d487a, e30bb42168
files: gpu/shim/gow3_sections.h, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h, gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp
symbols: Gow3Sections, GOW3_SECTION, Scope, Names, recording_thread, NoteTask, task_kinds, record_calls, PrintPipeStats, ObtUpload, Clamp
switches: GOW3_SECTIONS, GOW3_FRAME_STATS
tests: T-S1, T-S3, T-S1a, T-HZ4g, N-8, N-28, Native run 3
docs: fix-inventory.md#from-the-bloodborne-port-credited-in-the-commits, plan-stage-b-split.md#t-s1-stage-bs-time-per-packet-2026-10-19-2333-2341-launched-by-claude-with-the-users-go-build-c4bf2dc-gow3_sections1-log-datat-s1-sectionslog, plan-stage-b-split.md#t-s3-record-calls-and-ordered-tasks-2026-10-1920-2357-0002-launched-by-claude-with-the-users-go-build-ba9c433-log-datat-s3-record-taskslog, plan-linux-deck.md#native-run-3-stage-b-by-part-2026-11-03-appimage-of-56e323b-gow3_frame_stats1-gow3_sections1-gow3_slow_frames14-log-datadeck-native-3log
depends_on:
related: slow-frames, stage-b-counters, pool-small-copies, E1, texset-region-stamps, stream-class-table, W3, ring-reuse
verified: 2026-11-07
---
## What it does

`GOW3_SECTION(id)` opens an rdtsc scope (`Gow3Sections::Scope`) that adds its cycles to `cycles[id]`, but only on
the draw recording thread (`recording_thread`, set in `DrawPipe`'s run loop). Parts are inclusive: a nested scope
counts in its parent too. There are 47 parts: packet, tasks, draw, bind, textures (`texset`, `texset emit`,
`samplers`, `texslow`, `60d27d4`), ObtainBuffer and its `obt_*` parts (`de30a39`, stream-class-table's commit),
and `obt_upload` and `clamp` (`e30bb42`). Parts added by other cards' commits: `flip_signals` / `flip_present`
(`31a6c1a`, ring-reuse), `rs_target` / `find_image` / `cache_lock` (`ae09fcc`) and the `task_*` kinds (`320af9b`),
both stage-b-counters. `ba9c433` adds
`Scheduler::Record` calls per packet (`record_calls`) and ordered tasks timed by function address (`NoteTask`, 24
slots, printed as `exe+offset`). `Rasterizer::PrintPipeStats` prints "stage B us per packet" with the draw pipe
line every 5 s. slow-frames prints the same parts per frame.

## Invariants

- `Names` must list exactly one name per `Id` before `Count`. `static_assert(Names[Count - 1] != nullptr)` catches a
  missing name; a new id with its name in the wrong place prints under the wrong label.
- Scopes count only when `Enabled()` and `recording_thread` are both true. Time spent on stage A, copy threads or
  the recorder thread is invisible, by design. Without the draw pipe nothing is counted.
- When off, a scope costs one static-bool check and stores 0. A scope is a local object named after `__LINE__`: it
  covers the rest of its C++ block, and two `GOW3_SECTION` on one line do not compile.
- The report needs `GOW3_FRAME_STATS` set (`PrintPipeStats`: `getenv != nullptr`, so any value, `0` included). It
  runs on stage A (after `draw_pipe->Commit`) every 1,024 packets once 5 s have passed, as synchronous `printf`.
- `NoteTask` and `record_calls` count only on the recording thread with sections on (`RunDrawPacket`,
  `Scheduler::Record`).
- Nested scopes double count by design: the parts do not sum to `packet`.
- `task_kinds` keeps the first 24 distinct functions; more are dropped silently.

## Interactions

- slow-frames adds the per-frame "stage B ms:" line when both switches are on.
- stage-b-counters (N-28) and the Deck per-part ratios (native run 3) are read from these lines.
- The T-S1 / T-S3 readings drove pool-small-copies (T-S2, rejected), E1 and W3. T-S1a's texture split led to
  texset-region-stamps. N-8's `obt_regions` split found the stream-class-table cost.

## Evidence

- T-S1 (heavy view): 2.3-2.8 us a packet. Ordered tasks were ~35%, of which the end of pipe ~22%. The draw was
  ~60%, with no single part dominating.
- T-S3: 7.5 Record calls a packet, 0.094 us in all. `RunEventWriteEop` 0.44-0.45 us a packet (~52,000/s).
- Deck native run 3 against the PC: the draw 4-5x slower, tasks 7.6x, flip_present 6.5x.
- N-8 (`t-pan2`): `obt_regions` took 8-12 ms in panning hitches.

## History and pitfalls

- Ported from the Bloodborne port's `BB_SECTIONS` (`bbport_sections.h`).
- The scopes change timing (an rdtsc pair and an atomic add per part of every draw): N-8's rule says no smoothness
  judgement with `GOW3_SECTIONS` on.
- plan-linux-deck.md says native run 2 (`GOW3_SECTIONS=1` without `GOW3_SLOW_FRAMES`) "did not take effect" and
  logged no breakdown. Only the per-frame "stage B ms" lines were missing: `data/deck-native-2.log` holds 97
  five-second "stage B us per packet" lines (e.g. packet 15.2, tasks 10.0 us). The per-part 5 s averages need only
  `GOW3_FRAME_STATS` + `GOW3_SECTIONS`.
- Not verified in game: diagnostics only.
