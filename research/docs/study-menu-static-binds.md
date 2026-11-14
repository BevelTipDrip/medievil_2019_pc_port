# Study: the game's menus keep their buttons while gameplay binds are custom (2026-11-13)

Research only: nothing was built, launched or changed in the port or the game folders. Question (the user's): can
the game's own menus (title, pause, options, inventory, dialogs, ...) keep their default buttons while gameplay
binds are custom? Example: jump on Triangle, while Cross stays "accept" in menus; the same for keyboard keys.

**Follow-ups (sections 7-10):** the goal is keyboard-first with per-device maps kept. Section 7 covers
device-switched tables, additive keys and the cutscene skip; section 8 native keyboard input; section 9 a truth
table. **Section 10 is the current recommendation** and replaces sections 3-5's.

Status words: **found** = read in the game's data or the port's code; **confirmed** = seen in game in an earlier
run (quoted); **inferred** = UE 4.21 engine behaviour or reasoning, not seen. Nothing here is fixed or verified.

Evidence comes from both builds' paks (01.00 `C:\Users\Ty\MediEvil\Game Files\CUSA11227`, 01.02
`E:\MediEvil\CUSA11227-0102`) and eboots, read with the scratch tools listed at the end. Offsets are `.uexp` file
offsets; bytecode offsets are in-memory (`mem`) offsets from the scratch Kismet walker (`kis.py`, `NM=12`).

## Summary and recommendation

- **The game already has the mechanism, and it keeps menus and gameplay apart.** Gameplay face-button actions
  are not in `DefaultInput.ini` (there they are `Key=None`). At run time the game instance reads a layout DataTable
  (`KeyRemapperTable_Classic` or `_Modern`, 19 rows) and calls `InputSettings.AddActionMapping` for each row
  (found). The menu actions `GUINext` (Cross) and `GUIBack` (Circle) are separate rows of the same table. The menus
  accept through UMG focus (Slate), go back through `GUIBack`, and navigate with D-pad actions that are fixed in the
  ini (found). Every gameplay prompt takes its picture from the same table row (`GetButtonIconForAction`, found).
- **Recommended: approach C. Edit the layout tables in the override pak, and leave the pad layer physical.** The
  runtime passes Cross as Cross. The table moves gameplay actions only: for example, Jump's row gets
  `Gamepad_FaceButton_Top` and the `SP_PS4_Triangle` sprite. `GUINext`, `GUIBack` and the other GUI rows stay as
  shipped. Menus, Slate accept, raw key checks and cheat codes keep the physical defaults. Prompts follow the table.
  Both tables and the game instance logic are byte-identical in 01.00 and 01.02 (found). A same-size edit (an FName
  index and an import index per row) covers the 11 keys and 13 sprites already in the table's name and import maps.
  Other keys (D-pad, L3, R3, Options) need a name-map extension or an extra ini mapping.
- **Approach A as framed (only `DefaultInput.ini`) cannot move face-button actions.** The ini can only add keys,
  because the table adds its key at run time on top of the ini's mappings. It is still useful for actions the table
  does not hold (Pause, ToggleForcedWalk, RecenterCamera) and for extra keys.
- **Approach B (detect "in a menu" and swap the pad map) is possible but fragile.** No single signal covers every
  menu. The candidate natives were found in both eboots (section 2.8). Pause, input mode and the game instance's
  menu flags each miss some screens, and every transition needs held-button handling.
- **Keyboard: C covers only part of it.** The game cannot receive keyboard FKeys: the eboot has no keyboard
  library, so every key reaches the game as one of the 16 PS4 buttons. With C, the keyboard and the pad share one
  gameplay layout. A key that moves between gameplay buttons while keeping its menu meaning is possible only as a
  permutation of the default keys (section 4). Fully independent keyboard gameplay binds with default menus need
  B, for the keyboard only.
- **First step (cheap, one launch, no runtime change):** an opt-in override-pak edit of `KeyRemapperTable_Modern`
  that swaps Jump and Attack1 (Jump on Square, Attack on Cross: a pairing Classic already ships for Cross). The
  run log carries one line. It tests the whole idea: Square jumps, Cross attacks in gameplay and still selects in
  every menu, Circle still backs. Section 5, T-SB1.

## 1. How binds work today (found, port code)

| Step | Where | What |
|---|---|---|
| Launcher | `launcher/medievil_launcher.py:194-198` `CONTROLS`, `:2018` `build_controls` | Per PS4 button, a KEYBOARD key, a MOUSE button and a GAMEPAD (SDL) button. Pad triggers are not remappable (`None`). Saved in the launcher's settings (`controls.key/pad/mouse`). |
| Environment | `:281-286` `control_map`, `:599-601` | Only the changed buttons: `GOW3_KEY_MAP` / `GOW3_PAD_MAP` / `GOW3_MOUSE_MAP` = `cross=Q,circle=Left Shift`. |
| run.py | `run.py:101-105` `prompt_controls`, `:248-259` | Passes the maps to `ue4_override.apply(..., controls=...)` for the prompt pictures only; the game process inherits the variables. |
| Runtime | `src/runtime_pad.c:151-176` default maps, `:195-232` `apply_button_map`, `:234-243` `load_button_maps` | A mapped PS4 button loses its default sources; `none` unbinds. |
| Game input | `:312-361` `sample_host`, `:616-624` `scePadReadState` | Gamepads (as mapped), keys and mouse buttons are ORed into one `OrbisPadData`. Keys also drive the sticks (WASD, arrows; not remappable). |
| Port menus | `:551-573` `menu_buttons`, `:492-509` `list_buttons`, `:519-534` fixed fallback keys | The ImGui settings menu and Load list read the same mapped buttons, plus arrows / Enter / Esc fallbacks for keys bound to nothing the menus use. |
| Prompts | `scripts/ue4_glyphs.py:210-235` `icons`, `scripts/ue4_override.py:589-594` | Each PS4 button's atlas cell is redrawn with the picture of the physical button or key mapped to it (study-glyphs.md). |

Result: the game only ever sees PS4 pad buttons, and a remap changes menus and gameplay alike (found). The
PS4 "enter button" system parameter is answered with Cross (`src/runtime_services.c:88`, study-glyphs.md 1.4).

## 2. How the game reads input (findings)

### 2.1 `OVERBITE/Config/DefaultInput.ini` (pakchunk0, both builds; found)

- Face-button gameplay and GUI actions are present with **`Key=None`**: `Attack1`, `Attack2`, `Block`, `DanCam`,
  `GUIBack`, `GUINext`, `GUINextCategory`, `GUIPreviousCategory`, `GUISpecial`, `Interact`, `Jump`,
  `OpenInventory`, `ShoulderCharge`, `SkipSequence`, `Strafe`, `SwapWeapon` and `TargetSwap`.
- Fixed in the ini:
  - `GUIUp/Down/Left/Right`: D-pad and arrows.
  - `MovementUp/Down/Left/Right`: D-pad and WASD.
  - `GUINextSubTab`: R2 and E; `GUIPreviousSubTab`: L2 and Q.
  - `Pause`: `Gamepad_Special_Right` (Options); `ToggleForcedWalk`: L3.
  - Debug simulation speed: Add / Subtract.
  - 01.02 only: `RecenterCamera` on R3. This is the only difference between the builds.
- Axes: `MoveForward/MoveRight` (left stick, WASD, arrows), `TurnRate` (right stick X), `LookUpRate` (right
  stick Y).
- An override of this file in the pak is expected to load like the `PS4Engine.ini` and `PS4Game.ini` overrides
  the port already ships. Inferred: it is the same config hierarchy. It has not been tested for this file. The
  4ba06ad attempt (debug-menu-0102 card) did not show whether the edit loaded, because R3 was blocked by Blueprint
  logic.

### 2.2 The layout tables and the remapper (found)

`OVERBITE/Content/Blueprints/KeyRemapping/`:

- `KeyMapsEnum`: Classic 0, Modern 1.
- `KeyRemapperStruct`: per row `actionName` (string), `action` (`InputActionKeyMapping` with an FKey), `axis`,
  `buttonPromptImage` (SlateBrush), `ActionID` (`InputActionEnum`) and `AxisID`.
- `KeyRemapperTable_Classic` / `_Modern`: `.uexp` 27,172 bytes each, the same md5 in both builds (Classic
  `36e528d7...`, Modern `4d6b24df...`; the `.uasset` files are identical too).

| Row (ActionID) | Classic key | Modern key | Kind |
|---|---|---|---|
| Interact (19) | R1 | Triangle | gameplay |
| Jump (20) | Circle | Cross | gameplay |
| Attack1 (1) | Cross | Square | gameplay |
| Attack2 (2) | Square | Circle | gameplay |
| Block (3) | Triangle | R1 | gameplay |
| SwapWeapon (31) | R1 | Triangle | gameplay |
| Strafe (29) | R2 | R2 | gameplay |
| DanCam (4) | L2 | L2 | gameplay |
| TargetSwap (30) | L1 | L1 | gameplay |
| ShoulderCharge / Daring Dash (27) | Triangle (`PS4_Triangle` bone prompt) | R1 (`PS4_R1`) | gameplay |
| OpenInventory (25) | touchpad | touchpad | gameplay, also closes the inventory |
| SkipSequence (28) | Circle | Circle | cutscene skip (menu-like) |
| GUINext (11) | Cross | Cross | menu |
| GUIBack (8) | Circle | Circle | menu |
| GUINextCategory (12) / GUIPreviousCategory (14) | R1 / L1 | R1 / L1 | menu |
| GUISpecial (17) | Triangle | Triangle | menu |
| LookUpRate, MoveForward | right / left stick (axis rows) | same | axes |

`InputActionEnum` values were read from the enum's names and display map. They match the IDs the menus pass to
`RegisterButtonDownEvent`: 8 GUIBack, 11 GUINext, 26 Pause.

**The remapper** (`Blueprints/GameInstance/OverbiteGameInstance`, export numbers the same in both builds; the
bytecode is identical apart from debug strings):

- `UpdateInputMap` (export 304) picks Classic or Modern by `CurrentKeyMap` (saved in the game state) and calls
  `SetInputMapFromDataTable` (export 280).
- `SetInputMapFromDataTable` loops over the table's rows and builds `ActionID -> row` (`Map_Add`, so one row per
  action). Its Cross/Circle swap block runs only if `GetCrossCircleButtonSwapped()` (the port answers no). It then
  calls `SetInputActionsMap` (export 278).
- `SetInputActionsMap` calls `UnbindActionRemapper` (export 282) for every old row, then `BindActionRemapper`
  (export 284) for every new row. It then stores the map in `InputActionMap` and fires `InputMappingsChanged`.
- `BindActionRemapper` sets `action.ActionName = Conv_StringToName(actionName)`, then calls
  `GetInputSettings().AddActionMapping(action, true)`. `Unbind` does the same with `RemoveActionMapping`.
- Callers of `UpdateInputMap` in the assets checked:
  - `ImportGameStateFromSaveGameObject` (export 111, mem 0x02d4);
  - `SetControlsInverted` (192, 0x007a);
  - `SetControlConfig` (306, 0x007c). Options > Button Layout calls this.
- Inferred, UE 4.21 source: `RemoveActionMapping` removes only an equal mapping (same name, key and modifiers),
  and `AddActionMapping` is `AddUnique` plus a keymap rebuild. So an extra ini mapping for `Jump` survives a
  layout switch, and an edited row key is removed again on the next switch.

### 2.3 Action dispatch and menu priorities (found)

- `Blueprints/InputManager/BP_InputDelegateActor` binds every action by name except `DebugSwapShield` (`InpActEvt_<Action>`: 32 actions on 01.02, 31 on 01.00)
  and the four axes. It forwards them as `ProcessInputActionPressed/Released(ActionID, Key)` (game instance export
  216). That function walks a priority-sorted listener list for the action, calls each listener's delegate, and
  stops after one that consumes.
- `InputPriorities`: Player 1, CrystalCapture 2, Statue 3, Interactive 4, LostSoul 5, Inventory 6, Bestiary 7,
  Sequence 8, PauseMenu 9, SOptionsPanel 10, SButtonLayoutPanel 11, SBrightnessPanel 12, SColorblindSettingsPanel
  13, MapScreen 14, ConfirmationPopUp 15, DemoThankYou 16, PSNPurchaseUI 17, DebugMenu 18.
- When a menu opens it calls `DisableAllInputBelowPriority` (`OverbiteGameInstanceFunctions` export 367). That
  calls the game instance's `BlockAllInputBelowPriority` (export 328), which calls `BlockAllButtonDownEvents`,
  `BlockAllAxisEvents` and optionally `BlockAllButtonUpEvents` at the menu's priority. 18 assets call it.
- Game-instance menu flags (bool properties): `bPauseMenu`, `bInventoryMenu`, `bBestiaryMenu`, `bStoreMenu`,
  `bBookUI`, `bDialogueInteract` (`Get/SetPauseMenuVisible` etc. in `OverbiteGameInstanceFunctions`).

### 2.4 How each menu reads input (found, 01.02 disassembly; the same assets exist in 01.00)

| Screen | Select / accept | Back | Navigation / other | Raw key reads (`IsInputKeyDown`) |
|---|---|---|---|---|
| Pause menu | UMG button `OnClicked` (Slate focus, `SetUserFocus`) | actions 8 GUIBack, 26 Pause | Slate focus | none (cheat codes: `BP_TrophyManager.CheckCheatCode`, fed by the pause menu) |
| Options (`WidgetOptionsMenu/Base`) | `OnClicked`; `SetInputMode_GameAndUIEx(SFXSlider)` | 8, 26 | Slate focus | none |
| Button Layout page | 11 GUINext (switch layout) | (manager) | - | none |
| Inventory (`InventoryMenu`) | 11 GUINext, 17 GUISpecial | 8; 25 OpenInventory closes | 12-15 categories / subtabs, 21-24 **Movement\*** (D-pad), axes 3/4 | Triangle, L1, R1, L2, R2 |
| Bestiary | `OnClicked` (`BestiaryUIButton`); prompt 11 | 8 | 12-15, 17 | D-pad, left stick up/down, Circle, Triangle, L1, R1, L2, R2 |
| Map screen (`MapPawn`) | 11 GUINext | | 17, axes 3/4, `SetInputMode_GameOnly` | none |
| Title (`MainMenu`, `MainMenuPawn`) | `OnClicked`; `SetInputMode_UIOnlyEx` / `GameAndUIEx` | | | none |
| Confirmation pop-ups, lost soul dialog, lost souls unlock, demo thank-you | `OnClicked`; `SetInputMode_GameAndUIEx` | the Acknowledge / Small pop-ups register actions (IDs not decoded) | | none |
| New item splash | | | `SetGamePaused` | Cross, Circle |
| Cutscenes (`BP_Sequence_Control`), intro text, video player | | 8 GUIBack; raw Circle | `ButtonBar` prompt for 28 SkipSequence | Circle |
| Dan in gameplay | actions through the dispatcher (19, 25, 26, 27, 30, 31, 33, 22/23, 8 = closes the pause menu, `GUIPreviousSubTab` leads to the 01.02 DanCam toggle check, purpose inferred) | | | none |

- Raw gamepad **key events** exist only for debug features: Dan's R3 debug menu, 01.02's L1 + R1 combo flags, and
  the main menu pawn's R3. Keyboard key events (NumPad, R, Five, Tab, brackets) are developer leftovers that never
  fire on a PS4 build.
- Dan reads the Interact keys back from `InputSettings.GetActionMappingByName(Interact)` (export 314
  `ProcessInputMappingChanged`), to resolve a button shared by two actions. It therefore follows the table.
- Inferred, UE 4.21: UMG `OnClicked` fires from Slate's accept key, which is the platform's gamepad accept
  (Cross unless the PS4 "enter button" says Circle), Enter or Space. It is not an action mapping, so no table or
  ini edit moves it.

### 2.5 Prompts (found)

- Every action prompt is `AddButtonPromptForAction(ActionID)` or `GetButtonIconForAction(ActionID)`, which read
  `InputActionMap[ActionID].buttonPromptImage`: the active table row.
  - Menus: 11 / 8 (pause, inventory, bestiary, lost soul dialog, Button Layout).
  - Gameplay: Interact 19 (`InteractButton`), SwapWeapon 31 (`PlayerHUD` swap icon), `BP_TutorialPrompt`,
    `ButtonBar` skip 28, `ItemWidget`.
- Options > Button Layout labels each controller button from `UI/OptionsMenu/ButtonLayoutDataTable` (rows Classic
  and Modern; per button a `STR_GENERAL` key such as `PMENU_OPTIONS_ACTION_JUMP`). The file is the same in both
  builds. With a custom layout these labels are wrong unless that table is edited too.

### 2.6 Natives for a menu signal (found, `natives.py` on both eboots; slot = `FNameNativePtrPair` exec slot)

| Native | 01.00 slot / exec | 01.02 slot / exec |
|---|---|---|
| `GameplayStatics::SetGamePaused` | 0x519fa48 / 0x2910980 | 0x519ffc8 / 0x2913740 (same as study-pc-settings-entry 1.4) |
| `GameplayStatics::IsGamePaused` | 0x519f8e8 / 0x290e1c0 | 0x519fe68 / 0x2910f80 |
| `WidgetBlueprintLibrary::SetInputMode_GameOnly` | 0x5012548 / 0x1a70240 | 0x5012ac8 / 0x1a72ec0 |
| `...SetInputMode_GameAndUIEx` | 0x5012538 / 0x1a700a0 | 0x5012ab8 / 0x1a72d20 |
| `...SetInputMode_UIOnlyEx` | 0x5012568 / 0x1a70400 | 0x5012ae8 / 0x1a73080 |
| `InputSettings::AddActionMapping` | 0x51baba8 / 0x292ac50 | 0x51bb128 / 0x292da10 |
| `InputSettings::RemoveActionMapping` | 0x51bac28 / 0x292b340 | 0x51bb1a8 / 0x292e100 |
| `InputSettings::GetActionMappingByName` | 0x51babd8 / 0x292af90 | 0x51bb158 / 0x292dd50 |

Callers (found): `SetGamePaused` is called by the pause menu, `Inventory`, `InventoryMenu`, `LostSoulsUnlock`,
`NewItemPickupSplash`, `Store`, Dan, and the Hall of Heroes exit trigger. `SetInputMode_GameOnly` is called by
Dan, the confirmation pop-ups, `MainMenuPawn`, `MapPawn` and `NewItemPickupSplash`. The **pause menu and the
inventory never call `SetInputMode`**: Dan sets GameAndUI for pause. No keyboard library is among the imports the
port resolves (`src/import_names.inc`: sceMouse yes, sceKeyboard / sceImeKeyboard no).

## 3. Approaches

### A. Rebind gameplay in `DefaultInput.ini` only (pak override)

- **Feasibility: low for face buttons; good as a helper.** `-ActionMappings` cannot remove the table's key,
  because the table adds it at run time. An ini line only adds a second key to an action. It works for:
  - the actions the table does not hold (Pause, ToggleForcedWalk, RecenterCamera on 01.02, the Movement\*
    D-pad; Movement\* also drives inventory navigation);
  - extra keys, such as a D-pad key for Jump, which the table's name map cannot hold.
- **Work:** about half a day: text edit, as `debug_menu_config` does for `PS4Game.ini`.
- **Risks:** the ini override loading is inferred, not seen for this file. Moving Movement\* also moves inventory
  navigation.
- **Breaks:** nothing by itself. Prompts do not follow ini-only keys: the prompt comes from the table row.
- **Test:** T-SB2 (section 5).

### C. Custom layout in the game's own tables (recommended)

- **How:**
  - The runtime pad map is physical (no `GOW3_PAD_MAP` for the standard 16 buttons).
  - The override pak carries edited `KeyRemapperTable_Classic.uexp` and `_Modern.uexp`. Gameplay rows get new
    keys and matching sprites; menu rows (`GUINext`, `GUIBack`, `GUISpecial`, both categories) and `SkipSequence`
    stay as shipped.
  - **C-c, the translation that keeps today's launcher meaning:** the launcher's pad column says "physical P acts
    as PS4 button X". For every gameplay row whose stock key is X, the new key is the FKey at P's position, and
    the sprite is P's. Both tables get the same permutation, so the game's Classic / Modern switch still works.
  - Extra sources that take no default away (Deck paddles, `misc1`, a second key on a button) stay in the
    runtime map: they add a button everywhere and never move a menu button.
- **Same-size edit:**
  - The FName index (int32) of `action.Key.KeyName` and the import index of `buttonPromptImage.ResourceObject`,
    per row.
  - The name map holds `Gamepad_FaceButton_Bottom/Left/Right/Top` (55-58), `LeftShoulder` 59, `LeftTrigger` 60,
    `RightShoulder` 62, `RightTrigger` 63, `Special_Left` 65 and `None` 111.
  - The imports hold `SP_PS4_Circle/Cross/L1/L2/R1/R2/Square/StickLeft/StickRight/Touchpad/Triangle` and
    `PS4_R1` (objects -17 to -28).
  - Both tables have the same row offsets: Interact key 0x024d / sprite 0x0438, Jump 0x07db / 0x09c6, Attack1
    0x0d6c / 0x0f57, Attack2 0x12fd / 0x14e8, Block 0x188c / 0x1a77, SwapWeapon 0x1e20 / 0x200b, Strafe 0x23b0 /
    0x259b, DanCam 0x2940 / 0x2b2b, TargetSwap 0x2ed4 / 0x30bf, OpenInventory 0x3f8d / 0x4178, SkipSequence
    0x4523 / 0x470e, ShoulderCharge 0x66ae / 0x6899.
  - D-pad, L3, R3 and Options keys need a name-map and import-map extension: a header rewrite, the package
    writer study-pc-settings-entry S1 / S7 already plans. The fallback is the row key `None` plus an ini
    mapping (A), but then the prompt has no matching sprite.
- **Options > Button Layout:** permute the `STR_GENERAL` text values among the button fields of each
  `ButtonLayoutDataTable` row. A permutation keeps the export's size: values are swapped and the tags' sizes swap
  with them. Every language follows, because the values are string-table keys.
- **Feasibility: high (found):** one code path, identical data in both builds, prompts table-driven. The pak
  mechanism is confirmed for same-size `.uexp` edits: T-PC1, the user: "it actually opened our options menu"; the
  edited `PauseMenu.uexp` loaded.
- **Work:**
  - Stage 1 experiment: half a day.
  - Gamepad C-c with the same-size subset (both tables, Button Layout labels, ini for Pause/L3/R3, launcher switch,
    run.py, glyph rules, tests): 2-3 days.
  - Name-map extension for D-pad, L3, R3 and Options keys: 3-5 days.
- **Risks:**
  - Two actions the game never shares on one button (for example Interact with GUINext on Cross) may both fire
    in one press. Inferred: UE4 fires every action bound to the key in the same input component. The launcher
    should warn on new overlaps; the Stage 1 experiment uses an overlap Classic already ships.
  - Restart-only: the table loads at start, as other pak features do.
  - The title screen before a save loads may run with the stock table. Inferred from the callers: the first
    `UpdateInputMap` is at save import.
- **What breaks / stays:**
  - **Prompts:** gameplay prompts follow; menu prompts are fixed and stay right; the Button Layout page needs
    the label permutation.
  - **Classic / Modern:** kept with C-c.
  - **Steam Deck:** works; Steam Input stays the physical layer.
  - **01.00 and 01.02:** one edit.
  - **Cheat codes:** physical buttons, the stock behaviour.
  - **The port's own menus:** with the pad identity they read physical buttons too.
- **Tests:**
  - Offline: every edit is found exactly once at its offset, nothing else changes, and both builds give the same
    result.
  - In game: T-SB1, then a full C-c run on 01.02, 01.00 and the Deck.

### B. Detect "in a menu" and swap the runtime's map

- **Signal candidates:**

  | Signal | What it covers | Gaps and costs |
  |---|---|---|
  | `SetGamePaused` / `IsGamePaused` wrappers | pause, inventory, store, item splash, lost souls unlock | title, options from the title, dialogs, confirmation pop-ups, map screen, books |
  | `SetInputMode_*` wrappers | title, options, pause (via Dan), pop-ups, lost soul dialog | the inventory; GameOnly is not called everywhere a menu closes (each pair must be checked) |
  | Game instance flags (`bPauseMenu` ...) | 6 flags | needs the game instance's address and property offsets from bytecode, as the camera hook does |
  | `PostEventAtLocationWithMotion` (the PC Settings wrapper) | menu sounds | activity only, not state |
  | Slate user focus | everything that focuses a widget | engine C++ internals per build; no hook exists |

  The existing hooks find natives by fixed per-build slot, name and prologue (`src/pc_settings_hook.h`,
  `probe.c install_pc_settings`). Four wrappers are about a day.
- **Transitions:** a button held across a switch changes meaning: Triangle held as Jump becomes GUISpecial in the
  inventory. The runtime needs `hold_after_capture`-style hiding (`runtime_pad.c:476-478`, `:587-590`) at every
  switch. Gameplay prompts (Interact) must keep the gameplay map, and menu-like gameplay screens (book, gargoyle
  dialogue, cutscene skip) need a decision each.
- **Feasibility:** medium. **Work:** 1-2 weeks, with launches over every menu type in both builds. **Risk:**
  high. A missed close leaves the menu map in gameplay (jump moves back), and a missed open leaves gameplay binds
  in a menu.
- **Breaks:** prompts would show the gameplay map in gameplay only if the glyph cells switched with it, which
  needs a runtime texture swap (study-glyphs option b). Classic / Modern unaffected. Deck the same.
- **Tests:** per menu (18 `DisableAllInputBelowPriority` users plus title and map), open and close with a held
  button, both builds.

### Others

| Option | Verdict |
|---|---|
| Hybrid "fixed accept/back only" in the runtime | Not possible without B's signal. Under C it is automatic: Slate accept and `GUIBack` stay physical. |
| A third layout in `KeyMapsEnum` | Needs enum and bytecode changes (`UpdateInputMap`'s select, the Button Layout page). Overwriting the two tables (C-c) is cheaper. |
| Runtime wrapper on `InputSettings::AddActionMapping` that rewrites the key | Any key and live changes become possible, but it must build a valid FKey (FName from the guest name pool, `KeyDetails` shared pointer). Prompts would still read the table. Keep it as a **trace only**: log the binds, which is cheap and useful for T-SB1. |
| Edit the loaded table rows in guest memory, then let the game re-apply them (Button Layout switch) | A later path to live rebinding from the port's menu; needs the DataTable's row map in memory. Not now. |
| Extra FKeys for keyboard-only binds through sceMouse buttons (7 keys) | Mouse buttons reach Slate too: a click the game received left the main menu deaf (mouse-input T-MS2). Rejected. |

## 4. Keyboard

- The game sees keys only as PS4 buttons (found: no keyboard library). The default keyboard map gives all 16 PS4
  buttons a key, so there is no free PS4 button.
- **With C:** the keyboard keeps its map (key to PS4 button) in menus and gameplay, and the table changes what each
  PS4 button does in gameplay. A keyboard player gets "Jump on Q, Space still accepts" by moving Jump to Triangle
  in the table. The pad then has Jump on Triangle as well, because the layout is shared.
- **Keyboard-only remaps:** a remap made in the keyboard column alone can go into the table only as a permutation
  of default keys. A new key (Jump on G) can only be an *added* source of a PS4 button, which also acts in menus
  (harmless for Square, L3 and R3, which have no menu meaning). A default key that moves (Space to Square) needs B.
- **If both columns differ,** one table cannot hold both. Proposed rule: the table follows the gamepad column, the
  keyboard column stays global as today, and the run log says so. The user's call (section 6).

## 5. Recommendation and staged plan

| Step | Work | Proves | Launch |
|---|---|---|---|
| **S0** | `ue4_override.apply`: opt-in test switch (e.g. `GOW3_TEST_LAYOUT=1` in extra-env.txt) that writes an edited `KeyRemapperTable_Modern.uexp` (four 4-byte swaps, see the note). The summary line ("Render resolution: ...") adds "gameplay layout test (Modern: Jump on Square, Attack on Cross; menus unchanged)". Unit test. | the table is read from the pak; menus and gameplay separate | **yes (T-SB1)** |
| S0b | Optional, the next launch: `DefaultInput.ini` override adds `+ActionMappings=(ActionName="Jump",...,Key=Gamepad_LeftThumbstick)` | ini overrides of input load and survive the table (A) | yes (T-SB2) |
| S1 | `scripts/ue4_layout.py`: table walker (offsets per row, both tables), C-c translation from the pad column, Button Layout label permutation, ini lines for Pause / L3 / R3; launcher switch "Game menus keep their default buttons"; run.py stops passing the pad permutation to the runtime when it is on; glyph rules (PlayStation style: identity cells; the table sprites carry the move); patch card, tests | gamepad remaps in gameplay only | yes (T-SB3 on 01.02, T-SB4 on 01.00, T-SB5 Deck) |
| S2 | Keyboard rule (section 4) and the launcher's warnings (shared buttons, keyboard vs pad conflict) | | yes |
| S3 | Package writer: name and import map extension (D-pad, L3, R3, Options keys and sprites) | any pad button for any action | yes |
| S4 | Only if the user wants independent keyboard gameplay binds: B for the keyboard, starting with `SetGamePaused` + `SetInputMode_*` wrappers in trace mode (log only) over a play session, before any switching | | several |

Note on S0's bytes (found, the same in both builds):

| Row | Field | Offset | Before | After |
|---|---|---|---|---|
| Jump | key | 0x07db | `37000000` (55, `Gamepad_FaceButton_Bottom`) | `38000000` (56, `Gamepad_FaceButton_Left`) |
| Jump | sprite | 0x09c6 | `eeffffff` (-18, `SP_PS4_Cross`) | `e9ffffff` (-23, `SP_PS4_Square`) |
| Attack1 | key | 0x0d6c | `38000000` | `37000000` |
| Attack1 | sprite | 0x0f57 | `e9ffffff` | `eeffffff` |

**Edit by offset, not by pattern:** the 16 bytes around Jump's key and sprite also occur in the GUINext row
(both are Cross). A find-and-replace would hit both rows and move menu accept. Check each row's `actionName`
first (Jump at 0x06ce, Attack1 at 0x0c5c) and the four old values, and raise on any mismatch.

### T-SB1 (S0; 01.02, the user's daily build; one launch)

Settings: extra-env.txt `GOW3_TEST_LAYOUT=1`; launcher gamepad and keyboard columns at their defaults.

1. Load a save. In the pause menu, choose Options > Button Layout and switch to **Modern** if it is not already.
   Expected: Cross selects in every menu step, Circle backs out.
2. Back in the level: **Square jumps, Cross attacks.** Circle (Attack2), Triangle (Interact / swap weapon) and R1
   (Block) are unchanged.
3. Pause: Cross selects, Circle closes. Inventory (touchpad): Cross uses, Circle exits, L1/R1 change category.
4. Open a dialog (a gargoyle or a book) and a confirmation pop-up (Quit to Map, then No): Cross and Circle as
   before.
5. Options > Button Layout: switch to **Classic** (expected: the stock Classic, Cross attacks and Circle jumps),
   then back to Modern (Square jumps again). This proves the unbind and rebind leave nothing behind.
6. If a tutorial prompt or the Book shows the jump button, expected: Square.

Collect: the run log (the override line) and a note per step. Known and expected: the Button Layout page still
labels Cross "Jump" (S0 does not edit its labels).

T-SB2 (S0b): L3 jumps and still toggles walking. Collect the log.

## 6. Open questions for the user

1. Is a shared gameplay layout for the pad and keyboard acceptable, with keyboard-only remaps limited as in
   section 4? Or are fully independent keyboard gameplay binds wanted, which means B (S4)?
2. Should "Game menus keep their default buttons" be on by default for players who already have remaps? Today's
   remaps would then stop moving menu buttons.
3. Cutscene skip (`SkipSequence`): treat it as a menu button (fixed Circle, matching the raw Circle checks in
   intro text and videos) or as gameplay?
4. Should the D-pad, Options, L3 and R3 be bindable for gameplay actions now (S3), or later?
5. Which layout does the user's save use (Classic or Modern)? T-SB1 step 1 switches to Modern.

## 7. Follow-up (2026-11-13): keyboard-first, per-device independence kept

The user's goal, clarified:

- The feature is mainly for keyboard players.
- Each device keeps its own physical-to-PS4 map, as today. Gamepad defaults are logical, and a pad remap may
  keep moving the pad's menu buttons.
- The keyboard's menu keys stay the defaults while its gameplay keys are custom. Example: Space bound to Square
  must still accept in menus.
- The D-pad, L3, R3 and Options only need to stay bindable as today, through the port's pad layer. Game actions do
  not move onto them through the tables.

This changes the recommendation in sections 3-5: a static table shared by both devices (C) breaks per-device
independence. The candidates are a table switched by device in use (7.1) and additive keyboard keys (7.2).

### 7.1 Per-device tables, switched at run time

**Model:**

- The pad keeps today's runtime map (`GOW3_PAD_MAP`) and the game's own table, stock Classic or Modern.
- The keyboard layer becomes static: the default keys (`runtime_pad.c:169-176`) always send their default PS4
  button, so menus see the default keys. Keys bound to the D-pad, L3, R3 and Options keep working as today:
  global, menus follow them.
- While the keyboard is the device in use, the runtime has the game apply a *keyboard table*: the current table's
  rows with the keyboard's gameplay keys.
- Derivation: for a row whose stock key is PS4 button X, the launcher's keyboard column binds key K(X). The row's
  new key is D(K(X)), the PS4 button that key K(X) sends by default; the row's sprite is that button's.
  Example: Space bound to Square makes Attack1's row (Modern: Square) take Cross; Space sends Cross, which
  accepts in menus and attacks in gameplay.
- Rows not edited: `GUINext`, `GUIBack`, `GUISpecial`, both category rows and `SkipSequence` (7.3).
- Because the derivation uses the current table's stock keys, Classic and Modern both work.

**New keys (not one of the 16 defaults, e.g. G) and mouse buttons:**

- A mouse button bound to the same PS4 button as a key sends that key's default button. No extra slot is needed.
- A new key needs a *free slot*: a table button (Cross, Circle, Square, Triangle, L1, R1, L2, R2, touchpad) whose
  default key the custom keyboard map no longer uses. The new key sends that button statically, so it also has
  that button's menu meaning. The freed default key still sends it too: in gameplay it does the new key's action.
- Counting: the launcher gives each PS4 button one key, so the unused default keys are at least as many as the
  new keys. They can still run out among *table* buttons when default keys of table buttons are rebound to the
  D-pad, L3, R3 or Options. The launcher must refuse that case with a message.
- Prefer Square as a free slot: it has no menu meaning apart from cheat codes.

**Can the runtime switch the applied table? Yes, inferred feasible, through the game's own path.** The game
already re-applies a table mid-session: Options > Button Layout calls `SetControlConfig` → `UpdateInputMap` (2.2).
The runtime would edit the active table's rows in memory, have the game run `UpdateInputMap`, and then restore the
stock rows. `SetInputActionsMap` keeps *copies* of the rows in `InputActionMap`, so the restore does not undo the
binds. Later unbinds use those copies, so they stay correct. Pieces:

| Piece | How, both builds | State |
|---|---|---|
| Game instance, `InputSettings`, the class | Wrap `InputSettings::AddActionMapping` (slots 2.6). A call whose `Stack.Node` (FFrame +0x10) is `BindActionRemapper` (in-memory script size 0x2a4, identical in both builds) gives the game instance (`Stack.Object` +0x18) and `InputSettings` (Context). The class is the function's Outer (+0x20). | found: offsets are the port's working hooks (`probe.c:722`, `:880-896`, `pc_settings_hook.h`) |
| `UpdateInputMap` UFunction | The class's function children: the one with script size 0xa0 and the known bytes. UStruct `Children` (+0x38) and UField `Next` (+0x28) are the 4.21 layout. Alternatively, `SetControlConfig`'s script holds the pointer at mem 0x007d. | `Children` / `Next` offsets inferred; the rest found |
| The two table objects | `UpdateInputMap`'s script: `EX_ObjectConst` pointers at mem 0x000b (Modern) and 0x001e (Classic) | found (kis3 token dump) |
| Rows | `UDataTable::RowMap` (inferred +0x30, after `RowStruct` +0x28) maps FName to row memory. The field offsets come at run time from `RowStruct`'s property chain (`Offset_Internal` +0x44): `action` → `Key.KeyName` (FKey: FName, then a `KeyDetails` shared pointer) and `buttonPromptImage.ResourceObject`. | inferred; check in X1 |
| Calling `UpdateInputMap` | `UObject::ProcessEvent(function, NULL)` through the game instance's vtable. **The slot is not found yet:** a quick byte heuristic gave a wrong candidate. Find it offline in Ghidra from a BlueprintImplementableEvent thunk; it is expected to be the same in both builds (same engine). The port already calls guest virtual functions on the game thread from `scePadReadState` (`runtime_extra.c` `ue4_set_cvar`, vtable +0x20 / +0x68). | to find |
| Where to apply | In `scePadReadState` (the game thread, the same place as the live cvars), before that read's state is returned. The press that switched devices then already acts under the new table, with no wrong frame. | inferred |

**Rejected alternative:** calling `AddActionMapping` / `RemoveActionMapping` (or their C++ functions) directly and
editing `InputSettings.ActionMappings`. It skips the game's `InputActionMap` (prompts, later unbinds) and its
`InputMappingsChanged` event. Dan caches the Interact keys on that event (`ProcessInputMappingChanged`, used to
resolve shared buttons), so the cache would go stale.

**Safety mid-gameplay:**

- **Held buttons.** UE4's keymap rebuild (`ForceRebuildKeymaps`, run by `AddActionMapping(…, true)`) leaves key
  states alone (inferred). A table button held across a re-apply would release into a different action: Block
  pressed, then released as something else, so Block never ends. Rule: switch only on a *press edge* of a table
  button from the other device, and only when the current device holds no table button.
- **Re-entrancy.** `UpdateInputMap` runs about 19 unbinds, 19 binds and 19 keymap rebuilds. Running it inside
  the input poll is ordinary game-thread work (inferred). It must never run from inside a game call of the same
  path. The `AddActionMapping` wrapper knows when the game is applying.
- **The game's own re-applies** (save load, Options > Button Layout, the inverted-camera option) apply stock rows.
  The wrapper sees `BindActionRemapper` calls the runtime did not start. On the next pad read, while the keyboard
  is the device in use, the runtime applies the keyboard table again.
- **Rows' `KeyDetails`.** The rows are cooked FKeys; their shared pointers are probably empty until looked up
  (inferred). Copy only the FName while they are empty. Otherwise swap whole FKeys (a permutation, reference
  counts unchanged), or refuse with a log line.
- **No save change.** `CurrentKeyMap` is never touched; the save keeps the player's Classic / Modern choice.
- **Steam Deck:** a pad alone never switches, so behaviour is today's.

**Prompts:**

- `GetButtonIconForAction` reads `InputActionMap`, so prompts made after a switch show the new row's sprite.
- Property-bound brushes repaint live, for example `PlayerHUD`'s `Get_SwapButton_Brush_0`.
- Prompts already on screen (a tutorial prompt, a button bar) keep their picture until rebuilt (inferred).
- The atlas cells are written at start for one style (study-glyphs.md). With the Keyboard style, cell X should
  draw the key whose gameplay action sits on X: the default key, or the new key on a free slot. The menus then
  show the default menu keys too. A per-device live glyph switch is study-glyphs option (b), separate.

**Debounce and both devices at once:**

- Only button presses switch; sticks, WASD and mouse motion never do, since movement does not depend on the table.
- Optionally, a minimum of 250 ms between switches.
- While one device holds a table button, the other device's presses act under the current table and do not
  switch. Example: pad Cross during keyboard play does what the keyboard table put on Cross. Log it once.

**Size:**

- Offline: ProcessEvent slot, `RowMap` / `Children` offsets, both builds. 1-2 days.
- Runtime: finder, row edit / apply / restore, device tracking, hold rules, log lines, C tests for the byte
  checks. 3-4 days.
- Launcher: keyboard column semantics, free-slot assignment and refusals, glyph cells. About 2 days.
- Launches: X1-X3 below, a full run on 01.02 and one on 01.00.
- Total about 1.5-2 weeks.

**Risks:** the ProcessEvent slot and the UDataTable layout are not seen yet; brush copies (a cached
`ResourceHandle` in a row brush, inferred unused); stale prompts already on screen; a re-apply during a level load;
the title screen before the first table apply (the save import); the device rule when both devices are held.

**Cheap first experiments:**

| # | Change | Proves | Launch |
|---|---|---|---|
| X1 | Log-only wrapper on `AddActionMapping`. Logs per apply: count, calling function (script size), game instance, `InputSettings`, the two table pointers read from `UpdateInputMap`, each table's row count (19 expected if the `RowMap` offset is right), and the `KeyName` / `ResourceObject` offsets from `RowStruct`. The user loads a save and toggles Options > Button Layout twice. | every object is found in this build; no behaviour change | yes, once per build |
| X2 | At the first apply, the runtime swaps Jump and Attack1 in the Modern table's rows in memory (as T-SB1, but in memory). The game's own Button Layout toggle applies it. | an in-memory row edit drives gameplay and prompts, menus unchanged, without calling the game | yes |
| X3 | With the ProcessEvent slot found: a test key makes the runtime call `UpdateInputMap` mid-gameplay with and without the X2 edit | runtime re-apply; held-button rules | yes |

T-SB1 (the pak edit, section 5) stays the cheapest proof that the table alone drives gameplay while menus keep
their buttons. It needs no runtime code.

### 7.2 Additive keyboard keys (static, no switching)

- **Mechanism:** the launcher lets a PS4 button take extra keys or mouse buttons; the runtime already supports
  several sources per button (`ButtonMap`, `runtime_pad.c:157-158`). `apply_button_map` replaces a button's
  sources, so it needs an "add" form such as `cross+=K`. Default keys never change, so menus keep the defaults.
- **Cost:** an extra key has its button's menu meaning: K → Cross also accepts.
- **What it covers:**
  - more keys and mouse buttons for a button (mouse left as Square, Attack in Modern);
  - moving keys between buttons that have no menu meaning (Square; L3 / R3 / Options / D-pad stay global anyway).
- **What it does not cover:**
  - re-purposing a default key: Space as Square while Space still accepts, which is the user's own example;
  - freeing a default key from gameplay.
- **Size:** 1 day (launcher column, runtime syntax, glyph cells showing the extra key), no game-side work.
- **Verdict:** a useful near-term step, but not the user's goal on its own.

The menu meaning of each PS4 button (found, 2.4): Cross accept / GUINext; Circle back; Triangle GUISpecial
(inventory, bestiary); L1 / R1 categories; L2 / R2 subtabs; touchpad closes the inventory; Options pause and back;
D-pad navigation. Square has none (cheat codes only).

### 7.3 Cutscene skip (plan only, no decision)

**How skipping works today (found, 01.02 disassembly):**

| Cutscene type | Input | Details |
|---|---|---|
| In-engine sequences (`Blueprints/Sequences/BP_Sequence_Control`, placed in about 40 cutscenes: Hall of Heroes intros and rewards, boss intros and outros, Dan's intro, boulders, Ant Caves fairies) | **Action 8 `GUIBack`**, listener at priority 8 (Sequence); `RegisterInputEventHandlers` first blocks everything below 8 | `OnGUIBackPressed` (ubergraph entry 0x19ce): on `Level_IN_MM` (title) it runs `HandleMainMenuSequence` (jump to the menu frame); elsewhere (0x1106 → 0x05e5) `if IsSkippable: SkipSequence()` (set the playback position to the end), then a **0.5 s `RetriggerableDelay` debounce** (`DelayInput`). No hold timer. Not skippable: nothing happens (the "not skippable" message is a stripped `PrintString`). |
| Its prompt | **`SkipSequence` row (action 28)** | When `IsSkippable`, a `ButtonBar` with `IncludeSkipButton` calls `AddButtonPromptForAction(PROMPT_SKIP, 28)`: the picture comes from the `SkipSequence` row, Circle in both layouts. No listener on action 28 was found in the decoded assets: the row is the prompt only, and the button is `GUIBack`. Both are Circle, so they agree today. |
| FMV cinematics (`UI/Movies/UI_VideoPlayer`, used by `Level_AV_00` and `Level_AV_GameIntro`) and intro text (`UI/IntroScrollingText`) | **Raw `IsInputKeyDown(Gamepad_FaceButton_Right)`** (`ProcessInput` → `Circle Button`) | The callers in the two levels' scripts, which also bind `GUIBack` / `GUINext` action events, were not decoded (hold or press not known). The intro text has a 1 s `Delay` around skipping. |
| Return from the background | none | Dan skips the active sequence after a suspend (`IsRecentBackground`, 0x4ed7). Not input. |

**Options for keeping it fixed:**

1. **Fixed on Circle, as a menu button (no work).** Skip is `GUIBack` plus raw Circle, both menu-side and
   untouched by 7.1 / 7.2. Keep the `SkipSequence` row stock in the keyboard table so the prompt matches. Keyboard:
   the default Circle key (Left Shift) skips; with the Keyboard prompt style the prompt shows it.
2. **Follows gameplay.** Make `BP_Sequence_Control` listen to action 28 instead of 8: one same-size byte, the
   `EX_ByteConst` 8 in `RegisterInputEventHandlers` (mem 0x00bb call; offset to take in both builds). The
   `SkipSequence` row then decides both the button and the prompt, and the keyboard table may move it. FMV and
   intro text stay raw Circle unless their key names are edited (a name-map change). Skipping would then differ
   between cutscene types.
3. **Both.** Keep `GUIBack` and add 28. That is not same-size (a second register call); alternatively, give the
   `SkipSequence` row an extra ini key. It costs more for little gain.

Option 1 matches the "menus keep the defaults" goal; 2 and 3 are only worth it if players expect the gameplay
"back" or "attack" key to skip.

### 7.4 Summary of the follow-up

Superseded by section 10, after the user's preferred idea (native keyboard input, section 8) and the truth table
(section 9). Kept for 7.1-7.3: per-device tables remain the fallback, additive keys the quick option, and the
skip plan stands.

## 8. Native keyboard input (the user's preferred idea)

The idea: keyboard keys stop becoming PS4 pad buttons. The runtime sends the engine real key events (SpaceBar, W,
Escape ...), as on a PC, and keyboard binds become UE4 key mappings. The missing PS4 keyboard library only means
the PS4 driver is absent. The engine's input layer above it (Slate, `UPlayerInput`) is platform-neutral.

### 8.1 What the build has (found, both eboots)

- **The keyboard FKeys exist.** The `EKeys` static initializer builds `FKey` objects from ASCII names
  (`lea rdi, <static FKey>; lea rsi, "<name>"; call`). The UTF-16 display names, such as "Space Bar", are there
  too (`EKeys::Initialize`).

  | Static `FKey` | 01.00 | 01.02 |
  |---|---|---|
  | `EKeys::Enter` | 0x5b1b1f0 (name "Enter" 0x3cbd1c4, init 0xb64c78) | 0x5b1b230 (0x3cc0824, 0xb678f8) |
  | `EKeys::SpaceBar` | 0x5b1b250 (0x3cbd1e0, 0xb64d40) | 0x5b1b290 (0x3cc0840, 0xb679c0) |
  | `EKeys::LeftMouseButton` | 0x5b1b148 (0x3cbd160, 0xb64b1a) | 0x5b1b188 (0x3cc07c0, 0xb6779a) |
  | `EKeys::Gamepad_FaceButton_Bottom` | 0x5b1bdd8 (0x3cbd59e, 0xb66555) | 0x5b1be18 (0x3cc0bfe, 0xb691d5) |

  The statics follow `EKeys` declaration order at a 0x18 (`sizeof(FKey)`) stride. For example, Enter → SpaceBar
  is 0x60, four keys: Enter, Pause, CapsLock, Escape. A runtime can read any key's FName from its static after
  start.
- **No `Virtual_Accept` / `Virtual_Back` keys** (no such strings in either eboot). Inferred: this build's Slate
  takes Enter, SpaceBar and `Gamepad_FaceButton_Bottom` as a button's accept key directly (the 4.21 `SButton`), and
  its default navigation uses the arrows and Tab for keys, the D-pad and left stick for pads.
- **The message handler's vtable** is known from the mouse code (study-mouse-input.md 2.3):
  - Found: `OnMouseDown` +0x30, `OnMouseUp` +0x40, `OnMouseDoubleClick` +0x50, `OnMouseWheel` +0x60,
    `OnRawMouseMove` +0x78.
  - Inferred from the 4.21 order (`FGenericApplicationMessageHandler`), which fits those five: `OnKeyChar` +0x18,
    `OnKeyDown` +0x20, `OnKeyUp` +0x28.
  - The PS4 input code reaches the handler through a shared reference, e.g. 01.02 0xb75c5c
    `mov rdi,[rbx]; mov rax,[rdi]; call [rax+0x40]`. The owning object and its offset are **to read offline**
    from that function (it starts near 0xb74d20 in 01.02).
- **Raw-check natives** (found, `natives.py`):

  | Native | 01.00 slot / exec | 01.02 slot / exec |
  |---|---|---|
  | `PlayerController::IsInputKeyDown` | 0x526aed8 / 0x2a41e70 | 0x526b458 / 0x2a44c30 |
  | `PlayerController::WasInputKeyJustPressed` | 0x526b1a8 / 0x2a446e0 | 0x526b728 / 0x2a474a0 |
  | `PlayerController::GetInputKeyTimeDown` | 0x526ae58 / 0x2a41580 | 0x526b3d8 / 0x2a44340 |

### 8.2 The injection point

Two routes, both on the game thread at `scePadReadState`, which is the same place as the live cvars:

- **Through the handler:** `MessageHandler->OnKeyDown(keyCode, charCode, isRepeat)` (+0x20) and `OnKeyUp`
  (+0x28). These are the calls a PC platform layer makes. `FSlateApplication::OnKeyDown` turns the codes into an
  FKey through `FInputKeyManager`, whose tables come from the platform (`FPlatformInput::GetKeyMap` /
  `GetCharKeyMap`).
  - **Unknown: whether the PS4 platform fills those tables.** If it does not, every key becomes `EKeys::Invalid`
    and is dropped. `OnKeyDown`'s return value (handled or not) shows this in the first log line.
- **Past the code mapping:** `FSlateApplication::ProcessKeyDownEvent(const FKeyEvent&)` / `ProcessKeyUpEvent`,
  with an `FKeyEvent` whose FKey is copied from the `EKeys` static. It needs the function addresses and
  `FKeyEvent`'s layout and vtable. All three can be read offline from `FSlateApplication::OnKeyDown`, which builds
  one.

The Slate route covers menus and gameplay, as on a PC:

- Slate first offers the key to the focused widget: UMG accept and navigation.
- Unhandled keys go to the game viewport, then `UGameViewportClient::InputKey`, the player controller and
  `UPlayerInput`, which applies the action and axis mappings.
- Inferred: user index 0 becomes controller 0, the same local player as the pad. Keyboard and pad then merge in
  one `UPlayerInput`, as on a PC: both at once work with no switching.

Not taken: `APlayerController::InputKey` / `UPlayerInput::InputKey` directly. That is gameplay only; UMG accept
and navigation would not see the keys.

### 8.3 Gameplay through `DefaultInput.ini` keyboard mappings

- The table adds only pad keys, and `RemoveActionMapping` removes only exact matches (2.2). Lines such as
  `+ActionMappings=(ActionName="Jump",Key=SpaceBar)` added to the override pak's `DefaultInput.ini` therefore stay
  bound next to the table's pad key (inferred; T-SB2 tests the same rule with L3).
- Axes already list keys: `MoveForward` W/S/Up/Down, `MoveRight` A/D, `TurnRate` Left/Right (found). They work as
  soon as keys arrive. `LookUpRate` has no key; add one, for example PageUp/PageDown. Mouse look stays on the
  sceMouse path.
- Classic / Modern changes only the pad keys. Keyboard binds are per action, so they are independent of the
  in-game layout and of the pad. This is the per-device independence the user wants, with no switching.
- Dan's shared-button logic reads every key of `Interact` from `InputSettings` (`ProcessInputMappingChanged`), so it
  includes the keyboard keys.
- **Template leftovers in the stock ini that would go live and must be removed in the override:**
  - `MovementUp/Down/Left/Right` on W/A/S/D. Dan listens to `MovementLeft/Right` (actions 22/23) in gameplay, and
    the inventory navigates with `Movement*`, so give them non-WASD keys.
  - `GUIUp/Down/Left/Right` on the arrows; keep these for menus, but they share keys with `TurnRate`.
  - `GUINextSubTab` E, `GUIPreviousSubTab` Q.
  - `DebugIncrease/DecreaseSimulationSpeed` on Add / Subtract.
  - Also `bF11TogglesFullscreen`, `bAltEnterTogglesFullscreen`, and the console key Tilde (no console in a
    shipping build, inferred).
- **Developer key events in Blueprints would fire too** (found, 2.4). Each needs neutralizing before its key can
  be injected: a same-size edit of the key's FName in the class's `InputKeyDelegateBinding`, or the runtime never
  injects it.

  | Asset | Key | What it does |
  |---|---|---|
  | `UI/Map/MapPawn` | **G** | `SetLevelCompleted(ActiveLocation.Level, true)`: **marks the selected level completed in the save** (no guard seen) |
  | `UI/Map/MapPawn` | C, A, U | not decoded |
  | `BP_df_character` (Dan) | R | `RemoveHealth(30)`, guarded by `IsPackagedForDistribution()` (no-op in this build) |
  | `BP_df_character` | Five, NumPad 2/4/6/7/8/9 | debug: item effect, camera |
  | `BP_HH_Manager` | Y | guarded |
  | `BP_CameraManager` | Tab, [, ] | fades |
  | `WaterBody_BP0` | E | water plugin |
  | `GlobalUpdater_BP` | AnyKey | fires for pad keys too today |

### 8.4 Menus

| Menu input | Native keyboard | Notes |
|---|---|---|
| UMG accept (pause, options, title, pop-ups, dialogs) | Enter / Space on the focused button (inferred, 4.21 Slate) | no ini needed |
| UMG navigation | arrows, Tab / Shift+Tab (inferred) | |
| `GUIBack` (back everywhere, cutscene skip) | needs an ini key, e.g. Escape | Escape on both `Pause` and `GUIBack` could close and reopen pause in one press (both fire); test before binding both |
| `GUINext` (inventory, map screen, Button Layout) | ini key, e.g. Enter | Space too if wanted |
| Categories / subtabs / `GUISpecial` / `Movement*` (inventory) | ini keys chosen for menus (e.g. PageUp/PageDown, Q/E, a free key; `Movement*` on IJKL) | |
| **Raw pad checks** (`IsInputKeyDown(Gamepad_*)`): item splash (Cross, Circle), FMV and intro text (Circle), inventory (Triangle, L1, R1, L2, R2), bestiary (D-pad, left stick up/down, Circle, Triangle, L1, R1, L2, R2) | **miss the keyboard** | fallback below |

Fallbacks for the raw checks:

- **(a) Wrap `IsInputKeyDown`** (slots above), recommended. When the asked key is one of those pad keys and its
  keyboard menu key is held (Escape for Circle, Enter for Cross, arrows for the D-pad ...), return true. It
  affects only those checks: about 25 call sites in 5 assets per build, identified as in `pc_settings_hook.h` by
  the calling function's script size and the code position. Or compare the FKey's FName with the `EKeys`
  statics, which needs no site table.
- **(b) Keep sending the pad button for the menu keys only.** In gameplay those keys then also fire the pad's
  gameplay action: Enter would jump in Modern.
- **(c) Edit the checks' key in the pak.** That replaces the pad key, so the pad stops working there. Rejected.

### 8.5 Prompts

- The game's prompts read the active table row's sprite, which is a pad glyph per action (2.5). Under native
  keys, keyboard prompts would show the pad glyph of the current layout. The prompt is wrong whenever the keyboard
  key differs from the pad button's default keyboard key.
- `ue4_glyphs.py`'s Keyboard style redraws one atlas cell per **PS4 button**, but native binds are per
  **action**, and Cross holds both Jump (Modern) and `GUINext`.
- Showing the right key per action needs one cell per action: point each table row at its own sprite (a
  same-size import-index edit, while sprites are free) and redraw that cell with the action's key.
  - There are 16 atlas cells; about 11 gameplay rows and 5 menu rows need one each.
  - Sprites the table does not import (D-pad, sticks, Options) need the import-map extension (S3).
  - Fixed-sprite menu prompts (options pages) then show whatever their cell holds.
- The glyph style is chosen at start, so prompts do not follow the device in use; that is today's behaviour too.

### 8.6 Mouse, both devices, PS4 assumptions

- **Mouse look** is unchanged (sceMouse raw moves).
- **Mouse buttons** must stay out of Slate: a click the game received left the main menu deaf (T-MS2). Two
  options: keep them as pad buttons (today), or inject them as FKeys straight into `UPlayerInput` (gameplay only,
  `LeftMouseButton` statics exist). The second needs the player controller's input object (offline).
- **Keyboard and pad at once:** one player, one key state (inferred). This is the strongest point of this
  mechanism.
- **PS4 assumptions to watch (inferred):**
  - the `FKeyEvent` user index must be 0 (scePad user 1 is controller 0);
  - Slate keyboard focus is user 0's focus;
  - nothing on PS4 expects text input (no IME use was found for keys);
  - the port's overlay must stop injection while it captures input, and release every injected key on focus loss
    or menu open, as `hold_after_capture` does for pad buttons.

### 8.7 Size, risks, first experiment

- **Size, about 3-4 weeks in all:**
  - Offline (Ghidra, both builds): the handler's owner, `OnKeyDown` → `ProcessKeyDownEvent`, the PS4 key map
    question, `FKeyEvent` layout. 2-3 days.
  - Runtime: injection with auto-repeat, capture and focus rules, the `IsInputKeyDown` wrapper. About a week.
  - Pak: the keyboard ini and the developer key neutralizing. 2 days.
  - Launcher: the keyboard column becomes per action (about 20 actions plus axes), and old binds migrate. 2 days.
  - Prompts per action (optional, needs S3). 3-5 days.
  - Tests and launches.
- **Risks:**
  - Engine C++ internals per build (non-UFUNCTION addresses, struct layouts) where the port so far wraps only
    named natives.
  - The PS4 key-code tables may be empty: the `ProcessKeyDownEvent` route is then needed.
  - Live developer keys (the map screen's G edits the save).
  - The ini leftovers.
  - Prompts that do not match keyboard keys until the per-action cells exist.
- **First experiment (X-N1; offline work first, then one launch):**
  1. Offline, find the handler pointer's owner and `OnKeyDown` in 01.02.
  2. Opt-in runtime test: while the test key is held, call `OnKeyDown` / `OnKeyUp` for Space (key code 0x20, char
     ' ') once per press. Log `native key SpaceBar: handled=<bool>`.
  3. Pak: `DefaultInput.ini` override with `+ActionMappings=(ActionName="Jump",...,Key=SpaceBar)`.
  4. Launcher: Space unbound from Cross for the test, so it reaches the game only natively.
  - Expected: the test key makes Dan jump, and on a focused pause-menu button it accepts.
  - If `handled=0` and nothing happens, the key-code tables are empty; X-N2 then tries `ProcessKeyDownEvent`.

## 9. Truth table

**Assumptions:**

- Controllers: menus on the physical defaults; gameplay from the stock table, or the pad's own static table
  (section 3, C-c) if the pad is remapped.
- Keyboard: an enforced menu set.
  - Native (**N**): Enter / Space accept, Escape back, arrows navigate, plus the ini menu keys.
  - Device-switched table (**T**) and additive keys (**A**): the 16 default keys on their default PS4 buttons.
- Keyboard gameplay binds come from N, T or A.

**Marks:** ✓ works as intended; ~ works with a stated cost; ✗ breaks. "Prompt" is what the game's prompt shows,
with the Keyboard glyph style where the keyboard is used; "pad glyph" means the PS4 or Xbox picture.

| # | Device | Context | Bind | N: behaviour / prompt | T: behaviour / prompt | A: behaviour / prompt |
|---|---|---|---|---|---|---|
| 1 | Pad only | gameplay | default (Classic or Modern) | ✓ stock / pad glyph | ✓ stock table / pad glyph | ✓ / pad glyph |
| 2 | Pad only | game menus, pop-ups, skip | any | ✓ physical defaults / pad glyph | ✓ | ✓ |
| 3 | Pad only | port ImGui menus | any | ✓ fixed menu buttons | ✓ | ✓ |
| 4 | Keyboard only | gameplay | default | ✓ ini defaults / ~ pad glyph unless per-action cells | ✓ table = stock / ✓ default key | ✓ / ✓ default key |
| 5 | Keyboard only | game menus (pause, options, title) | any | ✓ Enter/Space accept, arrows, Escape back (ini) / ~ menu prompt cell (one key per cell) | ✓ Space, LShift, IJKL ... / ✓ | ✓ / ✓ |
| 6 | Keyboard only | inventory, bestiary (raw pad checks) | any | ~ needs the `IsInputKeyDown` wrapper (else ✗ held-shoulder and bestiary navigation) / ~ | ✓ keys send pad bits / ✓ | ✓ / ✓ |
| 7 | Keyboard only | pop-ups in gameplay (lost soul dialog, confirmations, books) | any | ✓ Slate accept + `GUIBack` / ~ | ✓ / ✓ | ✓ / ✓ |
| 8 | Keyboard only | new item splash (raw Cross/Circle) | any | ~ wrapper needed, else ✗ (cannot dismiss) | ✓ | ✓ |
| 9 | Keyboard only | cutscene skip: sequences (`GUIBack`) | any | ✓ Escape (ini) / ~ prompt from the `SkipSequence` row | ✓ LShift / ✓ | ✓ / ✓ |
| 10 | Keyboard only | cutscene skip: FMV, intro text (raw Circle) | any | ~ wrapper, else ✗ | ✓ | ✓ |
| 11 | Keyboard only | port ImGui menus | any | ✓ (runtime reads keys; no injection while open) | ✓ | ✓ |
| 12 | Keyboard only | gameplay | gameplay key moved onto a default menu key (Attack on Space) | ✓ Space attacks; in menus Slate accepts and gameplay listeners are blocked / ~ prompt | ✓ Space sends Cross; keyboard table puts Attack on Cross / ✓ | **✗** not expressible: Space must stay Cross |
| 13 | Keyboard only | game menus | same bind as 12 | ✓ Space still accepts | ✓ | ✗ (see 12) |
| 14 | Keyboard only | gameplay | gameplay key on a non-menu key (Jump on G) | ✓ / ~ prompt | ~ G takes a free slot (Square preferred); the freed default key also jumps / ✓ cell redrawn G | ~ G added to Jump's slot / ~ cell shows the default key |
| 15 | Keyboard only | game menus | same as 14 | ✓ G does nothing in menus | ~ G acts as its slot's button (none for Square; accept if it got Cross) | ~ G acts as Jump's slot (Modern: Cross, so G accepts) |
| 16 | Keyboard only | gameplay | action left unbound | ✓ no ini key | ✓ row key `None` | **✗** a default key cannot be removed without losing its menu use |
| 17 | Keyboard only | gameplay | two actions on one key | ~ both fire (as on a PC); game conflicts as in Classic R1; warn | ~ two rows on one slot | ~ only if the stock table already shares that slot |
| 18 | Keyboard only | Classic ↔ Modern switched in Options | custom | ✓ keyboard unaffected (per action) | ✓ the runtime re-applies the keyboard table after the game's apply / ~ one frame of stock | ~ meaning of extra keys follows the layout |
| 19 | Both at once | gameplay | custom keyboard, stock pad | ✓ both work together (one player) | **✗** the table belongs to the first device; the other device's presses act under it | ✓ |
| 20 | Both at once | game menus | any | ✓ | ✓ menus are fixed on both | ✓ |
| 21 | Switching mid-game | gameplay | custom keyboard | ✓ nothing to switch | ~ switches on a press edge with nothing held; held across is blocked (see 19) | ✓ |
| 22 | Switching mid-game | game menus | any | ✓ | ✓ (the table is irrelevant in menus) | ✓ |
| 23 | Any keyboard | prompts with the PlayStation / Xbox style | any | ~ pad glyphs | ~ pad glyphs | ~ pad glyphs |
| 24 | Pad only | gameplay | pad remapped (C-c static table) | ✓ / ✓ pad glyph of the moved button | ✓ pad table = C-c / ✓ | ✓ / ✓ |
| 25 | Keyboard only | map screen | G pressed | **✗ marks the level completed** unless the G key event is neutralized in the pak or never injected | ✓ (G sends a pad bit or nothing) | ✓ |

Rows that break: N 25 (until neutralized), and N 6/8/10 without the wrapper; T 19 (both at once); A 12, 13 and 16
(the user's own example cannot be expressed).

## 10. Updated recommendation (keyboard-first)

1. **Target: native keyboard input (N).** It gives full per-device independence with no device switching, which
   is the only mechanism with both devices working at once (row 19). Keyboard binds become per-action UE4
   mappings, the game's menus get keyboard keys natively, and Classic / Modern affects the pad only.
   - Its costs: engine-internal addresses (offline), an `IsInputKeyDown` wrapper for the raw menu checks, ini
     cleanup, neutralizing developer keys (the map screen's G), and prompts that stay pad glyphs until per-action
     cells exist.
   - Stage it: offline find, then **X-N1** (one Space press, ini `Jump=SpaceBar`, a log line with the handled
     flag). It decides feasibility before any launcher work.
2. **Fallback if X-N1 / X-N2 fail: per-device tables (T, 7.1),** staged X1 → X2 → X3.
3. **Optional quick win: additive keys (A, 7.2),** about 1 day, valid under any of them.
4. **Pad:** stays on today's map, or the static C-c table if the user wants pad remaps to leave menus alone.
5. **Cutscene skip:** option 1 (fixed, menu-side). Under N, Escape is bound to `GUIBack` in the ini, and the
   FMV / intro raw Circle is covered by the `IsInputKeyDown` wrapper.
6. **T-SB1** (pak table edit) stays the cheapest proof that tables drive gameplay; it matters for T and C, not
   for N.

**Open questions for the user:**

1. The keyboard menu set under N: Enter + Space accept, Escape back, arrows navigate, plus keys for categories and
   subtabs?
2. Escape on `Pause` as well as `GUIBack`?
3. Should keyboard prompts show the right key per action (S3 work), or are pad glyphs acceptable at first?
4. Mouse buttons: keep them as pad buttons, or move them to native gameplay-only keys?

**The user's answers (2026-11-13):**

1. Keyboard menu set: W/A/S/D and the arrows navigate, Escape goes back, Enter and Space accept, Q / E change
   tabs. (WASD in menus is not Slate's default navigation: it needs its own mappings, checked during X-N work.)
2. Escape also opens and closes the pause menu, if possible.
3. Pad glyphs are fine for the first setup; once N works, keyboard key glyphs replace them for consistency.
4. Mouse buttons go the same native route as the keyboard: mouse navigation is planned for the game's menus and
   ours.
5. Both devices at once under the fallback: decided only if the fallback is needed.
6. Cutscene skip stays fixed on Circle unless that adds difficulty.

## Scratch tools and data

In `C:\Users\Ty\AppData\Local\Temp\claude\c--Users-Ty-MediEvil\78f7734e-4522-4580-95d1-94046d666589\scratchpad\`:

- `static-binds\`:
  - `rd.py`: one pak file.
  - `kstr.py`: strings (ASCII and UTF-16) in both eboots; `xref.py`: the code that references an address (the `EKeys` statics); `scan3.py`: name search over every asset header.
  - `ue_enum.py`: user-defined enum values and display names.
  - `ex.py`: an export list.
  - `dtoff.py`: DataTable rows with the `.uexp` offsets of key names, sprites and IDs.
  - `scan.py` / `scan_0102.txt`: input-related names over every asset header.
  - `scan2.py` / `scan2_0100.txt` / `scan2_0102.txt`: action and raw key events per asset.
  - `disall.sh` and `dis\*.txt`: every function of the input-related assets, disassembled.
  - `ogi_input_0100.txt` / `ogi_input_0102.txt`: the game instance's input functions per build.
  - `DefaultInput_0100.ini` / `DefaultInput_0102.ini`.
  - Extracted assets in `x0100`, `x0102`, `y0100`, `y0102`.
- Older tools reused:
  - `kis.py` (Kismet walker);
  - `glyphs\extract.py`, `glyphs\dt.py`;
  - `pc-settings-research\names.py`;
  - `natives.py`, which takes native names as arguments.
