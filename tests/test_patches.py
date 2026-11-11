from paths import ROOT
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from patches import (compile_patches, external_patches, external_selection, compile_external,
                     game_profile, selected_patches, patch_requirements)

MEDIEVIL = ROOT / 'patches/MediEvil.xml'
MEDIEVIL_IDS = {'CUSA11227', 'CUSA12982'}
SEGMENTS = [(0, 0x6000000)]
POOL = 'Native 4K framebuffer pool (512 MB)'
BOUNDS = 'GPU particle bounds'

# The selection rules MediEvil.xml has no example of (shadPS4 notes asking for a VBlank rate,
# resolution patches that exclude each other and replace a texture fix), in a small file of the
# same format (God of War III's names: the port's first game).
RULES = """<?xml version="1.0"?>
<Patch>
  <TitleID><ID>CUSA01623</ID></TitleID>
  <Metadata Title="t" Name="Bug Fix - Texture Corruption Fix" AppVer="01.02" isEnabled="true"
            Note="Requires setting DMEM to 1024 MB."><PatchList/></Metadata>
  <Metadata Title="t" Name="Skip Any Video With X Button" AppVer="01.02" isEnabled="true"><PatchList/></Metadata>
  <Metadata Title="t" Name="Resolution Patch - 720p" AppVer="01.02"><PatchList/></Metadata>
  <Metadata Title="t" Name="Resolution Patch - 4K" AppVer="01.02"
            Note="Requires setting DMEM to 6144 MB."><PatchList/></Metadata>
  <Metadata Title="t" Name="Frame Rate Patch - 120 FPS" AppVer="01.02"
            Note="Requires setting VBlank to 120 FPS."><PatchList/></Metadata>
</Patch>"""


def rules_file(folder):
    path = Path(folder) / 'rules.xml'
    path.write_text(RULES)
    return path


EXTERNAL = """<?xml version="1.0"?>
<Patch>
  <TitleID><ID>CUSA01623</ID></TitleID>
  <Metadata Title="God of War III" Name="On" Author="x" PatchVer="1.0" AppVer="01.02" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00401000" Value="9090"/></PatchList>
  </Metadata>
  <Metadata Title="God of War III" Name="Off" Author="x" PatchVer="1.0" AppVer="01.02" AppElf="eboot.bin">
    <PatchList><Line Type="bytes32" Address="0x00402000" Value="0x12345678"/></PatchList>
  </Metadata>
  <Metadata Title="God of War III" Name="Mask" Author="x" PatchVer="1.0" AppVer="01.02" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="mask" Value="90 ?? 90" Offset="0"/></PatchList>
  </Metadata>
  <Metadata Title="God of War III" Name="Old" Author="x" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00403000" Value="90"/></PatchList>
  </Metadata>
</Patch>"""


class ExternalPatchTests(unittest.TestCase):
    def test_selection_and_unsupported_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'extra.xml').write_text(EXTERNAL)
            (folder / 'other.xml').write_text(EXTERNAL.replace('CUSA01623', 'CUSA99999'))
            (folder / 'broken.xml').write_text('<Patch>')
            found = external_patches(folder, '01.02', MEDIEVIL, {'CUSA01623'})
            self.assertEqual([key for key, _, _ in found],
                             ['extra.xml/On', 'extra.xml/Off', 'extra.xml/Mask'])
            # The file's isEnabled; the mask patch is skipped as a whole.
            writes = compile_external(external_selection(found, None), SEGMENTS)
            self.assertEqual(writes, [(0x1000, bytes.fromhex('9090'))])
            config = folder / 'patches.json'
            config.write_text('{"enabled": ["extra.xml/Off"], "disabled": ["extra.xml/On"]}')
            writes = compile_external(external_selection(found, config), SEGMENTS)
            self.assertEqual(writes, [(0x2000, (0x12345678).to_bytes(4, 'little'))])

    def test_built_in_file_is_not_external(self):
        self.assertEqual(external_patches(MEDIEVIL.parent, '01.02', MEDIEVIL, MEDIEVIL_IDS), [])


class GameProfileTests(unittest.TestCase):
    def test_profiles_by_title_id(self):
        self.assertEqual(game_profile('CUSA11227')[1:3], (MEDIEVIL, '01.00'))  # the first entry
        self.assertEqual(game_profile('CUSA12982', '01.02')[1:3], (MEDIEVIL, '01.02'))
        self.assertIsNone(game_profile('CUSA99999'))
        self.assertIsNone(game_profile(None))

    def test_defaults_and_extras(self):
        self.assertEqual(selected_patches(MEDIEVIL, '01.02', ''), [POOL, BOUNDS])
        self.assertEqual(selected_patches(MEDIEVIL, '01.02', 'Boot display size 2560x1440; ' + BOUNDS),
                         [POOL, BOUNDS, 'Boot display size 2560x1440'])
        self.assertEqual(selected_patches(MEDIEVIL, '01.01', ''), [])
        # The launcher's selection is exact: unchecked defaults stay off.
        self.assertEqual(selected_patches(MEDIEVIL, '01.02', BOUNDS, only=True), [BOUNDS])
        self.assertEqual(selected_patches(MEDIEVIL, '01.02', '', only=True), [])

    def test_resolution_patch_replaces_texture_fix(self):
        with tempfile.TemporaryDirectory() as folder:
            rules = rules_file(folder)
            self.assertEqual(selected_patches(rules, '01.02', ''),
                             ['Bug Fix - Texture Corruption Fix', 'Skip Any Video With X Button'])
            self.assertEqual(selected_patches(rules, '01.02', 'Resolution Patch - 4K'),
                             ['Skip Any Video With X Button', 'Resolution Patch - 4K'])
            with self.assertRaises(ValueError):
                selected_patches(rules, '01.02', 'Resolution Patch - 4K;Resolution Patch - 720p')

    def test_eboot_build_check(self):
        from patches import eboot_matches
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'eboot.bin').write_bytes(b'another build')
            self.assertFalse(eboot_matches(folder, game_profile('CUSA11227', '01.02')))
            self.assertTrue(eboot_matches(folder, None))  # no profile: nothing to check

    def test_every_patch_compiles(self):
        for version in ('01.00', '01.02'):
            names = [m.get('Name') + (' 1920x1080' if m.get('Name') == 'Boot display size' else '')
                     for m in ET.parse(MEDIEVIL).getroot().iter('Metadata') if m.get('AppVer') == version]
            self.assertTrue(names)
            self.assertTrue(compile_patches(MEDIEVIL, names, version, SEGMENTS), version)

    def test_requirements_from_patch_notes(self):
        # Notes ask for extra direct memory (shadPS4 "DMEM", on top of the retail pool) and a VBlank rate.
        self.assertEqual(patch_requirements(MEDIEVIL, [POOL, BOUNDS], '01.02'), (5056 + 512, None))
        self.assertEqual(patch_requirements(MEDIEVIL, [], '01.02'), (None, None))
        with tempfile.TemporaryDirectory() as folder:
            rules = rules_file(folder)
            self.assertEqual(patch_requirements(rules, ['Resolution Patch - 4K', 'Bug Fix - Texture Corruption Fix'],
                                                '01.02'), (5056 + 6144, None))  # the larger one
            self.assertEqual(patch_requirements(rules, ['Frame Rate Patch - 120 FPS'], '01.02'), (None, 120))

    def test_sized_boot_patch(self):
        # "Boot display size WxH" fills the entry's Type="size" lines with W and H (int32 LE).
        medievil = ROOT / 'patches' / 'MediEvil.xml'
        writes = compile_patches(medievil, ['Boot display size 2560x1440'], '01.00', [(0, 1 << 40)])
        self.assertEqual([(offset + 0x400000, data.hex()) for offset, data in writes],
                         [(0x40be758, '000a0000a0050000'), (0x40be7b8, '000a0000a0050000')])
        with self.assertRaises(ValueError):
            compile_patches(medievil, ['Boot display size'], '01.00', [(0, 1 << 40)])
        with self.assertRaises(ValueError):
            compile_patches(medievil, ['Native 4K framebuffer pool (512 MB) 1x1'], '01.00', [(0, 1 << 40)])


if __name__ == '__main__':
    unittest.main()
