#!/usr/bin/env python3
"""Line-ending guard: catch edits that flip a file's line endings (CRLF <-> LF).

This repo mixes CRLF files (the launcher, tests, most docs, gow3_settings.*) and LF files (most C++ and shaders),
and has no .gitattributes. Git Bash's `sed -i` strips CRs, and Python text-mode writes (`open(p, 'w')`) write
LF: either turns a CRLF file into a whole-file diff that hides the real change (2026-11-14: the launcher,
test_launcher_controls.py, plan-fsr.md, medievil.ini; vk_ue4_*.cpp, gow3_graphics.h, ue4-fsr.md the other way).

  python research/tools/eol_guard.py               staged files vs HEAD (the pre-commit hook); exit 1 on a flip
  python research/tools/eol_guard.py --worktree    unstaged + staged changes vs HEAD
  python research/tools/eol_guard.py --fix [...]   put flipped working-tree files back to HEAD's line endings
  python research/tools/eol_guard.py --install     install the pre-commit hook (.git/hooks/pre-commit)
  python research/tools/eol_guard.py FILE ...      these files vs HEAD (also untracked: reports the style only)
  --base REV (or EOL_GUARD_BASE=REV)              compare with REV instead of HEAD: when HEAD itself has a flip,
                                                  e.g. committing the repair (EOL_GUARD_BASE=8585c5a^ git commit)

Style: CRLF (every line), LF (no CR), mixed or none (no newline). New files and binary files are skipped.
"""
import os
import subprocess
import sys

ROOT = subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True, text=True,
                      cwd=os.path.dirname(os.path.abspath(__file__))).stdout.strip()
HOOK = """#!/bin/sh
# Installed by research/tools/eol_guard.py --install: refuse commits that flip a file's line endings.
exec python "$(git rev-parse --show-toplevel)/research/tools/eol_guard.py"
"""


BASE = os.environ.get('EOL_GUARD_BASE') or 'HEAD'


def git(*args):
    return subprocess.run(['git', '-C', ROOT, *args], capture_output=True)


def style(data):
    if data is None:
        return None
    if b'\0' in data[:8000]:
        return 'binary'
    crlf = data.count(b'\r\n')
    lf = data.count(b'\n') - crlf
    if not crlf and not lf:
        return 'none'
    return 'CRLF' if crlf and not lf else 'LF' if lf and not crlf else 'mixed (%d CRLF, %d LF)' % (crlf, lf)


def blob(spec):
    r = git('show', spec)
    return r.stdout if r.returncode == 0 else None


def changed(staged_only):
    # Staged: the files this commit changes (index vs HEAD), each compared with BASE. Working tree: every file that
    # differs from BASE, in the index or the working tree.
    if staged_only:
        names = set(git('diff', '--cached', '--name-only', '--diff-filter=M', 'HEAD').stdout.decode().split('\n'))
    else:
        names = set(git('diff', '--cached', '--name-only', '--diff-filter=M', BASE).stdout.decode().split('\n'))
        names |= set(git('diff', '--name-only', '--diff-filter=M', BASE).stdout.decode().split('\n'))
    return sorted(n for n in names if n)


def check(paths, staged):
    flips = []
    for path in paths:
        before = style(blob(BASE + ':' + path))
        if staged:
            after = style(blob(':' + path))
        else:
            full = os.path.join(ROOT, path)
            after = style(open(full, 'rb').read()) if os.path.isfile(full) else None  # submodules: dirs
        if before is None:
            if after:
                print('%s: %s (not in %s)' % (path, after, BASE))
            continue
        if 'binary' in (before, after) or after is None or 'none' in (before, after):
            continue
        if before != after:
            flips.append((path, before, after))
    return flips


def fix(paths):
    for path in paths:
        before = style(blob(BASE + ':' + path))
        full = os.path.join(ROOT, path)
        if before not in ('CRLF', 'LF') or not os.path.isfile(full):
            print('%s: %s is %s, left alone' % (path, BASE, before))
            continue
        data = open(full, 'rb').read()
        lf = data.replace(b'\r\n', b'\n')
        out = lf if before == 'LF' else lf.replace(b'\n', b'\r\n')
        if out != data:
            open(full, 'wb').write(out)
            print('%s: back to %s' % (path, before))


def main(argv):
    global BASE
    if '--base' in argv:
        i = argv.index('--base')
        BASE = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    if '--install' in argv:
        hook = os.path.join(ROOT, '.git', 'hooks', 'pre-commit')
        if os.path.exists(hook) and open(hook).read() != HOOK:
            print('%s exists and is not ours: not replaced' % hook)
            return 1
        with open(hook, 'w', newline='\n') as f:
            f.write(HOOK)
        os.chmod(hook, 0o755)
        print('installed', hook)
        return 0
    files = [a for a in argv if not a.startswith('--')]
    if '--fix' in argv:
        fix(files or [p for p, _b, _a in check(changed(False), False)])
        return 0
    staged = not files and '--worktree' not in argv
    flips = check(files or changed(staged), staged)
    for path, before, after in flips:
        print('LINE ENDINGS FLIPPED: %s: %s %s, now %s' % (path, BASE, before, after))
    if flips:
        print('Fix: python research/tools/eol_guard.py --fix %s  (then git add them again)'
              % ' '.join(p for p, _b, _a in flips))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
