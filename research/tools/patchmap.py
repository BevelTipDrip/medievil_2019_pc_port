#!/usr/bin/env python3
"""Patch map: one card per patch in research/patches/, facts computed from git and the code,
lookups that need no reading of the big docs, and a check that keeps the cards true.

  python research/tools/patchmap.py facts            recompute research/patches/_facts/ (git, code, docs)
  python research/tools/patchmap.py index            rebuild research/patches/INDEX.md and TESTS.md
  python research/tools/patchmap.py check [--list]   validate cards against the code; coverage gaps
  python research/tools/patchmap.py file <path>      patches touching a file (with their invariants)
  python research/tools/patchmap.py switch <name>    a GOW3_* / perf_* / toggle: patch and code sites
  python research/tools/patchmap.py commit <hash>    the patch a commit belongs to
  python research/tools/patchmap.py test <id>        where a test ID (T-F6, N-30, Launch 14) is written up
  python research/tools/patchmap.py show <id>        print a card
  python research/tools/patchmap.py find <text>      search card titles and bodies
  python research/tools/patchmap.py doc <file.md>    a doc's outline with anchors
  python research/tools/patchmap.py area <area>      facts for one area (cards, commits by path)

Card format: research/patches/README.md. Python 3.8+, standard library only.
"""
import os
import re
import subprocess
import sys
from collections import defaultdict

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
PATCHES = os.path.join(ROOT, 'research', 'patches')
FACTS = os.path.join(PATCHES, '_facts')
DOCS = os.path.join(ROOT, 'research', 'docs')
UPSTREAM = 'upstream/main'
MAIN_BRANCHES = ('lab', 'deck-lab')  # every code commit on these must belong to a card
CODE_DIRS = ('src', 'gpu', 'scripts', 'launcher', 'run.py', 'tests', 'patches', 'build.sh', 'CMakeLists.txt')
SKIP_DIRS = ('gpu/third_party/',)

AREAS = {
    'memory': 'guest memory, pools, flexible/direct memory, region table (src/runtime_memory.c and friends)',
    'runtime': 'loader, HLE libraries, threads, locks, files, audio, input (src/ outside memory)',
    'stage-a': 'PM4 decode, Liverpool, command processing, stage A pacing (amdgpu/liverpool*, gow3_stage_a*)',
    'stage-b': 'draw recording, rasterizer, pipelines, descriptors, recorder, fences, drains',
    'caches': 'texture cache, buffer cache, readbacks, page tracking, uploads/downloads',
    'present': 'presentation, frame pacing, limiters, vblank, Reflex, VSync, overlay',
    'ue4': 'UE4 config overrides, eboot patches, game hooks, particles, render scale, boot resolution',
    'deck': 'Linux / Steam Deck specific code, build, launcher Deck preset, power and clocks',
    'launcher': 'launcher, settings, medievil.ini keys, run.py, packaging, build scripts',
    'tooling': 'diagnostics, tracing, capture, stats, research tools (no gameplay effect)',
}
STATUS = {
    'active': 'in the build; see defaults for whether it is on',
    'rejected': 'built and measured, not kept; code may remain behind a switch that is off',
    'reverted': 'code removed again',
    'superseded': 'replaced by another card (superseded_by)',
    'diagnostic': 'measurement or debug aid, no gameplay effect when off',
    'chore': 'small commits with no behaviour of their own (refactors, build fixes, comments)',
}
DEFAULTS = ('on', 'off', 'n/a', 'mixed')
LIST_KEYS = ('commits', 'files', 'symbols', 'switches', 'tests', 'docs', 'depends_on', 'conflicts_with',
             'supersedes', 'related')
REQUIRED = ('id', 'title', 'area', 'status', 'platforms', 'default_windows', 'default_deck', 'origin')
SECTIONS = ('What it does', 'Invariants', 'Interactions', 'Evidence', 'History and pitfalls')
ID_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9.+_-]*$')
TEST_RES = [
    re.compile(r'\bT-[A-Za-z0-9][A-Za-z0-9.]*(?:-[A-Za-z0-9.]+)*'),
    re.compile(r'\bN-\d+\b'),
    re.compile(r'\b(?:[Ll]aunch(?:es)?|[Nn]ative run|[Pp]roton runs?|[Dd]eck run|[Rr]un) \d+(?:-\d+)?\b'),
]


def git(*args):
    out = subprocess.run(['git', '-C', ROOT] + list(args), capture_output=True, text=True,
                         encoding='utf-8', errors='replace')
    if out.returncode != 0:
        raise SystemExit('git %s failed: %s' % (' '.join(args), out.stderr.strip()))
    return out.stdout


def is_code_path(path):
    if any(path.startswith(s) for s in SKIP_DIRS):
        return False
    return any(path == d or path.startswith(d + '/') for d in CODE_DIRS)


# ---------------------------------------------------------------- facts

def facts():
    os.makedirs(FACTS, exist_ok=True)
    branches = [b for b in git('for-each-ref', '--format=%(refname:short)', 'refs/heads').split()
                if not b.startswith('worktree-') and b != 'main']
    member = defaultdict(set)
    for b in branches:
        for h in git('rev-list', '%s..%s' % (UPSTREAM, b)).split():
            member[h].add(b)
    log = git('log', '--all', '--name-only', '--format=\x01%H\x02%P\x02%ad\x02%s', '--date=short',
              '%s..deck-lab' % UPSTREAM, *['%s..%s' % (UPSTREAM, b) for b in branches if b != 'deck-lab'])
    seen = set()
    rows = []
    for block in log.split('\x01')[1:]:
        head, _, rest = block.partition('\n')
        h, parents, date, subject = head.split('\x02', 3)
        if h in seen or h not in member:
            continue
        seen.add(h)
        files = [f for f in rest.split('\n') if f.strip()]
        if len(parents.split()) > 1:
            kind = 'merge'
        elif any(is_code_path(f) for f in files):
            kind = 'code'
        else:
            kind = 'docs'
        bs = member[h]
        # lab is inside deck-lab: 'lab' = both, 'deck-lab' = Deck branch only, else the unmerged branches
        tag = 'lab' if 'lab' in bs else 'deck-lab' if 'deck-lab' in bs else ','.join(sorted(bs))
        rows.append((h[:10], date, kind, tag, subject.replace('\t', ' '), ';'.join(files)))
    rows.sort(key=lambda r: r[1])
    with open(os.path.join(FACTS, 'commits.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# hash\tdate\tkind\tbranches\tsubject\tfiles  (generated by patchmap.py facts)\n')
        for r in rows:
            f.write('\t'.join(r) + '\n')

    # Switches: names in string literals of code files ("GOW3_X"), upstream's set for origin.
    def switch_sites(rev):
        args = ['grep', '-n', '-o', '-E', '"GOW3_[A-Z0-9_]+"']
        if rev:
            args.append(rev)
        args += ['--'] + [d for d in CODE_DIRS] + [':!gpu/third_party']
        out = subprocess.run(['git', '-C', ROOT] + args, capture_output=True, text=True,
                             encoding='utf-8', errors='replace').stdout
        sites = defaultdict(list)
        for line in out.splitlines():
            parts = line.split(':')
            if rev:
                parts = parts[1:]
            if len(parts) < 3:
                continue
            path, lineno, name = parts[0], parts[1], parts[-1].strip('"')
            if name.endswith('_'):
                continue
            sites[name].append('%s:%s' % (path, lineno))
        return sites
    here = switch_sites(None)
    up = switch_sites(UPSTREAM)
    with open(os.path.join(FACTS, 'switches.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# name\torigin\tsites  (origin: upstream = also in %s; generated)\n' % UPSTREAM)
        for name in sorted(here):
            f.write('%s\t%s\t%s\n' % (name, 'upstream' if name in up else 'ours', ' '.join(here[name][:12])))
        up_text = git('show', UPSTREAM + ':gpu/shim/gow3_toggles.h') + git(
            'show', UPSTREAM + ':scripts/graphics_options.py')
        for t in toggles():
            ours = not re.search(r'\b%s\b' % t[0].split(':')[1], up_text)
            f.write('%s\t%s\t%s\n' % (t[0], 'toggle' if ours else 'toggle-upstream', t[1]))
        for k in ini_keys():
            f.write('%s\t%s\t%s\n' % (k[0], 'ini-upstream' if "'%s'" % k[0] in up_text else 'ini', k[1]))

    # Doc headings with anchors (outline of every doc) and test IDs found in them.
    with open(os.path.join(FACTS, 'headings.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# file\tline\tlevel\tanchor\theading  (generated)\n')
        for doc in sorted(os.listdir(DOCS)):
            if doc.endswith('.md'):
                for line, level, anchor, text in headings(os.path.join(DOCS, doc)):
                    f.write('%s\t%d\t%d\t%s\t%s\n' % (doc, line, level, anchor, text))
    # Test write-ups: a heading naming a test ID, or a paragraph / bullet starting with a bold test ID.
    with open(os.path.join(FACTS, 'tests.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# id\tfile\tline\tanchor (enclosing heading)\ttext  (generated)\n')
        for doc in sorted(os.listdir(DOCS)):
            if doc.endswith('.md'):
                for tid, line, anchor, text in test_sites(os.path.join(DOCS, doc)):
                    f.write('%s\t%s\t%d\t%s\t%s\n' % (tid, doc, line, anchor, text))
    n_code = sum(1 for r in rows if r[2] == 'code')
    print('facts: %d commits (%d code), %d switches in code (%d ours), %d toggles, %d ini keys -> %s'
          % (len(rows), n_code, len(here), sum(1 for n in here if n not in up), len(toggles()),
             len(ini_keys()), os.path.relpath(FACTS, ROOT)))


def toggles():
    path = os.path.join(ROOT, 'gpu', 'shim', 'gow3_toggles.h')
    out, ns = [], None
    with open(path, encoding='utf-8') as f:
        for i, line in enumerate(f, 1):
            m = re.match(r'\s*namespace (Gow3Toggle2?)\b', line)
            if m:
                ns = 'toggle2' if m.group(1) == 'Gow3Toggle2' else 'toggle'
            elif re.match(r'\s*namespace ', line):
                ns = None
            m = re.match(r'\s*([A-Z][A-Za-z0-9]+)\s*=\s*([0-9]+[uUlL]*(?:\s*<<\s*\d+)?)\s*,', line)
            if ns and m:
                out.append(('%s:%s' % (ns, m.group(1)), 'gpu/shim/gow3_toggles.h:%d' % i))
    return out


def ini_keys():
    path = os.path.join(ROOT, 'scripts', 'graphics_options.py')
    out, seen = [], set()
    with open(path, encoding='utf-8') as f:
        for i, line in enumerate(f, 1):
            for k in re.findall(r"'((?:perf|present)_[a-z0-9_]+)'", line):
                if k not in seen:
                    seen.add(k)
                    out.append((k, 'scripts/graphics_options.py:%d' % i))
    return out


def slug(text):
    s = text.strip().lower()
    s = re.sub(r'[^\w\- ]', '', s, flags=re.UNICODE)
    return s.replace(' ', '-')


def headings(path):
    out, count, fence = [], defaultdict(int), False
    with open(path, encoding='utf-8', errors='replace') as f:
        for i, line in enumerate(f, 1):
            if line.startswith('```'):
                fence = not fence
            if fence:
                continue
            m = re.match(r'^(#{1,6})\s+(.*?)\s*#*\s*$', line)
            if m:
                text = m.group(2)
                base = slug(re.sub(r'`|\*\*|\[([^\]]*)\]\([^)]*\)', r'\1', text))
                n = count[base]
                count[base] += 1
                out.append((i, len(m.group(1)), base if n == 0 else '%s-%d' % (base, n), text))
    return out


def test_sites(path):
    heads = {line: anchor for line, level, anchor, text in headings(path)}
    out, anchor = [], ''
    with open(path, encoding='utf-8', errors='replace') as f:
        for i, line in enumerate(f, 1):
            if i in heads:
                anchor = heads[i]
                text = line.lstrip('#').strip()
            else:
                m = re.match(r'^\s*(?:[-*] )?\*\*(.{0,120})', line)
                if not m or not any(rx.match(m.group(1)) for rx in TEST_RES):
                    continue
                text = m.group(1)
            for tid in sorted(set(t for rx in TEST_RES for t in rx.findall(text[:60]))):
                out.append((tid, i, anchor, text[:100].replace('\t', ' ')))
    return out


def load_tsv(name):
    path = os.path.join(FACTS, name)
    if not os.path.exists(path):
        raise SystemExit('missing %s: run "patchmap.py facts" first' % os.path.relpath(path, ROOT))
    with open(path, encoding='utf-8') as f:
        return [l.rstrip('\n').split('\t') for l in f if not l.startswith('#') and l.strip()]


# ---------------------------------------------------------------- cards

def parse_card(path):
    with open(path, encoding='utf-8') as f:
        text = f.read()
    m = re.match(r'^---\n(.*?)\n---\n(.*)$', text, re.S)
    if not m:
        return None, text, ['no front matter']
    meta, errors = {}, []
    for n, line in enumerate(m.group(1).split('\n'), 2):
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        k, sep, v = line.partition(':')
        if not sep:
            errors.append('line %d: not "key: value"' % n)
            continue
        k, v = k.strip(), v.strip()
        meta[k] = [x.strip() for x in v.split(',') if x.strip()] if k in LIST_KEYS else v
    meta['_path'] = path
    meta['_rel'] = os.path.relpath(path, PATCHES).replace(os.sep, '/')
    return meta, m.group(2), errors


def cards():
    out = []
    for area in sorted(os.listdir(PATCHES)) if os.path.isdir(PATCHES) else []:
        d = os.path.join(PATCHES, area)
        if area.startswith('_') or not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if name.endswith('.md'):
                meta, body, errors = parse_card(os.path.join(d, name))
                out.append((meta or {'_path': os.path.join(d, name), '_rel': '%s/%s' % (area, name)},
                            body, errors))
    return out


def section(body, title):
    m = re.search(r'^## %s\s*\n(.*?)(?=^## |\Z)' % re.escape(title), body, re.S | re.M)
    return m.group(1).strip() if m else ''


def commit_hits(meta, short):
    return any(short.startswith(c) or c.startswith(short) for c in meta.get('commits', []))


# ---------------------------------------------------------------- index

def index():
    cs = cards()
    by_area = defaultdict(list)
    for meta, body, _ in cs:
        by_area[meta.get('area', '?')].append((meta, body))
    commits = {r[0]: r for r in load_tsv('commits.tsv')}
    lines = ['# Patch index (generated by `research/tools/patchmap.py index`; do not edit)', '',
             'One line per card. Open a card for invariants, interactions and evidence; look up without',
             'reading: `patchmap.py file <path>` / `switch <name>` / `commit <hash>` / `test <id>`.',
             'Card format and statuses: [README.md](README.md). W / D = default on Windows / Deck.', '',
             '%d cards.' % len(cs), '']
    for area in list(AREAS) + sorted(a for a in by_area if a not in AREAS):
        rows = by_area.get(area)
        if not rows:
            continue
        lines += ['## %s' % area, '', '_%s_' % AREAS.get(area, ''), '',
                  '| ID | Status | W / D | Branch | Switches | Title |', '|---|---|---|---|---|---|']
        for meta, _ in sorted(rows, key=lambda r: r[0].get('id', '').lower()):
            br = set()
            for c in meta.get('commits', []):
                for h, r in commits.items():
                    if h.startswith(c) or c.startswith(h):
                        br.update(r[3].split(','))
            branch = 'lab' if 'lab' in br else ('deck-lab' if 'deck-lab' in br else ','.join(sorted(br)) or '-')
            sw = ', '.join(meta.get('switches', [])[:3]) + (' …' if len(meta.get('switches', [])) > 3 else '')
            lines.append('| [%s](%s) | %s | %s / %s | %s | %s | %s |' % (
                meta.get('id', '?'), meta['_rel'], meta.get('status', '?'), meta.get('default_windows', '?'),
                meta.get('default_deck', '?'), branch, sw.replace('|', '\\|'), meta.get('title', '').replace('|', '\\|')))
        lines.append('')
    write(os.path.join(PATCHES, 'INDEX.md'), lines)

    tests = defaultdict(list)
    for tid, doc, line, anchor, text in load_tsv('tests.tsv'):
        tests[tid].append((doc, line, anchor, text))
    cited = defaultdict(list)
    for meta, _, _ in cs:
        for t in meta.get('tests', []):
            cited[t].append(meta.get('id', '?'))
    tl = ['# Test IDs (generated by `research/tools/patchmap.py index`; do not edit)', '',
          'Every doc heading that names a test run, with its anchor; cards citing the ID last.',
          'Some IDs were reused for different runs (each write-up is listed).', '',
          '| ID | Where | Text | Cards |', '|---|---|---|---|']

    def key(t):
        return [int(p) if p.isdigit() else p.lower() for p in re.split(r'(\d+)', t)]
    for tid in sorted(tests, key=key):
        for doc, line, anchor, text in tests[tid]:
            tl.append('| %s | [%s:%s](../docs/%s#%s) | %s | %s |' % (
                tid, doc, line, doc, anchor, text[:80].replace('|', '\\|'), ', '.join(cited.get(tid, []))))
    write(os.path.join(PATCHES, 'TESTS.md'), tl)
    print('index: %d cards, %d test IDs' % (len(cs), len(tests)))


def write(path, lines):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines).rstrip() + '\n')


# ---------------------------------------------------------------- check

def check(list_all=False, only_area=None):
    cs = cards()
    commits = load_tsv('commits.tsv')
    switches = {r[0]: r for r in load_tsv('switches.tsv')}
    anchors = defaultdict(set)
    for doc, line, level, anchor, text in load_tsv('headings.tsv'):
        anchors[doc].add(anchor)
    tracked = set(git('ls-files').splitlines())
    errors, warns = [], []
    ids = defaultdict(list)
    file_cache = {}

    def read(p):
        if p not in file_cache:
            try:
                with open(os.path.join(ROOT, p), encoding='utf-8', errors='replace') as f:
                    file_cache[p] = f.read()
            except OSError:
                file_cache[p] = None
        return file_cache[p]

    for meta, body, perr in cs:
        rel = meta['_rel']
        if only_area and not rel.startswith(only_area + '/'):
            continue
        e = lambda msg: errors.append('%s: %s' % (rel, msg))
        for x in perr:
            e(x)
        for k in REQUIRED:
            if not meta.get(k):
                e('missing %s' % k)
        cid = meta.get('id', '')
        ids[cid].append(rel)
        if cid and not ID_RE.match(cid):
            e('bad id %r' % cid)
        if meta.get('area') and meta['area'] not in AREAS:
            e('unknown area %r' % meta['area'])
        if meta.get('area') and not rel.startswith(meta['area'] + '/'):
            e('card is not in its area folder')
        if meta.get('status') and meta['status'] not in STATUS:
            e('unknown status %r' % meta['status'])
        for k in ('default_windows', 'default_deck'):
            if meta.get(k) and meta[k] not in DEFAULTS:
                e('%s must be one of %s' % (k, '/'.join(DEFAULTS)))
        if meta.get('status') == 'superseded' and not meta.get('superseded_by'):
            e('superseded without superseded_by')
        gone = meta.get('status') in ('reverted',)
        for s in SECTIONS:
            if meta.get('status') != 'chore' and not section(body, s):
                e('empty or missing section "## %s"' % s)
        for c in meta.get('commits', []):
            if not re.match(r'^[0-9a-f]{7,40}$', c):
                e('bad commit %r' % c)
            elif not any(r[0].startswith(c) or c.startswith(r[0]) for r in commits):
                if subprocess.run(['git', '-C', ROOT, 'cat-file', '-e', c + '^{commit}'],
                                  capture_output=True).returncode != 0:
                    e('commit %s does not exist' % c)
        for p in meta.get('files', []):
            if p not in tracked and not gone:
                e('file %s not in the repo' % p)
        for sym in meta.get('symbols', []):
            if gone:
                continue
            rx = re.compile(r'\b%s\b' % re.escape(sym.split('::')[-1].rstrip('()')))
            if not any(read(p) and rx.search(read(p)) for p in meta.get('files', [])):
                e('symbol %s not found in the card\'s files' % sym)
        for sw in meta.get('switches', []):
            if sw not in switches and not gone:
                e('switch %s not found in the code (facts)' % sw)
        for d in meta.get('docs', []):
            doc, _, anchor = d.partition('#')
            if not os.path.exists(os.path.join(DOCS, doc)) and not os.path.exists(os.path.join(ROOT, doc)):
                e('doc %s not found' % doc)
            elif anchor and doc in anchors and anchor not in anchors[doc]:
                e('anchor %s not in %s (patchmap.py doc %s lists them)' % (anchor, doc, doc))
    known = set(m.get('id', '') for m, _, _ in cs)  # all areas, also when checking one
    for meta, body, _ in cs:
        if only_area and not meta['_rel'].startswith(only_area + '/'):
            continue
        for k in ('depends_on', 'conflicts_with', 'supersedes', 'related'):
            for ref in meta.get(k, []):
                if ref not in known:
                    errors.append('%s: %s refers to unknown card %s' % (meta['_rel'], k, ref))
        if meta.get('superseded_by') and meta['superseded_by'] not in known:
            errors.append('%s: superseded_by unknown card %s' % (meta['_rel'], meta['superseded_by']))
    for cid, rels in ids.items():
        if len(rels) > 1:
            errors.append('id %s used by %s' % (cid, ', '.join(rels)))

    # Coverage
    covered_commits = set()
    for meta, _, _ in cs:
        for c in meta.get('commits', []):
            covered_commits.add(c[:7])
    uncommitted = [r for r in commits if r[2] == 'code' and set(r[3].split(',')) & set(MAIN_BRANCHES)
                   and r[0][:7] not in covered_commits]
    covered_sw = set(sw for meta, _, _ in cs for sw in meta.get('switches', []))
    unsw = [n for n, r in switches.items() if r[1] in ('ours', 'toggle', 'ini') and n not in covered_sw]
    for msg in errors:
        print('ERROR', msg)
    print('\n%d cards, %d errors' % (len(cs), len(errors)))
    print('code commits on %s without a card: %d of %d' % ('/'.join(MAIN_BRANCHES), len(uncommitted),
          sum(1 for r in commits if r[2] == 'code' and set(r[3].split(',')) & set(MAIN_BRANCHES))))
    print('our switches / toggles / ini keys without a card: %d of %d' % (
        len(unsw), sum(1 for r in switches.values() if r[1] in ('ours', 'toggle', 'ini'))))
    if list_all:
        for r in uncommitted:
            print('  commit %s %s [%s] %s' % (r[0], r[1], r[3], r[4][:90]))
        for n in sorted(unsw):
            print('  switch %s (%s) %s' % (n, switches[n][1], switches[n][2].split(' ')[0]))
    return 1 if errors else 0


# ---------------------------------------------------------------- lookups

def brief(meta, body, invariants=True):
    print('%s [%s, W %s / D %s] %s  (%s)' % (meta.get('id'), meta.get('status'), meta.get('default_windows'),
          meta.get('default_deck'), meta.get('title'), 'research/patches/' + meta['_rel']))
    if meta.get('switches'):
        print('    switches: ' + ', '.join(meta['switches']))
    if invariants:
        inv = section(body, 'Invariants')
        for line in inv.splitlines()[:8]:
            print('    ' + line)


def cmd_file(path):
    path = path.replace('\\', '/').lstrip('./')
    hits = [(m, b) for m, b, _ in cards() if any(f == path or f.endswith('/' + path) or path.endswith(f)
                                                   for f in m.get('files', []))]
    for m, b in hits:
        brief(m, b)
    rows = [r for r in load_tsv('commits.tsv') if r[2] == 'code' and any(
        f == path or f.endswith('/' + path) for f in r[5].split(';'))]
    covered = set(c[:7] for m, _, _ in cards() for c in m.get('commits', []))
    loose = [r for r in rows if r[0][:7] not in covered]
    print('\n%d cards; %d commits touch it, %d without a card%s' % (len(hits), len(rows), len(loose),
          ':' if loose else ''))
    for r in loose[-15:]:
        print('  %s %s %s' % (r[0], r[1], r[4][:90]))


def cmd_switch(name):
    rows = [r for r in load_tsv('switches.tsv') if name.lower() in r[0].lower()]
    for r in rows:
        print('%s (%s): %s' % (r[0], r[1], r[2]))
    for m, b, _ in cards():
        if any(name.lower() in s.lower() for s in m.get('switches', [])):
            brief(m, b, invariants=False)


def cmd_commit(h):
    for r in load_tsv('commits.tsv'):
        if r[0].startswith(h[:10]) or h.startswith(r[0]):
            print('%s %s %s [%s] %s\n  files: %s' % (r[0], r[1], r[2], r[3], r[4], r[5].replace(';', ' ')))
    for m, b, _ in cards():
        if commit_hits(m, h[:10]):
            brief(m, b)


def cmd_test(tid):
    for t, doc, line, anchor, text in load_tsv('tests.tsv'):
        if t.lower() == tid.lower():
            print('research/docs/%s:%s  #%s\n    %s' % (doc, line, anchor, text[:100]))
    for m, b, _ in cards():
        if tid in m.get('tests', []):
            brief(m, b, invariants=False)


def cmd_show(cid):
    for m, b, _ in cards():
        if m.get('id', '').lower() == cid.lower():
            with open(m['_path'], encoding='utf-8') as f:
                print(f.read())
            return
    print('no card %s; try: patchmap.py find %s' % (cid, cid))


def cmd_find(text):
    rx = re.compile(re.escape(text), re.I)
    for m, b, _ in cards():
        if rx.search(m.get('title', '')) or rx.search(b) or rx.search(m.get('id', '')):
            brief(m, b, invariants=False)


def cmd_doc(doc):
    doc = os.path.basename(doc)
    for d, line, level, anchor, text in load_tsv('headings.tsv'):
        if d == doc:
            print('%5s %s%s  #%s' % (line, '  ' * (int(level) - 1), text[:100], anchor))


def cmd_area(area):
    for m, b, _ in cards():
        if m.get('area') == area:
            brief(m, b, invariants=False)


def main(argv):
    if len(argv) < 2 or argv[1] in ('-h', '--help'):
        print(__doc__)
        return 0
    cmd, rest = argv[1], argv[2:]
    if cmd == 'facts':
        facts()
    elif cmd == 'index':
        index()
    elif cmd == 'check':
        area = next((a for a in rest if not a.startswith('-')), None)
        return check('--list' in rest, area)
    elif cmd in ('file', 'switch', 'commit', 'test', 'show', 'find', 'doc', 'area') and rest:
        globals()['cmd_' + cmd](' '.join(rest))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
