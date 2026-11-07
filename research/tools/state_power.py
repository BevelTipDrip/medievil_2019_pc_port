"""Power and CPU time by toggle state, from apu_trace.py's CSV and deck.sh steps / ab output (Steam Deck).

For CPU cuts judged at a frame cap: at a fixed frame rate a saving shows as idle time and watts, not FPS.
The steps output's "HH:MM:SS mono NS STATE" lines mark each switch on CLOCK_MONOTONIC, the APU trace's
clock. Per state, over every period in that state less the first --skip seconds after the switch: mean
socket / CPU / SoC / GPU power, GPU clock and busy, the game process's CPU (all threads, % of one core)
and each traced thread's CPU; with --flip (the game's GOW3_FLIP_TRACE), the frame rate from its
"presented" notes, so a cap that did not hold is seen.

    py -3.14 state_power.py APU.csv STEPS.txt [--flip FLIP.csv] [--skip 3]
"""
import argparse
import bisect
import csv
import math


def read_apu(path):
    with open(path, newline='') as f:
        reader = csv.reader(f)
        header = next(reader)
        rows = [[float(x) if x != '' else math.nan for x in row] for row in reader if len(row) == len(header)]
    return header, {name: [row[i] for row in rows] for i, name in enumerate(header)}


def read_marks(path):
    marks = []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 4 and parts[1] == 'mono':
                marks.append((int(parts[2]), ' '.join(parts[3:])))
    if not marks:
        raise SystemExit(f'{path}: no "mono" lines (deck.sh steps output)')
    return marks


def presented(path):
    start, times = None, []
    with open(path, newline='') as f:
        for row in csv.reader(f):
            if len(row) != 3 or not row[2] or row[2] == 'ns':
                continue
            if row[0] == 'start':
                start = int(row[2])
            elif row[0] == 'presented':
                times.append(int(row[2]))
    return [start + t for t in sorted(times)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('apu')
    parser.add_argument('steps')
    parser.add_argument('--flip')
    parser.add_argument('--skip', type=float, default=3)
    args = parser.parse_args()
    header, apu = read_apu(args.apu)
    t = apu['t_ns']
    marks = read_marks(args.steps)
    period = marks[1][0] - marks[0][0] if len(marks) > 1 else 30e9
    spans = {}
    for (t0, state), nxt in zip(marks, marks[1:] + [(marks[-1][0] + period, None)]):
        spans.setdefault(state, []).append((t0 + args.skip * 1e9, nxt[0]))
    flips = presented(args.flip) if args.flip else None
    threads = [name[:-len('.run_ns')] for name in header if name.endswith('.run_ns')]
    means = ['socket_mw', 'cpu_mw', 'soc_mw', 'gfx_mw', 'gfx_mhz', 'gfx_busy']
    counters = (['process.cpu_ns'] if 'process.cpu_ns' in apu else []) + [f'{n}.run_ns' for n in threads]
    table = {}
    for state, ranges in spans.items():
        sums, n, used = {k: 0.0 for k in means}, 0, 0.0
        busy = {k: 0.0 for k in counters}
        frames = 0
        for lo, hi in ranges:
            i, j = bisect.bisect_left(t, lo), bisect.bisect_right(t, hi) - 1
            if j - i < 2:
                continue
            for k in means:
                sums[k] += sum(apu[k][i:j + 1])
            n += j - i + 1
            used += (t[j] - t[i]) / 1e9
            for k in counters:
                if not math.isnan(apu[k][i]) and not math.isnan(apu[k][j]):
                    busy[k] += apu[k][j] - apu[k][i]
            if flips:
                frames += bisect.bisect_right(flips, t[j]) - bisect.bisect_left(flips, t[i])
        if not n:
            continue
        row = {k: sums[k] / n for k in means}
        row.update({k: busy[k] / 1e9 / used * 100 for k in counters})
        row['seconds'] = used
        if flips:
            row['fps'] = frames / used
        table[state] = row
    if not table:
        raise SystemExit('no APU rows inside the marked periods (was the trace running?)')
    states = list(table)
    keys = ['seconds'] + (['fps'] if flips else []) + means + counters
    width = max(len(s) for s in states) + 2
    print(f'{"":34s}' + ''.join(f'{s:>{max(width, 12)}s}' for s in states))
    for k in keys:
        label = {'process.cpu_ns': 'process CPU %'}.get(k, k[:-len('.run_ns')] + ' CPU %' if k.endswith('.run_ns') else k)
        print(f'{label:34s}' + ''.join(f'{table[s][k]:>{max(width, 12)}.1f}' for s in states))


if __name__ == '__main__':
    main()
