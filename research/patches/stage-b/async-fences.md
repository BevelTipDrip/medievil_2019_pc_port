---
id: async-fences
title: Async end-of-pipe fences: EOP label written once the copy threads finish (GOW3_ASYNC_FENCES)
area: stage-b
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: bloodborne a6fd94b
class: C
commits: a6fd94b
files: gpu/shadps4/video_core/amdgpu/liverpool.cpp, gpu/shim/gow3_copy.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp, gpu/shim/gow3_toggles.h
symbols: async_fences, AfterCopies, SignalAfterHostCopies, WaitDeferredSignals, AsyncFences
switches: GOW3_ASYNC_FENCES, toggle:AsyncFences
tests:
docs: study-deck-rescore-rejected.md#a-switches-that-already-exist-no-build, study-fence-waits.md#answer, impl-gpu-ordered-fences.md, impl-decode-pacing.md
depends_on:
conflicts_with: E1, W3
related: lineage-recorder-fences, pool-small-copies, U5-2, gate2-ordered-fences, early-frame-eop, decode-pacing
verified: 2026-11-07
---
## What it does

In the end-of-pipe task (`liverpool.cpp`, `TaskEop`), `GOW3_ASYNC_FENCES=1` (read once) hands the EOP label write to
`Gow3Copy::AfterCopies`: it is written by a copy thread once the copy-thread copies issued so far are done, instead
of through the recorder (`SignalAfterHostCopies`, the default) or after `WaitHostCopies` + `WaitDeferredSignals`.
Toggle word 1 bit 18 (`262144`) switches it off live. GPU-ordered fences (`GOW3_GPU_FENCES`) return before it.

## Invariants

- A guest-visible label must not be written before the host copies it covers, nor before an earlier deferred label
  (else the guest frees or rewrites memory still being copied: flicker, heap corruption; see `WaitDeferredSignals`).
- [inferred from code, not tested] Today's async path breaks both: `Gow3Copy::AfterCopies` knows only the copy-thread
  pool. Small copies replayed by the recorder (`RecordHostCopy`, the default since `9120756`, 16 minutes after
  `a6fd94b`) and labels deferred to the recorder (E1's end-of-shader fences, `SignalAfterHostCopies`) are not waited
  for. When it was written every small copy ran on the copy threads, so it was consistent then.
- Setting toggle bit 19 (pool-small-copies) restores the copy part only; the deferred-label order stays unguarded.

## Interactions

- lineage-recorder-fences: supersedes it for stage B. The recorder already defers the label, so stage B does not
  wait either way (rescore A11).
- E1, W3: both defer other labels to the recorder; the async EOP can overtake them (conflict, above).
- U5-2 / gate2-ordered-fences: GPU-ordered fences take the EOP path first; impl-gpu-ordered-fences.md: the async and
  recorder-fence switches do not apply to the fences that switch handles.
- early-frame-eop (Deck preset): counts frame EOPs in `RunEventWriteEop` before this branch; the frame EOPs it
  signals at decode never reach it; fallbacks and other EOPs do.
- decode-pacing (Deck preset): its retire / idle steps are registered later through `SignalAfterHostCopies`, whose
  `AfterCopies` epoch follows the async label's, so they still run after it (impl-decode-pacing.md item 1). That
  covers the step, not the gaps under Invariants.

## Evidence

- Bloodborne, not MediEvil: 79 vs 83 FPS in Hunter's Nightmare (`a6fd94b`): "the guest waits on these fences, and
  later fences stall it more than the wait costs". study-fence-waits.md: "**is not a MediEvil measurement**". The
  comment and switch came in with `a6fd94b` (`BB_ASYNC_FENCES`, in upstream's history); `a7cf186` only renamed them
  (the study says it "came in with the rename").
- Rescore A11 (Deck): ~0 for stage B; early frame EOP did not shorten the game's wait either (5.6 ms). No MediEvil
  launch ever set it. Not verified in game.

## History and pitfalls

- Was the lineage default (the toggle bit switched it off), made opt-in by `a6fd94b` after the slowdown.
- Do not set it as is: fix the two ordering gaps first (wait for `host_copies_done` and the deferred signals, or
  register the label in the recorder's stream as `SignalAfterHostCopies` does, which is what the default already is).
  With those fixed it equals the default, so there is nothing left to retry for stage B.
