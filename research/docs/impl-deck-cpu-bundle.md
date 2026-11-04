# Implemented: Deck CPU bundle (D1, D2, recorder lean mode) (2026-11-04)

Status: built (Windows and Linux), not run. Branch `deck-lab`: `02c15bf` (D1), `471913a` (D2), `ff54428` (R, S).
Inputs:
[study-deck-rescore-rejected.md](study-deck-rescore-rejected.md) (D1, D2, A13),
[study-deck-other-threads.md](study-deck-other-threads.md) (3.1 and cut B: the recorder),
[study-stage-b-per-draw.md](study-stage-b-per-draw.md) (C2: recorder wakes forced by every label).

| # | Change | Where | Switch | Default |
|---|---|---|---|---|
| D1 | `-ftls-model=initial-exec` for `libgow3gpu.so` (spacepepperoni/medievil_ps4_Linux `7800f32`) | `gpu/CMakeLists.txt` | build only (Linux) | on |
| D2 | `scePthreadGetthreadid` caches the `gettid` result per thread (spacepepperoni `1232650`) | `src/runtime_extra.c` | none | on (Linux; Windows already reads the TEB) |
| R | Recorder lean mode: no spin before sleeping; polled labels do not wake a sleeping recorder | `vk_scheduler.cpp/.h`, `vk_draw_pipe.h`, `vk_rasterizer.cpp/.h`, `liverpool.cpp` | **second toggle word bit 32** (set: off, live); `GOW3_RECORDER_LEAN=1/0` | on at <= 8 hardware threads (the Deck); off on the PC |
| S | Counters: "Recorder per frame" and "Thread ids per frame" lines | `driver.cpp`, `gow3_toggles.h` | `GOW3_FRAME_STATS=1` | - |
| G | GPU library linked statically into `medievil-runtime` (study-deck-platform-optimization.md B1), `98dca62`; section "G" below | `gpu/CMakeLists.txt`, `build.sh`, `packaging/linux/appimage.sh` | build: `GOW3_STATIC_GPU=OFF` | on (Linux; Windows always static) |

## D1: static TLS for the GPU library

The library's `thread_local`s (stage B's `DrawPipe::on_stage_b`, `Gow3Sections::recording_thread`, the section
flags, the user-data snapshots, the sampler memo, `record_data_chunk`, ...) were reached through the
general-dynamic model: a `__tls_get_addr` call per access. Initial-exec reads them at a fixed offset from `%fs`.

Why it is valid here:
- `medievil-runtime` and the test binaries link the library at start (`build.sh:82`, `-lgow3gpu`: DT_NEEDED). Nothing
  `dlopen`s it (no `dlopen` of it in `src/`, `scripts/`, `packaging/`). The AppImage keeps it DT_NEEDED and only
  rewrites the rpath (`packaging/linux/appimage.sh:148`).
- A library in the initial set has its TLS sized into the initial static TLS block at start. The limit that makes
  initial-exec risky (glibc's surplus for libraries `dlopen`ed later, ~1.6 KB + `rtld.optional_static_tls`) does not
  apply. The library's TLS segment is 0x4b48 bytes (19.3 KB, `readelf -lW`), fine for the initial block.
- The linker marks the library `STATIC_TLS` (`readelf -d` FLAGS), so a future `dlopen` would fail loudly rather
  than misbehave.
- Windows: the library is static there; no change.

Check (WSL Debian, clang, the same build before and after): `objdump -d out/gpu/libgow3gpu.so | grep -c
__tls_get_addr` **524 before (`f03ee52`), 5 after (`ff54428`)**: the PLT entry, its GOT reference and 3 calls, all in
the FFX library linked in (`ffxVkPortableUpscaleContextCreate`, `...FrameGenerationContextCreate`,
`trackedCreateBackendContext`: context creation, cold). `readelf -d`: `FLAGS STATIC_TLS`; TLS segment 0x4b48 bytes.

## D2: cached thread id

`thread_id()` (`scePthreadGetthreadid`) made `syscall(SYS_gettid)` on every call; UE4 calls it for every
`IsInRenderingThread()`-style check (the fork measured ~150,000 calls in 4 s on the render thread). It now keeps the
id in a `__thread` variable after the first call. Thread ids never change, threads are never forked, and the host TLS
is the thread's own while guest code runs (the runtime's other `__thread` state works the same way). Windows already
used `GetCurrentThreadId()` (a TEB read).

## R: recorder lean mode

Mechanism today: stage B hands the recorder a chunk every 32 KiB, and every end-of-pipe / end-of-shader label kicks
with `force`: the partial chunk is handed over and a sleeping recorder is woken (`notify_one`, a futex wake on
stage B). On 8 threads the recorder spins 20 us first. On the Deck it runs a chunk faster than stage B fills the next
(a 32 KiB chunk is >= 100 us of stage B), so it spins the full 20 us and sleeps after almost every chunk: ~7.3k spins
a second (~15% of a CPU, issue slots taken from the sibling hyperthread, often stage B's) and a wake per chunk.
~200 labels a frame come every ~75 us, so most hand-offs and wakes are label-forced.

Lean mode (`RecorderLean()`: <= 8 hardware threads, or `GOW3_RECORDER_LEAN=1`; `=0` never; second toggle word
bit 32 switches it off live):

1. **No spin** before sleeping (`GOW3_RECORDER_SPIN_US=<us>` sets one). The spin almost never caught a chunk, so it
   saved stage B no wakes. Off lean mode: 200 us at >= 12 threads, else 20 us, as before.
2. **Polled labels do not wake a sleeping recorder.** End-of-pipe labels without an interrupt and end-of-shader
   labels (`SignalAfterHostCopies(..., urgent = false)`, only from stage B) still hand their chunk over at once,
   but if the recorder sleeps the wake is owed, not made (`wake_deadline_ns`). It is made by the first of:
   - the next kick that wakes anyway: a 32 KiB hand-off, a fence with an interrupt (UE4's frame end-of-pipe), the
     GPU-idle and frame-retire steps, `WaitDeferredSignals`, `WaitHostCopies`, `SyncRecording` (submission,
     flip), all `KickRecording(true)`;
   - any later kick on stage B once `GOW3_RECORDER_WAKE_US` (default 200; 0 = every label wakes as before) has
     passed since the first owed label (stage B kicks at the end of every draw, dispatch and label);
   - stage B running out of packets (`DrawPipe` idle hook, before it spins or sleeps);
   - `Scheduler::Wait` (a GPU wait) on any thread;
   - the backstop: in lean mode the recorder sleeps at most max(1 ms, 2 x the wake delay) at a time.
   An awake recorder needs no wake: it runs everything queued before it sleeps again (checked under the same mutex).
3. `GOW3_RECORDER_HANDOFF_KB` (lean mode only, default 32, 4-96): a larger hand-off for a test without a rebuild
   (fewer hand-offs and wakes, but `SyncRecording` then waits for more at submissions and the flip).

**What is guaranteed.** Order is unchanged: the signals still run on the recorder in recording order, then through
`Gow3Copy::AfterCopies`, exactly as before; only when the recorder starts on a queued chunk moves. Every wait that
needs a deferred signal (`WaitDeferredSignals`, `WaitHostCopies`, `SyncRecording`) forces the wake first, so no wait
can stall on an owed wake. Stage A's `WAIT_REG_MEM` drains stage B, which then runs dry and wakes the recorder (and
labels already handed to stage B count as written for it, `PendingFenceValue`). Latency: a polled label reaches the
recorder at most `GOW3_RECORDER_WAKE_US` plus one packet later while stage B works, at once when it idles, and in any
case within the backstop. Labels with an interrupt, the GPU-idle step (`sceGnmSubmitDone`) and the decode-pacing frame
retire are unchanged (woken at once). Not changed either: `GOW3_GPU_FENCES` (labels by the signal thread) and stage-A
labels (the draw pipe off).

**Risk.** The polled labels reach the guest up to ~200 us later than before while stage B is busy. If the game polls
one of them on its critical path, FPS can drop; that is what the A/B checks ("Guest event waits", frame lead, fence
lag). The PC is unaffected unless `GOW3_RECORDER_LEAN=1`.

## S: counters (`GOW3_FRAME_STATS=1`)

- `Recorder per frame (lean on|off|toggled off): hand-offs, sleeps, wakes; label kicks left asleep, owed wakes made at
  the deadline, at stage B idle / a wait / a forced kick, by the backstop; spin ms (spins, found work)`.
  Wakes = the "Stage B syscalls" line's recorder wakes (stage B + other threads); their us each is on that line.
- `Thread ids per frame: scePthreadGetthreadid N, cache hits N, kernel lookups N` (Linux: lookups ~0 after start;
  Windows: no system call either way).

## Build and test

- Windows: `research/tools/build-clang64.sh` (LTO), clean.
- Linux: WSL Debian, clang, `build.sh` + `packaging/linux/appimage.sh` at `ff54428`: `dist/MediEvil-x86_64.AppImage`
  (126 MB, 2026-11-04 04:59).
- Unit tests (`py -3.14 -m unittest discover -s tests`): 124 run, the 8 known failures (2 failures, 6 errors), no new
  ones.
- Not run in game.

## The Deck test (one launch, the user's go)

Settings as the C1 test ([study-stage-b-per-draw.md](study-stage-b-per-draw.md) section 6): 67%, auto clocks,
`GOW3_FRAME_STATS=1`, sections off, toggle file as in extra-env.txt. D1 and D2 are always on in this AppImage (their
effect shows against the previous AppImage's numbers in the same spot, not inside this launch).

1. Start with `toggles.txt` = `0 0` (lean on; C1 on). Graveyard heavy view, stand still.
2. Every ~30 s switch the second number: `0 32` (lean off), `0 0`, `0 32`, `0 0`: 4-5 windows each way, dropping
   the first 5 s window after each switch (the log prints `Runtime: disabled optimizations second word=32`).
3. Compare lean on vs off:
   - **FPS** and **stage B busy** ("Draw pipe: ... stage B busy").
   - "Recorder per frame": wakes (expect fewer with lean on), label kicks left asleep (~150-200 a frame), how the owed
     wakes were made (mostly deadline / 32 KiB; backstop ~0), spin ms (0 with lean on; off: ~20 us x spins).
   - "Stage B syscalls": recorder wakes on stage B and their us each.
   - Must not get worse: "Guest event waits" (ident-64), frame lead, fence lag, `blocked: recorder`, `RunWriteData`
     and `flip_present` waits.
   - Optional, over SSH: the per-thread sampler (study-deck-other-threads.md section 5) for `gow3:VkRecorder` CPU and
     voluntary switches (expect ~-15 points of a CPU).
4. Also read "Thread ids per frame" (D2: lookups ~0) once.

Pass: lean on gives stage B busy lower or FPS higher, recorder CPU lower, and no worse guest waits or fence lag. If
FPS drops with lean on while the recorder CPU falls, try `GOW3_RECORDER_WAKE_US=50` (the latency side) before
dropping the label part; `GOW3_RECORDER_WAKE_US=0` keeps only the no-spin part.

## G: static GPU library on Linux (`GOW3_STATIC_GPU`, `98dca62`)

Status: built (Windows and Linux), not run. What changed:
- `gpu/CMakeLists.txt`: option `GOW3_STATIC_GPU` (default ON). ON on Linux does what Windows does: `gow3gpu` is a
  static library and CMake links `medievil-runtime` (and `runtime-test` / `sema-test`) with it, so the runtime C
  joins the ThinLTO unit (`GOW3_LTO`). Runtime flags as in `build.sh`: C11, `-O2 -g`, asserts on (`-UNDEBUG`),
  `-no-pie` (brk heap below 1 TiB), `-rdynamic` (`dladdr` names in fault and thread dumps). `-fno-plt` on the library
  and the runtime. D1's `-ftls-model=initial-exec` stays; the static link relaxes it to local-exec.
- `build.sh`: passes `GOW3_STATIC_GPU` (env, default ON), builds `medievil-runtime` with ninja and copies it to
  `out/`; removes a stale `out/gpu/libgow3gpu.so`. OFF: the previous path (`libgow3gpu.so` + `$CC` link).
- `packaging/linux/appimage.sh`: installs `bin/gpu/libgow3gpu.so` and its rpaths only when the runtime has it as
  DT_NEEDED; else RUNPATH `$ORIGIN/../../../lib` only. ldd collection and the GLIBCXX/GLIBC scan follow the same list.
- `tests/test_gpu_runtime_stubs.cpp`: `runtime_disabled_optimizations2` (the statically linked CMake tests need it).

Switch back: `GOW3_STATIC_GPU=OFF bash build.sh && bash packaging/linux/appimage.sh` (rebuilds the library). The OFF
build is bit-identical to the previous one (`libgow3gpu.so`, `medievil-runtime` and the AppImage, checked).

Counts (WSL Debian, clang 19, ThinLTO; `objdump -d` call/jmp lines to `@plt>`, `nm -D`, `size`, `ldd`):

| | shared (OFF; = `ff54428`) | static (ON) |
|---|---|---|
| PLT call/jmp sites | 49,039 (runtime 1,626 + library 47,413) | 4,055 (`_Unwind_Resume` 1,971, `abort` 360, exception paths, backend `memcpy`/`memset`; 29,621 calls through the GOT) |
| of those: into the library's own exports (interposable) | 16,000 | 0 |
| library -> `runtime_*` / runtime -> `gow3gpu_*` through the PLT | 101 / 14 | 0 / 0 (`runtime_memory_clamp`: 6 PLT calls -> 6 direct) |
| `__tls_get_addr` | 5 (FFX context creation) | 0; one TLS segment of 0x5518 bytes |
| text / bss | 12.21 + 0.39 MB / 42.2 + 2.0 MB | 12.56 MB / 44.2 MB |
| files (stripped) | 1.27 MB + 77.3 MB (0.45 + 12.5 MB) | 78.3 MB (12.7 MB) |
| dynamic symbols | 434 + 7,595 | 7,833 (`-rdynamic`) |
| `ldd medievil-runtime` | 165 libraries, `libgow3gpu.so` | 164, no `libgow3gpu` |
| AppImage | 131,209,720 bytes | 131,152,376 bytes |

Of the shared library's 7,055 exported symbols, 14 are absent from the static runtime: the unreferenced archive members
`native_clock`, `spin_lock`, `string_util`, `rdtsc` (no static initializers; no member with an initializer was left out).

Build and test: Windows `research/tools/build-clang64.sh` clean (no change there). WSL: `build.sh` +
`appimage.sh` both ways; the AppImage's `--help` works and `--vulkan-info` reaches `vkCreateInstance` (-9, no GPU in
WSL), the same as the shared one. CMake GPU tests (`pipeline-binary-*`, `motion-shader-`, `scene-resolution-`,
`taa-shader-`, `camera-motion-test`) link statically; `pipeline-binary-cache-test` passes. Unit tests: 124 run, the 8
known failures. Already broken before this change, either way: `build.sh --test` (`tests/test_pad.c` does not compile:
`gamepad`), and `runtime-test` / `sema-test` do not link (`runtime_image_base` / `_size` live in `probe.c`).

Risk: L. Things that differ at run time: the library's code and data now sit in the executable at low addresses
(below 64 MiB, as on Windows), the executable exports the library's symbols ahead of `libm` (before, the `.so` did,
right after `libm`), and fault dumps name the frames `medievil-runtime+0x...` instead of `libgow3gpu.so+0x...`.

### The Deck test (one launch each, the user's go)

Two AppImages from the same commit, in WSL `/root/src/medievil/dist/`: `MediEvil-x86_64-static.AppImage` (default)
and `MediEvil-x86_64-shared.AppImage` (`GOW3_STATIC_GPU=OFF`). Same settings for both as the test above (67%, auto
clocks, `GOW3_FRAME_STATS=1`, sections off, `toggles.txt` = `0 0`), Graveyard heavy view, stand still ~60 s; drop the
first 5 s window. Compare FPS and "Draw pipe: ... stage B busy" (expect 1-4% lower busy or higher FPS static), and
that nothing else got worse (guest event waits, fence lag, recorder line). One launch each is within run-to-run noise
for 1-4%: if the difference is smaller than the spread of the 5 s windows, call it no difference or ask for an A-B-A.
