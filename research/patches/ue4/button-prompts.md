---
id: button-prompts
title: Button prompts: the game's PS4 prompt textures re-drawn in the override pak (medievil.ini button_prompts)
area: ue4
status: active
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits: 4bfd053951, 5ebcaa16ec, f82291a28b, 2598c44c39, aa5e865248, 396cc55a8a, 175941cf62, 29d3782e05
files: scripts/ue4_glyphs.py, scripts/game_ui_assets.py, scripts/ue4_override.py, run.py, tests/test_ue4_glyphs.py, third_party/input-prompts/README.md, packaging/windows/package.sh, packaging/linux/appimage.sh, launcher/medievil_launcher.py, gpu/shim/gow3gpu.cpp, gpu/gow3gpu.h, scripts/make_mouse_prompts.py, launcher/medievil_launcher.py
symbols: files, atlas, small, place, icons, key_icon, button_map, load_icon, visible, fit, SQUARE_TABS, SIZE_32x22, INVENTORY_SLOT, DEFAULT_KEYS, DEFAULT_PAD, PAD_ICONS, KEY_ICONS, overlay_prompts, OVERLAY, BUTTON_PROMPTS, gow3gpu_choice_style, ART, whole, with_whole, bc3, strip, with_strip, test_cells, test_square, texture, detile, tile, ATLAS_CELLS, SMALL, BONE, STYLES, MOUSE_ICONS, prompt_controls, blank_key, side_buttons
switches: GOW3_PROMPTS_PREFER_MOUSE
tests: T-G1, T-G1b, T-G2, T-G3, T-G3b, T-G4, T-G4c
docs: study-glyphs.md#4-step-plan-smallest-first-every-launch-needs-the-users-go, study-glyphs.md#21-format-of-the-cooked-texture-what-a-patch-must-write-v
depends_on: game-ui-cache
related: debug-menu-0102
checked:
---
## What it does

The ini key `button_prompts` (`playstation`, the default, writes nothing; `xbox`; `keyboard`; `test`) picks the
button prompt style. Xbox and keyboard show what each PS4 button is mapped to (the launcher's remaps,
`GOW3_PAD_MAP` / `GOW3_KEY_MAP`; keyboard: the pack's Vintage keys, the sticks as WASD / arrow clusters). `xbox` draws the CC0 Free Input Prompts pack's Default Xbox icons (`third_party/input-prompts/xbox/`).
Launcher > Controls > "In-game button prompts" sets it (until chosen: the Gamepad names style). The port's Load
list follows: `run.py` writes `prompts/style/Cross|Circle|Square.png` into the game UI cache at each start
(`overlay_prompts`) and `gow3gpu_choice_style` reads them. For a style other than PlayStation, `ue4_override.apply` puts edited copies of the game's prompt textures in
the override pak: today the 16 64x64 cells in the top row of `InventoryButtonAtlas` (the `SP_PS4_*` sprites),
which `test` fills with numbered yellow squares (00 Circle ... 15 StickRight, `ATLAS_CELLS`), and the three small
textures of the Button Layout page and the inventory / bestiary tabs, `L1_Active`, `R1_Active`, `Options_Active`
(16, 17, 18, `SMALL`), and the three pearl DXT5 prompts of the inventory, Daring Dash and tutorial box,
`PS4_Cross`, `PS4_Triangle`, `PS4_R1` (19, 20, 21, `BONE`). Step G1 of
study-glyphs.md; the Xbox and keyboard sets (G3, G4) and the bone textures (G2) come next.

## Invariants

- Same size and format as the game's texture: only payload bytes change, so the `.uasset` header (export size,
  bulk offsets, `PF_*` names) stays the game's. Never write another size or format.
- The atlas is B8G8R8A8 in 1D thin tiling; tile rows are stored one after another, so the cells' top 64 pixel rows
  are the first 256 KiB of the payload and are re-tiled alone (`strip` / `with_strip`). The rest of the atlas (HUD
  art) stays byte-identical.
- `detile` / `tile` (game_ui_assets.py) are exact inverses; the game UI cache's `decode` uses the same code.
- DXT5 is encoded by Pillow (11 or later; `bc3` takes the blocks from a DDS save after its 128-byte header). With an
  older Pillow only the `BONE` textures stay PlayStation, with a start-log line; the rest of the style applies.
- `ART` is `third_party/input-prompts` beside `scripts/` (the packages copy it); in the Windows build the bundled
  module's folder is `_internal\`, whose parent is the install folder, the same rule as `patches.py`'s `patches/`.
  A missing icon raises ValueError: the style is not applied (the pearl ones: only they stay PlayStation).
- `place` clears the whole box before drawing, so none of the PS4 art shows around a smaller icon.
- Icons are cropped at alpha > `VISIBLE` (16): the pack's keys fade to 1-2 out to the canvas edge.
- `SQUARE_TABS` (xbox / keyboard only): the menus that show `L1_Active` / `R1_Active` get square boxes (BestiaryMenu
  Friendlies, WidgetButtonLayout, WidgetOptionsmanager: ImageSize (32, 22) -> (32, 32); InventoryMenu: slot height
  44 -> 64, top -39 -> -49), so one square picture fits every screen. Each edit needs its 01.02 count (4 / 4 / 2 /
  4, both widget trees); another count leaves that menu as it is, with a start-log line. PlayStation keeps the
  game's boxes (its wide plate was drawn for 1.45:1).
- `DEFAULT_KEYS` / `DEFAULT_PAD` must equal the launcher's `CONTROLS` and `runtime_pad.c` (a test compares the
  launcher's).
- An unknown style counts as `playstation`. A game build whose atlas does not parse prints "Button prompts: not
  available for this game build" and the pak is written without it.

## Interactions

- load-list-details: the Load list's prompts come from `prompts/style/` (this card), else the cache's `PS4_*`
  (an older cache, or `overlay_prompts` failed: "Game UI files skipped" in the log).
- game-ui-cache: shares the texture reader and tiling code (`texture`, `detile`, `tile`); its decode output did not
  change (22 textures compared with the previous decoder).
- The pak is rewritten at every start, so the style changes with a restart.

## Evidence

- 2026-11-11, offline (01.02): the test atlas `.uexp` keeps its size (4 194 628 bytes); bytes change only from
  payload +48 to +262 095; the HUD art below row 64 decodes identically; the file reads back identically from a
  written pak. `apply` with `test` puts the atlas in the pak, with `playstation` or no key it does not.
  tests/test_ue4_glyphs.py passes.
- T-G1 (PC, 01.02): run. The Button Layout page shows the squares for every button but Start, L1 and R1 (their
  own textures, next step); the HUD's weapon swap prompt shows 02. Clean exit.
- T-G1b (PC, 01.02): run. With the three small textures, the Button Layout page and the bestiary tabs show only
  squares; the user: "everything looks correct". Clean exit.
- T-G2 (PC, 01.02): run. The inventory's pearl Cross / Triangle show 19 / 20; the user: "everything appears to be
  working". Offline: DXT5 differs from the drawn picture in about 30 of 10 000 pixels (edges). Clean exit.
- T-G3 (PC, 01.02): run. `xbox` on the HUD and menus; the user, with screenshots of the menus: "Everything looks
  fine, it seems to be working well." Clean exit.
- T-G3b (PC, 01.02): run by the user from the launcher, Xbox then PlayStation: "everything looks good, the load
  menu also works".
- T-G4 (PC, 01.02): run by the user from the launcher. Keyboard and remaps work; after the crop and squeeze fixes
  "Everything looks good" except the bestiary's Enemies / Bosses tabs (narrow keys: the 1.45 narrowing, since removed).
- T-G4c (PC, 01.02): square boxes. The user: "the bestiary all looks correct, as do the rest of the prompts".

- Mouse prompts (the user, 2026-11-12): with Keyboard prompts and the mouse on, a game button bound to a mouse
  button (GOW3_MOUSE_MAP) shows a mouse icon: always with Controls > Mouse > "Prefer mouse binds for glyphs" (on by
  default, `GOW3_PROMPTS_PREFER_MOUSE`), otherwise only where the key is unbound or has no picture (`icons`,
  `prompt_controls` in run.py; the game's prompts and the Load list's). The icons (left, right, middle, wheel up /
  down / either, side back / forward) are drawn in the round key style by scripts/make_mouse_prompts.py
  (third_party/input-prompts/README.md); redrawn after the user's review (`687d80b`: round outline, side
  buttons without a halo, the mouse as large as the key takes). T-MP1 (2026-11-12): the Load list showed the right
  button for Go Back and the left for Details / Summary (Space kept for Select, no mouse bind); the user: "looks
  great". Verified on the Load list.

## History and pitfalls

- The L1 / R1 tab boxes (T-G4): the menus show `L1_Active` / `R1_Active` 1.45:1 (brush 32x22; the inventory's slot
  64x44), except the bestiary's Enemies / Bosses pages (no stored brush size: 32x32). Drawing the icons 1.45x
  narrower fixed every screen but those two. Chosen (the user, 2026-11-11): **square everywhere** for xbox / keyboard
  (`SQUARE_TABS`, same-size float edits in BestiaryMenu, WidgetButtonLayout, WidgetOptionsmanager, InventoryMenu).
  **Future option, if the square tabs look off:** make Enemies / Bosses 1.45:1 instead: insert the 57-byte
  `ImageSize (32, 22)` tag (copied from the Friendlies `Image_1`, same name table) into the 8 Enemies / Bosses brushes
  of `BestiaryMenu.uexp` (both widget trees), brush tag size 37 -> 94; the `.uexp` grows by 456 bytes, so
  `BestiaryMenu.uasset` needs `SerialSize` +57 for those 8 exports, every later export's `SerialOffset` moved and
  `BulkDataStartOffset` updated. Built and re-parsed in scratch on 2026-11-11 (not run in game); then the icons go back
  to being drawn 1.45x narrower (git history: `STRETCH` in `175941c`). Cause and widget names: `study-glyphs.md` T-G4c.

- The repo's `.gitignore` ignores `*.png` (screenshots): the icons need its `!third_party/input-prompts/**/*.png`
  exception, or a commit leaves them out (the Xbox commit did; added in the next one).

- The game has no Xbox, keyboard or PC glyphs (study-glyphs.md 1.4): the new art comes from the CC0 pack the user
  chose (study-glyphs.md, Decisions).
