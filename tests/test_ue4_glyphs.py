"""Button prompt glyphs: PS4 tiling round trip and the atlas cell patch (synthetic texture)."""
import struct
import unittest

import paths  # noqa: F401
from game_ui_assets import decode, detile, texture, tile
import ue4_glyphs


def fake_uexp(width, height, payload):
    """The bytes of a cooked B8G8R8A8 Texture2D that texture() reads: sizes, format name, bulk
    element count / size / offset, payload, then a tail."""
    name = b'PF_B8G8R8A8\0'
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

    def test_playstation_and_unknown_styles_write_nothing(self):
        self.assertEqual(ue4_glyphs.files(lambda name: b'', 'playstation'), {})
        self.assertEqual(ue4_glyphs.files(lambda name: b'', 'nonsense'), {})


if __name__ == '__main__':
    unittest.main()
