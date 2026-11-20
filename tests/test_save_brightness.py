import os
import struct
import tempfile
import unittest
from pathlib import Path

import paths  # noqa: F401  (puts scripts/ on sys.path)
import save_backup
import save_brightness


def gvas(brightness=None, extra=b''):
    """A save as MediEvil writes it around the Brightness property (GVAS tagged properties)."""
    body = b'GVAS' + b'\x00' * 40
    if brightness is not None:
        body += struct.pack('<i', 11) + b'Brightness\x00' + struct.pack('<i', 14) + b'FloatProperty\x00'
        body += struct.pack('<q', 4) + b'\x00' + struct.pack('<f', brightness)
    return body + extra + struct.pack('<i', 7) + b'GameId\x00' + b'\x00' * 16


class SaveBrightnessTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.user = Path(self.temp.name)
        self.root = self.user / 'savedata' / '1' / 'CUSA11227'

    def tearDown(self):
        self.temp.cleanup()

    def slot(self, n, data, mtime):
        path = self.root / f'@MediEvil{n:04d}' / 'ue4savegame.ps4.sav'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        os.utime(path, (mtime, mtime))
        return path

    def test_sets_neutral_keeps_times_and_backs_up(self):
        a = self.slot(1, gvas(0.42), 1_000_000)
        b = self.slot(2, gvas(), 2_000_000)
        c = self.slot(3, gvas(0.2978), 3_000_000)
        backup, changed, skipped = save_brightness.set_neutral(self.user)
        self.assertEqual((changed, skipped), (2, []))
        self.assertEqual([p for p, _ in save_brightness.slots(self.user)], [c, b, a])  # order by time kept
        self.assertEqual(os.stat(a).st_mtime, 1_000_000)
        values = dict(save_brightness.slots(self.user))
        self.assertAlmostEqual(values[a], save_brightness.NEUTRAL, places=6)
        self.assertIsNone(values[b])
        self.assertEqual(len(a.read_bytes()), len(gvas(0.42)))
        self.assertIn(backup.name, save_backup.list_backups(self.user))
        old = backup / '1' / 'CUSA11227' / '@MediEvil0001' / 'ue4savegame.ps4.sav'
        self.assertEqual(old.read_bytes(), gvas(0.42))

    def test_nothing_to_do_makes_no_backup(self):
        self.slot(1, gvas(save_brightness.NEUTRAL), 1_000_000)
        self.assertEqual(save_brightness.set_neutral(self.user), (None, 0, []))
        self.assertEqual(save_backup.list_backups(self.user), [])

    def test_unexpected_layout_is_left_alone(self):
        odd = gvas(0.42).replace(b'FloatProperty', b'FloatPropertX')
        path = self.slot(1, odd, 1_000_000)
        backup, changed, skipped = save_brightness.set_neutral(self.user)
        self.assertEqual((backup, changed), (None, 0))
        self.assertEqual([p for p, _ in skipped], [path])
        self.assertEqual(path.read_bytes(), odd)

    def test_display_gamma(self):
        self.assertAlmostEqual(save_brightness.display_gamma(save_brightness.NEUTRAL), 2.2001, places=4)


if __name__ == '__main__':
    unittest.main()
