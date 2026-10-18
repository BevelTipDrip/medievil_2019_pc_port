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

## Tests (one change per launch, the user's go each time)

| Test | Change from run 1 | Answers |
|---|---|---|
| T-I1a | `render_resolution=native` | I1 cause (live r.SetRes); also map GPU load at native output |
| T-I2a | run 1 settings + `GOW3_SAMPLE_FILE=<research-data\samples\run>` + same route | Where stage B's time goes (symbolize with `scripts/symbolize_samples.py`) |
| T-I2b | + `GOW3_TEXTURE_HELPER=1` | Does parallel texture binding move stage B busy / FPS on this game |

Then, by what T-I2a shows: plan-gpu-threading.md T1 (host-copy waits), parallel recording (T7), or a stage-B-specific
fix. Each as its own stage with before/after numbers on the same route.
