# Study: Sleeping Village GPU gaps per frame, run 6 (2026-11-25)

Static analysis only. The game was closed; nothing was launched, built or edited. Status words: everything here is
**found** (from the flip trace, the ETW captures and the code). Nothing is fixed, run or verified.

- Run 6: 2026-11-25 12:15:51-12:29:18, release exe (`655ad2a`, npc-waves), game 01.02, Sleeping Village. Settings: 4K,
  DLSS 75%, Reflex limiter at 237 (minimum interval 4219 us, sleep before the present), VSync on (FIFO). Also on:
  the three fence patches, the 7 Deck switches, `GOW3_HDR_GAME=1`, `motion_blur=4` and `GOW3_FLIP_TRACE_GPU_READY=1`.
- Data:
  - `run6-gpu-flip-trace.csv`: 180,098 complete frames, with a `wall` row and a `gpu_ready` row per frame.
  - `run6-gpu.log`.
  - Five WPR ring captures `session-20261125-1219..1224*.etl`.
- **The captures used the `Jitter` profile, not `JitterGpu`.** A probe export has no DxgKrnl provider events, only
  ISR/DPC, CSwitch, ReadyThread and 1 kHz stacks. So there are no GPU queue packets. GPU timing comes from the flip
  trace's `gpu_ready` (section 0).
- The user's context: the same two behaviours also show in the older bestrun build (21886b6), so they are not a
  regression.
- Scripts and outputs: `research-data/sv-hunt-20261125/analysis-run6/` (list at the end).
  - Three ETW windows were exported, parsed into numpy (`np-c1`, `np-c4`, `np-c5`) and the CSVs deleted:
    - c1: 12:19:10-12:19:22, condition 1, moving and still;
    - c4: 12:22:31-12:22:47, calm, then the wall fight at the same spot;
    - c5: 12:23:56-12:24:08, fight.

## Summary

1. **There are three frame states.** The flip trace (`classes.py`) tells them apart per frame. Over 168,235
   frames from 12:17:00 to 12:29:17:
   - **CAP (50%): calm, at the cap.**
     - The GPU is about 92% busy: ~3.9 ms of work in a 4.22 ms frame.
     - It **does start every frame late**. Almost all of frame k is submitted in one flush, in PrepareFrame. That
       flush comes only after the display-buffer label is released, at the previous present. The GPU idles
       0.48-0.59 ms per frame, ~0.3 ms net.
     - Reflex then holds the present until ~0.08 ms after the GPU finishes. That is legitimate, and at the cap it
       costs nothing.
   - **GPU (39%): condition 1, the camera moving.**
     - The GPU is fully fed: frame k is queued 0.69 ms before the GPU finishes k-1.
     - It **needs more time per frame**: GPU time p50 4.25, p90 4.67, p99 5.08 ms, against ~3.9 when still.
     - The CPU side does not change.
     - These frames make 92-98% of condition 1's slow frames.
   - **LATE (11%): condition 2, the fight.**
     - The game thread runs 4.65-4.73 ms of each 4.85-4.92 ms frame (95-97%, no waits).
     - The render thread (85-90%) and stage B (90-92%) are just behind it.
     - The guest's flip submission arrives late. Stage B then needs 2.4-2.6 ms to record it, and only then is frame k
       flushed to the GPU.
     - The GPU idles ~1.0 ms per frame (~0.75 net): about 85% busy.
     - Reflex does not sleep in these frames, and the label never waits.
     - These frames make 90-97% of condition 2's slow frames.
2. **Condition 1 (mouse in a light view) is GPU work, not CPU.**
   - Per frame, while the camera moves, the CPU side is flat or lower: the game thread, the render thread, stage
     A / B, the recorder and the copy workers.
   - The GPU's time per frame rises by about +0.35 ms at p50, +0.8 at p90 and +1.2 at p99.
   - At 237 FPS the still view already uses ~3.9 of the 4.22 ms GPU budget. So any camera-dependent GPU cost tips
     the frame over.
   - The pass is not visible in these captures (no GPU timestamps). The leading suspect is motion blur at quality
     4: screen-space, grows with per-pixel velocity, and is independent of what is on screen.
3. **Condition 2 (the wall fight) is the CPU chain, with the game thread first.**
   - The GPU's idle gap is the consequence: frame k reaches the GPU only when stage B has recorded the late flip
     submission.
   - The GPU itself needs ~4.1-4.25 ms per fight frame. So the fight would sit at the cap, GPU-bound, once the CPU
     side got ~0.7 ms faster.
4. **The early display-buffer release removes almost nothing of either condition.**
   - Condition 2: stage A's label wait is 0.00 ms per LATE frame.
   - Condition 1: the GPU is already fed.
   - In calm frames it would only move the ~0.3 ms bubble, at +1 present interval of latency.
   - The user's reading "the GPU starts late each frame" is **found true** in calm and in the fight. But the late
     start has two different causes:
     - calm: the end-of-frame-only flush plus the label;
     - fight: the CPU chain.
   - In neither case is it what makes the slow frames:
     - in condition 1 the GPU work grows;
     - in condition 2 the game thread sets the frame period.
5. **The NPC-ticking hypothesis, tested:**
   - The game thread is the root of the fight frames (95-97% busy, sets the period): **supported**.
   - The AI / navigation / movement code itself costs 0.14-0.16 ms per frame and does not grow calm to fight:
     **not supported**.
   - Collision (PhysX scene queries, 0.65 to 0.84 ms) and the Blueprint VM (0.65-0.79) are large constant shares.
   - The fight's growth (+1.57 ms on the game thread at the same spot) is spread out: task-graph dispatch +0.50,
     Cascade particles +0.25, engine other +0.21, PhysX +0.19, Blueprint +0.11, core +0.11.

Ranked fixes (section 6):
1. Fight: game-thread work, then stage B per draw.
2. Condition 1: find the camera-motion GPU pass. Motion blur A/B first (a live setting); `GOW3_GPU_PROFILE` if it is
   not that.
3. Incremental GPU submission: flush at the VO-label drain, and at guest submission ends. Lower latency and a
   smoother GPU; no FPS gain in either condition.
4. Early display release: not for these conditions.

## 0. How GPU time is measured without GPU events

- `gpu_ready(k)` is the frame's own ready tick: the flush at the end of `PrepareFrame` (`vk_presenter.cpp:693-696`),
  noted by the `gow3:GpuReady` thread when the timeline semaphore reaches it.
- With `frames_queued=1`, `PrepareFrame(k)` flushes all of frame k, then waits for frame k-1's tick
  (`vk_presenter.cpp:698-716`).
- **GPU-fed frames.** When frame k was queued before the GPU finished k-1 (`queued(k) < gpu_ready(k-1)`, "lump" < 0),
  every command of frame k was on the GPU queue when the GPU began it. So `G = gpu_ready(k) - gpu_ready(k-1)` is frame
  k's true GPU time (plus k-1's present blit). No CPU starvation is possible inside it.
- **When does frame k's work reach the GPU?** Flushes happen only:
  - at guest submission end / frame end (`liverpool.cpp:249, 376`);
  - at `WAIT_REG_MEM` with unsubmitted signals (`:856`);
  - at decoder idle with unsubmitted signals (`:176`);
  - for HZB readbacks (`vk_rasterizer.cpp:4382`);
  - in PrepareFrame.

  **The VO-label wait drains stage B but does not flush** (`liverpool.cpp:1885-1889`).
- **Test (`classes.py`, "rem by lump")**: `rem = gpu_ready(k) - queued(k)`, the GPU time after the final flush, against
  how long the GPU had already been idle (`lump = queued(k) - gpu_ready(k-1)`):

  | lump (GPU idle before the flush) | 0-0.3 ms | 0.3-0.6 | 0.6-0.9 | 0.9-1.2 | 1.2-2.0 | 2.0-3.0 |
  |---|---|---|---|---|---|---|
  | calm (12:25:35-12:26:13), rem ms | 3.91 | 3.64 | 3.63 | | | |
  | fights (cond 2), rem ms | 4.09-4.13 | 3.83-3.88 | 3.85-3.88 | 3.85-3.88 | 3.84-3.88 | 3.84-3.88 |

  `rem` does not shrink however long the GPU waited. So **the GPU does almost none of frame k before PrepareFrame's
  flush**: only ~0.25-0.3 ms of GPU work is submitted earlier (the first bin). Hence:
  - GPU time per frame ≈ rem + ~0.27 ms;
  - net GPU idle per frame ≈ lump − ~0.27 ms.

## 1. Question 1: the frame, per state

Times are medians in ms from `gpu_ready(k-1)`, the moment the GPU finished the previous frame (`gapchain.py`,
`classes.py`). "Flip submission" is the guest registering flip k (its last submission); "irq" is stage B reaching the
flip; "queued" is PrepareFrame's flush, when frame k reaches the GPU.

| | calm / CAP (c4 12:22:34-38; c1 still) | condition 1 / GPU (c1 moving) | condition 2 / LATE (c4, c5 fights) |
|---|---|---|---|
| frame time | 4.22 | 4.32 (cap1 window mean 4.30) | 4.88-4.92 |
| guest flip submission | −3.40 to −3.48 | −4.74 | −1.51 to −1.63 |
| presented(k-1) = label release | +0.19 to +0.26 | −0.94 (presented before the GPU finished) | −3.48 to −3.62 |
| first guest submission of frame k reaching stage A | (stage A busy; n/a) | | −3.34 to −3.45 |
| irq (stage B at the flip) | +0.40 to +0.48 | −0.74 | +0.80 to +0.86 |
| **queued: frame k reaches the GPU** | **+0.52 to +0.59** | **−0.69** | **+0.89 to +0.93** |
| gpu_ready(k) | +4.21 | +4.21 (G) | +4.78 to +4.82 |
| present_start(k) − gpu_ready(k) (Reflex hold) | +0.08 | −1.6 to −2.3 | −2.7 to −3.7 |
| **GPU gap per frame** (lump) | **0.48-0.59** (~0.3 net) | **none** (fed) | **0.98-1.03** (~0.75 net) |
| **GPU time per frame** | **~3.9** (3.63 + ~0.28) | **p50 4.25, p90 4.67, p99 5.08** | **~4.13** (3.87 + 0.26); fed fight frames: p50 4.23-4.25 |
| GPU busy share | ~92% | 100% | ~85% |
| stage A label wait (A ← presenter), ms per frame | 1.6-2.0 | 2.2 (c1) | **0.00** |

What runs in the GPU gap, ms (`gapchain.txt`):
- **Calm:**
  - stage A 0.23-0.25: decoding frame k's tail after the label release;
  - stage B 0.27-0.30 (it waits on A for 0.20-0.28);
  - recorder 0.25-0.28;
  - the game thread runs independently.
- **Fight:**
  - stage B 0.97-1.02 of 1.0: busy for the whole gap, never waiting on A;
  - recorder 0.96-1.01;
  - stage A 0.34-0.38 (the rest of the gap it waits for guest submissions);
  - game thread 0.98-1.00 and render thread 0.95-1.01, both running;
  - RHI 0.08.

Thread running time per frame (`wm-c1.txt`, `wm-c4.txt`, `wm-c5.txt`; ms and % of the frame):

| Thread | calm (c4 CAP) | cond 1 still (c1 CAP) | cond 1 moving (c1 GPU) | fight (c4 LATE) | fight (c5 LATE) |
|---|---|---|---|---|---|
| game `main` | 3.10 (73%) | 2.94 (70%) | 3.05 (71%) | **4.65 (95%)** | **4.73 (97%)** |
| render thread | 2.01 | 1.23 | 1.26 | 4.41 (90%) | 4.14 (85%) |
| RHI | 0.89 | 0.89 | 0.91 | 1.14 | 1.12 |
| stage A | 1.65 | 1.59 | 1.53 | 3.24 | 3.20 |
| stage B | 2.45 | 2.36 | 2.28 | **4.51 (92%)** | **4.37 (90%)** |
| recorder | 2.46 | 2.35 | 2.27 | 4.31 | 4.17 |
| presenter | 0.27 | 0.26 | 0.25 | 0.98 | 0.88 |

## 2. Question 2: condition 1, mouse movement in a light view

Capture 1 is 12:18:12-12:19:47; the light area, camera only. In the window 12:18:20-12:19:47 (`classes.txt` cap1):
70% of frames are GPU-fed (G p50 4.25, p90 4.67, p99 5.08, max 5.83). Captures 2 and 3 (12:20:13-12:21:23) are
the same: 78-92% GPU-fed, G p50 4.29-4.36, p99 5.15-5.25.

Per second, c1 12:19:10-12:19:21 (`wm-c1.txt`; mouse = raw-reader wake-ups per frame):

| Seconds | mouse / frame | classes | game | render | A | B | REC | G (median) |
|---|---|---|---|---|---|---|---|---|
| 12:19:10-15 (moving) | 1.4-1.8 | GPU 87-194 of ~232 / s | 2.68-3.44 | 1.16-1.36 | 1.31-1.73 | 2.01-2.54 | 2.00-2.51 | 4.18-4.36 |
| 12:19:17-19 (still) | 0.00-0.07 | all CAP | 2.72-2.81 | 1.21-1.22 | 1.60-1.64 | 2.38-2.44 | 2.37-2.43 | 4.21-4.22 (cap) |
| 12:19:20-21 (moving) | 2.9 | GPU 194 of 231 | 2.99-3.05 | 1.28-1.32 | 1.59-1.61 | 2.40-2.41 | 2.38-2.41 | 4.23-4.28 |

- **The CPU side does not change with movement.**
  - Moving vs still frames:
    - game thread 3.05 vs 2.94 ms;
    - stage A 1.53 vs 1.59;
    - stage B 2.28 vs 2.36;
    - recorder 2.27 vs 2.35.
  - Runtime functions of stage A / B / recorder and the copy workers: no upload, copy or readback grows
    (`hostfn-c1.txt`; largest change −0.13 ms in the recorder loop).
  - Draw counts are therefore unchanged. That rules out occlusion / HZB changes and texture streaming as causes.
- **The GPU work per frame rises.**
  - Still: ~3.9 ms (calm rem 3.63 + ~0.28).
  - Moving, GPU-fed: 4.25 / 4.67 / 5.08 at p50 / p90 / p99, so about **+0.35 / +0.8 / +1.2 ms**.
  - The slow frames: 92-98% of frames over 5 ms in these windows are GPU-fed frames (`slowshare.txt`).
- **Which GPU stage: not measurable here** (no GPU timestamps). What fits is work that grows with per-pixel motion
  and does not care what is on screen: "even looking at the ground".
  1. **Motion blur, `r.MotionBlurQuality` 4** (the log: `motion_blur=4`). At 4K output, the velocity-tile gather takes
     its full path on every tile when the whole view rotates. A live setting (Full / Low / Off).
  2. Other temporal / velocity work at output size: DLSS evaluation, about constant per frame. The port's UE4
     motion-vector path ("UE4 motion: on"), fixed size. Both less likely.
  3. Not supported by the data: HZB / occlusion and streaming (no CPU change), particle simulation (no fight in
     the view).
- **Why so little headroom.** At 237 FPS the calm frame is a serial loop:
  - the label release (the previous present);
  - +0.3 ms CPU (tail decode, record, flush);
  - +3.63 ms GPU;
  - +0.08 ms Reflex;
  - +0.13 ms present.

  That is 4.15 of 4.22 ms. When the GPU needs more, Reflex presents before the GPU is done (hold negative), the
  queue deepens to one frame and the GPU runs fed. From then on the frame time is the GPU time.

## 3. Question 3: condition 2, the fight against the wall

Same spot, c4: calm 12:22:34-38, fight 12:22:40-46. Fight windows (`classes.txt`, `slowshare.txt`):
- 12:22:40-46: 88% LATE, ft 4.84 mean, 1% low 155 FPS, 0.1% low 141 FPS;
- 12:23:01-42: 62% LATE;
- 12:23:56-12:24:30: 60-76% LATE;
- 12:24:50-12:25:23: 76% LATE, 1% low 152 FPS, 0.1% low 129 FPS.

LATE frames make 90-97% of the fight's frames over 5 ms.

Where a LATE frame's GPU idle goes, per frame (medians, c4 / c5):
1. The guest registers flip k **1.4-1.8 ms after the previous present** (`sub`). Its frame comes from:
   - the game thread at 95-97%;
   - then the render thread at 85-90%;
   - then the RHI, which waits on the render thread 2.28 ms and on task-graph workers 1.27-1.39 ms;
   - then the submit thread.
2. From that flip submission to stage B reaching the flip: **2.36-2.56 ms**.
   - **Stage B runs all of it** (2.34-2.54 ms) and waits on stage A 0.01 ms.
   - Stage A needs only 1.66-1.76 ms of that span.
   - So the record stage, not the decode, gates the end of the frame.
3. PrepareFrame flushes 0.07-0.09 ms later. The GPU had been idle since `gpu_ready(k-1)`: **0.98-1.03 ms mean**, about
   0.75 ms net of the ~0.27 ms flushed earlier.
4. The GPU then needs 3.87 ms (rem) to finish frame k.
5. Reflex does not sleep: present_start comes 0.1 ms after queued, 2.7-3.7 ms before the GPU finishes.

So:
- **What each gap waits on:**
  - the stage A label wait: 0;
  - the present: 0;
  - stage B: the last 1.0 ms, recording the late flip submission;
  - upstream, the guest chain that made that submission late: game thread, then render thread.
- **What sets the period:** the game thread. It runs 4.65-4.73 ms per 4.85-4.92 ms frame, its waits total 0.16 ms
  (its own task-graph workers), and the guest's flip-to-flip interval equals the frame time (4.74-4.83 ms).
- **The next limits** if the game thread fell under the cap:
  - stage B 4.37-4.51 ms per frame;
  - render thread 4.14-4.41;
  - recorder 4.17-4.31;
  - then the GPU at ~4.13-4.25.

  Everything rises together, as in run 4's fight: stage B 2.38 to 4.50 ms at the same spot is about 1.9x the calm
  draw load.
- **Frame pacing** in the fight: frame-to-frame change |dft| is 0.6-0.7 ms in LATE frames vs 0.08-0.15 calm. The
  frames flip between LATE, GPU-fed (19-27% of fight frames, G 4.23-4.25) and CAP. With Reflex not sleeping, the
  frame time follows the game thread's per-frame variance.

### 3.1 The game thread in the fight (`gamecat-c4.txt`, `gamecat-c5.txt`; ms per frame, exclusive by code area)

| Area | calm (c4 CAP) | fight (c4 LATE) | change | fight (c5 LATE) |
|---|---|---|---|---|
| **total** | 3.12 | 4.69 | **+1.57** | 4.68 |
| task-graph queues / events | 0.49 | 0.99 | **+0.50** | 0.99 |
| PhysX scene queries + scene lock (collision) | 0.65 | 0.84 | +0.19 | 0.87 |
| Blueprint VM | 0.65 | 0.76 | +0.11 | 0.79 |
| engine other | 0.35 | 0.56 | +0.21 | 0.54 |
| Cascade particles | 0.11 | 0.36 | **+0.25** | 0.32 |
| core / CoreUObject other | 0.21 | 0.32 | +0.11 | 0.30 |
| **AI / navigation (Detour) / movement** | **0.15** | **0.14** | **−0.00** | 0.16 |
| navigation path purge (patched) | 0.05 | 0.07 | +0.02 | 0.08 |
| animation, tick dispatch, UI, audio | 0.28 | 0.39 | +0.11 | 0.39 |

Elsewhere, same windows:
- **Render thread**: 1.84 to 4.40 (+2.56). Renderer / RHI +1.15, particles +0.45, core +0.41.
- **Task-graph workers** (sum): 3.00 to 7.80. Rendering tasks +1.70, PhysX simulation +0.90, particles +0.74. The game
  thread waits on them only 0.16 ms per frame.

Guest stacks are one frame deep: costs are exclusive per function. AI code that sweeps or traces is booked under
PhysX, and AI logic written in Blueprints under the Blueprint VM. So "NPC work" in the broad sense is at most
AI 0.14 + PhysX queries 0.84 + Blueprint 0.76, about 1.7 ms per frame. Only ~+0.3 of it grows in the fight. The
growth is spread over task dispatch, effects and particles.

## 4. Question 4: what the early display-buffer release and the game thread account for

| State | label wait (stage A ← presenter) | GPU gap now | Removed by the early release ([study-early-display-release.md](study-early-display-release.md)) | Removed by a faster game thread |
|---|---|---|---|---|
| calm / CAP | 1.6-2.0 ms per frame | 0.48-0.59 (~0.3 net) | the ~0.3 ms net bubble: frame k's tail is decoded during k-1's Reflex sleep. No FPS change at the cap; +1 present interval (4.2 ms) of latency. | nothing (not limiting) |
| condition 1 / GPU-fed | 2.2 ms per frame | none | **nothing**: the GPU is already fed. The CPU only runs one frame further ahead (latency). | nothing |
| condition 2 / LATE | **0.00** | 0.98-1.03 (~0.75 net) | **nothing**: the label is released before stage A gets there | the frame period: ~0.45-0.5 ms per frame over the cap is the game thread. Then stage B (+0.15-0.3) and the render thread. |

Other causes found:
- **End-of-frame-only GPU submission.** In the calm frames it is half of the GPU idle. In the fight it turns the CPU
  chain's lateness into a whole-frame GPU start delay:
  - stage B records frame k for 2.4-2.6 ms after the flip submission;
  - the GPU sees none of it until PrepareFrame.

  Submitting as stage B records would let the GPU follow ~0.3-0.5 ms behind the record stage. The fight's
  `gpu_ready(k)` would come about 3 ms sooner after the flip (latency), with the same frame rate.
- **Reflex is not a cause.**
  - Calm: it presents 0.08 ms after the GPU finishes, as designed.
  - Condition 1: it presents before the GPU finishes, letting the queue deepen.
  - Fight: it does not sleep at all.

  Run 5's "PRESENT class" (Reflex sleeping 3.4 instead of 2.4 ms with the mouse moving) is condition 1: the GPU got
  heavier. That is reading 1 of run 5's section 3. Reading 2, a refill bubble, exists only as the ~0.3 ms calm gap
  and does not make the slow frames.

## 5. The user's readings, tested

| Reading | Verdict | Evidence |
|---|---|---|
| Reflex legitimately waits for the GPU | **found true** in calm and condition 1; Reflex does not wait at all in the fight | hold +0.08 ms (calm), −1.6 to −2.3 (cond 1), −2.7 to −3.7 (fight) |
| The GPU runs behind because something delays the start of its work each frame | **true in calm and the fight, but not what makes the slow frames** | calm: the label plus the end-only flush delay the GPU ~0.3 ms net (at the cap, free). Fight: the GPU waits ~0.75 ms net for the record stage, which waits for the late guest chain; the period is the game thread's. Condition 1: no delay, the GPU is fed and slower. |
| Stage A waits at the VO label every frame (double buffering) | **true in calm and condition 1, 0 in the fight** | A ← presenter 1.6-2.2 ms per frame (CAP / GPU), 0.00 (LATE) |
| NPC navigation, collision and AI ticking every rendered frame is the root | **the game thread is the root of the fight; the AI / navigation code itself is not** | game thread 95-97% busy and sets the period. AI / nav / movement 0.14-0.16 ms, flat calm to fight; collision queries 0.84 (+0.19); Blueprint 0.76 (+0.11); the growth is task dispatch +0.50, particles +0.25, engine +0.21. |
| Not a regression (same in bestrun) | consistent | condition 1 is GPU work of the game's frame; condition 2 is guest CPU work plus stage B per draw |

## 6. Fixes, ranked (proposals only; each needs the user's go; one change per launch)

1. **Condition 2: the game thread first, then stage B.** The fight exceeds the 4.22 ms budget by:
   - game thread: 0.45-0.5 ms;
   - stage B: 0.15-0.3;
   - render thread: up to 0.2.

   All three must come down to hold 237. The GPU (~4.1-4.25 ms) would then be the limit, right at the cap.
   - First measure inclusive costs per tick group: the guest frame-pointer walk of study-sv-hotspots.md 8.4. The
     one-frame stacks cannot say how much of the PhysX and Blueprint time is NPC ticking.
   - Candidates by growth:
     - task-graph dispatch overhead (+0.50; the fence patch is already on);
     - Cascade particle ticks (+0.25 game thread, +0.45 render thread, +0.74 workers);
     - collision queries (0.84 constant).
   - An NPC tick-rate cap (AI, movement sweeps, Blueprint) can save at most part of ~1.7 ms. Only ~0.3 of it is
     fight growth. [inferred]
   - Stage B per draw: the fight is ~1.9x the calm draws; existing work items stage-B-per-draw / impl-stage-b-*.
2. **Condition 1: find the GPU pass that grows with camera motion.**
   - Step 1: a settings A/B with no code. Motion blur Full to Off (live) during the mouse wiggle, with the flip trace
     `gpu_ready`. Compare `classes.py` GPU-fed G (p50 / p90 / p99) and the share of GPU-fed frames.
     - Expected if it is motion blur: GPU-fed frames mostly gone, G back to ~3.9.
   - Step 2, if not: a dev build with `GOW3_GPU_PROFILE=1` (per-pass timestamps, "slow passes by growth",
     card gpu-profiler-fixes) on still vs wiggle.
   - Whatever the pass: the calm view uses ~92% of the GPU at 237. A pass that costs 0.3-1.2 ms only in motion
     needs one of:
     - a cheaper setting (motion blur Low / Off);
     - a lower render scale (live);
     - a cap the GPU can hold while panning.
3. **Incremental GPU submission** (code, our pipeline).
   - (a) Flush at the VO-label drain before stage A sleeps (`liverpool.cpp:1885-1889`): one extra submit per frame.
     - Calm: the GPU starts frame k's head behind k-1. The ~0.3 ms net idle goes, and the at-cap headroom grows
       from ~0.07 to ~0.4 ms, without the extra queued frame of the early release. [inferred]
   - (b) Flush at guest submission ends, or every N draws, during the frame.
     - Fight: the GPU follows the record stage. About 3 ms less latency per frame and no idle bursts, but the same
       frame rate while the game thread bounds it. [inferred]
   - Cost / risk: more `vkQueueSubmit` calls on stage B. "Mid-frame split submits" were dropped on analysis before
     (fix-inventory.md, study-frame-cost section 4): re-read why before building (b).
   - Neither fixes the slow frames of condition 1 or 2.
4. **Early display-buffer release: do not build it for these conditions.**
   - Fight: 0 removed. Condition 1: 0 removed.
   - Calm: the bubble only, at +4.2 ms latency.
   - 3(a) gives the calm part without the latency.
5. **Reflex markers**: no evidence they cause either condition (section 4).

## 7. Limits

- No GPU queue events (the Jitter profile). GPU busy per frame is exact only for GPU-fed frames. Elsewhere it is
  `rem` + ~0.27 ms, from the rem-by-lump test (section 0), not measured per frame.
- Per-pass GPU cost is unknown: the condition 1 pass is a suspect, not found.
- `gpu_ready` is noted by a thread (`gow3:GpuReady`) that wakes on the semaphore: tens of us late at most. The
  thread itself cost small overshoots at 237 (flip-trace card).
- Thread roles come from sampled functions (no ETW thread names in these captures). The NVIDIA driver thread is
  inferred, and missing in c5.
- c5's CAP frames are short fight pauses (12:24:04), not a calm baseline. Calm vs fight at one spot uses c4.
- Guest costs are exclusive (one-frame guest stacks). The AI vs collision split is by code area.
- 1 kHz samples, so 1 sample ≈ 1 ms of running. The c4 / c5 windows have 1,000-1,300 LATE frames each.

## Files (`research-data/sv-hunt-20261125/analysis-run6/`)

| File | What |
|---|---|
| `flip6.py` / `flip-overview.txt`, `frames.npz` | flip trace to numpy (local clock); per second: frame time, gpu_ready interval, tail, rem, hold |
| `classes.py` / `classes.txt` | per-frame CAP / GPU / LATE classes, per window; GPU idle bounds; rem by lump (section 0) |
| `rawtl.py` / `rawtl.txt` | consecutive frames' stages relative to presented(k-1): calm, moving, fight |
| `slowshare.py` / `slowshare.txt` | slow frames by class, GPU time distributions, lows per window |
| `parse6.py`, `r6.py`, `roles6.py` / `roles-c1/c4/c5.*` | xperf export to numpy (copy of run 5's parser with a PID argument), loader on run 5's `r5.py`, thread roles |
| `np-c1/`, `np-c4/`, `np-c5/` | parsed ETW windows (CSV exports deleted) |
| `wm6.py` / `wm-c1.txt`, `wm-c4.txt`, `wm-c5.txt` | per class: thread running ms and waits by waker; per second with mouse wake-ups |
| `gapchain.py` / `gapchain.txt` | the GPU gap per frame and what each thread did in it; the flip submission to irq segment |
| `gamecat6.py` / `gamecat-c4.txt`, `gamecat-c5.txt` | guest threads by code area (hotspot study's `classify.category`), per class |
| `hostfn6.py` / `hostfn-c1.txt` | runtime functions of stage A / B / recorder / copy workers, still vs moving |
