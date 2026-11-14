"""Button prompt glyphs: PS4 tiling round trip and the atlas cell patch (synthetic texture)."""
import struct
import unittest

try:
    from PIL import Image
except ImportError:  # the launcher's Python and both packages have Pillow; a bare system Python may not
    raise unittest.SkipTest('needs Pillow')

import paths  # noqa: F401
import sys

sys.path.insert(0, str(paths.ROOT / 'launcher'))
from game_ui_assets import decode, detile, texture, tile
import ue4_glyphs


def fake_uexp(width, height, payload, name=b'PF_B8G8R8A8\0'):
    """The bytes of a cooked Texture2D that texture() reads: sizes, format name, bulk element
    count / size / offset, payload, then a tail."""
    return (bytes(20) + struct.pack('<iiii', width, height, 1, len(name)) + name + bytes(12)
            + struct.pack('<iiq', len(payload), len(payload), 0) + payload + b'tail')


class GlyphTests(unittest.TestCase):
    def test_tile_inverts_detile(self):
        linear = bytes(range(256)) * (16 * 8 * 4 // 256)
        tiled = tile(linear, 16, 8, 4)
        self.assertNotEqual(tiled, linear)
        self.assertEqual(detile(tiled, 16, 8, 4), linear)
        # Element (1, 0) is the second of the first micro tile; (0, 1) the third (x0 y0 x1 ...).
        self.assertEqual(tiled[4:8], linear[4:8])
        self.assertEqual(tiled[8:12], linear[16 * 4:16 * 4 + 4])

    def test_atlas_cells_only(self):
        payload = bytes((i * 7) & 0xff for i in range(1024 * 72 * 4))
        uexp = fake_uexp(1024, 72, payload)
        info = texture(uexp)
        self.assertEqual(info[:3], ('PF_B8G8R8A8', 1024, 72))
        edited = ue4_glyphs.files(lambda name: uexp, 'test')[ue4_glyphs.ATLAS]
        self.assertEqual(len(edited), len(uexp))
        at, cells = info[3], 1024 * 64 * 4
        self.assertEqual(edited[:at], uexp[:at])
        self.assertNotEqual(edited[at:at + cells], uexp[at:at + cells])
        self.assertEqual(edited[at + cells:], uexp[at + cells:])
        picture = decode(edited)
        self.assertEqual(picture.getpixel((8, 8)), (255, 200, 0, 255))  # cell 00, yellow
        self.assertEqual(picture.crop((0, 64, 1024, 72)).tobytes(), decode(uexp).crop((0, 64, 1024, 72)).tobytes())

    def test_unchanged_strip_round_trip(self):
        payload = bytes((i * 13) & 0xff for i in range(1024 * 64 * 4))
        uexp = fake_uexp(1024, 64, payload)
        picture, at, pitch = ue4_glyphs.strip(uexp, 64)
        self.assertEqual(ue4_glyphs.with_strip(uexp, picture, at, pitch), uexp)

    def test_small_texture_keeps_padding(self):
        # 88x137 pads to 88x144 elements: the square covers the texture, the padding rows stay.
        payload = bytes((i * 5) & 0xff for i in range(88 * 144 * 4))
        uexp = fake_uexp(88, 137, payload)
        edited = ue4_glyphs.small(uexp, 'test', 18)
        self.assertEqual(len(edited), len(uexp))
        picture, at, pitch = ue4_glyphs.strip(edited)
        before = ue4_glyphs.strip(uexp)[0]
        self.assertEqual(picture.getpixel((8, 8)), (255, 200, 0, 255))
        self.assertEqual(picture.crop((0, 137, 88, 144)).tobytes(), before.crop((0, 137, 88, 144)).tobytes())

    def test_dxt5_texture(self):
        try:
            ue4_glyphs.bc3(Image.new('RGBA', (4, 4)))
        except ValueError:
            self.skipTest('this Pillow cannot encode DXT5')
        # 100x100 DXT5: 25x25 blocks padded to 32x32, 16 bytes each.
        uexp = fake_uexp(100, 100, bytes(32 * 32 * 16), b'PF_DXT5\0')
        edited = ue4_glyphs.small(uexp, 'test', 19)
        self.assertEqual(len(edited), len(uexp))
        self.assertTrue(edited.endswith(b'tail'))
        picture = decode(edited)
        self.assertEqual(picture.size, (100, 100))
        red, green, blue, alpha = picture.getpixel((12, 12))
        self.assertTrue(red > 240 and 180 < green < 220 and blue < 16 and alpha == 255, picture.getpixel((12, 12)))

    def test_art_is_complete(self):
        for style in ('xbox', 'keyboard'):
            chosen = ue4_glyphs.icons(style, log=self.fail)  # the defaults: every button has a picture
            for button in ue4_glyphs.ATLAS_CELLS:
                self.assertIn(button, chosen, (style, button))
            for icon in chosen.values():
                for name in icon if isinstance(icon, tuple) else (icon,):
                    self.assertTrue((ue4_glyphs.ART / style / f'{name}.png').is_file(), (style, name))
        names = set(ue4_glyphs.PAD_ICONS.values()) | set(ue4_glyphs.PAD_FIXED.values())
        names |= {ue4_glyphs.key_icon(n) for n in ue4_glyphs.KEY_ICONS}
        for name in names:
            style = 'xbox' if name in set(ue4_glyphs.PAD_ICONS.values()) | set(ue4_glyphs.PAD_FIXED.values()) else 'keyboard'
            self.assertTrue((ue4_glyphs.ART / style / f'{name}.png').is_file(), name)

    def test_defaults_match_the_launcher(self):
        import medievil_launcher  # launcher/, on the path above
        for button, key, pad in medievil_launcher.CONTROLS:
            self.assertEqual(ue4_glyphs.DEFAULT_KEYS[button], key)
            self.assertEqual(ue4_glyphs.DEFAULT_PAD.get(button), pad)

    def test_remaps_pick_the_icon(self):
        xbox = ue4_glyphs.icons('xbox', pad_map='cross=b,circle=a')
        self.assertEqual((xbox['cross'], xbox['circle'], xbox['l2']), ('B', 'A', 'LT'))
        keys = ue4_glyphs.icons('keyboard', key_map='cross=Q,triangle=left ctrl,options=F5')
        self.assertEqual((keys['cross'], keys['triangle'], keys['options'], keys['circle']), ('Q', 'Crtl', 'F5', 'Shift'))
        self.assertEqual(keys['stick_left'], ('W', 'A', 'S', 'D'))
        self.assertEqual(ue4_glyphs.key_icon('Keypad 7'), '7')
        self.assertEqual(ue4_glyphs.key_icon('Right Shift'), 'Shift')
        lines = []
        keys = ue4_glyphs.icons('keyboard', key_map='square=Keypad .', log=lines.append)
        self.assertNotIn('square', keys)  # no picture: the cell keeps the PS4 art
        self.assertEqual(len(lines), 1)

    def test_mouse_binds_pick_the_mouse_icon(self):
        mouse = 'circle=right,triangle=wheel,l1=x1,r1=x2,square=left'
        # Preferred: every button with a mouse button shows the mouse.
        keys = ue4_glyphs.icons('keyboard', mouse_map=mouse, prefer_mouse=True)
        self.assertEqual([keys[b] for b in ('circle', 'triangle', 'l1', 'r1', 'square', 'cross')],
                         ['Mouse_Right', 'Mouse_Wheel', 'Mouse_Back', 'Mouse_Forward', 'Mouse_Left', 'Space'])
        # Not preferred: the key, and the mouse only where the key is unbound (None) or has no picture.
        keys = ue4_glyphs.icons('keyboard', key_map='circle=None', mouse_map=mouse, log=lambda _l: None)
        self.assertEqual((keys['circle'], keys['triangle'], keys['square']), ('Mouse_Right', 'Q', 'E'))
        # The gamepad style and unknown names ignore the mouse.
        self.assertEqual(ue4_glyphs.icons('xbox', mouse_map=mouse, prefer_mouse=True)['circle'], 'B')
        self.assertEqual(ue4_glyphs.icons('keyboard', mouse_map='circle=thumb', prefer_mouse=True)['circle'], 'Shift')
        for name in ue4_glyphs.MOUSE_ICONS.values():
            self.assertTrue((ue4_glyphs.ART / 'keyboard' / f'{name}.png').is_file(), name)
        import medievil_launcher  # every mouse button the launcher offers has a picture
        self.assertEqual(medievil_launcher.MOUSE_BUTTON_NAMES, set(ue4_glyphs.MOUSE_ICONS))

    def test_icons_are_cropped_to_what_is_visible(self):
        # The pack's keys fade to alpha 1-2 out to the 128 px edge: that border is not drawn.
        self.assertEqual(ue4_glyphs.load_icon('keyboard', 'Q').size, (96, 96))
        self.assertEqual(ue4_glyphs.load_icon('keyboard', 'Space').size, (128, 64))

    def test_square_tab_edits_keep_sizes(self):
        for name, edits in ue4_glyphs.SQUARE_TABS.items():
            for old, new, _count in edits:
                self.assertEqual(len(old), len(new), name)
        # A menu whose sizes are found as often as in 01.02 is edited; any other count (another
        # game build) leaves it alone, and only the xbox / keyboard styles touch the menus.
        fake = {ue4_glyphs.ATLAS: fake_uexp(1024, 64, bytes(1024 * 64 * 4))}
        for name in {**ue4_glyphs.SMALL, **ue4_glyphs.BONE}:
            fake[name] = fake_uexp(64, 64, bytes(64 * 64 * 4))
        two = bytes(8) + ue4_glyphs.SIZE_32x22[0] + bytes(8) + ue4_glyphs.SIZE_32x22[0]
        for name in ue4_glyphs.SQUARE_TABS:
            fake[name] = two
        result = ue4_glyphs.files(fake.get, 'keyboard')
        options = 'OVERBITE/Content/UI/OptionsMenu/WidgetOptionsmanager.uexp'  # 2 in 01.02
        self.assertEqual(result[options], two.replace(*ue4_glyphs.SIZE_32x22))
        self.assertEqual([n for n in ue4_glyphs.SQUARE_TABS if n in result], [options])
        self.assertFalse(set(ue4_glyphs.SQUARE_TABS) & set(ue4_glyphs.files(fake.get, 'test')))

    def test_xbox_cells_replace_the_old_art(self):
        payload = bytes([0xff]) * (1024 * 72 * 4)  # opaque white everywhere
        uexp = fake_uexp(1024, 72, payload)
        picture = decode(ue4_glyphs.atlas(uexp, 'xbox', ue4_glyphs.icons('xbox')))
        self.assertEqual(picture.getpixel((0, 0))[3], 0)  # the cell's corner is cleared
        self.assertEqual(picture.getpixel((32, 32))[3], 255)  # the B disc
        self.assertEqual(picture.getpixel((32, 68)), (255, 255, 255, 255))  # below the cells: unchanged
        edited = ue4_glyphs.small(fake_uexp(64, 64, bytes([0xff]) * (64 * 64 * 4)), 'xbox', 16, 'LB')
        self.assertEqual(decode(edited).getpixel((0, 0))[3], 0)

    def test_overlay_prompts_follow_the_style(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as cache:
            (Path(cache) / 'prompts').mkdir()
            for button in ue4_glyphs.OVERLAY:  # the game's prompts, as the game UI cache holds them
                name = ue4_glyphs.GAME_PROMPT.get(button, button)
                Image.new('RGBA', (100, 100), (10, 20, 30, 255)).save(Path(cache) / 'prompts' / f'PS4_{name}.png')
            ue4_glyphs.overlay_prompts('playstation', cache)
            with Image.open(Path(cache) / 'prompts' / 'style' / 'Cross.png') as image:
                self.assertEqual(image.getpixel((50, 50)), (10, 20, 30, 255))
            self.assertTrue((Path(cache) / 'prompts' / 'style' / 'Touchpad.png').is_file())  # Touchpad (APPLY & RESTART)
            self.assertTrue((Path(cache) / 'prompts' / 'style' / 'Triangle.png').is_file())  # the Load list's DETAILS
            ue4_glyphs.overlay_prompts('xbox', cache)
            with Image.open(Path(cache) / 'prompts' / 'style' / 'Cross.png') as image:
                self.assertEqual(image.size, (100, 100))
                self.assertEqual(image.getpixel((0, 0))[3], 0)
                self.assertNotEqual(image.getpixel((50, 50)), (10, 20, 30, 255))

    def test_playstation_without_other_pictures_and_unknown_styles_write_nothing(self):
        # The touchpad bound to the touchpad itself: every PS4 button shows its own picture.
        self.assertEqual(ue4_glyphs.files(lambda name: b'', 'playstation', pad_map='touchpad=touchpad'), {})
        self.assertEqual(ue4_glyphs.files(lambda name: b'', 'nonsense'), {})

    def test_playstation_shows_the_bound_pad_button(self):
        # The prompt shows the physical button (the user, 2026-11-13): the touchpad is on Share / Create by default.
        self.assertEqual(ue4_glyphs.icons('playstation'), {'touchpad': 'PS4_Share'})
        self.assertEqual(ue4_glyphs.icons('playstation', pad_map='cross=b,circle=a,touchpad=touchpad'),
                         {'cross': 'PS4_Circle', 'circle': 'PS4_Cross'})
        # The game's atlas: only the touchpad cell changes, to the Share texture's picture.
        atlas = fake_uexp(1024, 72, bytes(1024 * 72 * 4))  # transparent
        share = fake_uexp(64, 64, bytes([0x40, 0x80, 0xc0, 0xff]) * (64 * 64))  # opaque, BGRA
        read = lambda name: share if name.endswith('PS4_Share.uexp') else atlas
        edited = decode(ue4_glyphs.files(read, 'playstation')[ue4_glyphs.ATLAS])
        cell = ue4_glyphs.ATLAS_CELLS.index('touchpad') * ue4_glyphs.CELL
        self.assertEqual(edited.getpixel((cell + 32, 32)), (0xc0, 0x80, 0x40, 0xff))
        self.assertEqual(edited.getpixel((32, 32))[3], 0)  # circle: untouched
        # The menu's prompts: Touchpad.png is the cache's PS4_Share.png.
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as cache:
            (Path(cache) / 'prompts').mkdir()
            for button in ue4_glyphs.OVERLAY:
                name = ue4_glyphs.GAME_PROMPT.get(button, button)
                Image.new('RGBA', (100, 100), (10, 20, 30, 255)).save(Path(cache) / 'prompts' / f'PS4_{name}.png')
            Image.new('RGBA', (100, 100), (90, 80, 70, 255)).save(Path(cache) / 'prompts' / 'PS4_Share.png')
            ue4_glyphs.overlay_prompts('playstation', cache)
            with Image.open(Path(cache) / 'prompts' / 'style' / 'Touchpad.png') as image:
                self.assertEqual(image.getpixel((50, 50)), (90, 80, 70, 255))
            with Image.open(Path(cache) / 'prompts' / 'style' / 'Cross.png') as image:
                self.assertEqual(image.getpixel((50, 50)), (10, 20, 30, 255))


if __name__ == '__main__':
    unittest.main()
