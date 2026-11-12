#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds the mouse button prompts (third_party/input-prompts/keyboard/Mouse_*.png) in the style of the keyboard
ones: a Vintage round key (its letter painted over) with the White set's mouse inside, the pressed button dark.
The pack has no side buttons or Vintage mouse in that style, so those are drawn here. Run once with the
Free Input Prompts pack (Julio Cacko, CC0) unpacked; the results are checked in.

    make_mouse_prompts.py <pack>/Source/Keyboard_Mouse"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

KEYS = Path(__file__).resolve().parent.parent / 'third_party' / 'input-prompts' / 'keyboard'
WHITE = (245, 245, 245, 255)
DARK = (30, 30, 30, 255)


def blank_key():
    """The Vintage Q key with its letter painted over in the face's colour; (key, face radius)."""
    key = Image.open(KEYS / 'Q.png').convert('RGBA')
    px = key.load()
    cx, cy = key.width // 2, key.height // 2
    face = px[cx - 30, cy + 22]  # between the letter and the ring
    radius = next(x - cx for x in range(cx + 25, key.width) if sum(px[x, cy][:3]) / 3 > 110)  # the ring starts
    ImageDraw.Draw(key).ellipse((cx - radius + 3, cy - radius + 3, cx + radius - 3, cy + radius - 3), fill=face)
    return key, radius


def outlined(icon, width=5):
    """icon on a white silhouette `width` pixels larger, so a dark pressed button stays inside the mouse."""
    alpha = icon.getchannel('A').filter(ImageFilter.MaxFilter(2 * width + 1))
    out = Image.new('RGBA', (icon.width + 4 * width, icon.height + 4 * width), (0, 0, 0, 0))
    grown = Image.new('RGBA', icon.size, WHITE)
    grown.putalpha(alpha)
    out.alpha_composite(grown, (2 * width, 2 * width))
    out.alpha_composite(icon, (2 * width, 2 * width))
    return out


def side_buttons(mouse, forward):
    """The plain mouse with two side buttons on its left edge (forward in front); the pressed one dark."""
    mouse = mouse.copy()
    left, top, _right, bottom = mouse.getchannel('A').getbbox()
    height = bottom - top
    draw = ImageDraw.Draw(mouse)
    for front, y in ((True, top + int(height * 0.48)), (False, top + int(height * 0.66))):
        draw.rounded_rectangle((left - 14, y, left + 6, y + int(height * 0.14)), radius=4,
                               fill=DARK if front == forward else WHITE, outline=DARK, width=3)
    return mouse


def compose(mouse, arrows=()):
    """The mouse in the middle of a blank key; for the wheel, arrows 'up' / 'down' above / below it."""
    key, radius = blank_key()
    mouse = outlined(mouse.crop(mouse.getchannel('A').getbbox()))
    size = int(2 * radius * (0.5 if arrows else 0.62))
    mouse.thumbnail((size, size), Image.LANCZOS)
    cx, cy = key.width // 2, key.height // 2
    key.alpha_composite(mouse, (cx - mouse.width // 2, cy - mouse.height // 2))
    draw, a = ImageDraw.Draw(key), int(radius * 0.16)
    for arrow in arrows:
        tip = cy - mouse.height // 2 - 5 if arrow == 'up' else cy + mouse.height // 2 + 5
        point = tip - a if arrow == 'up' else tip + a
        draw.polygon([(cx, point), (cx - a, tip), (cx + a, tip)], fill=WHITE)
    return key


def main(pack):
    white = Path(pack) / 'White'

    def mouse(name):
        return Image.open(white / f'T_Mouse_{name}_Key_White.png').convert('RGBA')
    icons = {'Mouse_Left': (mouse('Left'), ()), 'Mouse_Right': (mouse('Right'), ()),
             'Mouse_Middle': (mouse('Middle'), ()), 'Mouse_Wheel_Up': (mouse('Middle'), ('up',)),
             'Mouse_Wheel_Down': (mouse('Middle'), ('down',)), 'Mouse_Wheel': (mouse('Middle'), ('up', 'down')),
             'Mouse_Back': (side_buttons(mouse('Simple'), False), ()),
             'Mouse_Forward': (side_buttons(mouse('Simple'), True), ())}
    for name, (icon, arrows) in icons.items():
        compose(icon, arrows).save(KEYS / f'{name}.png')
        print(KEYS / f'{name}.png')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
