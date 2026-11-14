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
- The tests planned here after T-SM3 (Apply and restart, Load list, plain look, Steam Deck, custom combo) were
  never run; they are replaced by the list in section 9.6, which reuses the IDs from T-SM4 on.

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
high keeps the plain look. Nothing launched: T-SM1-T-SM3 were next, with the user's go (run, section 8).

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

The user, after T-SM2: the scroll bar does not work with the mouse: the wheel scrolls, but clicking or dragging
the bar does nothing. Found, not traced; to fix with the polish list above, after the user's tab reorganisation.

## 9. The reorganisation (2026-11-13, fix built, not run)

**Partly superseded by section 10** (the user's decisions on the report, the same day): the frame buffer size is
restart-only, the menu reads keys only as bound, APPLY & RESTART is Square + Touchpad held 5 s. 9.2, 9.3 and 9.6
below are updated to match; the rest stands.

The user's approved layout: the workbook (`research/docs/data/settings-menu-items.xlsx`, row order = intended
order) with the changes agreed afterwards, and the mockups drawn from it (geometry and texts: the mockup
generator's constants, renames and help texts). Commits on `menu-redesign`: `0bff4d6` (settings shared by the
launcher, run.py and the menu), `33491a9` (Square in the menu's input), `0666928` (the menu).

### 9.1 What changed per tab

Tabs: CONTROLS, DISPLAY, GRAPHICS, PERFORMANCE, OVERLAY. The God of War III cheats (GameTab) keep their code and
have no tab for now. `menu_tab` keeps its number, so a saved tab opens one tab to the right of before, once.

- **CONTROLS**: as before (the workbook's order is the code's).
- **DISPLAY**: -RESOLUTION-: Frame buffer size (was "Output resolution"; `output_resolution`; Match display /
  4K / 1800p / 1440p / 1080p (original) / 720p, any other WxH shows Custom), then Display mode. -FRAME CAP-: Engine
  frame rate (RESTART), Frame rate limit, G-Sync / FreeSync display, Effective limit (info, when it applies).
  -VSYNC-: VSync, Frames queued. -SCALING AND SHARPENING-: Upscale with FSR 1, Anti-aliasing / upscaler, Render
  scale (its two notes when they apply; help shortened to three lines), AMD FSR sharpening, Sharpening (RCAS),
  Sharpening strength. -INFORMATION-: Frame buffer, Render resolution ("W x H (N%)"), Window size, FSR 1,
  Sharpening, Upscaler. "Game resolution now", its buttons and note are gone. Frame buffer size is restart-only
  (section 10): it always carries the RESTART tag.
- **GRAPHICS**: one heading -GRAPHICS QUALITY-: Graphics quality, Preshadows, Effects resolution, the 13 live
  settings, then the 5 restart-only ones with the RESTART tag. Found and fix built: "Deck Optimized" never showed
  (the list stopped at `std::size(UE4QualityKeys)` = 5); it shows now and can be picked: it sets the Deck level's
  graphics settings only (ue4_quality low, as the launcher writes it), not the preset's other settings.
- **PERFORMANCE**: -PERFORMANCE PATCHES-: Single GPU particle pass, Particle flip guard (RESTART), Small buffers
  from VRAM, Lock-free memory map lookups, Cheap memory frees, Cheaper draw binding, Constant ring reuse, Compile
  new shaders in the background, HZB occlusion culling, Skip the decal depth prepass, Texture set memo, GPU data
  without waits, 4K memory sizes, Fewer render pass breaks, Game reads GPU data without waiting, Deferred GPU
  readbacks (the last eight RESTART). A dim note when the single pass is on but the flip guard is not running in
  this start (the pass row is not greyed out: the user may still compare). -DECK TUNED-: the Steam Deck switches,
  all RESTART; on Windows the three that do nothing there (A1, thread priorities, U2) are hidden.
- **OVERLAY**: as before; the "Game hooks" row (God of War III's hooks, always "none" in MediEvil) only shows for
  that game.

Frame and chrome (the mockups' numbers, frame pixels): title 50, tab strip 26 with 38 px icons; the frame's width
as before (86% of the height) and its straight middle (rows 300-600 of `frame_options.png`) stretched so it fills
92% of the height; rows 38 / text 26, info rows 30 / 23, headings 26; the list from 224 to 636 (plus the stretch),
the help (22 px, three lines) under it, the prompt bar inside the frame's bottom at 722 (34 px icons, 26 px labels),
clear of the title screen's copyright line; an 80% black dim over the whole screen behind the frame (the game's
pause menu stays drawn under it). The RESTART tag is small, dim, letter-spaced, in a rounded outline just left of
the value column. The list shows whole items only: its drawing is cut at the bottom of the last item that fits and
the top of the first one wholly in view, and rows outside the shown band take no clicks.

### 9.2 Restart, input, scroll bar

- The pinned "N changes need a restart" row is gone. While a restart-only change waits, the prompt bar has a fourth
  prompt: "[Square]+[Touchpad] HOLD: APPLY & RESTART" (section 10.3).
- The menu's input is the PS buttons as bound in the launcher, nothing more (section 10.2).
- The scroll bar takes the mouse (found by the user after T-SM2): a 16 px area over it, submitted before the rows
  so it gets the hover; a click on the track centres the thumb there, the thumb drags; the wheel as before. It
  brightens while hovered.

### 9.3 Frame buffer size: live switch against a start at that size (why it is restart-only now)

Built first as a live switch (`0666928`), made restart-only the same day (section 10.1). What starting at a size
does (run.py, `graphics_options.output_environment`, `ue4_override`) and what a live r.SetRes would not have got:

| At start | Live switch |
|---|---|
| "Boot display size WxH" rodata patch (or "4K display metrics at boot"): UE4's cached display metrics and work area, the game window, viewport, backbuffers, Slate geometry and UMG DPI start at the size (study-boot-resolution.md, I1) | Only the render targets and backbuffers are made again. The cached metrics and the hit-test grid of the game's own menus stay at the boot size: **I1 again** (the game's menus did not navigate at 4K after a live switch from 1080p; fixed by booting at the size). Any size other than the boot size, larger or smaller [smaller inferred]. Until the next start. |
| VideoOut at the size (`GOW3_INTERNAL_WIDTH/HEIGHT`) | Stays at the boot size [inferred: the game registers its buffers at boot]; the presenter scales what the game draws (Information > Frame buffer shows what it draws now). |
| Above 1080p: the 512 MB framebuffer pool patch forced, and `GOW3_DMEM_MB=7104` (4K memory sizes) | The memory is sized at boot (the game pools all direct memory once). From a 1080p start a live 4K switch runs in the PS4's memory pool (5,056 MiB): what `perf_4k_memory=0` gives at 4K, texture reloads on small camera moves, no crash known. The pool patch is in the default selection at every size; **unticked** (launcher > Patches, possible at 1080p and below) the stock 128 MB pool ran out on a live 4K switch: crash. |
| Render scale above 100% at that size | 1800p at 200% needed 6400x3600 targets and crashed (upstream `783b25a`). |
| Finer occlusion culling decided from the start's scene (up to 1280x800: on) | Kept: a 720p start switched to 4K keeps the full-size HZB (a 4096x4096 pyramid for nothing); a large start switched to 720p goes without it. |
| r.SetRes found per eboot (`GOW3_UE4_SETRES_CVAR`) | Without it nothing switches. |

The first build handled three of these cases (`OutputRestartReason`); the user then made every change restart-only
(section 10.1), which avoids all of them: the game always boots at the chosen size.

### 9.4 Particle flip guard, single particle pass, Deck switches

- Particle flip guard moved out of the Deck switches into the performance patches: `perf_particle_flip_guard`
  (run.py sets `GOW3_PARTICLE_FLIP_GUARD=1` through `PERF_ON_SWITCHES`), shown under Single GPU particle pass in
  the menu and the launcher, RESTART. **On by default** (the coordinator, 2026-11-13: the user reports a thorough
  PC test, performance unchanged, no torch / crypt floor glitch with the single pass). On in Deck Optimized.
- **Single GPU particle pass on by default** too (the user's decision, 2026-11-13): `PERF_DEFAULT_OFF` is empty;
  without a key run.py exports no `GOW3_PARTICLE_FIXED_STEP`, so the pak's `r.GPUParticle.FixDeltaSeconds=0` and
  the runtime's start value agree (single-particle-pass invariant). An explicit `perf_particle_single_pass=0` stays
  off. Help: it needs Particle flip guard.
- **Deck switch storage: medievil.ini** (`deck_<key>=0/1`, off by default, 1 in Deck Optimized). Why: the menu and
  "Apply and restart" must change them, and a restart only re-derives what run.py derives (LANDMINES 12); the
  launcher's settings.json reached the game only as variables the launcher set, which a restart inherits unchanged.
  Now: `graphics_options.DECK_SWITCH_ITEMS` / `deck_environment` (run.py sets the variables, a variable already set
  wins, Linux-only ones never on Windows, logged "Deck switches: ..."), `gow3_settings.h DeckSwitches` (the same
  table, a test checks), the launcher's rows bound to the same keys (the checkbox now means "all on" and sets all).
  Migration: `migrate_deck_switches` (launcher start, `--play` too) writes an old `deck_switches=true` into the
  keys (unchecked ones 0; an unchecked particle_flip_guard becomes `perf_particle_flip_guard=0`, an explicit off),
  then drops both keys from settings.json; `deck_switches=false` writes nothing (the defaults apply).

### 9.5 Polish fixes and the workbook's oddities

- Found and fix built: VSync differed between menu and launcher: the launcher marked its VSync row "applies at next
  start" though On / Off switch live in the game; only its two fixed modes need a start (row no longer marked, hint
  says so). In the menu VSync is greyed out with an explanation while the launcher fixes the present mode
  (`GOW3_PRESENT_MODE`), where the switch had no effect.
- Found and fix built: launcher help texts: Upscale with FSR 1 named the old "Render resolution 720p"; the
  performance patches' note said every patch applies at the next start (several switch live in the menu); the single
  pass's hint said only "off by default: torches".
- Found and fix built: God of War III text in MediEvil: Upscale with FSR 1's "Render resolution 720p + FSR 1" (kept
  for that game only) and the "Game hooks" row.
- Not done: the overlay preview box (Overlay tab) is a child window and is not cut to whole rows with the list.

### 9.6 Tests for the user (each launch needs the user's go)

Back up medievil.ini first; note `button_prompts`, the launcher's binds (Controls) and whether a pad is connected.

- **T-SM4 Look (Windows, 4K).** Open the menu: smaller title, the frame to ~95% of the screen bottom, prompts
  inside the frame above the copyright line, the help under the list, the game dimmed behind. Every tab against the
  mockups; RESTART tags only on restart-only rows (Frame buffer size always); while scrolling no half row at the top
  or bottom. F12 each tab.
- **T-SM5 Frame buffer size (restart-only).** Display > Frame buffer size 1440p: nothing switches now, RESTART tag,
  the APPLY & RESTART prompt appears. Hold it (T-SM7): the game restarts and the log says "Output resolution:
  2560x1440"; the game's own menus navigate at that size. Back to the old size the same way.
- **T-SM6 Binds and fallbacks.** With the launcher's default keys: I / K / J / L and the arrows move and change,
  Space and Enter (and keypad Enter) change, Left Shift and Esc close, 1 / 3 switch tabs; Backspace, Q, E and Tab
  alone do nothing (E and Tab only as Square / Touchpad, the restart hold). Bind an arrow to Square in the launcher:
  that arrow is Square in the menu, not a direction. Rebind Cross to another key: it works and its glyph shows in the
  prompt bar (never the fallback keys). A non-left mouse button bound to Circle closes; left click, wheel and the
  scroll bar work on the menu itself. The Load list (title screen) the same way: binds, arrows, Enter, Esc.
- **T-SM7 APPLY & RESTART held.** Change Engine frame rate: "[Square]+[Touchpad] HOLD: APPLY & RESTART" appears.
  Hold Square + Touchpad (pad: Square + Share / Create; keys: E + Tab): the prompt fills over 5 s, the help says
  keep holding; let go at 3 s: it empties, nothing happens; hold the full 5 s: the game restarts with the new
  value. Also holding the left mouse button on the prompt. Square or Touchpad alone does nothing.
- **T-SM8 Scroll bar.** Performance tab: wheel; click the track below the thumb (jumps), drag the thumb; the bar
  brightens under the mouse.
- **T-SM9 Graphics quality.** With the launcher's Deck Optimized chosen the row reads Deck Optimized; pick Low, then
  Deck Optimized in the menu: the settings follow; the five RESTART rows count as restart changes.
- **T-SM10 Deck switches and the guard.** Performance > Deck tuned: seven rows on Windows. Turn one on, APPLY &
  RESTART: the log's "Deck switches: 1 on: ..."; the launcher's Performance page shows it ticked. With an old
  settings.json (`deck_switches: true`), the launcher's first start moves them into medievil.ini.
- **T-SM11 New defaults and the one-time update (PC).** A copy of an ini with `perf_particle_single_pass=0`,
  `stale_readback=0` and no `settings_version`: the first start logs "Settings: one-time update to settings_version
  2: ...", the pass, the flip guard and stale readbacks are on (an ini already at version 1, like the user's: only
  stale readbacks); the Hall of Heroes lighting stays right (readback-first-wait) (log: guard installed, no `GOW3_PARTICLE_FIXED_STEP`); the lone torch and Dan's crypt floor look
  right (T-0102-1's spots); frame times as before. Turn the pass off in the menu, restart: it stays off.
- **T-SM16 Prompt glyphs.** PlayStation style, default binds: the restart prompt shows Square + SHARE, and the
  game's own HUD prompts that show the touchpad show SHARE too; rebind Cross to the Circle button on the pad: both
  show Circle there. Keyboard style: E + Tab; with a mouse button bound to Square and "Prefer mouse binds", its
  mouse icon.
- **T-SM12 Load list** (T-SV5's look; input now as T-SM6: binds and the arrows / Enter / Esc fallbacks, Square
  for the details as bound, prompts in the bound glyphs). **T-SM13 Plain look** (rename `user/cache/game_ui`: the
  ImGui menu, the new tabs, the held restart button). **T-SM14 Steam Deck** (1280x800: text size, help lines,
  prompts inside the frame, all of T-SM3's pad steps, Deck tuned shows all ten; Steam Input's mapping of the
  touchpad / Share for T-SM7). **T-SM15 Custom combo** (`GOW3_MENU_PAD=back+start`).

### 9.7 State

See section 10.6.

## 10. The user's decisions on the report (2026-11-13, fix built, not run)

### 10.1 Frame buffer size: restart-only

The live switch existed only because the original 4K implementation broke the game's menus (I1); a start at the
size has none of 9.3's gaps. Now a change only saves `output_resolution` and counts as a restart change
(`Gow3Settings::OutputNeedsRestart`: the value differs from the start's, custom WxH included); the row always shows
RESTART; help: "... Match display for a sharp 1:1 image. Applies at the next start." Removed: the overlay's
`RequestOutput`, `ApplyOutput` and its 0.5 s debounce and close-menu apply, `OutputRestartReason`, `OutputSize`,
the desktop size it needed. `runtime_ue4_request_setres` (src/runtime_extra.c) is left in place, unused: the menu
was its only caller (the start path `GOW3_UE4_SETRES` calls `ue4_setres` directly).

### 10.2 Menu input: the launcher's binds only

**Updated (the user, later the same day):** the arrows (D-pad) and Enter / keypad Enter (Cross) stay as standard
fallbacks in both menus, the Load list follows the same rules, and a bind wins over a fallback (an arrow bound to
Square is Square). "A bind wins" counts binds to the menus' buttons only: Enter is bound to Options by default,
which the menus do not use, so Enter is Cross there. Prompts always show the bound glyphs, never the fallbacks.

A key or mouse button does in the menu what the PS button it is bound to does (pad, keys, mouse as mapped:
`menu_buttons`), and the prompts show that bind's glyph (`prompts/style/`, written by `ue4_glyphs.overlay_prompts`
in the launcher's prompt style). Every hard-coded key found, and what happened to it:

| Where | Key | Did | Now |
|---|---|---|---|
| runtime_pad.c `menu_keys` | Up / Down / Left / Right | D-pad | **kept as a fallback** (removed, then restored by the user): only while the key is not bound to a menu button |
| runtime_pad.c `menu_keys` | Enter, keypad Enter | Cross | **kept as a fallback**, same rule |
| runtime_pad.c `menu_keys` | Backspace | Circle | removed (Circle: Left Shift) |
| runtime_pad.c `menu_keys` | Q / E | L1 / R1 | removed (L1 / R1: 1 / 3) |
| runtime_pad.c `menu_keys` | Tab | Square (added in `33491a9`) | removed; Tab is the Touchpad's default bind |
| runtime_pad.c `menu_keys` | Esc | Circle | **kept** as a fallback, same rule |
| runtime_pad.c `menu_mouse_buttons` | non-left mouse buttons | Circle / L1 / R1 / Square only | every menu button as bound; the left button stays the pointer's |
| runtime_pad.c `sample_host` (game input) | Tab | touched the pad (left) whatever the binds said | now the Touchpad's default bind in `key_map` (same effect by default; a rebind or "none" in the launcher now really moves or removes it) |
| gow3_overlay.cpp `HandleEvent` fallback (only without the runtime's poll) | arrows, Enter, Esc, Q, E, Tab | menu buttons | the arrows, Enter and Esc; the pad fallback (raw SDL buttons as the default map) kept, unreachable in the game |
| gow3_overlay.cpp | the menu key (Insert / `GOW3_MENU_KEY`), F8-F12 | open / close; global hotkeys | kept: no PS button (the pad's equivalent is the L2+R3 combo) |
| runtime_pad.c `menu_buttons` | the left stick | D-pad | kept (a stick, not a key; the Load list reads it too) |
| runtime_pad.c `sample_host` (game input) | Backspace, W / A / S / D, arrows | touchpad right half, left stick, right stick | unchanged: game input with no PS button of their own, not read by the menu |
| gow3_overlay.cpp `HandleChoiceEvent` (Load list) | Tab, Up / W, Down / S, Enter / Space, Esc / Backspace | details, move, accept, cancel | **the settings menu's rules** (the user): the list's poll (`list_buttons`) adds the same fallbacks (`menu_fixed_keys`) to its binds; key events act no more while that poll runs (they doubled bound keys); without it only the arrows, Enter and Esc. Tab, W, S, Space and Backspace are no fixed keys there now. Its mouse binds still count from every button, the left one included (the list has no pointer) |

### 10.3 APPLY & RESTART: Square + Touchpad held 5 s

"Select" in the port is the PS4 **Touchpad** button (the user's launcher: Touchpad = keyboard Tab, controller
"Share / Create"; runtime_pad.c `pad_map`: `SDL_GAMEPAD_BUTTON_BACK` and the DualSense touchpad both press it).
While restart-only changes wait, holding Square + Touchpad together (pad, or the keys / mouse buttons bound to them,
E + Tab by default) for 5 s restarts the game (`RestartHoldStep`, every presented frame); letting go of either
cancels; Square or Touchpad alone does nothing; a combination held when the menu opened waits for release. The
prompt bar shows "[Square]+[Touchpad] HOLD: APPLY & RESTART" (the Touchpad glyph in the prompt style:
`prompts/style/Touchpad.png`, new in `overlay_prompts`, fallback the game's `PS4_Touch_Pad.png`), filled in gold over
the 5 s; the help reads "Keep holding ...". **Also built (the optional part):** holding the left mouse button on that
prompt for 5 s does the same. The plain look has a "Hold Square + Touchpad 5 s (or hold here)" button. The arm /
confirm of 9.2's first build is gone.

### 10.4 One-time update of older medievil.ini files (settings_version)

The launcher writes every key it knows with its default, so an install from before a default changed keeps the old
value as if the user had chosen it. `graphics_options.SETTINGS_STEPS` / `settings_migration` / `migrate_settings`:
when the ini's `settings_version` is older than `SETTINGS_VERSION` (2), each step's keys are set once and the
version written; a later off stays. Run by run.py (`prepare`, logged "Settings: one-time update ...") and by the
launcher at start (after the Deck switch migration, before it reads the ini). A new ini starts at the current version
(launcher `INI_DEFAULTS`; run.py writes it into a missing file), so nothing is forced on a new install.

The list, from the released defaults (launcher `INI_DEFAULTS` and `PERF_DEFAULT_OFF` at v1.3.8, v2.0.0, v2.1.0,
v2.1.1, v2.2.0, v2.2.1, v2.3.0 and now; v1.4.0 is upstream's tag, not this fork's) and the cards:

| Key | Released default | Now | Evidence | Forced on once |
|---|---|---|---|---|
| `perf_particle_single_pass` | 0 in v2.0.0-v2.3.0 (`8d85929`, 2026-11-01) | 1 | single-particle-pass: T-H3b, Deck particle 1 (79.4 vs 77.1 FPS, 1,122 vs 1,316 draws); correct with the guard (particle fix 1 / 2); the user's decision 2026-11-13 | **yes** |
| `perf_particle_flip_guard` | none (a launcher Deck switch, off unless Deck Optimized; an unchecked one migrates as 0) | 1 | particle-flip-guard: Deck particle fix 1 / 2 (the user: torch animating, crypt floor fixed; "Yup, it's back" with it off); the user's PC test 2026-11-13 | **yes** |
| `stale_readback` | 0 in v1.3.8 and v2.0.0, 1 from v2.1.0 (`8600b28`) | 1 | the user: "we have specifically patched the issues that can arise from having this on". The fixes: readback-first-wait (a stale first readback fed UE4's sky light brightness: Hall of Heroes teal / white walls; verified by the user 2026-11-09, on by default), hzb-remap (stale HZB results culled Dan and the ground, T-HZ1; the remap serves the newest finished readback per primitive), W2 (the host-copy wait before watching pages; its removal in R1-stale-eop gave V2's black torch boxes). The user's ini and the Deck preset run it | **yes, step 2** |
| `fps_limit` | 0 in v1.3.8, -3 from v2.0.0 | -3 | a setting (frame cap), not a patch | no |
| every other `perf_*`, `present_*`, `display_vrr` | on from their first release (v2.0.0) | on | on all along: an off is the user's | no |

**Version 2, not 1:** version 1 had already run on the user's install when stale readbacks were added (their
medievil.ini got `settings_version=1` at 02:45 on 2026-11-13, after `2364371`), so a key added to step 1 would never
reach it. Step 2 holds only `stale_readback`; an ini at version 1 gets just that, an older one both steps.

Env-only fixes that became default since (`GOW3_READBACK_FIRST_WAIT`, `GOW3_POOL_ZERO_FOREIGN`) have no ini key and
already reach every install.

### 10.5 Prompt glyphs: the physical button or key a PS button is bound to

The user: every prompt shows the physical button the action is bound to, not the PS4 button's own icon (their
DualSense has Touchpad on Share / Create, so the restart prompt must show SHARE). Found: `ue4_glyphs.icons` already
did this for the Xbox and keyboard styles (the bound pad button's or key's icon, mouse binds with "Prefer mouse
binds"), but the PlayStation style always kept each PS button's own picture (Touchpad showed TOUCH). Fix built:
`icons('playstation')` maps each PS button to the game's own PS4 picture of the pad button it is bound to
(`PAD_PS4`: SDL back -> PS4_Share, a -> PS4_Cross, ...) where that is not its own picture (`OWN_PS4`). Both writers
use it, so the game's prompts and the menu agree: `overlay_prompts` (run.py, every start) copies the cache's
`prompts/<picture>.png` into `prompts/style/`, and `files` (the override pak) draws that texture into the atlas
cell, the small textures and the bone prompts. With the default binds only the touchpad differs: the game's atlas
cell and the menu's Touchpad prompt show SHARE (checked offline on the 01.00 paks: the atlas row's touchpad cell
is the game's SHARE art, every other cell unchanged). Before, the PlayStation style wrote no pak files at all.

The menu's Touchpad prompt is `prompts/style/Touchpad.png` (fallback the game's `PS4_Touch_Pad.png`); the prompt
and help name the button "Touchpad", as the launcher's Controls page does.

### 10.6 State

Fix built, not run. Build clean (no warnings in the last build), Python tests 206 run, OK (31 skipped),
graphics-settings-test PASS, pad-map-test PASS (built by hand). Tests: 9.6 (T-SM4-T-SM15,
updated for section 10, and T-SM16). Nothing launched. Commits: `3c42639` (binds-only input), `768ced0` (PlayStation
glyphs), `6ab5c19` (restart-only frame buffer, held restart), `2364371` (settings_version), `dc330e1` (arrows / Enter
fallbacks, the Load list on the binds), `fc4351d` (settings_version 2: stale readbacks).

### 10.7 First run (2026-11-13, run) and its fixes (fix built, not run)

The user's run of 9.6 on Windows at 4K (keyboard prompts, DualSense):

- **Run, as intended:** the layout (T-SM4, no half rows), arrows / Enter / Esc and Space (T-SM6), the held restart
  (T-SM7: empties when let go, restarts at 5 s, Square or Touchpad alone nothing), Frame buffer size (T-SM5), the
  flip guard on (T-SM10), the Load list, PlayStation glyphs (T-SM16). The DualSense works throughout. The mouse
  in the list did nothing on the first launch and worked on the second (nothing changed between them; not
  explained).
- **Found: the last row of most tabs hidden** (its help showed under the list). ImGui truncates the content height
  and the scroll to whole pixels (imgui.cpp CalcWindowContentSizes), so at the scroll's end the last row overhung
  the list by up to ~1.6 px (4K: 1.14 px) and the whole-row cut (0.5 px slack) dropped it. **Found too:** the
  scroll bar's InvisibleButton, placed at the scrolled top, stretched the scroll range past the content every
  frame (blank space, the list creeping). Confirmed offline (scratchpad `menu-scroll\harness.cpp`, real ImGui, the
  list's geometry at 2160 / 1440 / 1080 / 800 lines, 10-30 rows): as built the last row was hidden in 19 of 76
  layouts and the range too long in 57; with a 2 px slack (`CutSlack`) and the button's extent not counted, 0 and 0.
- **W/A/S/D:** the keyboard's left stick in play, so the menus ignored them while the game's own menus move with
  the stick. Now fallback directions like the arrows (a bind on the key wins); the Load list too.
- **Camera note:** "Turn off to disable behavior" (the user's wording) instead of "Untick a move".

Build clean; pad-map-test PASS (new W/A/S/D cases), graphics-settings-test PASS. To run: T-SM4 (every tab's last
row reachable, no blank space at the end, the Controls tab opens without creeping), T-SM6 (W/A/S/D move in the
menu and the Load list), T-SM8 (wheel, track click, thumb drag), T-SM9 (Deck Optimized, after the user's ini
backup).
