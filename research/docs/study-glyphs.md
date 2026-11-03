# Study: Xbox and keyboard button prompts (glyphs)

Status: study, 2026-11-03. Read-only. Nothing was built, run or changed in the port or the game folders. Parent:
[START-HERE.md](START-HERE.md).
Evidence: the game's own paks (01.02 at `E:\MediEvil\CUSA11227-0102`, 01.00 at `Game Files\CUSA11227`), read with
`scripts/ue4_override.py`'s `read_index` (`:61`) and `research/tools/uasset_props.py`'s `Package` (UE 4.21 tagged
properties). Scratch scripts and outputs (temporary, not in the repo) are in
`C:\Users\Ty\AppData\Local\Temp\claude\c--Users-Ty-MediEvil\78f7734e-4522-4580-95d1-94046d666589\scratchpad\glyphs\`:
`listpaks.py` (pak index dump, `index0102.tsv` / `index0100.tsv`), `extract.py <game> <regex> <out>`,
`scanrefs.py` (every `.uasset`/`.umap` header of every pak searched for glyph names, result `refs.tsv`), `dump.py`,
`dt.py` (DataTable rows), `texinfo.py`, `detile.py` / `decode_all.py` (PS4 texture payload to PNG),
`retile_test.py` (tiling round trip, and a test atlas `test_atlas.uexp`), decoded PNGs in `png\` (`sheet.png`,
`atlas_row.png`, `all\*.png`).
**[V]** means verified statically (assets, code). **[I]** means inferred. Nothing here was tested in game.

## Summary

- **The prompts are plain textures, picked through two DataTables.** There is no icon font, no rich-text decorator
  and no per-platform switch. Every gameplay / menu prompt comes from one function of the game instance,
  `GetButtonIconForAction` / `GetButtonIconForAxis`, which reads the row of the current control layout
  (`KeyRemapperTable_Classic` or `KeyRemapperTable_Modern`) and returns its `buttonPromptImage` (an `FSlateBrush`
  whose resource is a **Paper2D sprite**, `SP_PS4_*`). The sprites are 64x64 cells of **one 1024x1024 atlas
  texture, `InventoryButtonAtlas`** (top row, 16 cells; the rest of the atlas is HUD art). A few widgets use three
  other small textures (`L1_Active`, `R1_Active`, `Options_Active`) and three 100x100 "bone" prompt textures
  (`ButtonPrompts/PS4_Cross`, `PS4_Triangle`, `PS4_R1`). **[V]**
- **No Xbox or PC glyphs are left in the paks** (file names and all asset headers searched). The game shipped on PS4
  only; 01.00 and 01.02 have byte-identical glyph assets. **[V]**
- **The cooked textures can be patched in place.** The pixel data sits inline in the `.uexp`, already in the PS4
  GPU layout: **1D-thin tiling (8x8-element micro tiles in Morton order, row-major), sizes padded to multiples of 8
  elements, one mip, no compression for the atlas (B8G8R8A8), DXT5 for the bone prompts.** Decoding with that rule
  gives clean images and detile → retile gives back the identical bytes. New art of the same size and format
  replaces the payload with no header change, the same way `ue4_override.py` already ships edited `.uexp` files in
  `pakchunk99-ps4_p.pak`. The 16 glyph cells are exactly the first 256 KiB of the atlas payload. **[V]**
- **Recommended:** (a) a static choice written into the override pak at every start (PlayStation / Xbox /
  Keyboard), following the launcher's existing "Gamepad names" style and the user's key and pad remaps; then,
  if wanted, (b) a runtime switch by last-used device that rewrites the same payload bytes in guest memory and lets
  the texture cache re-upload. Option (a) needs no C/C++ change; (b) is a texture-cache change with real risk.

## 1. How the game draws prompts

### 1.1 Chain from action to pixels [V]

| Step | Asset / code | What it holds |
|---|---|---|
| Control layout tables | `OVERBITE/Content/Blueprints/KeyRemapping/KeyRemapperTable_Classic.uasset` / `_Modern.uasset` (DataTable, row struct `KeyRemapperStruct`) | 19 rows each (Interact, Jump, Attack1, Attack2, Block, SwapWeapon, Strafe, DanCam, TargetSwap, GUINext, GUIBack, OpenInventory, SkipSequence, GUINextCategory, GUIPreviousCategory, GUISpecial, LookUpRate, MoveForward, ShoulderCharge). Per row: `action` (`InputActionKeyMapping`, an FKey such as `Gamepad_FaceButton_Bottom`), `axis`, enum IDs, and `buttonPromptImage` (`SlateBrush`, `ImageSize` 32x32, `ResourceObject` = a sprite). |
| Which table | `Blueprints/GameInstance/OverbiteGameInstance` | `CurrentKeyMap` (`KeyMapsEnum`), `GetKeyMap`, `BindActionRemapper`, `GetButtonIconForAction`, `GetButtonIconForAxis`, `AddButtonPromptForAction`, `CreateNewButtonPrompt`, `ActiveButtonPrompts`. Also `GetCrossCircleButtonSwapped` (native, see 1.4). |
| Prompt widgets | `UI/ButtonPrompt` (UMG: `ButtonPromptImage` Image + `ButtonActionText`, `SetButtonPromptImage` → `SetBrush`; default brush `SP_PS4_Cross`), `UI/ButtonBar`, `UI/TutorialPrompt` (default `ButtonPrompts/PS4_Cross`), `Blueprints/InteractiveProps/InteractButton` (in-world UMG widget, calls `GetButtonIconForAction`, default `SP_PS4_Triangle`) | Brush set at runtime from the table |
| Sprites | `UI/Sprites/HUD/SP_PS4_*.uasset` (PaperSprite, `TranslucentUnlitSpriteMaterial`) | `SourceTexture` + `SourceUV` + `SourceDimension` (an atlas region) |
| Texture | `UI/Sprites/HUD/InventoryButtonAtlas` | the pixels |

The text prompts are split around the image: `STR_PROMPTS` has `DC_INVENT_BOOK_PROMPT1` "Press" and
`DC_INVENT_BOOK_PROMPT2` "to open your Inventory" with the image in between, so no string names a button glyph.
**[V]** Two Book of Gallowmere texts in `Localization/Game/en/Game.locres` carry placeholders (`<INVENTORY>`,
`<DARING DASH>`) and the English subtitles `<INVENTORY BUTTON>` / `<TOUCH PAD BUTTON>`; `UI_Book_Text` uses
`ButtonPrompt`, so the book most likely shows a prompt image for them. **[I]** One option label is literal text:
"Hide HUD with L3 Button". **[V]**

### 1.2 Atlas cells (all 64x64, row y = 0..63 of `InventoryButtonAtlas`) [V]

| x | Sprite | Used by (asset headers that name it, besides the two tables) |
|---|---|---|
| 0 | `SP_PS4_Circle` | `WidgetButtonLayout`, `WidgetOptionsmanager` |
| 64 | `SP_PS4_Cross` | `ButtonPrompt` (default), options pages |
| 128 | `SP_PS4_Triangle` | `PlayerHUD`, `ItemWidget`, `InteractButton`, options pages |
| 192 | `SP_PS4_Square` | options pages |
| 256 / 320 / 384 / 448 | `SP_PS4_DpadUp` / `Down` / `Left` / `Right` | options pages (`DpadUp`) |
| 512 | `SP_PS4_R1` | `PlayerHUD` |
| 576 | `SP_PS4_L1` (64x63) | tables |
| 640 / 704 | `SP_PS4_R2` / `SP_PS4_L2` | tables, options pages |
| 768 | `SP_PS4_Options` | — |
| 832 | `SP_PS4_Touchpad` | tables (OpenInventory) |
| 896 / 960 | `SP_PS4_StickLeft` / `SP_PS4_StickRight` | tables (MoveForward, LookUpRate) |

Style (`png\atlas_row.png`): polished steel. Face buttons are dark glossy discs in a steel ring with the coloured PS
symbol; R1/L1 are steel plates with serif lettering; R2/L2 taller shield-shaped plates; Options a steel capsule;
touchpad a steel rectangle; sticks steel discs with "L" / "R"; d-pad cells a steel cross with one arm lit.

### 1.3 All prompt textures [V]

Sizes are the texture's; "payload" is the padded GPU data in the `.uexp`.

| Texture (`OVERBITE/Content/UI/...`) | Size | Format | Payload | Referenced by (headers) |
|---|---|---|---|---|
| `Sprites/HUD/InventoryButtonAtlas` | 1024x1024 | B8G8R8A8 (TC_EditorIcon, UI group) | 4 194 304 at `.uexp` +300 | all `SP_PS4_*` except `_New`, HUD sprites |
| `Sprites/HUD/L1_Active` / `R1_Active` | 64x64 | B8G8R8A8 | 16 384 | `SP_PS4_L1_New` / `SP_PS4_R1_New` → `BestiaryMenu`, `InventoryMenu`, options pages |
| `Sprites/HUD/Options_Active` | 88x137 (88x144 padded) | B8G8R8A8 | 50 688 | `SP_PS4_Options_New` → options pages |
| `Textures/ButtonPrompts/PS4_Cross` | 100x100 (128x128 padded) | DXT5 | 16 384 at `.uexp` +281 | `TutorialPrompt`, `Inventory`, `KeyRemapperStruct` (default) |
| `Textures/ButtonPrompts/PS4_Triangle` | 100x100 | DXT5 | 16 384 | `Inventory`, Classic table (ShoulderCharge) |
| `Textures/ButtonPrompts/PS4_R1` | 100x100 | DXT5 | 16 384 | Modern table (ShoulderCharge) |
| `Textures/ButtonPrompts/PS4_*` (other 15), `HUD_PS4Button_Triangle` (64x64 BGRA) | | | | no header names them: unused |
| `Sprites/Options/PS4_Controller` | 1024x754 | B8G8R8A8 | 3 112 960 | `WidgetButtonLayout`, `WidgetOptionsmanager` (Options > Button Layout page; the green lead lines are part of the picture) |
| `Sprites/Options/ps4-controller` | 829x612 | B8G8R8A8 | 2 050 048 | unused |
| `Textures/DPadIcon` (94x94), `UI_Button_Dpad_Up` / `_Down` (185/186x278) | | B8G8R8A8 | | `PlayerHUD` (d-pad item HUD; generic, no PS-specific shape) |

The bone prompts are parchment discs with the coloured symbol (`png\sheet.png`). All of these live only in
`pakchunk0-ps4.pak`; the 01.00 and 01.02 copies have the same SHA-1 (atlas `.uexp` `a473f2fd6f7c…`, `PS4_Cross.uexp`
`c1c33192d5a9…`, `L1_Active.uexp`, `KeyRemapperTable_Classic.uexp`). **[V]**

### 1.4 Platform logic [V]

- No Xbox, keyboard or PC glyph asset exists. Searched: all 73 450 file names (01.02) for `xbox`, `xb1`, `keyboard`,
  `mouse`, `glyph`, `_pc_`, and all asset headers for the same plus `RichText`, `Glyph`, `InputDevice`,
  `IsUsingGamepad`. The only hits are UE4 engine leftovers (mobile HUD joystick, Slate editor icons) and window
  meshes. The custom font `UI/Fonts/MediEvil.ufont` (pakchunk1/17) is a 138-character Latin text font used by the
  countdown timers, not an icon font.
- The only platform-dependent glyph logic is `GetCrossCircleButtonSwapped`
  (`/Script/OVERBITE.OverbiteGameSingletonFunctionLib`, native), read by the game instance. It follows the PS4
  system parameter "enter button", which the port answers with Cross: `src/runtime_services.c:88`
  (`case 1000: *value=1`). Xbox's A-confirms / B-backs matches that, so nothing to change.
- `Pickup_Helmet` calls `GetPlatformName` (not prompt related). The eboot has two `XboxOne` strings (engine).
- The game only ever sees a PS4 pad: `src/runtime_pad.c` merges SDL gamepads (`:143`) and the keyboard
  (`:153`, `:293-301`) into one libScePad state. The game cannot know which device was used. The launcher already
  has a PlayStation / Xbox naming style (`launcher/medievil_launcher.py:197` `PAD_STYLES`, setting `pad_style`) and
  per-button key / pad remaps (`:187` `CONTROLS`, passed as `GOW3_KEY_MAP` / `GOW3_PAD_MAP`, `:491`).

## 2. How to swap them

### 2.1 Format of the cooked texture (what a patch must write) [V]

`Texture2D` export in the `.uexp`, after the tagged properties: `bHasGuid` 0, two `FStripDataFlags`, `bCooked` 1,
pixel-format FName (`PF_B8G8R8A8` / `PF_DXT5`), skip offset (int64), `SizeX`, `SizeY`, packed slices (1), pixel-format
FString, first mip 0, `NumMips` 1, then the mip: `bCooked`, bulk flags `0x48` (ForceInlinePayload | SingleUse),
element count = size on disk = payload size, offset, **the payload**, mip `SizeX/SizeY/SizeZ`, `None`, package tag.
The payload is the PS4 GPU image:

- Elements: one pixel (4 bytes) for BGRA, one 4x4 block (16 bytes) for DXT5.
- Width and height in elements rounded up to a multiple of 8 (100x100 DXT5 → 25x25 blocks → 32x32; 88x137 → 88x144).
- 1D-thin tiling: 8x8-element micro tiles stored row-major; inside a tile the element index interleaves the bits
  x0 y0 x1 y1 x2 y2 (thin micro tiling, `gpu/shadps4/video_core/host_shaders/tiling.comp`, `ComputePixelIndexWithinMicroTile`).

```python
def morton8(x, y):
    return sum((((x >> b) & 1) << (2 * b)) | (((y >> b) & 1) << (2 * b + 1)) for b in range(3))

def tile_1d(linear, w, h, esize):          # w, h: padded size in elements
    out = bytearray(len(linear))
    for y in range(h):
        for x in range(w):
            dst = (((y // 8) * (w // 8) + x // 8) * 64 + morton8(x % 8, y % 8)) * esize
            out[dst:dst + esize] = linear[(y * w + x) * esize:(y * w + x + 1) * esize]
    return bytes(out)
```

Checks: the linear decode of `PS4_Cross` and of the atlas is scrambled, the 1D-thin decode is clean, and
`tile_1d(detile_1d(p)) == p` for both. A test atlas with the 16 cells overwritten (`retile_test.py`) changes bytes
0..262 143 of the payload only, and its `.uexp` keeps the original size (4 194 628). Same size and format means the
`.uasset` header (export size, bulk offsets, name table with `PF_*`) stays untouched, and the GPU tile mode the game
picks at load is the same as for the original. **Do not change size or format** (that would need header edits).

### 2.2 Options

| | What | How | Effort | Risk |
|---|---|---|---|---|
| (a) Static, one set per launch | Xbox (or keyboard) art replaces the PS cells in the override pak | New `scripts/ue4_glyphs.py`: read the original `.uexp` with `read_file` (`ue4_override.py:103`), overwrite the payload (atlas: first 256 KiB; small textures: whole payload; DXT5 needs a BC3 encoder), add the files to `files` in `apply` (`ue4_override.py:523-551`, as `INFINITE_AMMO` does at `:537`). Launcher setting "Button prompts: PlayStation / Xbox / Keyboard", default = `pad_style`. | Code ~0.5-1 day; art separate (section 3) | Low: same mechanism as the shipped `.uexp` edits; a wrong payload shows as garbled icons, not a crash |
| (a+) Static, follows remaps | The cell for each PS4 button shows the physical button or key mapped to it | At launch, compose the atlas cells from a per-button glyph library using `controls` / `pad_style` (the generator runs at every start anyway: the pak is rewritten each launch). Keyboard: key caps drawn with the key's name (`Space` → SPACE, `Left Shift` → SHIFT, `Return` → ENTER); sticks are fixed WASD / arrow keys (`runtime_pad.c:298-301`), touchpad Tab. Gamepad: with `GOW3_PAD_MAP` `cross=b`, the Cross cell shows B. L2/R2 stay on the triggers. | +1 day | Low. Long key names must fit a 64x64 cell: the brushes are square (32x32 `ImageSize` in both tables), so wide caps need abbreviations |
| (b) Runtime, last-used device | Switch PS/Xbox ↔ keyboard while playing | `runtime_pad.c` tracks the last device (key down vs pad button / stick past a dead zone, with hysteresis). The texture cache records the guest address of images whose bytes match a known glyph payload (any style) when first uploaded (`RefreshImage`, `texture_cache.cpp:1617`). On a switch: re-hash the guest bytes (skip if they no longer match a known variant: memory reused), `memcpy` the other variant's tiled payload into guest memory (identical to the file payload, since the pak holds the GPU layout) and call `InvalidateMemory(addr, size)` (`texture_cache.cpp:677`) so the next draw re-uploads. Variant payloads written by the launcher next to the pak. | 2-4 days | Medium: texture-cache locking and the port's own upload shortcuts (texture-set memo by region stamps, parked images) must all see the write; the 1024x1024 atlas must not be split across aliases. No game patch, no Blueprint edit |
| (b') Runtime via game data | Swap the DataTable rows or add a third key map | Blueprint/DataTable edits | Large | High; not needed, (b) does the same at the texture level |

No eboot switch exists to hook for (b) **[V]**: the game has nothing that selects glyphs by device or platform.

Mouse: the runtime has no mouse input for the game (`runtime_pad.c`; `gpu/shim/window.cpp:191` only notes the time of
mouse motion, and the port's overlay takes mouse events), so "keyboard/mouse" prompts are keyboard prompts today.

Not covered by the atlas (optional, later): the Options > Button Layout page picture (`PS4_Controller`, 1024x754;
an Xbox-style controller drawing whose lead lines end where the PS4 picture's do, since the labels are UMG text at
fixed places), and the option label "Hide HUD with L3 Button" (a `Game.locres` text edit, per language).

## 3. Art

The glyphs must be original art drawn for the port: no Microsoft or Sony images from elsewhere, no Xbox logo
(the guide button never appears). Letters A/B/X/Y, LB/RB/LT/RT, "View" and "Menu" symbols drawn in the game's style
are generic. The launcher already draws its own PS/Xbox icons in code (`launcher/button_icons.py`, colours
`FACE`), a reference for colours and shapes but not for the steel look.

| Asset | Canvas | Format to write | Style to match |
|---|---|---|---|
| Atlas cells (16) | 64x64 each, row y 0..63 | BGRA8, straight alpha, sRGB, one mip | Steel discs / plates as in `atlas_row.png`. Xbox: face discs with coloured letters (A green, B red, X blue, Y yellow), RB/LB plates, RT/LT shields, Menu (≡) capsule for Options, View (two squares) for Touchpad (OpenInventory; SDL `back` maps to Touchpad, `runtime_pad.c:147`), LS/RS discs, d-pad cells can stay. Keyboard: steel key caps with the key name; WASD and arrow clusters for the stick cells |
| `L1_Active`, `R1_Active` | 64x64 | BGRA8 | Steel plate with serif lettering (LB / RB or keys) |
| `Options_Active` | 88x137 (pad to 88x144) | BGRA8 | Steel capsule (Menu / Enter) |
| `PS4_Cross`, `PS4_Triangle`, `PS4_R1` (bone) | 100x100 (pad to 128x128) | DXT5 (BC3) | Parchment / bone disc, coloured letter |
| `PS4_Controller` (optional) | 1024x754 | BGRA8 | Dark glossy controller, green lead lines |

At 2160p UMG scales the 32x32 brushes by 2, so 64x64 cells show about 1:1 at 4K. **[I]** Fonts: the plates' serif
looks like the game's `1529 Champ Fleury Pro` (`UI/Fonts/*.ufont` are plain TrueType/OpenType files, Pillow opens
them). Rendering key names with the user's own copy at launch distributes nothing, but the doc's rule is original
art: an OFL serif shipped with the port is the safe default.

Cooking without the UE4 editor (section 2.1): draw RGBA → pad to the multiple of 8 → swizzle R/B to BGRA (or encode
BC3 for the bone textures; Pillow 10.4 here cannot encode BC3, so use `texconv` / Compressonator (MIT) or a small
encoder) → `tile_1d` → write over the payload. **Yes, existing cooked textures can be patched in place** with new
pixel data of the same format and size; no mips to generate (all have one).

## 4. Step plan (smallest first; every launch needs the user's go)

| Step | Change | Test |
|---|---|---|
| G1 | Glyph patch helper in `scripts/` (payload locate, `tile_1d`, `.uexp` patch) + an opt-in test switch that puts `test_atlas.uexp`-style numbered squares into the 16 atlas cells | Offline: decode the written pak's atlas back to PNG and compare. **Launch 1:** HUD prompts (e.g. the Triangle interact prompt, the R1 swap icon) show numbered squares; health bar and other HUD parts of the atlas unchanged; no crash |
| G2 | Same for one DXT5 bone texture (`PS4_Cross`, test pattern via a BC3 encoder) | Offline round trip. **Launch 2:** the tutorial prompt / inventory Cross shows the test pattern, not garbage |
| G3 | Xbox art for the 16 cells + `L1_Active` / `R1_Active` / `Options_Active` + 3 bone textures; launcher "Button prompts" setting (default = `pad_style`) | **Launch 3:** main menu, pause menu, inventory (L1/R1 tabs), bestiary, an interact prompt, a tutorial prompt, Options > Button Layout (Classic and Modern) show Xbox glyphs |
| G4 | Remap-aware cells (a+): pad remaps for Xbox, key caps for Keyboard | **Launch 4:** remap Cross to B in the launcher; the Jump/confirm prompt shows B. Keyboard style with the defaults: Space, Shift, E, Q, 1, 3, R, F, Enter, Tab, WASD, arrows |
| G5 | Optional: Xbox controller picture for the Button Layout page; "L3" label text | Launch 5: the page lines point at the right buttons |
| G6 | Optional runtime switch (b): device tracking in `runtime_pad.c`, address capture + payload swap in the texture cache, behind a setting | Launch 6: on the HUD, press a key then a pad button; prompts switch within a frame or two; log shows each switch; test across a level load (textures reloaded at new addresses) and in the inventory |

## Open questions

- Does any prompt not go through the atlas or the six small textures (for example the book's `<INVENTORY>` text, the
  loading-screen tips)? A screenshot pass after G1 answers it.
- The bone prompts' on-screen size (their brush `ImageSize` was not read).
- For (b): whether the port's texture-set memo and parked-image paths re-upload an image after `InvalidateMemory`
  from the runtime thread, and whether the atlas is ever freed and reloaded (level loads).
- Which layout to show for keyboard on the Button Layout page (it is a controller picture).
- Art source: hand-drawn by the user / a contributor, or generated in code (reproducible, always original).

## Decisions (the user, 2026-11-03)

- **Art source:** the free pack https://juliocacko.itch.io/free-input-prompts (assets CC0 1.0, "free to use in any
  project, commercial or non-commercial, no attribution required"; credit given anyway). 128x128 icons, Xbox-style,
  other gamepads and four keyboard/mouse styles (Default, Light, Alt, retro); its PlayStation set was removed by the
  author after a trademark dispute, which does not matter here: the game's own PS4 prompts stay.
- **Static choice at launch (a) is the plan.** Live switching by last-used device (b) is a nice-to-have, lower
  priority: the user usually overrides an automatic setting anyway.
- Open: how the pack's flat style sits next to the game's metal HUD (use as is, or the pack's symbols on the game's
  steel button base); which keyboard/mouse style.
