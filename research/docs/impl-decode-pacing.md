# Implementation: decode pacing under the copy model (`GOW3_DECODE_PACING=1`)

Status: implemented on `deck-lab`, 2026-11-04 (`fb62432`); D0/D1/D2 run on the Deck (section "Deck results").
Follow-up the same day: display buffer label waits that are met do not drain (spacepepperoni 6ef7566), and
`Rasterizer::OnSubmit` runs on stage B (`GOW3_ONSUBMIT_STAGE_B`); built on Windows and Linux, **not launched**. Unit
tests unchanged (8 pre-existing failures). Parent:
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
| `GOW3_ONSUBMIT_STAGE_B` | as `GOW3_DECODE_PACING` | `1`/`0`: OnSubmit's work and the frame's Flush on stage B in stream order instead of a drain (section "OnSubmit on stage B"); ignored with `GOW3_GPU_FENCES=1` |
| `GOW3_VO_LABEL_DRAIN` | off | `1`: every display buffer label wait drains, as before 6ef7566's change (section "Display buffer label waits"); **the new behaviour is on in every configuration** |

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

1. **`Rasterizer::OnSubmit` drained once a frame** (first version). Confirmed on the Deck (D2: `OnSubmit:1592`
   12.2% of stage A, was 1.5%); now on stage B by default with the pacing (section "OnSubmit on stage B").
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

## Deck results (2026-11-04, build `fb62432`; logs `data/deck-decode-D0.log`, `data/deck-decode-D1.log`, D2 on the Deck)

Graveyard heavy view, 67%, auto clocks, `GOW3_EARLY_FRAME_EOP=1` in every run.

| | D0 (baseline) | D1 (pacing, lead 1) | D2 (pacing, lead 2) |
|---|---|---|---|
| FPS | 64-65.5 | unchanged | 64-65 |
| game's ident-64 wait | 5.9 ms | 5.7-7.6 ms (log windows) | 6.6 ms |
| submission lock / pacing waits | lock 83 ms/s | lock gone; pacing 78-103 ms/s (later submissions only: 226 of 1204 waited, 2.27 ms avg; SubmitDone 0; flips 0) | pacing 0 ms/s; max frame lead still 1 |
| stage A waits | submission end (`Process:287`) 8.4%, `ProcessGraphics:1819` (WAIT_REG_MEM, display buffer label) 10.1%, compute 4.6%, `OnSubmit:1592` 1.5% | `OnSubmit:1592` 11.4%, `ProcessGraphics:1819` 9.7%, compute 5.3% | `OnSubmit:1592` 12.2%, `ProcessGraphics:1819` 8.2%, compute 3.9% |
| idle steps | (drains) | 504-571 per 5 s deferred, **all skipped by the generation check** (0 raised): a submission always arrived before stage B reached the step; 1.14 ms avg after the hand-off | |
| visual | | no issues | no issues |

Reading: the two gates are gone, and the time moved to the two drains left on stage A's path. The idle drain moved to
`OnSubmit`'s drain (11-12% of stage A, as risk 1 predicted). The display buffer label wait (`ProcessGraphics:1819`)
drains on every frame. Stage A still reaches the next frame's EOP only after stage B has finished the frame before.
With lead 2 the pacing never waits, yet the frame lead never passes 1: that cap is stage A, not the pacing. Both drains
are removed below. GPU idle is never raised under the pacing (every step skipped). Nothing consumes it there (the lock
is never armed), so this is expected.

## Display buffer label waits (spacepepperoni/medievil_ps4_Linux `6ef7566`, ported 2026-11-04)

The display buffer labels (`VideoOutPort::buffer_labels`) are set to 1 by the flip's WRITE_DATA, which
`PatchFlipRequest` writes into the flip submission and stage B runs in order. They go back to 0 when the presenter
shows the next flip (`driver.cpp`, "Reset prev flip label"). Before rendering into a display buffer the game issues
WAIT_REG_MEM on its label (== 0). Stage A drained the draw pipe before that wait every time (`liverpool.cpp`
`ProcessGraphics`, the `IsVoLabel` branch). Then it slept on the label if it was unmet.

6ef7566 (from the Uncharted Linux fork; the second half of that commit, the rest is not ported): a wait that the
label in memory already meets does not drain. `GOW3_VO_LABEL_DRAIN=1` drains always.

**One addition (ours).** Memory can be stale for this point of the stream: the WRITE_DATA that marks the buffer in use
(1) may still be queued for stage B, so memory still shows the 0 of an older release. Passing then would let the game
render into a buffer whose flip is still to be shown. That can happen only when stage B lags a full display-buffer
cycle, which lead 2 allows. So the fast path also requires that no write to the label is pending in the draw pipe
with a value that fails the test (`PendingFenceValue`, fed by `NotePendingWrite` for every pipelined WRITE_DATA). Such
a wait is counted as "a flip's write pending" and drains as before. An unmet wait drains and sleeps as before. This is
on in every configuration, with or without the pacing.

Stats (`GOW3_FRAME_STATS=1`): `Display buffer label waits (5 s): N met without a drain, S met in memory but a flip's
write pending (drained), D drained (not met)`.

## OnSubmit on stage B (`GOW3_ONSUBMIT_STAGE_B`, ported 2026-11-04)

At `sceGnmSubmitDone`'s flush, `Liverpool::Process` ran `Rasterizer::OnSubmit()` and then `Rasterizer::Flush()`. Both
drained the draw pipe first. OnSubmit then did:
- `ProcessFaultBuffer` (only after a draw with `uses_dma`);
- `ProcessDownloadImages`;
- `RunGarbageCollector` (texture images and samplers);
- `runtime.TickFrame` (the staging pool's frame count and trim).

With the switch, `Rasterizer::OnSubmitInOrder()` hands one task to stage B with `RunInOrder`. The task runs
`OnSubmitWork()` (the same four steps without the drain) and then `Flush()` (stage B's own; its `DrainDrawPipe` does
nothing there). Stage A goes back to decoding at once. Without a draw pipe the task runs on stage A after a drain, as
before. `GOW3_ONSUBMIT_STAGE_B=0` (or the pacing off): the old code, unchanged.

### Thread-safety argument

1. **Same stream position.** The task enters the draw pipe at the moment stage A used to drain, after every packet
   handed over before `sceGnmSubmitDone` and before anything decoded after it. Stage B runs packets one at a time. So
   the four steps run between the same two packets as before. The only differences:
   - they run on stage B instead of on stage A while stage B waited;
   - stage A decodes the next frame meanwhile.
2. **Who owns the caches.** The draw pipe's invariant (`Rasterizer::IsGpuSideThreadId`, `SetDrawPipeSync`) is that
   stage B owns the texture cache, buffer cache, scheduler and staging pool whenever the pipe has work. Stage A
   touches them only after a drain (commands, compute, readbacks, `ForeignRecord`). The four steps are cache
   operations like stage B's own binds and uploads, so running them on stage B adds no sharing that stage B's draws do
   not already have. Each structure in turn:
   - **Fault buffer.** `fault_process_pending` is set by stage B (`BindResources`, `uses_dma`) and is now read and
     cleared on stage B. `ProcessFaultBuffer` is a buffer cache operation (it may `Finish()`, which stage B does for
     readbacks anyway).
   - **Image downloads.** `ProcessDownloadImages` already runs on stage B at every EOP (`RunEventWriteEop`).
   - **Staging pool** (`TickFrame` → `TrimRing` / `TrimLarge`; no lock). Its other user, `Request*` for uploads, is
     the thread that owns the scheduler: stage B, or stage A after a drain. On stage B the trim and the uploads are on
     one thread, never concurrent.
     - Uploads in flight are protected as before. A ring block is trimmed only when idle for `RING_IDLE_FRAMES` and
       `scheduler.IsFree(LastTick)`. A large buffer is trimmed only when not `held`, idle for `LARGE_IDLE_FRAMES` and
       free.
     - A free tick implies its submission was made, and every submission waits for the host copies before
       `vkQueueSubmit`.
     - The Vulkan recording thread is no more drained by the old `DrainDrawPipe` than now, so its exposure is
       unchanged.
   - **Guest threads** (unmaps, invalidations, fault handlers) take the texture cache mutex, as `GarbageCollectImages`
     does. Nothing changes there.
3. **Texture GC with prepared draws ahead of it.** Draws decoded after the step may already be prepared when GC
   frees an image. That is safe because nothing on stage A or the workers holds a texture cache object:
   - **Prepared draws** (`PreparedStage`, `vk_draw_prep.h`) hold the flattened user data, the resource sharps (guest
     T#/S#/V# values), the image description hashes and the program/pipeline. The workers call only
     `pipeline_cache.PrepareGraphicsPipeline` ("never create or compile"). No `ImageId`, image view or `BufferId`.
     `FindPreparedStage` matches sharps against the GPU thread's own flattened data.
   - **Draw packets** from stage A carry register blocks and the pipeline pointer. The pipeline cache is never
     collected.
   - **Every cache object is resolved on stage B at bind time** (`BindTextures`, `FindImage`, targets), after the GC
     step in stream order.
   - **The stage B memos that hold `ImageId`s** are the texture-set memo, the image description cache's `found_id`
     and view memo, the target memos and `LastTarget`. They are validated by
     `TextureCache::RegistryGeneration()` / `RegionStamp()`, which `FreeImage` → unregister bumps. They already
     survive the collector running between two stage B packets: that is what the drained version did.
   - **Destruction is deferred.** Image views and the image slot go through `scheduler.DeferOperation` to the GPU
     tick, and samplers likewise. A command buffer recorded before the step keeps its handles valid.
   - **The texture binding helper** (`bind_helper`) is forked and joined inside one draw's `BindResources` (and joined
     by the image access hook), so it never runs while a task packet runs.
   - **The GC's synchronous write-back** of evicted GPU-written images (`DownloadImageMemory(..., true)`) writes guest
     memory while stage A decodes. It touches image memory, not command buffers or descriptors, the same as the
     downloads stage B already does at each EOP.

   So no generation for prepared draws, deferred GC or periodic drain is needed: the collector stays per frame, at
   the same stream position.
4. **The frame's Flush** moves with it. `Flush()` on stage B is the same call `SubmitForSignalsNow` makes there under
   `GOW3_GPU_FENCES`, and the pacing's retire and idle steps are queued after it.

Stats (`GOW3_FRAME_STATS=1`): `OnSubmit on stage B (5 s): N frame-end steps handed over (drains avoided): stage B
reached them a ms avg m max after the hand-off (x ms/s stage A no longer waits); work and Flush a ms avg m max on
stage B`. The `OnSubmit:` site in "stage A waits over 0.5%" should disappear.

Risks: stage B now pays the GC and staging trim inside its busy time (it paid them while idle before, with stage A
waiting). On the Deck that moves up to the "work and Flush" average from stage A's critical path to stage B's. If
stage B is the limit, FPS may not change and the gain is only stage A's lead. Untested: a pressured GC
(write-backs) during gameplay with stage A ahead.

## Deck test, follow-up (one launch per setting, each with the user's go; build after `fb62432`)

Same scene and route as D0-D2. Settings by file:

```bash
# E1: D2's settings: pacing lead 2, now with OnSubmit on stage B and the display label fast path
printf "GOW3_FRAME_STATS=1\nGOW3_EARLY_FRAME_EOP=1\nGOW3_DECODE_PACING=1\nGOW3_DECODE_PACING_LEAD=2\n" > ~/.local/share/medievil-pc/extra-env.txt
# E2: E1 with lead 1 (the safer default)
printf "GOW3_FRAME_STATS=1\nGOW3_EARLY_FRAME_EOP=1\nGOW3_DECODE_PACING=1\n" > ~/.local/share/medievil-pc/extra-env.txt
# E0 (only if E1 shows a change, to split the two): E1 without OnSubmit on stage B
printf "GOW3_FRAME_STATS=1\nGOW3_EARLY_FRAME_EOP=1\nGOW3_DECODE_PACING=1\nGOW3_DECODE_PACING_LEAD=2\nGOW3_ONSUBMIT_STAGE_B=0\n" > ~/.local/share/medievil-pc/extra-env.txt
```

Read off: `grep -E "Extra environment|OnSubmit on stage B|Display buffer label|Decode pacing|Perf:|Frame pacing|stage A waits|Draw pipe|Guest event waits|Guest memory" <log>`.

Decide on:
- FPS and lows against D0 / D2;
- the game's ident-64 wait;
- frame lead (it should reach 2 in E1 if stage A is no longer the cap);
- pacing waits;
- stage A waits (`OnSubmit:` gone, `ProcessGraphics:1819` down by the "met without a drain" share);
- stage B busy.

Visual checks as before, plus the display: tearing or a frame shown twice or out of order would point at the label
fast path (rerun with `GOW3_VO_LABEL_DRAIN=1`); flicker or missing textures after streaming at the GC (rerun with
`GOW3_ONSUBMIT_STAGE_B=0`). Then the 20-30 min soak of the kept setting (guest faults, watchdog lines, "Guest memory"
flat, texture cache "evicted / written back" lines under pressure).
