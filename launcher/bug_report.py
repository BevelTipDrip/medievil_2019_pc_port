# SPDX-License-Identifier: GPL-2.0-or-later
"""Bug reports from the launcher: a GitHub issue form filled in from the URL, and a zip of the logs and
settings the player drags into it. Every text that leaves the machine goes through redact() first, and the
launcher shows it before anything is opened. No account, token or upload of our own: GitHub does the posting."""
import datetime
import os
from pathlib import Path
import re
import socket
import urllib.parse
import zipfile

REPO = 'BevelTipDrip/medievil_2019_pc_port'
ISSUES_NEW = f'https://github.com/{REPO}/issues/new'
# (key, label); 'feature' uses the feature request form and sends no logs.
KINDS = [('crash', 'Crash or freeze'), ('graphics', 'Graphics'), ('performance', 'Performance'),
         ('input', 'Input (keyboard, mouse, gamepad)'), ('other', 'Other bug'), ('feature', 'Feature request')]
# GitHub and browsers handle longer URLs badly; the full log is in the zip.
URL_LIMIT = 7000
DESCRIPTION_LIMIT = 2000
CUT = '[... {} lines cut, the whole log is in the report zip ...]'
# Lines that fill a log without saying anything about a fault.
NOISY = re.compile(r'^(Mouse: |Mouse look: |Save trace)|vk_instance.*unavailable')
WANTED = re.compile(r'error|fault|exception|crash|device lost|exited \(code', re.I)
MIN_WORD = 3  # shorter names (a two-letter user name) would replace parts of ordinary words


def redaction_context(env=None, home=None, host=None, player=''):
    """The names redact() replaces: home folder, user names, the PC's name and the player's name."""
    env = os.environ if env is None else env
    home = str(Path.home() if home is None else home)
    users = {env.get(k, '') for k in ('USERNAME', 'USER', 'LOGNAME')} | {Path(home).name}
    hosts = {env.get('COMPUTERNAME', ''), env.get('HOSTNAME', '')}
    if host is None:
        try:
            host = socket.gethostname()
        except OSError:
            host = ''
    hosts.add(host)
    return {'home': home, 'users': sorted(u for u in users if u), 'hosts': sorted(h for h in hosts if h),
            'player': str(player or '').strip()}


def _word(name):
    return re.compile(r'(?<![\w.-])' + re.escape(name) + r'(?![\w-])', re.I)


def redact(text, ctx):
    """text with the home folder as ~, user, PC and player names, Steam IDs and e-mail addresses replaced.
    Version numbers (drivers: 616.92.0.0) and addresses stay."""
    home = ctx['home'].rstrip('\\/')
    if len(home) > 3:
        for form in {home, home.replace('\\', '/'), home.replace('/', '\\')}:
            text = re.sub(re.escape(form), '~', text, flags=re.I)
    text = re.sub(r'([A-Za-z]:[\\/]+Users[\\/]+)[^\\/\s"\'<>]+', r'\1<user>', text, flags=re.I)
    text = re.sub(r'(/home/|/Users/)[^/\s"\'<>]+', r'\1<user>', text)
    text = re.sub(r'7656119\d{10}', '<steamid>', text)
    text = re.sub(r'[\w.+-]+@[\w-]+(\.[\w-]+)+', '<email>', text)
    for names, mark in ((ctx['users'], '<user>'), (ctx['hosts'], '<pc>'), ([ctx['player']], '<player>')):
        for name in names:
            if len(name) >= MIN_WORD:
                text = _word(name).sub(mark, text)
    return text


def log_excerpt(text, tail=30):
    """The lines of a run's log worth reading first: the start up to the first Runtime: line (settings, GPU,
    patches), every error / fault / crash / exit line, and the last `tail` lines; noise and repeats dropped."""
    lines = text.splitlines()
    head_end = next((i for i, line in enumerate(lines) if line.startswith('Runtime:')), min(len(lines), 40))
    picked = list(range(min(head_end, 60)))
    picked += [i for i, line in enumerate(lines) if WANTED.search(line)]
    picked += range(max(0, len(lines) - tail), len(lines))
    out, seen = [], set()
    for i in sorted(set(picked)):
        line = lines[i].rstrip()
        if not line or NOISY.search(line) or line in seen:
            continue
        seen.add(line)
        out.append(line)
    return out


def fields_text(fields):
    """The issue's fields as GitHub shows them (### heading, value): the preview and Copy summary."""
    return '\n\n'.join(f'### {name}\n\n{value}' for name, value in fields if value)


def issue_fields(kind, description, system, excerpt):
    """(heading, value) of what the issue gets; the headings are the forms' labels."""
    label = dict(KINDS).get(kind, kind)
    description = description.strip()[:DESCRIPTION_LIMIT]
    if kind == 'feature':
        return [('The idea', description), ('Version and system', system)]
    log = '\n'.join(excerpt)
    return [('What happened', description), ('Kind', label), ('Version and system', system),
            ('Log excerpt', f'```\n{log}\n```' if log else '')]


def issue_url(kind, description, system, excerpt, limit=URL_LIMIT):
    """The new-issue URL with the form filled in, at most `limit` characters: log lines go from the middle
    of the excerpt first (the start and the end say the most)."""
    feature = kind == 'feature'
    title = short_title(description)
    excerpt = [] if feature else list(excerpt)

    def build(lines):
        params = {'template': 'feature_request.yml' if feature else 'bug_report.yml', 'title': title}
        if feature:
            params.update(idea=description.strip()[:DESCRIPTION_LIMIT], system=system)
        else:
            params.update(what_happened=description.strip()[:DESCRIPTION_LIMIT], kind=dict(KINDS).get(kind, kind),
                          system=system, log_excerpt='\n'.join(lines))
        return ISSUES_NEW + '?' + urllib.parse.urlencode({k: v for k, v in params.items() if v},
                                                         quote_via=urllib.parse.quote)
    def trimmed(n):  # n lines out of the middle, one marker in their place
        if n >= len(excerpt):
            return []
        head = (len(excerpt) - n) // 2
        return excerpt[:head] + ([CUT.format(n)] if n else []) + excerpt[head + n:]
    for n in range(len(excerpt) + 1):
        url = build(trimmed(n))
        if len(url) <= limit:
            break
    return url


def system_summary(values):
    """system.txt and the issue's system field: one 'name: value' line each, empty values left out."""
    return '\n'.join(f'{name}: {value}' for name, value in values if value not in (None, ''))


def gpu_lines(log_text, limit=3):
    """The runtime's own 'GPU: ' lines (card, driver, Vulkan version, memory and feature notes); not the
    lines that merely mention the GPU (patch names, pacing, the renderer's 'GPU [Render]' messages)."""
    found = [line.strip() for line in log_text.splitlines()[:400] if line.startswith('GPU: ')]
    return found[:limit]


def short_title(text, limit=80):
    """The first line of text, cut at a word under `limit` characters."""
    first = text.strip().splitlines()[0] if text.strip() else ''
    if len(first) <= limit:
        return first
    cut = first[:limit - 1].rsplit(' ', 1)[0].rstrip(' ,.;:')
    return (cut or first[:limit - 1]) + '…'


ATTACH_LIMIT = 25 << 20  # GitHub's limit for a file attached to an issue


def dump_fits(dump):
    """Whether the dump, zipped with the logs, stays under GitHub's attachment limit (with room for the rest)."""
    return dump is not None and dump.stat().st_size < ATTACH_LIMIT - (2 << 20)


def newest_dump(user_dir):
    """The newest crash dump (<saves folder>/crashdumps, Windows: src/probe.c), or None."""
    dumps = sorted(Path(user_dir).glob('crashdumps/*.dmp'), key=lambda p: p.stat().st_mtime)
    return dumps[-1] if dumps else None


def report_files(user_dir, ini, config_file, data_dir):
    """(name in the zip, path) of the files a bug report carries, the ones that exist."""
    user_dir, data_dir = Path(user_dir), Path(data_dir)
    candidates = [('last_run.log', user_dir / 'last_run.log'), ('last_run.1.log', user_dir / 'last_run.1.log'),
                  ('medievil.ini', Path(ini)), ('settings.json', Path(config_file)),
                  ('extra-env.txt', data_dir / 'extra-env.txt'), ('mods.json', data_dir / 'mods.json'),
                  ('patches.json', data_dir / 'patches.json')]
    return [(name, path) for name, path in candidates if path.is_file()]


def write_report(folder, files, ctx, summary, dump=None, now=None):
    """The report zip in folder: each text file redacted, summary as report.txt, the crash dump as it is
    (only when the player ticked it). Returns its path."""
    now = now or datetime.datetime.now()
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f'medievil-report-{now:%Y%m%d-%H%M%S}.zip'
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('report.txt', redact(summary, ctx))
        for name, source in files:
            z.writestr(name, redact(source.read_text(encoding='utf-8', errors='replace'), ctx))
        if dump:
            z.write(dump, f'crashdumps/{Path(dump).name}')
    return path


def game_mode(env=None):
    """True in Steam Deck Game Mode (gamescope), where dragging a file into a browser is not practical."""
    env = os.environ if env is None else env
    return env.get('XDG_CURRENT_DESKTOP', '').lower() == 'gamescope' or env.get('SteamGamepadUI') == '1'
