---
id: decode-pacing
title: Decode pacing under the copy model: GPU idle in stream order, frame lead cap (GOW3_DECODE_PACING)
area: stage-a
status: active
platforms: all
default_windows: off
default_deck: on
origin: bloodborne 0fc2921
class: C
commits: fb62432
files: gpu/shim/gow3_decode_pacing.cpp, gpu/shim/gow3_decode_pacing.h, gpu/shadps4/video_core/amdgpu/liverpool.cpp, gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp
symbols: Enabled, LeadOption, SubmitAheadOption, BeginFrame, AfterSubmit, WaitForLead, NoteFramesRetired, SignalDecodedFrames, DeferGpuIdle, SignalGpuIdle, submit_counter
switches: GOW3_DECODE_PACING, GOW3_DECODE_PACING_LEAD, GOW3_SUBMIT_AHEAD
tests:
docs: impl-decode-pacing.md#what-it-is-for, impl-decode-pacing.md#heap-safety-argument, impl-decode-pacing.md#deck-results-2026-11-04-build-fb62432-logs-datadeck-decode-d0log-datadeck-decode-d1log-d2-on-the-deck, plan-linux-deck.md#u2-ab-early-frame-eop-and-the-submission-gates-2026-11-04, plan-linux-deck.md#decode-pacing-d0-d2-2026-11-04-appimage-fb62432-67-auto-clocks-helpers-auto-no-dedicated-core-logs-datadeck-decode-d012log
depends_on: lineage-draw-pipe, lineage-recorder-fences
conflicts_with: U5-2
related: early-frame-eop, E1-deck-drains, deck-preset, L5
verified: 2026-11-07
---
## What it does

Two gates made the game wait for stage B twice a frame. First, at the end of queued work, stage A drained the pipe
and `WaitDeferredSignals` before raising GPU idle. Second, `sceGnmSubmitDone` armed the console lock (the next
submission waits for GPU idle). With the switch, `Liverpool::Process` calls `SignalDecodedFrames` + `DeferGpuIdle`
instead of draining. Both hand a step to stage B (`RunInOrder`), which defers it with `SignalAfterHostCopies`.
`sceGnmSubmitDone` arms no lock: `BeginFrame` waits until frames ended minus retired is below the lead.
`AfterSubmit` lets a frame's first `GOW3_SUBMIT_AHEAD` (2) submissions through; later ones and the flip submission
always go through the lead check (`WaitForLead`, which returns at once when the lead is not reached). This is Bloodborne 0.7's `decode` pacing (as ported for `GOW3_GPU_FENCES` in `0771a23`), where "retired"
means stage B and its label writes passed the frame end, not the GPU.

## Invariants

- "Retired" / "idle" must mean that every label of earlier work is written. The steps run through
  `SignalAfterHostCopies`, which runs callbacks in registration order after the host copies. A new label path that
  writes after its stream position outside that order would let the guest free label objects under a late write.
  That is the corrupted guest heap free list seen once when GPU idle fired before stage B finished.
- `SignalGpuIdle(generation)` raises only if `num_submits == 0` and `submit_counter` still equals the generation
  `DeferGpuIdle` read. `Submit*` increments `submit_counter` before `++num_submits`, so later undecoded work is never
  marked retired.
- `LeadOption` clamps `GOW3_DECODE_PACING_LEAD` to 1..2 (default 1; the Deck preset sets 2). Above 2, the guest's 3-frame label free list is not covered.
  At lead 2 (the Deck preset) there is zero margin for render-thread pool data and labels. A frame that ends without
  a flip submission and with K or fewer submissions is one frame short (heap-safety table).
- `AfterSubmit(true)` (flip) waits even among the first K. That wait bounds UE4's EndFrame recycling.
- `SignalDecodedFrames` reads `frame_ends` before the queues (`OldestUndecoded`). Only stage A removes ends.
- `Enabled()` is false with `GOW3_GPU_FENCES=1` (logged). Read once at start. Submission numbering
  (`submit_index`, `submit_counter`) and `frame_ends` exist only with one of the two switches (`track_frames`).
- Without a rasterizer the retire happens at once and GPU idle is raised as before; when the pipe is not
  in use (no pipe, host markers, the `DrawPipeline` or `PipelinedTasks` toggle off) `RunInOrder` drains and runs the
  steps inline on stage A (untested, see below).
- Assumption (C): UE4's PS4 RHI recycles by frame count (study-eop-wait-site.md: 4-slot transient pool, 3-frame
  label list). With K > 0, nothing may be freed on "my submission returned, so all before is done". The study found
  no such path. `GOW3_SUBMIT_AHEAD=0` with lead 1 is exactly the console lock's guarantee.

## Interactions

- early-frame-eop: the intended pair. The pacing removed the idle drain, so the frame EOP is decoded sooner.
- E1-deck-drains: `GOW3_ONSUBMIT_STAGE_B` defaults to this switch. It removed the OnSubmit drain, where D1/D2 moved
  the wait.
- U5-2 (`GOW3_GPU_FENCES`): mutually exclusive (this switch is ignored with it). It shares `GOW3_SUBMIT_AHEAD`.
- lineage-recorder-fences, W3 / W3b, async-fences: label paths whose order the safety argument lists (item 1).
- L5: commands still drain per command. Only the idle drain is gone.
- deck-preset: `GOW3_DECODE_PACING=1`, `GOW3_DECODE_PACING_LEAD=2`.

## Evidence

- Deck D0-D2 (2026-11-04, `fb62432`, Graveyard heavy view, 67%, early frame EOP in every run): FPS 64-65.5 / 63.5-64.8
  / 64-65. Submission lock 83 ms/s goes to 0. Pacing waits 78 ms/s at lead 1 and 0 at lead 2. Idle drains avoided
  571 per 5 s, all idle steps skipped by the generation check. Max frame lead 1. No visual issues (the user).
- FPS unchanged until E1-deck-drains: the wait moved to `OnSubmit:1592` (1.5% of stage A goes to 12.2% in D2).

## History and pitfalls

- D0/D1/D2 here are settings, not the Deck patches D1-tls / D2-gettid.
- The impl doc calls lead 2 "the experiment, not a default". The preset ships it. The 20-30 minute soak the doc
  prescribes (guest faults, watchdog, "Guest memory" flat) is not written up, and the lead never passed 1 in D2 or
  E1. If a guest allocator fault shows up minutes in, try `GOW3_SUBMIT_AHEAD=0` first, then lead 1.
- Untested: `GOW3_VK_RECORD_THREAD=0`, the `PipelinedTasks` toggle off, frames without a flip submission.
- The watchdog (`Decode pacing watchdog:` after 10 s with no retire) only logs. It never breaks the wait.
