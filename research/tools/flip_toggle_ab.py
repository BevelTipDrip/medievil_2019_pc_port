"""Frame times of a flip trace (GOW3_FLIP_TRACE) split by a live toggle's state (toggle-ab.ps1 log): per 20 s
phase and per state, the mean frame, 1% / 0.1% lows and the frames over 7 / 9 ms. Frames within 1 s after a
switch are skipped. The trace's "start" row (steady_clock = QueryPerformanceCounter) is turned into wall time
on this machine, so run it on the PC the game ran on, before a reboot.

    py -3.14 flip_toggle_ab.py <trace.csv> <toggle-ab log>
"""
import csv
import datetime
import re
import statistics
import sys
import time

trace, toggles = sys.argv[1], sys.argv[2]
rows = list(csv.DictReader(open(trace)))
start = int(rows[0]['ns']) if rows[0]['stage'] == 'start' else None
if start is None:
    sys.exit('trace has no start row (built before daeebdb)')
offset = time.time_ns() - time.perf_counter_ns()
day = datetime.datetime.fromtimestamp((offset + start) / 1e9).date()
to_wall = lambda ns: (offset + start + ns) / 1e9
presented = [int(r['ns']) for r in rows if r['stage'] == 'presented']
frames = [(to_wall(presented[i]), (presented[i] - presented[i - 1]) / 1e6) for i in range(1, len(presented))]

switches = []
for line in open(toggles, encoding='utf-8-sig', errors='replace'):
    m = re.match(r'(\d+:\d+:\d+\.\d+) toggles .*\((on|off)\)', line.strip())
    if m:
        t = datetime.datetime.combine(day, datetime.datetime.strptime(m.group(1), '%H:%M:%S.%f').time())
        switches.append((t.timestamp(), m.group(2)))
if not switches:
    sys.exit('no switches in the toggle log')


def lows(fts):
    s = sorted(fts, reverse=True)
    n1, n01 = max(1, len(s) // 100), max(1, len(s) // 1000)
    return 1000 / statistics.mean(s[:n1]), 1000 / statistics.mean(s[:n01])


phases = []
for i, (t, state) in enumerate(switches):
    end = switches[i + 1][0] if i + 1 < len(switches) else float('inf')
    fts = [ft for w, ft in frames if t + 1.0 <= w < end and ft < 200]
    if len(fts) > 200:
        phases.append((t, state, fts))
print('phase start   state  frames  mean ms  1% low  0.1% low  >7ms  >9ms')
for t, state, fts in phases:
    l1, l01 = lows(fts)
    print(f'{datetime.datetime.fromtimestamp(t):%H:%M:%S}   {state:5} {len(fts):7}  {statistics.mean(fts):7.2f}  '
          f'{l1:6.1f}  {l01:8.1f}  {sum(f > 7 for f in fts):4}  {sum(f > 9 for f in fts):4}')
for state in ('on', 'off'):
    fts = [f for _, s, ph in phases if s == state for f in ph]
    if fts:
        l1, l01 = lows(fts)
        print(f'{state:3} total: {len(fts)} frames, mean {statistics.mean(fts):.2f} ms, 1% low {l1:.1f}, '
              f'0.1% low {l01:.1f}, >7 ms {sum(f > 7 for f in fts)}, >9 ms {sum(f > 9 for f in fts)}')
