import sys
import tempfile
import unittest
import urllib.parse
import zipfile
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'research' / 'tools'))
import bug_report  # noqa: E402
import issue2card  # noqa: E402

HOME = r'C:\Users\Ty'
CTX = bug_report.redaction_context(env={'USERNAME': 'Ty', 'COMPUTERNAME': 'GALLOWMERE-PC'}, home=HOME,
                                   host='gallowmere-pc', player='SirDaniel')
LOG = '\n'.join([
    r'Settings: C:\Users\Ty\MediEvil\medievil_2019_pc_port\medievil.ini',
    'GPU: NVIDIA GeForce RTX 4090, driver 616.92.0.0',
    'Save: /home/deck/.local/share/medievil user SirDaniel on GALLOWMERE-PC, steam 76561198012345678',
    'Runtime: loading eboot.bin',
    *[f'Mouse look: last 5.0 s: {i} camera ticks' for i in range(400)],
    *[f'Stage B: frame {i} ok, nothing to see here at all, padding the log a little' for i in range(1500)],
    'GPU [Render] <Error> vk_rasterizer.cpp:10 device lost',
    'The game exited (code 3221225477).',
])


class RedactTest(unittest.TestCase):
    def test_names_and_paths_go_versions_stay(self):
        text = bug_report.redact(LOG, CTX)
        for secret in ('Ty\\', 'GALLOWMERE', 'gallowmere', 'SirDaniel', '76561198012345678', '/home/deck'):
            self.assertNotIn(secret, text)
        self.assertIn(r'~\MediEvil\medievil_2019_pc_port\medievil.ini', text)
        self.assertIn('616.92.0.0', text)
        self.assertIn('/home/<user>/', text)
        self.assertIn('<steamid>', text)

    def test_short_names_only_in_paths(self):
        # A two-letter user name would eat parts of words ("Type", "Tyrant"): replaced only as a path part.
        self.assertEqual(bug_report.redact('Type Ty Tyrant', CTX), 'Type Ty Tyrant')
        self.assertEqual(bug_report.redact(r'D:\Users\Ty\x', CTX), r'D:\Users\<user>\x')

    def test_email(self):
        self.assertEqual(bug_report.redact('mail me: dan.fortesque+bugs@gallowmere.co.uk', CTX), 'mail me: <email>')


class ExcerptAndUrlTest(unittest.TestCase):
    def test_excerpt_keeps_start_errors_and_end_drops_noise(self):
        lines = bug_report.log_excerpt(LOG)
        self.assertEqual(lines[0], LOG.splitlines()[0])
        self.assertIn('GPU [Render] <Error> vk_rasterizer.cpp:10 device lost', lines)
        self.assertEqual(lines[-1], 'The game exited (code 3221225477).')
        self.assertFalse([line for line in lines if line.startswith('Mouse look:')])

    def test_url_fits_and_keeps_both_ends(self):
        errors = '\n'.join(f'GPU [Render] <Error> page_manager.cpp:{i} region not mapped' for i in range(400))
        excerpt = bug_report.log_excerpt(LOG.replace('Runtime: loading', errors + '\nRuntime: loading'))
        self.assertGreater(len('\n'.join(excerpt)), 3 * bug_report.URL_LIMIT)
        url = bug_report.issue_url('crash', 'It crashed in the Hall of Heroes', 'Launcher: 2.2.1', excerpt)
        self.assertLessEqual(len(url), bug_report.URL_LIMIT)
        query = urllib.parse.parse_qs(urllib.parse.urlparse(url).query)
        self.assertEqual(query['template'], ['bug_report.yml'])
        self.assertEqual(query['title'], ['It crashed in the Hall of Heroes'])
        log = query['log_excerpt'][0]
        self.assertIn('lines cut, the whole log is in the report zip', log)
        self.assertTrue(log.startswith(LOG.splitlines()[0]))
        self.assertTrue(log.endswith('The game exited (code 3221225477).'))

    def test_feature_request_has_no_log(self):
        url = bug_report.issue_url('feature', 'Mouse in the menus', 'Launcher: 2.2.1', ['a log line'])
        query = urllib.parse.parse_qs(urllib.parse.urlparse(url).query)
        self.assertEqual(query['template'], ['feature_request.yml'])
        self.assertNotIn('log_excerpt', query)
        self.assertEqual(query['idea'], ['Mouse in the menus'])

    def test_url_fields_are_the_forms_ids(self):
        """Every field the launcher fills in is an id of its issue form."""
        forms = ROOT / '.github' / 'ISSUE_TEMPLATE'
        for kind, form in (('crash', 'bug_report.yml'), ('feature', 'feature_request.yml')):
            text = (forms / form).read_text(encoding='utf-8')
            query = urllib.parse.parse_qs(urllib.parse.urlparse(
                bug_report.issue_url(kind, 'x', 'Launcher: 2.2.1', ['line'])).query)
            for key in set(query) - {'template', 'title'}:
                self.assertIn(f'id: {key}\n', text, (form, key))


class ZipTest(unittest.TestCase):
    def test_zip_is_redacted_and_dump_only_when_asked(self):
        with tempfile.TemporaryDirectory() as tmp:
            user = Path(tmp) / 'user'
            (user / 'crashdumps').mkdir(parents=True)
            (user / 'last_run.log').write_text(LOG, encoding='utf-8')
            (user / 'crashdumps' / 'medievil-1.dmp').write_bytes(b'MDMP')
            ini = Path(tmp) / 'medievil.ini'
            ini.write_text(r'ue4_voice=C:\Users\Ty\voice' + '\n', encoding='utf-8')
            config = Path(tmp) / 'settings.json'
            config.write_text('{"player_name": "SirDaniel", "game_dir": "C:\\\\Users\\\\Ty\\\\Games"}', encoding='utf-8')
            files = bug_report.report_files(user, ini, config, tmp)
            self.assertEqual([name for name, _p in files], ['last_run.log', 'medievil.ini', 'settings.json'])
            path = bug_report.write_report(user / 'bug-reports', files, CTX, 'summary by SirDaniel')
            with zipfile.ZipFile(path) as z:
                self.assertEqual(sorted(z.namelist()), ['last_run.log', 'medievil.ini', 'report.txt', 'settings.json'])
                for name in z.namelist():
                    text = z.read(name).decode('utf-8')
                    for secret in ('SirDaniel', 'GALLOWMERE', r'Users\Ty', r'Users\\Ty'):
                        self.assertNotIn(secret, text, name)
            dump = bug_report.newest_dump(user)
            path = bug_report.write_report(user / 'bug-reports', files, CTX, 's', dump,
                                           now=__import__('datetime').datetime(2026, 1, 1))
            with zipfile.ZipFile(path) as z:
                self.assertIn('crashdumps/medievil-1.dmp', z.namelist())

    def test_game_mode(self):
        self.assertTrue(bug_report.game_mode({'XDG_CURRENT_DESKTOP': 'gamescope'}))
        self.assertTrue(bug_report.game_mode({'SteamGamepadUI': '1'}))
        self.assertFalse(bug_report.game_mode({'XDG_CURRENT_DESKTOP': 'KDE'}))


BODY = """### What happened

Camera | shakes in `Dan-cam`
and then it crashes

### Kind

Input (keyboard, mouse, gamepad)

### Version and system

Launcher: 2.2.1 (Windows zip)
Platform: Windows
Game: CUSA11227 version 01.02

### Log excerpt

```shell
GPU: x
```

### Logs

_No response_
"""


class IssueToQueueTest(unittest.TestCase):
    def test_form_parses(self):
        fields = issue2card.parse_form(BODY)
        self.assertEqual(fields['Kind'], 'Input (keyboard, mouse, gamepad)')
        self.assertEqual(fields['Logs'], '')
        self.assertEqual(issue2card.system_values(fields['Version and system'])['Game'], 'CUSA11227 version 01.02')

    def test_row_is_one_safe_line_without_log_text(self):
        issue = {'number': 7, 'title': 'Camera | `shakes`\nbadly', 'body': BODY, 'labels': [{'name': 'bug'}],
                 'url': 'https://github.com/BevelTipDrip/medievil_2019_pc_port/issues/7'}
        row = issue2card.queue_row(issue)
        self.assertEqual(row.count('\n'), 0)
        self.assertEqual(row.count('|'), 5)
        self.assertNotIn('GPU: x', row)
        self.assertTrue(row.startswith("| #7 | Camera / 'shakes' badly | input (keyboard, mouse, gamepad); Windows;"))

    def test_queue_adds_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            queue = Path(tmp) / 'plan-work-queue.md'
            queue.write_text('# Work queue\n\n## Other\n\ntext\n', encoding='utf-8')
            self.assertTrue(issue2card.add_to_queue('| #7 | a | b | open |', 7, queue))
            self.assertFalse(issue2card.add_to_queue('| #7 | a | b | open |', 7, queue))
            self.assertTrue(issue2card.add_to_queue('| #8 | c | d | open |', 8, queue))
            text = queue.read_text(encoding='utf-8')
            self.assertEqual(text.count(issue2card.HEADING), 1)
            self.assertIn('| #7 | a | b | open |\n| #8 | c | d | open |\n', text)
            self.assertTrue(text.startswith('# Work queue\n\n## Other\n\ntext\n'))


if __name__ == '__main__':
    unittest.main()
