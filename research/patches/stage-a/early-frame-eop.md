---
id: early-frame-eop
title: Signal the per-frame EOP label at decode, capped at 2 frames of lead (GOW3_EARLY_FRAME_EOP)
area: stage-a
status: active
platforms: all
default_windows: off
default_deck: on
origin: ours
class: M
commits: 8600b80
files: gpu/shadps4/video_core/amdgpu/liverpool.cpp, gpu/shim/gow3_fence_stats.h
symbols: EarlyFrameEop, EarlyFrameEopLead, IsFrameEop, frame_eops_decoded, frame_eops_recorded, RunEarlyEopTail, RunEventWriteEop, SignalEop, early_eops, early_fallbacks, early_max_lead
switches: GOW3_EARLY_FRAME_EOP, GOW3_EARLY_FRAME_EOP_LEAD
tests:
docs: plan-linux-deck.md#u2-ab-early-frame-eop-and-the-submission-gates-2026-11-04, impl-decode-pacing.md#3-with-gow3_early_frame_eop, study-eop-wait-site.md#answer, study-eop-wait-site.md#33-flush-then-rewind-intra-frame-reuse
depends_on: lineage-draw-pipe
related: decode-pacing, E1-deck-drains, U5, U5-2, deck-preset
checked: 2026-11-07
---
## What it does

UE4's submit task (guest 0x68acc0, wait site guest+0x68af47, equeue ident 64) waits each frame for frame N-1's
end-of-pipe label before `sceGnmSubmitDone` and frame N's submissions. Signalled at stage B, that wait is the game
waiting for our draw recording. With the switch, stage A's `EventWriteEop` case calls `SignalEop` (the label and
the interrupt) at decode for a frame EOP (`IsFrameEop`: an interrupt and 64-bit data). It does so while stage B is
fewer than `EarlyFrameEopLead()` (default 2) frame EOPs behind (`frame_eops_decoded - frame_eops_recorded`). It then
queues `RunEarlyEopTail` (stage B: `ProcessDownloadImages` and the recorded count) in its place. Past the lead, it
falls back to stage B as before (`early_fallbacks`).

## Invariants

- Only EOPs with `int_sel != None` and `DataSelect::Data64` go early. The flush-then-rewind paths' spin labels
  (guest 0x679190 / 0x68baf0) are no-IRQ EOPs with data 1, so they stay at stage B. Widening `IsFrameEop` would let
  the game rewind LCUE tables and command chunks stage B has not consumed yet.
- Each frame EOP examined at decode increments `frame_eops_decoded`. Each one reaching stage B increments
  `frame_eops_recorded`, in `RunEarlyEopTail` or in `RunEventWriteEop` (the latter only with the switch on). A
  missed decoded count understates the lead, so labels go early beyond the cap (unsafe). A missed recorded count
  overstates it, so every later frame EOP falls back (safe, no gain). The early path writes the label at once and
  never enters `pending_fences` (`NotePendingFences` runs only on the fallback path).
- Lead bound: `EarlyFrameEopLead` clamps only from below (at least 1). It has **no upper clamp**, unlike decode
  pacing's 1..2. A value above 2 can exceed UE4's frame-counted recycling (4-slot transient pool, 3-frame label free
  list; study 3.4).
- Off with `GOW3_GPU_FENCES=1` (checked at the decode site). Without a rasterizer, `SignalEop` runs directly as
  before.
- Assumption (M): the frame label only gates submission. Nothing is freed or rewritten inside the wait function
  after it (study answer 1), and the game reads no GPU-written result on the strength of it. Inferred: under the
  copy model stage B lags at most the lead (2 frames), and the transient pool reuses a slot 4 frames later, so
  stage B has read a slot before the game rewrites it.

## Interactions

- decode-pacing: with the idle drain gone, stage A decodes the frame's last submission sooner, so the early label
  moves forward. Its lead waits bound what the game does next. impl-decode-pacing.md section 3 covers the pair.
- E1-deck-drains: the OnSubmit and label drains also kept stage A from reaching the next frame EOP early.
- U5 / U5-2: under in-place reads (U5) early signalling is not safe for the transient pool (study answer 4). This
  card is safe only under the copy model. Ignored with `GOW3_GPU_FENCES`.
- deck-preset: `GOW3_EARLY_FRAME_EOP=1` (lead left at the default 2).

## Evidence

- Deck (2026-11-04, `8600b80`, `data/deck-early-eop-1.log`): every frame EOP signalled at decode, 0 fallbacks, **max
  lead 1 frame** (the stat is lead + 1: stage B had always reached the previous frame EOP, the cap never bound). FPS 63.7-64.4. Flip-release pacing stddev 1.3-1.5 ms (was about 2.2), p99 about 19 ms (was about
  21). **The game's ident-64 wait stayed about 5.6 ms a frame.** The user: still unstable, a small gain.
- The cause of the remaining wait (the idle drain and the console lock) led to decode-pacing.

## History and pitfalls

- The idea came from study-eop-wait-site.md ("New idea for the copy model"). The study's runtime checks (pool
  inventory, lag at slot reuse, rewind-path counts, frame EOP identity) are not written up as run. The frame-EOP
  identity check (only the two parity labels carry an IRQ) is the one this card's `IsFrameEop` relies on.
- The user's overlay (tearing allowed) still showed uneven frame starts and presents. The doc calls frame-time
  consistency a separate, unexplained problem.
