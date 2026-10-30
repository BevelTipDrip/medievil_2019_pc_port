"""MediEvil's built-in patch file: version selection and 01.02 addresses."""
import unittest

from paths import ROOT
from patches import compile_patches, game_profile, selected_patches

XML = ROOT / 'patches/MediEvil.xml'
NAME = 'Native 4K framebuffer pool (512 MB)'
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

    def test_01_02_patch_is_the_default(self):
        self.assertEqual(selected_patches(XML, '01.02', ''), [NAME])
        self.assertEqual(selected_patches(XML, '01.00', ''), [NAME])


if __name__ == '__main__':
    unittest.main()
