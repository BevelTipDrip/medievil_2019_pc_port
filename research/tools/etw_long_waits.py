"""A thread's waits longer than <min_ms> in a window of an ETW export: when it switched out, the wait reason, how
long until it ran again, and who readied it (with that thread's top sampled functions, i.e. its role).

    py -3.14 etw_long_waits.py <export.csv> <tid> <from_s> <to_s> [min_ms 1.0]

"readied by Idle" or by another process's thread is a timer expiry (the ready event is logged on whatever ran
on that CPU when the timer fired): a run of ~1 ms waits ended that way is a sleep / timed-wait polling loop.
Example (jitter-1, a 13.9 ms frame): py -3.14 etw_long_waits.py j1-18.csv 24168 18.170 18.200 0.8
"""
import bisect
import collections
import sys
import etw_common as E

dump, tid, a, b = sys.argv[1], sys.argv[2], float(sys.argv[3]) * 1e6, float(sys.argv[4]) * 1e6
min_us = float(sys.argv[5]) * 1000 if len(sys.argv) > 5 else 1000
pid = E.detect_pid(dump)
smp = E.samples(dump, pid)
base, _, _ = E.runtime_base(smp)
sym = E.symbolize([x for _, _, s in smp for x in s[:10] if base <= x < base + 0xaca5000], base)
fn_of = collections.defaultdict(collections.Counter)
for _, t, s in smp:
    n = next((sym[x][0] for x in s[:10] if x in sym), None)
    fn_of[t][n or ('guest code' if any(E.GUEST_LO <= x < E.GUEST_HI for x in s[:10]) else 'system')] += 1
outs, ins, readies = [], [], []
for kind, p in E.rows(dump):
    if kind == 'CSwitch':
        if p[9] == tid: outs.append((int(p[1]), p[12], p[13]))
        if p[3] == tid: ins.append(int(p[1]))
    elif kind == 'ReadyThread' and p[5] == tid:
        readies.append((int(p[1]), p[2], p[3]))
ins.sort()
for ts, state, reason in outs:
    if not a <= ts <= b: continue
    i = bisect.bisect_right(ins, ts)
    if i >= len(ins) or ins[i] - ts < min_us: continue
    r = [x for x in readies if ts <= x[0] <= ins[i]]
    who = (f'readied by tid {r[-1][2]} ({r[-1][1]}) [{", ".join(n[-45:] for n, _ in fn_of[r[-1][2]].most_common(2)) or "no samples"}]'
           if r else 'no ready event')
    print(f'{ts / 1e6:.6f} out {state} {reason}: waited {(ins[i] - ts) / 1000:.2f} ms; {who}')
