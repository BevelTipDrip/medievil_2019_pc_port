"""HDR settings (research/patches/present/hdr-output.md): the boot UI level and the runtime's live HDR cvar rows."""
import re
import sys
import unittest

from paths import ROOT

sys.path.insert(0, str(ROOT / 'scripts'))
import ue4_override  # noqa: E402

EXTRA = (ROOT / 'src' / 'runtime_extra.c').read_text(encoding='utf-8')
SETTINGS = (ROOT / 'gpu' / 'shim' / 'gow3_settings.h').read_text(encoding='utf-8')


class HdrSettingsTest(unittest.TestCase):
    def test_ui_level_is_nits_over_100(self):
        self.assertEqual(ue4_override.hdr_ui_level('100'), '1')
        self.assertEqual(ue4_override.hdr_ui_level('50'), '0.5')
        self.assertEqual(ue4_override.hdr_ui_level('120'), '1.2')
        self.assertEqual(ue4_override.hdr_ui_level('10'), '0.5')    # clamped to HdrUiNitsMin
        self.assertEqual(ue4_override.hdr_ui_level('9000'), '4')    # HdrUiNitsMax
        self.assertEqual(ue4_override.hdr_ui_level('bad'), '1')

    def test_range_matches_the_menu(self):
        m = re.search(r'HdrUiNitsMin = (\d+), HdrUiNitsMax = (\d+), HdrUiNitsDefault = (\d+)', SETTINGS)
        self.assertEqual(m.groups(), ('50', '400', '100'))

    def test_live_rows_for_the_menu(self):
        for name in ('r.HDR.UI.Level', 'r.HDR.Display.OutputDevice'):
            line = next(l for l in EXTRA.splitlines() if l.strip().startswith('{"%s",' % name))
            rows = re.findall(r'\{(0x[0-9a-f]+),(0x[0-9a-f]+),\{([^}]*)\}\}', line)
            self.assertEqual(len(rows), 2, name)
            for _, _, code in rows:
                self.assertTrue(code.startswith('0x48,0x8d,0x35'), name)  # lea rsi, [rip+name]


if __name__ == '__main__':
    unittest.main()
