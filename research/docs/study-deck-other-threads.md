# Study: CPU of the non-GPU threads on the Steam Deck, and what the port can cut

Status: study, 2026-11-04, on `deck-lab` (`6958db9`). Read only: nothing built, launched or committed. Parent:
[plan-linux-deck.md](plan-linux-deck.md), "Wait sites and per-thread CPU" and later. Inputs:
`research/docs/data/deck-waitsites-1.log` (the wait-site run), `data/deck-helpers-run1.log` (2 helpers),
the runtime sources (`src/runtime_*.c`), the GPU threads (`vk_scheduler.cpp`, `vk_draw_prep.cpp`, `gow3_threads.h`)
and the 01.02 eboot disassembly (`out/eboot.elf`; `guest+0x...` = ELF virtual address = the wait-site offset).

Marks: **[code]** read in this tree or in the eboot disassembly. **[log]** a number from a log in `data/`.
**[inferred]** reasoning or an estimate, not checked. Nothing here is "verified" in the project's sense.

## Answer

1. **Wwise.** `AK::EventManager` runs a plain loop: do the audio work, then wait on an event flag with no timeout
   (guest 0x3517b60, [code]). It does not poll. It wakes once per output buffer (93.75/s) plus about once per game
   frame: 94/s in menus, 157-159/s in gameplay at 64-66 FPS ([log]). That is the console's rhythm.
   `sceAudioOutOutput` blocks to a steady 10.67 ms period for Wwise's 512-frame port, as on PS4 ([code]).
   - The port's own audio work is small: the volume, layout and 7.1-to-stereo pass is ~2-3 ms/s, NGS2 is a memset,
     there is no resampling, and it runs on the guest output threads ([code], cost [inferred]).
   - The one port-side cost inside Wwise's CPU is **ATRAC9 decoded in software, synchronously, on the calling
     guest thread** (`sceAjmBatchStartBuffer`). On PS4 the ACP hardware does that decoding. Estimate: 0.03-0.15 of a
     CPU. It is not measured yet.
   - The 58% was sampled in the pinned-GPU-clock tail, where the CPU is capped near 2 GHz. The wait sites give at
     most ~28-31% of a CPU in gameplay at auto clocks, and ~46-52% in the tail ([log]).
2. **Main thread and RHIThread.** No HLE call the game makes at a high rate is expensive:
   - clocks go through the vDSO (a measured lock plus two clock reads averages 0.1 us);
   - mutexes and condition variables are glibc futexes, and uncontended mutexes cost 0.1-1 us;
   - TLS is a thread-local array;
   - `usleep(0)` runs 300-480/s;
   - no guest yield spin reaches the report.

   The RHIThread's 6.6k switches/s are the game's own blocking: task events, contended guest mutexes, and the RHI
   mutex held across the frame-EOP wait. What the port adds on these threads is **kernel work**:
   - the 32-48k tracker write faults/s;
   - on Linux, an `munmap` + `mmap` for every pool decommit and an `mmap` for every commit. There is no keep-views
     path on Linux (L2).

   The split between the game's own work and this kernel work is unknown. The decisive check is utime/stime per
   thread (section 5).
3. **Our threads.**
   - **Recorder.** It hands off a chunk every ≥32 KiB, about 110 times a frame. The recorder drains each chunk faster
     than stage B fills the next, so it **spins its full 20 us and then sleeps every time**: ~7.3k × 20 us ≈ 15% of a
     CPU wasted, plus a futex wake on stage B per chunk ([code] + [log] arithmetic).
   - **DrawPrep.** The 4 SCHED_IDLE workers spend ~0.76 CPU, about 8 us of worker CPU per prepared draw. That is as
     much as stage B's whole cost per draw (8.5 us). With 2 workers, 90% of draws still arrived prepared (94% with 4)
     ([log]).
   - **SCHED_IDLE on 4 SMT cores works against us.** It keeps the workers off the run queue but not off stage B's
     sibling hyperthread. Every guest wake-up preempts them (1.7k/s each). They also share one mutex with stage A and
     the guest submit thread, so a preempted worker can hold up the critical path ([code], effect [inferred]).
4. **Top cuts** (section 4):
   1. 2 prep workers on the Deck. An env/launcher setting, about -0.4 CPU.
   2. No recorder spin on 8 or fewer hardware threads, plus a larger hand-off. About -0.15 CPU and fewer wakes on
      stage B.
   3. Linux pool keep-views (L2). Kernel time and TLB shootdowns off the guest threads.

   An Ajm decode counter, then the FFmpeg ATRAC9 decoder, comes after these.

## 0. The measurement, and how far it carries

- The per-thread table in the parent doc (main ~1 CPU, `AK::EventManager` 58%, `RHIThread` 42%, `gow3:VkRecorder`
  63%, four `gow3:DrawPrep` ~19% each, 12 `TaskGraphThread` 5-20%) was **sampled in the pinned-GPU-clock part of
  the run**. The parent doc's "Pinned GPU clock" section says that state caps the CPU at ~2 GHz. The wait-site
  windows from that tail show it: `GPU thread 14.85 / 11.45 us/draw` against 8.2-8.9 elsewhere, and 34-52 FPS (log
  lines ~4928-5069) [log]. **Read every % as a low-clock number.** At auto clocks the same work is ~1.3-1.5x fewer %
  [inferred].
- The wait-site report (`src/runtime_waitsites.c:44-74`) is per **call site**, not per thread, and lists only the
  **16 sites with the most time** in each 5 s window. The 16th entry is ~6-8 ms per window. So any yield, sleep or
  wait site costing more than ~1.5 ms/s would appear. High-rate calls that never block (clock reads, TLS, trylock)
  never appear.
- Wait time includes the wait from wake-up to running (run-queue delay). So "1 − time waiting" is an **upper bound**
  on a thread's busy share.

## 1. Wwise

### 1.1 What `AK::EventManager` runs [code]

The only event-flag wait site, guest+0x3517baf, is the return address of `call 0x3bf6a80` in this loop
(guest 0x3517b60):

```
3517b6a  call 0x3bf34d0           ; result stored to a global (the audio thread id) [inferred]
3517b79  call 0x348fa70           ; thread init
3517b90  mov  rdi, [0x5c63a10]    ; loop:
3517b93  call 0x3468eb0           ;   the work function (Wwise's Perform) [inferred name]
3517b98  mov  rdi, [rbx]          ;   event flag handle
3517b9b  mov  esi, 1              ;   bits 1
3517ba0  mov  edx, 0x12           ;   WAIT_OR | CLEAR_PAT
3517ba5  xor  ecx, ecx            ;   result = NULL
3517ba7  xor  r8d, r8d            ;   timeout = NULL: no timeout
3517baa  call 0x3bf6a80           ;   sceKernelWaitEventFlag (the wait-site kind says so)
3517baf  cmp  byte [rbx+8], 0     ;   stop flag
3517bb3  je   loop
```

The loop matches Wwise's audio thread: do the work, then wait for the next signal. It never spins. With no timeout
there is no timeout polling either. The HLE event flag is a mutex plus condition variable
(`src/runtime_extra.c:80-88` set, `:129-157` wait).

### 1.2 Wake cadence vs the PS4 [log]

| Window (deck-waitsites-1.log) | FPS | waits/s at 0x3517baf | avg wait | busy share, at most |
|---|---|---|---|---|
| Menus (lines ~396-537) | 235-240 | 94 | 10.2-10.6 ms | 1-5% |
| Gameplay, graveyard (lines ~1720-4494) | 63-66 | 153-159 | 4.0-4.7 ms | 27-37% (most 28-31%) |
| Tail (lines 5307-5414) | 63-64 | 111-156 | 3.1-6.3 ms | 30-52% |

- **94/s = 48000 / 512.** Wwise's port 1 is opened with 512 frames ("audio port 1 opened (type 0, 8 ch, float, 512
  frames, downmixed to stereo)", followed at once by "guest thread 'AK::LibAudioOut' created", log 309-310). The audio
  thread wakes once per consumed output buffer.
- **157-159/s ≈ 93.75 + the frame rate.** At 55 FPS the rate was 149/s (log ~4695). The game signals the audio
  thread once per game frame as well [inferred: the UE4 Wwise integration's per-tick RenderAudio]. On the PS4 at
  30 FPS this would be ~124/s. The extra wakes on the Deck follow the higher frame rate. They process the message
  queue; they do not render audio buffers [inferred]. They are not a port bug.

### 1.3 Where each piece of the port's audio path runs

| Piece | Runs on | Code | Cost |
|---|---|---|---|
| ATRAC9 decode (LibAtrac9, doubles, scalar, `-O2`) | **the guest thread that calls `sceAjmBatchStartBuffer`**: synchronous, under one global recursive mutex. `sceAjmBatchWait` returns at once | `src/runtime_ajm.c:1-6`, `:75`, `:516-544` (`run_job` at `:537`), `:188-235`; `third_party/LibAtrac9/C/src/imdct.c:6-80`; `build.sh:88` | **Not measured.** Estimate 10-30 us per channel-frame (256 samples), so ~4-11 ms/s per stereo stream. With 10-20 streams, 0.03-0.15 of a CPU [inferred]. The calling thread is most likely `AK::EventManager` (Wwise drives the codec from its audio frame) [inferred] |
| MP3 decode (FFmpeg) | the same calling thread | `src/runtime_ajm.c:279-307`, `src/runtime_mp3.c` | Probably rare in MediEvil [inferred] |
| NGS2 | the caller of `sceNgs2SystemRender`: memset of the buffers only | `src/runtime_extra.c:360-365` | ~0 |
| `sceAudioOutOutput`: per-channel volume, PS4-to-SDL 7.1 order, float copy | the guest thread that outputs. Port 1 = `AK::LibAudioOut`. Port 0 (256 frames, 187.5/s, silence from the silent NGS2 path) = a UE4 audio thread [inferred] | `src/runtime_audio.c:200-210` | 4096 samples per call at 93.75 and 187.5 calls/s: ~0.5-1 ms/s per port [inferred] |
| 7.1 to stereo downmix + soft limiter | same thread | `src/runtime_audio.c:167-189`, `:252-253` | 48k frames/s per port, ~0.5 ms/s [inferred] |
| Resampling | none in the port: the streams are 48 kHz, as the guest's | `src/runtime_audio.c:102`, `:131` | 0 |
| SDL stream mix and device I/O | SDL's audio thread (host), plus the `pipewire-pulse` server outside the process | `src/runtime_audio.c:132-133`, `:235-256` | ~1-2% of a CPU [inferred] |
| Event flags | the waiting and setting threads | `src/runtime_extra.c:80-157` | ~160 wakes/s: negligible |

**Every Deck run used the PulseAudio backend through pipewire-pulse.** SDL's PipeWire backend fails inside the
AppImage: `[E] pw.loop ... can't make support.system handle`, then "audio backend pulseaudio" [log, every
`deck-*.log`]. It works, but it adds an extra hop through a server process.

### 1.4 Busy polling: none found [code]

- `sceAudioOutOutput` sleeps to an absolute deadline: `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`
  (`src/runtime_audio.c:55-63`). The period is frames / 48000 (`:199`), and the deadline advances one period per
  call (`:229-234`).
- The queue control adjusts the next deadline by ±period/32, about ±3%, from the minimum queue level over 32 buffers
  (`:240-251`). Prefill only adds silence when the queue drops below one buffer.
- `sceAudioOutOutput` returns immediately only on the first call, or after a stall of more than 8 periods, where it
  restarts the cadence (`:231`).
- `sceAudioOutOutputs` paces once per call for all of its ports (`:265-272`).
- **Result:** the port neither returns early nor blocks too briefly. The measured 94/s menu wake rate matches the
  PS4's hardware period exactly [log]. Wwise does not run its mixer loop more often than on the console.
- One hazard, not a CPU cost: the port lock is held across the period sleep (`:197`, `:232`, `:260`). A
  `sceAudioOutSetVolume` on the same port from another thread (`:281`) would block for up to 10.7 ms.

### 1.5 What in Wwise's CPU is the port's

- The work function is Wwise's own work: voices, effects, its pitch/resampling and the mix to the port's 7.1 layout.
  On PS4 that is the same code. Zen 2 at 2-3.5 GHz is roughly 2x+ a 1.6 GHz Jaguar per thread [inferred].
- The **ATRAC9 decode** is the console's ACP hardware work that the port moves onto the CPU. It is the only
  port-side term [inferred, see 1.3].
- ~~Wwise mixes 7.1 because the guest opens an 8-channel port. Making it mix stereo would require refusing that
  format, which is guest behaviour of unknown outcome. Not proposed.~~ **Corrected (2026-11-05,
  [study-wwise-stereo-mix.md](study-wwise-stereo-mix.md)):** Wwise already mixes **stereo**. Its PS4 sink takes
  the layout from `sceAudioOutGetPortState`'s `channel`, which the port reports as 2 for the 8-channel port; the
  frames arrive as `L R 0 0 0 0 0 0`. The mix term above is a stereo mix; there is no layout saving to take.
  The 7.1-to-stereo downmix in 1.3 runs over zeros in channels 2-7.

## 2. The main thread and RHIThread

### 2.1 HLE calls at high rate, and what each costs [code] / [log]

| Call (guest) | Rate (gameplay) | Host implementation | Cost |
|---|---|---|---|
| `scePthreadMutexLock`/`Unlock` (`guest+0x6be86d`, `0xf5d44e`, `0x70de9a`, `0x319c9d4` ...) | 70-100k/s in total; most 0.1-1.7 us avg including wait-site timing | glibc `pthread_mutex_lock`; default type ERRORCHECK (`src/runtime_mutex.c:59`, `:83-110`) | user-space CAS when uncontended. **Each lock and unlock also increments shared, non-atomic globals** `locks`/`unlocks` (`:15`, `:87`, `:108`). One cache line written from every core 150-200k times/s: ~0.01 CPU of line transfers [inferred] |
| Contended guest mutexes: `0x697cdb` (4.3-9.5k/s, 1.5-6.8 us), `0x739dab` (8.9k/s, 3.7 us), `0x6795a1` (56-80/s, 2.5-3.3 ms) | see left | futex sleep | Game design: `0x6795a1` is EndFrame blocking on the RHI mutex held across the frame-EOP wait ([study-eop-wait-site.md](study-eop-wait-site.md)) |
| `cond_wait` 0x70e02c, `cond_timedwait` 0x70df3a (UE4 `FEvent::Wait`; the deadline is built from a ms count, [code] at 0x70dee2-0x70df35) | 2.4-6.5k/s and 5.2-5.8k/s | glibc `pthread_cond_*` (futex). The Windows-only WaitOnAddress path is off on Linux (`:236-240`, `:241-262`) | One futex sleep and a context switch per wait. **The timed waits mostly expire:** actual ≈ requested ≈ 4.6-5 ms, ~27 thread-equivalents (5.8k/s × 4.6 ms). Likely UE4's idle `PoolThread` ×16 and `FImgMediaSchedulerThread` ×8 polling [inferred]. The same on PS4 |
| `sceKernelGetProcessTime(Counter)`, `clock_gettime` | not counted | `clock_gettime(CLOCK_MONOTONIC)` (`src/runtime_kernel.c:79-87`) | vDSO. A wait-site sample (lock + two clock reads) averages 0.1 us at 53k/s (log 402), so the Deck's clock source is not a syscall [log] |
| `sceKernelReadTsc` | not counted | `rdtsc` (`:88`) | ~10 ns |
| `pthread_getspecific` | not counted | a `_Thread_local` array (`:301-327`) | a few ns |
| `sceKernelUsleep(0)` | 300-480/s at guest+0x8da727 | `nanosleep` (`:109-123`) | 3-5 us each: <0.2% CPU [log] |
| `sched_yield`, `scePthreadYield` | below the report threshold | `sched_yield` (`:178`, `src/runtime_thread.c:345`) | no guest yield spin above ~1.5 ms/s [log] |
| `sceKernelUsleep` 8 ms, three 33 ms timers, vblank and flip equeue waits | 357/s, 3 × 30/s, ~300/s | `nanosleep`, equeue | blocking. The vblank is 240 Hz on the Deck ("VideoOut: vblank 240 Hz"), so `RHIFrameOffsetThread` wakes ~240/s instead of 60 [log]. Negligible |

None of these explains a full CPU on the main thread. They add up to a few percent of one CPU, spread over all
threads [inferred].

### 2.2 Kernel work the port puts on guest threads

- **Tracker write faults: 32-48k/s, about 760 a frame** (`Frame stats ... write faults/s` [log]). Each one costs:
  - a SIGSEGV delivery;
  - the handler (4.8-9 ms/s measured in total);
  - an `mprotect`, which sends TLB-shootdown IPIs to every CPU running the process.

  Native run 6 showed that these faults stretch the game's frames. The Deck answer is U5, or "hot pages" on the PC
  (parent doc). Which guest threads take the faults is not logged.
- **Pool commit/decommit on Linux: no keep-views.** `pool_keep_views()` returns 0 on Linux
  (`src/runtime_memory.c:1004-1010`). So every `sceKernelMemoryPoolDecommit` does an `munmap` and then an
  `mmap(PROT_NONE)` under the region write lock (`:1080-1085`, `:453-463`). Every commit maps again, and the next
  touch takes fresh page faults on the memfd.
  - On Windows, T-M1 counted ~1,100 decommits/s at ~60 us each before keep-access (`runtime_memory.c:415`, comment).
  - On Linux each `munmap`/`mmap` also takes `mmap_lock` for writing, which stalls concurrent page faults (including
    the tracker faults above), and sends shootdown IPIs.

  This is the parent's item L2. It is not done, and it is not counted on Linux (the host-map stats are
  Windows-only, `:433-436`).
- **Guest priorities are ignored** (`src/runtime_thread.c:4`, `:351-354`). Wwise's prio-256 threads, the game thread
  and the 60+ pool and task threads all run at nice 0, beside the port's threads.
- **`hw.ncpu` reports 7** (`src/runtime_kernel.c:274`), the PS4 game-core count. UE4 sizes some pools from its own
  PS4 constants: 12 task-graph threads (NP/HP/BP ×4), 16 PoolThreads and 8 FImgMedia threads, for 67 guest threads
  in all ([log]: "guest thread ... created" ×67). On 8 hardware threads, beside ~10 port threads.

### 2.3 RHIThread's 6.6k voluntary switches/s

That is about 100 per frame. The guest's own blocking explains them:
- task events (`cond_wait` 0x70e02c);
- the contended allocator-area mutexes `0x697cdb` and `0x739dab` [log; 0x697cdb sits beside the transient-pool
  allocator at 0x697e54 named in study-eop-wait-site.md];
- the RHI mutex at `0x6795a1`.

The port could make each of these cheaper: shorter lock hold times if fewer faults land inside critical sections,
or adaptive spinning for short sections (cut H). It cannot remove them.

## 3. Our threads

### 3.1 `gow3:VkRecorder`: 63%, 7.3k switches/s [code] / [log]

- Stage B hands a chunk to the recorder once ≥32 KiB are recorded (`vk_scheduler.cpp:491`). It wakes the recorder
  only if it sleeps (`:506-513`).
- The recorder spins `pause` for **20 us** when the process has fewer than 12 hardware threads, otherwise 200 us. It
  then sleeps on a condition variable (`:536-551`).
- 7.3k switches/s at 64-66 FPS is ~110 sleeps a frame, about one per chunk. Stage B takes ~8.5 us per draw, so a
  32 KiB chunk (a dozen or more draws) takes ≥100 us to fill. The recorder executes it faster. **It therefore finds
  the queue empty after almost every chunk, spins the full 20 us, and sleeps.**
- **Spin cost: ~7.3k × 20 us ≈ 146 ms/s, ~15% of a CPU**, a quarter of the recorder's 63%. The catch rate is near
  zero. Zen 2's `pause` is short, so the loop also takes issue slots from whatever runs on the sibling hyperthread
  [inferred].
- **Stage B cost:** one `notify_one` futex wake per chunk, ~7.3k/s × ~1.5-3 us ≈ 1-2% of stage B, which is the
  critical path [inferred].
- The rest of the 63% is real Vulkan recording on RADV. Recording on stage B instead (`GOW3_VK_RECORD_THREAD=0`)
  would put that work on the bottleneck. Keep the thread.

### 3.2 `gow3:DrawPrep` ×4: ~19% each, 1.7k involuntary switches/s each [code] / [log]

- **Cost per prepared draw:**
  - 0.76 CPU over ~92-98k prepared draws/s ("Draw preparation: 462257 prepared pipelines used, 28239 not (4
    workers)" per 5 s) is **~8 us of worker CPU per prepared draw**. Stage B's whole cost is 8.2-8.9 us per draw.
  - A prepared draw saves stage B only the sharp fetches, the texture-description hashing and the pipeline selection.
    The source comment puts the first two at ~7% of the GPU thread (`vk_draw_prep.cpp:89-90`), so the saving is
    roughly 1-2 us [inferred].
  - Each worker also re-applies the register deltas from its last position (`:437-451`). The scanner walks every
    buffer once more (`:384-386`).
- **2 workers are almost as good.** `deck-helpers-run1.log` (2 workers; dedicated stage B core in the same run, so
  confounded): 389-407k prepared vs 37-47k not per 5 s, **90% prepared** (94% with 4); stage B 8.4-8.8 us/draw, the
  same as with 4 [log].
- **Why SCHED_IDLE hurts here** (`gow3_threads.h:107-113`; `vk_draw_prep.cpp:330-342`):
  1. **SMT.** SCHED_IDLE only orders the run queue. With 8 logical CPUs on 4 cores at 70-80% load, the "idle"
     logical CPU is usually the sibling of a busy one, often stage B's. The worker then takes a share of stage B's
     core that no scheduler policy sees.
     - Evidence, confounded: with stage B's sibling kept free, stage B busy fell 79 → 70% (parent doc, "Earlier the
       same session").
  2. **Preemption churn.** Any normal wake-up preempts a SCHED_IDLE task at once. The guest alone makes
     ~10-15k wakes/s (timed waits, events, sleeps; 2.1), so each worker is preempted ~1.7k/s and resumes with cold
     caches.
  3. **Lock-holder preemption.** One `std::mutex` (`vk_draw_prep.h:148`) is shared by:
     - the workers and the scanner (`cv.wait` predicates scan the deque under it, `:412-432`, `:365-368`);
     - the **guest submit thread**: `Enqueue` under the GPU queue lock, `liverpool.cpp:2381-2392`,
       `vk_draw_prep.cpp:238-248`;
     - **stage A**: `BeginSubmission` per top-level DCB, `liverpool.cpp:1236-1240`, `vk_draw_prep.cpp:250-284`;
     - the end of each submission: `Collect`, `:286-325`.

     If a worker is preempted while it holds the mutex, the critical-path threads wait until a SCHED_IDLE thread runs
     again. SCHED_IDLE gets almost no share on a busy run queue [inferred: not measured, no lock timer exists].
  4. **Wakes everyone.** `notify_all` wakes all 5 helper threads on every enqueue, scan and rebase (`:247`, `:264`,
     `:278`, `:394`). They queue on the one mutex.
- The workers also fill otherwise-idle CPUs by design, which inflates the "CPU 70-80% on all threads" the user saw
  (the "fake" CPU), as do the recorder spin and stage A's yield-spin.

### 3.3 Others, briefly

- `shadPS4:GpuComm` (stage A) yield-spins about one core. That is covered by the drain-sleep and decode-pacing work in
  the parent doc.
- The Deck runs had no precise vblank tick ("VideoOut: precise vblank tick" absent [log]). The 240 Hz vblank uses
  AccurateTimer: no spin.

## 4. Ranked cuts for the Deck

The CPU figures are at the run's (capped) clocks, in fractions of one logical CPU. "Confirm" names one Deck run.
Section 5 has the per-thread sampler.

| # | Cut | CPU saved (est.) | Effort / risk | Confirm |
|---|---|---|---|---|
| A | **2 draw-prep workers on the Deck** (`GOW3_PREP_WORKERS=2`, or launcher helpers = 2; default `Available()/2` at `vk_draw_prep.cpp:50-57`). Try 1 too. Not 0: a prepared draw saves stage B ~1-2 us | ~0.35-0.4 total, plus less SMT pressure on stage B | none (setting) / L | One launch at 2 (no dedicated core), same spot as `deck-waitsites-1`: per-thread sampler (DrawPrep CPU, stage B's CPU); frame stats `GPU thread us/draw` (must not rise), prepared share in "Draw preparation", FPS |
| B | **No recorder spin on ≤8 hardware threads** (`spin_time` 20 → 0 us, `vk_scheduler.cpp:536-537`); then a **larger hand-off** on the Deck (32 → 64-96 KiB, `:491`) | spin ~0.15; 2-3x fewer futex wakes on stage B (~5-15 ms/s of stage B) | S / L (spin), S / M (hand-off: SyncRecording may wait longer) | Sampler: `gow3:VkRecorder` CPU (expect ~-15 points) and voluntary switches; frame stats `blocked: recorder` (0.1-0.3% today) must stay low; FPS |
| C | **Linux pool keep-views (L2):** keep the memfd mapping on decommit (mark the region reserved; drop access only if a test needs it), so a commit at the same address is a table update | unknown on the Deck. Windows had ~1,100 decommits/s × 60 us ≈ 0.07 before its fix; Linux adds `mmap_lock` stalls and refaults | S-M / M (pool accounting is shared with Windows; stale reads of decommitted memory, accepted on Windows by T-M3) | First: sampler stime + minflt of the main thread, RHIThread, RenderThread and task threads. A Linux decommit counter (5 s line) would size it. After: the same columns, plus FPS and frame p99 |
| D | **DrawPrep threads off SCHED_IDLE** (nice 10-19 SCHED_OTHER), `notify_one` / targeted wakes, and keep the shared mutex out of the workers' long scans | ~0.02-0.05, plus removing a possible stall of stage A and the guest submit thread | S-M / L-M | Add lock-wait timers to `BeginSubmission`/`Enqueue`/`Collect` (5 s line) and run before and after at the A setting; DrawPrep involuntary switches |
| E | **ATRAC9 cost:** first an Ajm 5 s stats line (batches, jobs, AT9 frames/s, ms/s, and the calling thread's name). Then, if it is large, **FFmpeg's ATRAC9 decoder** (float, SIMD transforms; FFmpeg is already linked for MP3) instead of LibAtrac9 | 0.03-0.15 now; maybe half of that saved [inferred] | stats S / L; decoder M / M (superframes, gapless, RIFF config must match `runtime_ajm.c:188-252`) | The stats line in one gameplay run. Or, with no launch: a LibAtrac9 decode microbenchmark of a game AT9 file on the Deck over SSH |
| F | Drop the shared hot counters in `runtime_mutex.c` (`locks`, `unlocks`, `waits`, `wakeups`; or make them per-thread) | ~0.01 | S / L | Folded into any A/B (too small to see alone) |
| G | Timer slack for guest threads (`PR_SET_TIMERSLACK`, e.g. 0.5-1 ms) to batch the ~5.5k/s expiring 5 ms polls. Stretching the timeouts themselves is legal (a timeout is a minimum) but riskier | ~0.01-0.03, plus fewer preemptions of helpers and fewer C-state exits (power on a shared CPU/GPU budget) | S / L (slack) to M (stretch) | Sampler: DrawPrep involuntary switches and total CPU; battery watts in the overlay |
| H | Adaptive spinning for short guest mutexes (`PTHREAD_MUTEX_ADAPTIVE_NP` for normal-type mutexes; errorcheck semantics would be lost for type 1) | uncertain (fewer sleeps for 0x697cdb / 0x739dab, more spinning) | S / M | Wait sites: those sites' averages; sampler: RHIThread voluntary switches |
| I | SDL PipeWire backend in the AppImage (fix the `pw_loop_new` support-plugin lookup), instead of PulseAudio through pipewire-pulse | outside the game process; small | S / L | `top -H` of `pipewire-pulse` and SDL's audio thread, before and after |

Not proposed:
- recording on stage B (`GOW3_VK_RECORD_THREAD=0`);
- 0 prep workers;
- making Wwise mix stereo (guest behaviour);
- asynchronous Ajm on a host thread (it moves CPU without cutting it, and audio is off the frame path);
- the write faults (tracked as U5 / hot pages).

## 5. Measurement recipes (no perf)

**Per-thread sampler over SSH.** Untested; it reads only `/proc`. Two snapshots 3 s apart, then the differences:

```sh
pid=$(pgrep -o medievil-runti)
snap() { for t in /proc/$pid/task/*; do
  s=$(cat "$t/stat" 2>/dev/null) || continue
  c=${s#*(}; c=${c%)*}; c=${c// /_}; set -- ${s##*) }   # $1 = field 3 (state)
  v=$(awk '/^voluntary_ctxt/{print $2}' "$t/status"); n=$(awk '/^nonvoluntary_ctxt/{print $2}' "$t/status")
  printf '%s %s %s %s %s %s %s %s\n' "${t##*/}" "$8" "${12}" "${13}" "$v" "$n" "${37}" "$c"
done | sort; }                                     # tid minflt utime stime vol invol lastcpu name
snap > /tmp/a; sleep 3; snap > /tmp/b
join /tmp/a /tmp/b | awk '{ # $2-$8 first snapshot, $9-$15 second
  printf "%-18s usr %5.1f%% sys %5.1f%% minflt/s %6.0f vol/s %6.0f invol/s %6.0f cpu %s\n", $15,
  ($10-$3)/3, ($11-$4)/3, ($9-$2)/3, ($12-$5)/3, ($13-$6)/3, $14}' | sort -k3 -nr | head -40
```

(utime and stime are in 1/100 s, so dividing the 3 s difference by 3 gives %. Spaces in thread names become `_`.)
- What it adds over the parent doc's table: **user vs system time per thread** (system time = faults, mmap/munmap,
  futex, signals: the port's kernel cost on guest threads), minor faults, and the last CPU (are DrawPrep workers
  sitting on stage B's sibling?).
- Run it at **auto clocks** in the graveyard view. Then the % can be compared with the frame stats.

Other one-line checks:
- `cat /sys/devices/system/cpu/cpu*/topology/thread_siblings_list | sort -u` gives the sibling pairs for reading
  "last CPU".
- The wait-site run already shows clock reads are cheap (2.1). A `current_clocksource` read is optional.

## Sources

Code:
- `src/runtime_audio.c`, `src/runtime_ajm.c`, `src/runtime_extra.c` (event flags, NGS2), `src/runtime_mutex.c`,
  `src/runtime_kernel.c`, `src/runtime_thread.c`, `src/runtime_waitsites.c`, `src/runtime_memory.c`
  (`:1004-1010`, `:1062-1092`);
- `third_party/LibAtrac9/C/src/imdct.c`; `build.sh:85-89`;
- `gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp:482-571`;
- `gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.cpp:50-57`, `:224-501`; `vk_draw_prep.h:147-149`;
- `gpu/shim/gow3_threads.h:97-113`;
- `gpu/shadps4/video_core/amdgpu/liverpool.cpp:1236-1240`, `:2381-2392`;
- `gpu/shadps4/core/libraries/videoout/driver.cpp:1145-1161`, `:1259-1270`.

Disassembly:
- eboot 01.02 guest 0x3517b60-0x3517bbf (the Wwise audio loop) and 0x70dea0-0x70df40 (UE4 event wait).

Logs:
- `data/deck-waitsites-1.log` (wait-site windows; Wwise rates per window; thread creation, lines 260-310);
- `data/deck-helpers-run1.log` (2 workers);
- every `data/deck-*.log` for the audio backend.
