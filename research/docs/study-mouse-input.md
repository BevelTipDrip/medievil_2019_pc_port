# Study: raw mouse camera and mouse menus (2026-11-03)

Research only: no build, no launch, no code change. The question (the user's words): real raw-mouse camera
control, not a clunky "mouse to joystick" feel, plus mouse navigation in the game's menus; 8000 Hz is probably
excessive. Everything below was read statically from the 01.02 eboot (`out/eboot.elf`, written by `prepare.py`
from `E:\MediEvil\CUSA11227-0102`; its r.SetRes slot is 0x5c28368, the 01.02 value in `scripts/ue4_override.py:35`)
and from the paks of both builds. Addresses are guest offsets (ELF virtual addresses, image base 0, the same space
as `ue4_override.setres_slot`); add 0x400000 for the Ghidra project.

Markers: **read** = seen in the eboot, a pak or source; **inferred** = reasoning, not seen. Nothing here is
"verified" in this project's sense (the user confirming it in game); every claim that matters has a test in the
step plan (section 7).

## 1. Summary

| Question | Answer |
|---|---|
| Is the PS4 mouse path compiled in? | **Yes (read).** UE4 4.21's PS4 input code calls `sceSysmoduleLoadModule(0xA9)`, `sceMouseInit`, `sceMouseOpen` (after each `scePadOpen`) and `sceMouseRead(handle, buf, 8)` once per input poll, behind the cvar `PS4.EnableMouse` (default 0, "Cannot be changed at runtime"). Both 01.00 and 01.02 contain it. |
| What does UE4 do with it? | **Read:** per entry, moves the platform cursor by delta x `PS4.MouseSensitivity`, calls `MessageHandler->OnRawMouseMove(dx, dy)` with the raw delta, `OnMouseWheel`, and `OnMouseDown / OnMouseUp / OnMouseDoubleClick` for 5 buttons. **Inferred (UE4 4.21 source):** Slate turns raw moves into `EKeys::MouseX / MouseY` only while the game viewport has mouse capture. |
| Is the mouse bound to the camera? | **No.** `DefaultInput.ini` maps `MouseX -> Turn` and `MouseY -> LookUp` (left over from the UE4 template), but the game's input actor `BP_InputDelegateActor` binds only `TurnRate`, `LookUpRate`, `MoveForward`, `MoveRight` (read). Nothing listens to `Turn` / `LookUp`. |
| How does the game turn the camera? | **Read (names) / inferred (logic):** `BP_df_character` handles `OnInputAxisTurnRate` with `BaseTurnRate`, the options menu's `CameraTurnRate`, `GetWorldDeltaSeconds` and `AddControllerYawInput`: the UE4 template's rate path (stick x rate x dt). Pitch is a position, not a rate: `BP_CameraAreaFree` turns the stick into `TargetLookUpAxisValue`, smooths it (`FInterpTo`, `UserPitchInterpolationSpeed`) into a pitch offset within `UserPitchMin/Max`. |
| Recommended design | (1) Implement `sceMouse` in the runtime from SDL3 relative mode, accumulated the moment events arrive, and enable `PS4.EnableMouse`: menus get the engine's own cursor and clicks. (2) Camera yaw: add the mouse's counts x sensitivity to the game's own `AddControllerYawInput` call (guest hook): exact, frame-rate independent, and silent whenever the game blocks the stick. (3) Pitch: chosen after a test (control pitch if the camera uses it, else a held virtual stick position). No dedicated 8000 Hz thread. |
| From MGS2 | The game samples once a frame, so a faster reader buys nothing; keep win32k calls off the game and render threads (an 8000 Hz mouse made each one 20-30x slower); read in batches, not per report; cut counts at a steady point of the frame if per-frame counts turn out uneven; build a verify sweep before tuning by feel. |
| From Bloodborne | Its mouse look is right-stick emulation with a 60 ms decay and a 16/127 dead-zone offset: exactly the "mouse to joystick" feel to avoid. Its capture rules (click takes the mouse, F1 / focus loss / menu releases it, relative mode) are worth copying. |

## 2. sceMouse: the API and how UE4 uses it

### 2.1 Imports and the runtime today

| Where | What |
|---|---|
| `src/import_names.inc:253-258` | `sceMouseInit`, `sceMouseClose`, `sceMouseOpen`, `sceMouseRead` (libSceMouse), between `sceSysmoduleLoadModule` and the `scePad*` imports. |
| `src/runtime_services.c:546-554` | `mouse_open` returns a new id; `mouse_read` zeroes 40 bytes and returns 1 (connected = 0); `mouse_close` returns 0. |
| `src/runtime_services.c:716` | the four exports (`sceMouseInit` = `ok_void`). |

The game never gets that far today: with `PS4.EnableMouse` at 0 it does not call `sceMouseOpen` or `sceMouseRead`
(read, below), so the stub's "absent mouse" answer is never consulted.

### 2.2 The data layout (read from the consumer's offsets)

`sceMouseRead(handle, SceMouseData *data, 8)` returns the number of entries; each entry is 40 bytes (the loop
indexes `rbx = i*5`, `[rbp + rbx*8 - 0x190]`):

| Offset | Field | Use in the game (guest+0xb759a4..0xb75c9e) |
|---|---|---|
| +0x00 | `uint64 timestamp` | not read |
| +0x08 | `bool connected` | entry skipped when 0; OR of all entries stored at `[app+0xaa0]` ("a mouse is connected") |
| +0x0c | `uint32 buttons` | entry skipped when negative (bit 31 = `SCE_MOUSE_BUTTON_INTERCEPTED`); compared with the previous buttons through a 5-pair table at guest+0x3cc34e0: `1 -> Left, 2 -> Right, 4 -> Middle, 8 -> Thumb01, 16 -> Thumb02` |
| +0x10 / +0x14 | `int32 xAxis / yAxis` | relative counts (below) |
| +0x18 | `int32 wheel` | `OnMouseWheel((float)wheel)` when non-zero |
| +0x1c | `int32 tilt`, +0x20 `reserve[8]` | not read |

`SceMouseOpenParam` is one byte set to 1 (`behaviorFlag`, merged: all mice as one) and the call is
`sceMouseOpen(userId, 0 /* standard port */, 0, &param)` (guest+0xb7092d..0xb70934, the same at +0xb70a72 and
+0xb74af7). The PLT stubs were resolved from the eboot's own relocation table: guest+0x3bf3690
`sceSysmoduleLoadModule`, +0x3bf5840 `sceMouseInit`, +0x3bf5850 `sceMouseOpen`, +0x3bf5860 `sceMouseRead`,
+0x3bf58b0 `scePadOpen`.

### 2.3 The engine side (read)

`PS4.EnableMouse` is an `FAutoConsoleVariableRef` on the int at guest+0x5b1d2b0 (registration guest+0xb77eb7..
0xb77ed2, flags 4 = `ECVF_ReadOnly`: the console cannot change it, an ini can). Its readers:

| Site | What it gates |
|---|---|
| guest+0xb71361 (input interface constructor) | `sceSysmoduleLoadModule(0xA9)` then `sceMouseInit()` |
| guest+0xb70918, +0xb70a56, +0xb74adb | `sceMouseOpen` right after each `scePadOpen` (one handle per user; without it the handle slot keeps `0x80df0003`) |
| guest+0xb7595e | the per-poll `sceMouseRead`, in the same function that reads the pad (it starts near guest+0xb74d20) |

The same constructor reads `[PS4Application] bDS4TouchEvents / bDS4TouchAxisButtons / bDS4MouseEvents /
bDS4MotionEvents` from the engine ini (guest+0xb713b3..0xb71419). `bDS4MouseEvents` is the DS4 touchpad driving the
cursor (guest+0xb758d9..0xb7595b, cursor clamped to -20..1940 x -20..1100), not the USB mouse.

Per mouse entry (guest+0xb75a06..0xb75c66):

```
if cursor object [app+0xa90]:  pos = cursor->GetPosition() + (dx, dy) * PS4.MouseSensitivity (float at guest+0x5605cc8, 1.0)
                               cursor->SetPosition((int)x, (int)y)          no clamp in this path
MessageHandler->OnRawMouseMove(dx, dy)                                      vtable +0x78, raw ints, not scaled
double-click distance += (dx, dy)
wheel != 0:  MessageHandler->OnMouseWheel(wheel)                            vtable +0x60
per button that changed:  down -> OnMouseDoubleClick (+0x50) if within PS4.DoubleClickDelay (0.5 s) and
                          PS4.DoubleClickArea (10), else OnMouseDown (+0x30);  up -> OnMouseUp (+0x40)
```

The vtable slots fit UE4 4.21's `FGenericApplicationMessageHandler` order (inferred from the call pattern; the
eboot reports `++UE4+Release-4.21`). How often: once per call of this function, which also reads the pad, so once
per game frame on the game thread (inferred from it sharing the pad read; the runtime counts pad reads in
`src/runtime_pad.c:492`).

**Inferred, from UE4 4.21 source (not this eboot):** `FSlateApplication::OnRawMouseMove` makes a pointer event
with that delta; `FSceneViewport::OnMouseMove` adds the delta to `MouseDelta` only while the viewport has mouse
capture (Y negated), and `ProcessAccumulatedPointerInput` sends `EKeys::MouseX / MouseY` to `UPlayerInput` once per
frame. `UPlayerInput` then applies the ini's axis config (sensitivity 0.07, `bEnableMouseSmoothing=True`,
`bEnableFOVScaling=True` with `FOVScale=0.01111`).

## 3. The game's input config and camera

### 3.1 DefaultInput.ini (read, `OVERBITE/Config/DefaultInput.ini` in pakchunk0)

| Setting | Value | Note |
|---|---|---|
| `AxisConfig MouseX / MouseY` | DeadZone 0, Sensitivity 0.07, Exponent 1 | UE4 defaults |
| `AxisMappings` | `Turn = MouseX`, `LookUp = MouseY x -1`; `TurnRate = Gamepad_RightX, Left/Right keys`; `LookUpRate = Gamepad_RightY` | |
| Mouse options | `bEnableMouseSmoothing=True`, `bEnableFOVScaling=True`, `bCaptureMouseOnLaunch=True`, `bDefaultViewportMouseLock=True`, `DefaultViewportMouseCaptureMode=CapturePermanently_IncludingInitialMouseDown`, `DefaultViewportMouseLockMode=LockOnCapture`, `bUseMouseForTouch=False` | the template's PC settings, never used on PS4 |
| Action mappings | keyboard / D-pad for `GUI*`, `Movement*`, `Pause`; `Attack1`, `Jump` etc. `Key=None` | no mouse buttons anywhere |

01.00 and 01.02 differ by one line (01.02 adds `RecenterCamera = Gamepad_RightThumbstick`). The engine-level
`Engine/Config/PS4/PS4Input.ini` only has a commented debug binding; `PS4Engine.ini` sets
`[MobileSlateUI] bTouchFallbackToMouse=false`.

### 3.2 Who consumes the axes (read: name tables of the cooked Blueprints)

| Asset | What its names show |
|---|---|
| `Blueprints/InputManager/InputAxisEnum` | the game's axis list: `LookUp, LookUpRate, MoveForward, MoveRight, Turn, TurnRate` |
| `Blueprints/InputManager/BP_InputDelegateActor` | axis events only for `LookUpRate`, `MoveForward`, `MoveRight`, `TurnRate` (`InpAxisEvt_*_K2Node_InputAxisEvent`); forwards them with `InvokeAxisHandlers(AxisID, AxisValue)` |
| `Blueprints/GameInstance/OverbiteGameInstance` | `RegisterAxisEvent`, `BlockAllAxisEvents`, `CameraTurnRate` / `SetCameraTurnRate`, `ControlsInverted` |
| `Characters/Dan/BP_df_character` | `OnInputAxisTurnRate`, `OnInputAxisLookUpRate`, `BaseTurnRate`, `BaseLookUpRate`, `GetCameraTurnRate`, `GetWorldDeltaSeconds`, `AddControllerYawInput`, `AddControllerPitchInput`, `LookUpAxisValue`, `SetInputMode_GameOnly`, `SetInputMode_GameAndUIEx` |
| `Blueprints/Camera/BP_CameraManager` | `GetControlRotation`, `InputRotation`, `InputRotationRaw`, `ComputeCamera`, `RInterpTo`, `FInterpTo` |
| `Blueprints/Camera/BP_CameraAreaFree` | `LookUpAxisValue`, `TargetLookUpAxisValue`, `SmoothedLookUpAxisValue`, `UserPitchInterpolationSpeed`, `UserPitchAutoInterpolationSpeed`, `UserPitchMin/Max/Weight`, `IsYawing` |

In the eboot, the only code using the wide strings `TurnRate`, `LookUp`, `LookUpRate` is the ArchVisCharacter
plugin's constructor (guest+0x4b8a07..0x4b8b06); no game C++ binds `Turn`. The game's C++ camera is
`UMedievilSpringArmComponent` (`/Script/OVERBITE`): `ComputeCameraSimple`, `ComputeCameraWithSpringyRotation`
(smoothing parameters `BoomSmoothing`, `FinalSmoothing`, `TargetOffsetSmoothing`) and `YawCamera`. `YawCamera`'s
body (guest+0x6339a0, called from its thunk guest+0x65e70f) is a collision search over yaw steps (a loop of up to
0x91 probes through guest+0x6334d0), not an input integrator.

**Inferred chain (names only, the Blueprint bytecode was not decoded):** right stick -> `TurnRate` (dead zone 0.25)
-> character: `AddControllerYawInput(TurnRate x BaseTurnRate x CameraTurnRate x dt)` -> control rotation ->
camera manager reads it (`InputRotation`) -> camera area constraints (`ConstrainYaw`, keep-outs, yaw limits) ->
spring arm with smoothing. Pitch: `LookUpRate` -> a target pitch offset, eased back by
`UserPitchAutoInterpolationSpeed` when the stick returns.

### 3.3 What a pak edit could and could not do

- Adding `+AxisMappings=(AxisName="TurnRate",Scale=k,Key=MouseX)` is possible (an override pak, as
  `scripts/ue4_override.py:132-145` writes for `PS4Engine.ini`). UE4 sums all keys of an axis, so `TurnRate` would
  get stick + mouse. But the character multiplies by `dt`: a mouse delta per frame (already proportional to the
  frame time) times `dt` again makes the turn per centimetre halve when the frame rate doubles. That is the
  frame-rate dependent, rate-shaped feel to avoid. The runtime could pre-divide the reported delta by the interval
  between reads (UE4's `dt` measures the same interval one frame later), which cancels it while frame times are
  steady and errs at hitches. Useful as a no-hook experiment, not as the final path.
- Binding `Turn` (direct `AddControllerYawInput(value)`, the PC template's mouse path) needs a new axis event in a
  cooked Blueprint: not a pak-text edit. The port's byte-checked Blueprint edits (`scripts/ue4_override.py:256-299`, same-length
  edits checked per game version) change constants, not graphs.
- `bEnableMouseSmoothing=False` and `bEnableFOVScaling=False` should go into the same edit whenever the native
  `MouseX` path is used, so UE4 adds no smoothing of its own.

## 4. Design options

### 4.1 Where the mouse enters the game

| | (a) sceMouse + native UE4 path | (b) Guest hook on the camera input | (c) Right-stick emulation (Bloodborne) |
|---|---|---|---|
| How | Runtime reports a connected mouse with the counts since the last read; `PS4.EnableMouse=1` | Add `counts x sensitivity` to the game's own `AddControllerYawInput` call (exec thunk guest+0x2a290e0 in 01.02, which dispatches through the pawn's vtable +0x730) | Mouse speed -> stick deflection, decayed, clamped (`bloodborne_pc/src/runtime_pad.c:463-497`) |
| Camera | Only through `TurnRate` (rate x dt, see 3.3) | Direct: degrees per count, independent of frame rate and of the 8-bit stick | Rate: saturates at full deflection, dead-zone offset, 60 ms lag |
| Menus | **Yes**: Slate cursor, hover, `OnClicked` on UMG buttons | No | No |
| Gating | Slate decides | The game's own: no call when axis events are blocked or the game is paused (inferred) | Runtime decides |
| Version cost | none (cvar + ini) | one hook site per eboot, byte-checked (as the 01.02 mapping did) | none |
| Feel | approximate (dt compensation) | exact; the camera's own spring smoothing remains | "mouse to joystick" |

Recommendation: (a) for the device, the menus and the buttons, plus (b) for yaw. (c) is rejected for the camera
(the user's requirement); its capture handling is reused (4.4).

**Pitch** waits for a test (step 6): if the character's `AddControllerPitchInput` reaches the camera, the same hook
works for pitch. If pitch is the stick-position offset of `BP_CameraAreaFree`, a mouse can drive a held virtual
right-stick Y (integrate counts into a position, clamp to the stick's range, no decay): that is position control,
the same thing the stick does, not a rate, so it does not have the joystick feel; the game's own `FInterpTo` still
smooths it.

### 4.2 Capture: SDL, not a dedicated 8000 Hz thread

**Revised 2026-11-11 (the user: MGS2's first reader hurt frame times): our own batched Raw Input reader, not SDL.**
SDL3's raw input thread (`src/video/windows/SDL_windowsrawinput.c`, read in MGS2-Lab's SDL 3.5 copy) runs at
`THREAD_PRIORITY_TIME_CRITICAL` and wakes for each arriving report (`MsgWaitForMultipleObjects(..., QS_RAWINPUT)`),
then every report becomes a queued SDL event: the per-report wake-up pattern MGS2 dropped (MGS2 3.11). Step 2 uses
MGS2's reader instead (`src/runtime_mouse.c`): message-only window, `RIDEV_INPUTSINK`, normal priority, at most one
wake-up a millisecond, one `GetRawInputBuffer` batch; SDL relative mode stays off (one raw mouse registration per
process). The table below is the original comparison.

| | SDL3 relative mode + event watch (recommended) | Own Raw Input thread (MGS2 pattern) |
|---|---|---|
| Source | SDL 3.4.18 (`C:\msys64\clang64\include\SDL3\SDL_version.h`) relative mode: raw counts, no Windows acceleration (`SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE` off by default) | `RegisterRawInputDevices(INPUTSINK)` on a message-only window, `GetRawInputBuffer` |
| Latency to the accumulator | at event push: an `SDL_AddEventWatch` callback runs on the thread that queues the event (inferred for SDL's Windows raw-input thread), so the window thread's 2 ms poll (`gpu/shim/gow3gpu.cpp:246-248`) is not on the path | wake-up at most once a millisecond |
| Conflict | none | **Windows keeps one raw-input target per device class per process**: a second registration replaces SDL's, so it cannot run alongside SDL relative mode (then: hidden cursor + clip instead) |
| CPU at 8000 Hz | to measure (step 2): SDL batches raw input on its own thread; the motion events still pass through the queue the window thread drains every 2 ms | known: batched drain, normal priority, ~0.01-0.03 ms per frame on the render thread after the fix |

Why not 8000 Hz on our side: the game reads once a frame (2.3). Any reader that keeps an exact running total is
enough; the report rate then only sets how finely counts split between frames (MGS2 3.6: +-1 report a frame at
1 kHz). A faster thread adds wake-ups, not precision. The user's mouse (Keychron M7 8K, from the MGS2 notes) may
still send 8000 reports a second, so the cost must stay off the game, GPU and present threads whatever we pick.

Latency, per frame (inferred): report -> accumulator (< 1 ms) -> the game thread's read at the start of its frame
(average half a frame: ~4 ms at 120 FPS, the same wait the pad has) -> game, render and GPU frames -> present.
Option (b) adds nothing to this; option (c) adds its 60 ms decay. A Reflex `eInputSample` marker at the read would
let the driver's latency stats include it (the port sets `eSimulationStart / End` at
`gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp:166, 236`).

Even motion: the counts a frame gets follow when the game thread reads, not when the frame is shown. With the
Reflex pacing the read sits right after the driver's sleep, so the intervals should be even (inferred). If the
timing log (step 4) shows uneven counts for a steady hand, take MGS2's fix: keep timestamped totals and cut each
frame's counts at a fixed point (the `eSimulationStart` time) rather than at the read.

### 4.3 Sensitivity, invert, acceleration, coexistence

- **Units:** degrees per count for yaw (and pitch if it is a rate), shown also as cm per 360 for a given DPI. If the
  hook passes `v` to `AddControllerYawInput`, the turn is `v x InputYawScale` (UE4 default 2.5; no ini sets it,
  inferred) degrees, so `v = counts x deg_per_count / 2.5`. Step 5 measures the factor instead of trusting it.
- **No acceleration** anywhere: raw counts from SDL, no curve, UE4 smoothing off (3.3). The game's camera spring
  (`FinalSmoothing` and the camera area interpolators) stays; whether it feels floaty is a test question, and its
  parameters are visible properties if it does.
- **Invert Y:** our own setting, applied to the counts. The game's `ControlsInverted` acts on the stick path; with
  the hook it does not apply to the mouse (inferred).
- **The game's Camera Turn Rate slider** scales the stick only; the mouse has its own sensitivity (as MGS2 decided:
  bypass the game's gain while the raw path is active).
- **Gamepad:** additive. The pad's sticks are untouched (unlike Bloodborne, where the mouse replaces the right stick
  while it moves, `bloodborne_pc/src/runtime_pad.c:479-497`).

### 4.4 Cursor and capture rules (with the cursor-hide work in the main session)

Two modes, switched on the window thread (`gpu/shim/window.cpp:146-212`):

| Mode | When | Mouse |
|---|---|---|
| Look | the window has focus, the overlay is closed, the game is in gameplay | SDL relative mode (cursor hidden and held), counts -> camera hook; buttons -> pad buttons per a mouse map |
| Cursor | the overlay is open (`gow3_overlay.cpp:98-110`, `1309-1336` already feed ImGui only while open), the save list or text dialog, focus lost, or the game is in a menu | relative mode off; outside the overlay, deltas and buttons go to `sceMouse` so the game's Slate cursor follows, and the overlay draws a cursor where it is |
| Off | `mouse_look=0` | nothing taken; today's behaviour |

**Step 2 as built (2026-11-11):** one mode so far: captured or not. A whole click (button up) in the game window
takes the mouse; the buttons held at that moment are blocked until let go, so the game never sees half a click.
Plain F1 releases it (with Alt / Shift / Ctrl / Win held it is ignored: Alt+F1 is NVIDIA's overlay, the user,
2026-11-11); focus loss and the Insert menu release it. While captured the cursor is hidden and held in a 2x2
rectangle at the window's centre (`SDL_SetWindowMouseRect`). Counts reach `sceMouse` only while captured.

Rules taken from Bloodborne (`bloodborne_pc/gpu/shim/window.cpp:126-147` `CaptureMouse`, `198-219`, `236-241`):
a click in the window takes the mouse and is not passed on; F1 releases it (free in this port: Insert opens the
overlay, F9 / F11 / F12 are taken, `gow3_overlay.cpp:1231-1253`, `runtime_pad.c:362`); focus loss releases it;
motion from before the capture is dropped so taking the mouse does not turn the camera.

"In a menu" for the switch: the game gives no direct flag. Candidate signal (inferred, step 6): the yaw hook is
called every gameplay frame and not while the game is paused or blocks axis events; no call for ~100 ms = cursor
mode. The game also calls `SetInputMode_UIOnlyEx` (main menu pawn) and `SetInputMode_GameAndUIEx` (menus,
character), which could be hooked later if that signal is not clean.

### 4.5 Menu navigation

**Read:** the main menu, pause menu, options, inventory, bestiary buttons, colour-blind widget, lost-soul dialogs
and confirmation pop-ups bind `OnClicked` on UMG buttons (15 widgets; scan of every UI / Blueprint `.uasset`); no
widget binds hover or mouse-move events, none sets `bShowMouseCursor`. **Inferred:** with the PS4 mouse enabled,
Slate hit-tests the cursor and a click fires the same `OnClicked` the controller's Cross fires on the focused button;
hover shows the button's hovered style (if it has one) but does not move the game's focus highlight; the game draws
no cursor, so the runtime has to. Unknowns for the test: the cursor's coordinate space (the boot display size, which
the port makes the output size, or 1920x1080), whether `SetPosition` clamps (the mouse path does not, the touchpad
path clamps to 1940 x 1100), and the cursor's start position. A resync trick if it clamps: a large negative delta
puts it in the corner, then a known move puts it under the host cursor.

## 5. Lessons from MGS2 (read: `C:\Users\Ty\MGS2-Lab\MGSHDFix\research\docs\mouse-input.md`)

| MGS2 finding | Where | Here |
|---|---|---|
| The game reads the mouse once a frame; the per-frame count follows the sample time, not the display | 3.6 | Same shape (2.3). Measure counts per frame first; cut at a fixed frame point only if uneven. |
| Present-timed cut: a ring of timestamped totals, interpolation over gaps <= 2 ms | 3.6, `src/features/mgs2_lab/lab_mouse_probe.cpp:35` | Optional, step 4. |
| One wake-up per report at Highest priority made frame starts wobble at 8000 Hz; batched drain at most every 1 ms, normal priority, `GetRawInputBuffer` | 3.11, `lab_mouse_probe.cpp:225-262`, `264-333` | If SDL's own path costs too much (step 2), the fallback thread copies this, without SDL relative mode. |
| Every win32k call on the render thread (hotkeys, window queries, input) got 20-30x slower with an 8000 Hz mouse; one 5 ms key-poll thread fixed it | 3.11 (commit b888036) | Rule: no win32k calls per frame from the game thread (`sceMouseRead` must only read an atomic), the GPU threads or the present thread. `runtime_pad.c` reads SDL's cached keyboard state (no win32k); keep it that way. |
| `RIDEV_NOLEGACY` stops ordinary mouse messages waking the window thread 8000 times a second | `lab_mouse_probe.cpp:274-279` | Check what SDL registers in relative mode; measure the window thread's CPU (step 2). |
| The gain chain must be exact and linear; a whole-unit truncation read as a speed clamp; carry the fraction | 2.1-2.3, 3.2 | UE4 rotations are floats: no truncation expected, but verify with a sweep (step 5). |
| Camera eases discarded or delayed the input (pitch 78% thrown away; entry transition lag) | 2.3, 2.6 | MediEvil's camera has springs and interpolators (3.2); judge them only after the input path is shown exact. |
| A verify sweep (inject N counts, measure the picture) and a timing capture before tuning by feel | 3.4 keys numpad 0 / 1, `mouse-test-plan.md` | Steps 4-5. |
| Mouse hardware: PAW3950 smoothing depends on the DPI band; Motion Sync shifts timing | 4 | Record the mouse's DPI / rate with each test. |

`cpu-research.md` has no mouse content; the CPU fix is the 3.11 section above.

## 6. Bloodborne's mouse look, compared (read, `bloodborne_pc` at 3c57da4)

| Part | Bloodborne | Take? |
|---|---|---|
| Capture | `SDL_SetWindowRelativeMouseMode` on a click, F1 / focus loss / menu release (`gpu/shim/window.cpp:126-147`, `198-219`, `236-241`) | Yes |
| Accumulation | in `PollEvents` from `SDL_EVENT_MOUSE_MOTION` xrel / yrel under a mutex (`window.cpp:171-177`); the window thread sleeps 2 ms between polls (`gpu/shim/bbgpu.cpp:287`) | Partly: accumulate in an event watch instead, so the poll interval is not added |
| Camera | right-stick emulation: leaky sum with `LOOK_TAU_US` 60 ms, ~1000 px/s = full deflection, a 16/127 dead-zone offset, replaces the stick while moving (`src/runtime_pad.c:463-497`) | No: rate-based, saturating, lagging |
| Buttons / wheel | bindable like keys; wheel notches become short presses (`runtime_pad.c:407-426`; `docs/CHANGES_0.51.md:17-19`) | Yes for gameplay buttons (look mode) |
| Cursor | hidden in fullscreen and after 3 s still in a window (`window.cpp:245-256`) | The main session's cursor work covers this |

## 7. Step plan (smallest first; each step one change, one launch with the user's go)

| Step | Change | Test (what proves it) |
|---|---|---|
| 1 | No code: launch with `GOW3_UE4_CVARS=PS4.EnableMouse=1` (the pak builder already writes extra cvars into `[SystemSettings]`, `scripts/ue4_override.py:513-519`) and a log line in the existing `mouse_open` / `mouse_read` stubs is the only code (counts, handle, caller). | The log shows `sceMouseOpen` (user 1, type 0, param 1) and `sceMouseRead(..., 8)` once per frame (count vs `pad reads`). If not, the ini is applied too late: try `[ConsoleVariables]`, or poke guest+0x5b1d2b0 = 1 (01.02) before the game runs. |
| 2 | `mouse_read` reports a connected mouse with counts from our batched Raw Input reader (4.2, revised 2026-11-11), buttons, wheel; capture rules of 4.4 (click / plain F1 / focus / overlay). Switch `GOW3_MOUSE=1` (also writes `PS4.EnableMouse=1`). | Nothing in gameplay should change (no binding uses the mouse). Log counts per read. With an 8000 Hz mouse: window-thread CPU, game-thread time and frame-time graph moving vs still (the MGS2 3.11 check). |
| 3 | Cursor mode in menus: deltas to `sceMouse`, the overlay draws a cursor at the guest position. | User: does hovering highlight, does a click press the button (main menu, pause, options, inventory, pop-ups)? Note the cursor's coordinate space and clamps (4.5). |
| 4 | Timing log (key-triggered): every read's time, counts, and the frame's `eSimulationStart`. | Counts per frame for a steady pan at 1000 and 8000 Hz; read time within the frame. Decide whether a fixed cut is needed. |
| 5 | Yaw hook (4.1 b), byte-checked site, off by default, sensitivity in degrees per count. Plus a verify key: inject N counts, log the control-rotation yaw before and after. | Turn = N x setting at 60 and 120 FPS; the user's feel vs the stick; whether the hook runs every gameplay frame and stops in menus, pauses and cutscenes. |
| 6 | Pitch: log whether `AddControllerPitchInput` is called with stick input and whether the view moves with it; else the virtual stick-position path. | The user's feel for vertical look; no snap-back when the mouse stops. |
| 7 | Settings: `mouse_look`, sensitivity, invert Y, mouse button map, in `medievil.ini` and the launcher; mode switch from the hook signal. | One launch per setting change, as usual. |
| Optional | The no-hook experiment of 3.3 (`MouseX -> TurnRate`, smoothing off, dt compensation). | Compare with step 5 if the hook is ever not available (another eboot). |

## 7b. Step results

### T-MS1: PS4.EnableMouse from the pak's ini (2026-11-11, launched by Claude with the user's go, log data/mouse-step1-enable.log)

Change: log lines in the runtime's `sceMouseInit` / `sceMouseOpen` / `sceMouseRead` stubs (caller, handle, read
rate; `src/runtime_services.c`), the stubs still report no mouse. Run: `GOW3_UE4_CVARS=PS4.EnableMouse=1` in
`extra-env.txt` (written into the override pak's `[SystemSettings]`), game build 01.02, 4K output at 75%, Reflex cap
141, main menu -> Load -> a level, mouse moved.

| Check | Result |
|---|---|
| `sceMouseInit` | once, from guest+0xb71388 |
| `sceMouseOpen` | right after the runtime's pad open for user 1: `user 1 type 0 index 0 param 1` -> handle 15, from guest+0xb74afc (the call at +0xb74af7 read in 2.2) |
| `sceMouseRead` | handle 15, count 8, from guest+0xb7599c; 134-141 reads a second over ~19000 reads = the frame rate (cap 141): once per game-thread frame |
| Game | unchanged (the stub answers "no mouse"); the user saw nothing different |

**Confirmed:** `[SystemSettings]` is applied early enough; no `[ConsoleVariables]` or memory poke is needed (open
question 1). The read rate confirms one read per frame (2.3), so step 2's accumulator is read once a frame.

### T-MS2: the batched Raw Input reader feeds sceMouse (2026-11-11, launched by Claude with the user's go, log data/mouse-step2-raw-reader.log)

Build `e27103e`, `GOW3_MOUSE=1`, game build 01.02, borderless 4K at 75%, Reflex cap 141. The user stayed on the
main menu (skipped the intro movies with the pad after the first capture), moved the mouse at 1000 Hz, then
switched the mouse to 8000 Hz and moved it again.

| Check | Result |
|---|---|
| Registration | "registered to the reader" at every capture change: nothing replaced it |
| Counts to the game | up to ~430 counts in one read moving fast, 0 still; counts only while captured |
| Reports per second moving | ~930 at 1000 Hz (12089 in ~13 s of moving reads), ~7100 at 8000 Hz (69114 in ~10 s) |
| Reader wake-ups | 690-704 a second at both rates **and with the mouse still** (batches = wake-ups); ~10 reports a wake-up at 8000 Hz |
| Frame times | the user: no spikes from mouse input that they could see, at 1000 or 8000 Hz |
| Menu | **found:** after a click reached the game, the main menu took no keyboard or pad input. The capturing click (blocked) and movement did not do it (the intro was skipped with the pad after the first capture). Likely (inferred, UE4 4.21 behaviour): the click moved Slate's focus off the menu widget and the console-only game never puts it back |

Follow-ups:

- **Buttons withheld** (fix built after T-MS2): the game gets movement and wheel only; `GOW3_MOUSE_BUTTONS=1`
  passes clicks again for the menu work (step 3, with a focus fix). Gameplay buttons go through the pad (step 7).
- **Idle polling, found, low priority, left as is:** the wait (`MsgWaitForMultipleObjectsEx(..., QS_ALLINPUT,
  MWMO_INPUTAVAILABLE)`, then the 1 ms timer) returns at once even with no input, so the reader polls ~700 times a
  second. It is MGS2's "Every 1 ms" mode line for line, the one verified "rock steady" there (MGS2 commits
  `18fcef2`, `7ca83ac`) and here at 8000 Hz. MGS2's history: per-report wake-ups (the original, frame starts ~3x as
  uneven at 8000 Hz) -> every 1 ms (default) -> once a frame at Present (`4e282c1`, Present waited 0.2-0.3 ms:
  jitter at 8000 Hz). Changing only the idle wait (SDL's `GetQueueStatus` then a wait without
  `MWMO_INPUTAVAILABLE`, keeping the 1 ms gate) would be its own A/B launch at 8000 Hz (the user, 2026-11-11: check
  MGS2's history before changing the reader).

### T-MS3: no mouse buttons to the game (2026-11-11, launched by Claude with the user's go, log data/mouse-buttons-withheld.log)

Build `870f584`, `GOW3_MOUSE=1` (buttons off), same settings as T-MS2. Main menu, Load list (slot 6), a level.

| Check | Result |
|---|---|
| Keyboard and pad | **the user: "all the inputs work"** on the main menu, the Load list and in the level, with clicks, movement and the wheel used freely |
| Load list | the cursor shows while it is open (cursor-hide's rule, as intended). **Found:** a click there snapped the cursor to the window's centre. Log: each click "captured", then "released" in the same instant (5 times): the click took the mouse (`SetMouseCapture`: the 2x2 rectangle at the centre), then `UpdateCursor` let it go because the list has the cursor |

Fix built after T-MS3: a click takes the mouse only while `Gow3Overlay::CapturesInput()` is false (the menu, the
save list and text entry closed), the check `UpdateCursor` already uses.

### T-MS4: no capture from clicks on the save list (2026-11-11, launched by Claude with the user's go, log data/mouse-capture-check.log)

Build `88bf535`, `GOW3_MOUSE=1`, frame limit 237 (the user's new setting). The user: "it all works as expected":
clicks on the Load list leave the cursor where it is; a click in the game takes the mouse and hides the cursor.
Log: one capture before the list, one release when it opened, no capture / release pairs while it was open
(T-MS3 had five); 7355 reports a second at 8000 Hz with 681 reader wake-ups a second.

### Step 5 research: the yaw and pitch sites, and what really turns the camera (2026-11-11, no launch)

A research agent read both eboots (01.02 = `out/eboot.elf`, sha256 `310748698af4...`; 01.00 rebuilt from
`Game Files\CUSA11227\eboot.bin` as prepare.py does) and decoded the camera Blueprints' Kismet bytecode (4.21
package reader, scratchpad tools). The site bytes were checked again independently (scratchpad `verify_yaw.py`).

| Item | 01.02 | 01.00 |
|---|---|---|
| Exec-pointer slot (FNameNativePtrPair + 8), Yaw / Pitch | 0x525bc48 / 0x525bc28 | 0x525b6c8 / 0x525b6a8 |
| Exec thunk, Yaw / Pitch | 0x2a290e0 / 0x2a28fa0 | 0x2a26320 / 0x2a261e0 |
| APawn::AddControllerYawInput / PitchInput (pawn vtable +0x730 / +0x728) | 0x23de8f0 / 0x23de890 | 0x23dbb20 / 0x23dbac0 |
| APlayerController::AddYawInput / AddPitchInput (PC vtable +0xb78 / +0xb70) | 0x24927b0 / 0x2492760 | 0x248f9e0 / 0x248f990 |

Signature (both builds, checked): slot -> thunk and slot - 8 -> `"AddControllerYawInput"`; thunk +0x72:
`49 8b 06 c5 fa 10 45 dc ff 90 30 07 00 00` (`call [rax+0x730]`; pitch `... 28 07 00 00`). The thunk prologue
alone is not unique (3003 functions start with the same 13 bytes): never match on it alone.

Read:

- The thunk reads the float through `FFrame::Step` (Blueprint calls), then calls vtable +0x730. APawn's body is
  UE4's: `Val != 0`, a local player controller, then `PC->AddYawInput`, which adds `Val x InputYawScale (2.5,
  BaseGame.ini; nothing in the game changes it)` to `RotationInput.Yaw` (+0x52c) unless `IgnoreLookInput`. A slot
  wrapper sees every call, zero values included.
- **Only `BP_df_character` calls AddControllerYawInput / PitchInput** (all 26,791 packages of 01.02), from its
  TurnRate handler: `InputRotation = select(AllowUserCameraControl, Axis x dt x BaseTurnRate (45) x CameraTurnRate,
  0)` on `BP_CameraManager`, then `AddControllerYawInput` with the same value. The axis event fires with 0 at rest
  (inferred from UE4 4.21: every tick, not while paused); no call while the camera area is invalid, `CameraCanRotate`
  is false or another listener consumes the axis.
- **The camera mostly ignores control yaw.** `BP_CameraManager` (a plain actor, not UE4's PlayerCameraManager)
  ticks `ComputeCamera` on the current camera area and then `SetControlRotation(0, 0, cameraYaw)`. In
  `BP_CameraAreaFree`: `yaw = (horizontal distance < 400 or TestInputBits(256)) ? control yaw : atan2(camera ->
  focus)`, plus `InputRotation x 2`. In the follow branch, extra control yaw is overwritten every tick (inferred
  from the bytecode). The free, spline, cylinder, suggest-direction and Kardok areas integrate `InputRotation`.
- **Pitch is not control rotation:** control pitch is overwritten with 0 each tick and never used. Camera pitch
  comes from Dan's `LookUpAxisValue` (stick position) and the area's eased `UserPitchMin -65 / Max -5`: the
  virtual-stick path of 4.1 stands.

Consequences for step 5: adding mouse yaw through `AddControllerYawInput` (4.1 b as written) would likely turn the
camera only in the near / `TestInputBits(256)` branch. Mouse yaw must reach `BP_CameraManager.InputRotation` (the
value the areas integrate), whose offset is a Blueprint property (resolved at runtime by name). Open: which branch
is typical per level; whether auto-recentring fights a mouse that never sets `IsCameraRotating` / `IsYawing`;
`AllowUserCameraControl` = false looks like a stick at rest to a slot wrapper.

## 8. Open questions

1. Does `[SystemSettings]` reach `PS4.EnableMouse` before the PS4 application and the pad opens read it (step 1)? **Yes (T-MS1).**
2. Does the camera follow control rotation for yaw, and how much lag does its spring add to an exact input?
3. Pitch: control rotation or the stick-position offset?
4. Does the character call `AddControllerYawInput` every gameplay frame, including with the stick at rest (needed
   for the hook to carry mouse yaw), and never while paused or in cutscenes (needed for the mode signal)?
5. The Slate cursor's coordinate space and clamping at 4K output; where it starts.
6. SDL3's raw-input registration flags in relative mode, and its cost with an 8000 Hz mouse.
7. 01.00: every guest address above is 01.02. Both eboots contain the strings (`PS4.EnableMouse` at SELF offset
   63858698 in 01.00, 63872650 in 01.02); the sites need the same byte-checked mapping 01.02 got.

## Method notes

Strings: ASCII and UTF-16 scans of `out/eboot.elf`. Code references: RIP-relative displacements in the executable
segment (file offset 0x4000 = guest 0). Import stubs: `jmp [rip+x]` in guest+0x3bf0000..0x3c00000 matched to the
`DT_SCE_JMPREL` relocations and named through `src/import_names.inc`. Native UFUNCTION thunks: `R_X86_64_RELATIVE`
addends equal to the function-name string, the next slot being the thunk. Disassembly: capstone 5.0.7 under
`py -3.14 -I`. Paks: `ue4_override.read_index / read_file` (all stored, no compression in pakchunk0); Blueprint
contents from their name tables only (no bytecode decoding).

## Decision (the user, 2026-11-03)

The recommended design is approved: `sceMouse` from SDL relative mode with `PS4.EnableMouse=1`, the yaw hook on the
game's own AddControllerYawInput, pitch decided after a test, no dedicated high-rate capture thread.
