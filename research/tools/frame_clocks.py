"""Frame times beside APU clocks and power, frame by frame (Steam Deck push-pull study; deck.sh trace).

Inputs: the game's flip trace (GOW3_FLIP_TRACE: stage, flip argument, ns since its "start" row, which
holds the start in CLOCK_MONOTONIC ns) and apu_trace.py's CSV (CLOCK_MONOTONIC ns rows). A frame is
the interval between two "presented" notes; for each one:
- ft: its length; game: the game's submit-to-submit interval for that flip; rec: submit -> irq (stage B
  has recorded the flip); gpu: irq -> gpu_ready (the GPU finished it).
- over the frame: mean GPU clock and busy, socket / CPU / GPU power; for each traced thread its run
  time in the frame (ms) and the mean clock of the core it ran on.

Printed: frame-time spread, the autocorrelation of frame times at lags 1-8 (negative at lag 1 = long /
short alternation; a peak at lag k = a k-frame cycle), the share of alternating pairs, means by
frame-time band (short / middle / long thirds and spikes over 1.3x the median), and each metric's
correlation with frame time one frame before, at, and one frame after (a clock that drops in the frame
before a long frame leads it). --csv writes the per-frame table.

    py -3.14 frame_clocks.py FLIP.csv APU.csv [--skip S] [--seconds S] [--csv OUT]
"""
import argparse
import bisect
import csv
import math
import statistics


def read_flip(path):
    stages, start = {}, None
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            if row['stage'] == 'start':
                start = int(row['ns'])
                continue
            stages.setdefault(row['stage'], []).append((int(row['ns']), int(row['flip_arg'])))
    if start is None:
        raise SystemExit(f'{path}: no start row')
    for events in stages.values():
        events.sort()
    return start, stages


def read_apu(path):
    with open(path, newline='') as f:
        reader = csv.reader(f)
        header = next(reader)
        rows = [[float(x) if x != '' else math.nan for x in row] for row in reader if row]
    columns = {name: [row[i] for row in rows] for i, name in enumerate(header)}
    threads = [name[:-len('.run_ns')] for name in header if name.endswith('.run_ns')]
    cores = [name for name in header if name.startswith('core') and name.endswith('_mhz')]
    return columns, threads, cores


def flips(stages, later=('irq', 'gpu_ready', 'presented', 'flipdone')):
    """One record per submitted flip, in submit order: each later stage is the first note with the
    same flip argument at or after the submit and before the next submit with that argument (the
    present can be queued before the GPU has finished, so the stages are matched per flip, not by
    time order). A stage a flip never reached (a dropped flip) is None."""
    submits = stages.get('submit', [])
    next_same, seen = [None] * len(submits), {}
    for i in range(len(submits) - 1, -1, -1):
        next_same[i] = seen.get(submits[i][1])
        seen[submits[i][1]] = submits[i][0]
    by_arg = {stage: {} for stage in later}
    for stage in later:
        for ns, arg in stages.get(stage, []):
            by_arg[stage].setdefault(arg, []).append(ns)
    records = []
    for (ns, arg), bound in zip(submits, next_same):
        record = {'submit': ns}
        for stage in later:
            events = by_arg[stage].get(arg, [])
            i = bisect.bisect_left(events, ns)
            record[stage] = events[i] if i < len(events) and (bound is None or events[i] < bound) else None
        records.append(record)
    return records


def window(t, values, lo, hi):
    """Mean of the samples in [lo, hi), or the nearest sample if none falls inside."""
    i, j = bisect.bisect_left(t, lo), bisect.bisect_left(t, hi)
    chosen = [v for v in values[i:j] if not math.isnan(v)]
    if chosen:
        return sum(chosen) / len(chosen)
    k = min(max(i, 0), len(t) - 1)
    return values[k]


def at(t, values, ns):
    """Cumulative counter at ns, linear between samples."""
    i = bisect.bisect_left(t, ns)
    if i <= 0:
        return values[0]
    if i >= len(t):
        return values[-1]
    a, b = values[i - 1], values[i]
    if math.isnan(a) or math.isnan(b):
        return math.nan
    return a + (b - a) * (ns - t[i - 1]) / max(1, t[i] - t[i - 1])


def corr(xs, ys):
    pairs = [(x, y) for x, y in zip(xs, ys) if not (math.isnan(x) or math.isnan(y))]
    if len(pairs) < 3:
        return math.nan
    mx = sum(p[0] for p in pairs) / len(pairs)
    my = sum(p[1] for p in pairs) / len(pairs)
    sxy = sum((x - mx) * (y - my) for x, y in pairs)
    sxx = sum((x - mx) ** 2 for x, _ in pairs)
    syy = sum((y - my) ** 2 for _, y in pairs)
    return sxy / math.sqrt(sxx * syy) if sxx and syy else math.nan


def autocorr(xs, lag):
    return corr(xs[:-lag], xs[lag:]) if len(xs) > lag + 2 else math.nan


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('flip')
    parser.add_argument('apu')
    parser.add_argument('--skip', type=float, default=0, help='seconds of the APU trace to leave out at its start')
    parser.add_argument('--seconds', type=float, default=0, help='only this many seconds after --skip (0: all)')
    parser.add_argument('--csv', help='write the per-frame table here')
    args = parser.parse_args()
    start, stages = read_flip(args.flip)
    apu, threads, cores = read_apu(args.apu)
    t = [x - start for x in apu['t_ns']]  # APU rows on the flip trace's clock
    lo = t[0] + args.skip * 1e9
    hi = lo + args.seconds * 1e9 if args.seconds else t[-1]
    end_stage = 'presented' if stages.get('presented') else 'flipdone'
    records = [r for r in flips(stages) if r[end_stage] is not None]
    if len(records) < 2:
        raise SystemExit('flip trace: no submit -> presented / flipdone chains')
    run = {name: apu[f'{name}.run_ns'] for name in threads}
    cpu = {name: apu[f'{name}.cpu'] for name in threads}
    frames = []
    for prev, cur in zip(records, records[1:]):
        a, b = prev[end_stage], cur[end_stage]
        if a < lo or b > hi or b <= a:
            continue
        s, r, g = cur['submit'], cur['irq'], cur['gpu_ready']
        frame = {'end_ms': b / 1e6, 'ft': (b - a) / 1e6, 'game': (s - prev['submit']) / 1e6,
                 'rec': (r - s) / 1e6 if r is not None else math.nan,
                 'gpu': (g - r) / 1e6 if g is not None and r is not None and g >= r else math.nan}
        for key in ('gfx_mhz', 'gfx_busy', 'socket_mw', 'cpu_mw', 'gfx_mw'):
            frame[key] = window(t, apu[key], a, b)
        frame['gfx_gpu_window'] = window(t, apu['gfx_mhz'], r, g) if r is not None and g is not None and g > r else math.nan
        for name in threads:
            frame[f'{name} ms'] = (at(t, run[name], b) - at(t, run[name], a)) / 1e6
            i, j = bisect.bisect_left(t, a), max(bisect.bisect_left(t, b), bisect.bisect_left(t, a) + 1)
            clocks = []
            for k in range(i, min(j, len(t))):
                c = cpu[name][k]
                if not math.isnan(c):
                    clocks.append(apu[cores[min(int(c) // 2, len(cores) - 1)]][k])
            frame[f'{name} MHz'] = sum(clocks) / len(clocks) if clocks else math.nan
        frames.append(frame)
    if len(frames) < 10:
        raise SystemExit(f'{len(frames)} frames inside the APU trace: do the two files come from the same run?')
    ft = [f['ft'] for f in frames]
    median = statistics.median(ft)
    ordered = sorted(ft)
    print(f'{len(frames)} frames over {(frames[-1]["end_ms"] - frames[0]["end_ms"]) / 1000:.1f} s; frame time median '
          f'{median:.2f} ms, mean {statistics.mean(ft):.2f}, sd {statistics.pstdev(ft):.2f}, p99 '
          f'{ordered[min(len(ft) - 1, len(ft) * 99 // 100)]:.2f}, max {ordered[-1]:.2f}; APU rows '
          f'{sum(lo <= x < hi for x in t)}')
    print('frame-time autocorrelation, lags 1-8: ' + ' '.join(f'{autocorr(ft, k):+.2f}' for k in range(1, 9)))
    swings = [(ft[i] - median) * (ft[i + 1] - median) < 0 and abs(ft[i + 1] - ft[i]) > 2 for i in range(len(ft) - 1)]
    print(f'alternating pairs (one above, one below the median, > 2 ms apart): {sum(swings) / len(swings) * 100:.1f}%')
    keys = ['ft', 'game', 'rec', 'gpu', 'gfx_mhz', 'gfx_gpu_window', 'gfx_busy', 'socket_mw', 'cpu_mw', 'gfx_mw']
    keys += [f'{n} {u}' for n in threads for u in ('ms', 'MHz')]
    bands = [('short third', ordered[len(ft) // 3]), ('middle third', ordered[2 * len(ft) // 3]),
             ('long third', math.inf)]
    groups, low = {}, -math.inf
    for label, high in bands:
        groups[label] = [f for f in frames if low <= f['ft'] < high]
        low = high
    groups['spikes > 1.3x median'] = [f for f in frames if f['ft'] > 1.3 * median]

    def mean(group, key):
        values = [f[key] for f in group if not math.isnan(f[key])]
        return sum(values) / len(values) if values else math.nan

    width = max(len(k) for k in keys) + 2
    print('\nmeans by frame-time band:')
    print(' ' * width + ''.join(f'{label:>22}' for label in groups))
    print(' ' * width + ''.join(f'{len(g):>22}' for g in groups.values()))
    for key in keys:
        print(f'{key:<{width}}' + ''.join(f'{mean(g, key):22.2f}' for g in groups.values()))
    print('\ncorrelation with frame time: metric one frame before / same frame / one frame after')
    for key in keys[1:]:
        values = [f[key] for f in frames]
        before = corr(ft[1:], values[:-1])
        same = corr(ft, values)
        after = corr(ft[:-1], values[1:])
        print(f'{key:<{width}}{before:+8.2f}{same:+8.2f}{after:+8.2f}')
    if args.csv:
        with open(args.csv, 'w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=['end_ms'] + keys)
            writer.writeheader()
            for frame in frames:
                writer.writerow({k: (round(v, 3) if isinstance(v, float) else v) for k, v in frame.items()})
        print(f'\nper-frame table: {args.csv}')


if __name__ == '__main__':
    main()
