# Study: Sleeping Village critical path per frame, run 5 (2026-11-25)

Static analysis only. The game was closed; nothing was launched, built or edited. Status words: everything here is
**found** (from the ETW capture and the code). Nothing is fixed, run or verified.

- Data: `sv-fences-1125-1.etl` (165.5 s, 04:17:29.29 to 04:20:14.77, release exe `out/medievil-runtime.exe`, pid 12392;
  events end at ETW 162.3 s), `run5-fences-flip-trace.csv` (aligned through its `wall` row), `run5-fences.log`.
- Run 5 settings: 4K, DLSS 75%, 237 cap through **Reflex** (minimum interval 4219 us, sleep before the present). The
  log also shows decode pacing lead 2, the 7 Deck switches, and the three eboot patches (nav purge, RHI pending
  deletes, task-queue fences). The capture profile was `Jitter`: no GPU events, and no `gpu_ready` rows in the flip
  trace.
- Scripts and outputs: `research-data/sv-hunt-20261125/analysis-run5/` (list at the end). One full xperf export was
  parsed once into numpy arrays (`np/`), and the 9.9 GB CSV was then deleted.

## Summary

1. **There are two kinds of slow frame, and neither is our pipeline being serialised behind the game thread.**
   Over the whole capture (4-162 s, 37,209 frames) there are 999 frames with a submit gap over 5 ms:
   - **88% are presenter-paced** (882 frames). The game thread is only 73% busy, and stage A, stage B and the
     recorder sit idle waiting for the present.
     - The present comes late because the **NVIDIA Reflex sleep** (on the driver's thread) runs **3.4 ms instead
       of 2.4-2.6 ms**.
     - The driver thread's spin after that sleep is unchanged (~1.2 ms).
   - **11% are game-thread-bound** (110 frames: wave 1 at 8-11 s, the burst at 112 s). The game thread `main` is
     99-100% busy for the whole 5.5-7 ms frame.
     - Stage A waits 2.7 ms per frame for the guest's submissions.
     - Stage B and the recorder sit idle 2.2-2.9 ms per frame.
     - The GPU gets no new work in that time.
2. **What our pipeline does to the game: per-frame handoffs, but not to the game thread.**
   - The game thread barely waits on the render thread, the RHI thread or any of ours: 0.02-0.10 ms per frame.
   - The game's render and RHI threads are gated every frame through a chain:
     - the guest submit thread waits for our EOP (stage A), 1.8-3.4 ms per frame;
     - the RHI thread waits on it, 2.2-3.3 ms;
     - the render thread waits on the RHI thread, 1.0-3.3 ms.
   - Stage A in turn waits on the presenter: the display-buffer label (`WaitRegMem` on a VO label). It drains the
     pipe and sleeps until the presenter has presented the previous frame: 1.9-2.6 ms per frame whenever the frame
     is not game-bound.
   - So the frame period equals the Reflex sleep schedule plus ~0.3 ms. This is the cap working as designed, but
     every extra 1 ms of Reflex sleep becomes 1 ms of frame time.
3. **Stage A is not the limit.**
   - It runs 1.4-1.6 ms per frame when calm and 2.0-2.7 ms in waves. More than 3.5 ms happened in only 10 of
     37,209 frames, all game-bound frames whose submissions arrived late in a burst (maximum 5.5 ms in a 6.9 ms
     frame).
   - Its growth in waves is per-draw work: `Rasterizer::Draw` 0.46 to 1.0-1.2 ms per frame, `CollectRingBindings`
     0.22 to 0.54, `CopySparseSerial` 0.08 to 0.17-0.25, `PostDraw`, and prepared-pipeline lookups (`UsedPrepared`
     0.25 to 0.48).
   - Draws per frame are not in the capture. By recording time (stage B `DrawRecord` 1.24 to 2.47 ms per frame) the
     waves carry about **2x the calm draw load**.
4. **GPU usage dips.**
   - In game-bound frames the GPU must idle: no new work is recorded for 2.2-2.9 ms of a 5.5 ms frame.
   - In presenter-paced frames the CPU side records about the same work as in ok frames, yet waits about 0.9 ms
     longer for Reflex.
     - If the GPU work per frame is unchanged, that is ~0.9 ms of GPU idle per slow frame. Ten to thirty such frames
       a second take GPU usage from the high 90s into the low 90s.
     - If it is not unchanged, the GPU got heavier and Reflex is right to wait.
   - This capture cannot tell the two apart (no GPU events). The next capture needs `JitterGpu` or
     `GOW3_FLIP_TRACE_GPU_READY=1`.
5. **Mouse.**
   - The raw reader costs ~1% of one core (11 ms/s at ~670 wake-ups/s while moving, 10 us per wake-up). Three
     quarters of its wake-ups land on idle CPUs. It took 0.13 ms/s from the critical threads in total.
   - The USB driver's interrupts (Wdf01000, ~7,600/s while moving, all on CPU 0) take 4-18 us per frame from each
     critical thread. Neither can make a 1 ms wave.
   - But **mouse movement goes with presenter-paced slow frames**:
     - 9.1 per second in seconds with the mouse moving fast, 0 in still seconds (correlation +0.48);
     - the game thread's load rises only a little (3.46 vs 3.02 ms per frame), stage A's and the recorder's barely.
   - So camera motion mainly changes what the present / Reflex side sees: GPU cost of motion (motion blur is at 4),
     or Reflex's reaction to it.
   - Also: **the 166 ms + 57 ms hitch at 2.4 s** is the presenter blocked on a win32k resource lock. The window
     thread was in a 119 ms cross-process call at the moment the reader started waking (mouse capture). Medium
     confidence.

Top candidate fixes (details in section 6):
1. A/B the limiter in the same wave scene: Reflex vs Precise (a setting, no code).
2. Give Reflex true markers, or measure GPU time per frame first.
3. Reduce the game-thread bursts: UObject create / hash churn with a host mutex per object, see section 1.4.

## 1. Question 1: critical path per frame

### 1.1 Threads (run 5 tids)

Roles come from sampled functions. Guest threads were matched to run 2's named threads (cosine similarity of their
guest-function profiles, `roles.py`). The RHI / submit pair was settled by who wakes whom (section 1.3).

| Role | tid | Busy (whole capture) | Notes |
|---|---|---|---|
| game thread `main` | 8324 | 123k samples | starts at `WinMainCRTStartup` |
| `RenderThread 1` | 19340 | 50k | |
| `RHIThread` | 29288 | 33k | notes the flip trace's `submit` stamp (the thread running at the stamp) |
| `SubmitGPUCommandsAsyncTaskThread` | 27188 | 3k | waits for our EOP |
| `AK::EventManager` (Wwise) | 15056 | 50k | off the path |
| stage A `Liverpool::Process` | 14020 | 66k | |
| stage B `DrawPipe::Run` | 15052 | 97k | |
| VkRecorder | 26352 | 98k | |
| presenter (`VideoOutDriver::PresentThread`, inline present + Reflex sleep) | 14136 | 9k | |
| NVIDIA driver thread (`nvoglv64.dll`) | 14188 | 48k (~0.3 core) | 92% in an `NtYieldExecution` loop; signals the presenter after the Reflex sleep |
| 9 draw-prep workers | 18360, 18080, ... | 15k each | off the path (0.1 ms per frame on it) |
| mouse reader (`reader_thread`) | 16592 | 1k | |
| window thread (`WindowSDL::PollEvents`) | 18596 | 0.7k | |

### 1.2 The frame loop, from ready events (`timeline.py`)

The same loop every frame, calm or wave. Example: calm, 54.000 s + ms:

```
 3.347  timer      -> NVDRV    (Reflex sleep ends, DelayExecution 3.00 ms)
 4.038  NVDRV      -> PRESENTER (after ~0.7 ms of yield-spinning; presenter waited 3.70 ms)
 4.048  [present_start 176]   4.195 [presented 176]
 4.198  PRESENTER  -> A        (A waited 1.72 ms: display-buffer label of frame 177's tail)
 4.207  A -> B, 4.234 B -> REC (both waited ~1.5 ms: drained, nothing to do)
 4.294  A -> SUBMIT            (guest submit thread waited 2.96 ms for our EOP)
 4.351  SUBMIT -> RHI          (RHI waited 2.52 ms)
 4.413  [irq 177]  4.479 [queued 177]   4.562 PRESENTER -> NVDRV (Reflex sleep for 177 starts)
 4.932  RHI -> RENDER          (render thread waited 3.04 ms)
 ...    stage A decodes frame 178's scene; B -> A drain wakes ...
 7.406  timer -> NVDRV ... 8.280 NVDRV -> PRESENTER ... 8.424 presented 177 -> A ...
```

- **A frame's tail waits for the previous frame's present.** UE4 writes the display buffer late in the frame, after
  a `WaitRegMem` on the VO label.
- At that packet `liverpool.cpp:1859-1887` drains the draw pipe and sleeps in `WaitVoLabel` unless the label is
  already met (two display buffers: frame N's buffer is free once N-1 is presented).
- The present happens after the Reflex sleep. So the whole chain hangs off the Reflex schedule:
  - stage A, B and the recorder;
  - then our EOP;
  - then the guest submit, RHI and render threads.
- The game thread is not in this chain. It runs a frame ahead and, at frame end, waits on the render thread only
  briefly.

### 1.3 Who waits on whom, ms per frame (`waitmatrix.py`; waits split by the thread that readied the waiter)

| Wait (waiter <- waker) | calm 52-58 s | calm 101-106 s | wave 1 ok / slow (8-11 s) | wave 4 ok / slow (150-161 s) |
|---|---|---|---|---|
| game `main` <- task-graph HP worker | 0.46 | 0.42 | 0.13 / 0.19 | 0.27 / 0.33 |
| game `main` <- render thread | 0.00 | 0.00 | 0.02 / 0.06 | 0.00 / 0.00 |
| render <- RHI | 2.94 | 2.86 | 1.03 / 1.34 | 2.54 / 3.29 |
| render <- game `main` | 0.00 | 0.04 | 0.48 / 0.50 | 0.10 / 0.11 |
| RHI <- guest submit thread | 2.59 | 2.61 | 0.99 / 1.10 | 2.24 / 2.94 |
| RHI <- render (many short) | 0.54 | 0.54 | 1.56 / 2.26 | 0.67 / 0.78 |
| guest submit <- **stage A** (our EOP) | 2.98 | 2.99 | 1.88 / 1.81 | 2.80 / 3.42 |
| **stage A <- presenter** (display-buffer label) | 2.04 | 2.33 | 1.05 / 1.13 | 1.55 / 2.24 |
| stage A <- guest submit (no work) | 0.00 | 0.03 | 0.19 / 0.71 | 0.03 / 0.06 |
| stage A <- stage B (drains) | 0.49 (2.8x) | 0.35 | 0.79 / 0.76 | 0.66 / 0.68 |
| stage B <- stage A (packet ring) | 1.85 | 2.14 | 0.95 / 1.47 | 1.36 / 2.07 |
| recorder <- stage B | 1.79 | 2.06 | 0.89 / 1.46 | 1.27 / 2.00 |
| presenter <- NVIDIA driver thread (Reflex) | 0.49* | 0.70* | 0.43 / 0.52 | 0.33 / 0.51 |

\* Only waits that started inside the frame window are counted. Section 3 has the whole Reflex sleep per present.

Running time (ms per frame), same columns:

| Thread | calm 52-58 | calm 101-106 | wave 1 ok / slow | wave 4 ok / slow |
|---|---|---|---|---|
| game `main` | 2.98 | 3.31 | 4.31 / 5.03 | 3.84 / 4.34 |
| render | 1.17 | 1.17 | 2.56 / 2.84 | 1.49 / 1.53 |
| RHI | 0.88 | 0.84 | 1.19 / 1.20 | 1.10 / 1.10 |
| stage A | 1.63 | 1.45 | 2.44 / 2.73 | 2.05 / 2.13 |
| stage B | 2.38 | 2.06 | 3.59 / 4.02 | 3.00 / 3.14 |
| recorder | 2.38 | 2.09 | 3.56 / 3.95 | 2.97 / 3.08 |

### 1.4 What bounded each slow frame (`classify.py`)

Per frame (submit to submit):
- **GAME**: stage A did not wait on the presenter (< 0.15 ms) and the game thread was >= 85% busy.
- **PRESENT**: stage A waited >= 0.5 ms on the presenter.
- **MIXED**: neither.

Columns:
- main%: the game thread's busy share of the frame;
- render: the render thread's running ms;
- rhiW: the RHI thread's ms waiting for the guest submit thread (our EOP);
- A<-SUB, A<-VBL: stage A's ms waiting for guest submissions / for the presenter;
- B<-A, REC<-B: stage B's / the recorder's idle ms;
- A run: stage A's running ms.

| Window | frames > 5 ms | GAME | PRESENT | main% (slow) | A<-SUB | A<-VBL | REC<-B | A run |
|---|---|---|---|---|---|---|---|---|
| whole capture 4-162 s | 999 of 37,209 | 110 | 882 | 99.5 (GAME) / 72.9 (PRESENT) | 2.74 / 0.05 | 0.00 / 2.64 | 2.26 / 2.41 | 2.21 / 2.11 |
| wave 1, 8-11 s | 105 of 635 | 37 | 66 | 99.2 / 87.4 | 1.88 / 0.04 | 0.00 / 1.80 | 1.31 / 1.55 | 2.98 / 2.59 |
| wave 2, 37-40 s | 93 of 658 | 0 | 93 | 71.2 | 0.04 | 2.17 | 1.92 | 2.11 |
| burst, 111.5-112.5 s | 51 of 220 | 48 | 3 | 99.7 | 3.30 | 0.00 | 2.90 | 1.82 |
| wave 4 (constant mouse), 150-161 s | 145 of 2,484 | 0 | 144 | 83.1 | 0.06 | 2.26 | 2.01 | 2.13 |
| moving, 18-26 s | 147 of 1,781 | 0 | 147 | 61.5 | 0.04 | 2.04 | 1.78 | 2.10 |
| calm 52-58 / 101-106 / 128-132 s | 0 / 0 / 1 | | | (70-78 all frames) | 0.02-0.03 | 2.04-2.33 | 1.79-2.06 | 1.45-1.63 |

Slowest frames, as examples:

- **Game-bound** (gap ms; main%; ms A waits for submissions; ms the recorder idles):
  - 8.6657 s: 7.25 ms gap; main 100%; A waits 2.44; recorder idle 1.71.
  - 112.0007 s: 6.48 ms gap; main 99.8%; A waits 4.29; recorder idle 3.09.
- **Presenter-paced** (gap ms; main%; rhiW; A<-VBL):
  - 157.9659 s: 5.84 ms gap; main 81%; RHI waits 3.81 on our EOP; A waits 2.78 on the presenter.
  - 8.5128 s: 6.74 ms gap; main 68%; rhiW 2.93; A<-VBL 2.90.

### 1.5 Answer to "is our pipeline serialised behind the game thread?"

- **No for the game thread.** It waits on render / RHI / our threads 0.02-0.10 ms a frame. When it is the limit
  (GAME frames), our threads are the ones waiting:
  - stage A 1.9-3.3 ms per frame for submissions;
  - stage B and the recorder 1.3-2.9 ms idle.

  That idle time is caused by the game thread, not created by our pipeline.
- **Yes for the game's render / RHI side, by design.** These are synchronous handoffs every frame, from the guest's
  point of view:
  - the submit thread waits for our EOP: 1.8-3.4 ms;
  - the RHI thread waits for the submit thread: 2.2-3.3 ms;
  - the render thread waits for the RHI thread: 1.0-3.3 ms.

  They all hang off stage A, and stage A hangs off the presenter through the display-buffer label. At the cap this
  is the pacing itself. It costs the game nothing while the render thread has slack (1.2-1.5 ms of work per
  frame). It becomes the frame time when the presenter is late, which is the PRESENT class.
- Within our pipeline:
  - stage A <- stage B drains: 0.35-0.8 ms per frame, 2.4-2.8 drains;
  - stage B <- stage A and recorder <- stage B: 1.3-2.1 ms idle per frame.

  These are waits for work, not a bottleneck: stage B runs 2.1-4.0 ms per frame.

## 2. Question 2: stage A's bursts

Profiles: `stageA-fn.txt` (1 kHz samples; ms per frame, inclusive).

| Function (inclusive) | calm 52-58 | calm 101-106 | wave 1 ok | wave 1 slow | wave 4 ok | wave 4 slow |
|---|---|---|---|---|---|---|
| sampled total | 1.53 | 1.38 | 2.43 | 2.71 | 2.00 | 2.12 |
| `Liverpool::ProcessGraphics` | 1.23 | 1.12 | 1.98 | 2.26 | 1.60 | 1.66 |
| `Rasterizer::Draw` | 0.55 | 0.46 | 1.02 | 1.20 | 0.81 | 0.87 |
| `CollectRingBindings` | 0.28 | 0.22 | 0.53 | 0.54 | 0.44 | 0.49 |
| `PipelineCache::UsedPrepared` (prepared-pipeline lookups) | 0.26 | 0.25 | 0.39 | 0.48 | 0.29 | 0.26 |
| `GetGraphicsPipeline` / `RefreshGraphicsKey` / `RefreshGraphicsStages` | 0.17 / 0.16 / 0.14 | 0.17 / 0.16 / 0.14 | 0.21 / 0.17 / 0.15 | 0.28 / 0.19 / 0.16 | 0.16 / 0.13 / 0.11 | 0.15 / 0.12 / 0.12 |
| `PostDraw` (self) | 0.11 | 0.09 | 0.17 | 0.26 | 0.14 | 0.10 |
| `CopySparseSerial` | 0.11 | 0.08 | 0.17 | 0.25 | 0.18 | 0.23 |
| `cached_region` / `runtime_memory_region` / `runtime_memory_clamp` (region lookups) | 0.06 / 0.05 / 0.04 | 0.04 / 0.04 / 0.04 | 0.09 / 0.07 / 0.10 | 0.05 / 0.07 / 0.06 | 0.09 / 0.07 / 0.08 | 0.08 / 0.08 / 0.06 |
| `DrawPipe::SpinUntil` (drain spin) | 0.08 | 0.09 | 0.08 | 0.08 | 0.09 | 0.10 |

- **Busy or waiting?** Both, depending on the frame class.
  - In GAME frames stage A is busy in bursts: the submissions arrive late and together. In between it is starved:
    A<-SUB 1.9-3.3 ms per frame.
  - In PRESENT frames it runs 2.1 ms and then sleeps on the display-buffer label: 2.2-2.6 ms per frame.
  - It ran over 3.5 ms in only 10 frames of the capture, all GAME / MIXED, mean gap 6.45 ms, maximum 5.5 ms.
  - The "4-6.7 ms per frame" peaks are those burst frames. Stage A works off a late burst there; it is not
    limiting.
- What grows is per draw: `Draw`, ring bindings, prepared-pipeline lookups and sparse copies. All of it scales with
  draws, not with a single slow call.
- **Draws per frame** are not in this capture. Proxies, as calm to wave-1-slow ratios: stage A `Rasterizer::Draw`
  2.2-2.6x, stage B `DrawRecord` 1.24 to 2.47 ms (2.0x).

## 3. Question 3: GPU usage dips

No GPU timestamps exist in this run (Jitter profile; no `gpu_ready`). What the capture does show:

- **Reflex sleep per present** (`reflex.py`). The sleep is the driver thread's `DelayExecution`. The spin is its
  yield loop until it readies the presenter.

  | Window | interval (ms) | driver sleep | spin until presenter woken | queued -> present_start |
  |---|---|---|---|---|
  | calm 52-58 s | 4.25 | 2.41 | 1.29 | 3.79 |
  | moving 31-37 s, ok / slow | 4.36 / 5.20 | 2.56 / **3.39** | 1.24 / 1.22 | 3.89 / 4.69 |
  | wave 4 150-161 s, ok / slow | 4.40 / 5.17 | 2.59 / **3.39** | 1.22 / 1.20 | 3.90 / 4.68 |
  | wave 2 37-40 s, ok / slow | 4.47 / 5.20 | 2.75 / **3.54** | 1.17 / 1.11 | 4.00 / 4.74 |

  The extra time of a presenter-paced slow frame is all in Reflex's sleep (+0.8-1.0 ms); the spin is constant.
  Reflex decides it. Our markers (`vk_swapchain.cpp:237-248`) set SimulationEnd, RenderSubmitStart / End and
  PresentStart at the same instant, right before the present. So Reflex sees ~0 ms of CPU work per frame, while the
  real frame takes ~4 ms on the CPU side.

  In low-latency mode Reflex sleeps so that the next submission meets an empty GPU queue. Two readings fit:
  1. The GPU got heavier when the camera moved (section 4), and Reflex correctly slowed to it.
  2. Reflex waited for GPU-queue drain that our pipeline then refills late, because the next frame's tail is only
     decoded after the present (section 1.2): a bubble every frame.

  The second makes the GPU idle by construction; the first does not.
- **Recorded GPU work per frame** (recorder running ms): calm 2.1-2.4, wave 4 ok 2.97 / slow 3.08, wave 1 ok 3.56 /
  slow 3.95. In presenter-paced slow frames the recorded work is the same as in the ok frames around them.
- **GPU idle, estimated:**
  - Game-bound frames (gap 5.5-6.5 ms):
    - stage B and the recorder sit idle 2.2-2.9 ms per frame;
    - calm frames hold the 4.22 ms cap, so the GPU work of a frame is under 4.22 ms;
    - so the GPU idles at least 1.3-2.3 ms per such frame (16-46 frames a second in wave 1 / the 112 s burst).
  - Presenter-paced frames: if the GPU work equals that of the neighbouring ok frames, the idle is ~0.8-0.9 ms per
    slow frame. At 10-28 such frames a second that is 1-3% of GPU time, which fits "dips into the 90s".
- **NVIDIA kernel DPCs per frame** (`gpu-gaps.txt`; nvlddmkm, 11-13 per frame): the longest gap between them is
  1.4-1.8 ms in calm and ok frames and 1.7-2.8 ms in slow ones. Too indirect to call GPU idle; listed only for
  completeness.
- **To settle it:** the next capture with `wpr-jitter.wprp!JitterGpu` (DxgKrnl queue packets: GPU busy / idle per
  frame), or `GOW3_FLIP_TRACE_GPU_READY=1`, in the same mouse-wiggle wave.

## 4. Question 4: the mouse

### 4.1 The reader thread in the capture (`mouse.txt`, `mouse2.txt`, `irq-steal.txt`)

- **tid 16592**, start `reader_thread`. 72,815 runs in 162 s:
  - CPU total 1,081 ms (6.7 ms/s; 10-12 ms/s while moving);
  - run length median 10 us, p99 244 us, max 765 us.
- Wake-ups:
  - ~670/s while the mouse moves: the 1 ms high-resolution waitable timer of `2c41b0b` gives ~1.5 ms periods;
  - 0 when still: `bf244ca` works, with no idle polling.
  - The log's session line: 2,713 reports/s, 370 wake-ups/s average.
- Priority at switch-in: 12 (73%), 11, 10 (normal, with boosts). Ready-to-run delay: median 6 us, p99 23 us.
- Cores: spread over all 16 CPUs, CPU 0-2 most. 53,666 of 72,815 switch-ins (74%) went onto an **idle** CPU.
- **Displaced a runnable critical thread**, over the whole capture:

  | Thread | times | total |
  |---|---|---|
  | game `main` | 187 | 8.5 ms |
  | stage B | 171 | 5.6 ms |
  | recorder | 138 | 4.0 ms |
  | stage A | 133 | 0.8 ms |
  | render | 83 | 0.5 ms |
  | RHI | 63 | 0.4 ms |

  That is about 0.12 ms per second in all: negligible. It mostly displaces draw-prep and task-graph workers.
- **No lock or atomic contention with the game thread** shows in samples:
  - the game thread's runtime samples hold no `runtime_mouse` function;
  - the reader's counters sit on their own line since `bf244ca`.
- **USB interrupts** (Wdf01000.sys, the mouse's KMDF stack):
  - 7,566 ISR+DPC events/s and 46 ms/s while moving, 213/s still. Correlation with the reader's wake-ups: 0.82.
  - All on **CPU 0**, which also takes dxgkrnl's 88 ms/s.
  - The critical threads run 3-11% of their time on CPU 0, so Wdf01000 takes 4-18 us per frame from each:
    game 17.6, recorder 8.7, stage A 8.4, render 6.2, RHI 5.3, stage B 3.9 us.
  - Measurable, far below a wave.

### 4.2 Frame chain with the mouse moving vs still (`per-second.txt`; 4-162 s)

| Seconds by reader wake-ups | n | gap | main% | main ms | render | A run | REC run | A<-VBL | slow GAME/s | slow PRESENT/s |
|---|---|---|---|---|---|---|---|---|---|---|
| moving >= 500/s | 78 | 4.38 | 79.0 | 3.46 | 1.41 | 1.88 | 2.75 | 1.78 | 0.24 | **9.09** |
| some 50-500/s | 67 | 4.30 | 76.3 | 3.28 | 1.34 | 1.74 | 2.51 | 1.95 | 1.27 | 2.31 |
| still < 50/s | 13 | 4.25 | 71.1 | 3.02 | 1.21 | 1.73 | 2.56 | 1.86 | 0.46 | **0.00** |

Neighbouring seconds:

| Seconds | Mouse | main ms | recorder ms | slow PRESENT frames per second |
|---|---|---|---|---|
| 27-29 | still | 2.96-3.14 | 2.96-3.26 | 0 |
| 31-37 | moving | 2.99-3.52 | 2.97-3.21 | 5-25 |
| 52-57 | still | | | 0 |
| 61-62 | moving | | | 2-9 |

Correlation with the reader's wake-ups:
- slow PRESENT frames +0.48;
- game-thread ms +0.41;
- stage A and recorder run +0.31;
- slow GAME frames -0.04.

**Reading:**
- Moving the mouse turns the camera. The CPU side does a little more (+0.4 ms game thread), but the slow frames it
  brings are **presenter-paced**: Reflex sleeps longer.
- So the mouse mainly acts through something the presenter / Reflex sees:
  - GPU cost of camera motion: `motion_blur=4` in the log; motion blur and the temporal upscaler cost more when
    every pixel moves; occlusion changes;
  - or Reflex's frame-to-frame reaction to it.
- Repeated small movements switching that cost on and off fit the oscillation the user can induce. This is
  inferred; the GPU data to confirm it is missing.

### 4.3 Static review of the per-read costs (game thread)

- `sceMouseRead` (`runtime_services.c:666-702`):
  - release build: a relaxed load (a store only for the first 3 reads), a 40-byte memset;
  - `runtime_mouse_take`: 3 `atomic_exchange` (locked xchg) and at most one `fetch_add`;
  - `clock_gettime(CLOCK_MONOTONIC)` (QPC, user mode).

  No syscall, lock or allocation. About 100 ns.
- `camera_look` (`probe.c:942-1040`, once per camera tick): `clock_gettime`, 2 `atomic_exchange`, a 16-entry class
  scan, `medievil_camera_rotation_local` (3 byte compares), `gow3_mouse_look` (3 relaxed atomic loads). The 5 s
  summary is dev-build only. No syscall, lock or allocation.
- `mouse_lock_record` (the `AddControllerYawInput` wrapper): pointer compares and `clock_gettime`.
- The reader per active millisecond, all on its own thread:
  - `GetRawInputBuffer` (at least 2 calls: until empty);
  - 2 `PeekMessageW`;
  - `SetWaitableTimer` + `WaitForSingleObject`;
  - `GetQueueStatus` only after an empty drain.

  About 6 system calls per wake-up, ~10 us.
- Window thread: `SDL_PollEvents` every 2 ms (`gow3gpu.cpp:348`, `SDL_Delay(2)`). Capture changes call
  `SDL_SetWindowMouseRect` (ClipCursor) and `runtime_mouse_capture` (`GetRegisteredRawInputDevices`, printf + fflush)
  on events only.

### 4.4 The hitch at 2.4 s (one-off, but mouse-related)

- Frames of 166 ms and 57 ms at ETW 2.40 / 2.47 s. Stage A waited 163 ms and then 56 ms on the presenter.
- The presenter itself waited in **WrResource** (a kernel resource lock, win32k) four times: 86 ms readied by
  explorer.exe, 9 ms, 24 ms (msedgewebview2), 11 ms (dwm), then 53 ms readied by **our window thread** 18596.
- The window thread was in a **119 ms + 25 ms cross-process call** (WrLpcReply) and then `WrResource` waits, at the
  same moment.
- The reader's first wake-ups are in the same second (0/s at 1 s, 217/s at 2 s): the mouse capture started there.
- So: the presenter's `vkQueuePresentKHR` (fullscreen, win32k) blocked behind the window thread's cursor / capture
  calls, while the shell (explorer / WebView2 / dwm) held the lock. [medium: the log lines have no timestamps to tie
  the capture to 2.24 s exactly]

## 5. What is ours vs the game's

| Item | Owner | Size |
|---|---|---|
| Game-thread bursts (UE4 task graph, PhysX, Blueprint; at 112 s UObject create / hash: guest 0xb085b0 `AllocateUObjectIndex` "Unexpected concurrency while adding new object", 0xb09c70 UObject hash add / remove with `scePthreadMutexLock`; our host mutex HLE 0.3-0.7 ms per frame in that burst) | game, plus our lock HLE | GAME class, 11% of slow frames |
| Reflex sleep length (driver), with our markers all at present time | ours (markers, choice of limiter) and the driver | PRESENT class +0.8-1.0 ms, 88% of slow frames |
| Display-buffer label gating: a frame's tail waits for the previous present; stage A drains and sleeps | ours (`liverpool.cpp` VO label path) mirroring the console | 1.9-2.6 ms per frame of pipeline idle; turns Reflex sleep into frame time |
| Guest render / RHI waiting for our EOP | game structure (UE4 PS4 RHI) on our timing | 2.2-3.4 ms per frame, not limiting while the game thread runs ahead |
| Stage A per-draw cost | ours | 1.5 to 2.7 ms per frame, never the limit here |
| Mouse reader, USB interrupts | ours / the system | < 0.02 ms per frame on the critical threads |
| Presenter blocked by win32k at capture change | ours (window thread) plus shell | 166 + 57 ms once |

## 6. Candidate fixes, ranked (proposals only, no code; each needs the user's go and one change per launch)

1. **Limiter A/B in the wave scene: Reflex vs Precise at 237** (setting only).
   - Expected: if Reflex's adaptive sleep makes the PRESENT class, the 88% share of slow frames shrinks and the GPU
     dips flatten.
   - Measure: the flip trace plus `JitterGpu`, and the user's mouse-wiggle test.
   - Risk: low. Precise was rejected earlier for flicker at 119 (T-F27), so judge flicker too.
   - Variant: Reflex with the sleep after the present (`GOW3_REFLEX_SLEEP_BEFORE_PRESENT=0`). Then the present
     (and the display-buffer label) is not held back by the sleep.
2. **Measure GPU time per frame before changing Reflex** (`JitterGpu` or `GOW3_FLIP_TRACE_GPU_READY=1`).
   - It settles reading 1 vs 2 of section 3: whether the GPU really gets heavier when the camera moves.
   - Then, if Reflex is wrong, **true Reflex markers**:
     - SimulationStart at the guest's frame begin (the game thread's frame start, or `sceGnmSubmitDone` /
       `BeginFrame`);
     - RenderSubmitStart at stage A's first packet of the frame;
     - RenderSubmitEnd at `queued`.
   - Expected: Reflex learns the real CPU time (~4 ms) and stops sleeping for a GPU-queue drain that our pipeline
     then refills late.
   - Risk: medium (pacing; T-F tests showed the limiter is sensitive). If the GPU really is heavier, the gain is in
     GPU settings (motion blur 4 when the camera moves) instead.
3. **Game-thread bursts** (GAME class; the 112 s burst; P18 items 4-6).
   - Fewer per-object host mutex calls in UObject create / hash: the guest `scePthreadMutexLock` HLE took 0.3-0.7 ms
     per frame in the burst.
   - Fewer path UObjects per tick: `FindPathToLocationSynchronously` creates one per call, per
     study-npc-tick-rate.md.
   - Expected: the game-bound waves shrink.
   - Risk: medium (game behaviour). It needs the inclusive cost by guest frame pointer walk (study-sv-hotspots.md 8.4).
4. **Display-buffer label without stalling the CPU pipeline.**
   - Today an unmet VO-label wait drains stage B and sleeps stage A until the present. Alternatives:
     - let stage A go on decoding the following packets and enforce the wait on the GPU or at the recorder (a
       timeline semaphore signalled at present);
     - or give the guest a third display buffer, if the game's `sceVideoOutRegisterBuffers` count can be raised.
   - Expected: the pipeline stops going idle 1.9-2.6 ms every frame. The next frame's tail is recorded and queued
     before the present, which removes any per-frame GPU refill bubble.
   - Risk: high:
     - the label means "buffer not on screen"; writing it early tears or corrupts the image;
     - the decode-pacing heap-safety argument depends on stream order.
   - Do it only if items 1-2 show a refill bubble.
5. **Window thread and present on win32k.** No cursor / capture calls (`SDL_SetWindowMouseRect`, `printf`/`fflush`
   in `runtime_mouse_capture`) on the window thread while a frame is presenting, or defer them. Expected: no
   repeat of the 166 ms hitch at capture changes. Risk: low. Value: rare events only.
6. **Mouse reader: leave as is.** About 1% of a core, 74% of wake-ups on idle CPUs, no measurable effect on the
   critical threads. The USB interrupt load on CPU 0 is the system's (an affinity / interrupt-steering question,
   not the port's).

## 7. Limits of this study

- Thread roles are inferred (no ETW thread names for the port's threads). The RHI and submit threads were told apart
  by their wake patterns.
- A ready event's "readier" is the thread running when the ready was logged. Timer expiries show the interrupted
  thread, so small "readied by X" entries for unrelated threads are timers. The tables use the dominant pairs only.
- "slow" = submit gap > 5 ms. The PRESENT / GAME split uses thresholds (A<-VBL 0.15 / 0.5 ms, main 85%).
- Stage A function shares are 1 kHz samples, about 2,000-6,000 samples per window.

## Files (`research-data/sv-hunt-20261125/analysis-run5/`)

| File | What |
|---|---|
| `parse_full.py` | full xperf export (no symbols) to `np/cs.npy`, `rd.npy`, `smp.npy`, `stk.npy`, `dpc.npy`, `meta.pickle` (game CSwitch / ReadyThread / samples with stacks, all DPC / ISR) |
| `r5.py` | loader: run intervals, waits with wakers, symbols (runtime base 0x7ff65e4f0000) |
| `flip_overview.py` / `flip-overview.txt`, `frames.pickle` | flip trace per ETW second, frames in ETW time |
| `roles.py` / `roles.txt`, `roles.json`, `roles-extra.json` | thread roles |
| `critpath.py` / `critpath.txt` | first backward wait-graph walk (confused by the driver's timer-woken thread; superseded by classify) |
| `waitmatrix.py` / `waitmatrix.txt` | section 1.3 |
| `timeline.py` / `tl-calm1.txt`, `tl-wave4.txt` | section 1.2 |
| `classify.py` / `classify.txt`, `classify-top.txt` | section 1.4 |
| `thread_fn.py` / `stageA-fn.txt`, `stageB-fn.txt`, `recorder-fn.txt`, `vblank-fn.txt`, `rhi-fn.txt`, `main-fn.txt`, `render-fn.txt` | profiles |
| `reflex.py` / `reflex.txt`, `reflex-full.txt` | section 3 |
| `gpu_gaps.py` / `gpu-gaps.txt` | nvlddmkm DPC gaps, recorder idle |
| `mouse.py`, `mouse2.py`, `irq_steal.py`, `per_second.py` / `mouse.txt`, `mouse2.txt`, `irq-steal.txt`, `per-second.txt`, `mouse-per-second.npy`, `per-second.npy` | section 4 |
