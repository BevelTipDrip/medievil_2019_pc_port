# Study: frame pacing under G-Sync / VRR, and where it goes in this pipeline

Status: study, 2026-10-23. Nothing built, run or committed. I re-analysed the existing flip traces and ran
`vulkaninfo` (read only). Parent: [START-HERE.md](START-HERE.md). Background:
[plan-stage-b-split.md](plan-stage-b-split.md), sections T-L2 to "Presentation settings in the launcher".
Code paths are relative to `gpu/shadps4/` unless stated. D = `core/libraries/videoout/driver.cpp`,
P = `video_core/renderer_vulkan/vk_presenter.cpp`, S = `video_core/renderer_vulkan/vk_swapchain.cpp`.

**Evidence labels.** **[src]** = stated by a primary source (list at the end). **[trace]** = measured in our flip
traces now (scripts in the session scratchpad, numbers below). **[local]** = this PC's driver (`vulkaninfo`,
driver 616.92). **[inf]** = my inference, to be tested. **[com]** = community or forum sources, or search snippets
only. The Blur Busters, Guru3D and Special K pages returned 403 or did not render.

## Summary

1. **The unreproducible flat run (T-V120) is explained by the trace itself [trace].** In `flip-trace-8.csv`,
   from ~240 s on:
   - **The present call shrinks steadily.** It took ~4.0 ms and fell by ~0.5 ms every 20 s (a ~25 ppm drift
     between `steady_clock` and the display).
   - **Waves, then a reset.** At ~1 ms the 12.5 / 4.17 ms waves start. Then the call jumps back up by one 240 Hz
     refresh (to ~4 ms). The period is ~160 s.
   - **Queued to presented stays at ~8.2 ms throughout.** So the display held back the presents (FIFO: the
     acquire / present blocked). That back-pressure reached the guest through the 3-frame pool (`GetRenderFrame`,
     P:907-915).
   - **Locked share.** Inside the blocking phase, 97-100% of intervals were within 8.33 +- 0.1 ms. Before 240 s
     (call 0.3 ms) it was 13%.
   - **Interpretation [inf].** The window was on a **fixed 240 Hz refresh (VRR not engaged)**, with **one image
     queued in FIFO** acting as a jitter buffer. The queue filled during the catch-up after the 218 ms hitch. It
     then persisted because input rate = output rate. Later runs never filled the queue (with G-Sync engaged
     there is no fixed refresh to queue behind).
2. **The 120 Hz tick already paces the game's frame start (front edge) [trace].**
   - **Timing.** The game submits ~1.2 ms after each tick. Stage B reaches the flip 0.26 ms after the previous
     release. The frame then waits ~7.8 ms for the next tick (traces 10, 13).
   - **With `GOW3_PRECISE_TICK=1` (T-V120p).** Release intervals were p1/p99 8.32 / 8.42 ms. Present calls in the
     steady windows had a worst-1% average of 8.8 ms (113 FPS: what RTSS showed).
   - **So the overlay's "80s" lows come from after `vkQueuePresentKHR`, or from the missed-tick episodes [inf].**
     T-V120p had 16.7 ms intervals in two 15 s windows (60-90 s).
   - **12.5 ms is a 240 Hz grid number.** A worst-1% average of 12.5 ms is exactly 80 FPS, what alternating
     12.5 / 4.17 ms pairs on a fixed 240 Hz grid give. That is what a composed or not-VRR presentation would
     produce **[inf]**.
3. **The decisive missing measurement is the display time.** This driver exposes **`VK_EXT_present_timing`**
   with the `IMAGE_FIRST_PIXEL_OUT` stage, plus `VK_KHR_present_wait2`, `VK_NV_low_latency2`,
   `VK_NV_present_metering` and `FIFO_LATEST_READY` **[local]**. **PresentMon** gives the same display times from
   outside, with no code change (`PresentMode`, `MsBetweenDisplayChange`).
4. **Ranked plan.**
   1. **Measure first.** PresentMon alongside a 120-tick run (no code change).
   2. **Raise the present / pacer / tick threads' priority** (MMCSS). `SetCurrentThreadRealtime` is a no-op on
      Windows (`common/thread.cpp:76-78`).
   3. **A deliberate "display-locked" fixed-refresh mode** (the T-V120 state on purpose).
   4. **A VRR mode:** precise front-edge tick plus GPU-ready gating, with display-time feedback from
      `VK_EXT_present_timing`.
   5. **Reflex (`VK_NV_low_latency2`)** as the VRR limiter.

   Details and first experiments are at the end.

## What the traces show (new analysis, no launch)

| Trace (run) | Mode | Release interval p1 / p99 | Present call | Presented worst 1% | Note |
|---|---|---|---|---|---|
| 8 (T-V120), 0-220 s | 120 tick (AccurateTimer), FIFO | 7.1 / 9.6 | 0.30 ms | - | locked 11-15% |
| 8 (T-V120), 300-380 s | same | 8.26 / 8.42 (presented) | 3.8 -> 1.8 ms, falling linearly | ~8.4 ms | locked 97-100%: display-paced |
| 8, wave bins (e.g. 420-460 s) | same | 4.1 / 12.6 (presented) | ~1 ms, then jumps to ~4 ms | 12.5 ms | 400-600 pairs of 12.5 / 4.17 ms per 20 s |
| 10 (T-V120p) | 120 tick, `GOW3_PRECISE_TICK=1` | 8.32 / 8.42 | 0.27 ms (8.54-8.92 after the tick) | 8.8 ms in steady bins | 24 + 19 intervals of 15-20 ms in the 60-90 s bins |
| 13 (T-V120c, G-Sync on) | 120 tick, AccurateTimer | 7.12 / 9.64 | ~0.3 ms | - | ticks +-1.2 ms (AccurateTimer) |

Stage offsets from the previous frame's release (ms, p5 / median / p95):

| Trace | submit | irq | queued | release | presented |
|---|---|---|---|---|---|
| 10 (precise tick) | -7.48 / -7.10 / -6.54 | 0.20 / 0.26 / 0.36 | 0.28 / 0.34 / 0.70 | 8.33 / 8.33 / 8.33 | 8.54 / 8.60 / 8.92 |
| 13 (AccurateTimer) | -8.00 / -7.04 / -6.07 | 0.21 / 0.25 / 0.35 | 0.39 / 0.47 / 0.69 | 7.55 / 8.27 / 9.20 | 7.95 / 8.74 / 9.68 |

What this means [trace + inf]:

- **Two ticks of latency.** The guest's command stream for frame N completes only after frame N-1's flip completes
  (its last pass waits for the display-buffer label, which `CompleteFlip` resets: D:871-908). So in tick mode the
  game runs one frame ahead and is held at the label. The present of frame N comes ~1 tick plus 0.27 ms after its
  final pass, so latency is ~2 ticks from the game's submit.
- **Both edges are already tick-locked in tick mode.** The game's start is paced at the vblank event (D:1167-1194,
  fired right after the release). The present is paced at the next tick. The only jitter left on our side is the
  tick itself (fixed by `GOW3_PRECISE_TICK`) and the swap-thread hand-off (+0.2-0.6 ms, `RunPresenter` D:96-127).
- **The release does not check that the GPU has finished the frame** (D:1150-1168 takes the request as is). That is
  probably harmless at 50% scale (the frame was recorded ~8 ms earlier), but unproven at 4K in heavy views.

## The pipeline and its pacing points

| # | Stage | Thread / code | What paces it today |
|---|---|---|---|
| 1 | Guest vblank event (UE4 frame start, `rhi.SyncInterval 1`) | `shadPS4:PresentThread`, D:1113-1121 (tick), D:1176-1193 (event) | tick: AccurateTimer (D:1119), `PreciseWait` (D:1116), or DXGI `WaitForVBlank` (D:1114) |
| 2 | Game CPU frame, `sceGnmSubmitAndFlip` | guest | the vblank event, the flip label |
| 3 | PM4 processing, recording (stage A/B) | GPU threads; `SubmitFlip` D:926, `SubmitFlipInternal` D:953 | `PrepareFrame`: frame pool (P:907-915) and `GOW3_FRAMES_AHEAD` (P:538-557) |
| 4 | Flip request queued | D:972-976 | - |
| 5 | Release (limiter / tick / pacer) | PresentThread D:1138-1168 / D:1200-1226; Pacer D:808-859 | frame limit, precise slot, newest-frame pacer |
| 6 | `CompleteFlip` (flip event, label) | in `Flip`, D:716-718 (at release, not at display) | - |
| 7 | Present | `gow3:Present` (SwapThread D:111-127): acquire P:713, blit and submit P:842-848, `vkQueuePresentKHR` P:849-855 | FIFO back-pressure (acquire blocks), image count = minImageCount + 1 = 3 (S:305) |
| 8 | DXGI layer -> DWM / independent flip -> display | NVIDIA driver, Windows | VRR, or a fixed refresh plus FIFO |

The flip trace (`gpu/shim/gow3_flip_trace.h`) covers stages 2-7. Stage 8 is invisible to it. `displayed` via
`VK_KHR_present_wait` returned at the present on NVIDIA (T-V120b; S:34-60, `vk_instance.cpp:325-332`).

## State of the art (by technique)

| Technique | What it does | Established [src] | Availability here |
|---|---|---|---|
| **NVIDIA Reflex limiter** (`VK_NV_low_latency2`) | **What it does.** `vkLatencySleepNV` returns at once; the app then waits on its timeline semaphore "before input sampling", once between presents. That makes it a **front-edge** limiter. **Settings.** `VkLatencySleepModeInfoNV{lowLatencyMode, lowLatencyBoost, minimumIntervalUs}`: `minimumIntervalUs` is the minimum time between `vkQueuePresentKHR` calls, enforced by the sleep, even when low-latency mode is off. **Markers.** `SIMULATION_START` goes after the sleep; `RENDERSUBMIT_*` must not span asynchronous rendering; `PRESENT_START/END` go around `vkQueuePresentKHR`. They carry an **app-defined frame id**, not related to `VkPresentIdKHR`. **Requirements.** `VK_KHR_present_id` or `present_id2`, and `VkSwapchainLatencyCreateInfoNV`. **Reports.** `vkGetLatencyTimingsNV` gives per-frame sim, submit, present, driver, OS queue and GPU times. **Auto-cap.** NVIDIA: with G-Sync + V-Sync on, Reflex caps "below the refresh rate, preventing VSYNC backpressure" (225 at 240 Hz [com]). NVCP V-Sync "will only work for Fullscreen applications". | [src] Khronos refpages; NVIDIA latency guide; NVIDIA engineer on the dev forum: marker "ordering ... does not matter" | **Exposed by 616.92 [local]** (since 551.70). The sleep and the markers can come from different threads (frame-id keyed) [inf]. NVIDIA has not said whether it works with the DXGI-layered swapchain [gap]; one user report of Reflex not capping on Vulkan [com]. |
| **RTSS limiter** | All modes wait inside the hooked Present. **Front edge** waits just before the real Present: presents are flat, content timing floats, latency is higher. **Async / back edge** wait after Present, so the frame start is aligned: start-to-start flat, presents jitter, lower latency. Back edge runs on a fixed clock and resyncs after a stutter; async drifts. **Reflex mode** (7.3.5+) drives NVIDIA's limiter by injection. **Scanline sync** is for fixed refresh. Latency: RTSS adds up to ~1 frame, the NVCP limiter ~2; in-game limiters ~0. | [com] Unwinder on Guru3D; Blur Busters G-SYNC 101 p11 | External. It times our present call, not the game's start. |
| **Special K** | The limiter splits idle time between waiting for vblank (stability) and delaying the next frame's start (latency): "Delay Bias", default 0% = stability. Reflex "Low Latency" mode caps a few FPS below refresh; Auto VRR sets the cap. Kaldaien: for least OLED VRR flicker the "Normal" limiter is best; Low Latency (VRR) can flicker slightly even at stable FPS. Latent Sync works only in independent flip or MPO, not composed. | [src] wiki snippet; [com] Blur Busters forum | SK hooks DXGI; with a layered Vulkan swapchain, unverified. The lesson carries over: stability-biased pacing flickers least. |
| **AMD Anti-Lag 2 / Intel XeLL** | Same shape as Reflex: `Update(ctx, enable, maxFPS)` / `xellSleep(frameId)` before input; XeLL has a `minimumIntervalUs` cap and sim / render / present markers. XeLL is D3D12 only. | [src] GPUOpen; Intel XeSS docs | Not on NVIDIA. |
| **DXGI frame-latency waitable** | `SetMaximumFrameLatency` (default 1, only with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`). The handle "signals when the DXGI adapter has finished presenting a new frame". The app waits on it before starting any rendering each frame, so back-pressure moves to the front edge. Gets "down to 1 frame of latency" in independent flip. | [src] Microsoft Learn | Not exposed through Vulkan. Our accidental equivalent is the FIFO acquire plus the frame pool (T-V120). |
| **Flip model, independent flip, MPO** | Flip-model swapchains in a borderless window covering the output can be promoted to **independent flip** (DWM bypassed; VRR and tearing possible), or composed through DWM. "Hardware Composed: Independent Flip" uses MPO planes. Overlays, a non-covering window and some scaling cause demotion. Windows 11's "Optimizations for windowed games" upgrades legacy blt-model windowed games to flip model. VRR in windowed mode needs flip model, plus `DXGI_PRESENT_ALLOW_TEARING` for the tearing / VRR path. | Microsoft DirectX blog, Learn | NVIDIA's "Vulkan/OpenGL present method: layered on DXGI swapchain" makes our Vulkan presents DXGI flip-model presents, so PresentMon sees them and the promotion rules apply [src: NVIDIA driver notes; inf for the details]. |
| **Present timing in Vulkan** | `VK_EXT_present_timing` (Vulkan 1.4.335). **Needs** `present_id2`, `get_surface_capabilities2`, `calibrated_timestamps`. **Feedback** (`vkGetPastPresentationTimingEXT`): queue-ops-end, request-dequeued, first-pixel-out, first-pixel-visible. **Target times** (`targetTime`, absolute or relative) **in FIFO modes only**. `vkGetSwapchainTimingPropertiesEXT`: `refreshInterval == UINT64_MAX` means VRR. Supersedes `VK_GOOGLE_display_timing` (Android / Swappy). `VK_KHR_present_wait2` / `present_id2`: a wait per present id. | [src] Khronos proposal and refpages; NVIDIA Vulkan driver page (present_timing since 581.89, present_wait2 since 573.38) | **616.92 exposes all of them [local].** The surface reports `presentTimingSupported`, `presentAtRelativeTimeSupported = true`, `presentAtAbsoluteTimeSupported = false`, stages `QUEUE_OPERATIONS_END` + `IMAGE_FIRST_PIXEL_OUT`. Caveat: this was vulkaninfo's own 256x256 window, so re-query the game's surface. NVIDIA present wait v1 gives no display information: T-V120b [trace]; a 3060 forum report found the blocking in `vkQueuePresentKHR` instead [com]. |
| **OLED VRR flicker** | Gamma is tuned for the native refresh, so VRR away from it shifts gamma; it shows most in dark scenes. The larger the frame-rate swings, the more flicker. WOLED gamma tracks the rate; QD-OLED spikes randomly, more at low FPS. The LFC boundary (and loading screens) gives "a fairly significant and jarring change". Mitigation: stable FPS, a cap, staying off the VRR / LFC boundaries. | [src] TFTCentral; RTINGS | Our capped modes had small present spreads, so the flicker the user sees means the display's intervals are not even [inf], which ties back to Summary 2. Loading hitches (16.7-200 ms frames) also cross the LFC boundary. |
| **"Cap a few FPS below refresh"** | G-Sync + NVCP V-Sync on + cap >= 3 FPS below the maximum refresh. 2 below was enough; lower does not help. Preference: in-game limiter, then RTSS (~1 frame of extra latency), then NVCP (~2). Reflex auto-cap: 138 at 144 Hz, ~225 at 240 Hz (fits `refresh - refresh^2/3600` [inf]). | [com] Blur Busters G-SYNC 101 (snippets); [src] NVIDIA latency guide | A 120 cap on a 240 Hz panel is far below the ceiling, so this rule is not what hurts us. |
| **Front-edge pacing and animation error** | Motion looks smooth when **each frame's content time step equals its display time step** (Croteam, "The Elusive Frame Timing", GDC 2018: measure when frames reach the display, not CPU time; they used `VK_GOOGLE_display_timing`). PresentMon: `MsAnimationError(N) = (AnimationTime(N) - AnimationTime(N-1)) - MsBetweenDisplayChange(N)`, with CPU start (or XeLL / Reflex sim start) as the animation time. Swappy uses presentation timestamps so frames are not shown early. Pacing the simulation start with a fixed delta, then presenting at a fixed latency, gives zero error; back-edge-only pacing gives even presents of unevenly timed content. | [src] PresentMon docs / GamersNexus write-up; Phoronix on Croteam; Android docs | The 120 tick is front-edge pacing. Newest-frame mode (T-L5) is the opposite case: its motion error was 3x T-L4's [trace]. |

## Mapping onto this pipeline

| Option | Where | Replaces | Prerequisites | Risks | Success measure |
|---|---|---|---|---|---|
| **M. PresentMon capture** (no code) | external, `PresentMon-2.5.1-x64.exe --process_name medievil-runtime.exe --qpc_time --v1_metrics` (and a v2 run) | - | admin (one UAC prompt); the exe at `C:\mgspf_tools\pw\presentmon\` (run it or a copy, output to `research-data\`, never write into `C:\mgspf_tools`) | none | `PresentMode` (Hardware: Independent Flip vs Composed: Flip), `MsBetweenDisplayChange` histogram (continuous = VRR; multiples of 4.167 = fixed 240 Hz), `MsUntilDisplayed` (queued images), `SyncInterval`, `AllowsTearing` |
| **T. MMCSS priority for the present path** | `SetCurrentThreadRealtime` Windows branch (`common/thread.cpp:76-78`): `AvSetMmThreadCharacteristicsW(L"Games")` + `AvSetMmThreadPriority(HIGH)`, or `THREAD_PRIORITY_TIME_CRITICAL`; plus a call in SwapThread (D:111) | today a no-op | `avrt.lib` | starving stage A/B on the 8 cores (T-L1b showed spin cost) | flip trace: release -> presented spread (today +0.2-0.6 ms) |
| **F. Display-locked mode (fixed refresh, G-Sync off)** | tick = half the refresh (120 at 240), FIFO, with the queue deliberately filled once: present the first frame twice after a hitch, or run the tick ~0.05% fast so the swapchain fills, then let the acquire hold it (back-pressure through the pool, P:907-915). Better: derive the tick from presentation progress (release the guest's vblank when acquire returns), so it cannot drift. | the PC-clock tick in this mode | G-Sync off for the game (NVCP per-app), VSync on | +1 refresh of latency (4.17 ms); the guest stalls if the pool empties | T-V120's 97-100% locked for a whole session, no waves; PresentMon `MsBetweenDisplayChange` = 8.333 exactly |
| **G. GPU-ready release (VRR)** | before `Flip(request)` (D:1167 / D:1225): `presenter->WaitFrameReady(frame, slot + margin)` (P:863), with a trace stage `gpu_ready` | release at the tick regardless of the GPU | none | a not-ready frame becomes a late frame (it already would be at the display) | new `gpu_ready` stage ahead of `release` for >99.9% of frames; overlay lows |
| **E. Present-timing feedback** (`VK_EXT_present_timing`) | enable in `vk_instance.cpp` beside the present-wait block (325-332); swapchain flag; query `IMAGE_FIRST_PIXEL_OUT` per present in `Swapchain::Present` (S:163-201), written as trace stage `displayed` | `GOW3_DISPLAY_TRACE` present wait | headers v363 have it (msys2 `vulkan_core.h`) [local]; check the game surface's caps at runtime | enabling present ids changed the present spread in T-V120b: keep it off unless tracing | real display intervals in our own trace, aligned with release |
| **E2. Relative-time presents** | `presentAtRelativeTime`: ask for each image at least 8.33 ms after the previous one | our pacer's slot timing | same as E; FIFO only (spec); G-Sync on | unknown driver behaviour under VRR | `displayed` interval spread < +-0.1 ms |
| **R. Reflex (`VK_NV_low_latency2`)** | **Swapchain:** `VkSwapchainLatencyCreateInfoNV` + present id enabled (S:89 create, S:171-189). **Cap:** `vkSetLatencySleepModeNV(minimumIntervalUs = 8333)`. **Sleep:** `vkLatencySleepNV` + wait on its semaphore **in the PresentThread right before the guest vblank event (D:1176)**, which is the simulation start. **Markers** with our own frame id (the guest `flip_arg`): `SIMULATION_START` at the event, `RENDERSUBMIT_START/END` at `irq` / `queued` (D:928, D:975), `PRESENT_START/END` around `presentKHR` (S:184). | the tick wait (D:1113-1121) in this mode | `VK_KHR_present_id`; the extension [local]; G-Sync + NVCP V-Sync on as NVIDIA recommends | the guest sees one vblank per frame (fine for UE4 at SyncInterval 1; the movie player and the 16 ms menu redraw D:1149 need a check); present ids changed timing before | NVIDIA overlay shows "PC latency" (Reflex active); `vkGetLatencyTimingsNV` gives per-frame sim / present / GPU times |
| **N. Native vs layered present** | NVCP present method per app | - | - | T-V120d: no difference seen | PresentMon `PresentMode` changes |

## Specific questions

**(a) Pacing the frame start (front edge).** Already done by the 120 Hz tick. The traces show the game's submit
locked to the tick (p5-p95 within 0.9 ms with the precise tick). This is the right model (Croteam, Reflex) [src].
A 480 Hz tick plus a present-side limiter (T-L2-T-L4) paces only the back edge: the game starts at the first
480 Hz tick after its flip completes, with 0-2.08 ms of phase error, so its content steps are uneven even when
presents are even [inf]. **Keep the 120 tick as the capped mode.** The remaining work is in stages 7-8, not the
front edge.

**(b) `VK_NV_low_latency2`.**
- **Availability.** Exposed by driver 616.92 [local]. The NVIDIA DXGI layering is the default present path, so
  Reflex must work through it. NVIDIA ships Reflex in Vulkan games on the same default [inf: no Windows-layering
  caveat in the spec].
- **Our separate present thread is acceptable.** Markers carry an app-defined frame id [src], and an NVIDIA engineer
  says their ordering does not matter [src, forum]. The spec has no single-thread rule [src]. The sleep goes at
  the guest vblank (our stage 1); the present markers go on the swap thread [inf]. Caveat: Reflex's
  `minimumIntervalUs` is defined as the time between presents, which is what the sleep enforces [src].
- **What it would add over the precise tick.** NVIDIA's limiter knows the GPU's and the display's state, plus the
  automatic below-refresh cap. Its cap is a front-edge sleep like our tick, so for even 120 FPS it should do no
  better than the precise tick unless the display-side issue is GPU-side [inf].
- **Verdict.** Medium value, medium effort. Do it after the measurement shows where the display intervals go
  wrong.

**(c) A frame-latency-waitable equivalent for Vulkan.**
- **What gives back-pressure today.** (1) The acquire in FIFO, which blocked in T-V120 and propagated through the
  3-frame pool (proven in trace 8: queued -> presented constant). (2) `GOW3_FRAMES_AHEAD` (P:538-557), which is
  GPU-completion only.
- **Why present wait cannot replace them.** "Wait for present N-1 displayed" (`vkWaitForPresentKHR`) returns at
  the present on NVIDIA [trace].
- **Waiting on display feedback instead.** `VK_KHR_present_wait2` / `present_timing`'s `FIRST_PIXEL_OUT` should
  give the real point; verify with E. A wait on "frame N-2's first pixel out" before releasing the guest's vblank
  = `SetMaximumFrameLatency(2)` semantics.
- **The heavyweight alternative.** Our own D3D12/DXGI swapchain fed through Vulkan external memory, which gets the
  real waitable object plus `GetFrameStatistics`. High effort [inf].

**(d) The flat run.** Explained above (Summary 1).
- **Present-call fit.** The present call falling linearly, then jumping by one refresh, is the signature of a FIFO
  queue at depth on a **fixed** refresh, drifting ~25 ppm against our tick [trace].
- **Composition vs independent flip.** Composed vs independent flip alone would not produce a blocking present.
  A queued image is the mechanism. DWM / fixed refresh is the precondition [inf].
- **Why not reproduced.**
  - Runs with G-Sync confirmed on have no fixed grid to queue behind.
  - G-Sync-off runs (g, h) never had a catch-up burst to fill the queue. With ~1.2 ms tick jitter (AccurateTimer)
    the phase was random, so frames were not held back.
  - In T-V120 itself, "G-Sync state unverified": most likely VRR was not engaged for the window during that run.
- **How PresentMon would confirm it.**
  - `MsUntilDisplayed` ~ 8-12 ms (one queued image).
  - `MsBetweenDisplayChange` = 8.333 exactly in the flat phase.
  - 12.50 / 4.17 ms in the waves.
  - `PresentMode` stays "Hardware: Independent Flip" or "Composed: Flip" with `SyncInterval 1`.
- **Reproducing it on purpose.** That is option F.

**(e) Recommended modes.**
- **VRR users (G-Sync on):**
  - **Uncapped by default** (the user's best feel). The panel follows the game, and frame-time variance at 200+
    FPS is small in absolute terms.
  - **Capped: 120 Hz precise tick** (`present_tick_hz=120`, `present_precise_tick=1`). Add, once built: MMCSS on
    the tick / swap threads (T), GPU-ready gating (G), and present-timing feedback (E) to check display intervals.
    Optional: Reflex (R) replacing the tick.
  - **Not:** the 480 tick + present limiter, newest-frame mode, or `GOW3_VBLANK_SYNC`.
- **Fixed-refresh users (or G-Sync off):**
  - Tick = refresh / n, FIFO, with the queue kept at one image (F, display-locked: T-V120's flat state on purpose).
  - Without F: the precise tick at refresh / n, FIFO, accepting the waves.

## Ranked options

| Rank | Option | Benefit | Effort | Risk | Why here |
|---|---|---|---|---|---|
| 1 | M: PresentMon with the current 120 precise-tick setup | decisive (tells VRR vs fixed grid, composed vs independent, queue depth) | none (no code) | none | every later choice depends on stage 8 |
| 2 | T: MMCSS / time-critical for PresentThread, Pacer, SwapThread | small-medium (+-0.3 ms hand-off, missed-tick tails) | small | low | `SetCurrentThreadRealtime` is a Windows no-op today |
| 3 | E: `VK_EXT_present_timing` display stage in the flip trace | high for diagnosis (our own display times) | medium | low (trace-only) | present wait v1 gave nothing on NVIDIA; this driver has first-pixel-out |
| 4 | F: display-locked fixed-refresh mode | high for fixed-refresh users (T-V120: best ever) | medium | medium (latency +1 refresh, pool stalls) | proven state, now understood |
| 5 | G: GPU-ready release + `gpu_ready` stage | medium at 4K / heavy views | small | low | the release ignores GPU completion today |
| 6 | R: Reflex via `VK_NV_low_latency2` | medium (latency, NVIDIA's VRR cap, PCL in the overlay) | medium-high | medium (vblank semantics, present ids) | front-edge pacing is already done by the tick |
| 7 | E2: relative-time presents | unknown | medium | unknown under VRR | a driver-side pacer if E shows the display drifts from our presents |
| - | Own DXGI swapchain via interop | high control | high | high | only if 1-6 fail |

## First experiments (one change per launch, the user's go each time)

1. **X1, measure only.**
   - **Settings.** Launcher > Presentation: tick 120, precise vblank tick on, VSync on, frame trace on, G-Sync on
     (indicator on). 50% scale like T-V120.
   - **Capture.** Start PresentMon (an admin console, before or during the run):
     `PresentMon-2.5.1-x64.exe --process_name medievil-runtime.exe --output_file C:\Users\Ty\MediEvil\research-data\pm-x1.csv --qpc_time --v1_metrics --stop_existing_session`.
   - **Read.**
     - `PresentMode`.
     - Histogram of `MsBetweenDisplayChange`: continuous around 8.33 = VRR working; a 4.167 grid = not VRR.
     - Its worst 1% vs the overlay's.
     - `MsUntilDisplayed` vs our flip trace's release -> presented.
   - **Second capture.** Repeat with G-Sync off in the same session (still one change per launch) for the
     fixed-grid reference.
2. **X2 (code: T).** MMCSS on the three threads, behind `GOW3_MMCSS=1`; same settings as X1. Look at the flip
   trace's release -> presented p99 and the 15-20 ms interval count per minute.
3. **X3 (code: E).** `GOW3_PRESENT_TIMING=1`: a `displayed` stage from `IMAGE_FIRST_PIXEL_OUT`. A/B the present
   spread with it on vs off, since present ids changed it before.
4. **X4 (code: F)**, if X1 shows a fixed grid or the user wants a G-Sync-off mode: the display-locked tick.
   Target: T-V120's flat line for a whole session.

## Sources

- Khronos, `VK_NV_low_latency2` and its structures:
  https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_low_latency2.html ,
  .../VkLatencySleepModeInfoNV.html , .../vkLatencySleepNV.html , .../VkLatencyMarkerNV.html ,
  .../VkSetLatencyMarkerInfoNV.html , .../VkLatencyTimingsFrameReportNV.html
- NVIDIA: NvAPI sleep params https://docs.nvidia.com/nvapi/struct__NV__SET__SLEEP__MODE__PARAMS.html ; Vulkan driver
  support https://developer.nvidia.com/vulkan-driver ; latency guide
  https://www.nvidia.com/en-us/geforce/guides/system-latency-optimization-guide/ ; marker ordering
  https://forums.developer.nvidia.com/t/reflex-marker-association-across-a-low-latency-off-on-transition/380728 ;
  present wait on Windows https://forums.developer.nvidia.com/t/presentkhr-still-blocks-on-windows-even-when-using-vk-khr-present-wait/251142
- Khronos, `VK_EXT_present_timing` proposal: https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_present_timing.html ;
  availability: https://vulkan.gpuinfo.org/listextensions.php?platform=windows
- RTSS modes (Unwinder): https://forums.guru3d.com/threads/is-someone-able-to-explain-how-async-and-back-edge-sync-differ-thanks.447580/ ,
  https://forums.guru3d.com/threads/understanding-fes-mode-is-front-edge-sync-mode-visually-smoother-looking-than-async-back-edge.456412/
- Special K: https://wiki.special-k.info/Advanced/Video ; OLED flicker and limiter modes https://forums.blurbusters.com/viewtopic.php?p=113783 ;
  Latent Sync https://forums.blurbusters.com/viewtopic.php?p=74924
- AMD Anti-Lag 2: https://gpuopen.com/learn/integrating-amd-radeon-anti-lag-2-sdk-in-your-game/ ; Intel XeLL:
  https://github.com/intel/xess (doc/xell_developer_guide_english.md)
- OLED VRR flicker: https://tftcentral.co.uk/articles/exploring-and-testing-oled-vrr-flicker ;
  https://www.rtings.com/monitor/learn/research/vrr-flicker
- Animation error: https://gamersnexus.net/gpus-gn-extras-cpus/problem-gpu-benchmarks-reality-vs-numbers-animation-error-methodology-white ;
  Croteam https://www.phoronix.com/news/Frame-Timing-Game-Stuttering
- Microsoft, frame latency waitable object: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject ;
  reduce latency with DXGI 1.3 swap chains: https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains
- Microsoft, flip model: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model ;
  VRR displays: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays ;
  "Demystifying Full Screen Optimizations": https://devblogs.microsoft.com/directx/demystifying-full-screen-optimizations/
- PresentMon (metrics, PresentMode values): https://github.com/GameTechDev/PresentMon (README-ConsoleApplication.md)
- Blur Busters, G-SYNC 101: https://www.blurbusters.com/gsync/gsync101-input-lag-tests-and-settings/14/ (and /5/, /11/)
- Microsoft, VRR and Windows 11 windowed optimizations: https://support.microsoft.com/en-us/topic/3f006843-2c7e-4ed0-9a5e-f9389e535952
- Croteam (Alen Ladavac), "The Elusive Frame Timing", GDC 2018: https://www.gdcvault.com/play/1025407/
- Android frame pacing (Swappy): https://developer.android.com/games/sdk/frame-pacing
- AMD Anti-Lag 2: https://gpuopen.com/amd-anti-lag-2/ ; Intel XeLL (XeSS 2 SDK): https://github.com/intel/xess
- Special K wiki (frame limiter, latent sync): https://wiki.special-k.info/
- RTSS (front-edge / back-edge sync, Reflex limiter mode): RTSS release notes by Unwinder,
  https://www.guru3d.com/download/rtss-rivatuner-statistics-server-download/
- Local: `vulkaninfo` (driver 616.92, RTX 4090), 2026-10-23. Traces: `C:\Users\Ty\MediEvil\research-data\flip-trace-{3,8,10,11,13,16}.csv`.
