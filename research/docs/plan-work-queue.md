# Work queue: what is next, in order

Status: started 2026-10-30 (the user's list after N-26). Parent: [START-HERE.md](START-HERE.md). The single to-do list:
update an item's state here when it moves; details live in the linked plan or study. States: `open` / `in progress` /
`done` (with commit) / `dropped` (with why).

## The user's quality-of-life list (2026-11-03, after the v2.0.0 release)

Not in any order (the user's numbering kept). The performance items below stay open alongside. Also: the user will run
the build on a second PC (5800X3D + RX 6900 XT), the first test on other hardware and on AMD.

**Order (the user, 2026-11-03):** quick wins first (11 cursor, 3/13 mouse, 6 debug menu); then **10/9 Linux and the Steam Deck** (moved up: lower-spec data for every fix, and unified-memory fixes that may help handhelds in general); the second PC and 8 (Anti-Lag 2) when that PC is available; then 1/2 DLSS/FSR; then 4, 12; then 7, 5. Bug reports from players (the build is being shared online) come in alongside. Research running 2026-11-03: `plan-linux-deck.md`, `study-mgs2-temporal-for-ue4.md`, `study-mouse-input.md`, `study-glyphs.md`.
**Research done and the user's decisions (2026-11-03)**, details in each doc's "Decisions" section:
- 3/13 mouse: the game has UE4's PS4 mouse path behind `PS4.EnableMouse`; design approved (sceMouse from SDL, yaw
  hook) ([study-mouse-input.md](study-mouse-input.md)).
- 12 glyphs: prompts are one atlas row + six textures, swapped in place through the override pak; art from a CC0
  pack; static choice first, live switching a nice-to-have ([study-glyphs.md](study-glyphs.md)).
- 1/2 DLSS / FSR: UE 4.21 velocity pass + View buffer give the inputs; **SR before FG**; DLSS-G paces itself after
  our hand-off; try real MVs for particles; no RCAS with DLSS
  ([study-mgs2-temporal-for-ue4.md](study-mgs2-temporal-for-ue4.md)).
- 10/9 Linux / Deck: 01.02, OLED, build in WSL2 here, Proton baseline first, read the other Linux forks, PR #68
  checked against our movie fix before porting ([plan-linux-deck.md](plan-linux-deck.md)).
- 11 cursor: **done** (`4bb5565` + `638b6a0`; the user: "exactly what we want"): hidden in borderless / fullscreen and after 3 s idle in a window, shown in the menu and while the window has no input focus.
- Working rule: one item at a time.

| # | Item | What is known (2026-11-03) | State |
|---|---|---|---|
| 1 | DLSS / FSR upscaling | Needs the UE4 frame map and motion vectors first ([plan-ue4-motion-vectors.md](plan-ue4-motion-vectors.md) M0-M2). UE4 already runs TAA with a velocity buffer (movable objects only, the rest from depth + camera) and its own jitter; the dormant path is Bloodborne's and must not be reused as is. DLSS SR bridge exists ([plan-dlss.md](plan-dlss.md)) | open |
| 2 | DLSS / FSR frame generation | After 1's inputs. FSR FG API already linked with no caller ([plan-fsr-frame-generation.md](plan-fsr-frame-generation.md)); DLSS FG through NGX Vulkan ([plan-dlss.md](plan-dlss.md)) | open |
| 3 | Mouse look | [study-mouse-input.md](study-mouse-input.md). Verified (T-MS10): batched Raw Input reader, auto capture, mouse look on both axes in free areas and Dan-cam (on the camera tick), mouse buttons and wheel bound in the launcher, sensitivity / ratio / invert in the launcher and the menu. Later: drop the `GOW3_MOUSE_INPUT_YAW` fallback, mouse in menus (step 3), fixed / scripted cameras with item 4's Free mode | in progress |
| 4 | Camera options: gentler / no automatic camera moves; free camera whenever Dan is controllable | Researched 2026-11-11 ([study-camera-moves.md](study-camera-moves.md)). The user's design: a switch per forced move; Hybrid = Free except fixed / scripted cameras; run zoom and vertical easing separate. T-CM1 (pak version, all six off): the user: works great. Live switches in the menu's Controls tab (`10beddc`, T-CM2) and the launcher's Advanced camera section (`7449317`, T-MS10): verified. Later: Free in scripted areas | in progress |
| 5 | Live settings in the game's own menu (low priority) | The ImGui overlay already has live settings ([plan-live-switches.md](plan-live-switches.md)). Idea: one item in the game's pause menu that opens it, overlay restyled to the game's look. **2026-11-12 (branch `menu-redesign`):** the settings menu drawn in the Load list's game look (Options frame, fonts, colours, prompts) and usable with a pad alone (focus rows, D-pad / stick change, L1 / R1 tabs, Circle back; `GOW3_MENU_PAD`); built, not run; tests T-SM1-T-SM8 in [plan-settings-menu.md](plan-settings-menu.md), card runtime/settings-menu. **Pause-menu item, 2026-11-12:** the game's hidden "Switch to Classic Controls" button shown as "PC Settings" (translated, after Options) through the override pak, and its click opens the settings menu (medievil.ini `pc_settings_entry`, default on; `GOW3_PC_SETTINGS_OPEN=0` log only); research and T-PC1 in [study-pc-settings-entry.md](study-pc-settings-entry.md), card ue4/pc-settings-entry. Fix built, not run. Waiting: a row in the Options menu itself (structural package edit, S7) | fix built |
| 6 | Debug menu on 01.02 | Pak config edits apply on 01.02; 01.02's DefaultInput.ini maps R3 to "RecenterCamera", the debug menu's button ([plan-0102.md](plan-0102.md)). Checked 2026-11-09 (paks of both builds): that line is the only input difference; no Blueprint names R3 (the R3 check is native; `DebugHUD` has `OpenDebug`), while 01.02's `BP_InputDelegateActor` and camera areas listen for RecenterCamera, whose action binding takes the button. Fix built: with the debug menu on, the override pak carries 01.02's DefaultInput.ini without that line (`debug_menu_input`; 01.00 unchanged); R3 then no longer recenters the camera. Not run. Fallback: our own menu with 01.00's features, plus UE4 debug features | fix built |
| 7 | Launcher redesign | Banner low resolution, grouping, scaling: follow the window size, or fixed size scaled by Windows DPI only | open |
| 8 | AMD equivalent of Reflex | `VK_AMD_anti_lag` (Anti-Lag 2), same shape as the Reflex path ([study-vrr-present-pacing.md](study-vrr-present-pacing.md) l.108). Test on the RX 6900 XT | open |
| 9 | Handheld fixes for shared memory (Steam Deck, the user owns one) | On UMA the GPU memory is host visible: memory sync copies between guest and GPU memory may become zero-copy | open, after 10 |
| 10 | Linux builds, Steam Deck in mind | AppImage script exists (`packaging/linux/`, community build, not official) | open |
| 11 | Hide the mouse cursor in borderless (and fullscreen) | Cursor always shown; overlay sets ImGui `MouseDrawCursor` only (`gpu/shim/gow3_overlay.cpp:102`) | open, small |
| 12 | Controller glyphs: PlayStation, Xbox, keyboard / mouse | Plan G1-G6 in [study-glyphs.md](study-glyphs.md); pack downloaded (1.4 texture files, untracked `glyph-pack-download/`). G1 run (T-G1): the atlas patch reaches the HUD and menus; gaps Start / L1 / R1 = `Options_Active` / `L1_Active` / `R1_Active`, covered in T-G1b (run, the user: "everything looks correct"). G2 (DXT5 bone prompts) run, T-G2. G3 Xbox art (pack's Default set, `button_prompts=xbox`) run, T-G3. G3b launcher setting + Load list prompts run, T-G3b. G4 keyboard + remaps run, T-G4; square tab boxes and launcher key capture, T-G4c (the user: all correct). Released in 2.2.0. Optional next: G5 Xbox controller picture, G6 live switching. Also to do (the user, 2026-11-11): the port's own Load list prompts (game_ui cache `prompts/PS4_*`) must follow the chosen style. Card ue4/button-prompts | in progress |
| 13 | Mouse navigation in the game's menus (nice to have) | Same lead as 3 (`sceMouse`) | open |
| 14 | Small frame-time spikes in the base game (the user, 2026-11-12) | On the map screen at 237 FPS, frames 4.22 ms on average with steady small spikes to about 4.7-5.2 ms, with the game's own TAA too (T-F3 / T-F4: the upscaler hook's cost removed, these remain). Chase their cause with traces (frame trace, sampling profiler, GPU timestamps), not toggles | open |
| 15 | Black pixelated artifacts with the final RCAS sharpening (CAS) at 1440p (the user, 2026-11-12) | Borderless and fullscreen show it, windowed not; off with RCAS off; not at 1080p or 4K. The user could not reproduce it on the RTX 4090, even at a native 1440p output: likely AMD only. Found: the presenter's RCAS runs only when the game image and the window are the same size (`Gow3Graphics::FsrPassRuns`), so windowed probably never ran it. Suspect (not confirmed): the half-precision RCAS (`fsr.comp`, `A_HALF`) dividing by zero (`4 * mn4 - 4` on white neighbours) where AMD's native FP16 gives NaN / negative values. Next: a Shift+F12 dump on the RX 6900 XT or from the reporter, before any fix | open |

## The user's order (2026-10-30)

| # | Item | State | Where |
|---|---|---|---|
| 1 | **Release on GitHub + upstream check.** Build a proper release of `lab`; re-check what upstream (bc160201983) has added since the 1.3.5 merge (their focus has been other areas; ours is performance). Publishing needs the user's go. **2026-10-30:** upstream at 1.3.8 (19 commits not in `lab`); merged on branch `merge-1.3.8` (`e308c4c`, builds; 17 conflicts resolved, see its commit message). **No regression from the merge (2026-10-30):** the pre-merge `lab` build with the same settings and logging
(`t-lab-1-premerge-check`, effects 100, `frames_queued=1`) is no better than the merge (`t-m138-1-check`,
`t-m138-3-upstream-off`): heavy normal frames 4.55 ms (lab) vs 4.45 / 4.48 ms, lowest gameplay 5-s window 180 FPS vs
186 / 184, heavy-scene frames ≥ 30 ms 4 (30-37 ms) vs 0 / 1. The better alley lows remembered from before were
from runs without logging. **Clean run of the merge with the flip trace** (`t-m138-flip-1`, `-NoStats`,
`GOW3_FLIP_TRACE`, trace in `research-data\flip`; tools `research/tools/flip_lows.py` / `flip_stages.py`; N-27): (a) the fight dip the
user sees (~100 FPS) is 86 frames over 0.74 s, ramping smoothly 7.6 → 9.9 ms, with queued → GPU-ready equal to the
frame time: **GPU-bound** (the effects burst at 100% effects, N-18/N-19); (b) **isolated single-frame stalls of ~7 ms**
in a flat 237 FPS scene (81, 96, 116 s, ...) that land on a different stage each time (present thread, game submit →
stage A, recording): process-wide, not draw cost. Whether (b) is new with the merge is open: next, the same trace
with upstream's host switches off (`GOW3_TIMER_RES=0`, `GOW3_POWER_THROTTLE=1`, `GOW3_PAGEABLE_MEMORY=0`,
`GOW3_FUTEX_COND=0`), or the `lab` build with the same trace. **Host switches off (`t-m138-flip-2-host-off`):**
the stalls stay and come more often (5.3/min at the cap vs 2.8/min; 10-200 ms frames 148 in 208 s vs 150 in 467 s):
upstream's host switches are not the cause and stay on. **Pre-merge `lab` with the same trace (`t-lab-flip-1`):
both are old.** Fight dips to a 1% low of 99-105 FPS (10-11.8 ms frames), as on the merge (103); stalls 2.0/min at the cap
(7, up to 9.9 ms) vs 2.8/min on the merge (19, 7 of them over 10 ms): perhaps somewhat larger on the merge, too few to
tell. Fix them on the merge: the stalls first (a WPR capture, the user runs it: CPU sampling + context switches,
stall times from the flip trace), the GPU effects burst after (which passes grow; effects at 75% is the default).
Then A/B of upstream's switches (`GOW3_FUTEX_COND`, `GOW3_TIMER_RES`, `GOW3_PAGEABLE_MEMORY`, `GOW3_SPARE_IMAGE_BLOCK`, `GOW3_TRACKER_MERGE_GAP`), then **move the user's game to 01.02**: folder merged and every 01.00-only hook mapped and built (2026-10-31, [plan-0102.md](plan-0102.md)); played on 01.02 since (T-0102-1 to T-0102-6), effect issues fixed 2026-11-01. Then the release. | in progress | [plan-upstream-merge.md](plan-upstream-merge.md), [study-upstream-commits.md](study-upstream-commits.md), [repo-boundaries.md](repo-boundaries.md) |
| 2 | **Pending fixes from the 2026-10-29 research:** go through the list below, aim also at lower-end hardware. | open | [study-frame-cost-2026-10-29.md](study-frame-cost-2026-10-29.md) §2 |
| 3 | **Stage B per draw** (pending fixes 2, 3, 4, 6), then the **ring reuse retest** (with the stamp fix; the user saw better 0.1% lows with reuse during effect-heavy stretches, N-15). | open | below; [plan-frame-pacing.md](plan-frame-pacing.md) N-15, N-16 |
| 4 | **Graphics settings:** live switches for the quality options, and more settings the launcher lacks, so a player can min-max (highest frame rate, keep what they care about). | open | [plan-live-switches.md](plan-live-switches.md) |
| 5 | **DLSS / FSR** for slower GPUs and CPUs (on the 4090 / 7800X3D it may not matter any more). | open | [plan-dlss.md](plan-dlss.md), [plan-fsr-frame-generation.md](plan-fsr-frame-generation.md), [plan-ue4-motion-vectors.md](plan-ue4-motion-vectors.md) |
| 6 | **Scaling on lower-end hardware:** measure raw costs on this system, then emulate fewer cores / threads and a slower GPU, and look for non-linear drops (the CPU/GPU pipeline is asynchronous; a bottleneck on one side may behave badly). | open | to write: `study-scaling.md` |

## Pending fixes (research 2026-10-29, ranked there; state now)

**Stage B review (2026-10-30, on `lab` = upstream 1.3.8 merged).** Still valid after the merge: upstream's
`GOW3_RECORD_DATA_FIX` adds a `SyncWithDrawRecorder()` to every `RecordData()` (1 + N per descriptor push), but on
stage B it returns at once (`DrawPipe::OnStageB()`), so per-draw cost is unchanged and item 2's design stands.
Step 0's bind counter landed (`32586bc`, "Binds per frame"); the rest of the Step-0 counters (joint S1 predicate,
writes per push, why a texture set is not memoized, FindImage generation-only misses, PrepareRenderState sub-timers
for the 0.5-2 ms render-state spikes in ~8% of heavy frames) was never built. Order: (1) the Step-0 counter build
(stats-gated, no behaviour change; gates items 3, 4, 8, 10, and the sub-timers bear on the user's question about
single slow frames under steady load, N-27); (2) item 6 LTO (no code, A-B-A); (3) item 2 behind a Gow3Toggle bit,
A/B inside one launch; (4) item 3's sampler-handle cache (key also on `ta_bc_base` and `sampler_lod_bias`), S1 only
if its gate passes; (5) item 4 if gated in; item 8 last (risky). Then the ring reuse retest (stamp fix in `32586bc`).

| # | Fix | Expected | State |
|---|---|---|---|
| 1 | Preshadows cut | stage B -0.4 to -0.9 ms in busy views | **done as a live switch** (N-25, `fdd12e8`), default on. Open: variant (a) `r.Shadow.PreShadowFadeResolution=32;r.Shadow.MinPreShadowResolution=16` (PS4-1080p-equivalent culling at 4K, keeps them) if the user wants them on but cheaper |
| 2 | Bulk descriptor copy per push (Bloodborne 0.3), one ChunkGuard | stage B -0.1 to -0.2 ms (2.5-6%) | **done** (`e0c7566`, N-30: descriptors -0.12 us a draw, ~-0.18 ms of stage B a heavy frame) |
| 3 | Sampler-handle cache in the texture-set memo, then S1 delta-draw | -0.1 ms, then -0.25 to -0.35 ms | sampler cache **done** (`5c752e8`, N-31: samplers -0.085 us a draw, ~-0.13 ms a heavy frame); S1 needs its predicate counter (not built) |
| 4 | Memoizable sets that contain the draw's own target | -0.08 to -0.17 ms | **gate passed** (N-28: the dropped sets are own-target ones, colour more than depth; ~78 a heavy frame, ~2.3 us each) |
| 5 | Mid-frame 4K stencil-plane copy | GPU <= 0.1 ms | open, low |
| 6 | LTO build (ThinLTO, clang/MinGW) | 0-2% stage B | **done** (N-29: ThinLTO on by default; equal or slightly better, within noise) |
| 7 | Volumetric fog grid at PS4 density for 4K (GridPixelSize 16 -> 32) | GPU 0.05-0.2 ms; more on weak GPUs | open; a candidate quality option (item 4) |
| 8 | Skip identity GPU-particle sim draws | -0.15 to -0.28 ms | open, risky (frozen particles) |
| 9 | Shadow-map transition batching | GPU 0.02-0.12 ms | open, re-evaluate after 1 |
| 10 | FindImage generation-only misses | lows +1-3 FPS | **dropped** (N-28: 3-9 generation-only misses a frame, gate 50); render-state spikes now 0.8% of heavy frames, in target creation |
| 11 | `r.Shadow.MaxResolution=1024` | 0-5% | open, after 1; risk to the 0.1% lows |
| 12 | Real results for the ~20 shadow occlusion queries | -1% to +3% | open, low |

## Found on the way (2026-10-30)

| Item | State |
|---|---|
| The in-game menu doubles ~60 frames a second while open (~177 FPS, 296 frames at ~8.7 ms per 5 s; N-25) | open |
| `pad-map-test` (gpu/CMakeLists.txt) does not link on Windows: src/compat_win.c needs winmm (`timeBeginPeriod`); builds by hand with `-lwinmm` (found 2026-11-12, settings-menu) | found |
| Parking: some 4K pairs made new while one sat parked (a texture lookup first, other geometry fields?) (N-24) | open, small |
| `ue4_override.py` High/Ultra: `r.Shadow.MaxResolution`, `r.Shadow.DistanceScale`, `r.Shadow.CSM.MaxCascades` overwritten later by the local / sun shadow tables (study-live-switches.md) | open: the presets don't do what they say |
| Pak values inert: `r.GarlicHeapSizeMB` (in neither eboot), `r.Streaming.PoolSize=1600` (loses to the PS4 device profile's 2000: SetBy checked in the eboot, fix-inventory.md Caveats, 2026-11-03). Upstream's 4K crash fix (pool patch + `GOW3_DMEM_MB`) is separate and stays | checked statically; both dropped from `ue4_override.py` (`c1db412`); confirmed 2026-11-03: 4K run without them, pool +6,592 MiB as before, 237 FPS cap held, ~6 GB VRAM (the user) |
| Toggle-file bit 31 switches two things (narrow image stages, pool views kept across a decommit) (study-live-switches.md) | open, small |
| CLAUDE.md says `origin` is the public upstream; it is the BevelTipDrip fork (`upstream` is bc160201983, push disabled) | the user to edit (Claude does not edit CLAUDE.md unasked) |
| S1 boot crash (~1 in 8 launches): `pthread_mutex_lock` on a garbage mutex, found in the Windows event log ([plan-baseline-issues.md](plan-baseline-issues.md) S1) | open: lock sites in `runtime_mutex.c`, then a crash dump |
| `frames_queued=2` now smoother than 1; default stays 1 so testing does not mask real issues (N-25) | decided |
| **Write faults on the game threads (PC too):** ~760 a frame on both machines (530-760 in PC logs; Deck ~760). On the Deck they stretch the game's own frame (+5.3 ms in slow frames; userfaultfd made it +8.9). Fix to try on the PC: pages that fault every frame stay writable and are uploaded **once a frame** (not per binding: the rejected `GOW3_HOT_PAGES` grew to 14k pages, 33 FPS). First a fault map (which pages / guest sites, how many every frame). May explain part of the PC's isolated 0.1% stalls (N-27). Details: plan-linux-deck.md runs 5-6 (`deck-lab`) | open |
| **Scarecrow Fields obstacle tied to 60 FPS** (the user, 2026-11-09): at 144 FPS one of the level's obstacles cannot be passed; with the frame cap at 60 it can. Not the vblank tick (the user: a 60 FPS cap alone fixed it). Likely game logic that counts frames instead of time above the PS4's 60 (a Blueprint or animation step without delta time, or physics substeps), exposed by Engine frame rate 120/240. To find: which obstacle, what goes wrong; then the Blueprint / actor driving it (pak) and a per-frame vs delta-time check; fix by clamping that actor's tick to 60 or scaling its step | open |
| **Water refraction glitch in Dan's Crypt** (the user, 2026-11-09, 01.02): not DLSS or render scale (the user); it is `effects_resolution` below 100 (the user, 2026-11-10: 100 fixes it). **Found (RenderDoc, 2026-11-10, `research-data/dans-crypt-refraction-20261110`, crypt_frame38561 / 46077 at 75%, 54294 at 100%):** the cyan patches are the level's own reflection look; at 75% they get hard edges on 64-px lines (x 768, 1280, row 320) and shrink toward the top-left. Same primitive and alpha on both sides of an edge, colour 6x apart. The water shader (fs_0xe3b625eaec7fca55) picks reflection captures / local lights from UE4's forward light grid: `cell = uint(SvPosition - ViewRectMin) >> LightGridPixelSizeShift` (ForwardLightData dword 8 = 6; grid 41x23 cells for the 2573x1448 view). In downsampled separate translucency (1929x1086) the pixel position is not scaled, so it reads the top-left 31x17 cells. UE4 4.21 behaviour; the PS4 runs it at 100%. Depth fade and depth UVs are correct at 75% (checked: downsampled depth matches, ScreenPositionScaleBias right). Fix: recompiler pass `LightGridScalePass` scales the grid pixel position per draw (card ue4/light-grid-scale, study-light-grid-scale.md). **Verified** T-LG1 (2026-11-10, 50 / 75 / 100 live; the user: "Yes, all 3 now look correct!"). Open: the forest cauldron fog (suspected same cause) | verified |
| **Save written to the wrong slot:** MediEvil saves into the newest slot of a time-ordered search and makes a loaded slot the newest by writing its file back at load; we dated a slot by param.bin only, so after loading any slot but the newest, the next save replaced the newest other slot (confirmed in log, T-SV2). Fix: a save's date is its last write (card runtime/save-date-last-write, study-save-slots.md). **Verified** T-SV3 (2026-11-10; the user: "It looks like it worked.") | verified |
| **Load menu with slot details and more slots (the user, 2026-11-10):** our SaveDataDialog list (`save_open`, `gow3gpu_choice_begin`) shows only the param.bin title / subtitle / date. Done: "Slot N" from the folder name, "Continue" = the newest save (verified T-SV4, card runtime/load-list-continue); per-save details, the level's loading screen, every level on Square, scrolling, the game's Options menu look (verified T-SV5, cards runtime/load-list-details, launcher/game-ui-cache). The game's New Game already makes a new slot (T-SV1). Next on the same cache: controller / keyboard glyphs (the user) | verified |
| **Launcher save backups pruned by name:** `scripts/save_backup.py` kept the 10 newest names; letter-named hand-made backups sorted first and every automatic backup was deleted when made (found 2026-11-10). Fix built: none are deleted (the user: saves are small, keep all until save problems are understood); card launcher/save-backup-keep-all. Run T-SV4: the start's backup stayed | run |
| **Stale Python tests (found 2026-11-10, after other work):** 8 fail on Windows. 6 in `tests/test_patches.py` read `patches/God_of_War_III_Remastered.xml` / profile CUSA01623, deleted upstream in 316f552 ("MediEvil branding"); rewrite against MediEvil's profile (CUSA11227, `patches/MediEvil.xml`). 2 in `tests/test_packaged_vulkan.py` split Linux `:` path lists and break on `C:\` paths; all 7 pass under WSL Debian: skip them off Linux. Done 2026-11-11 (`d75c1c8`, card tooling/python-test-upkeep): rewritten against MediEvil.xml plus an inline rules file, the 2 skipped on Windows, the glyph tests skip without Pillow; Windows 3.14: 170 OK, WSL 3.13: 158 OK | done |
| **Hang at "quit to main menu" (PC, 2026-11-08, run `u1-frame-map`, render scale 80):** a small guest copy replayed on `gow3:VkRecorder` read-faulted; the recorder is not a GPU-side thread (`IsGpuSideThread`), so the fault waits on the GPU command thread (`SendCommand<true>`), which waits for stage B to drain, which waits for the recorder. UE4's watchdog ended the game after 120 s. Stacks: `data/u1-hang-recorder-fault-20261108.txt`. Not the frame map (not on any stack). One occurrence | found; fix to propose |

## From GitHub issues

Made by research/tools/issue2card.py queue N.

| Issue | Item | What is known | State |
|---|---|---|---|
| #1 | Requesting FSR4.X support, potentially with the ability to use FSRs ML versionâ€¦ | feature request; Windows; 2.2.1 (source); CUSA11227 version 01.02; [issue](https://github.com/BevelTipDrip/medievil_2019_pc_port/issues/1) | open |
