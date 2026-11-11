import datetime
import os
import tempfile
import unittest
from pathlib import Path

import paths  # noqa: F401  (puts scripts/ on sys.path)
import save_backup


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8')


class SaveBackupTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.user = Path(self.temp.name)
        self.save = self.user / 'savedata' / '1' / 'CUSA01623' / 'AUTOSAVE' / 'data.bin'
        self.clock = datetime.datetime(2026, 10, 9, 18, 0, 0)

    def tearDown(self):
        self.temp.cleanup()

    def tick(self):
        self.clock += datetime.timedelta(minutes=1)
        return self.clock

    def test_copies_savedata(self):
        write(self.save, 'v1')
        made = save_backup.backup(self.user, now=self.tick())
        self.assertEqual(made.name, '20261009-180100')
        self.assertEqual((made / self.save.relative_to(self.user / 'savedata')).read_text(), 'v1')

    def test_no_backup_without_saves(self):
        self.assertIsNone(save_backup.backup(self.user, now=self.tick()))
        (self.user / 'savedata').mkdir()
        self.assertIsNone(save_backup.backup(self.user, now=self.tick()))
        self.assertEqual(save_backup.list_backups(self.user), [])

    def test_same_second_gets_unique_name(self):
        write(self.save, 'v1')
        first = save_backup.backup(self.user, now=self.clock)
        second = save_backup.backup(self.user, now=self.clock)
        self.assertNotEqual(first, second)
        self.assertEqual(len(save_backup.list_backups(self.user)), 2)

    def test_keeps_every_backup(self):
        write(self.save, 'v')
        # A hand-made backup (letter name) once crowded every automatic one out of a count by name.
        (self.user / 'savedata_backups' / 'manual-before-test').mkdir(parents=True)
        made = [save_backup.backup(self.user, now=self.tick()) for _ in range(12)]
        kept = save_backup.list_backups(self.user)
        self.assertEqual(len(kept), 13)
        self.assertTrue(all(m.exists() for m in made))
        self.assertIn(made[-1].name, kept)

    def test_keeps_file_times(self):
        # The game's save slot order follows file times (study-save-slots.md).
        write(self.save, 'v')
        os.utime(self.save, (1_700_000_000, 1_700_000_000))
        made = save_backup.backup(self.user, now=self.tick())
        copy = made / self.save.relative_to(self.user / 'savedata')
        self.assertEqual(int(copy.stat().st_mtime), 1_700_000_000)
        save_backup.restore(self.user, made.name, now=self.tick())
        self.assertEqual(int(self.save.stat().st_mtime), 1_700_000_000)

    def test_restore_backs_up_current_first(self):
        write(self.save, 'old')
        old = save_backup.backup(self.user, now=self.tick())
        write(self.save, 'new')
        write(self.save.parent / 'extra.bin', 'x')
        safety = save_backup.restore(self.user, old.name, now=self.tick())
        self.assertEqual(self.save.read_text(), 'old')
        self.assertFalse((self.save.parent / 'extra.bin').exists())
        self.assertEqual((safety / self.save.relative_to(self.user / 'savedata')).read_text(), 'new')

    def test_restore_oldest_keeps_all(self):
        write(self.save, 'oldest')
        oldest = save_backup.backup(self.user, now=self.tick())
        write(self.save, 'later')
        for _ in range(11):
            save_backup.backup(self.user, now=self.tick())
        save_backup.restore(self.user, oldest.name, now=self.tick())
        self.assertEqual(self.save.read_text(), 'oldest')
        self.assertEqual(len(save_backup.list_backups(self.user)), 13)  # 12 + the restore's own backup

    def test_restore_rejects_unknown_or_unsafe_names(self):
        write(self.save, 'v')
        for name in ('missing', '..', '../savedata', ''):
            with self.assertRaises(ValueError):
                save_backup.restore(self.user, name, now=self.tick())
        self.assertEqual(self.save.read_text(), 'v')


if __name__ == '__main__':
    unittest.main()
