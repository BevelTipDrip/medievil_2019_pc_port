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


def outlined(icon, width):
    """icon on a white silhouette grown by `width` pixels with a round edge, so a dark pressed button stays inside
    the mouse: the shape blurred and cut again (a square grow left corners on the rounded mouse)."""
    pad = 2 * width
    alpha = Image.new('L', (icon.width + 2 * pad, icon.height + 2 * pad), 0)
    alpha.paste(icon.getchannel('A'), (pad, pad))
    alpha = alpha.filter(ImageFilter.GaussianBlur(width * 0.55)).point(lambda a: 255 if a > 24 else 0)
    alpha = alpha.filter(ImageFilter.GaussianBlur(0.8))  # an anti-aliased edge
    out = Image.new('RGBA', alpha.size, WHITE)
    out.putalpha(alpha)
    out.alpha_composite(icon, (pad, pad))
    return out


def side_buttons(mouse, forward, width):
    """Two side buttons drawn onto the outlined mouse's left edge (forward in front): the pressed one dark with a
    white rim, the other white with a dark rim. After the outline, so they get no white halo of their own."""
    left, top, _right, bottom = mouse.getchannel('A').getbbox()
    height = bottom - top
    draw = ImageDraw.Draw(mouse)
    for front, y in ((True, top + int(height * 0.40)), (False, top + int(height * 0.61))):
        pressed = front == forward
        draw.rounded_rectangle((left, y, left + 5 * width, y + int(height * 0.18)), radius=max(2, width),
                               fill=DARK if pressed else WHITE, outline=WHITE if pressed else DARK,
                               width=max(2, width // 2))
    return mouse


def compose(mouse, arrows=(), side=None):
    """The mouse as large as the key's face takes it; for the wheel, arrows 'up' / 'down' beside it (to the right);
    side: None, or True / False for the forward / back side button."""
    key, radius = blank_key()
    mouse = mouse.crop(mouse.getchannel('A').getbbox())
    width = max(3, mouse.height // 40)
    mouse = outlined(mouse, width)
    mouse = mouse.crop(mouse.getchannel('A').getbbox())
    if side is not None:  # room on the left for the buttons
        wider = Image.new('RGBA', (mouse.width + 3 * width, mouse.height), (0, 0, 0, 0))
        wider.alpha_composite(mouse, (3 * width, 0))
        mouse = side_buttons(wider, side, width)
    # the largest size whose corners stay inside the face (the mouse's corners are round: a little past is fine)
    room = 2 * radius * 0.96
    beside = 0.5 if arrows else 0.0  # a column for the arrows, as a share of the mouse's height
    scale = room / ((mouse.width / mouse.height + beside) ** 2 + 1) ** 0.5 / mouse.height
    mouse = mouse.resize((round(mouse.width * scale), round(mouse.height * scale)), Image.LANCZOS)
    cx, cy = key.width // 2, key.height // 2
    column = round(mouse.height * beside)
    x = cx - (mouse.width + column) // 2
    key.alpha_composite(mouse, (x, cy - mouse.height // 2))
    draw, a = ImageDraw.Draw(key), int(column * 0.42)
    ax = x + mouse.width + column // 2
    for arrow in arrows:
        both = len(arrows) == 2
        base = cy - (a // 3 if both else -a // 2) if arrow == 'up' else cy + (a // 3 if both else -a // 2)
        tip = base - a if arrow == 'up' else base + a
        if not both:
            base, tip = cy + a // 2 if arrow == 'up' else cy - a // 2, cy - a // 2 if arrow == 'up' else cy + a // 2
        draw.polygon([(ax, tip), (ax - a, base), (ax + a, base)], fill=WHITE)
    return key


def main(pack):
    white = Path(pack) / 'White'

    def mouse(name):
        return Image.open(white / f'T_Mouse_{name}_Key_White.png').convert('RGBA')
    icons = {'Mouse_Left': (mouse('Left'), ()), 'Mouse_Right': (mouse('Right'), ()),
             'Mouse_Middle': (mouse('Middle'), ()), 'Mouse_Wheel_Up': (mouse('Middle'), ('up',)),
             'Mouse_Wheel_Down': (mouse('Middle'), ('down',)), 'Mouse_Wheel': (mouse('Middle'), ('up', 'down')),
             'Mouse_Back': (mouse('Simple'), (), False), 'Mouse_Forward': (mouse('Simple'), (), True)}
    for name, (icon, arrows, *side) in icons.items():
        compose(icon, arrows, side[0] if side else None).save(KEYS / f'{name}.png')
        print(KEYS / f'{name}.png')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
