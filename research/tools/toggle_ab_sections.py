"""Stage B microseconds per draw split by a live toggle's state (toggle-ab.ps1 runs): frames are assigned to the
state the runtime last logged ("Runtime: disabled optimizations second word=N" / "mask=N"); frames within 1 s
after a switch are skipped. GOW3_SECTIONS=1 with GOW3_SLOW_FRAMES.

    py -3.14 toggle_ab_sections.py <log> [min_draws 1000] [word 2]
"""
import re
import statistics
import sys

log = sys.argv[1]
min_draws = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
word = sys.argv[3] if len(sys.argv) > 3 else '2'
pattern = r'second word=(\d+)' if word == '2' else r'optimizations mask=(\d+)'
times = [l.split()[1] for l in open(log + '.times', encoding='utf8')]
lines = open(log, encoding='utf8', errors='replace').read().split('\n')
sec = lambda t: (lambda h, m, s: int(h) * 3600 + int(m) * 60 + float(s))(*t.split(':'))
state, since, draws, frame_t, frame_ms = 0, 0.0, None, 0.0, 0.0
groups = {}
for t, line in zip(times, lines):
    if m := re.search(pattern, line):
        state, since = int(m.group(1)), sec(t)
    elif line.startswith(('Slow frame', 'Frame sample')):
        draws = int(re.search(r': (\d+) draws', line).group(1))
        frame_ms = float(re.search(r' ([\d.]+) ms:', line).group(1))
        frame_t = sec(t)
    elif line.startswith('  stage B ms:') and draws:
        if draws >= min_draws and frame_t - since >= 1.0:
            s = {k.strip(): float(v) for k, v in re.findall(r'\s*([A-Za-z][A-Za-z_ ]*?) (\d+\.\d+)', line[13:])}
            groups.setdefault(state, []).append((draws, s, frame_ms))
        draws = None
for st, rows in sorted(groups.items()):
    print(f'toggle value {st} ({"on" if st == 0 else "off"}): {len(rows)} frames, {statistics.mean(r[0] for r in rows):.0f} draws, '
          f'frame median {statistics.median(r[2] for r in rows):.2f} ms')
    for k in ('packet', 'draw', 'bind', 'descriptors', 'Record calls', 'samplers', 'textures'):
        per = [1000 * s.get(k, 0) / d for d, s, _ in rows]
        print(f'   {k:13} us/draw median {statistics.median(per):.3f}  mean {statistics.mean(per):.3f}')
