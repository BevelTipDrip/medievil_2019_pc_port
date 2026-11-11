"""Button prompt glyphs: PS4 tiling round trip and the atlas cell patch (synthetic texture)."""
import struct
import unittest

from PIL import Image

import paths  # noqa: F401
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

    def test_xbox_art_is_complete(self):
        names = set(ue4_glyphs.XBOX_CELLS) | {icon for _n, icon in ue4_glyphs.SMALL.values()} | \
            {icon for _n, icon in ue4_glyphs.BONE.values()}
        self.assertEqual(len(ue4_glyphs.XBOX_CELLS), len(ue4_glyphs.ATLAS_CELLS))
        for name in names:
            self.assertTrue((ue4_glyphs.ART / 'xbox' / f'{name}.png').is_file(), name)

    def test_xbox_cells_replace_the_old_art(self):
        payload = bytes([0xff]) * (1024 * 72 * 4)  # opaque white everywhere
        uexp = fake_uexp(1024, 72, payload)
        picture = decode(ue4_glyphs.atlas(uexp, 'xbox'))
        self.assertEqual(picture.getpixel((0, 0))[3], 0)  # the cell's corner is cleared
        self.assertEqual(picture.getpixel((32, 32))[3], 255)  # the B disc
        self.assertEqual(picture.getpixel((32, 68)), (255, 255, 255, 255))  # below the cells: unchanged
        edited = ue4_glyphs.small(fake_uexp(64, 64, bytes([0xff]) * (64 * 64 * 4)), 'xbox', 16, 'LB')
        self.assertEqual(decode(edited).getpixel((0, 0))[3], 0)

    def test_playstation_and_unknown_styles_write_nothing(self):
        self.assertEqual(ue4_glyphs.files(lambda name: b'', 'playstation'), {})
        self.assertEqual(ue4_glyphs.files(lambda name: b'', 'nonsense'), {})


if __name__ == '__main__':
    unittest.main()
