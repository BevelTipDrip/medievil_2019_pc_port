"""MediEvil's built-in patch file: version selection and 01.02 addresses."""
import unittest

from paths import ROOT
from patches import compile_patches, game_profile, selected_patches

XML = ROOT / 'patches/MediEvil.xml'
NAME = 'Native 4K framebuffer pool (512 MB)'
BOUNDS = 'GPU particle bounds'
SEGMENTS = [(0, 0x6000000)]


class MedievilPatchTests(unittest.TestCase):
    def test_profile_per_version(self):
        self.assertEqual(game_profile('CUSA12982', '01.00')[1:3], (XML, '01.00'))
        self.assertEqual(game_profile('CUSA12982', '01.02')[1:3], (XML, '01.02'))
        self.assertEqual(game_profile('CUSA11227', '01.02')[1:3], (XML, '01.02'))
        self.assertIsNone(game_profile('CUSA12982', '01.01'))
        self.assertIsNone(game_profile('CUSA99999', '01.02'))

    def test_01_02_writes(self):
        writes = compile_patches(XML, [NAME], '01.02', SEGMENTS)
        self.assertEqual(writes, [
            (0x8f77c1, bytes.fromhex('00200000')),
            (0x8f77cc, bytes.fromhex('00200000')),
            (0x8f78ca, bytes.fromhex('00000020')),
            (0x8f78f2, bytes.fromhex('00000020')),
            (0x8f7911, bytes.fromhex('00000020')),
            (0x8f7921, bytes.fromhex('00000020')),
            (0x3c8b758, bytes.fromhex('0000002000000000')),
            (0x8e3ba3, bytes.fromhex('1d')),
            (0x8e3ba7, bytes.fromhex('00020000')),
            (0x8e4842, bytes.fromhex('1d')),
            (0x8e4845, bytes.fromhex('00020000')),
        ])

    def test_01_02_boot_patches(self):
        # The boot resolution patches at their 01.02 addresses (research/docs/plan-0102.md).
        self.assertEqual(compile_patches(XML, ['4K display metrics at boot'], '01.02', SEGMENTS),
                         [(0xb6bcb2, bytes.fromhex('9090')), (0xb70316, bytes.fromhex('9090'))])
        size = bytes.fromhex('000a000070080000')  # 2560, 2160 (int32 LE)
        self.assertEqual(compile_patches(XML, ['Boot display size 2560x2160'], '01.02', SEGMENTS),
                         [(0x3cc1db8, size), (0x3cc1e18, size)])

    def test_01_02_max_fps_commands_lifted(self):
        # 01.02's game instance caps with "t.MaxFPS 30/60"; the edit keeps the asset's length.
        from ue4_override import without_max_fps
        data = b'\x17\x1ft.MaxFPS 30\x00\x2a\x16\x1ft.MaxFPS 60\x00\x2a'
        self.assertEqual(without_max_fps(data), b'\x17\x1ft.MaxFPS 00\x00\x2a\x16\x1ft.MaxFPS 00\x00\x2a')
        self.assertEqual(without_max_fps(b'no commands'), b'no commands')

    def test_gpu_particle_bounds(self):
        # UpdateBoundingBox's -1 / 1 loads read -2000 / 2000 (rel32 displacements, plan-0102.md T-0102-5).
        self.assertEqual(compile_patches(XML, [BOUNDS], '01.00', SEGMENTS),
                         [(0x23828e1, bytes.fromhex('db669a01')), (0x23828e9, bytes.fromhex('d36f9a01'))])
        self.assertEqual(compile_patches(XML, [BOUNDS], '01.02', SEGMENTS),
                         [(0x23856b1, bytes.fromhex('0b6d9a01')), (0x23856b9, bytes.fromhex('03769a01'))])

    def test_default_patches(self):
        self.assertEqual(selected_patches(XML, '01.02', ''), [NAME, BOUNDS])
        self.assertEqual(selected_patches(XML, '01.00', ''), [NAME, BOUNDS])


if __name__ == '__main__':
    unittest.main()
