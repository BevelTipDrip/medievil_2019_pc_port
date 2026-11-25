import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import medievil_launcher as launcher  # noqa: E402
import graphics_options  # noqa: E402  (scripts/, on the path through the launcher)


class HdrOutputTest(unittest.TestCase):
    """HDR output (v2.5.0): medievil.ini hdr_output, both runtime switches from it, the old settings.json value
    copied once."""

    def test_both_switches_from_the_ini_key(self):
        self.assertEqual(graphics_options.hdr_environment({'hdr_output': '1'}),
                         {'GOW3_HDR': '1', 'GOW3_HDR_GAME': '1'})
        self.assertEqual(graphics_options.hdr_environment({'hdr_output': '0'}), {})
        self.assertEqual(graphics_options.hdr_environment({}), {})  # off by default

    def test_launcher_key_and_no_app_setting(self):
        self.assertIn('hdr_output', launcher.INI_FLAGS)
        self.assertEqual(launcher.INI_DEFAULTS['hdr_output'], '0')
        self.assertNotIn('hdr', launcher.APP_DEFAULTS)
        added = {k: v for k, v in launcher.game_environment({**launcher.APP_DEFAULTS, 'hdr': True}).items()
                 if k not in os.environ}
        self.assertNotIn('GOW3_HDR', added)  # run.py derives it from medievil.ini

    def test_old_setting_copied_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            config, ini = Path(tmp) / 'settings.json', Path(tmp) / 'medievil.ini'
            config.write_text(json.dumps({'hdr': True, 'game_dir': 'x'}), encoding='utf-8')
            ini.write_text('vsync=1\n', encoding='utf-8')
            self.assertEqual(launcher.migrate_hdr_setting(config, ini), {'hdr_output': '1'})
            self.assertEqual(graphics_options.read_ini(ini).get('hdr_output'), '1')
            self.assertNotIn('hdr', json.loads(config.read_text(encoding='utf-8')))
            self.assertEqual(launcher.migrate_hdr_setting(config, ini), {})  # once

    def test_old_setting_off_or_ini_set_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            config, ini = Path(tmp) / 'settings.json', Path(tmp) / 'medievil.ini'
            config.write_text(json.dumps({'hdr': False}), encoding='utf-8')
            ini.write_text('vsync=1\n', encoding='utf-8')
            self.assertEqual(launcher.migrate_hdr_setting(config, ini), {})
            self.assertNotIn('hdr_output', graphics_options.read_ini(ini))
            config.write_text(json.dumps({'hdr': True}), encoding='utf-8')
            ini.write_text('hdr_output=0\n', encoding='utf-8')  # the user's ini choice wins
            self.assertEqual(launcher.migrate_hdr_setting(config, ini), {})
            self.assertEqual(graphics_options.read_ini(ini).get('hdr_output'), '0')


if __name__ == '__main__':
    unittest.main()
