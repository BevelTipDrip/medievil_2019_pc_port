---
id: frame-cost-dropped-ideas
title: Dropped on analysis (study-frame-cost section 4): mid-frame split submits, idle flush, recorder spin cut, shadow-map rename, shadow flush barrier
area: stage-b
status: rejected
platforms: all
default_windows: n/a
default_deck: n/a
origin: ours
class: G
commits:
files:
symbols:
switches:
tests:
docs: study-frame-cost-2026-10-29.md#4-dropped-ideas-do-not-re-propose, study-deck-rescore-rejected.md#c-analysed-gated-or-ranked-never-built, study-deck-rescore-rejected.md#a-switches-that-already-exist-no-build, fix-inventory.md#10-tried-and-dropped
depends_on:
related: frames-queued-2, recorder-lean, compute-drains-pipelined, obtainbuffer-memo, narrow-image-stages, effects-resolution, ring-reuse
verified: 2026-11-07
---
## What it does

Records the 2026-10-29 study's "do not re-propose" list for stage B, submission and barriers. None was built or
launched; each was dropped on a trace replay, a capture or a code reading:
- **Mid-frame split submits (2-4 parts)** and a **Bloodborne-style idle flush (+SubmitAsync)**;
- **Recorder 200 us spin cut**;
- **Shadow-map backing rename** (N-6 premise);
- **"Drop the global memory barrier" from shadow flushes**; narrower barrier masks as a GPU win;
- copy prologue / epilogue batching, swapchain acquire off the graphics queue.

## Invariants

- No code exists. Before re-proposing one, the premise below must have changed; check it first.
- Shadow maps: 26-34 separate maps a frame, each written once (no shared backing to rename).
- The shadow-flush global barrier ORs into the next flush: removing it changes nothing.

## Interactions

- frames-queued-2: mid-frame submits had the same effect as frames queued 2, which hurt the lows three times.
- recorder-lean: on <= 8 threads (the Deck) the recorder no longer spins at all; the spin question is settled there
  by that patch, not by this list.
- compute-drains-pipelined, obtainbuffer-memo: also on the section 4 list ("stage-B drain bubbles", "ObtainBuffer
  memo"); they have their own cards because the Deck rescore reopened them.
- narrow-image-stages: ships (on, no visual problems); the study only dropped the claim that it is a large GPU win
  (study correction of N-5: ~9% was idle variance, -0.8% GPU active).
- effects-resolution and ring-reuse were on this list (separate translucency 50%: "colour-ROP pixels are flat"; ring
  reuse: "off, mechanism unknown") and were later reversed: both ship now. The list is a snapshot, not a law.

## Evidence

- Mid-frame submits: "the trace replay gives +1.2% mean and leaves the lows unchanged". Rescore C12 (Deck): 0 to +3%
  (GPU overlap, not a stage B saving), low priority.
- Idle flush: "≤+1% free, about 0 at 100 µs per flush, net negative in stage-B-bound stretches".
- Recorder spin cut: "gain below noise. Blocked-on-recorder is 0.0–0.1%, and spin 0 likely costs 0.1–0.9 ms".
  Rescore A13: 0 uncapped on the Deck (spins already drop below 12 threads).
- Narrower masks: -0.8% GPU active, not 9%. Copy batching ~0.5-1%. Not verified in game (none ran).

## History and pitfalls

- The study's own rule: ranking by usage %, stage-B busy %, drain count, WFI share or GR-active alone "all have misled
  before".
- Two entries were reversed later by measurement (effects resolution, ring reuse): re-test an item only with a new
  measurement that contradicts its stated premise, one change per launch.
