"""Per-window table and summary of a run log's per-second stats (GOW3_FRAME_STATS lines).

    py -3.14 series.py <log> [--from N] [--to N] [--csv out.csv]

One row per "Frame stats" window (5 s): FPS, worst frame, flip pacing (median / stddev / p99 ms,
frames over 1.5x median), draws per frame, stage A waited % / stage B busy %, blocked on host copies /
copy threads / GPU ticks %, write faults per second. The summary gives percentiles over the selected
windows. Windows are numbered from 1 in log order; --from / --to select a range (e.g. in-game only).
"""
import argparse
import re
import statistics
import sys

NUM = r'([0-9.]+)'
PATTERNS = {
    'stats': re.compile(r'^Frame stats: ' + NUM + r' FPS, worst frame ' + NUM + r' ms.*?'
                        r'(\d+) write faults/s.*?' + NUM + r' draws/frame.*?host copies ' + NUM +
                        r'%.*?copy threads ' + NUM + r'%, GPU ticks ' + NUM + '%'),
    'pacing': re.compile(r'^Frame pacing: median ' + NUM + r' ms, stddev ' + NUM + r' ms, p99 ' + NUM +
                         r' ms, (\d+) frames'),
    'pipe': re.compile(r'^Draw pipe: .*stage A waited ' + NUM + r'%, stage B busy ' + NUM + '%'),
}
COLUMNS = ['fps', 'worst', 'pace_med', 'pace_sd', 'pace_p99', 'over', 'draws', 'a_wait', 'b_busy',
           'hostcopy', 'copythr', 'gpu', 'faults']


def windows(path):
    rows, current = [], {}
    with open(path, encoding='utf-8', errors='replace') as log:
        for line in log:
            if m := PATTERNS['stats'].match(line):
                if current:
                    rows.append(current)
                fps, worst, faults, draws, hostcopy, copythr, gpu = m.groups()
                current = dict(fps=float(fps), worst=float(worst), faults=int(faults), draws=float(draws),
                               hostcopy=float(hostcopy), copythr=float(copythr), gpu=float(gpu))
            elif current and (m := PATTERNS['pacing'].match(line)):
                current.update(pace_med=float(m[1]), pace_sd=float(m[2]), pace_p99=float(m[3]), over=int(m[4]))
            elif current and (m := PATTERNS['pipe'].match(line)):
                current.update(a_wait=float(m[1]), b_busy=float(m[2]))
    if current:
        rows.append(current)
    return rows


def percentile(values, p):
    values = sorted(values)
    return values[min(len(values) - 1, int(p * len(values)))] if values else float('nan')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--from', dest='first', type=int, default=1)
    ap.add_argument('--to', dest='last', type=int, default=0)
    ap.add_argument('--csv')
    ap.add_argument('--rows', action='store_true', help='print every window')
    a = ap.parse_args()
    rows = windows(a.log)
    selected = rows[a.first - 1:a.last or None]
    if a.csv:
        with open(a.csv, 'w', encoding='utf-8') as out:
            out.write('n,' + ','.join(COLUMNS) + '\n')
            for i, r in enumerate(rows, 1):
                out.write(f'{i},' + ','.join(str(r.get(c, '')) for c in COLUMNS) + '\n')
    if a.rows:
        print('   n ' + ' '.join(f'{c:>8}' for c in COLUMNS))
        for i, r in enumerate(rows[a.first - 1:a.last or None], a.first):
            print(f'{i:4d} ' + ' '.join(f'{r.get(c, float("nan")):8.1f}' for c in COLUMNS))
    print(f'{a.log}: windows {a.first}-{a.first + len(selected) - 1} of {len(rows)}')
    for c in COLUMNS:
        v = [r[c] for r in selected if c in r]
        if v:
            print(f'  {c:9} p10 {percentile(v, .1):8.2f}  median {statistics.median(v):8.2f}  '
                  f'p90 {percentile(v, .9):8.2f}  max {max(v):8.2f}')


if __name__ == '__main__':
    sys.exit(main())
