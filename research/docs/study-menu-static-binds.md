# Study: the game's menus keep their buttons while gameplay binds are custom (2026-11-13)

Research only: nothing was built, launched or changed in the port or the game folders. Question (the user's): can
the game's own menus (title, pause, options, inventory, dialogs, ...) keep their default buttons while gameplay
binds are custom? Example: jump on Triangle, while Cross stays "accept" in menus; the same for keyboard keys.

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

## Scratch tools and data

In `C:\Users\Ty\AppData\Local\Temp\claude\c--Users-Ty-MediEvil\78f7734e-4522-4580-95d1-94046d666589\scratchpad\`:

- `static-binds\`:
  - `rd.py`: one pak file.
  - `enum.py`: user-defined enum values and display names.
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
