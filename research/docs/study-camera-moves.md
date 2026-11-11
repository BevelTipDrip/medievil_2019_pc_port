# MediEvil camera: every automatic move, per area class (research, 2026-11-11, no launch)

Switches built from this: section 6 and card `ue4/camera-moves`. The user's design (2026-11-11): every forced
move gets a switch; **Hybrid** = the game keeps only the fixed / scripted cameras, the free areas behave as
**Free**; run zoom and vertical easing are separate switches.

Source: 01.02 paks (`E:\MediEvil\CUSA11227-0102`) and eboot (`out/eboot.elf`), Kismet 4.21 bytecode decoded with
the scratchpad tools. Offsets are **in-memory script offsets** (FScriptName 12 bytes, pointers 8, `NM=12`) unless
marked `uexp` (byte offset in the 01.02 `.uexp` file, for a pak edit). Tags: **[B]** read in bytecode or
disassembly, **[I]** inferred.

Research agent's notes (2026-11-11), kept as written; the extracted packages, per-function disassembly, a dump of
every placed area with its instance properties (38 maps) and the scratch tools (pak index, Kismet walker with mem /
uexp offsets, tagged-property dumper) were in the session's scratchpad, not in the repository.

## 1. How the camera works, and which area is active

- `BP_CameraManager` (an actor, not UE4's PlayerCameraManager) keeps `CameraAreaStack`. **Active area =
  last element's `GetParentArea()`** (`GetTOSCameraArea` / `GetCurrentOrMakeNewCamera`) [B]. `GetParentArea` is
  `self` in the base [I: base export 44 did not decode] and `Parent` in `BP_CameraAreaExtend` [B], so the 104 placed
  Extend volumes act as their parent area.
- Push: the area's `Area` component begin-overlap with Dan -> `FindCameraAreaByID`; if not already on the stack,
  `PushCameraOnStack` [B]. Pop: end-overlap (not while Dan is dead) [B]. `PushCameraOnStack`: if the parent is
  already TOS -> push without transition; else `ActiveTime = 0`, `StartCameraCrossFade` (blend or cut fade) and push
  [B]. A DanCam on top stays on top (new area is swapped under it) [B]. `isEnabled=false` turns the Area collision
  off (`SetEnabled`) [B]. Empty stack -> actor tagged `LevelStartupCameraArea`, else a spawned `BP_CameraAreaFree` at
  z=10000 [B]. `PushInitialCameraAreaOverlap` pushes every area Dan already overlaps [B].
- `TickCamera` (cm export 34) [B]: skip while `isSequencePlaying`; `area.ActiveTime += dt`;
  `area.ComputeCamera(IsPlayerControlActive(), dt, controlYaw)`; then
  - not blending: rotation.yaw += `GetVelocityYaw(dt, area.VelocityYawWeight)`; `ApplyCameraPosition`; then
    `SetControlRotation(0, 0, ComputeCamera yaw)` (**without** the velocity offset);
  - blending (`BlendTime < BlendDuration`): `VInterpTo/RInterpTo(PreviousCamera*, computed + velocity yaw, BlendT)`,
    `PostProcessCamera`, apply; no `SetControlRotation`.
- `IsPlayerControlActive` = `VSize2DSquared(Dan.AnalogMovementInput) >= 0.0484` (stick past 0.22) [B].
- Native `MedievilSpringArm.ComputeCameraSimple` (impl 0x6366e0, exec thunk 0x65d570, 01.02) [B, disassembly]:
  sweeps / probes from the origin along `rot.Vector() * boom` and eases the boom length; **`outrot` is the input
  rotation copied unchanged** (0x6369f1-0x6369ff -> 0x636f3c-0x636f4f), **`outyawprevented` is only ever written 0**
  (0x636a07), and the `InputRotation` / `JumpingZ` float arguments (xmm6 / xmm7) are never read (not saved before
  the first call). So the native arm never turns the camera; it only shortens the boom against geometry.
- `TestInputBits(256)` = **strafe held**: `ActivateStrafe` (ubergraph entry 13177 -> 0xbce) sets bit 256,
  `DeactivateStrafe` (13182 -> 0x2cf3) clears it; `GetIsStrafing` tests it [B, BP_df_character export 13 / 337].

## 2. Area classes

Placed instances counted in all 01.02 level packages (`level_areas.json`). "Player yaw" = does the player's input
turn this camera at all.

| Class (parent) | Placed | Player yaw | Kind | Hybrid keeps? |
|---|---|---|---|---|
| BP_CameraAreaFree (BaseClass2) | 29 (+ manager fallback) | yes | free orbit | no (Free rules) |
| BP_CameraAreaFreeSuggestDirection (Free) | 31 (18 with `UseSuggest=false`) | yes | free + entry nudge | no (Free rules) |
| BP_CameraAreaFreeSimple (BaseClass2, copy of Free) | 1 (06_PG `StartingCam`) | yes | free orbit | no (Free rules) |
| BP_CameraAreaKardok (Free) | 1 (21_ZL Zarok arena) | partly | boss arena, looks at boss | yes (scripted) |
| BP_CameraAreaBPCamera (BaseClass2) | 111 (+46 via Extend) | no | fixed / spline cam, LimitedLookAt at Dan | yes |
| BP_CameraAreaPirateBoss (BPCamera) | 2 (+3 via Extend) | no | boss fixed cam | yes |
| BP_CameraAreaDanSpline ("Depreciated" folder, still used) | 51 (+42 via Extend) | no | spline / fixed cam, LimitedLookAt | yes |
| BP_CameraArea2Spline (BaseClass2) | 8 (+9 via Extend) | no (`AllowUserCameraControl=false`) | camera on rail, look at Dan | yes |
| BP_CameraAreaSplineConstraint (BaseClass2) | 1 (04_GY2) | no (pitch only) | rail, yaw = look at Dan | yes |
| BP_CameraAreaCircle (BaseClass2) | 1 (+3 via Extend) (02_CH) | no (pitch only) | orbit a centre object | yes |
| BP_CameraAreaFixed (BaseClass2) | 5 | no | fixed, optional look-at | yes |
| BP_CameraAreaFixed_HideObjects (Fixed) | 1 (11_EE) | no | fixed | yes |
| BP_CameraAreaFixedAntQueen (Fixed) | 1 (12_AC boss) | no | fixed boss cam | yes |
| BP_CameraAreaMechaImp (Fixed) | 0 (pushed by MechaImp trigger) | no | fixed, picks 1 of 2 cams | yes |
| BP_CameraAreaGuardians ("Depreciated") | 1 (04_GY2) | no | scripted | yes |
| BP_CameraAreaConst (BaseClass) | spawned by `MakeConstCamera` (Interactive_Parent) | no | fixed location / look-at | yes |
| BP_CameraAreaRedirector (BaseClass) | 0 | - | forwards to `RedirectButton.Target` | - |
| BP_CameraAreaExtend (BaseClass) | 104 | - | volume that resolves to `Parent` | as parent |
| BP_CameraAreaDanCam (BaseClass2) | spawned by `BP_CameraManager.DanCam` (player button) | yes (control yaw) | over-the-shoulder aim cam | yes (player-initiated) |

**Fixed / scripted (Hybrid keeps):** every class except the Free family (Free, FreeSuggestDirection, FreeSimple).
Kardok is Free-derived but its `ComputeCamera` is its own (does not call Free's) [B]; DanCam is player-initiated.
Hybrid can decide by the class of `GetTOSCameraArea()` (already parent-resolved), but with the interventions below
no runtime class test is needed: they only touch Free-family functions, plus `GetVelocityYaw`, whose weight is 0 on
every non-free class (all CDOs; the only 2 instance overrides set it to 0) [B].

### 2.1 Free family (BP_CameraAreaFree; FreeSuggestDirection inherits it; FreeSimple is a near copy)

CDO: `VelocityYawWeight 0.025`, `UserPitchMin -65 / Max -5`, `UserPitchAutoInterpolationSpeed 0.5`,
`UserPitchInterpolationSpeed 100`, `RunStartModBoom Away -0.25 / Towards -0.4`, `RunStartModPitch Away -0.8 /
Towards -0.8`, RunEnd* / YawStart* / YawEnd* = 0, BaseClass2 `BoomLength 900`, `Pitch -20`, base `CameraBlendTime 1`
[B]. Instances mostly override only BoomLength (300-2000) and Pitch (-65..+5); one overrides RunStart*
(09_AG `MazeWander`: boom 0, pitch -0.2) [B].

Yaw formula (ComputeCamera, export 20, 01.02) [B]:
`moving = IsPlayerControlActive`;
`follow = (VSizeXY(focus - cameraLoc) < 400 || strafeHeld) ? controlYaw : atan2(focus - cameraLoc)`;
`yaw = moving ? follow + 2*InputRotation : ((ActiveTime > 1.5*CameraBlendTime || !RecenterInProgress) ? controlYaw : DanYaw)`;
`yaw = ProcessKeepOuts(yaw)`; pitch = `SmoothedLookUpAxisValue + PitchEffective`; boom = `BoomLengthEffective`.

| # | Automatic move | Where (01.02, mem offset) | Decided by / typical | Off-switch for Free (smallest) | Hybrid |
|---|---|---|---|---|---|
| F1 | **Chase / follow yaw** while Dan moves: camera yaw = direction from last camera position to Dan, so it swings after him | free export 20 `ComputeCamera` 0x04b0 `Less(VSizeXY, 400.0f)` and the select at 0x0554 [B] | moving (stick > 0.22), camera XY distance >= 400 (boom 900 / pitch -20 gives ~845, so the usual case), not strafing. Rate up to ~55 deg/s at full sideways run (812 cm/s at ~845) [I] | float `400.0f` -> `1e30f`: mem 0x04cd, uexp 0x0f7f (FreeSimple export 20: mem 0x044a, uexp 0x0f31) [B]. Forces the "near" branch = control yaw | off |
| F1b | (side effect of F1-off) moving turns faster: near branch = control yaw (2.5 x stick) **plus** 2 x InputRotation = 4.5 x stick vs 2.5 standing | same function, `Multiply(InputRotation, 2.0f)` at 0x0214 [B] | - | float `2.0f` -> `0.0f`: mem 0x024f, uexp 0x0dfd (FreeSimple mem 0x01cc, uexp 0x0daf) [B]. Then moving = standing = control yaw only (mouse hook: only the control-yaw half is then needed) [I] | off |
| F2 | **Velocity yaw**: yaw offset = Dot(DanVelocity, cameraRight) x weight, eased (FInterpTo speed 1); added to the shown rotation only, not to control yaw | cm export 34 `TickCamera` 0x0221 / 0x047b, cm export 63 `GetVelocityYaw` [B] | `area.VelocityYawWeight`: 0.025 on Free / Suggest / FreeSimple CDO, 0 on all other classes; instance overrides only 0 (18_GS `FreeRotator`, 00_DC SplineConstraint_Si). Full sideways run 812 x 0.025 = ~20 deg, shoulder charge ~40 deg [B values / I effect] | (a) runtime: write 0 to `VelocityYawWeight` (float on BP_CameraAreaBaseClass) of the TOS area; or (b) bytecode: `GetVelocityYaw` jumpifnot at 0x0022, target u32 at mem 0x0023 (`0x67`) -> `0x30` (both paths reach `VelocityYawRecent = 0`); uexp: op at 0x4aa4, target at 0x4aa5 [B]. Weight 0 resets the offset instantly [B] | off (non-free areas are 0 anyway) |
| F3 | **Recentre latch** (01.02 only): standing still during the first 1.5 x CameraBlendTime of an area's `ActiveTime`, yaw = Dan's facing if `RecenterInProgress` | export 20 0x0060 (`1.5f` at mem 0x0085, uexp 0x0ceb), 0x05da [B] | `RecenterInProgress` is set by R3 (`RecenterCamera` 0x0190 / `DelayRecenter`) and **never cleared** [B]; `ActiveTime = 0` on every crossfade push [B] -> after one R3 in an area, re-entering it while standing snaps toward Dan's facing for ~1.5 s [I] | runtime: when `ActiveTime > 1.5*CameraBlendTime`, write `RecenterInProgress = false` (bool on the Free class). Not a literal: changing `1.5f` or the push's `ActiveTime = 0` also kills R3 / DanCam alignment [I] | off |
| F4 | **Suggest direction**: for `SuggestInfluenceTimer` s after entry (`ActiveTime`), camera = the area's Arrow yaw if `AlwaysForceDirection` or the camera faces against the arrow | suggest export 5 `ComputeCamera` 0x0037 `jumpifnot 0x538 (UseSuggest)` [B] | CDO `UseSuggest true`, timer 1.0; 13 of 31 instances active (timers 0.2-1.2; 6 with AlwaysForceDirection, e.g. 22_HH, 09_AG bell fight) [B] | op 0x07 -> 0x06 at mem 0x0037 (uexp 0x01cb): unconditional jump to the plain Free result [B]; or runtime `UseSuggest = false` | off (or keep as an option: it is a level-design nudge) |
| F5 | **Keep-out push**: yaw clamped to the tangent of a post / guard rail / cylinder when the boom would pass through it | base export 26 `ProcessKeepOuts` (called only from Free / FreeSimple ComputeCamera) [B]; `BP_CameraKeepOutPost.ConstrainYaw` [B], GuardRail native `ConstrainYawByGuardRail` [B] | area `KeepOuts` array: only 2 placed Free areas have one (13_PD `BP_CameraAreaFree_2`, 07_PS_Boss), both a post [B] | op 0x07 -> 0x06 at base export 26 mem 0x00af (uexp 0x24ac): loop never runs, yaw unchanged [B]; note `Radius = 0` is **not** a no-op [I] | off (or keep: 2 areas, prevents a pillar between camera and Dan) |
| F6 | Area **transition blend**: RInterpTo from the previous camera rotation over `CameraBlendTime` (1 s; 4 instances 2 s), or a cut fade when the trace is blocked / far | cm `TickCamera` blend branch, base `PushCameraOnStack` / `PopCameraOffStack` / cm `StartCameraCrossFade` [B] | Free -> Free: new yaw = control yaw, so only pitch / boom blend [I] | none needed | keep |
| F7 | `DanYaw` on manual R3 recentre | free export 56 `RecenterCamera` (fade + `ActiveTime` window) [B] | player input (R3) | keep (manual) | keep |

Pitch / boom (the separate "run zoom" switch), same class [B]:

| # | Automatic move | Where | Typical | Off-switch |
|---|---|---|---|---|
| P1 | **Run start zoom/tilt**: while moving (and not yawing), `BoomLengthGoal = BoomLength x (1 + d x RunStartModBoom*)`, `PitchGoal = Pitch x (1 + d x RunStartModPitch*)`, d = stick direction . camera forward (recomputed every tick while moving); `*Effective` eased to the goals (FInterpTo speed 1) | free export 24 `CheckMovingState` 0x0344..0x0b2f; ease at 0x000f | running away: boom 900 -> 675, pitch -20 -> -4; towards the camera: boom 1260, pitch -36 [I from CDO] | op 0x07 -> 0x06 at mem 0x0344 (uexp 0x182a; FreeSimple same mem, uexp 0x1803): `IsMoving` never set, run-start and run-end blocks skipped [B]; or runtime: zero the 4 `RunStartMod*` floats (CDO + one instance) |
| P2 | Run end: on stop, goals = base x (1 + facing-dot x RunEndMod*) | `CheckMovingState` 0x0b30..0x1051 | RunEndMod* = 0 everywhere -> back to base [B] | covered by P1 patch |
| P3 | Yaw start / end mods (`IsYawing` while turning standing) | 0x00be..0x02a4, 0x1052.. | YawStart/End mods 0 everywhere [B] -> no effect | none needed |
| P4 | **Pitch auto-return**: while moving or attacking and the pitch stick is centred, `TargetLookUpAxisValue = 0` and the user pitch offset eases to 0 (FInterpTo, `UserPitchAutoInterpolationSpeed 0.5`) | free export 7 ubergraph 0x0250 `jumpifnot 0x275 (moving OR attacking)` [B] | CDO 0.5 everywhere | op 0x07 -> 0x06 at mem 0x0250 (uexp 0x0695; FreeSimple uexp 0x0699; **01.00 at mem 0x022b**) [B]. Setting the speed to 0 is wrong (FInterpTo snaps at speed <= 0) and a tiny speed still zeroes the target [I] |

### 2.2 Kardok (Zarok arena, Free-derived, own ComputeCamera, export 16) [B]

Yaw: `LookTowardsBoss` (export 31): with `AllowCameraMoves`, RInterpTo_Constant 30 deg/s toward "away from boss"
while `InputRotation == 0`, else control yaw; without `AllowCameraMoves`, locked to the boss line. When the boss is
valid, location / rotation blend (BlendCam over 1 s) to a fixed orbit at `Boom 2900`, `Zheight 1800`; also pushes Dan
back inside the KeepInCylinder and animates FOV 70->85 and the cylinder scale. CDO `VelocityYawWeight 0`,
RunStart* 0. Inherits Free's tick (P4 pitch auto-return applies). **Scripted boss camera: keep in Hybrid**; in Free,
turning it off would need a replacement camera (see section 4).

### 2.3 Fixed / scripted classes [B]

- **BPCamera** (export 10): placed `BP_Camera` actor; `UseSpline` / `UseDanSpline` pick a point on a spline;
  `LimitedLookAt` (cm) at the focus when `ShouldLookAtPlayer`, optional `isFixedBoomLength` / `isOverrideWithCamera
  Rotation`. No InputRotation (only the "no camera" HUD icon on input). PirateBoss = BPCamera + `NotifyBattleBegins`
  on overlap.
- **DanSpline** (export 10): same idea (spline point + LimitedLookAt). **2Spline** (export 17):
  `FindSplinePointForBoom` (native) + look at Dan / along spline. **SplineConstraint** (export 17): yaw = look-at
  from the spline point to Dan, pitch from the user's stick. **Circle** (export 12): yaw = atan2(centre - Dan).
- **Fixed** (export 10): placed camera location / rotation or LimitedLookAt (`UseYawLimit`, `YawLimit`);
  AntQueen adds an offset from Dan's position; MechaImp picks the nearer of two cameras.
- **Const**: fixed location, rotation or look-at (`MakeConstCamera` from `Interactive_Parent.EnableCamera`).
- **DanCam** (export 14): yaw = Dan's facing for 1.5 x 0.2 s after push, then control yaw; pitch = stick x 40.
- Every one of these moves the yaw by itself (look-at / spline tracking): there is no "off" short of replacing the
  camera.

### 2.4 One-shot control-rotation writes outside the camera classes [B]

`BP_Portal_Parent.RotateCameraOnExit`, `BP_Sequence_Control.TeleportToNewLocation`, `BP_StoneGolemCameraPop`,
`BP_Wolf_BronzeController` (also `SetCPitch(0)` / `SetCBoomLength(1000)` on the bottom area),
`Interactive_Parent` (restores `PreviousControlRotation` after its const camera). Event snaps (teleport / cutscene /
boss): keep in every mode. `RecenterCamera` is reached only from the input action (`BP_InputDelegateActor`
`InpActEvt_RecenterCamera`, BP_df_character 0x445e).

## 3. Recommended intervention sets

All one-byte / one-float patches keep the script length, so they work as a runtime write into the loaded
`UFunction::Script` (mem offsets) or as a pak edit of the same-size package (uexp offsets; pakchunk0 files are
stored uncompressed). Verify the original bytes first, as `mouse_hook.h` does.

- **Hybrid** = F1 (400 -> 1e30) + F1b (2.0 -> 0) + F2 (velocity yaw) + F3 (latch, runtime) + F4 + F5 (optional),
  in Free and FreeSimple. Nothing else changes: fixed classes never run these functions.
- **Run zoom / tilt switch** (separate): P1 (CheckMovingState 0x0344). **Pitch auto-return switch**: P4
  (ubergraph 0x0250) - probably wanted with mouse pitch.
- **Free** = Hybrid + replace the scripted cameras (section 4).
- **Original**: none.

Runtime vs pak: a pak edit fixes the mode at launch (an override `_P` pak per mode, chosen by the launcher), no
runtime patcher needed, but 5 packages (Free, FreeSimple in pakchunk6, Suggest, cm, base) must be shipped per build
and the 01.00 packages differ (below). A runtime patcher allows switching in game and per-build pattern checks.

## 4. Free mode in scripted areas (design, not yet researched in depth)

Fixed / spline / boss areas give the player no yaw at all, so "only the player turns the camera" there means
computing the camera with a free area instead. The manager already does this when the stack is empty
(`GetCurrentOrMakeNewCamera` spawns a `BP_CameraAreaFree`). A hook could make `TickCamera` call `ComputeCamera` on
one persistent hidden free area whenever the TOS class is not Free-family (keeping DanCam, Const and perhaps the
boss areas), leaving the placed areas on the stack so their overlap scripts (e.g. PirateBoss `NotifyBattleBegins`)
still run. Risks [I]: rooms built for a fixed view (walls / missing geometry behind the camera, 19_EH has 12
DanSpline areas), boss arenas (AntQueen, MechaImp, Pirate, Kardok), level art with `BP_CameraAreaFixed_HideObjects`
hiding objects only for its view, and the free area's `ActiveTime` / blends.

## 5. Risks and unknowns

- **Build differences [B]:** 01.00 `BP_CameraAreaFree` differs: no `RecenterInProgress` / Dan-yaw select (F3 is
  01.02 only), ComputeCamera export 19 with `2.0f` at mem 0x011c (uexp 0x0c75) and `400.0f` at mem 0x039a (uexp
  0x0df7); ubergraph pitch jump at mem 0x022b; CheckMovingState export 23 jump at mem 0x0344 (uexp 0x1683);
  cm `GetVelocityYaw` export 63 same mem 0x0022 (uexp 0x4aa4). Suggest / base / FreeSimple 01.00 not checked.
- F1b changes the stick turn rate while moving from 2 x to 2.5 x (same as standing) and makes the mouse hook's
  InputRotation half a no-op in free areas [I]; whether control yaw already holds this frame's input when
  `TickCamera` reads it is still inferred (tick order).
- Without F1 the camera never chases Dan: on long runs toward / away from the camera nothing changes, but running
  sideways now circles Dan around a fixed yaw [I]. Areas tuned with SuggestDirection or follow to show the path
  (13 active suggestions) lose that guidance.
- Velocity yaw also skews Dan's camera-relative movement input today (movement uses the shown camera rotation)
  [I]; removing it changes steering slightly.
- `ProcessKeepOuts` off: the spring arm still shortens against the post, so the camera may zoom in instead [I].
- Unaligned 4-byte float writes into `Script` should be done on the game thread (or between ticks) [I].
- `BP_CameraAreaBaseClass.GetParentArea` (export 44) and `RecenterCamera` (export 89) did not decode (kis.py blob
  heuristic); their behaviour is inferred from use.
- Not covered: camera shakes (`CameraShake_*`, separate from yaw), FOV changes (Kardok), sequences (`isSequencePlaying`
  stops TickCamera), `ComputeStickyZ` (height only).

## 6. First build: `GOW3_CAMERA_OFF` (override pak, 2026-11-11; replaced by section 7)

Comma-separated moves to turn off; each is a same-size `.uexp` edit (count-checked, one variant per build where they
differ), all or nothing per move, a move whose bytes are missing is skipped with a log line. Checked against both
builds' files (E:\MediEvil\CUSA11227 = 01.00, CUSA11227-0102): every move applies; changed offsets below.

| Move | Research # | Files | 01.02 uexp | 01.00 uexp |
|---|---|---|---|---|
| `follow` | F1 + F1b | Free, FreeSimple | Free 0xf7f-0xf82 (400 -> 1e30), 0xe00 (2.0 -> 0); Simple 0xdd5, 0xc56 | Free 0xdf7, 0xc78; Simple same |
| `swing` | F2 | BP_CameraManager | 0x4aa5 (0x67 -> 0x30) | 0x4aa5 |
| `suggest` | F4 | FreeSuggestDirection | 0x1cb | 0x1cb |
| `keepout` | F5 | BP_CameraAreaBaseClass | 0x24ac | 0x24f6 |
| `run_zoom` | P1 | Free, FreeSimple | Free 0x182a; Simple 0x1661 | Free 0x1683 |
| `pitch_return` | P4 | Free, FreeSimple | Free 0x695; Simple 0x65c | Free 0x658 |

FreeSimple (the same file in both builds, a copy of 01.00's Free; the research's FreeSimple uexp offsets were wrong)
and 01.00's `pitch_return` / `keepout` sites were found by the same bytes and the same jump targets, not
disassembled [I]. F3 (recentre latch) is not built: the user will judge it in play.

**T-CM1** (2026-11-11, PC, 01.02, `36deef0`, all six moves off through the pak, log `data/camera-moves-t-cm1.log`):
the user: "everything seems to work great". The launcher line confirmed "camera moves off: follow, swing, suggest,
keepout, run_zoom, pitch_return".

## 7. Built: live switches in the menu's Controls tab (`10beddc`, 2026-11-11)

The user asked for the switches live in the in-game menu, in a new Controls tab. The override-pak route was removed
(pak-edited bytes would hide the originals from the live hook). `src/camera_hook.h` / `probe.c`:

- Hook: `UMedievilSpringArm::ComputeCameraSimple`'s exec, slot 0x4ecc7f8 in both builds (name string 0x3c5627a /
  0x3c5967a, thunk 0x65b290 / 0x65d570 for 01.00 / 01.02; checked: slot -> thunk, slot - 8 -> name, thunk prologue).
  Callers (camera package name tables): Free, FreeSimple, FreeSuggestDirection (through Free's), Kardok, DanCam,
  SplineConstraint.
- From the call's FFrame: Node = the calling ComputeCamera (Outer = area class), Object = the area. Searched once
  each: the area class's own functions (follow, run_zoom, pitch_return), its parent chain (keepout), the area's class
  below it (suggest), the camera manager class = Outer of the InputRotation property in the x 2.0 statement (swing).
  UStruct Children +0x38, UField Next +0x28 (inferred from the 4.21 layout; Script +0x48 and SuperStruct +0x30 were
  already used by the mouse hook).
- The patterns are the pak ones (jump targets are in-memory offsets on disk too); counted in every extracted camera
  package: unique in their target classes. SplineConstraint has the x 2.0 bytes once and no 400 test, hence the
  "together" rule for the two follow edits.
- Settings: `medievil.ini` `camera_follow` ... `camera_pitch_return`, 1 = the game's move (default); the launcher keeps
  unknown keys. Fix built, not run.
