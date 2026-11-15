"""Run logs: every run's log kept in <saves folder>/logs/runs (count and size limits), a header that
identifies the run, and the older runs in a bug report."""
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import bug_report  # noqa: E402
import medievil_launcher as launcher  # noqa: E402


def write(path, text, when=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8')
    if when is not None:
        os.utime(path, (when, when))


class RunLogTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.folder = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_prune_keeps_the_newest_by_count_and_size(self):
        runs = self.folder / 'runs'
        for i in range(6):
            write(runs / f'run-2026111{i}-120000.log', 'x' * 100)
        write(runs / 'notes.txt', 'not a run log')
        launcher.prune_run_logs(runs, kept=4, max_bytes=10_000)
        self.assertEqual(sorted(p.name for p in runs.glob('run-*.log')),
                         [f'run-2026111{i}-120000.log' for i in range(2, 6)])
        launcher.prune_run_logs(runs, kept=100, max_bytes=250)
        self.assertEqual(sorted(p.name for p in runs.glob('run-*.log')),
                         ['run-20261114-120000.log', 'run-20261115-120000.log'])
        self.assertTrue((runs / 'notes.txt').exists())

    def test_archive_names_by_time_and_version_without_overwriting(self):
        log = self.folder / 'last_run.log'
        when = time.mktime((2026, 11, 15, 6, 30, 5, 0, 0, -1))
        write(log, 'Run log: 2026-11-15 06:00:00 (local), MediEvil launcher 2.4.0\nlines\n', when)
        runs = self.folder / 'logs' / 'runs'
        first = launcher.archive_run_log(log, runs)
        second = launcher.archive_run_log(log, runs)
        self.assertEqual(first.name, 'run-20261115-063005-v2.4.0.log')
        self.assertEqual(second.name, 'run-20261115-063005-v2.4.0-2.log')
        write(log, 'a log from before the header\n', when)
        self.assertEqual(launcher.archive_run_log(log, runs).name, 'run-20261115-063005.log')

    def test_start_run_log_moves_archives_and_writes_the_header(self):
        write(self.folder / 'last_run.log', 'the run before\n')
        with launcher.start_run_log(self.folder, ['Run log: header', 'Build: test']) as log:
            log.write('game output\n')
        self.assertEqual((self.folder / 'last_run.1.log').read_text(encoding='utf-8'), 'the run before\n')
        archived = list(launcher.run_logs_folder(self.folder).glob('run-*.log'))
        self.assertEqual(len(archived), 1)
        self.assertEqual(archived[0].read_text(encoding='utf-8'), 'the run before\n')
        self.assertEqual((self.folder / 'last_run.log').read_text(encoding='utf-8').splitlines(),
                         ['Run log: header', 'Build: test', 'game output'])

    def test_header_and_footer(self):
        lines = launcher.run_log_header({'game_dir': str(self.folder)})
        self.assertTrue(lines[0].startswith('Run log: '))
        self.assertIn(f'MediEvil launcher {launcher.VERSION}', lines[0])
        self.assertTrue(lines[1].startswith('Build: '))
        self.assertTrue(lines[2].startswith('System: '))
        self.assertEqual(lines[3], f'Game: no eboot.bin in {self.folder}')
        footer = launcher.run_log_footer(time.time() - 125)
        self.assertTrue(footer.startswith('Run ended: '))
        self.assertTrue(footer.endswith('after 2 min 5 s'))

    def test_bug_report_carries_three_older_runs(self):
        user = self.folder / 'user'
        write(user / 'last_run.log', 'now\n')
        write(user / 'last_run.1.log', 'before\n')
        for day in range(10, 16):
            write(user / 'logs' / 'runs' / f'run-202611{day}-120000.log', f'day {day}\n')
        names = [name for name, _path in bug_report.report_files(user, self.folder / 'none.ini',
                                                                  self.folder / 'none.json', self.folder)]
        self.assertEqual(names, ['last_run.log', 'last_run.1.log', 'runs/run-20261114-120000.log',
                                 'runs/run-20261113-120000.log', 'runs/run-20261112-120000.log'])


if __name__ == '__main__':
    unittest.main()
