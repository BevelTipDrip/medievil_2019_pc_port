# Implementation: decode pacing under the copy model (`GOW3_DECODE_PACING=1`)

Status: implemented on `deck-lab`, 2026-11-04. Built on Windows (CLANG64) and Linux (WSL Debian, AppImage); unit
tests unchanged (8 pre-existing failures). **Not launched**: nothing here is measured or verified in game. Parent:
[plan-linux-deck.md](plan-linux-deck.md) ("U2 A/B, early frame EOP, and the submission gates"). Inputs:
[study-eop-wait-site.md](study-eop-wait-site.md) (what the per-frame wait gates, the 3-frame label free list, the
4-slot transient pool), [impl-gpu-ordered-fences.md](impl-gpu-ordered-fences.md) (`0771a23`: Bloodborne 0.7's `decode`
pacing under `GOW3_GPU_FENCES`), `8600b80` (`GOW3_EARLY_FRAME_EOP`).

## What it is for

With `GOW3_EARLY_FRAME_EOP=1` every frame EOP is signalled at decode (Deck, `data/deck-early-eop-1.log`: 0 fallbacks,
max lead 1), yet the game's ident-64 wait stays ~5.6 ms a frame. Two gates serialise the game with stage B (the draw
recording thread):

1. **The idle drain.** At the end of queued work stage A (`Liverpool::Process`) drained the draw pipe and waited for
   the deferred signals before raising GPU idle (`Process:240` 8.8% of stage A's time, "submission end" 60 drains/s).
   While it drains it cannot decode the game's next submission, so the frame label inside it fires late.
2. **The console lock.** `sceGnmSubmitDone` arms the submission lock every frame (321 of 323 armed), so the next
   submission waits for GPU idle (1.39 ms avg, 88.9 ms/s), which follows the drain.

## Switches

Read once at start. With `GOW3_DECODE_PACING` unset nothing new runs: the default path is today's (the only shared
code is the submission numbering, still behind `GOW3_GPU_FENCES || GOW3_DECODE_PACING`).

| Variable | Default | What |
|---|---|---|
| `GOW3_DECODE_PACING` | off | `1`: the model below. Ignored with `GOW3_GPU_FENCES=1` (logged at start) |
| `GOW3_DECODE_PACING_LEAD` | `1` | frames the game may be ahead of the newest retired frame; clamped to 1..2 (see the safety argument) |
| `GOW3_SUBMIT_AHEAD` | `2` | submissions of a frame that do not wait (shared with `GOW3_GPU_FENCES`); `0`: every submission waits for the lead, which with lead 1 is the console lock's semantics without the drain |
| `GOW3_EARLY_FRAME_EOP` (+ `_LEAD`) | off | unchanged; **on together with this switch is the intended test** |

Start line: `Decode pacing: on (GOW3_DECODE_PACING=1): lead L frame(s), K submissions of a frame ahead
(GOW3_SUBMIT_AHEAD), GPU idle in stream order`.

## Design

### 1. GPU idle without the drain (`liverpool.cpp`)

At the end of queued work (after the inner loop and the `sceGnmSubmitDone` flush), stage A:

1. `SignalDecodedFrames()`: frames whose submissions are all decoded get a retire step (below);
2. `DeferGpuIdle()`: reads the generation (`submit_counter`) and hands a GPU idle step to stage B with
   `RunInOrder`, then returns at once to wait for or decode new work (`continue`, no drain, no
   `WaitDeferredSignals`).

Stage B runs the step after every draw and fence handed to it before, and defers it with
`Scheduler::SignalAfterHostCopies`: the Vulkan recording thread reaches it after the work recorded before it, then
`Gow3Copy::AfterCopies` runs it once the host copies issued so far are done, in registration order with every earlier
deferred signal (`gow3_copy.h`: "Callbacks run in the order they were registered"). The step is
`SignalGpuIdle(generation)` (the `GOW3_GPU_FENCES` function, now returning whether it fired): `work_retired` and the
GpuIdle interrupt only if `num_submits == 0` and nothing was submitted since the generation was read; otherwise it is
skipped and the next end of queued work hands over another. Without a rasterizer the old path runs (nothing recorded).

### 2. Pacing (`gow3_decode_pacing.{h,cpp}`, `gnmdriver.cpp`)

Adapted from 0.7's `decode` lock as ported in `0771a23`. A frame is one `sceGnmSubmitDone` to the next (MediEvil: once
per presented frame, called by the submit task right after its wait for the previous frame's label).

- **Retired** (copy model): all the frame's submissions are decoded on every queue (submission numbers,
  `OldestUndecoded`, as under `GOW3_GPU_FENCES`), then a retire step handed to stage B with `RunInOrder` and deferred
  with `SignalAfterHostCopies` ran: stage B and every label write deferred before it passed the frame's end. Not the
  GPU timeline. Stage A does not wait for it.
- **`sceGnmSubmitDone`** arms no lock. It waits until `frames ended - frames retired < lead`, then numbers the frame
  (`Liverpool::SubmitDone(frame)` records its end) and resets the frame's submission count.
- **Submissions** (`SubmitCommandBuffers[ForWorkload]`, `DingDong`; after the hand-over, as 0.7): the frame's first
  `GOW3_SUBMIT_AHEAD` return at once; later ones wait the same way.
- **The flip submission** (`SubmitAndFlipCommandBuffers[ForWorkload]`) always waits for the lead, even among the
  first K. This is the one addition to 0.7: UE4's EndFrame rotates the label free list and advances the transient
  pool slot right after it (study 1.4), so this wait bounds the guest's frame-counted recycling regardless of how many
  submissions a frame has (argument below). In gameplay a frame has ~4 submissions, so the flip is past K anyway.
- **Lead cap** `GOW3_DECODE_PACING_LEAD` is the bound in that wait. With lead 1 the condition is 0.7's ("every frame
  ended before has retired").
- **Watchdog**: a guest thread that waits 10 s with no frame retired logs once per stall (`Decode pacing watchdog:
  ...`, with the pacing state and the GPU command thread's queues, frame ends, oldest undecoded submission, draw pipe
  idle/busy). It never gives up the wait or kills.

Deadlocks: the pacing only waits for frames already ended, whose submissions are already queued; their retirement
needs only stage A and B. The residual cycle (a WAIT_REG_MEM in an ended frame on a CPU write the game makes after a
later, paced submission) exists under the console lock too (its next submission waits for everything before).

### 3. With `GOW3_EARLY_FRAME_EOP`

Unchanged code. The frame EOP is signalled when stage A decodes it (lead cap on `frame_eops_recorded`, counted by
stage B). With no idle drain stage A decodes the frame's last submission as soon as it arrives, so the label (and the
game's next submissions) move forward. The game then meets the pacing wait instead of the console lock.

```
game: wait label F-1 -> sceGnmSubmitDone (lead wait) -> first K submissions -> later ones / flip (lead wait)
stage A: decode (frame EOP signalled here with GOW3_EARLY_FRAME_EOP) -> frame decoded -> RunInOrder(retire F)
         end of queued work -> RunInOrder(GPU idle, generation) -> back to waiting / decoding at once
stage B: draws, fences (SignalAfterHostCopies) ... retire F / GPU idle -> SignalAfterHostCopies
Vulkan recorder + copy threads: deferred signals in order after the host copies: labels ..., retire F, GPU idle
```

## Heap-safety argument

The rule (the comment kept in `Process`): the guest frees or reuses the objects holding its labels once it believes
the work done, so every label must be written first (else a late write lands in reused memory: the corrupted guest
heap free list seen when GPU idle once fired before stage B finished, and stage B's lag was unbounded).

1. **Order of the writes.** Every label of work before an idle or retire step is written before the step runs:
   - labels deferred on stage B (`SignalAfterHostCopies`, the default `RecorderFences` path, W3 / W3b) run in
     registration order (Record stream order, then `AfterCopies` epochs in order); the step is registered after them;
   - labels stage B writes synchronously (EOS, WRITE_DATA, `RecorderFences` toggle off, `GOW3_ASYNC_FENCES` via
     `AfterCopies` called earlier) are done or queued earlier;
   - stage A's own writes (compute queue, after its drain) happen before the step is handed over;
   - `GOW3_VK_RECORD_THREAD=0`: `SignalAfterHostCopies` waits for the host copies and runs at once on stage B: same
     order.
   So "idle" and "retired" mean what the old drain plus `WaitDeferredSignals` meant: stage B passed and the deferred
   labels ran. It is not the GPU timeline, as today.
2. **Generation.** A submission made between the hand-over and the step changes `submit_counter` (incremented before
   `num_submits`), so the step cannot mark later, undecoded work as retired.
3. **What the guest learns, and when.** Under the console lock the first submission after `sceGnmSubmitDone(H)` returns
   only once everything before is retired. Under the pacing the guest learns nothing at idle (the lock is never
   armed); its only waits are the lead waits, which wait for retired frames. UE4 recycles by frame count (study 3):

   Distances needed when the RHI starts frame G (study 3.4, 4; the label list: released in R, reused from R+3 after
   EndFrame(R+2), so G-3):

   | Memory | Needs retired | Console lock (today) | Lead 1 | Lead 2 |
   |---|---|---|---|---|
   | transient pool, RHI-thread data | ≥ G-4 | G-2 | G-2 (+2) | G-3 (+1) |
   | transient pool, render-thread data [inferred lead 1] | ≥ G-3 | G-2 | G-2 (+1) | G-3 (0) |
   | label free list (3 frames) | ≥ G-3 | G-2 | G-2 (+1) | G-3 (0) |
   | same, frame ended without a flip submission and ≤ K submissions | | G-2 | G-3 (0) | **G-4 (-1)** |

   How: at frame H's flip submission (`sceGnmSubmitDone(H)` came before it in the same frame, 0x68acc0) the frames
   ended are up to H-1's submissions; the wait leaves at most `lead - 1` of them unretired, so retired ≥ H-1 (lead 1)
   or H-2 (lead 2) when the RHI starts G = H+1. Inside frame H, `sceGnmSubmitDone(H)` already gave ≥ H-2 / H-3, which
   covers what H writes (pool slot H-4, labels released ≤ H-3) and the render thread's H+1 slot (H-3).
   - **Default lead 1**: as safe as today for frame-counted recycling at every frame start, one frame spare.
   - **Lead 2**: zero margin for render-thread pool data and labels, and one short if a frame ends without a flip
     submission and with ≤ K submissions (loading screens may). It is the experiment, not a default. Above 2 is
     clamped: the 3-frame list would be short in the normal case.
4. **Residual (as 0.7, by design):** with K > 0 the first K submissions of frame H return before H-1 is retired. If
   UE4 freed something on the console's implication "my submission returned, so all before is done", K > 0 breaks it.
   The study found no such path (the wait at 0x68af47 frees nothing; recycling is frame-counted) and 0.7 ran
   Bloodborne at K=2 without corruption (a leak only). `GOW3_SUBMIT_AHEAD=0` with lead 1 is exactly the console lock's
   guarantee.
5. **Early frame EOP** now overtakes more pending labels (no drain before its decode). That label only gates
   submission (study 1); what the game does after seeing it meets the lead waits above.
6. Labels to memory unmapped meanwhile are dropped by `StoreLabel`, as before.

## Statistics (`GOW3_FRAME_STATS=1`, 5 s block, after the "Guest memory" line; nothing with the switch off)

```
Decode pacing (5 s, lead L, ahead K): SubmitDone n, waited w (a ms avg m max, x ms/s); later submissions: s
  submissions (p/frame), waited t (a ms avg m max, x ms/s; f of them flips); frame lead at SubmitDone a avg, M max;
  frames retired r, a ms avg m max after their last submission was decoded
Decode pacing idle (5 s): d GPU idle steps deferred to stage B (r raised, s skipped by the generation check); stage A
  drains avoided d: stage B and the labels reached them a ms avg m max after the hand-off (x ms/s stage A no longer
  waits)
```

- "later submissions" counts every paced submission (graphics, compute, flips); "waited" only those beyond K or flips
  that found the lead reached.
- "frame lead at SubmitDone" is frames ended minus retired right after the frame is numbered: max = the lead cap when
  the cap binds.
- "stage A time saved" is the hand-off to the step's run: how long the old drain + `WaitDeferredSignals` would have
  held stage A (an upper bound: stage A might have had nothing else to do).
- Also read: "Submission lock" (SubmitDone counted, 0 armed), "Draw pipe" (stage A waited %, "submission end" drains
  ~0; **watch `rasterizer` / `OnSubmit:` in the stage A wait sites**, see Risks 1), "Guest event waits" ident 64,
  "Early frame EOP", "Guest memory" (the soak).

## Risks

1. **`Rasterizer::OnSubmit` still drains once a frame** (at `sceGnmSubmitDone`'s flush: fault buffer, image
   downloads, texture GC, `TickFrame` need stage A to own the caches). Before, the idle drain had already emptied the
   pipe (5 such drains/s waited); now it waits for stage B there. It does not block the game (the label of the frame
   before is already out) but bounds how far stage A runs ahead, so lead 2 may gain little over lead 1. If the
   `OnSubmit:` wait site grows large, moving that work to stage B in order is the next step (not done: conservative).
2. **Heap class**: the residual in the safety argument (K > 0; lead 2 at zero margin; the study's inferred
   render-thread lead and Garlic/Onion mapping). Symptom: a guest fault in an allocator minutes in. First fallback:
   `GOW3_SUBMIT_AHEAD=0`, then lead 1.
3. **Deadlock** in a guest-closed cycle (as under the console lock). The watchdog names it; nothing kills.
4. **More work in flight on the CPU side**: the draw pipe and prepared draws stay fuller; stream buffer / staging use
   may rise (watch the mapped writes and "Guest memory" lines, VRAM in the overlay).
5. **Cost**: one deferred signal per end of queued work plus one per frame (a Record and an `AfterCopies` epoch each);
   the idle-step count shows how many.
6. GPU idle has no consumer under the pacing (lock never armed); it is kept so `work_retired` and the interrupt stay
   truthful for any.
7. Untested paths: `GOW3_VK_RECORD_THREAD=0`, the `PipelinedTasks` toggle off (steps run on stage A after a drain),
   frames without a flip submission.

## Deck test (one launch per setting, each with the user's go)

Same AppImage for every run; settings as the early-EOP run (67%, auto clocks, graveyard heavy view). Per run: stand
60 s in the same spot, then walk the same route 60 s. Settings by file (plan-linux-deck.md "Test settings by file";
the user or the session with SSH writes them, no launch):

```bash
# D0 baseline: early frame EOP only
printf "GOW3_FRAME_STATS=1\nGOW3_EARLY_FRAME_EOP=1\n" > ~/.local/share/medievil-pc/extra-env.txt
# D1: + decode pacing, lead 1
printf "GOW3_FRAME_STATS=1\nGOW3_EARLY_FRAME_EOP=1\nGOW3_DECODE_PACING=1\n" > ~/.local/share/medievil-pc/extra-env.txt
# D2: + lead 2
printf "GOW3_FRAME_STATS=1\nGOW3_EARLY_FRAME_EOP=1\nGOW3_DECODE_PACING=1\nGOW3_DECODE_PACING_LEAD=2\n" > ~/.local/share/medievil-pc/extra-env.txt
```

Read off: `grep -E "Extra environment|Decode pacing|Perf:|Frame pacing|Draw pipe|drains by reason|stage A waits|Guest event waits|Early frame EOP|Submission lock|Guest memory" <log>`.

Decide on (per window; compare lows with lows):
- **FPS and frame pacing** (Perf, Frame pacing; the user's overlay 1% / 0.1% lows): D1 / D2 against D0. Keep a
  setting only if FPS is up and lows are not worse.
- **The game's ident-64 wait** (Guest event waits): should drop from ~5.6 ms a frame.
- **Pacing waits** (SubmitDone, later submissions, flips; ms/s) and the frame lead: lead 1's max should be 1; if the
  waits replace the ident-64 wait one for one, stage B is the limit (stage B busy should then rise toward 90%+).
- **Stage A**: "submission end" drains ~0, `Process:` wait gone; the `OnSubmit:` site (Risks 1).
- **Visual checks**: per-draw constants flicker, texture corruption after streaming, missing objects at distance (HZB
  hidden count), menus and a level load.
- **Soak**: the better of D1 / D2 (D1 if D2 is not clearly better), 20-30 min moving between areas with level loads:
  no guest fault, no watchdog line, "Guest memory" flat after loads (sum within the D0 run's range).
- If a heap fault appears: rerun the soak with `GOW3_SUBMIT_AHEAD=0` added (the console's guarantee within a frame).
