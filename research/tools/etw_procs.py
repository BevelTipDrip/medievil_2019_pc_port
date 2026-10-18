"""CPU per second by process (all samples) from an xperf dumper export: whether anything outside
the game competes during a frame pacing episode.

    py -3.14 etw_procs.py <dump.csv>
"""
import collections, sys
per = collections.defaultdict(collections.Counter); tot = collections.Counter()
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    if not line.lstrip().startswith('SampledProfile,'): continue
    p = [x.strip() for x in line.split(',')]
    try: ts = int(p[1])
    except ValueError: continue
    s = ts // 1000000
    name = p[2].split('(')[0].strip()
    per[s][name] += 1
top = collections.Counter()
for s in per: top.update(per[s])
names = [n for n, _ in top.most_common(8)]
print('cores per second by process (all samples incl. idle)')
print('  s ' + ''.join(f'{n[:16]:>17}' for n in names))
for s in sorted(per)[:14]:
    print(f'{s:3d} ' + ''.join(f'{per[s][n] / 1000:17.2f}' for n in names))
