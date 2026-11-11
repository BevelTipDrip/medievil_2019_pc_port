#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Button prompt glyphs for the override pak (medievil.ini button_prompts).

The game draws every button prompt from PS4 textures (research/docs/study-glyphs.md): the 16
64x64 cells in the top row of InventoryButtonAtlas (the SP_PS4_* sprites), three small textures
and three 100x100 DXT5 "bone" prompts. Their .uexp holds the pixels already in the PS4 GPU layout
(game_ui_assets: single mip, 1D thin tiling), so new pixels of the same size and format are
written over the payload and the edited .uexp goes into the override pak; the .uasset header stays
the game's.

Styles: "playstation" (the game's own, no files), "test" (numbered squares in the 16 atlas cells
and the three small textures, for checking that the patch reaches the screen: study-glyphs.md G1).
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_ui_assets import FORMATS, detile, texture, tile  # noqa: E402

ATLAS = 'OVERBITE/Content/UI/Sprites/HUD/InventoryButtonAtlas.uexp'
CELL = 64
# The atlas's top row, left to right (study-glyphs.md 1.2); the rest of the atlas is HUD art.
ATLAS_CELLS = ('Circle', 'Cross', 'Triangle', 'Square', 'DpadUp', 'DpadDown', 'DpadLeft', 'DpadRight',
               'R1', 'L1', 'R2', 'L2', 'Options', 'Touchpad', 'StickLeft', 'StickRight')
# Small textures of their own (study-glyphs.md 1.3): the Button Layout page's L1, R1 and Start
# (the SP_PS4_*_New sprites, also the inventory and bestiary tabs), whole textures; the number
# is the test style's.
SMALL = {'OVERBITE/Content/UI/Sprites/HUD/L1_Active.uexp': 16,
         'OVERBITE/Content/UI/Sprites/HUD/R1_Active.uexp': 17,
         'OVERBITE/Content/UI/Sprites/HUD/Options_Active.uexp': 18}
STYLES = ('playstation', 'test')


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


def atlas(uexp, style):
    """The atlas .uexp with its 16 prompt cells in the given style."""
    picture, at, pitch = strip(uexp, CELL)
    if style == 'test':
        test_cells(picture)
    return with_strip(uexp, picture, at, pitch)


def small(uexp, style, number):
    """A small prompt texture's .uexp in the given style (the whole texture; the padding stays)."""
    _format, width, height, *_rest = texture(uexp) or (None, 0, 0)
    picture, at, pitch = strip(uexp)
    if style == 'test':
        test_square(picture, (0, 0, width, height), number)
    return with_strip(uexp, picture, at, pitch)


def files(read, style):
    """{pak path: edited .uexp} for a style; read(name) gives the game's file."""
    if style not in STYLES or style == 'playstation':
        return {}
    result = {ATLAS: atlas(read(ATLAS), style)}
    for name, number in SMALL.items():
        result[name] = small(read(name), style, number)
    return result


if __name__ == '__main__':
    # ue4_glyphs.py <game dir> <style> <out dir>: the edited files and the atlas row as a PNG.
    from ue4_override import read_file
    game, style, out = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
    for name, data in files(lambda n: read_file(game, n), style).items():
        (out / Path(name).name).write_bytes(data)
        if name == ATLAS:
            strip(data, CELL)[0].crop((0, 0, CELL * len(ATLAS_CELLS), CELL)).save(out / 'atlas_row.png')
        else:
            strip(data)[0].save(out / (Path(name).stem + '.png'))
    print(f'{style}: written to {out}')
