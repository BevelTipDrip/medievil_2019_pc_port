---
id: compute-drains-pipelined
title: Compute-queue drains pipelined (spacepepperoni d82d86c); stage B drain bubbles (not ported)
area: stage-a
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: spacepepperoni d82d86c
class: G
commits:
files: gpu/shadps4/video_core/amdgpu/liverpool.cpp
symbols: ProcessCompute, ReasonCompute, DrainDrawPipe
switches:
tests:
docs: study-deck-rescore-rejected.md#c-analysed-gated-or-ranked-never-built, study-fork-spacepepperoni.md#13-gpu-thread-synchronisation-memory-shadps4-video-core, study-frame-cost-2026-10-29.md#4-dropped-ideas-do-not-re-propose, impl-stage-a-decode.md#2-why-stage-b-waits-1-ms-a-frame-on-stage-a, plan-linux-deck.md#e1-and-the-fix-hit-rates-2026-11-04
depends_on: lineage-draw-pipe
related: E1-deck-drains, decode-pacing, drain-hybrid, L5, A3
checked: 2026-11-07
---
## What it does

Not built. Today `Liverpool::ProcessCompute` calls `DrainDrawPipe(ReasonCompute)` before **every** packet of an async
compute queue ("compute work uses the caches"), and its WRITE_DATA / RELEASE_MEM then wait inline for the host
copies and deferred signals (`WaitHostCopiesFor` / `WaitHostCopies`, `WaitDeferredSignals`). Its WAIT_REG_MEM loop
drains again only under `GOW3_GPU_FENCES`. spacepepperoni
`d82d86c` (from Uncharted `3f7be65`, `824a5c0`) drains only before packets that need stage B idle. Nop, SetShReg,
SetQueueReg, AcquireMem, dispatches, DmaData and IndirectBuffer do not drain. A compute RELEASE_MEM memory fence goes
to stage B in order (`RunInOrder`), written after the host copies, instead of a drain and an inline wait.

## Invariants

What a port would have to keep:
- Compute dispatches are posted through `PostDraw(..., &CsRegs())`. Without a drain before each SetShReg, the packet
  must snapshot the CS registers when the dispatch is posted. Check that it copies them before porting (the study's
  condition).
- WRITE_DATA, RELEASE_MEM, WAIT_REG_MEM and DMA still need stage B idle and the deferred signals written. Any one left
  undrained must go in order through stage B, as the end-of-pipe paths do.
- Ordering against the graphics queue: compute batches interleave with graphics submissions at points the
  preparation workers cannot reproduce (the SetShReg checksum mix in `ProcessCompute`, through A3's
  `HashRegisterPacket`; like A3's live flip it would desync the scanner's checksum chain for good, and the ~90%
  prepared share in the Deck logs suggests MediEvil never hits it [inferred]). A fence the game waits on must not
  overtake the work before it.

## Interactions

- E1-deck-drains / decode-pacing: once those removed the other drains, the compute drain is stage A's last gate.
  E1: stage A waits 30%, "all on the compute drain".
- drain-hybrid: makes each compute drain cheaper in CPU, not shorter.
- L5: `ProcessCommands` still runs per compute packet. Its per-command drain is separate.

## Evidence

- PC (study-frame-cost 2026-10-29, dropped): "Stage-B drain bubbles (0.45 ms): moving the work doesn't remove it.
  Refill latency is µs."
- Deck, deck-perdraw-1: stage A drains 2.75 times a frame (2.98 ms), every one a compute drain (`ProcessCompute:2023`).
  Hybrid wake latency is 51 us on average. Pre-E1 log: 151 compute drains a second waited (about 2.3 a frame).
- spacepepperoni's message ties this and `a08d0b0` to the GPU thread's host-copy blocking on the Deck (22-24%; the
  fork study's summary row: 17-24% down to 1.8%, Uncharted on the Deck). Their numbers, not ours to verify.

## History and pitfalls

- Fork study verdict: **Port**. Rescore C11: -0.2 to -0.8 ms (ceiling: stage B idle 9%), medium risk, "possibly wrong
  for the Deck" (refill means stage A decoding the next packets, 4-5x slower there). Not done.
- impl-stage-a-decode.md section 4: "larger, riskier (ordering against the graphics queue), not done". It helps only
  if MediEvil's compute batches lack RELEASE_MEM / WRITE_DATA, and what they contain is not logged.
- Worth building after: count drains by reason and by compute opcode on the current Deck build (existing "drains by
  reason" stats). If most compute drains precede SetShReg / dispatch packets, port with a switch and A/B it live.
- Status is `rejected` in the sense "analysed, not ported". The PC verdict (bubbles irrecoverable) does not settle
  the Deck.
