# Study: a "PC Settings" entry in the game's own menus (2026-11-12)

Research only: nothing built, nothing launched. Status of every item: **found** (read from the game's data or
the port's code), not run.

## Summary and recommendation

The game's menus are fixed UMG trees. Each entry is its own widget in a `VerticalBox`, and nothing is built from a
data table or list view. Clicks reach Blueprint handlers through the class's `ComponentDelegateBinding`, and focus is
plain Slate navigation. The **pause menu already has a hidden, fully wired button**: `Button_Controls`,
"Switch to Classic Controls". It is `Collapsed` in both trees of `UI/PauseMenu`, it is in the highlight array, it has
a click handler, and its label comes from a localized string-table key. **Recommendation: bring that button back as
"PC Settings" first.** That takes four same-size edits to `PauseMenu.uexp`, two new keys in `STR_GENERAL` (with
optional translations in the 25 `Game.locres` files), and one exec wrapper in the runtime. The wrapper is the same
`FNameNativePtrPair` slot technique that `mouse_hook.h` and `camera_hook.h` already use. Every byte pattern was
checked unique in both 01.00 and 01.02.

The Options menu (`UI/OptionsMenu/WidgetOptionsMenu`) has no spare row in game. The only rows not shown in game are the
Languages radio buttons, and the main menu still uses them. A real Options row therefore needs new exports, which is
a structural package edit. That edit is cheaper than it looks, because the game left a removed row's plumbing
behind: the class still binds `ScreenAdjustBtn.OnClicked` to `BndEvt__ScreenAdjustBtn_...`, the handler calls
`OnScreenAdjustmentClicked`, and the Options manager still opens a `WidgetScreenAdjustment` page for it (both builds).
Adding a Button named `ScreenAdjustBtn` under "-SCREEN-" reuses all of that. Plan it as stage 2, with a Python
package rewriter that the round-trip test checks first. If the rewriter proves fragile, the fallback is a prompt drawn by
the overlay on the Options page.

## 1. Findings

Sources: the 01.00 paks (`C:\Users\Ty\MediEvil\Game Files\CUSA11227`) and the 01.02 paks (`E:\MediEvil\CUSA11227-0102`),
read with `research/tools/uasset_props.py`, the scratchpad Kismet walker from the camera study (`kis.py` / `kis3.py`,
in-memory offsets, `NM=12`) and new scratch readers in
`C:\Users\Ty\AppData\Local\Temp\claude\c--Users-Ty-MediEvil\78f7734e-4522-4580-95d1-94046d666589\scratchpad\pc-settings-research\`
(`exports.py`, `tree.py`, `rows2.py`, `strtab.py`, `locres.py`, `crc.py`, `vis.py`). **[V]** means read from the data,
**[I]** means inferred (UE 4.21 engine behaviour or analogy with the existing hooks).

### 1.1 The assets

| Asset (pakchunk0, both builds) | What | 01.00 vs 01.02 |
|---|---|---|
| `OVERBITE/Content/UI/OptionsMenu/WidgetOptionsMenu` | The Options list (`WidgetOptionsMenu_C`, parent `WidgetOptionsBase_C`) | differs (01.02 adds camera speed, Hide HUD, DanCam toggle, subtitles, 30 FPS lock) [V] |
| `.../OptionsMenu/WidgetOptionsmanager` | `WidgetOptionsManager_C`: holds the list and its sub-pages (Brightness, ButtonLayout, ColorblindMode, ScreenAdjustment), swaps them (`ReplaceMenu`) | differs [V] |
| `.../OptionsMenu/WidgetScreenAdjustment` | A sub-page with one TextBlock (`PMENU_SCREEN_ADJUST`, "Screen Adjust"); no button anywhere opens it | same [V] |
| `OVERBITE/Content/UI/PauseMenu` | Pause menu (`PauseMenu_C`) | differs slightly (cheat-code and restart code) [V] |
| `OVERBITE/Content/UI/MainMenu/MainMenu` | Title menu; its Options button opens the same Options manager (`GetOptionsWidget`) | differs [V] |
| `OVERBITE/Content/Texts/OST/STR_GENERAL` | StringTable, namespace `General`: 405 keys (01.00), 416 (01.02) | differs [V] |
| `OVERBITE/Content/Localization/Game/<25 cultures>/Game.locres` | Translations, locres version 1; source hash = UE4 `FCrc::StrCrc32` of the English source (checked on 3 keys) | [V] |

These packages are unversioned cooked UE 4.21 packages: summary `FileVersionUE4 = 0`, no custom versions, flags
`0x80040000`. They use event-driven-loader preload dependencies (`WidgetOptionsMenu`: 1781 entries, `PauseMenu`:
1215) [V]. Both widget classes have `bAllowTemplate = True` and `bValidTemplate = True`, and each package holds a
second copy of the tree under `WidgetArchetype.WidgetTree_0`. That copy is the fast-path template, from which every
instance is copied [V data / I engine].

### 1.2 How the Options menu is built (01.00 rows; 01.02 adds 5)

The menu is one `VerticalBox_0` with fixed slots [V]: **-AUDIO-** (FX, music and speech sliders) · **-CONTROLS-**
(Vibration, Button Layout, Invert Look; 01.02 adds Camera Speed, Hide HUD, DanCam toggle) · **-SCREEN-** (Brightness;
01.02 adds Show Subtitles, 30 FPS Lock) · **-ACCESSIBILITY-** (Colourblind settings) · **-LANGUAGES-** (System
Language, Български, Hrvatski). Every label is a `STR_GENERAL` string-table entry (`PMENU_*`, `OPTIONS_*`), except
the two language names.

- **Construct** (ubergraph 0x1516) runs `AddSelectionsToArray`, which fills `MenuSelections` (the focusable widgets)
  and `MenuTexts` (their labels) in list order. It then removes the four Languages rows and calls
  `UpdateLanguageSelect`, which puts them back only when the level is `Level_IN_MM` (the title screen). **In game the
  list has no hidden or spare row.** [V]
- **Navigation:** Slate focus. `Initialize` calls `SetInputMode_GameAndUIEx(PC, SFXSlider)`. The ubergraph Tick scans
  `MenuSelections` for `HasUserFocus`, colours `MenuTexts[i]` (`SetMenuHighlight`) and plays the up or down sound by
  index (0x1167-0x1487). [V]
- **Select:** each Button's `OnClicked` is bound through `ComponentDelegateBinding_0` to a
  `BndEvt__<Widget>_..._OnButtonClickedEvent` function. That function calls the ubergraph at its entry, which posts
  `UX_Menu_Pause_Click_Confirm` and calls a delegate (`OnBrightnessClicked`, `OnButtonLayoutClicked`, ...). The
  manager bound those delegates (manager ubergraph 0x0292-0x054a) to `ReplaceMenu(page, true)`, which hides the list
  and shows and `UIOpen`s the page. [V]
- **Back:** `RegisterInputEventHandlers` calls `DisableAllInputBelowPriority(9)` and registers
  `RegisterButtonDownEvent` for actions 8 and 26, which go to `OnClickedCloseBtn`, then `OnClose`, then the manager's
  `Close`. [V]
- **The removed Screen Adjust row** (both builds) [V]: the binding entry `ScreenAdjustBtn.OnClicked` →
  `BndEvt__ScreenAdjustBtn_K2Node_ComponentBoundEvent_5_...` is still in `ComponentDelegateBinding_0`. Its handler
  is ubergraph 0x04e5 (01.00) / 0x04ef (01.02): `OnScreenAdjustmentClicked`, then the shared confirm sound at
  0x048c. The manager binds `OnScreenAdjustmentClicked` to `ReplaceMenu(WidgetScreenAdjustment, true)`. Only the
  Button widget and its `ObjectProperty` on the class are gone, along with the Combat Flourishes and High Contrast
  rows, whose bindings are also still there.

### 1.3 How the pause menu is built

The pause menu is `VerticalBox_61` (auto-size, centred, inside the `UI_Options_Frame` at 0.5 scale) with these slots in order [V]:

| Slot | Widget | Label | Visible |
|---|---|---|---|
| VerticalBoxSlot_2 | Button_Continue | `PMENU_RESUME` | yes |
| VerticalBoxSlot_1 | Button_Restart | `PMENU_RESTART` | yes |
| VerticalBoxSlot_4 | CheckBox_Invert | literal "Invert Look" | **Collapsed** |
| VerticalBoxSlot_3 | **Button_Controls** | literal "Switch to Classic Controls", set at Construct by `Update KeyMap` | **Collapsed** |
| VerticalBoxSlot_5 | Button_Options | `MM_OPTIONS` | yes |
| VerticalBoxSlot_0 | Button_Quit | `PMENU_QUIT_TO_MAP` | yes |

- `Button_Controls` has the same transparent style as the other buttons, and `Text_Controls` uses the same font
  (Champ Fleury, 30) and colour as `Text_Options`. Its only difference is `Visibility = ESlateVisibility::Collapsed`
  (EnumProperty tag) in both copies. Offsets in .uexp: 01.00 `0x2f19` (class tree) and `0x382f` (archetype); 01.02
  `0x2f4b` and `0x3861`. The `Visibility` name index is 742 / 743. It is the last tagged property [V].
- It is in `MenuButtons` / `MenuTexts` (`AddButtonsToArray`, both builds), so focus highlight and the up/down sounds
  already cover it [V].
- **Label:** `Update KeyMap` (ubergraph 0x158f, 01.02 0x166a) runs at Construct (0x03e4) and after a toggle. It sets
  `Text_Controls` to `STR_GENERAL:OPTIONS_MODERN_CONTROLS_SWITCH` or `..._CLASSIC_...` by key map. Both keys are
  translated in all 25 cultures. **They are also used by `WidgetButtonLayout`**, so relabelling those keys would
  change the Button Layout page too [V]. The key strings occur once each in `PauseMenu.uexp`: 01.00 at `0x165a` /
  `0x16b3`, 01.02 at `0x16f1` / `0x174a` [V].
- **Click:** `BndEvt__Button_ControlScheme_...` enters the ubergraph at 0x13d7 (01.00) / 0x14b2 (01.02). It runs
  `PostEventAtLocationWithMotion(UX_Menu_Pause_Click_Confirm)` and then `jumpifnot (InCheatMode)`; in cheat mode it
  does `pop`. Otherwise it continues to `GetGameInstance → GetKeyMap → SetControlConfig(toggled) → Update KeyMap`. The jump
  is at .uexp 01.00 `0x1540`, bytes `07 3e140000 01 46000000 4d` (target 0x143e, `pop` at 0x143d), and at 01.02
  `0x15d7`, `07 19150000 01 47000000 4d` (target 0x1519, `pop` at 0x1518). Each pattern occurs exactly once in its own
  build and zero times in the other [V].
- The sound call's `EX_FinalFunction` is at mem 0x13ff (01.00) / 0x14da (01.02). The ubergraph's in-memory script
  size is 0x2253 / 0x226f [V]. As in `mouse_hook.h`, `Stack.Code` at exec entry should be the first argument, +9 [I].
- **Options from pause** (0x1d9a; in 01.02 the entry 0x1e6a jumps to 0x000f, after a not-in-cheat-mode check): confirm sound, `Unpause()`, `GetOptionsWidget().Open()`,
  `SetGamePaused(true)`. The pause menu closes and the Options manager opens over a paused game [V].
- Back and cancel: `RegisterEventHandlers` calls `DisableAllInputBelowPriority(8)` and `Unpause_Input` on actions 8
  and 26 [V].

### 1.4 What the port already has

- **Override pak:** `scripts/ue4_override.py` writes `pakchunk99-ps4_p.pak` (mounted with priority) from the
  player's own files at every start. It already holds same-size Blueprint edits with one variant per build
  (`DebugHUD.uexp`, `OverbiteGameInstance(Functions).uexp`; `apply_edits`, `:241-295`) and texture edits
  (`ue4_glyphs.py`). No current edit touches `PauseMenu`, `WidgetOptionsMenu`, `STR_GENERAL` or a locres file [V].
- **Exec wrappers:** `src/probe.c` with `mouse_hook.h` / `camera_hook.h`: the `FNameNativePtrPair` slot holds
  `image + exec`, and the wrapper takes `(Context, FFrame&, Result)`. FFrame Node +0x10, Object +0x18, Code +0x20,
  Locals +0x28. UFunction Script.Data +0x48 / Num +0x50 [V in code]. Slots of the natives this plan can use, found
  with the scratchpad's `natives.py` (name string, then pair, then `push rbp` prologue) [V]:

  | Native | 01.00 slot / exec | 01.02 slot / exec |
  |---|---|---|
  | `VibrateWWiseHook::PostEventAtLocationWithMotion` | 0x4ed3f88 / 0x667d20 | 0x4ed4508 / 0x66a680 |
  | `KismetSystemLibrary::MakeLiteralByte` | 0x51f6718 / 0x298d600 | 0x51f6c98 / 0x29903c0 |
  | `KismetSystemLibrary::ExecuteConsoleCommand` | 0x51f6088 / 0x2986be0 | 0x51f6608 / 0x29899a0 |
  | `GameplayStatics::SetGamePaused` | 0x519fa48 / 0x2910980 | 0x519ffc8 / 0x2913740 |

- **Overlay hand-off** [V in code]: `Gow3Overlay::SetOpen` (`gow3_overlay.cpp:143`; `menu_buttons_fresh`: buttons
  held at open wait for release); `CapturesInput()`, which makes the game's pad neutral (`runtime_pad.c:317`,
  `:565`); `hold_after_capture`, which hides buttons still held at close until they are released (`runtime_pad.c:477`,
  `:576`); the game mouse released under the menu (`window.cpp` `UpdateCursor`); keys consumed by `HandleEvent`.
  **No C entry opens the menu from the runtime**: `gow3gpu.h` has `gow3gpu_menu_set_input` and
  `gow3gpu_overlay_captures_input` only.

## 2. Approaches

| # | Approach | Where | Feasibility and evidence | Already have | Unknowns | Both builds | Label in all languages | Pad / mouse | Effort | Risk |
|---|---|---|---|---|---|---|---|---|---|---|
| **D1** | **Bring back the hidden `Button_Controls`** as "PC Settings": same-size pak edits plus an exec wrapper | Pause menu | High: the row, style, highlight, handler and label key path exist (1.3) | Override pak and `apply_edits`, exec wrapper pattern, overlay capture | Engine skips a renamed tag [I]; 6 rows fit the frame [I]; Code at exec entry [I] | Yes: per-build patterns checked unique | New keys in `STR_GENERAL` (English everywhere); optional translated `Game.locres` entries (hash function confirmed) | Slate focus as the other rows; `OnClicked` takes the mouse (study-mouse-input.md) | ~1 day + 2 launches | Low |
| A1 | **Structural pak edit: add a `ScreenAdjustBtn` row** under -SCREEN- that reuses the orphaned binding, handler and manager page | Options (title screen and in game) | Medium. New exports: Button, ButtonSlot, TextBlock, VerticalBoxSlot, and `ObjectProperty` ScreenAdjustBtn and ScreenAdjust_Text on the class (FName numbers avoid most new names). `VerticalBox_0.Slots` +1, `AddSelectionsToArray` +2 elements (for the highlight), preload dependencies, `bAllowTemplate` → false (a 1-byte bool, so only the class tree needs the row) or the same row in `WidgetArchetype` | The readers (scratchpad), the binding and handler, Kismet walker | A Python writer for unversioned 4.21 packages (header offsets, export table, preload deps); EDL order; slow path with `bAllowTemplate` false [I]; the trigger shares the confirm-sound call site with Brightness (0x048c), so it must tell the entry point apart (ubergraph `EntryPoint` local) or use a new import (`ExecuteConsoleCommand` marker) | Yes if the writer is generic (edits by export name, not bytes) | Same keys and locres as D1 | As every Options row | 3-5 days + 2-3 launches | Medium |
| A2 | The same row through UAssetAPI / UAssetGUI (VER_UE4_21, unversioned) | Options | High for a **prototype** on the developer's copy; not shippable as a file (game assets) and a .NET tool in the launcher is a new dependency | none | as A1 | per build by hand | as D1 | as A1 | 1 day prototype | Low (prototype only) |
| B1 | Runtime bytecode patch only (in-memory `Script` edits, as `camera_hook.h`) | Both | Repurposing works (D1's jump retarget could be in memory), but **adding** a row needs a new widget object, which bytecode cannot make without new imports or a grown script. Changing the visibility and label still needs data edits | camera hook | Freeing a grown `Script` buffer we allocated | per-build offsets | label needs the string table anyway | - | - | Medium; rejected as stand-alone |
| C1 | Runtime UObject construction (NewObject / StaticDuplicateObject of `ColorblindBtn`, `AddChildToVerticalBox`, `SetText`, delegate bind) from the hooks | Options | Possible in principle: UFunction pointers can be taken from loaded bytecode, as the camera hook takes UProperty pointers. It needs non-virtual engine C++ addresses per build (object creation, FText), ProcessEvent calls with parameter structs, and the duplicated `OnClicked` cleared | hooks | many engine internals, GC rooting, per-build addresses | two address tables | runtime FText | as other rows | 1-2 weeks | High |
| D2 | Relabel an existing Options entry that does little on PC | Options | **No candidate:** every in-game row does something on PC. Brightness, Colourblind, Button Layout, Vibration and Invert all work. Languages rows are title-screen only and real. 01.02's "30 FPS Lock" works under 60 FPS and exists only in 01.02 | - | - | no (01.02 only) | - | - | - | Rejected |
| E1 | Prompt drawn by the overlay on the Options page ("△ PC Settings" beside Select / Go Back, clickable in ImGui), opened by a button the page does not use | Options | Medium: needs "the Options list is the visible page" from the runtime (wrap `SetInputMode_GameAndUIEx` from `Initialize` plus the manager's `ShowWidget`/visibility, or read the list widget's `Visibility` byte each frame) | prompt glyphs and fonts (`game_ui_assets.py`), overlay | the game's action-ID map for the chosen button; sub-pages hiding the list | per-build natives | ours (ImGui text, own translations) | pad button + mouse | 2 days | Low-medium; not a list row |

## 3. Hand-off: opening the overlay and giving input back

1. **Open.** The wrapper runs on the game thread while the click handler runs. It must not call `SetOpen` directly,
   because `menu_open` is atomic but `requested_tab` and the ImGui state are not. Add `gow3gpu_menu_open()` (C,
   `gow3gpu.h` / `gow3gpu.cpp`) that sets an atomic request, and have the overlay (`Render` / `HandleEvent`) take it and
   `SetOpen(true)`. UMG fires `OnClicked` on release with its default `DownAndUp` method [I], so Cross is already up.
   `menu_buttons_fresh` covers it either way.
2. **While open.** `CapturesInput()` already makes the game's pad neutral and lets go of the game mouse. Keys go to
   the overlay, and the keyboard reaches the game only through the pad mapping. The pause menu therefore gets no
   input: Slate focus stays on "PC Settings", and the game's own listeners (`Unpause_Input`, priority 8) see nothing.
   The world stays paused because the pause menu paused it. The overlay never touches pause state; from Options the
   game was already paused by `SetGamePaused(true)`.
3. **Close.** Circle / Esc / Backspace close (`SetOpen(false)`), which saves settings and applies the render scale.
   `hold_after_capture` keeps the closing Circle from reaching the game until it is released, so the pause menu does
   not take it as Back [V code]. The game is back where it was: pause menu open, focus on the row. The game mouse
   comes back on the next click in a window, or by itself in borderless or fullscreen.
4. The `GOW3_MENU_KEY` / L2+R3 combinations keep working unchanged.

## 4. Recommended plan

| Step | Work | What it proves | Launch? |
|---|---|---|---|
| S1 | Offline: add a small package reader to `scripts/` (names, imports, exports, tagged-property tags; from `research/tools/uasset_props.py`) and a locres v1 reader and writer, with tests that reading then writing the unmodified `STR_GENERAL` and the 25 `Game.locres` files gives identical bytes | The tooling round-trips the player's files | no |
| S2 | `ue4_override.py`, an opt-in switch (`pc_settings_entry`, default on once verified). (a) `PauseMenu.uexp`: in both `Button_Controls` exports, rename the `Visibility` tag name to an existing name that is not a UButton property (for example `Button_Controls`), so the property is skipped and the button stays Visible. (b) Rename both key strings to new keys of the same length (`PCPORT_PC_SETTINGS_MODERN_CTRL` is 30 characters and `PCPORT_PC_SETTINGS_CLASSIC_CTRL` is 31). (c) Retarget the jump: 0x143e → 0x143d (01.00), 0x1519 → 0x1518 (01.02), so the click only plays the confirm sound. (d) Optional: swap `VerticalBoxSlot_3` / `_5` in both `VerticalBox_61.Slots` and the `Button_Controls` / `Button_Options` (and text) operands in `AddButtonsToArray`, so the row sits after Options. `STR_GENERAL`: append the two keys with "PC Settings" (export `SerialSize` and `BulkDataStartOffset` updated). Unit tests: every edit applies once per build and changes nothing else | Data edits are computed from the player's files for both builds | no |
| S3 | Runtime: wrap `PostEventAtLocationWithMotion` (slots in 1.4). On a call whose Node has script size 0x2253 / 0x226f, whose bytes at the site match, and whose Code is at the first argument of the 0x13ff / 0x14da call, request the overlay (`gow3gpu_menu_open`). Log the first hit. Leave cheat-code mode alone (read `InCheatMode` through Object + property offset taken from the jump's operand, or accept the open) | The click is seen exactly at the revived row | no |
| **S4** | **Launch T-PC1** (below) | Row visible, labelled, highlighted, navigable; click is silent apart from the sound; wrapper sees the site | **yes** |
| S5 | Overlay: `gow3gpu_menu_open`; the runtime request opens the menu (S3 switched from log-only) | Hand-off: open, input captured, close without the game reacting | yes (T-PC2) |
| S6 | Optional: "PC Settings" translations in the 25 `Game.locres` (namespace `General`, the new keys, hash `StrCrc32("PC Settings")`); the user chooses English-only or translated | Label per culture | yes (one launch, a non-English culture) |
| S7 | Options row (A1): prototype with UAssetGUI on a scratch copy (A2) to learn what loads, then the Python package writer (new exports, class properties, preload deps, `bAllowTemplate` false), the row `ScreenAdjustBtn` / `ScreenAdjust_Text` under -SCREEN- with the same keys, handler 0x04e5 / 0x04ef's delegate call replaced by `EX_Nothing` (the page never opens). Trigger: `EntryPoint` local or an `ExecuteConsoleCommand("pcport.settings")` import with its wrapper | A real Options row on the title screen and in game | yes (T-PC3, T-PC4) |
| S8 | If S7 stalls: E1 (overlay prompt on the Options page) | A way in from Options without asset structure changes | yes |

### First launch for approval: T-PC1 (after S1-S3; 01.02, the user's daily build)

One change: the revived pause-menu row (pak edits S2 a-d plus the S3 wrapper in **log-only** mode; the overlay does
not open yet). Steps:

1. Start a level and press Options to pause. Expected: six rows, Resume, Restart, Options, **PC Settings**, Quit to
   Map. The row text is in the same font and colour, the frame still holds every row, and the highlight moves onto
   the row with the D-pad and stick and plays the up/down sound.
2. On PC Settings, press Cross. Expected: the confirm sound only. The controls do not switch, which shows in
   Options > Button Layout (its "Switch to ..." label is unchanged). The log shows one
   `PC settings entry: clicked (game build 01.02)` line per press.
3. Options, Resume and Quit to Map still work. With `GOW3_MOUSE=1`, a mouse click on the row gives the same log line.
4. Optional: the same on 01.00.

Collect: the log, plus a screenshot of the pause menu. T-PC2 (after S5): Cross opens the settings menu over the
paused game, Circle closes it, the pause menu is still open with the row focused, and the game does not unpause.

## 5. Open questions

1. Is "PC Settings" the wording, and should it be English in every language or translated (S6)?
2. Row position: after Options (S2 d) or where the game left it (between Restart and Options)?
3. Engine behaviour to confirm in T-PC1 [I]: a renamed property tag is skipped on load (4.21
   `SerializeTaggedProperties`). The fallback is to add an `ESlateVisibility::Visible` name, a header edit.
4. Does a sixth row fit the pause frame (`UI_Options_Frame` 1181 x 870 at 0.5, auto-sized list, rows 30 pt + 20 pad)?
   If not, shrink the bottom padding of the slot (same-size float edit).
5. Cheat-code mode: should the row open the menu there? The handler `pop`s in cheat mode, but the sound, and so our
   trigger, run before that check.
6. Options row: is `bAllowTemplate = false` enough in this cooked build, or must the archetype tree get the row too?
   And can the writer produce preload dependencies that EDL accepts? The S7 prototype answers both.
7. Should the title screen's Options get the row as well? A1 gives it there automatically, because the same widget is used.
