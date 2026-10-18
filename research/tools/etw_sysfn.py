"""Top functions and threads of one process in a time window of an xperf dumper export (e.g. what
the System process did during an episode).

    py -3.14 etw_sysfn.py <dump.csv> <process name> <from s> <to s>
"""
import collections, sys
dump, proc, lo, hi = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
fn = collections.Counter(); th = collections.Counter(); n = 0
for line in open(dump, encoding='utf-8', errors='replace'):
    if not line.lstrip().startswith('SampledProfile,'): continue
    p = [x.strip() for x in line.split(',')]
    try: ts = int(p[1]) / 1e6
    except ValueError: continue
    if not (lo <= ts < hi) or not p[2].startswith(proc): continue
    n += 1; fn[p[-3]] += 1; th[(p[3], p[6][:60])] += 1
print(f'{n} samples of {proc} in [{lo},{hi}) s')
for f, c in fn.most_common(18): print(f'  {c * 100 / n:5.1f}%  {f[:110]}')
print('threads:')
for t, c in th.most_common(6): print(f'  {c * 100 / n:5.1f}%  {t}')
