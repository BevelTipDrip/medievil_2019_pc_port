# Study: releasing a display buffer's VO label earlier (effective triple buffering)

2026-11-25, static code study for P18 step (b) ([plan-work-queue.md](plan-work-queue.md) P18,
[study-sv-critical-path.md](study-sv-critical-path.md) section 6 item 4). Branch `npc-waves` at `8a2f3c1`. Code read
only: nothing built, launched or changed. Line numbers are from that commit.

## Summary

- **Found: no path reads the guest display buffer after `Presenter::PrepareFrame`.** The copy / post-process into the
  presenter's own frame image is recorded on stage B, in stream order, inside the flip interrupt. The present,
  the overlay, idle redraws, dropped flips and blank frames use only the presenter's frame image or the swapchain
  image. Screenshots, frame dumps and the motion debug view all read inside `PrepareFrame`. So the label only has to
  outlive the copy, not the present.
- **Found: the release today is two presents late.** Buffer X's label goes back to 0 when the *next* flip completes
  (`CompleteFlip`, "Reset prev flip label"). With the default inline present that is after the next frame's Reflex
  sleep and `vkQueuePresentKHR`. With 2 guest buffers, frame N+1's tail waits for frame N's present. That is the
  1.9-2.6 ms per frame stage A sleep and the "frame lead ... 1 max" of run 1.
- **Recommended release point: the buffer's own flip completion, once its copy has finished on the GPU.** That is one
  present earlier than today and still tied to the present cadence. The queue between the pipeline and the present
  then holds at most **one** finished frame (triple buffering, with the presenter's frame images as the third
  buffer). Flip status, flip events, `flip_pending_num` and `gc_queue_num` stay exactly as today.
- **Not recommended as the first build: release as soon as the copy is done, independent of the present** (the
  wording of P18 (b)). It is just as safe for memory, but the only bound left is the presenter's frame pool. That
  allows 2 queued frames with 3 swapchain images (NVIDIA FIFO) and 3 with 4 images, so latency depends on the
  driver's image count.
- **Expected effect:** below the cap, the label wait is met and stage A stops draining and sleeping every frame.
  Decode pacing and early frame EOP can reach lead 2. At the cap, the wait still happens about once a frame, but one
  frame later and off the critical path: a finished frame is waiting when the present thread wakes. The cost is up
  to +1 present interval of latency while the cap binds (+4.2 ms at 237, +8.4 ms at 119, +16.7 ms at a Deck 60 cap).
- **Main risks:**
  - Decode pacing lead 2 binds for the first time. Its heap argument has zero margin there
    ([impl-decode-pacing.md](impl-decode-pacing.md):126-139).
  - The user found extra buffering hurt the lows before ([frames-queued-2](../patches/present/frames-queued-2.md)).
  - Reflex cannot see the queued frame: all its markers are set at the present.
  - The game's `RHIFrameOffsetThread` waits on flip events once a frame. If UE4 throttles on those, the gain may
    move there instead.

## 1. The frame path today (default Windows settings)

Settings: `present_inline=1` → `GOW3_PRESENT_THREAD=0`; limiter Reflex (`GOW3_REFLEX=1`,
`GOW3_REFLEX_SLEEP_BEFORE_PRESENT=1`, `GOW3_PRECISE_LIMIT=1`, `GOW3_LIMIT_HOLD=1`); `frames_queued=1`; the 7 deck_*
switches on (early frame EOP, decode pacing lead 2, ...). Sources: `scripts/graphics_options.py:561-565, 588-589, 628-643`.

| Step | Where | Thread | Touches the guest buffer? |
|---|---|---|---|
| 1. The game's command buffer waits `WAIT_REG_MEM label[X] == 0` before it writes display buffer X (late in the frame: composite, then UI) | `liverpool.cpp:1849-1891` | stage A | label only |
| 2. The flip submission: `PatchFlipRequest` replaces the guest's prepareFlip with `WRITE_DATA label[X] = 1` and a `PatchedFlip` NOP; `sceVideoOutSubmitEopFlip` registers the flip IRQ handler | `gnmdriver.cpp:2115-2150`, `video_out.cpp:365-381` | guest submit thread | no |
| 3. Stage B runs the `WRITE_DATA` (pipelined, `NotePendingWrite`), then `SignalFlip`: `WaitDeferredSignals`, then raises `GfxFlip` | `liverpool.cpp:1343-1355, 1783-1802, 610-640` | stage B | no |
| 4. The IRQ handler: `SubmitFlip` (`flip_pending_num++`, `gc_queue_num++`) → `SubmitFlipInternal` → **`PrepareFrame`** | `video_out.cpp:372-378`, `driver.cpp:1378-1439` | stage B (inside the IRQ) | **yes: the only read** |
| 5. `PrepareFrame`: `GetRenderFrame` (pool), `UpdateImage`, screenshot / dump copies, motion view, `Transit` to ShaderReadOnly, FSR1 / RCAS (compute) and `pp_pass` (fragment) into `frame->image`, `draw_scheduler.Flush` (`ready_tick`), then the `frames_queued` wait for the previous flip's tick | `vk_presenter.cpp:495-718` (reads 617-652, passes 672-679, flush 692-696, wait 698-716) | stage B | yes, recorded in stream order |
| 6. The request is queued (`queued`); the present thread takes it at once (Reflex: `current_limit()` returns 0, `immediate_flips`) | `driver.cpp:1429-1438, 1489-1526, 1632, 1772-1808` | vblank thread | no |
| 7. `Flip` → `RunPresenter` runs inline (`separate_swap` false) → `Present`: acquire, blit **`frame->image`** to the swapchain image, overlay, submit (waits `frame->ready_semaphore` at `ready_tick`), **Reflex sleep**, `vkQueuePresentKHR`, frame back to the pool | `driver.cpp:379-395, 150-154`; `vk_presenter.cpp:849-1078` (blit 944, waits 1007-1011, sleep 1054-1056, free 1074) | vblank thread | no |
| 8. `CompleteFlip`: flip status (`count`, `current_buffer`, `flip_pending_num--`, `gc_queue_num--`), flip events, then **`buffer_labels[prev_index] = 0`**, `SignalVoLabel`, `prev_index = req.index` | `driver.cpp:1167-1169, 1323-1360` | vblank thread | label only |

So label[X] goes to 0 when the flip *after* X completes, which is after that next frame's Reflex sleep and present.
The guest alternates its two buffers itself (EndFrame: display index `(i+1) % 2`, [study-eop-wait-site.md](study-eop-wait-site.md):155).
Frame N+1 (buffer B) therefore waits in its tail for `CompleteFlip(N)`, which releases B from flip N-1. That is the chain of
[study-sv-critical-path.md](study-sv-critical-path.md):105-121 ("presented 176 → A: display-buffer label of frame 177's tail").

Run 1 (`research-data/sv-hunt-20261125/run1-framestats.log`, 237 cap, median 4.22 ms):
- label waits: 2370 met / 1185 drained per 5 s, so **exactly one unmet wait per frame** (lines 394, 450, 500);
- decode pacing: lead 1.00 avg, 1 max, never waited (395, 451);
- early frame EOP: max lead 1 (388, 444).

The stage sampling of 2026-11-16 ([plan-work-queue.md](plan-work-queue.md), the RHIThread row) had stage A 47% in
`VideoOutPort::WaitVoLabel`.

### 1.1 Every path that could read the guest buffer (question 1)

| Path | Reads | When | Guest buffer after `PrepareFrame`? |
|---|---|---|---|
| Copy / post-process (`fsr_pass`, `pp_pass`) | the texture-cache image of X (or the upscaler's per-address display image) | `PrepareFrame`, recorded in stream order (`vk_presenter.cpp:617-679`) | no |
| FSR1 / RCAS | the same view, **in a compute shader** (`fsr_pass.cpp:200-243, 358-363`), while the texture cache recorded FragmentShader (`vk_presenter.cpp:648-650`) | inside `PrepareFrame` | no. A later guest write's barrier still waits for it: FSR's return barrier (compute → AllCommands, `fsr_pass.cpp:366-381`) chains into pp_pass's fragment reads |
| F12 screenshot (`Gow3Overlay::TakeScreenshotRequest`) | X, copied to a host buffer | inside `PrepareFrame` (`vk_presenter.cpp:596-626`) | no; the PNG is encoded from the copy |
| Frame dump (Shift+F12, `GOW3_FRAME_DUMP_TRIGGER`) | X and `frame->image`, to host buffers | inside `PrepareFrame` (627-634, 680-690); files written later from the buffer (`DumpRaw` 482-492) | no |
| Motion debug view (`GOW3_UE4_MOTION_VIEW`) | X, in a compute pass | inside `PrepareFrame` (635-646) | no |
| Upscaler display override (`TemporalUpscaler::DisplayOverride`, per guest address) | the port's own image for that address | inside `PrepareFrame` (528-536, 573-616) | no (off for MediEvil: `GOW3_UPSCALER=none`) |
| HDR: `ReplaceHdrComposite` / `ReplaceHdrTonemap` / `ReplaceHdrGrading` | substitute the game's own draws that *write* the display buffer | in the game's frame, after its label wait (`vk_rasterizer.cpp:1425, 4922-5024`) | no; they are writes and inherit the guest's wait |
| `Present` (blit, overlay, HDR flag) | `frame->image` → swapchain image | present time (`vk_presenter.cpp:890-998`) | no |
| `DrawLastFrame` / `PrepareLastFrame` (menu idle redraw, paused) | `last_submit_frame`, a presenter frame image | any time (`vk_presenter.cpp:293-346`) | no |
| `DrawBlankFrame` / `PrepareBlankFrame` | clears a frame image | port closed / blank flips (720-791) | no |
| `DropFlip` (newest-frame mode, rejected) | nothing (`DiscardFrame`) | `driver.cpp:1313-1321` | no |
| `TraceGpuReady` (flip trace) | the frame's ready semaphore and tick | `vk_presenter.cpp:1087-1123` | no |
| `FrameCapture::OnFlip` (dev) | the address only, for a log | `vk_frame_capture.cpp:157-184` | no |
| NVIDIA capture, RTSS | the swapchain | after present | no |

No path reads the guest buffer at present time, and nothing keeps a view of it past the recorded command buffer:
the upscaled path destroys its view at the GPU tick (`vk_presenter.cpp:583`). The present and everything after it
see only the presenter's frame images.

## 2. Where the release can move (question 2)

### 2.1 What "safe" needs

1. **GPU order.** The next guest write into X must not overtake the copy's reads on the GPU.
   - Every scheduler submits to the one graphics queue (`vk_scheduler.cpp:990`). The guest's later writes into X are
     decoded after its `WAIT_REG_MEM`, and recorded and submitted after `PrepareFrame`, so they come later in
     submission order.
   - The texture cache's `Transit` (`vk_runtime.cpp:204-218`) puts a barrier from the last recorded read before the
     write.
   - So stream order alone would do. Today there is a stronger guarantee: `PrepareFrame(N+1)` waits for flip N's
     ready tick (`frames_queued=1`, `vk_presenter.cpp:704-716`) before `CompleteFlip(N+1)` releases N's buffer. The
     copy is finished on the GPU before the label moves.
2. **Host copies.** The frame's submission waits for the copy threads before `vkQueueSubmit`. A finished GPU tick
   therefore also means the guest memory reads (`UpdateImage`) for that copy are done.
3. **Never overwrite a newer `1`.** A `0` must not land after a later flip of the same buffer has written its
   `WRITE_DATA 1`.
4. **The guest's view.** The label must not open before the flip's own `1` has landed. It cannot: `CompleteFlip`
   always runs after `PrepareFrame`, which runs after `SignalFlip` and its `WaitDeferredSignals`.
5. **Bounded depth.** The guest must not get more than one frame further ahead of the present (latency, and the
   decode-pacing margins).

### 2.2 Candidate release points

| Point | Depth behind the present | GPU-done of the copy guaranteed? | Verdict |
|---|---|---|---|
| Today: next flip's `CompleteFlip` | 0 queued | yes (`frames_queued=1`) | the coupling |
| **Own flip's `CompleteFlip` (after its present), and the copy finished on the GPU** | **1 finished frame queued** | yes (checked, else deferred to today's point) | **recommended** |
| Own flip's `Flip` entry (before the Reflex sleep) | 2 queued (the next tail runs during the sleep) | no | deeper than wanted |
| Copy finished on the GPU (P18 (b) wording) | up to pool − 1: 2 with 3 swapchain images (`vk_swapchain.cpp:385`, min + 1), 3 with 4 | yes | depth set by the driver |
| `SubmitFlipInternal`, copy recorded (as newest-frame mode's early `CompleteFlip`, `driver.cpp:1417-1437`) | pool-bound, as above | no (stream order only) | experiment only |

With the recommended point, frame N+2 (buffer A) waits in its tail for `CompleteFlip(N)` instead of `CompleteFlip(N+1)`:

```
today:        present N ......| N+1 tail -> copy -> queued | Reflex sleep N+1 | present N+1 | N+2 tail ...
recommended:  present N ......| present N+1 (already queued) ...
              N+1 tail -> copy -> queued (during N's sleep) | N+2 tail waits for CompleteFlip(N)
```

The pool covers the steady state: N being presented, N+1 queued and N+2 preparing make 3 frames, the NVIDIA image
count. With fewer images stage B waits in `GetRenderFrame` (`vk_presenter.cpp:1167-1210`) until N is presented. The
present never needs stage B, so this cannot deadlock.

### 2.3 What the guest sees (flip status, events, `current_buffer`)

- **Unchanged:** `flip_status` (`count`, `current_buffer`, `flip_arg`, `flip_pending_num`, `gc_queue_num`) and the
  flip events, all still at `CompleteFlip` after the present.
  - `sceVideoOutGetFlipStatus` and `sceVideoOutIsFlipPending` read the same fields (`video_out.cpp:161-167, 258-283`).
  - The game's `RHIFrameOffsetThread` waits on VideoOut ident 6 (Flip) once a frame
    ([plan-linux-deck.md](plan-linux-deck.md):816, 835); it gets the same events at the same times.
- **What changes:** the game may draw into X while `current_buffer == X`, which a PS4 never allows. On the PS4 the
  shown buffer is the one flipped last; here the screen shows the presenter's copy.
  - UE4 picks its back buffer from its own counter (`G[0x1f0]`, [study-eop-wait-site.md](study-eop-wait-site.md):155).
  - No read of `current_buffer` by the game is known. Not checked in the eboot.
- **`flip_pending_num`** can now be 2 when the next flip is submitted (one queued, one arriving), where it is 1
  today. That is far below the limit of 16 (`driver.cpp:1383`).
  - The limit matters: over 16 the EOP handler asserts (`video_out.cpp:376-377`).
  - The label bounds the queue to 1, so the count cannot climb.
- **Can the game draw into a buffer we still have to read?** No:
  - the only read is in `PrepareFrame`, before the release;
  - a delayed flip (queued behind the present) holds only the presenter's copy;
  - a dropped flip (newest-frame mode) reads nothing.

  The one case to guard is a buffer flipped twice in a row without a guest wait (a loading screen re-flipping).
  There a late `0` from the older flip could clear the newer flip's `1`. A per-buffer flip sequence check covers it
  (section 4.1).

## 3. Interactions (question 3)

| Item | Effect of the recommended release |
|---|---|
| **Label fast path** (`liverpool.cpp:1860-1890`, card [E1-deck-drains](../patches/stage-a/e1-deck-drains.md)) | Below the cap the tail wait is met and needs no drain. At the cap stage A still meets an unmet wait about once a frame (frame N+2 waits for N's present), but the present no longer waits for that frame's tail. The "a flip's write pending" guard (`PendingFenceValue`) matters more now that stage B can lag a full buffer cycle; a non-zero count there means the guard is working. |
| **Decode pacing** (`gow3_decode_pacing.cpp`, lead clamp 24-33, wait 98-143) | Lead 2 can bind for the first time (`frame lead at SubmitDone ... max 2`, then "waited" > 0). Its heap argument has **zero margin** at lead 2 for render-thread pool data and the 3-frame label list, and is one frame short for a frame with no flip and ≤ K submissions ([impl-decode-pacing.md](impl-decode-pacing.md):126-139). Until now that case never happened on Windows (max lead 1 in every log). Fallbacks: `GOW3_SUBMIT_AHEAD=0`, then `GOW3_DECODE_PACING_LEAD=1`. The display buffers are fixed allocations, so the label itself recycles nothing on the guest heap. |
| **Early frame EOP** (`liverpool.cpp:490-521, 1741-1745`) | Max lead 2 (stage A two frame EOPs ahead of stage B) becomes reachable; the lead cap and its fallback to stage B are unchanged. |
| **`frames_queued`** (`vk_presenter.cpp:698-716`) | Unchanged. Stage B still waits for the previous flip's GPU tick at each flip, which keeps the GPU at most one frame behind stage B. At `frames_queued=2` the guard below still gives GPU-done at release. |
| **Frame pool** (`vk_presenter.cpp:150-162`, `GetRenderFrame` 1167-1218) | 3 frames on NVIDIA (presenting, queued, preparing). Log the swapchain image count once, to read Deck / AMD runs. |
| **Reflex** (`vk_swapchain.cpp:145-168, 236-240`; `vk_presenter.cpp:1054-1056`) | Reflex still sets the present interval (`minimumIntervalUs`). The CPU is still paced, now through the label one frame later, the frame EOP waits and decode pacing. Reflex's markers are all at the present, so it does not see the queued frame: its latency figure leaves out the extra interval. It may also sleep differently with more GPU work queued. P18 (c) (true markers) is the natural follow-up. |
| **Limiters without Reflex** (precise + hold, Deck / AMD, `driver.cpp:1772-1808`) | Same rule: the release comes after the slot wait and the present. |
| **`GOW3_PRESENT_THREAD=1`** (swap thread, `driver.cpp:150-163`) | `Flip` queues the present and calls `CompleteFlip` at once, *before* the present (`driver.cpp:385-395, 1167`). An own-flip release there would be bounded only by the pool. **The switch must be ignored in this mode** (or the release moved into the swap-thread lambda after `Present`). |
| **Newest-frame mode** (rejected, `GOW3_NEWEST_FRAME`) | It completes flips at the copy already (`Request::completed`). The switch is ignored there. |
| **HDR passes** (UE4 composite / tonemap / grading) | Writes in the game's frame after its wait; unchanged. The swapchain HDR switch (`SetHDR`: `waitIdle` + recreate) touches only presenter images. |
| **Upscalers** | The UE4 DLSS / FSR paths replace TAA targets mid-frame, not the display buffer. The temporal upscaler's display override is per address and read inside `PrepareFrame`. No change. |
| **Steam Deck / Linux** | Same driver code on the vblank thread; no Reflex, so the precise limiter. Stage B is the wall there (91-93%), so the queue is mostly empty and the gain is the label drain (8-10% of stage A in D0-D2). But at a flat 60 cap just under the Deck's uncapped rate the queue stays full: **+16.7 ms latency**. Judge it separately. |
| **Minimized window / skipped present** (`vk_presenter.cpp:862-881`) | `Present` returns early and `CompleteFlip` still runs; the GPU-done check keeps the release correct. |

**Frames in flight.**

- Today:
  - one frame being presented (N);
  - the pipeline in N+1, held at its tail;
  - the game thread about a frame ahead.
- Recommended, with the cap binding:
  - N being presented;
  - N+1 finished and queued;
  - the pipeline in N+2, held at its tail;
  - the game thread at N+3.
- Below the cap the queue is usually empty and latency is as today or lower: frames reach the present sooner.

## 4. Design (question 4)

### 4.1 The change, behind a switch

**Switch:** `GOW3_EARLY_DISPLAY_RELEASE=1`, read once at start, default off. It is ignored, with one log line, when
`separate_swap` (`GOW3_PRESENT_THREAD=1`) or `GOW3_NEWEST_FRAME=1` is set. A live bit in the toggle file is better
for the A/B, if a free bit exists (check with `patchmap.py`). At a live switch, release the pending `prev_index` by
the old rule first.

**Code** (`driver.h`, `driver.cpp`, `vk_presenter.{h,cpp}`):

1. `Request` (`driver.h:100-112`) gets `u64 ready_tick` and `u32 flip_seq`. `SubmitFlipInternal` (`driver.cpp:1405-1428`)
   fills them after `PrepareFrame`: `frame->ready_tick`, and a per-buffer sequence `++port->flip_seq[index]` (a new
   `std::array<u32, MaxDisplayBuffers>` in `VideoOutPort`, `driver.h:24-41`).
2. `Presenter::DrawTickDone(u64 tick)`: non-blocking. It is `WaitReady(draw_scheduler's semaphore, tick, {})`
   (`vk_presenter.cpp:1125-1157`), already called from several threads (pacer, `GpuReady`).
3. `CompleteFlip` (`driver.cpp:1353-1359`), when the switch is on and `req.eop && req.index >= 0 && !req.completed`:
   - **first**, release a deferred buffer left by the previous completion (see the next step):
     - its label to 0 only if `flip_seq` for that buffer still equals the deferred request's sequence (rule 3);
     - its copy is GPU-done by now: `PrepareFrame` of this flip waited for it (`frames_queued=1`), so this is today's
       release point;
   - **then this flip's buffer:**
     - if `DrawTickDone(req.ready_tick)` and `flip_seq[req.index] == req.flip_seq`: `buffer_labels[req.index] = 0`;
     - else defer it (`port->deferred_index`, `deferred_seq`, `deferred_arg`);
   - `prev_index = -1`, so the old "Reset prev flip label" never runs beside the new rule. If it did, it could clear
     a newer flip's `1` (rule 3);
   - one `SignalVoLabel()` at the end.
4. Everything else as today:
   - the flip status block and flip events (`driver.cpp:1324-1350`);
   - non-EOP CPU flips and blank flips (`index == -1`), which keep the old `prev_index` rule;
   - `Close` (`driver.cpp:195-232`) also clears `deferred_index` and `flip_seq`.
5. Untouched: the WAIT_REG_MEM branch, decode pacing, early EOP, `frames_queued`, the presenter's passes.

### 4.2 Counting it (dev build only: `Gow3Stats::enabled`, `GOW3_DEV_TOOLS`; nothing in release)

- One new line in the 5 s block next to "Display buffer label waits" (`gow3_decode_pacing.cpp:323-335`, or a sibling in
  `driver.cpp`):

  ```
  Display buffer release (5 s, early): R at the buffer's own present, D deferred (copy not finished on the GPU),
    S skipped (a newer flip of the buffer), O by the old rule (CPU / blank flips); queued behind the present at
    release: avg a, max m
  ```

  The queue depth is `requests.size()` read under `mutex` in `CompleteFlip`.
- Stage A's sleep in `WaitVoLabel`, timed in ms/s (`liverpool.cpp:1889`). At the cap the "drained (not met)" count
  is not expected to fall: the time and its place in the frame are what change.
- A flip trace stage `vo_release` (flip_arg of the released buffer's flip), so tools can line it up with
  `queued` / `presented` / `gpu_ready`.
- Patch card: `present/early-display-release.md`, related to E1-deck-drains, decode-pacing, early-frame-eop,
  frames-queued-2, inline-present, reflex, newest-frame. Invariants: rules 1-5 of section 2.1, the swap-thread and
  newest-frame exclusions, and "a flip's own `1` before its `0`".

### 4.3 Rejected alternatives

- **A third guest display buffer** (raise the game's `sceVideoOutRegisterBuffers` count): an eboot patch that UE4's
  `% 2` counter would also need. The presenter's frame images already give the same thing.
- **Enforce the wait on the GPU or the recorder** (stage A decodes past the `WAIT_REG_MEM`, a timeline wait at
  record or submit time): much larger. Stage A would decode the display-buffer writes ahead of a guard that only
  stage B or the GPU enforces, and the decode-pacing stream-order argument would need redoing.
- **Release at the copy only** (2.2): kept as a possible later experiment, after the recommended point has been
  measured.

## 5. Risks

1. **Latency.** Up to +1 present interval whenever the cap binds (the queue is full). Reflex does not report it.
   The user rejected `frames_queued=2` for lows and stability (T-W1, T-Q2) and accepted it only as "cleaner graphs at
   +4.2 ms" (N-25). This is a different buffer, between the guest and the present rather than between stage B and the
   GPU, but the user's verdict on "more buffering" applies.
2. **Decode pacing lead 2 really binds.** It has zero heap margin in two rows and is one frame short for flipless
   frames with ≤ K submissions. The symptom would be a guest allocator fault minutes in. Soak before any default
   change. Fallbacks: `GOW3_SUBMIT_AHEAD=0`, then `GOW3_DECODE_PACING_LEAD=1`.
3. **The PRESENT class may stay.** The slow frames of the run 5 study are Reflex sleeping 0.8-1.0 ms longer. A
   queued frame does not stop Reflex from oversleeping, and with more GPU work queued it may sleep longer still. The
   gain is in GAME-class and mixed frames, where the queued frame covers a late pipeline.
4. **Guest throttling on flips.** `RHIFrameOffsetThread` waits on flip events every frame; their purpose is not
   read. If UE4 holds frames back by flips pending, the gain moves into that wait. Watch "Guest event waits" for
   ident 6.
5. **Deferred releases.** If the copy is often not GPU-done at the present (GPU-bound stretches), releases fall back
   to today's point and nothing changes for those frames. The "D deferred" count shows it. If it is high, release
   from a waiter on the ready semaphore instead (like `GpuReadyThread`; note its thread cost small overshoots at 237,
   flip-trace card).
6. **Mode mistakes.**
   - The swap-thread present and newest-frame mode must not use the rule (section 3).
   - Keeping the old `prev_index` reset beside it can clear a newer `1`.
   - A blank flip (`index -1`) must not index the label array.
7. **Unknown guest readers of `current_buffer` / the labels on the CPU.** None known; a CPU reader would only see
   "free" sooner.

## 6. Test plan (needs the user's go; dev build for the stats; one change per launch)

Baseline: P18 step (a), the GPU-timing run (`GOW3_FLIP_TRACE_GPU_READY=1`, `wpr-session.ps1 -Gpu`), in the same
scene with the same settings (Trolls Head, still camera, then the fight; 4K DLSS 75%, 237 cap, Reflex). This run is
that run plus `GOW3_EARLY_DISPLAY_RELEASE=1`. If the switch is built as a live bit, A/B it live inside one launch
(`flip_toggle_ab.py`) instead. Before the launch, re-read `medievil.ini` and confirm the log is quiet (no periodic
lines beyond the 5 s block). LANDMINES 8b applies: compare flip-trace runs only with flip-trace runs.

Look at, per 5 s window:

1. **The switch works:** "Display buffer release (5 s, early)": R ≈ frames, D small (< 5%), S ≈ 0. "queued behind the
   present" max 1.
2. **Label waits:** below the cap, "drained (not met)" should fall toward 0 (run 1: 778-1185 per 5 s, one per frame).
   At the cap, about one per frame is expected, but stage A's label sleep should move to the start of the frame's
   tail with a frame already queued. Read the new sleep ms/s and the `ProcessGraphics:` share in "stage A waits".
3. **Lead:** decode pacing "frame lead at SubmitDone ... max 2" in some windows (run 1: 1); early frame EOP "max lead 2".
   If decode pacing starts to wait ("waited" > 0), lead 2 is the active bound.
4. **Flip trace:**
   - `queued(N+1)` before `presented(N)` in most frames at the cap (the queue in use);
   - `release − queued` ≈ one interval at the cap;
   - `gpu_ready(N+1)` earlier relative to `present_start(N+1)` than in the baseline;
   - with `-Gpu`, the GPU idle gap per frame should shrink (the refill bubble).
5. **Pacing and feel (decide on these):** "Frame pacing" median / p99 / slowest, the user's overlay 1% and 0.1% lows,
   frames over 6 ms per minute (run 5: ~17 / min). Panning and the fight, not only the still view. Mouse-look latency
   by feel at 237. Also one window at 119, where +8.4 ms may be felt.
6. **Guest side:** "Guest event waits" ident 6 (`RHIFrameOffsetThread`) and ident 64. A rise in ident 6 means risk 4.
7. **Safety:**
   - no "Decode pacing watchdog" line, no guest fault;
   - menus, a level load (loading-screen flips), a movie, the pause menu, HDR on and off;
   - F12 and Shift+F12 still produce the right frame;
   - alt-tab, minimize and restore.
   - Then a 20-30 min soak with level changes before any default change (risk 2).

Deck: a separate launch later, at the user's 60 cap and uncapped. The latency cost at 60 is the main question there.

## Files read

- `gpu/shadps4/core/libraries/videoout/driver.{h,cpp}`, `video_out.cpp`
- `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp` (PatchFlipRequest)
- `gpu/shadps4/video_core/amdgpu/liverpool.cpp` (VO label branch, SignalFlip, early EOP)
- `gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp`, `vk_swapchain.cpp`, `host_passes/fsr_pass.cpp`,
  `vk_runtime.cpp` (Transit), `vk_rasterizer.cpp` (HDR replacements), `vk_frame_capture.cpp`,
  `vk_temporal_upscaler.cpp` (DisplayOverride)
- `gpu/shim/gow3_decode_pacing.{h,cpp}`, `scripts/graphics_options.py`
- Docs and cards: study-sv-critical-path.md, impl-decode-pacing.md, study-eop-wait-site.md, plan-work-queue.md (P18),
  plan-linux-deck.md, cards newest-frame, frames-queued-2, E1-deck-drains, flip-trace.
