#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Button prompt glyphs for the override pak (medievil.ini button_prompts).

The game draws every button prompt from PS4 textures (research/docs/study-glyphs.md): the 16
64x64 cells in the top row of InventoryButtonAtlas (the SP_PS4_* sprites), three small textures
and three 100x100 DXT5 "bone" prompts. Their .uexp holds the pixels already in the PS4 GPU layout
(game_ui_assets: single mip, 1D thin tiling), so new pixels of the same size and format are
written over the payload and the edited .uexp goes into the override pak; the .uasset header stays
the game's.

Styles: "playstation" (the game's own, no files), "xbox" and "keyboard" (third_party/input-prompts,
the CC0 Free Input Prompts pack's Default Xbox and Vintage keyboard sets; each prompt shows the pad
button or key the launcher's Controls page maps to that PS4 button, GOW3_PAD_MAP / GOW3_KEY_MAP),
"test" (numbered squares in the 16 atlas cells, the three small textures and the three bone
prompts, for checking that the patch reaches the screen: study-glyphs.md G1, G2).
"""
import io
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_ui_assets import FORMATS, detile, texture, tile  # noqa: E402

ATLAS = 'OVERBITE/Content/UI/Sprites/HUD/InventoryButtonAtlas.uexp'
CELL = 64
# The atlas's top row, left to right (study-glyphs.md 1.2), by the PS4 button each cell shows
# (the launcher's CONTROLS names); the rest of the atlas is HUD art.
ATLAS_CELLS = ('circle', 'cross', 'triangle', 'square', 'up', 'down', 'left', 'right',
               'r1', 'l1', 'r2', 'l2', 'options', 'touchpad', 'stick_left', 'stick_right')
# Small textures of their own (study-glyphs.md 1.3): the Button Layout page's L1, R1 and Start
# (the SP_PS4_*_New sprites, also the inventory and bestiary tabs), whole textures:
# (test style number, button).
SMALL = {'OVERBITE/Content/UI/Sprites/HUD/L1_Active.uexp': (16, 'l1'),
         'OVERBITE/Content/UI/Sprites/HUD/R1_Active.uexp': (17, 'r1'),
         'OVERBITE/Content/UI/Sprites/HUD/Options_Active.uexp': (18, 'options')}
# The menus that show L1_Active / R1_Active (the SP_PS4_*1_New sprites, the whole 64x64) draw them
# in a 1.45:1 box (brush 32x22; the inventory's slot 64x44), except the bestiary's Enemies and
# Bosses pages (no stored size: 32x32), so no one picture fit all (T-G4). For the xbox and keyboard
# styles the boxes are made square, same-size float edits: brush ImageSize Y 22 -> 32, the
# inventory's slot height 44 -> 64 with its top -39 -> -49 (centred). Every widget is stored twice
# (the class's WidgetTree and the WidgetArchetype). PlayStation keeps the game's boxes: its wide
# L1 / R1 plate was drawn for them.
SIZE_32x22 = (bytes.fromhex('000000420000b041'), bytes.fromhex('0000004200000042'))
INVENTORY_SLOT = (bytes.fromhex('00001cc22a04000000000000ae020000000000000400000000000000000000804'
                                '2a000000000000000ae0200000000000004000000000000000000003042'),
                  bytes.fromhex('000044c22a04000000000000ae020000000000000400000000000000000000804'
                                '2a000000000000000ae0200000000000004000000000000000000008042'))
SQUARE_TABS = {'OVERBITE/Content/UI/BestiaryMenu.uexp': [(*SIZE_32x22, 4)],  # Friendlies page
               'OVERBITE/Content/UI/OptionsMenu/WidgetButtonLayout.uexp': [(*SIZE_32x22, 4)],
               'OVERBITE/Content/UI/OptionsMenu/WidgetOptionsmanager.uexp': [(*SIZE_32x22, 2)],
               'OVERBITE/Content/UI/InventoryMenu.uexp': [(*INVENTORY_SLOT, 4)]}
# The pearl "bone" prompts (100x100 DXT5): inventory, Daring Dash (Classic: Triangle, Modern: R1),
# the tutorial prompt's default.
BONE = {'OVERBITE/Content/UI/Textures/ButtonPrompts/PS4_Cross.uexp': (19, 'cross'),
        'OVERBITE/Content/UI/Textures/ButtonPrompts/PS4_Triangle.uexp': (20, 'triangle'),
        'OVERBITE/Content/UI/Textures/ButtonPrompts/PS4_R1.uexp': (21, 'r1')}
STYLES = ('playstation', 'test', 'xbox', 'keyboard')
ART = Path(__file__).resolve().parent.parent / 'third_party' / 'input-prompts'
MARGIN = 2  # pixels left clear around an icon, as around the game's own

# The runtime's defaults (src/runtime_pad.c; the launcher's CONTROLS): the key and the SDL gamepad
# button of each PS4 button. L2 / R2 on a gamepad are the triggers, not remapped.
DEFAULT_KEYS = {'cross': 'Space', 'circle': 'Left Shift', 'square': 'E', 'triangle': 'Q', 'l1': '1', 'r1': '3',
                'l2': 'R', 'r2': 'F', 'l3': 'Z', 'r3': 'C', 'options': 'Return', 'touchpad': 'Tab',
                'up': 'I', 'down': 'K', 'left': 'J', 'right': 'L'}
DEFAULT_PAD = {'cross': 'a', 'circle': 'b', 'square': 'x', 'triangle': 'y', 'l1': 'leftshoulder',
               'r1': 'rightshoulder', 'l3': 'leftstick', 'r3': 'rightstick', 'options': 'start', 'touchpad': 'back',
               'up': 'dpup', 'down': 'dpdown', 'left': 'dpleft', 'right': 'dpright'}
# Xbox icons (xbox/<name>.png) by SDL gamepad button, and for what is not remapped.
PAD_ICONS = {'a': 'A', 'b': 'B', 'x': 'X', 'y': 'Y', 'leftshoulder': 'LB', 'rightshoulder': 'RB',
             'leftstick': 'LS', 'rightstick': 'RS', 'start': 'Menu', 'back': 'View', 'dpup': 'DpadUp',
             'dpdown': 'DpadDown', 'dpleft': 'DpadLeft', 'dpright': 'DpadRight'}
PAD_FIXED = {'l2': 'LT', 'r2': 'RT', 'stick_left': 'LS', 'stick_right': 'RS'}
# Keyboard icons (keyboard/<name>.png) by SDL key name, lower case; letters, digits and F1-F12 are
# their own name. The sticks are always WASD and the arrow keys (runtime_pad.c): key clusters.
KEY_ICONS = {'space': 'Space', 'left shift': 'Shift', 'right shift': 'Shift', 'return': 'Enter', 'enter': 'Enter',
             'keypad enter': 'Enter', 'tab': 'Tab', 'escape': 'Esc', 'backspace': 'BackSpace', 'left ctrl': 'Crtl',
             'right ctrl': 'Crtl', 'left alt': 'Alt', 'right alt': 'Alt', 'up': 'Up', 'down': 'Down', 'left': 'Left',
             'right': 'Right', 'delete': 'Del', 'insert': 'Ins', 'home': 'Home', 'end': 'End', 'pageup': 'PageUp',
             'pagedown': 'PageDown', 'capslock': 'CapsLock', 'numlockclear': 'NumLock', 'printscreen': 'PrtScrn',
             '-': 'Minus', '/': 'Slash', ';': 'Semicolon', "'": 'Quotation', '`': 'Tilde', '[': 'Brackets_L',
             ']': 'Brackets_R', 'keypad *': 'Asterisk', 'keypad +': 'Plus', 'keypad -': 'Minus', 'keypad /': 'Slash'}
KEY_FIXED = {'stick_left': ('W', 'A', 'S', 'D'), 'stick_right': ('Up', 'Left', 'Down', 'Right')}
# Mouse icons (keyboard/Mouse_*.png, scripts/make_mouse_prompts.py) by GOW3_MOUSE_MAP name (runtime_pad.c).
MOUSE_ICONS = {'left': 'Mouse_Left', 'right': 'Mouse_Right', 'middle': 'Mouse_Middle', 'x1': 'Mouse_Back',
               'x2': 'Mouse_Forward', 'wheel_up': 'Mouse_Wheel_Up', 'wheel_down': 'Mouse_Wheel_Down', 'wheel': 'Mouse_Wheel'}
VISIBLE = 16  # alpha above which an icon pixel counts as visible (the pack's keys fade to 1-2 at the edge)


def strip(uexp, rows=None):
    """The top `rows` pixel rows of a B8G8R8A8 texture (None: all, with the padding) as an RGBA
    picture, with the texture's (payload offset, pitch). rows: a multiple of 8, so the strip is
    whole tile rows."""
    from PIL import Image
    info = texture(uexp)
    if info and rows is None:
        rows = info[5]
    if not info or info[0] != 'PF_B8G8R8A8' or rows % 8 or rows > info[5]:
        raise ValueError('not a B8G8R8A8 texture of this layout')
    _format, _width, _height, at, pitch, _rows = info
    element = FORMATS['PF_B8G8R8A8'][1]
    linear = detile(uexp[at:at + pitch * rows * element], pitch, rows, element)
    return Image.frombytes('RGBA', (pitch, rows), linear, 'raw', 'BGRA'), at, pitch


def with_strip(uexp, picture, at, pitch):
    """uexp with the top rows of its payload replaced by picture (as made by strip)."""
    element = FORMATS['PF_B8G8R8A8'][1]
    raw = tile(picture.tobytes('raw', 'BGRA'), pitch, picture.height, element)
    return uexp[:at] + raw + uexp[at + len(raw):]


def whole(uexp):
    """A B8G8R8A8 or DXT5 texture as an RGBA picture of its padded surface, with
    (payload offset, pitch, rows, pixel format)."""
    from PIL import Image
    info = texture(uexp)
    if not info or info[0] not in ('PF_B8G8R8A8', 'PF_DXT5'):
        raise ValueError('not a B8G8R8A8 or DXT5 texture of this layout')
    pixel_format, _width, _height, at, pitch, rows = info
    side, element, bcn = FORMATS[pixel_format]
    linear = detile(uexp[at:at + pitch * rows * element], pitch, rows, element)
    if bcn:
        picture = Image.frombytes('RGBA', (pitch * side, rows * side), linear, 'bcn', bcn)
    else:
        picture = Image.frombytes('RGBA', (pitch, rows), linear, 'raw', 'BGRA')
    return picture, (at, pitch, rows, pixel_format)


def bc3(picture):
    """DXT5 blocks of an RGBA picture, row by row (Pillow 11 or later encodes DXT5)."""
    out = io.BytesIO()
    try:
        picture.save(out, 'DDS', pixel_format='DXT5')
    except (KeyError, OSError, ValueError, TypeError) as error:
        raise ValueError(f'this Pillow cannot encode DXT5 ({error})') from error
    size = (picture.width // 4) * (picture.height // 4) * 16
    data = out.getvalue()
    if data[84:88] != b'DXT5' or len(data) < size:
        raise ValueError('this Pillow cannot encode DXT5')
    return data[-size:]


def with_whole(uexp, picture, layout):
    """uexp with its payload made from picture (as made by whole)."""
    at, pitch, rows, pixel_format = layout
    element = FORMATS[pixel_format][1]
    linear = bc3(picture) if pixel_format == 'PF_DXT5' else picture.tobytes('raw', 'BGRA')
    raw = tile(linear, pitch, rows, element)
    return uexp[:at] + raw + uexp[at + len(raw):]


def test_square(picture, box, number):
    """A numbered yellow square filling box (left, top, width, height)."""
    from PIL import ImageDraw, ImageFont
    try:
        font = ImageFont.load_default(size=28)
    except TypeError:  # Pillow before 10.1: the small bitmap font
        font = ImageFont.load_default()
    x, y, w, h = box
    draw = ImageDraw.Draw(picture)
    draw.rectangle((x + 2, y + 2, x + w - 3, y + h - 3), fill=(255, 200, 0, 255), outline=(0, 0, 0, 255), width=3)
    draw.text((x + w // 2, y + h // 2), f'{number:02d}', fill=(0, 0, 0, 255), font=font, anchor='mm')


def test_cells(picture):
    """Numbered yellow squares over the 16 cells: 00 = Circle ... 15 = StickRight."""
    for i in range(len(ATLAS_CELLS)):
        test_square(picture, (i * CELL, 0, CELL, CELL), i)
    return picture


def button_map(text):
    """{button: name} of a GOW3_KEY_MAP / GOW3_PAD_MAP value ('cross=Q,circle=Left Shift')."""
    result = {}
    for item in (text or '').split(','):
        button, sep, name = item.partition('=')
        if sep and button.strip() and name.strip():
            result[button.strip().lower()] = name.strip()
    return result


def key_icon(key):
    """The keyboard icon of an SDL key name, or None."""
    name = key.strip().lower()
    if len(name) == 1 and name.isalnum():
        return name.upper()
    if name[:1] == 'f' and name[1:].isdigit() and 1 <= int(name[1:]) <= 12:
        return name.upper()
    if name.startswith('keypad ') and name[7:].isdigit():  # the pack has no keypad: the digit's key
        return name[7:]
    return KEY_ICONS.get(name)


def icons(style, key_map='', pad_map='', log=lambda line: print(line, flush=True), mouse_map='', prefer_mouse=False):
    """{button: icon name, or a tuple of names drawn as a key cluster} for xbox / keyboard: the pad
    button or key each PS4 button is mapped to. A key without a picture keeps the PS4 one (log).
    Keyboard: a button with a mouse button (GOW3_MOUSE_MAP) shows the mouse when prefer_mouse is set or
    its key has no picture (none: unbound in the launcher)."""
    if style == 'xbox':
        chosen = {**DEFAULT_PAD, **{b: n.lower() for b, n in button_map(pad_map).items()}}
        result = {b: PAD_ICONS.get(n) for b, n in chosen.items()}
        result.update(PAD_FIXED)
    elif style == 'keyboard':
        chosen = {**DEFAULT_KEYS, **button_map(key_map)}
        result = {b: key_icon(n) for b, n in chosen.items()}
        for button, name in button_map(mouse_map).items():
            icon = MOUSE_ICONS.get(name.lower())
            if icon and button in chosen and (prefer_mouse or not result.get(button)):
                result[button] = icon
        result.update(KEY_FIXED)
    else:
        return {}
    for button, icon in result.items():
        if icon is None and button in ATLAS_CELLS:
            log(f"Button prompts: no {style} picture for {chosen.get(button)!r} ({button}); it shows the PS4 one")
    return {b: i for b, i in result.items() if i}


def visible(icon):
    """icon cropped to its visible part (alpha above VISIBLE)."""
    box = icon.getchannel('A').point(lambda a: 255 if a > VISIBLE else 0).getbbox()
    return icon.crop(box or (0, 0, icon.width, icon.height))


def load_icon(style, name):
    """A style's icon, cropped to its visible part."""
    from PIL import Image
    path = ART / style / f'{name}.png'
    if not path.is_file():
        raise ValueError(f'{path} is missing')
    with Image.open(path) as image:
        icon = image.convert('RGBA')
    return visible(icon)


def fit(icon, w, h):
    """icon scaled to fit w x h (premultiplied while scaling, so the transparent pixels' colour
    does not bleed into the edge)."""
    from PIL import Image
    scale = min(w / icon.width, h / icon.height)
    size = (max(1, round(icon.width * scale)), max(1, round(icon.height * scale)))
    return icon.convert('RGBa').resize(size, Image.LANCZOS).convert('RGBA')


def place(picture, box, name, style):
    """The style's icon `name` scaled to fit box (left, top, width, height) less MARGIN, centred
    on a cleared box. A tuple of four names is a key cluster: the first above the other three."""
    x, y, w, h = box
    picture.paste((0, 0, 0, 0), (x, y, x + w, y + h))
    if isinstance(name, tuple):
        side = min((w - 2 * MARGIN) // 3, (h - 2 * MARGIN) // 2)
        left, top = x + (w - 3 * side) // 2, y + (h - 2 * side) // 2
        spots = ((left + side, top), (left, top + side), (left + side, top + side), (left + 2 * side, top + side))
        for key, (kx, ky) in zip(name, spots):
            icon = fit(load_icon(style, key), side, side)
            picture.alpha_composite(icon, (kx + (side - icon.width) // 2, ky + (side - icon.height) // 2))
        return
    icon = fit(load_icon(style, name), w - 2 * MARGIN, h - 2 * MARGIN)
    picture.alpha_composite(icon, (x + (w - icon.width) // 2, y + (h - icon.height) // 2))


def atlas(uexp, style, chosen=None):
    """The atlas .uexp with its 16 prompt cells in the given style; chosen: icons() of the style
    (a cell without an icon keeps its PS4 art)."""
    picture, at, pitch = strip(uexp, CELL)
    if style == 'test':
        test_cells(picture)
    else:
        for i, button in enumerate(ATLAS_CELLS):
            if (chosen or {}).get(button):
                place(picture, (i * CELL, 0, CELL, CELL), chosen[button], style)
    return with_strip(uexp, picture, at, pitch)


def small(uexp, style, number, icon=None):
    """A small or bone prompt texture's .uexp in the given style (the whole texture; the padding
    keeps its pixels). number: the test style's; icon: the other styles' icon (none: unchanged)."""
    _format, width, height, *_rest = texture(uexp) or (None, 0, 0)
    picture, layout = whole(uexp)
    if style == 'test':
        test_square(picture, (0, 0, width, height), number)
    elif icon:
        place(picture, (0, 0, width, height), icon, style)
    return with_whole(uexp, picture, layout)


# The port's Load list and settings menu prompts (gow3gpu_choice_style): file name and PS4 button.
OVERLAY = {'Cross': 'cross', 'Circle': 'circle', 'Square': 'square', 'L1': 'l1', 'R1': 'r1'}


def overlay_prompts(style, cache, key_map='', pad_map='', mouse_map='', prefer_mouse=False):
    """Writes <cache>/prompts/style/Cross|Circle|Square|L1|R1.png for the port's Load list and
    settings menu in the given style; the game's own PS4 prompts (game UI cache) for playstation,
    test, an unknown style or a key without a picture."""
    from PIL import Image
    out = Path(cache) / 'prompts' / 'style'
    out.mkdir(parents=True, exist_ok=True)
    chosen = icons(style, key_map, pad_map, log=lambda _line: None, mouse_map=mouse_map, prefer_mouse=prefer_mouse)
    for file, button in OVERLAY.items():
        if chosen.get(button):
            picture = Image.new('RGBA', (100, 100), (0, 0, 0, 0))  # the size of the game's prompts
            place(picture, (0, 0, 100, 100), chosen[button], style)
        else:
            with Image.open(Path(cache) / 'prompts' / f'PS4_{file}.png') as image:
                picture = image.convert('RGBA')
        picture.save(out / f'{file}.png')


def files(read, style, key_map='', pad_map='', mouse_map='', prefer_mouse=False):
    """{pak path: edited .uexp} for a style; read(name) gives the game's file; key_map / pad_map /
    mouse_map: the launcher's remaps (GOW3_KEY_MAP / GOW3_PAD_MAP / GOW3_MOUSE_MAP; icons)."""
    if style not in STYLES or style == 'playstation':
        return {}
    chosen = icons(style, key_map, pad_map, mouse_map=mouse_map, prefer_mouse=prefer_mouse)
    result = {ATLAS: atlas(read(ATLAS), style, chosen)}
    for name, (number, button) in SMALL.items():
        if style == 'test' or chosen.get(button):
            result[name] = small(read(name), style, number, chosen.get(button))
    if style in ('xbox', 'keyboard'):
        from ue4_override import apply_edits
        for name, edits in SQUARE_TABS.items():
            try:
                result[name] = apply_edits(name, read(name), edits)
            except (ValueError, FileNotFoundError) as error:  # this game build: the 1.45:1 box stays
                print(f'Button prompts: the L1 / R1 tabs keep their shape here ({error})', flush=True)
    try:
        for name, (number, button) in BONE.items():
            if style == 'test' or chosen.get(button):
                result[name] = small(read(name), style, number, chosen.get(button))
    except ValueError as error:  # the rest of the style still applies
        for name in BONE:
            result.pop(name, None)
        print(f'Button prompts: the inventory / tutorial prompts stay PlayStation ({error})', flush=True)
    return result


if __name__ == '__main__':
    # ue4_glyphs.py <game dir> <style> <out dir>: the edited files and the atlas row as a PNG.
    from ue4_override import read_file
    game, style, out = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
    for name, data in files(lambda n: read_file(game, n), style).items():
        (out / Path(name).name).write_bytes(data)
        if name == ATLAS:
            strip(data, CELL)[0].crop((0, 0, CELL * len(ATLAS_CELLS), CELL)).save(out / 'atlas_row.png')
        elif texture(data):  # the menus' size edits are not pictures
            whole(data)[0].save(out / (Path(name).stem + '.png'))
    print(f'{style}: written to {out}')
