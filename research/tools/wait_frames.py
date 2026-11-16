"""Slow game frames against the guest's long waits: a flip trace (GOW3_FLIP_TRACE, with its `wall` row) and a wait
trace (GOW3_WAIT_TRACE), lined up through their wall-clock rows.

    py -3.14 wait_frames.py <flip-trace.csv> <wait-trace.csv> <from_s> <to_s> [thread main] [slow_ms 7]

Times are flip-trace seconds. Prints:
- per guest thread: long waits per second and the time they cover;
- every slow game frame (submit to submit over slow_ms): the waits of `thread` (default `main`: UE4's game thread
  runs on the process's main thread) that overlap it, by kind and site, requested / actual ms;
- which of its sites cover slow frames far more than normal frames (wait ms per frame, slow vs normal);
- periodic long waits (3 ms or more) by thread and site, with their spacing (e.g. every 1.36 s).
A wait trace is large (MediEvil: ~8,600 rows a second at 0.5 ms, all threads): the file is read once.
"""
import bisect
import collections
import csv
import statistics as st
import sys

flip, waits, a, b = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
want = sys.argv[5] if len(sys.argv) > 5 and sys.argv[5] != '-' else 'main'
slow_ms = float(sys.argv[6]) if len(sys.argv) > 6 else 7.0

rows = list(csv.reader(open(flip, newline='')))
wall = next(r for r in rows[1:4] if r[0] == 'wall')
f_wall, f_at = int(wall[1]), int(wall[2])
sub = {}
for st_, arg, ns in rows[1:]:
    if st_ == 'submit':
        sub.setdefault(int(arg), int(ns) / 1e9)
subs = sorted(sub.values())
frames = [(subs[i - 1], subs[i]) for i in range(1, len(subs)) if a <= subs[i] < b]

W = []   # (start, end, thread, kind, site, requested ms, actual ms)
w_wall = w_mono = None
for r in csv.reader(open(waits, newline='')):
    if r[0] == 'wall':
        w_wall, w_mono = int(r[1]), int(r[2])
    elif r[0] == 'w' and w_wall is not None:
        end_t = (w_wall + int(r[1]) - w_mono - f_wall + f_at) / 1e9
        actual = int(r[7]) / 1e9
        if a - 1 <= end_t <= b + 1:
            W.append((end_t - actual, end_t, r[3], r[4], r[5], int(r[6]) / 1e6, actual * 1e3))
print(f'{len(frames)} game frames, {len(W)} long waits in {a}-{b} s')

per = collections.defaultdict(lambda: [0, 0.0])
for s, e, th, kind, site, req, act in W:
    per[th][0] += 1
    per[th][1] += act
print('\nlong waits per thread (per second, ms of wait per second):')
for th, (n, ms) in sorted(per.items(), key=lambda x: -x[1][1])[:14]:
    print(f'  {th:32s} {n / (b - a):7.1f}/s  {ms / (b - a):7.1f} ms/s')

def overlap(s, e, x, y):
    return max(0.0, min(e, y) - max(s, x))

# One thread's waits never overlap: sorted by start they are sorted by end too.
mine = sorted(w for w in W if w[2] == want)
ends = [w[1] for w in mine]
def waits_in(x, y):
    out = []
    for i in range(bisect.bisect_right(ends, x), len(mine)):
        if mine[i][0] >= y:
            break
        out.append(mine[i])
    return out

med = st.median([(y - x) * 1e3 for x, y in frames])
slow = [(x, y) for x, y in frames if (y - x) * 1e3 > slow_ms]
normal = [(x, y) for x, y in frames if (y - x) * 1e3 <= med * 1.15]
print(f'\nslow game frames (> {slow_ms} ms; median {med:.2f} ms): {len(slow)}; waits of {want} in each:')
for x, y in slow:
    ws = waits_in(x, y)
    print(f'  {y:9.4f}  game frame {(y - x) * 1e3:5.2f} ms: '
          + '; '.join(f'{k} {si} {req:.2f}/{act:.2f}' for _, _, _, k, si, req, act in ws))

def site_time(fr):
    c = collections.Counter()
    for x, y in fr:
        for s, e, th, k, si, *_ in waits_in(x, y):
            c[(k, si)] += overlap(s, e, x, y) * 1e3
    return c
cs, cn = site_time(slow), site_time(normal)
print(f'\nsites of {want}: wait ms per frame, slow frames vs normal frames')
for (k, si), v in sorted(cs.items(), key=lambda x: -x[1])[:10]:
    print(f'  {k:15s} {si:20s} slow {v / max(len(slow), 1):6.2f}  normal {cn.get((k, si), 0) / max(len(normal), 1):6.2f}')

print('\nperiodic long waits (>= 3 ms) by thread and site: count, spacing in s')
grp = collections.defaultdict(list)
for s, e, th, k, si, req, act in W:
    if act >= 3:
        grp[(th, k, si)].append(e)
for key, ts in sorted(grp.items(), key=lambda x: -len(x[1]))[:12]:
    ts.sort()
    gaps = [round(ts[i] - ts[i - 1], 2) for i in range(1, len(ts))]
    print(f'  {key[0]:28s} {key[1]:14s} {key[2]:20s} {len(ts):5d}; gaps {gaps[:10]}')
