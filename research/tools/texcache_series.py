"""Joins a run log's GOW3_TEXCACHE_STATS reports with its frame stats, one row per 5 s window.

    py -3.14 texcache_series.py <log> [--every N]

Columns: FPS, flip stddev / p99 (ms), draws/frame, blocked on GPU %, then the texture cache report that
followed: live images, created, expands, overlaps, uploads (MiB), slow lookups, avg candidates, GPU MiB.
"""
import argparse
import re

NUM = r'([0-9.]+)'
STATS = re.compile(r'^Frame stats: ' + NUM + r' FPS.*?' + NUM + r' draws/frame.*?GPU ticks ' + NUM + '%')
PACE = re.compile(r'^Frame pacing: median ' + NUM + r' ms, stddev ' + NUM + r' ms, p99 ' + NUM)
CACHE = re.compile(r'^Texture cache stats \(5 s\): (\d+) images registered \(\+(\d+) -(\d+)\).*?lookups fast (\d+) '
                   r'slow (\d+) \(avg ' + NUM + r' candidates\); overlaps (\d+), expands (\d+); uploads (\d+) \('
                   + NUM + r' MiB\); alias syncs (\d+), copies (\d+); GPU memory (\d+) MiB')

ap = argparse.ArgumentParser()
ap.add_argument('log')
ap.add_argument('--every', type=int, default=1)
a = ap.parse_args()
rows, row = [], None
for line in open(a.log, encoding='utf-8', errors='replace'):
    if m := STATS.match(line):
        row = {'fps': float(m[1]), 'draws': float(m[2]), 'gpu': float(m[3])}
        rows.append(row)
    elif row is not None and (m := PACE.match(line)):
        row.update(sd=float(m[2]), p99=float(m[3]))
    elif row is not None and (m := CACHE.match(line)):
        row.update(live=int(m[1]), created=int(m[2]), slow=int(m[5]), cand=float(m[6]), overlaps=int(m[7]),
                   expands=int(m[8]), upmib=float(m[10]), syncs=int(m[11]), gpumib=int(m[13]))
cols = ['fps', 'sd', 'p99', 'draws', 'gpu', 'live', 'created', 'expands', 'overlaps', 'upmib', 'slow', 'cand',
        'syncs', 'gpumib']
print('   n ' + ' '.join(f'{c:>8}' for c in cols))
for i, r in enumerate(rows, 1):
    if i % a.every == 0:
        print(f'{i:4d} ' + ' '.join(f'{r[c]:8.1f}' if c in r else f'{"-":>8}' for c in cols))
