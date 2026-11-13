# Plan: the settings menu in the game's look, usable with a pad alone (2026-11-12)

Work queue item 5 (the overlay "restyled to the game's look") and the user's request of 2026-11-12: the port's
settings menu (the "Insert menu", `gpu/shim/gow3_overlay.cpp`) drawn like the Load list already is, and fully
usable with a controller. Branch `menu-redesign` from `fsr` `b4e3620`. Card:
[runtime/settings-menu](../patches/runtime/settings-menu.md).

## 1. What exists (found 2026-11-12, code at `b4e3620`)

**The menu.** One ImGui window (`Menu()`): title "MediEvil (Home / R3+L2)", a tab bar (Game, Controls, Image,
Performance, Overlay), a scrolling body of two-column tables (`BeginRows`: name left, widget right), a footer with
the hovered item's help (`Help()` -> `footer_help`), the restart notice with "Apply and restart", a Close button.
Row helpers: `CheckRow` (checkbox), `ChoiceRow` / `IndexChoiceRow` (combo), `SliderRow` (int slider),
`NumberRow` (float slider + typed box: mouse sensitivity and ratio), `MultiplierRow` (God of War III only),
`InfoRow`; custom rows: render scale (slider and - / + buttons, applied 0.3 s after the last step), graphics
quality (combo, "Custom" when nothing matches), the four "Game resolution now" buttons, the Overlay tab's seven
item checkboxes beside a live preview at the overlay's own font size (`overlay_scale`, "Font size").
There is no frame-time graph in the code (the Overlay tab's "Frametime" item is a number); nothing to keep there.

**Its input today.** Keyboard: the menu key (`GOW3_MENU_KEY`, Insert by default; the user uses Home) toggles, Esc
closes; every other key goes to ImGui, whose keyboard navigation is on (arrows move ImGui's own focus, Space /
Enter activate). Pad: R3+L2 toggles (`HandleEvent`, raw SDL events); while open, SDL gamepad events go to ImGui's
gamepad navigation (`KeyFromGamepad`): D-pad moves, A activates, L1 / R1 switch tabs; the left stick does nothing;
a combo needs A to open, then the D-pad inside its popup; a slider needs A, then left / right. Q / E switch tabs.
While open, `CapturesInput()` holds the game's pad neutral (`sample_host`), the cursor shows and the game's
mouse is let go (window.cpp `UpdateCursor`), and buttons still held at close stay hidden from the game until
released (`hold_after_capture`).

**The Load list (how the game look is done).** `StyledChoiceBox` draws with `ImDrawList` straight onto the
foreground: the Options menu frame (`menu/frame_options.png`, 1181x870, opaque fill under its middle), the title
in the game's title font (Fontesque) as "-LOAD GAME-", the rows in the text font (Champ Fleury), colours Cream
(240,234,220), Gold (245,196,30), Green (72,226,42), Dim (176,166,150), White; the chosen row gets a faint gold
fill (255,214,120,26); a thin scroll bar; the prompts under the frame: icon 50 + label 36 frame pixels, "SELECT /
GO BACK / DETAILS". All sizes are in the frame's pixels times `u` = drawn height / 870.
Assets: `scripts/game_ui_assets.py` reads them from the player's own paks at each start into
`user/cache/game_ui/` (frame, every PS4 prompt `prompts/PS4_*.png` 100x100, the two fonts); `run.py` then runs
`ue4_glyphs.overlay_prompts`, which writes `prompts/style/Cross|Circle|Square.png` in the chosen prompt style
(`button_prompts`: PlayStation = the game's, Xbox / keyboard = the CC0 pack's icon of the bound button or key,
mouse icons for mouse binds with "Prefer mouse binds for glyphs"). `gow3gpu_choice_style` (gow3gpu.cpp) loads
them once, on the game thread, when the first save list opens (`runtime_services.c`); all files or the plain look.
Input: the list polls the runtime's buttons every presented frame (`PollChoiceInput` -> `list_buttons` in
`runtime_pad.c`: pad, keyboard and mouse binds as mapped, the left stick as up / down, scripted input), plus SDL
key events for the arrows, Enter, Esc. The prompts follow the binds because the input is the mapped PS4 buttons.

## 2. Design

### 2.1 Look (when the game UI cache is there)

```
                 ~~~~ frame (game's Options frame, 86% of the height, 3% down) ~~~~
   |                              -SETTINGS-                                    |  title font, White
   |   [L1]   GAME   CONTROLS   IMAGE   PERFORMANCE   OVERLAY   [R1]           |  tabs: Gold = current
   |   ----------------------------------------------------------------------  |
   |                            -DISPLAY-                                       |  section: Green
   |  > Display mode                         <  Borderless window  >        |  |  focused row: gold fill,
   |    VSync                                <  On  >                       |  |   Gold label and value
   |    Sharpening strength                  [=========------]  75%         |  |  slider: track + value
   |    Game image                           3840 x 2160                    |  |  info: Dim
   |    (wrapped notes in Dim)                                              |  |  thin scroll bar
   |  ----------------------------------------------------------------------   |
   |   3 changes need a restart:  APPLY AND RESTART                            |  pinned row (only then)
   |   Help for the focused row, up to three lines (Cream)                     |
                 ~~~~                                                ~~~~
              [X] SELECT        [O] GO BACK        [L1][R1] CHANGE TAB             prompts, clickable
```

- The same frame, fonts, colours and prompt layout as the Load list; the frame is larger (86% of the height
  instead of 76%: more rows) and keeps its proportions; everything is sized in frame pixels times `u`.
  At 1280x800 (Deck) the row text is 28 * 0.79 = 22 px, help 24 * 0.79 = 19 px; at 720p 20 / 17 px; at 4K 60 px.
- Row types: toggle (`<  On  >`), choice (`<  value  >`, a value too wide for its column is drawn smaller),
  slider (track with a gold fill and the value; the mouse sensitivity and ratio keep their typed box), info (Dim,
  not focusable), buttons (the four game resolutions: the selected one Gold), action ("Reset all to 1x", God of
  War III only). Section headings "-NAME-" in Green. Notes stay ImGui wrapped text in the game font and colours.
- The FPS overlay preview stays in the overlay's own font at its own size (`overlay_scale`), so "Font size" still
  shows what it changes. The menu itself does not follow `overlay_scale` (see Decisions).
- Plain look (no cache, or a file missing): today's ImGui window, plus the pad navigation below, the focus
  highlight (a gold row background) and the focused row's help in the footer.

### 2.2 Controls

| | Pad (PS4 names; Xbox in brackets) | Keyboard | Mouse |
|---|---|---|---|
| Open / close | **L2 + R3** (LT + RS), unchanged; `GOW3_MENU_PAD` changes it | menu key (Insert / launcher), unchanged | - |
| Move between rows | D-pad or left stick up / down (repeats when held) | arrows, or the keys bound to the D-pad | moving over a row focuses it |
| Change a value | D-pad or left stick left / right (choices step, sliders step, toggles Off / On; held: repeats, sliders speed up after 1.2 s) | arrows left / right | click: toggle flips, choice arrows step, value cycles; drag a slider; type in the number box |
| Change / press | Cross (A): toggles flip, choices go to the next value (wrapping), buttons press | Enter, or the key bound to Cross | click |
| Back / close | Circle (B) | Esc, Backspace, or the key bound to Circle | GO BACK prompt; a non-left mouse button bound to Circle |
| Tabs | L1 / R1 (LB / RB) | Q / E, or the keys bound to L1 / R1 | the tab names, the L1 / R1 icons; a non-left mouse button bound to L1 / R1 |
| Apply and restart | focus the pinned row, Cross twice within 3 s | Enter twice | click twice |

- One input path for pad and keys, as the Load list: the overlay polls the runtime's mapped PS4 buttons every
  presented frame (`menu_buttons`, new in runtime_pad.c): pad buttons as mapped, keyboard keys as mapped, the left
  stick as a 4-way D-pad, scripted input (`GOW3_PAD_FILE`), and fixed menu keys (arrows, Enter, Esc, Backspace,
  Q, E). A fixed key counts only when it is not bound to a button the menu uses (the D-pad, Cross, Circle, L1, R1):
  with the default binds Enter is Options (unused) so it is Cross here; Q / E are Triangle / Square (unused) so
  they are L1 / R1 here. One bitmask, so a key that is both bound and fixed is still one press.
- Mouse binds: only Circle, L1 and R1, and never the left button (the pointer's): a left-click bound to Cross or
  Circle would act twice or close the menu at every click.
- ImGui's own keyboard and gamepad navigation are switched off (their focus and Space / A activation would double
  the menu's). ImGui still gets the keys while a number box is being typed in; the menu's input waits meanwhile.
- The game sees none of it: `CapturesInput()` is unchanged (menu open), so the pad stays neutral and buttons
  held at close stay hidden until released. Buttons held when the menu opens (the opening combo) wait for release.
- Focus: rows are numbered in the order a tab draws them; one focus per tab, kept while the menu is closed. The
  focused row is scrolled into view when the pad or keys moved it (the first row scrolls to the top, the last to
  the bottom so the notes under it show). Disabled rows can be focused (their help says why) but do not change.
- Open / close combo: L2 + R3 stays the default (documented in the README, launcher and ini header). MediEvil's
  R3 is the debug menu button (and RecenterCamera on 01.02); L2 alone is not a menu action. `GOW3_MENU_PAD` takes
  SDL gamepad names joined by `+` (`back`, `guide`, `misc1`, `paddle1`-`paddle4`, `leftstick`, `rightstick`,
  `lefttrigger`, `righttrigger`, ...), all held; an unknown name keeps the default. Not in the launcher yet.

### 2.3 Assets

| File in `user/cache/game_ui/` | From | Used for |
|---|---|---|
| `menu/frame_options.png`, `fonts/title.ttf`, `fonts/text.ttf` | game paks (game_ui_assets.py), as now | frame, title, text |
| `prompts/style/Cross.png`, `Circle.png`, `Square.png` | overlay_prompts, as now | SELECT, GO BACK (Square: Load list) |
| `prompts/style/L1.png`, `R1.png` (new) | overlay_prompts (`OVERLAY` + L1 / R1), else the game's `prompts/PS4_L1.png` / `PS4_R1.png` (already in the cache) | CHANGE TAB, the tab strip |

No new game asset is extracted: the cache already holds every PS4 prompt. The style is loaded at start (a
background thread once the overlay exists), no longer only when the first save list opens, so the menu has it
from the first open; still all files or the plain look.

## 3. Risks

- Fonts: Champ Fleury lacks some characters (the menu texts are ASCII; the about row prints the game serial and
  version: ASCII). Missing glyphs draw as ImGui's fallback box.
- ImGui 1.92 dynamic fonts: every size used rasterises glyphs on demand (once). Many sizes (the shrink-to-fit of
  long values) add atlas entries; sizes are rounded to whole pixels to limit that.
- The opening combo can still reach the game: the game reads the pad on its own thread, so R3 (pressed second)
  may be seen once before the event opens the menu (as before). R3 first then L2 gives the game R3 alone first.
- The menu doubles the frame rate's work while open (work queue: ~60 frames a second while open); unchanged.
- A save list and the menu never show together: `HandleChoiceEvent` takes every event while a list is open (as
  before), and the menu's poll runs only while the menu is open.
- `StyledChoiceBox` and the menu share `choice_style`; the save list's look must not change (it does not use the
  new L1 / R1 textures).

## 4. Test plan for the user (each launch needs the user's go)

Windows first, then the Deck. Settings to note before: `button_prompts` (launcher > Controls > In-game button
prompts) and whether a pad is connected.

- **T-SM1 Look (Windows, 4K or 1440p).** Open with the menu key. Expect the game's frame, "-SETTINGS-", the tab
  strip with L1 / R1 icons, green section headings, rows in the game font, the prompt bar under the frame. Visit
  all five tabs. Look for: clipped or overlapping text, a value running into the scroll bar, boxes instead of
  letters, the preview on the Overlay tab at the overlay's size. Screenshot each tab (F12).
- **T-SM2 Mouse and keyboard (Windows).** Mouse: move over rows (the highlight and help follow), click toggles, click
  a choice's arrows and value, drag sliders (Sharpening strength, Render scale: the scene changes once let go),
  type a sensitivity in the box, wheel-scroll the Performance tab, click tabs and the prompts. Keyboard: arrows
  move and change, Enter changes, Q / E and the L1 / R1 keys switch tabs, Esc closes. Expect: no double changes
  (a toggle clicked once flips once), the game does not move or attack while the menu is open, the mouse look
  comes back on close.
- **T-SM3 Pad (Windows, Xbox or DualSense).** Hold L2, press R3: the menu opens; the game does not react to the
  R3 (no debug menu, no camera recentre). D-pad and left stick: up / down move (held: repeat), left / right
  change; render scale held right speeds up and applies once let go; Cross changes; L1 / R1 switch tabs; Circle
  closes, and the game does not get that Circle (Dan does not act). With `button_prompts=xbox` the prompts show
  A / B / LB / RB; PlayStation: the game's icons; keyboard style: the bound keys (and mouse icons with "Prefer
  mouse binds").
- **T-SM4 Apply and restart.** Change Engine frame rate: the pinned row appears; Cross once shows "Press again";
  wait 3 s: it resets; Cross twice: the game restarts with the new value.
- **T-SM5 Load list unchanged.** Open the Load list from the title screen: same look and input as before (T-SV5).
- **T-SM6 Plain look.** Rename `user/cache/game_ui` (back it up), start: the plain ImGui menu with the gold focus
  row; the pad works there too. Put the folder back.
- **T-SM7 Steam Deck (1280x800, Steam Input on).** Open with L2 + R3; read every tab at arm's length: text size,
  help lines, prompt bar under the frame and above the screen edge; all of T-SM3's pad steps; the touch screen
  as a mouse. Note anything too small.
- **T-SM8 Custom combo (optional).** `GOW3_MENU_PAD=back+start` in extra-env.txt: the combo opens the menu; the
  start log says which combo.

## 5. Decisions for the user

1. **Open / close combo:** kept L2 + R3 (no new collision, every doc names it). `GOW3_MENU_PAD` changes it; a
   launcher box for it is not built (the launcher's Controls page would need a pad capture). Candidates: View +
   Menu (Back + Start: the game's touchpad and Options, both read), a Deck back paddle (only seen without Steam
   Input's own mapping).
2. **Menu size:** the menu follows the display (like the Load list), not `overlay_scale` ("Font size" stays the
   FPS overlay's). A separate "Menu size" setting can be added if the Deck test finds it small.
3. **Apply and restart** now asks for a second press within 3 s, with the mouse too (it used to restart on one
   click). It ends the game without saving.
4. **Square / Triangle** do nothing in the menu (no "defaults" action): resetting one row needs every setting's
   default in one table; the launcher has the defaults.
5. **Mouse binds** in the menu: only Circle / L1 / R1 on a non-left button.

## 6. Steps

1. This plan (commit).
2. Runtime: `menu_buttons` (pad, keys with the fixed menu keys, stick 4-way, mouse binds for Circle / L1 / R1,
   scripted) and `gow3gpu_menu_set_input`; test_pad_map.c cases for the key rules.
3. Overlay: the navigation model and its input (poll, repeat, tabs, close, focus, help, scroll) on the plain look;
   ImGui navigation off; `GOW3_MENU_PAD`.
4. Overlay: the styled look (frame, title, tabs, rows, prompts, pinned restart row); style loaded at start;
   L1 / R1 prompts (ue4_glyphs.py `OVERLAY`, gow3gpu.cpp).
5. Card, INDEX, work queue.

## 7. State

**Built, not run (2026-11-12).** Commits on `menu-redesign`: `64fb18a` (this plan), `7f6a98e` (runtime input and
the navigation, plain look), `b914242` (the game's look, the style loaded at start, L1 / R1 prompts). Build clean
(no new warnings; the two `vk_rasterizer.cpp` -Wunused-result warnings were there before), Python tests 189 OK
(31 skipped), pad-map-test PASS (built by hand: its CMake target lacks winmm, found). Offline: every character
of the menu's texts is in the game's text font; the tab strip and prompt bar fit the frame. A window under 360 px
high keeps the plain look. Nothing launched: T-SM1-T-SM8 are next, with the user's go.

## 8. T-SM1-T-SM3 (2026-11-12, `409a71d`, Windows, 4K fullscreen, keyboard prompts with mouse binds)

The user: "The in-game menu worked great." Screenshots (NVIDIA, `Videos/NVIDIA/medievil-runtime.exe`, 21.07-21.09):
Controls (Advanced camera), Image, Performance. Found in them (polish, not run-breaking):

1. On the title screen the prompt bar sits on the game's own copyright line (both at the bottom edge).
2. The list stops well above the frame's bottom: about 100 px (of 1125) of empty frame under the help text while
   the list scrolls; a taller list area would show two more rows.
3. The list's bottom row is cut mid-text while scrolling (Image: "AMD FSR sharpening", Performance: "Reflections").
4. The tab icons follow the binds (left mouse-button icon and SHIFT), as designed.

Next (the user): reorganise the tabs; a workbook of every item per tab (research/docs/data/settings-menu-items.xlsx)
for planning that.
