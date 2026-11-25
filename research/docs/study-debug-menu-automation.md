# Study: the debug menu, UE4 facilities and an automated benchmark (2026-11-24)

Static research only: no launch, no build, no source edit. Raw data, scripts and disassemblies are in
`research-data/debug-menu-research-20261124/` (on D:). Game builds: 01.02 = `E:\MediEvil\CUSA11227-0102`,
01.00 = `E:\MediEvil\CUSA11227` (eboot sha256 `a7e976d1…`, the build the port was checked against). The eboot
images used for addresses are the vaddr-0 images from `research-data/fname-research-20261122/image_010x.bin`.

Status words as the user uses them: everything below is **found** (static evidence). Nothing is fix built, run or
verified.

## Summary

- **You get determinism almost for free from the engine:** the shipping eboot still parses
  `-Deterministic`, `-UseFixedTimeStep` and `-FixedSeed`, and the game reads its command line from
  `/app0/ue4commandline.txt`. With the fixed time step every frame advances game time by exactly
  `FApp::FixedDeltaTime` (1/30 s by default), whatever the frame took. The fixed seed seeds `FMath::Rand`.
  The globals sit at fixed addresses in both builds, so the port can also set them directly, including a step of
  1/120 s. `-benchmark` and `-FPS=` are compiled out (this is a Shipping build:
  `BuildVersionString="OVERBITE-PS4-Patch2-ALL-Shipping 26"`).
- **Booting straight into any level needs only a config change:** the boot map is the ini's `GameDefaultMap`
  (`Level_AV_GameIntro` today), and the override pak already overrides engine ini files. `InitLevel` takes the
  level from the world's own name (`GetCurrentLevelName`), not from the main menu's state. A level opened as the
  boot map, by `open`, or by `OpenLevel` should therefore set itself up as it does in normal play.
- **The debug menu** is one UMG widget (`DebugHUD`) with 95 functions. It has a level select over `LevelsEnum`
  (every real level, plus 10 art and test maps that do not ship), Points of Interest (teleports Dan to tagged
  `BP_FadePortalDestination` actors and zeroes the camera blend), Invincibility, Fly mode/NoClip,
  Disable Spawns, AI paused while the menu is open, stat FPS/unit/SceneRendering/Game/UObjects, MemReport,
  1080p/1440p, 4:3 aspect, FPS max 30/60, gamma, give items, complete level, and launch a PS1 game. It has no
  flythrough, attract, benchmark or demo-playback entry.
- **No level is hidden:** all 266 `.umap` files in the paks belong to the 23 normal levels, the main menu, the
  map screen, the cinematic levels and `Engine/Maps/Entry`. The 10 debug-only rows (ArtTest_*, AI_Stadium,
  DesignTest_Destructibles, Level_Material_Room, ArtTes_FX_room, ArtTest_Level_21_ZL_Test) have no map in either
  build. The `Level_AV_*` "cinematic levels" play pre-rendered videos (`MediaPlayer.OpenSource(VIDEO_*)`), so
  they are useless as GPU benchmarks.
- **Engine console:** the shipping `UEngine::Exec` keeps `OPEN`, `STREAMMAP`, `TRAVEL`, `STAT`, `GAMMA`,
  `SCALABILITY`, `DUMPTICKS`, `CANCELASYNCLOAD` and `CE`. `UWorld::Exec` keeps `DEMOREC/DEMOPLAY/DEMOSTOP`. The
  game engine keeps `EXIT/QUIT`, and the viewport keeps `SETRES`, `HighResShot` and `SHOT`. The `UCheatManager`
  code is compiled in ("God mode on", `BugItGo`, `CheatScript.%s` strings; `EnableCheats` has a native).
  `STARTFPSCHART`, `-ExecCmds` and `BENCHMARK` are gone. The game has no console UI, so commands can only arrive
  through Blueprint `ExecuteConsoleCommand` or through code the port calls itself.
- **Recommended design:** boot into a fixed level, set the fixed time step (set to 1/cap through the port, so the
  game runs at real speed when it holds the cap), replay pad input by game frame instead of by milliseconds, run
  N frames with the flip trace, then exit through the path that writes the trace. Options are ranked in section 4.

---

## 1. The debug menu (DebugHUD) in full

Asset: `OVERBITE/Content/Blueprints/Debug/DebugHUD` (widget; 01.02: 95 functions, 01.00: 94). It is created at
game start by the game instance (`OverbiteGameInstance` ReceiveInit: `Create(DebugHUD_C)` → `DebugWidget`).
Dan's Blueprint opens it (card `research/patches/ue4/debug-menu-0102.md`: 01.02 L1+R1+R3, gated by the native
`GetAllowDebugMenu` or `!IsPackagedForDistribution`, and not in Store/Expo demo SKUs). Full disassembly:
`dis0102/DebugHUD.txt`; labels: `dis0102/DebugHUD_strings.txt`. Differences from 01.00: 01.02 adds "Complete All
Lost Souls", and 01.00 has a "Levels Completed" label.

What happens when it opens (ubergraph `m0x217d…`):
- `RegisterInputEvents`, and `SetAIPaused(true)` on the game instance (`m0x29cc`). The menu freezes AI while
  it is open; `CloseDebug` and the port's Destruct edit resume it.
- It fills the level combo from `LevelsEnum` (42 entries by index, `GetEnumeratorUserFriendlyName`) and preselects
  the current level (`GetCurrentLevelName` → `LevelDataTable` row).
- It fills Points of Interest from `GetAllActorsWithTag("PointOfInterest")` cast to `BP_FadePortalDestination_C`.
- It lists the camera stack (`CameraManager.CameraAreaStack` display names).

### Pages and options (label → what it calls)

| Label (widget) | Calls |
|---|---|
| Select Level / Load Level (`ComboBoxString_LevelLoad`, `Button_LoadLevel`) | selection → `LevelDataTable` or `DebugLevelDataTable` row → `LevelToLoad`; button → `OverbiteGameInstanceFunctions.LoadLevel(LevelToLoad)` then `CloseDebug` |
| Restart Level (`Button_RestartLevel`) | `RestartLevel` → GameInstance `PrepToOpenLevel` (reopens `LevelToLoad`) |
| Points of Interest (`ComboBoxString_PointsOfInterest`) | `Map_Find(POIs)` → optional `Activate()` (`BP_FadePortalDestination_WithActivate`); Dan `K2_SetActorLocationAndRotation(loc, rot, teleport)`; `SetControlRotation(0,0,yaw)`; `CameraManager.BlendTime = BlendDuration = 0`; closes the menu |
| Invinciblity (`Invincible`) | `OverbiteGameInstance.isInvincible = checked` |
| Fly Mode/NoClip (`FlyModeCheckbox`) | `CharacterMovement.SetMovementMode(5 Flying)`, `MaxFlySpeed ×4`, capsule collision off; off: Walking, speed back, collision 3 |
| Disable Spawns (`CheckBox_DontSpawn`) | `SetDontSpawn(checked)` (read by `BP_Spawn_Parent.GetDontSpawn`) |
| Disable All Triggers (`DisableAllTriggersCheckbox`) | `GetAllActorsOfClass(BP_Trigger_C)` → `SetEnabled(!checked)` ("cannot reenable them as of now") |
| Show Spawn Triggers / Show AI Behavior / Show Sound Names | `SetSpawnTriggersVisible` / `SetAIBehaviorVisible` / `SetDisplaySoundNames` |
| Show FPS / Units | `ExecuteConsoleCommand("stat FPS")` + `("stat unit")` |
| Show Stat SceneRendering / Show Stat Game / Show Stat UObjects | `stat SceneRendering` (the port's override turns this into `r.SetRes 2560x1440`) / `stat Game` / `Stat UObjects` + `stats.MaxPerGroup 55` |
| Generate MemReport | `MemReport -Full` |
| Show Collision | `show collision` |
| DisableAllScreenMessages | `DisableAllScreenMessages` / `EnableAllScreenMessages` |
| Cinematic Mode (1440p) / Performance Mode (1080p) | `r.SetRes 2560x1440` / `r.SetRes 1920x1080` (the override pak changes these; `ue4_override.py DEBUG_HUD_EDITS`) |
| 4:3 aspect (`43AspectRatio`) | `MainCameraActor.CameraComponent.SetAspectRatio(1.33 / 1.78)` |
| Set FPS Max to 30 (`SetFPSMaxTo60`) | `GameUserSettings.SetFrameRateLimit(30 / 60)` + `ApplySettings` |
| Gamma slider + box, Retain Gamma | `gamma <0.5..5>`; Destruct resets `gamma` unless retained |
| Hide HUD / Hide Subtitles / Disable Music / Disable Vibration | `PlayerHUD.HideHUD/ShowHUD`; `SubtitlesVisible`; `SetMusicVolume(0/10)`; `SetVibrationActive` |
| Enable Debug Dan Cam | `GameInstance.DebugDanCamEnabled` (read only by `BP_CameraAreaDanCam` tick: a debug variant of the follow camera, **not a free camera**) |
| Camera Test / Camera Stack | text display of the camera area stack |
| Debug Targeting / Teetering / Footprints / Debug Safe Location System / Debug Weapon Impact Events / Show Blocking Splines / Show ActiveList States / Debug Surface Types / Show Movement / Input Data / Show Interactive Data / Show Player Info / Show Music Info / Debug Input / SpawnSoulsOnDissolve | Dan or game-instance debug flags, or debug widgets (`DebugPlayerInfo`, `DebugSurfaceInfo`, `DebugMovementInputData`, `DebugInteractivePath`) |
| Give 200 Gold / Give Level Chalice / Set Chalice Count to 20 / Give Life Bottle / Give All Runes / Give All Quest Items / Unlock All Weapons / Unlock All Bestiary Entries / Unlock Daring Dash / Unlock All Levels / Give All Lost Souls / Complete All Lost Souls / Release Lost Souls / Give Default Items For Level / Unlock Emulation | inventory and save-state functions in `OverbiteGameInstanceFunctions` (`AddItemToInventory`, `SetLevelUnlocked`, `SetChaliceCollected`, `DebugGiveDefaultItemLoadoutForLevel`, …) |
| Complete Level (`CompleteLevelButton`) | `SetLevelUnlocked(current)`, then `BP_LevelExit_Trigger.SelfActivation()` (not in the map screen) |
| Save / Load / Restore from checkpoint | `PrintString` stubs ("Clicked restore") |
| Load PS1 Game (`ComboBoxPS1Games`) | `OverbiteGameSingletonFunctionLib.LaunchPS1Game(EPS1GameSKUEnum)` |
| Language (IETF Tag) | `SetLanguageForOverbite` |

**For benchmarks:** the useful pieces are the level select, Points of Interest (a repeatable spawn point and
camera reset), Invincibility, Disable Spawns, AI pause, Hide HUD, Disable Music and Fly/NoClip. Everything else is
display or inventory. There is no camera path, attract mode, demo playback or stat capture to file.

The main menu has its own small level loader, `DebugLevelLoadHUD` (created by `MainMenuPawn`). R3 toggles it only
when `IsPackagedForDistribution()` is false, which is why the card found no debug menu in the main menu.

### Level list (`LevelsEnum`, `LevelDataTable`, `DebugLevelDataTable`)

`LevelsEnum` values → map (dumps: `dis0102/LevelDataTable.txt`, `dis0102/DebugLevelDataTable.txt`,
`research-data/fname-research-20261122/levelsenum_0102.txt`):

- 0 Level_00_DC, 1 Level_01_GY1, 2 Level_02_CH, 3 Level_03_HM, 4 Level_04_GY2, 5 Level_05_SF, 6 Level_06_PG,
  7 Level_07_PS, 8 Level_08_SV, 9 Level_09_AG, 10 Level_10_IA, 11 Level_11_EE, 12 Level_12_AC, 13 Level_13_PD,
  14 Level_14_LA, 15 Level_15_CC, 16 Level_16_GG, 17 Level_17_HR, 18 Level_18_GS, 19 Level_19_EH,
  20 Level_20_TD, 21 Level_21_ZL, 22 Level_22_HH, 24 Level_IN_MM (main menu), 25 MapScreen.
- Cinematic levels (videos): 38 Level_AV_GameIntro (the boot map), 39 Level_AV_00, 43 Level_AV_18, 44 Level_AV_19,
  45 Level_AV_20, 46 Level_AV_Outro. `LoadLevel` sends 00_DC / 18_GS / 19_EH / 20_TD to their intro video first,
  unless the level is completed (`HasIntroCinematic`, `CinematicLevel`). The video level then calls
  `LoadLevelFromCinematic`.
- Debug-only rows, with **no map in the paks** of either build: 27 ArtTest_Characters, 30 AI_Stadium,
  32/33/34 ArtTest_Anim_*, 36 Level_Material_Room, 37 DesignTest_Destructibles, 40 ArtTes_FX_room,
  41 ArtTest_Anim_Cutscenes, 42 ArtTest_Level_21_ZL_Test. The paks keep a few loose assets from
  `Maps/TestMaps/{BillTest,NickTest,PetesTest,GlenTests,Video_Test}`, but no `.umap`.
- Maps that are in the paks but not in the enum: `Sequences/CInematicStorySequences/Level_00_AV/Level_00_AV`,
  `Level_Battlefield`, `Level_AV_DansIntro_Shot_004` and `Level_IN_AV`. These are sequence-authoring maps for the
  videos, never opened by the game. Whether they render anything useful when opened is unknown (open question).
- The two builds' map lists differ by one file: 01.00 also has
  `Level_21_ZL/Sublevels/Level_21_ZL_MAIN_Art_Interactive.umap` (`files_0100_e.txt` vs `files_0102.txt`).

Points of Interest (tag `PointOfInterest`) exist in the gameplay sublevels of 03_HM, 06_PG, 10_IA, 11_EE, 14_LA,
15_CC, 17_HR, 18_GS, 20_TD and 21_ZL (`name_grep_0102.txt`). They are ready-made, named, repeatable spawn points.

### Other "cheats" in the game

- The pause menu's button-code entry (`PauseMenu.InCheatMode`, `BP_TrophyManager.CheckCheatCode`) only matches
  the PS1 codes (`DSTCDUST`, `TCTCCTLCUDRCLLTRCLLTCDCCR`, …) and unlocks the trophy "Old School Sneaky". It has no
  gameplay effect.
- Store and Expo demo SKUs (`OverbiteGameSingletonFunctionLib.IsDemoSKU` / `IsExpoDemoSKU`) only change menus and
  the end-of-demo screen (`UI_DemoThankYou` → `LoadLevel`). Dan has an unused `ExpoDemoTimer` variable. There is
  **no attract mode or recorded-input playback** in the game. The main menu background (`Level_IN_MM`) is a live
  level with a `CameraRig_Rail` sequence (`IN_MM_Sequences`): a fixed, input-free camera move.

## 2. Engine facilities in the shipping eboot

Evidence is UTF-16 strings with their image addresses (01.02), from `eboot0102_utf16.txt` / `eboot0100_utf16.txt`.

| Facility | State | Evidence |
|---|---|---|
| Build configuration | **Shipping** | `PS4Game.ini` BuildVersionString "…Shipping 26"; `FApp::bIsBenchmarking` forced to 0 at 0x7530 (no `BENCHMARK` string anywhere) |
| Command line | read from `/app0/ue4commandline.txt` (one line, `fgets` 256), trailing spaces trimmed, `" -pak"` appended | code 0x1c3a7 (01.02); the file holds `../../../OVERBITE/OVERBITE.uproject ` |
| `-Deterministic` / `-UseFixedTimeStep` / `-FixedSeed` | **live** | parse at 0x7538 / 0x755c / 0x77e4; Deterministic sets both flags |
| `-FPS=`, `-benchmark`, `-ExecCmds=`, `-dpcvars` | compiled out | strings absent (ASCII, UTF-16 and UTF-32 searched) |
| Map on the command line | probably ignored (4.21 Shipping clears the URL override in `StartGameInstance`) | the message "The map specified on the commandline…" is still present (0x3e058e0); untested |
| `UEngine::Exec` | `FLUSHLOG GAMEVER STAT STOPMOVIECAPTURE CRACKURL DEFER OPEN STREAMMAP SERVERTRAVEL DISCONNECT RECONNECT TRAVEL GAMMA SCALABILITY DUMPTICKS CANCELASYNCLOAD` | 0x3e617b2-0x3e618aa; `CE` handler messages at 0x3e61aa4 / 0x3e61ae6 |
| `UWorld::Exec` | `TRACETAG FLUSHPERSISTENTDEBUGLINES LOGACTORCOUNTS DEMOREC DEMOPLAY DEMOSTOP DEMOSCRUB DEMOPAUSE DEMOSPEED` | 0x3e6880e-0x3e688e6 |
| Viewport / game engine | `SETRES HighResShot SHOT SCREENSHOT TOGGLE_FULLSCREEN`; `EXIT QUIT` | 0x3e08200-0x3e082c0; 0x3e05452 |
| FPS chart | **gone** (no `STARTFPSCHART`); only the cvar `t.FPSChart.DoCsvProfile` | 0x3deae50 |
| CSV profiler | strings only; `CSV_PROFILER` is non-Shipping in 4.21: assume compiled out | 0x3c8a78c |
| Automation / Gauntlet | none for the game (`AutomationTesting` / `bForceSmokeTests` are LaunchEngineLoop leftovers; "Gauntlet" is "The Gallows Gauntlet") | 0x3bf885e |
| CheatManager | code present: God, Ghost/Fly/Walk, BugItGo, ViewActor/ViewPlayer/ViewSelf, Summon, Teleport, ToggleDebugCamera (`ADebugCameraController` exists), CheatScript | 0x3debfac-0x3dec31a; `EnableCheats` native pair 01.02 0x526b310 → exec 0x2a43430 |
| Console UI | none in Shipping; `ConsoleKey=None` in DefaultInput.ini | `config0102/OVERBITE/Config/DefaultInput.ini:131` |
| Demo / replay recording | DemoNetDriver and the null / HTTP replay streamers are present; `DEMOREC` exists | 0x3e8fe7a, 0x3dd82f8. **Not useful:** a replay records replicated actors, and this single-player game's Blueprints do not replicate (not checked actor by actor) |
| Fixed frame rate (`bUseFixedFrameRate`, `FixedFrameRate`) | config properties exist; the project sets `bUseFixedFrameRate=False` | DefaultEngine.ini:23. 4.21 behaviour (waits real time and fixes the delta?) unverified; the time-step route below is certain |
| Custom time step | `UFixedFrameRateCustomTimeStep` is abstract in 4.21 (pure virtual `GetFixedFrameRate`) and no concrete subclass ships | 0x3d9864e |
| Sequencer | used everywhere: in-level cutscenes `BP_Sequence_Control` (`PlaysOnReplay`, `InvincibleDuringSequence`), the main menu rail camera, the train carts in 20_TD | `name_grep_0102.txt` |

### The fixed time step in detail (the determinism source)

`UEngine::UpdateTimeAndHandleMaxTickRate` (01.02, around 0x2752667-0x27526db):
`if (bIsBenchmarking || bUseFixedTimeStep) { DeltaTime = (float)FixedDeltaTime; CurrentTime = LastTime + DeltaTime; }`.
In this branch the engine does no frame-rate sleep, so the port's own limiter alone paces the presents. Game time
per frame is constant, so the same frame index shows the same simulation state, whatever each frame cost to
render.

| Global | 01.00 | 01.02 | Notes |
|---|---|---|---|
| `FApp::bIsBenchmarking` (bool) | 0x581ecf0 | 0x581ed30 | set to 0 at start (Shipping); read every frame with the next one |
| `FApp::bUseFixedTimeStep` (bool) | 0x581ecf2 | 0x581ed32 | `-UseFixedTimeStep` / `-Deterministic` |
| `FApp::bUseFixedSeed` (bool) | 0x581ecf1 | 0x581ed31 | `-FixedSeed` / `-Deterministic` (store at 0x757d-0x7589); then `RandInit(0)` / `SRandInit(0)` |
| `FApp::FixedDeltaTime` (double, 1/30) | 0x5601638 | 0x5601638 | confirmed by the helper below; `FApp::DeltaTime` is probably 0x5601640 (30 code refs) |
| fixed-step helper (`FFixedTimeStepCaptureStrategy`-like: `FixedDeltaTime = den/num`, `bUseFixedTimeStep = 1`) | 0x1c30620 | 0x1c332a0 | the clearing function follows it (`bUseFixedTimeStep = 0`) |

(Image offsets as in `src/level_hook.h`, each read from the `lea` / `mov byte` pairs of the command-line parse
(0x7529-0x7589, the same code offsets in both builds).)

Consequences:
- The command-line route gives 1/30 s per frame, because `-FPS=` is gone. At a 120 FPS cap the game then runs 4x
  faster than real time: the same frames each run, but nothing like normal play. Writing `FixedDeltaTime = 1/cap`
  (port code, Tier B) gives real-time speed while the cap holds. A slow frame then slows the game down instead of
  skipping ahead, so the work per frame index stays identical.
- Probably still nondeterministic: async level streaming and texture streaming (I/O timing decides the frame a
  sublevel or a mip arrives), `FMath::Rand` callers on worker threads (particles: interleaving), Wwise (real time,
  irrelevant to rendering), media players (real time).
- Side effect: with the game step at 1/30 and `r.GPUParticle.FixDeltaSeconds` at its 1/30 default, the GPU
  particle fixed step runs exactly once a frame (study-particle-fixed-step.md).

### Natives reachable by the port (`FNameNativePtrPair` table slots, image offsets; pair = {name, exec})

| Native | 01.00 pair → exec | 01.02 pair → exec | Use |
|---|---|---|---|
| `UKismetSystemLibrary::ExecuteConsoleCommand` | 0x51f6080 → 0x2986be0 | 0x51f6600 → 0x29899a0 (calls the implementation at 0x21a24c0, `(UObject* ctx, const FString&, APlayerController*)`) | general console injection |
| `UGameplayStatics::GetPlayerController` | 0x519f810 → 0x290d6d0 | 0x519fd90 → 0x2910490 | called often from Blueprints: a hook point that yields a valid world-context object |
| `UGameplayStatics::OpenLevel` | 0x519f930 → 0x290ed50 | 0x519feb0 → 0x2911b10 | already hooked inside (level_hook.h) |
| `APlayerController::EnableCheats` | 0x526ad90 → 0x2a40670 | 0x526b310 → 0x2a43430 | cheats exist |
| `UKismetSystemLibrary::IsPackagedForDistribution` | 0x51f63e0 → 0x2988f70 | 0x51f6960 → 0x298bd30 | gates the main-menu level loader and Dan's dev paths (do not flip globally) |
| `GetAllowDebugMenu` / `IsDemoSKU` | 0x4ed1810 → 0x6639c0 / 0x4ed1880 → 0x663bd0 | 0x4ed1810 → 0x665ca0 / 0x4ed1880 → 0x665eb0 | the card's trace slot (0x4ed1818 = pair + 8) |

The exec addresses for the 01.02 `ExecuteConsoleCommand` implementation are from a quick disassembly
(`call 0x21a24c0` after the `FFrame::Step` calls). Confirm them in Ghidra before writing a hook.

## 3. How the port could drive these

| Route | What it gives | Effort | Determinism | Notes |
|---|---|---|---|---|
| **A. `ue4commandline.txt`** `-UseFixedTimeStep -FixedSeed` (or `-Deterministic`) | fixed 1/30 game step, fixed seed | none to test (edit the game folder's file, with a backup); in production a per-run file mounted over `/app0/ue4commandline.txt` (`runtime_file_mount` takes the longest prefix, so an exact-file mount works), ~20 lines | high (game time per frame fixed) | 4x speed at 120 FPS; one line of at most 255 characters |
| **B. Port writes the `FApp` globals** (`bUseFixedTimeStep`, `FixedDeltaTime = 1/cap`, `bUseFixedSeed` before the seed is used: set it at start, or rely on A for the seed) | fixed step at real speed | ~30 lines in probe.c, byte-checked like the other hooks; addresses in both builds above | high | can be switched at a trigger (level loaded), so the menus and loading run normally |
| **C. Boot map override**: `[/Script/EngineSettings.GameMapsSettings] GameDefaultMap=/Game/Maps/Level_08_SV/Level_08_SV.Level_08_SV` in the override pak's engine ini (it overrides `DefaultEngine.ini`) | the game boots straight into a level, skipping the intro, movies and menus | ~10 lines in `ue4_override.py` plus a launcher or ini key; `GOW3_UE4_CVARS` only writes `[SystemSettings]` today | start point fixed (PlayerStart). State comes from the `@AUTOSAVE` slot (`SLOT_InitSaveData` loads it at game-instance init), so use a **dedicated benchmark `--user` folder** with its own fixed save (never the real save folder) | reaches levels the user never loaded; check Wwise / InitLevel without the intro map |
| **D. Frame-indexed pad replay** (`runtime_pad.c`: key samples by pad reads since a sync point instead of ms; sync = first pad read after the level load: boot map or `gow3_level_opened` + LoadMap) | Dan's route, combat and camera (MediEvil's camera areas follow Dan) | ~80 lines; adopt spacepepperoni's frame-stamped recorder (plan-work-queue row) | high only together with B or A | removes the "timestamp drift after loads" gap; the F9 conflict remains |
| **E. Auto stop**: after N frames from the sync point, write the flip trace and exit through the trace's writer (`Gow3FlipTrace::Write`, then `_Exit`; a process killed from outside writes nothing) | unattended runs | ~20 lines | n/a | or the game's own `QUIT` through F |
| **F. Console injection**: wrap a frequently called exec slot (`GetPlayerController`), take its `Context` object as the world context, call the `ExecuteConsoleCommand` implementation with an `FString` built on our stack (UTF-16, Num = Max = len+1) for each queued line from a file / env. Same technique as `GOW3_DEBUG_MENU_TRACE` | any command: `open <map>` (several levels in one boot), `EnableCheats` then `God`, `BugItGo x y z pitch yaw roll` (exact placement), `PlayersOnly` (freeze every non-player actor), `ToggleDebugCamera` (free camera), `Slomo`, `ce <event>`, any cvar (would replace the per-cvar table), `quit` | ~150 lines + Ghidra confirmation; medium risk (game thread, inside Blueprint execution; `open` is deferred, so that is safe) | depends on use | cheat availability in Shipping: code present, `AGameModeBase::AllowCheats` is true when standalone; **verify in game** |
| **G. Override-pak Blueprint edits** (same-length bytecode edits, as unlimited ammo) | "benchmark mode" defaults: `GetDontSpawn` → true, `GetAIPaused` → true, Invincible, `AllowDebugMenu` (exists) | ~30 lines each, byte variants per build | high for static scenes (no AI) | static-view benchmarks without AI noise |
| **H. Debug menu by scripted pad** (`GOW3_PAD_FILE`: hold `l1 r1`, tap `r3`, D-pad to the row, cross) | level select, POI teleport, freeze AI, invincible, without code | none | medium; menu focus order is fragile | manual use or a stopgap |
| **I. Main menu rail camera** (do nothing: boot, wait at the main menu) | a fixed, input-free camera move in a live level | none | high with A or B | light scene; a smoke test only |
| **J. DEMOREC / DEMOPLAY** | — | — | — | not viable (needs replication) |
| **K. Calling a Blueprint function by FName** (`LoadLevel`, `SetAIFrozen`): needs `FindFunction` + `ProcessEvent` addresses and the game-instance pointer | full control of the game's own functions | high | — | F does nearly all of this more cheaply |

## 4. Proposed automated benchmark

**Goal:** the same frames rendered every run, in a busy, representative stretch (panning and combat), measured by
the flip trace (0.1% lows, worst frames, spike counts), with no person at the pad.

### Pipeline (ranked by effort and determinism)

1. **Tier 1, highest value per line of code: B + C + D + E.**
   - Boot: run.py with a benchmark profile: a dedicated `--user` folder holding a fixed `@AUTOSAVE` (copied once
     from a chosen save, so inventory and options are fixed), and `GameDefaultMap` set to the chosen level in the
     override pak.
   - The port sets `bUseFixedTimeStep = 1`, `FixedDeltaTime = 1/cap` and `bUseFixedSeed` (via the command-line
     file, route A) at the sync point: the first game-thread pad read after the boot map has loaded.
   - Pad replay by frame index from the sync point: a route recorded once by the user (the recorder also keys by
     frame).
   - After N frames: write the flip trace and exit.
   - Output: the flip trace CSV, then `research/tools/flip_lows.py` and friends.
   - Determinism: the same game state per frame index, except async streaming arrival.
2. **Tier 0, tests that need no code (A + I, or A + H):** the command-line flags plus the main menu rail camera
   or a debug-menu POI teleport by hand. They prove the fixed step and measure how repeatable a run is before any
   code.
3. **Tier 2: F (console injection).** Static multi-spot runs (`BugItGo` list, `PlayersOnly`, HUD off) and
   free-camera paths (`ToggleDebugCamera` + replayed stick input). Several levels in one boot (`open`), and a clean
   `quit`.
4. **Tier 3: G (Blueprint defaults)** for AI-free static scenes, if F's cheats turn out to be blocked.

### Making runs comparable

- Warm-up: discard the first X seconds after the sync point (shader / pipeline compiles, streaming), or run the
  route twice in one boot and keep the second pass.
- Same build inputs between A and B: same `medievil.ini` (re-read before each launch), same benchmark save folder,
  no periodic logging (memory rule), launches one change apart.
- Determinism check for the method itself: two runs of the same exe should match frame for frame. Compare (a) a
  frame dump at the same frame index (`GOW3_FRAME_DUMP_TRIGGER`; check whether it can trigger on a frame index) and
  (b) the frame-time curves lined up by frame index.

## Open questions

1. Is the 1/30 fixed step from the command line taken at all (does the PS4 launch path reach the
   `ue4commandline.txt` read, or does the port pass argv first)? The read is skipped when an earlier source gave
   a command line (`jne 0x1c57e` at 0x1c3a1). Test T1.
2. Does a level booted as `GameDefaultMap` set up fully: Wwise banks, HUD, Dan's inventory from the autosave,
   music? The intro map's `CompleteSetup` waits for `InitialWWiseBankSwapComplete`; the game instance loads the
   global banks itself. Test T2.
3. Does `UGameInstance::StartGameInstance` ignore a map on the command line in this Shipping build (expected), or
   does it work (it would be cheaper than C)?
4. CheatManager in this Shipping build: does `EnableCheats` create the cheat manager (`AllowCheats` in standalone),
   and do `BugItGo` / `ToggleDebugCamera` / `PlayersOnly` run? Only testable after F.
5. Does MediEvil's camera manager fight a `BugItGo`-placed pawn or the debug camera (camera areas re-taking the
   view)?
6. How much nondeterminism remains with B (streaming, threaded random calls)? Measure with the two-run check.
7. Can `Level_Battlefield` / `Level_IN_AV` / `Level_00_AV` (sequence maps) be opened, and do they render anything?

## Test steps for the user (each launch needs your go; one change per launch)

- **T1, fixed step from the command line (no code).** Back up
  `E:\MediEvil\CUSA11227-0102\ue4commandline.txt` (copy it to `ue4commandline.txt.orig`). Replace its one line with
  `../../../OVERBITE/OVERBITE.uproject -UseFixedTimeStep -FixedSeed` and launch as usual.
  Expected: at the main menu, and in a level at a 120 cap, everything (rail camera, flames, Dan) moves about 4x
  real speed (1/30 s per frame). If it looks normal, the flag was not read (open question 1).
  Then restore the original file.
- **T2, repeatability baseline (no code, after T1 works).** With the T1 file, sit at the main menu for 60 s with
  the flip trace on, twice in two launches. Compare the two traces by frame index. This also measures how
  repeatable the method itself is.
- **T3, boot map (needs the small `ue4_override.py` change, proposed separately).** Launch with
  `GameDefaultMap` = a level you have not reached (e.g. `Level_15_CC`), using a benchmark user folder (not the
  real saves). Check: Dan spawns and moves, the HUD shows, music and sounds play, and the pause menu works.
- **T4, Tier 1 end to end** (after B/D/E are built): record a 60 s route once, replay it twice, and compare 0.1%
  lows and worst frames between the two identical runs. The spread between them is the noise floor for later
  A/Bs.

## Files and tools

- Scripts (read-only, `research-data/debug-menu-research-20261124/scripts/`): `kismet.py` (a Kismet disassembler
  for 4.21 cooked bytecode: on disk an object reference is 4 B and a name 8 B; in memory a pointer is 8 B and an
  `FScriptName` 12 B; jump targets are memory offsets. Every function in DebugHUD, OverbiteGameInstance(+Functions)
  and Dan decodes with matching sizes), `pakindex.py` (cached index of all paks), `name_grep.py` (regex over every
  package's name map), `datatable_dump.py`, `extract_files.py`, `strings16.py`, `hdr.py`, `rawfunc.py`.
- Data: `files_0102.txt` / `files_0100_e.txt` (all pak files), `dis0102/*.txt` (disassemblies: DebugHUD,
  DebugLevelLoadHUD, OverbiteGameInstance(+Functions), BP_df_character, PauseMenu, BP_TrophyManager, MainMenu*,
  Level_IN_MM, Level_AV_*, …), `dis0100/DebugHUD*.txt`, `config0102/` (the game's ini files),
  `eboot010x_{utf16,ascii}.txt` (strings with addresses), `name_grep_0102.txt`.
