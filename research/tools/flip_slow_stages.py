"""Which pipeline stage makes a frame slower than its neighbours, from a flip trace (GOW3_FLIP_TRACE): for every
frame over <ratio> x the median of the frames within +-0.5 s, each stage's excess over its own local median.
Stages: game (submit to submit: the game's own frame), A (submit -> irq: stage A reaches the flip), B+rec
(irq -> queued: draws recorded), GPU (queued -> gpu_ready), present (present_start -> presented).

    py -3.14 flip_slow_stages.py <trace.csv> <from_s>-<to_s> [ratio 1.25]
"""
import collections, csv, statistics, sys

path = sys.argv[1]
a, b = map(float, sys.argv[2].split('-'))
ratio = float(sys.argv[3]) if len(sys.argv) > 3 else 1.25
ev = collections.defaultdict(dict)
for r in csv.DictReader(open(path)):
    ev[int(r['flip_arg'])].setdefault(r['stage'], int(r['ns']))
need = ('submit', 'irq', 'queued', 'gpu_ready', 'present_start', 'presented')
args = sorted(x for x in ev if all(k in ev[x] for k in need))
rows = []
for i in range(1, len(args)):
    e, p = ev[args[i]], ev[args[i - 1]]
    t = e['presented'] / 1e9
    if not (a - 1 <= t <= b + 1):
        continue
    ms = lambda x, y: (e[y] - e[x]) / 1e6
    rows.append(dict(t=t, ft=(e['presented'] - p['presented']) / 1e6, game=(e['submit'] - p['submit']) / 1e6,
                     A=ms('submit', 'irq'), Brec=ms('irq', 'queued'), GPU=ms('queued', 'gpu_ready'),
                     present=ms('present_start', 'presented')))
keys = ('game', 'A', 'Brec', 'GPU', 'present')
blame = collections.Counter(); excess = collections.defaultdict(list); slow = 0
times = [r['t'] for r in rows]
import bisect
for i, r in enumerate(rows):
    if not (a <= r['t'] <= b):
        continue
    lo, hi = bisect.bisect_left(times, r['t'] - 0.5), bisect.bisect_right(times, r['t'] + 0.5)
    near = rows[lo:hi]
    med = {k: statistics.median(x[k] for x in near) for k in keys + ('ft',)}
    if r['ft'] < ratio * med['ft']:
        continue
    slow += 1
    ex = {k: r[k] - med[k] for k in keys}
    top = max(ex, key=ex.get)
    blame[top] += 1
    for k in keys:
        excess[k].append(ex[k])
n = sum(1 for r in rows if a <= r['t'] <= b)
print(f'{n} frames in {a:g}-{b:g} s; {slow} over {ratio}x the local median frame')
if slow:
    print('largest excess stage: ' + ', '.join(f'{k} {v}' for k, v in blame.most_common()))
    print('mean excess per stage (ms): ' + ', '.join(f'{k} {statistics.mean(excess[k]):+.2f}' for k in keys))
