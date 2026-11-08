---
id: lineage-draw-pipe
title: Two-stage draw pipeline: stage A (PM4 decode) and stage B (draw recording), recorder thread
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream (inherited lineage: shadPS4 -> Bloodborne -> God of War III -> MediEvil)
class: G
files: gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h, gpu/shadps4/video_core/amdgpu/liverpool.cpp
symbols: DrawPipe, Drain, Commit, OnStageB, Backlog, DrawPipeWanted, UseDrawPipe, PostDraw, RunDrawPacket, DrainDrawPipe, OnStageA, RecorderThread, KickRecording, Regs
switches: GOW3_DRAW_PIPE, GOW3_PIPE_SPIN_US, GOW3_VK_RECORD_THREAD, toggle:DrawPipeline
docs: gpu-threading-reference.md#2-invariants-must-hold-after-every-change, START-HERE.md#knobs-and-triggers, study-deck-patch-audit.md#11-draw-pipeline-and-helper-threads-inherited-lineage-docsparallel_gpumd-measured-on-the-base-ports-game-not-medievil, docs/parallel_gpu.md
related: L5, drain-hybrid, E1-deck-drains, decode-pacing, lineage-recorder-fences, lineage-constant-ring, lineage-draw-preparation, upstream-record-data-fix, recorder-lean, stage-b-spin, cacheline-groups, parallel-recording
checked: 2026-11-07
---
## What it does

Stage A, the GPU command thread (`shadPS4:GpuCommandProcessor`, `shadPS4:GpuComm` in Linux tools), decodes PM4, keeps the registers and selects pipelines. For a
direct draw or dispatch, `Rasterizer::PostDraw` writes a packet into the 16 MiB `DrawPipe` ring: the register
blocks changed since the last packet, user data, constant-ring bindings and draw parameters. Stage B (`gow3:DrawRec`,
`RunDrawPacket`) applies the packet to its own register copy (`Regs()`) and does the rest: caches, barriers,
descriptors, recording. Anything else stage A does on that state first runs `DrainDrawPipe` (`DrawPipe::Drain`).
The Vulkan commands are replayed from 128 KiB chunks (`RecordChunk`, handed over every 32 KiB) by a third thread,
`gow3:VkRecorder` (`Scheduler::RecorderThread`). On by default with 8+ CPUs in the affinity mask (`DrawPipeWanted`).
`GOW3_DRAW_PIPE=0/1` (launcher "Two-stage GPU pipeline") forces it. Toggle word 1 bit 37 (`DrawPipeline`) turns it
off live. `GOW3_VK_RECORD_THREAD=0` records on the calling thread.

## Invariants

- While packets are in flight, stage B owns the texture and buffer caches, the runtime, the scheduler and the
  scene targets. Every other stage A action on them must drain first (`DrainDrawPipe`, `PipelinedOpcode`, and L5
  before queued commands). A missed drain is a race: wrong textures, crashes (#2 / #12 for recording).
- Packets run in stream order. Fences, WRITE_DATA and the flip run in order on stage B, so a `WaitRegMem` on a
  fence value handed to stage B counts as met (`pending_fences`).
- Guest-visible labels are written only after the work they cover, and GPU idle only after every label
  (gpu-threading-reference.md invariant 4). Breaking this corrupted the guest heap (fixed 2026-10-01: stage A
  drains and waits for deferred signals before GPU idle).
- Nothing outside the recorder records into the scheduler (invariant 8). `OnStageB()` is a thread-local flag:
  helper threads (copy threads, texture-helper) are not stage B, and the fence paths branch on it.
- With fewer than 8 CPUs in the mask the pipe is off by default, and the prep and copy thread counts change too.
  For SMT and affinity tests, set `GOW3_DRAW_PIPE=1` so the only variable is the CPU count.

## Interactions

- The drains are tuned by L5 (drain before every queued command), drain-hybrid (pause, then futex sleep;
  `GOW3_PIPE_SPIN_US` is stage B's idle spin, 200 us at 12+ threads, else 50), E1-deck-drains and decode-pacing
  (stage A's last drains off the frame path).
- Built on it: lineage-constant-ring (stage A copies, stage B binds), lineage-recorder-fences, recorder-lean,
  stage-b-spin, cacheline-groups. upstream-record-data-fix guards stage A recording against it.
- The toggle bits beside it (38-42, 46) move fences, WaitRegMem, dispatches, memory writes and indirect draws onto
  stage B.

## Evidence

- Inherited, measured on the base port's game, not MediEvil (docs/parallel_gpu.md): 96.1 vs 80.6 FPS on 16
  threads (+19%), 83.8 vs 71.0 on 4 cores / 8 threads (+18%). The second round, with the whole pipe: 110.9 vs
  83.9 (+32%).
- MediEvil: stage B is the limit in heavy views (T-P3, N-17: about 4.3 ms of a 4.8 ms heavy frame) and about 80-93%
  busy on the Deck (plan-linux-deck.md runs 8 and U5-2 A/B).

## History and pitfalls

- The launcher still offers "Off (more stable)". No current crash in the docs is tied to the pipe being on.
- `GOW3_VK_RECORD_THREAD=0` would put the recorder's work (63% of a CPU on the Deck) on stage B, the bottleneck.
  Keep the thread (study-deck-other-threads.md 3.1).
- Parallel recording (several recorders) was never built (parallel-recording).
