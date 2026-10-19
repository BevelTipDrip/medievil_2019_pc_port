# Plan: baseline issues to fix before any enhancement (menus, frame pacing)

Status: found in the first run 2026-10-18, nothing changed. Parent: [plan-roadmap.md](plan-roadmap.md).
**The user's order (2026-10-18): these come before DLSS / FG / any other enhancement.**

## Run 1 (2026-10-18 01:42-02:07, the user)

- Build `783b25a`, game CUSA11227 01.00 (eboot sha256 matches). RTX 4090, Ryzen 7 7800X3D (8c/16t).
- Settings: [data/baseline-run1-2026-10-18.ini](data/baseline-run1-2026-10-18.ini): `render_resolution=4K_native`
  (live `r.SetRes 3840x2160` about 10 s in), `ue4_quality=ultra`, `engine_fps=240`, `vsync=1` (Fifo, 480 Hz virtual
  vblank), `output_res=1920x1080`, borderless, `async_shaders=1`, `deferred_readback=1`. Patch applied: Native 4K
  framebuffer pool (11 writes).
- Log: [data/baseline-run1-2026-10-18.log](data/baseline-run1-2026-10-18.log); per-second series (FPS, pacing,
  draws/frame, stage waits): [data/baseline-run1-2026-10-18-series.txt](data/baseline-run1-2026-10-18-series.txt).

## I1. Menu navigation stops part-way

**The user:** "pressing start lets you move through the first few menu items, then you can't actually get back to the
menu, further inputs do nothing. The 'options' menu is similar, you can't go down past the item that lets you see the
controls."

What is known (read):
- The run used `4K_native`: the runtime changes the resolution live through `r.SetRes` (`src/runtime_extra.c:563-583`;
  log: `Native output: r.SetRes 3840x2160 (live resolution change)`).
- The README's known limits already say: "After a live resolution change the debug menu's gamepad navigation cannot
  reach its right-hand column: switch to 1080p ... to use the cheats, then back." Same symptom class (UMG widget
  navigation / focus geometry stale after the viewport size changes), now seen in the normal menus.

Hypothesis H1a: the live `r.SetRes` change breaks UMG navigation. **Test T-I1a:** same settings with
`render_resolution=native` (no live change) only. Menus work → H1a holds; next: find what UE4 needs after a
resolution change (e.g. viewport / DPI / widget geometry refresh) or start the game at the target size.
Menus still broken → input path (`src/runtime_pad.c`), controller vs keyboard, next.

**The user (2026-10-18): DualSense controller; 4K monitor at 3840x2160.** So `4K_native` is the right mode for this
display (supersampling to a 1080p output would be a downgrade), and I1 has to be fixed, not worked around.
`output_res=1920x1080` in the ini does not matter here: it only feeds the port's temporal upscaler
(`vk_temporal_upscaler.cpp:283-299`), which is off for MediEvil.

### I1 research (2026-10-18, verified in `out/eboot.elf` of 01.00)

- UE 4.21 (`++UE4+Release-4.21`). The game's display metrics (0xb68fd0, `FDisplayMetrics`-like: width/height +0/+4,
  rects +0x18/+0x28, 9 callers) and work area (0xb6d640, by vtable) are computed **once and cached** (flag 0x5b1d2c8,
  rect 0x5b1d2cc). Default rect {0,0,1920,1080} (rodata 0x3cbe750, checked).
- Only if the int at **0x5b1d1d8 = `r.PS4Allow4kOutput`** ("Allows the engine to allocate 4k backbuffers if a 4k tv
  is detected", read-only, 0 by default) is non-zero do they open VideoOut (`mov edi,0xff; xor esi,esi` → open),
  read the resolution status and use **3840x2160** when the display is taller than 1080 (`movabs rax,0x87000000f00`
  at 0xb69072 / 0xb6d6c9, checked). Checks: `cmp dword [rip→0x5b1d1d8],0; vmovups (default rect); je` at
  0xb69027/0xb69032 and 0xb6d689/0xb6d696 (checked with capstone).
- The game's own `OVERBITE/Config/PS4/PS4DeviceProfiles.ini` `[PS4_Neo_4k DeviceProfile]` sets
  `+CVars=r.PS4Allow4kOutput=1` (checked); profile chosen PS4 → PS4_Neo → PS4_Neo_4k (display height ≥ 1081). **A PS4
  Pro on a 4K TV boots with 4K display metrics: the shipped, tested path.**
- The port: VideoOut reports `GOW3_INTERNAL_WIDTH/HEIGHT` (default 1920x1080, `gpu/shim/core/emulator_settings.h:25-26`),
  `sceKernelIsNeoMode` = 0. The live `r.SetRes` itself is processed properly by the engine (render threads recreated
  in run 1's log), but the cached display metrics / work area stay 1920x1080.
- Mechanism (UE4 4.21 general knowledge, not verified in this binary): the SWindow / Slate hit-test grid keep the
  1080p size while UMG lays out at DPI 2.0 for 2160p, so widgets beyond the 1080p area are outside the grid and gamepad
  navigation (`FHittestGrid::FindNextFocusableWidget`) cannot reach them: matches all three symptoms.
- VideoOut has one port: a second `sceVideoOutOpen` while open → `RESOURCE_BUSY` (`driver.cpp:124-127`), and
  `sceVideoOutClose` closed the main port whatever handle it was given. Unknown: whether the game's display query runs
  before its main open (image 0x685e91).

### I1 fix, attempt 1 (lab change L3, built 2026-10-18 03:00, not yet run)

Opt-in: `GOW3_UE4_BOOT_4K=1` with `render_resolution=4K_native`; without it nothing changes.
- `patches/MediEvil.xml`: new patch "4K display metrics at boot" (`isEnabled=false`): `9090` at 0xf69032 and 0xf6d696
  (the two `je`, image + 0x400000). Compiled by `scripts/patches.py` to exactly two 2-byte writes at image 0xb69032 /
  0xb6d696 (checked).
- `run.py`: in that case report a 3840x2160 VideoOut (`GOW3_INTERNAL_WIDTH/HEIGHT`), add the patch to `GOW3_PATCHES`,
  and do not set `GOW3_UE4_SETRES` (no live change). The 512 MB pool patch stays (enabled by default).
- `video_out.cpp`: warning when a second open is refused (`gow3: main port already open: second open refused`), and
  `sceVideoOutClose` ignores an invalid handle instead of closing the main port.
- Python tests: same 12 pre-existing failures (GoW3 patch fixtures, Linux packaging, launcher versions) as clean
  `upstream/main` in a scratch worktree (109 run, 29 skipped): nothing new.

Test T-I1b (the user): launcher Render resolution = 4K native, then
`run-game.ps1 -Env @{GOW3_UE4_BOOT_4K='1'} -Tag i1-boot4k`. Expect in the log: `Patches: ... '4K display metrics at
boot'`, **no** `Native output: r.SetRes`, **no** `second open refused`; the picture at 4K from the main menu; menus
and the debug menu fully navigable with the DualSense. If `second open refused` appears, the query came after the
main open: next attempt is an alias handle for a second open.

### T-I1b result (2026-10-18 ~03:20-03:25): not verified yet

- The user: "that fixed the menu-bug for 4k output, it can be navigated now."
- But the only log is from **03:20**: a **launcher** run at `4K_native` (`Patches: ... 11 writes from ['Native 4K
  framebuffer pool (512 MB)']`, `Native output: r.SetRes 3840x2160 (live resolution change)`): the **old** path, not
  the 4K boot. Saved as [data/launcher-4k-native-live-setres-2026-10-18-0320.log](data/launcher-4k-native-live-setres-2026-10-18-0320.log).
- The scripted run started at 03:23 (`medievil.ini.bak-i1-boot4k`) left no log: **`run-game.ps1` bug**: run.py only
  prints, the launcher is what writes `last_run.log`, so the script copied the stale 03:20 log. Fixed: the script now
  runs run.py through `research/tools/tee_run.py`, which writes the output to `data\<tag>.log` (UTF-8) as the
  launcher does (tested with a dummy command: output, UTF-8 and exit code pass through).
- **The user (2026-10-18): the menus were checked in the scripted run (`GOW3_UE4_BOOT_4K=1`); the 03:20 launcher run
  was closed right after opening (Alt+F4).** → **I1 fix verified in game by the user** (4K output, menus fully
  navigable with the DualSense). H1a stands.
- Caveat: that run has no log (script bug above), so the log-side expectations (patch line, no live r.SetRes, no
  refused second open) are unconfirmed. Get them from the next scripted 4K run before this becomes a PR.

If H1a holds, fix directions (to research): (a) after `r.SetRes`, do what UE4 does after a normal resolution change so
UMG navigation uses the new geometry; (b) start the game at 3840x2160 so no live change is needed (the comment at
`src/runtime_extra.c:555-560` says the base PS4 renderer sizes its buffers from the display size at start: look at
what the runtime's VideoOut reports and whether a 4K start size is possible).

## I2. Frame times: oscillating 50 ↔ 120+ FPS, GPU not saturated

**The user:** "it oscillates between 120+ and 50 fps, very jarring, and GPU load isn't anywhere near 100% ... 70-80%
at most in super busy areas." Level-select map: 65 FPS with GPU ~100% but low temperature.

What the log shows (measured, run 1; see the series file):

| Scene | FPS | draws/frame | stage B (draw recording) busy | stage A waited | decode thread blocked on host copies | blocked on GPU | write faults/s |
|---|---|---|---|---|---|---|---|
| Gameplay, light (s 233-234) | 126 | ~1000-1200 | 95-96% | 56-58% | ~ | ~0 | ~ |
| Gameplay, busy (s 237-239) | 108 / 51 / 54 | 973 / 3915 / 3227 | 94-97% | 38-42% | 11-18% | 1-3% | 84k-111k |
| Level-select map (s 149-152) | 63 | ~840 | 96-97% | 65-67% | 6% | **30-31%** | 46k |

Reading:
1. **Gameplay is bound by one CPU thread: stage B (`DrawRec`, draw translation + Vulkan recording) is 93-97% busy
   whenever the game is busy.** Its throughput is roughly fixed (~200k draws/s), so FPS follows draws/frame: 973 →
   108 FPS, 3915 → 51 FPS. That is the oscillation, and why the GPU sits at 70-80%. Same limit Bloodborne's notes
   describe after its 0.3 work (plan-gpu-threading.md).
2. Texture binding is fully serial: `Texture helper: 0 binds in parallel, ... 976896 serial`. The helper is opt-in
   (`GOW3_TEXTURE_HELPER=1`, `vk_rasterizer.cpp:1445-1453`; "measured neutral on 16 threads" for Bloodborne).
3. Stage A blocks on host copies 11-18% in busy gameplay, ~105-131 waits/frame: plan-gpu-threading.md candidate #1
   (targeted host-copy waits) addresses exactly this.
4. 46k-220k write faults/s (GPU page tracking, VEH on the game's threads). Cost per fault on Windows not measured;
   the stall reports' "write faults 0.1-0.4 ms" counts only handler time, not exception dispatch. **Unknown, measure.**
5. The map is different: ~840 draws/frame, stage A blocked on the GPU 30% → GPU-side at 4K native + ultra. Low GPU
   temperature at "100%" suggests the GPU is mostly waiting (sync, barriers, small serialized submissions), not
   shading. Needs a GPU-side look (RenderDoc / GPU timestamps); compare with `render_resolution=native`.
6. Long single stalls (130-1350 ms) in the log are loading / level transitions (few draws, 100-700 MB image uploads,
   "image refresh" up to 114 ms): separate item, after the steady-state work.

Stage B's own breakdown is not in the log: **a sampling profile is the next measurement.**

## Run 2 (2026-10-18 ~02:22-02:26, the user): crash while loading into a level

- Build: PDB lab build of `783b25a` (`out/medievil-runtime.exe` + `.pdb`, installed 02:20; same code as run 1).
- Settings changed from run 1: `render_resolution=native` **and** `ue4_quality=console` (two changes; log line 2).
  Ran at ~160 FPS (stage B 67% busy) until the crash. Log: [data/run2-crash-2026-10-18.log](data/run2-crash-2026-10-18.log).
- Crash report (the runtime's own, `src/probe.c:120`):
  `Fault: read access to 0000000000000130 at nvoglv64.dll+0xe19d80 (thread 19980)`, thread `gow3:VkRecorder`, `rcx=0 rdx=0`.
- Symbolized with the PDB (`cdb -z`): `nvoglv64.dll` ← `Vulkan::Rasterizer::DrawRecord::<lambda2>+0x82`
  (`vk_rasterizer.cpp:1122-1137`, the recorded draw) ← `RecordChunk::Execute` (`vk_scheduler.cpp:308`, inlined) ←
  `Scheduler::RecorderThread+0x412`. The return address follows the `vkCmdDrawIndexed` call (dispatch slot 0x3f8,
  indexed branch); the preceding `vkCmdBindPipeline` returned.
- Reading: the driver dereferenced a null object while executing an indexed draw. NVIDIA resolves bound state at draw
  time, so the null can be the bound pipeline or other state the draw uses (index / vertex buffer, descriptors).
  A null pipeline handle is unlikely: `GraphicsPipeline` asserts on creation failure (`vk_graphics_pipeline.cpp:436`)
  and the async compiler returns `nullptr` (draw skipped) while a pipeline is pending (`vk_pipeline_cache.cpp:715-760`).
  **Not resolved by reading.** Same thread whose chunk handoff upstream 1.3.2 fixed (`783b25a`, null-chunk race).
- Next (C1): reproduce with `research/tools/run-game.ps1 -Validation -Tag c1-validation` (Khronos layer installed
  2026-10-18, names the null handle), WinDbg attached (`windbg.ps1`) for the full state at `fatal_exception`.
  Is it reproducible at the same place? (ask the user)
- The user did not reach gameplay in run 2, so the menu test (T-I1a) is still open.
- Suspects (the user's guess, 2026-10-18: the launcher's "GPU data" settings, all default in run 2):
  "Two-stage GPU pipeline" Auto = on at 16 threads (`GOW3_DRAW_PIPE`, launcher app setting, not in the ini) is the
  prime one: the crashing draw was prepared on stage B and replayed on the recorder thread. "Deferred GPU readbacks"
  on (`deferred_readback=1`) and "GPU readbacks" Relaxed (`GOW3_READBACKS`) change copy-back timing, less direct.
  "Game reads GPU data without waiting" off (`stale_readback=0`): unlikely.
- Order: C1a `run-game.ps1 -Validation -Tag c1-validation` (same area); if no crash, C1b same without validation
  (validation timing can hide a race); C1c `-Env @{GOW3_DRAW_PIPE='0'} -Tag c1-drawpipe-off`; readback settings
  only after that.

## Run 3 (2026-10-18 ~02:36-02:42, the user, from the launcher)

- Same settings as run 2 (`native`, `console`), launched from the launcher, so **no validation** (no VUID / validation
  lines in the log; `run-game.ps1` was not used). Same cutscene / level load as the run 2 crash: **no crash.** The user:
  the crash may have needed a shader compiling at that moment; non-deterministic. → C1 not reproduced (C1b done).
- **I1: the user confirmed the menus work at `native`** → H1a holds: the live `r.SetRes` change (4K_native) breaks
  menu navigation. The user's display is 4K, so the fix is needed (directions above).
- Performance (the user: "quite poor"). Log [data/run3-native-2026-10-18.log](data/run3-native-2026-10-18.log),
  series [data/run3-native-2026-10-18-series.txt](data/run3-native-2026-10-18-series.txt):

  | | Run 1 (4K_native, ultra) | Run 3 (native 1080p, console) |
  |---|---|---|
  | Busy gameplay, ~3100-3900 draws/frame | 51-54 FPS | 64-70 FPS |
  | Light scenes, ~250-700 draws/frame | 108-126 FPS | 180-240 FPS |
  | Stage B busy when busy | 94-97% | 84-97% |

  Dropping from 4K ultra to 1080p console barely moves busy gameplay: **confirms I2 is CPU-side and scales with draw
  count, not resolution or quality.** In run 3's busiest seconds (s 57-60, ~3100 draws/frame, 15 ms median) stage B
  is 83-85% busy and stage A waits only 22-24%: neither GPU-side thread is fully saturated there, so the game's own
  CPU threads (or the write-fault cost) may share the limit. The sampling profile has to cover the game's threads too.
  Also 390-604 ms single-frame hitches during play (s 30, 45, 48): streaming / loads, separate item.

## H1. Freeze on the last frame (run i2-profile-4k, 2026-10-18 03:35-03:37)

- Scripted run with `-Samples -Env @{GOW3_UE4_BOOT_4K='1'}`, 4K_native. The log confirms the 4K boot:
  `Patches: CUSA11227; 13 writes from ['Native 4K framebuffer pool (512 MB)', '4K display metrics at boot']`, no
  `Native output: r.SetRes`, no `second open refused` → **I1's log evidence collected.**
  ([data/i2-profile-4k.log](data/i2-profile-4k.log); it ends at 03:37:28, ~2 minutes after the start at 03:35:40:
  that is the freeze, the log is complete.)
- The game froze on a frame. **Not a fault** (no report, no dump): the process stayed alive, ~1.2 cores busy.
- Snapshot with `cdb -pv` (non-invasive) + PDB; raw output in
  `C:\Users\Ty\MediEvil\research-data\hang-2026-10-18-i2-profile-4k\` (`stacks.txt`, `counters*.txt`):
  - Thread 9 `shadPS4:GpuCommandProcessor` (stage A) spins in `Scheduler::WaitHostCopies` (yield loop) ←
    `SubmitExecution` ← `Finish` ← `BufferCache::DownloadMemory` ← `ReadMemory` lambda ← `Liverpool::SendCommand` ←
    `ProcessCommands`: a readback the game asked for (`vk_scheduler.cpp:217-228`).
  - Recorder (`gow3:VkRecorder`), stage B (`gow3:DrawRec`, in `DrawPipe::Run` atomic wait) and every `Gow3Copy` pool
    thread are idle.
  - Scheduler `0x54aa6048`: `host_copies_issued = 0x7a4fdf`, `host_copies_done = 0x7a4fd6` (**9 copies never run**),
    `queued_chunks = 0`, recorder sleeping, **`recording_owner = 0xb21c` = stage B's thread**, current `record_chunk`
    holds `0x45f0` bytes (17.9 KiB) of recorded commands **never handed to the recorder**.
- Mechanism: `WaitHostCopies` kicks the current chunk **once** (`KickRecording(true)`) and then only spins. Host copies
  recorded after that kick (here by stage B, last owner; also possible: a `RecordHostCopy` seq taken before the kick
  but pushed after it, since `++host_copies_issued` happens outside `chunk_lock`) stay in a chunk nobody hands over:
  stage B kicks by itself only at 32 KiB (`vk_scheduler.cpp:239`) and then sleeps waiting for packets. Stage A waits
  forever → frozen frame. How stage B came to record after stage A's drain + kick is not established from the snapshot
  (`ProcessCommands` drains first, `liverpool.cpp:91`); the fix does not depend on it.
- Bloodborne has the same one-kick wait (`bloodborne_pc/.../vk_scheduler.cpp:457-463`), but its readbacks avoid
  `Finish()` there (candidate #2 in plan-gpu-threading.md).
- **Fix L4 (lab):** in the wait loop, `KickRecording(true)` again every 256 spins. Safe: `Record()` and `KickRecording()`
  both hold `chunk_lock` (`vk_scheduler.h:760`, `vk_scheduler.cpp:237`, the 1.3.2 race fix); an empty chunk returns at
  once. Built clean 2026-10-18 03:43 into `out/gpu-pdb`, not installed (the frozen game held the exe). Not yet run.
- Likely an upstream bug (draw pipe is on by default at 8+ threads): PR candidate once it holds in runs.
- Test T-H1: same scripted run and route; no freeze over a long session. Rare and timing-dependent like C1, so
  "no freeze" needs several sessions before it counts.

## Run i2-profile-4k-2 (2026-10-18 03:47-03:52): C1 again, first profile

Build `684cd8f` (hang fix L4 + dumps), scripted, `-Samples`, 4K boot, `console`. Launched by Claude with the user's go.
- **C1 reproduced** after ~4 minutes: `Fault: read access to 0000000000000130 at nvoglv64.dll+0xe19d80`, thread
  `gow3:VkRecorder`, `#0 medievil-runtime.exe+0x13c8e2` = `Scheduler::RecorderThread+0x422` (same site as run 2).
  Exit code 3. Log [data/i2-profile-4k-2.log](data/i2-profile-4k-2.log). No freeze (H1) in those 4 minutes.
- **Dump failed:** `MiniDumpWriteDump failed (0x80070057)` (E_INVALIDARG), 0-byte file. The injected-fault test had
  passed; in game the memory-walking types fail (likely the placeholder reservations / mapped views). **Fix L1b:** fall
  back type by type (memory types → indirectly referenced → stacks/threads/handles → normal), delete the file if all
  fail, log each failure. Success path re-tested (injected fault: type 0x1a65, 184 MB); the fallback path waits for a
  real crash.
- Scripted runs had no per-second stats: the launcher always sets `GOW3_FRAME_STATS=1`
  (`launcher/medievil_launcher.py:333`); `run-game.ps1` now does too.
- **Profile** (`research-data\samples\i2-profile-4k-2\`, ~150k samples per thread over the whole session: menus,
  loading and play mixed). The samples are offsets into the 03:47 exe, which had been replaced: rebuilt from the same
  source (`684cd8f`, verified by the absence of L1b's string) into `build-684cd8f\`. `run-game.ps1 -Samples` now
  copies the exe and PDB next to the samples. Stage B (`DrawRec`, the bottleneck thread), share of all its samples:

  | Share | Where | What |
  |---|---|---|
  | 36% | `DrawPipe::Run` atomic wait | idle (waiting for packets) |
  | 15.3% | `RunEventWriteEop` (liverpool.cpp:267) → `TextureCache::ProcessDownloadImages` (texture_cache.cpp:165), yield / `SwitchToThread`; incl. `WaitHostCopies` 6.7% | end-of-pipe events wait for image readbacks on stage B |
  | ~12% (10.9% in the syscall) | `BindResources` → `TextureCache::ResolveOverlap` → `FreeImage` → `SignalImpl::Protect` (page_manager.cpp:388) → `runtime_memory_gpu_protect` → `VirtualProtect` | image churn: overlapping images freed / re-created, a protection syscall each |
  | 5% | `sceVideoOutSubmitEopFlip` → `SubmitFlip` → `Semaphore::Wait` | the flip waits for the GPU on stage B |
  | ~23% | `RunDrawPacket`, `DrawRecord`, `BindTextures`, `ResolveVertexBuffers`, `FindTargetMemoized` | translation work |

  Reading: about half of stage B's busy time is waits and syscalls, not translation. Leads: (1) image readbacks
  processed at EOP on stage B (plan-gpu-threading.md #2, readback queue); (2) texture-cache overlap churn and per-image
  `VirtualProtect` (#4 collector fixes; also why the same overlap repeats); (3) flip waiting on the GPU on stage B.
  Next profile: gameplay only (start sampling in the level), to separate menus / loads.

## H1 + C1: one cause? (2026-10-18)

**Repro scenario (the user):** load into the Graveyard level and leave the game idle. Freeze (H1) after ~2 min in
run i2-profile-4k; crash (C1) after ~4 min in run i2-profile-4k-2. Run 2's crash was at a cutscene / level load.

Hypothesis H-race: stage A submits (readback → `Finish` → `SubmitExecution`) **while stage B is still recording**.
- H1 showed exactly that state: stage B recorded host copies into the chunk after stage A's kick.
- C1 fits the same race: if stage A hands over and submits stage B's chunk in the middle of a draw, the draw's first
  commands (index / vertex buffer binds, from `EmitVertexBuffers` / `EmitIndexBuffer`, `vk_rasterizer.cpp:1004-1007`)
  end in one command buffer, and `bindPipeline` + `drawIndexed` (the `DrawRecord` lambda, `:1122-1137`) in the next,
  where no index buffer is bound: the driver reads a null object at `vkCmdDrawIndexed` (both crashes), after a
  `vkCmdBindPipeline` that returned.
- So one race, two outcomes: pending host copies → H1 freeze; split draw → C1 crash. Run 2's crash predates fix L4, so
  the race is upstream's; L4 only stops it from freezing (and may turn some freezes into crashes).
- Not shown yet: how stage B records while stage A runs a command callback (`ProcessCommands` drains first,
  `liverpool.cpp:91`).

Test T-race: the Graveyard idle scenario with the two-stage pipeline off (`GOW3_DRAW_PIPE=0`), ~10 minutes. Neither
freeze nor crash → H-race very likely; then find the path where stage B records during stage A's submit. A crash or
freeze anyway → the race is elsewhere (recorder vs. presenter?), next is the validation layer.

### T-race result (2026-10-18 04:01-04:10, launched by Claude with the user's go)

`GOW3_DRAW_PIPE=0` + 4K boot, build `5247265`, Graveyard idle (the user). Log
[data/t-race-drawpipe-off.log](data/t-race-drawpipe-off.log): no `Draw pipe:` lines (pipeline really off), **no fault,
no freeze in ~9 minutes**, clean exit (code 0); FPS per 5 s window min 55.7 / median 85.1 / max 239.7 (106 windows).
The two runs with the pipeline on failed at ~2 min (H1) and ~4 min (C1) in the same scenario. One run: moderate
evidence for H-race, not proof. Next: find the path where stage B records during stage A's submit.

### Root cause found (2026-10-18, read-only trace + checked by hand)

**`Liverpool::ProcessCommands` drained stage B only when its first `num_commands` check saw a command**
(`liverpool.cpp:89-104` before the fix). `num_commands` is atomic and `SendCommand` (liverpool.h:134-152) adds to it
from any thread; `ProcessCommands` runs once per PM4 packet (liverpool.cpp:141, 406, 737, 1370) while stage B is busy.
A command queued by a guest thread between the check and the loop ran on stage A **beside stage B**: readbacks
(`BufferCache::ReadMemory` → `DownloadMemory`: `CopyBuffer` record + `Finish()`, buffer_cache.cpp:179-213),
`TextureCache::ResolveReadbacks` (`Wait`/`Flush`), non-EOP flips (`PrepareFrame` on `draw_scheduler`). On Windows a
guest thread's fault posts these with `assume_locks=false` (page_manager.cpp:69-84), and `Rasterizer::ReadMemory`'s
own drain does nothing off stage A (vk_rasterizer.cpp:412), so `ProcessCommands` was the only drain.
- H1: stage A in `SubmitExecution` kicks (owner = A); stage B keeps drawing, `RecordHostCopy`s (non-atomic
  `++host_copies_issued` on two threads, vk_scheduler.h:837-843), ends each draw with a non-forced `KickRecording()`
  (owner = B, chunk < 32 KiB stays), runs out of packets and sleeps → stage A waits forever. Matches the snapshot.
- C1 (inferred, consistent): stage A's `SubmitExecution` ends rendering, submits, allocates a new command buffer and
  invalidates dynamic state while stage B is between its buffer binds (`EmitVertexBuffers`/`EmitIndexBuffer`) and its
  `bindPipeline` + `drawIndexed` → the draw lands in a command buffer with no index buffer bound.
- **The Bloodborne port found and fixed exactly this in 0.3 (`4d4d208`)**: drain inside the loop, per command, with a
  comment naming "null chunk crashes …, lost chunks, hangs". Its `WaitHostCopies` has no re-kick: root cause fixed.

**Fix L5 (`bc0e1d6`):** drain before every queued command (Bloodborne's change, attributed). **`684cd8f` reverted
(`a07f709`):** under the race its re-kick handed stage B's chunks to the recorder after `SyncRecording`, into a command
buffer being ended (could turn the hang into the crash); with the race fixed it has no job. Built clean 04:17,
installed. Test T-L5: Graveyard idle, two-stage pipeline **on**, 15+ minutes, more than one session.

**T-L5 session 1 (2026-10-18 10:29-10:48, the user, from `research\tools\launcher.cmd`):** build `bc0e1d6`,
two-stage pipeline on (105+ `Draw pipe:` lines), 4K boot, console, Graveyard. **No fault, no freeze in ~19 minutes,
clean exit** (the user: "a pretty long run, no crashes"). Log [data/t-l5-pipe-on-2026-10-18.log](data/t-l5-pipe-on-2026-10-18.log).
Pacing over the run (229 windows of 5 s): FPS min 34.8 / p10 91.4 / median 102.3 / p90 133.4; frame-time stddev
median 1.95 ms; p99 median 13.53 ms, max 39.68 ms. Against the pipe-off run in the same scenario (median 85.2 FPS,
p99 16.8 ms, worst p99 74.7 ms): ~20% faster with better p99, a bit more spread (frames over 1.5x median). The fix's
own cost: `commands` drains ~37/s, 0.1% of stage A. Before-fix pipe-on stats for this scenario do not exist (those
runs froze / crashed, scripted runs had no stats then). One more session before calling it verified.

Secondary path (not fixed, lower likelihood: it exists with the pipe off too, which ran clean): stage B faulting on a
guest read between its buffer binds and the draw (motion-vector hashing of bone palettes / indices,
vk_rasterizer.cpp:1049, 1070-1080, only for motion draws) runs the readback inline. Option: emit the vertex / index
binds after that block. Optional diagnostic: Bloodborne's `ProducerScope` check (two threads in the scheduler).

## C1 next: catch it when it happens

The crash is rare and timing-dependent, so reproduction runs are unreliable. Better: make every crash leave a full
memory dump. `fatal_exception` (`src/probe.c:120-178`) prints a report and calls `TerminateProcess` from inside the
first-chance handler, so Windows Error Reporting never sees it and no dump is written. Proposal (lab change, the
user to approve): write a minidump (`MiniDumpWriteDump`, full memory) from `fatal_exception` before terminating, into
`user\crashdumps\`. Then any random crash can be opened in WinDbg with the PDB, including the recorded draw's
captured state. Possible upstream PR: upstream 1.3.2 added crash reports; a dump would complement them.

**Done 2026-10-18 (approved by the user), lab change L1** (`src/probe.c`, uncommitted on `lab`):
`write_crash_dump()` called from `fatal_exception` after the report, before `TerminateProcess`. `dbghelp.dll` loaded
at run time (no link change, same pattern as `GetThreadDescription`); dir `<--user dir>\crashdumps` set in `main`;
`GOW3_CRASH_DUMP=0` off, `=full` adds all memory; default = private read-write memory (host heaps / stacks, where
recorded commands live) + data segments, handles, thread info, memory map, indirectly referenced memory; the guest's
mapped section stays out. Verified: injected `rip=0` under `cdb` after the runtime's handler was installed →
runtime report `Fault: execute access to 0000000000000000` + `Crash dump: ...\crashdumps\medievil-20261018-025025-41252.dmp`
(193 MB at that early point; in game it will be larger, size to note on the first real crash) → exit code 3 → the dump
opens in cdb, `.ecxr` gives the faulting context. Built clean (no new warnings). Draft PR record:
[pr-crash-dump.md](pr-crash-dump.md).

**Lab change L2** (`vk_scheduler.cpp`, diagnostic only, not for upstream as is): `gow3:VkRecorder` is now sampled by
`GOW3_SAMPLE_FILE` too (`Gow3Sampler::SampleCurrentThread("VkRecorder")`); before, only `GpuCmd` (stage A) and
`DrawRec` (stage B) were. The game's own threads are not covered by it: use Windows Performance Recorder for them.

## S1. Silent exit with code 5 at startup (2026-10-18 12:53, once)

First launch of the R2 + R3 build (`2097554`, run `t-r23`): the process exited with code 5 about 8 s in, right
after `Runtime: mapped flexible memory 0000001201000000, size=6291456` — where a good run goes on with the PSN toolkit
(`libSceNpToolkit2`) creating `Toolkit::NP::Main` / `Notifications` / `AbortThread`. No fault report, no dump, no
error line, no `The game exited` line from run.py. The runtime has no `exit(5)` (probably the game or the toolkit
giving up after an error). Log [data/t-r23.log](data/t-r23.log). The immediate retry (`t-r23-retry`) started
normally. Not seen in the ~12 launches before R2 / R3: **watch every launch**; if it repeats, start with R2 and R3
reverted one at a time.

## V2. Fire / torches broken, more box shadows (the user, 2026-10-18, run `perfdiag-heavy`)

"I loaded into the starting area … It looks like fire/torches were broken, and I saw more of those broken 'shadows'
than I recall seeing down there before." Build `7dcd6a0` lab (R1-R3), `stale_readback=1`,
`GOW3_STALE_SKIP_COPY_WAIT=1`, 4K boot, console, `GOW3_PERF_DIAG=1`. Possible regression: stale readbacks and R1
change when GPU-written data reaches the game (R2 / R3 behaviour-identical by test; the race fix changes thread
order only). Open: what "broken" looks like; whether the torches were right earlier today. Bisect: same torch with
`stale_readback=0`, then with R1 off.
**Resolved 2026-10-18 (the user):** broken = "black transparent flickering boxes"; torches were right in earlier
runs. V2a (stale off, so R1 off): torches correct. V2b (stale **on**, R1 off): torches correct. **Cause: R1**
(`GOW3_STALE_SKIP_COPY_WAIT`), reverted in `6b86c75`. Stale readbacks cleared and kept. The box shadows remain:
those are V1, pre-existing; the user can trigger them reliably (capture target).

## V1. Shadows sometimes drawn as a big box (the user, 2026-10-18)

"Some shadows occasionally drawn wrong (like they're just a big box instead of the shape of the actual shadow)",
seen before stale readbacks were turned on. Not investigated yet (RenderDoc capture when it shows).

**Persistent repro (the user, 2026-10-18):** with Dan near a light source, he has his correct shadow from the light,
and in addition a **black transparent box on the ground near him** that moves with his idle animation and jitters at
low amplitude with it. Fits a per-object / projected shadow pass: a bounds box drawn over the ground that should
sample a shadow depth map or mask but reads black (texture never written, wrong view / format, or a cache lookup
that resolves to the wrong image). Also seen: such effects appearing / disappearing at the moment of the pool commit
hitches (T-M1), probably because spawning them allocates memory. Plan: RenderDoc capture standing still near a torch;
find the draw that writes the box, inspect its bound textures and blend state, then the port's handling of that
texture (texture cache: block / format views, depth-as-texture, readbacks).

**RenderDoc captures (2026-10-18 19:08-19:11, launched by Claude with the user's go; `run.py` `GOW3_RENDERDOC`;
`research-dataenderdoc1-shadow_frame22745.rdc`, `_frame24679.rdc`; scripts `research/tools/renderdoc/`).** The user
circled the broken areas in frame 24679 (right of Dan along the path; lower left).
- Spot / per-object shadow maps (512² / 1024² D16) are cleared and rendered each frame with real depth (e.g. min
  0.27-0.47 to 1.0); UE4 projects each with a 36-index box (`min` blend into R / A of the shadow mask, R8G8B8A8 4K at
  0x2036000000), as designed.
- Pixel history, inside the right box (4K 3360,1512): projection eid 6479 writes **R = 0.000** (fully shadowed); just
  above it (3360,1308) the same draw writes 1.000. Lower left (720,1872): eid 6636 writes 0.565.
- eid 6479 reads the 512² map 0x2019f00000, rendered at eid 4715 (Dan's sword: 3492 indices, fine) and **eid 4727
  (2346 indices): a solid streaked slab** covering much of the map at depth down to 0.0377.
- eid 4727's vertices (raw, guest buffer 0x20598E8000, position-only stream) are **not Dan**: a flat object ~3.2 m x
  3.4 m x 0.2 m with box corners (a fence / wall section). **eid 4727 has no pixel shader** (`ResourceId::0`). A
  masked (alpha-tested) card drawn depth-only without its discarding pixel shader casts a solid shadow: everything
  behind it from the light goes black inside the projection box (which follows Dan's bounds, hence it moves and
  jitters with his idle animation).
- The port drops the fragment stage only when the PS program address is 0 (`RefreshGraphicsStages`). Either the
  game draws it without a PS (then PS4 would show the same) or the port used the wrong register state for this draw.
  Suspect: the draw-preparation workers (pipelines prepared from register snapshots). Live test: toggle bits 8192
  (prepared draws) / 4096 (prepared resources) at the torch; the box disappearing confirms it.
- Separately, the 1024² cached map 0x2076200000 (not re-rendered this frame) has solid zero-depth strips along its
  right and bottom edges: the lower-left partial darkening; check after the first fix.

## Tests (one change per launch, the user's go each time)

| Test | Change from run 1 | Answers |
|---|---|---|
| T-I1a | `render_resolution=native` | I1 cause (live r.SetRes); also map GPU load at native output |
| T-I2a | run 1 settings + `GOW3_SAMPLE_FILE=<research-data\samples\run>` + same route | Where stage B's time goes (symbolize with `scripts/symbolize_samples.py`) |
| T-I2b | + `GOW3_TEXTURE_HELPER=1` | Does parallel texture binding move stage B busy / FPS on this game |

Then, by what T-I2a shows: plan-gpu-threading.md T1 (host-copy waits), parallel recording (T7), or a stage-B-specific
fix. Each as its own stage with before/after numbers on the same route.

**Live toggle test (2026-10-18 19:30-19:40, launched by Claude with the user's go; log `data/t-v1-toggles.log`),
Dan at the torch with the box visible:** A draw preparation off (12288), B render-state memo off (1<<44), C two-stage
draw pipeline off (1<<37), E all general optimizations off (bits 0-19 and 32-46 = 140733194436607): **the box stays in
every case.** Not caused by the port's optimizations.

**Corrected reading of eid 4727 / 4578:** the same 445 vertices at 0x20598E8000 in both captures (static data, not
stale memory); dimensions and 4 cm posts fit the iron fence's real 3D bars, drawn depth-only (an opaque caster needs
no pixel shader). It sits right at the torch (depth down to 0.04), so its bars smear into a wide streaky shape in the
512² map; the lighter lines in it are the gaps. The inputs look legitimate, so the projection shader decides "fully
shadowed" inside the box where the fence should give stripes. Next: RenderDoc shader debugging of eid 6479 at
(3360,1512) in `v1-shadow_frame24679.rdc`: the shadow-map coordinate it computes, the sampled depths, the receiver
depth and the comparison (scale / offset, tiling, bias or compare direction).

**Cause found (2026-10-18, RenderDoc, no launch):** the user noted the shadows are in the wrong place (the fence's
stripes are right, but size / shape / position follow Dan; the torch is in front of the fence). The atlas is fine
(sword shadow in rows 4-238, fence in 246-440, own viewports / scissors); both projection passes (6479, 6507) use the
same shadow matrix and 512² size. The projection is limited to a shadow volume by stencil: 6465 draws the volume (front
faces increment, back faces decrement, depth test on), 6479 projects where stencil != 0 (masks 0xff, from the guest)
and zeroes it. **At the box pixel the stencil is already 0x80 before the volume** (set by the base pass's ground mesh,
eid 5227) and nothing clears it, so every such pixel inside the box passes. UE4 clears the stencil with a compute fill
of the depth buffer's **separate stencil plane** (GCN): eid 5985 (shader 307, 32640 x 64 x 4 bytes) and eid 3778 write
0x7F8000 bytes at 0x2018800000, a 3840x2160 8-bit plane. The texture cache's D32S8 image covers the depth plane
(0x201a000000) only, so those writes never reached the image's stencil.

**Fix V4** (toggle bit 59 `StencilPlaneWrites` restores the old path): each depth bind records its stencil plane
(address, bytes); a compute dispatch writing (nearly) a whole known plane marks it; the next bind of that depth buffer
copies the plane (GPU-current, `ObtainBufferForImage`) into the image's stencil aspect (`Runtime::CopyBufferToStencil`).
Exact for uniform fills (clears); other contents would need detiling (logged: "Stencil plane ... rewritten by a
shader", first 4). Test T-V4: Dan at the torch; live A/B with bit 59.
