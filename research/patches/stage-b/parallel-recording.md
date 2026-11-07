---
id: parallel-recording
title: Parallel Vulkan recording (several recorder threads, Bloodborne 0.3-0.4; not built)
area: stage-b
status: rejected
platforms: all
default_windows: n/a
default_deck: n/a
origin: bloodborne 4d4d208
class: C
commits:
files: gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp
symbols: RecorderThread, recorder_thread, OwnsRecording, ChunkGuard
switches:
tests:
docs: plan-gpu-threading.md#candidates-ranked-claimed-gains-are-bloodbornes-own-numbers, study-deck-rescore-rejected.md#c-analysed-gated-or-ranked-never-built, study-deck-rescore-rejected.md#the-yardstick
depends_on:
related: lineage-draw-pipe, lineage-recorder-fences, recorder-lean, pool-small-copies, record-skip-identical, lineage-dropped-stage-b, C1-semaphore-poll
verified: 2026-11-07
---
## What it does

Not built. The port has one `gow3:VkRecorder` (`Scheduler::RecorderThread`, `recorder_thread`) that replays the
closures stage B records into command buffers, in order. Bloodborne 0.3-0.4 (`4d4d208`, `daa7165` in
`bloodborne_pc`) records with several threads, plus async submit, direct segments, the presenter on the recorders
and GpuSignals (plan-gpu-threading candidate #8).

## Invariants

- Everything ordered by "the recorder ran it" today would need another order: small host copies (`RecordHostCopy`,
  `host_copies_done`), deferred labels (`SignalAfterHostCopies`, recorder fences, E1), `WaitHostCopies`, the
  submission order of command buffers. Several recorders must keep the guest-visible label order (else freed memory
  is rewritten: heap corruption) and the per-command-buffer state the recorded closures assume.
- One owner may record (`OwnsRecording`); the chunk hand-off (`ChunkGuard` spin lock) assumes one consumer.
  Bloodborne's design differs ("never-null chunk") from this port's spin lock (`783b25a`).
- Assumption (class C): it pays only when `gow3:VkRecorder` or CPU / GPU serialization is the limit.

## Interactions

- lineage-draw-pipe, lineage-recorder-fences: built on one recorder; both would change.
- recorder-lean: the Deck's recorder now sleeps without spinning; the 63% busy below was measured before it
  (`0771a23`, lean mode `ff54428`).
- pool-small-copies, lineage-dropped-stage-b (its B1, batched copies), record-skip-identical: cheaper ways to cut
  recorder work (copies elsewhere, fewer commands) to try before more recorder threads.

## Evidence

- Bloodborne's own numbers: heavy scene 95.5 -> 115-124 FPS (plan-gpu-threading candidate #8). Not measured on
  MediEvil.
- plan-gpu-threading: "**Profile first** ... parallel recording only helps if `gow3:VkRecorder` or CPU/GPU
  serialization is the limit."
- Deck (plan-linux-deck.md, wait sites run, `deck-waitsites-1.log`, AppImage `0771a23`): `gow3:VkRecorder` 63%, stage B ~1 core, stage A
  ~1 core, the game's main thread ~1 core: no idle core. Rescore C13: **<= 0** on 4 cores / 8 threads. PC: stage B
  (`DrawRec`) is the thread at 93-97%, not the recorder. Not verified in game (nothing built).

## History and pitfalls

- Ranked #8 of 14 Bloodborne candidates in 2026-10-18 and never taken up; the cheaper wait and memory items were
  ported first.
- The recorder's catch-up waits inside stage B (the per-draw study's "tasks", x7.6 on the Deck) are an ordering cost
  (a timeline-semaphore query per end-of-pipe event, C1-semaphore-poll), not recorder throughput: more recorders would
  not remove them.
- Retry only on a machine where profiling shows the recorder saturated and spare cores, after the cheaper cuts.
