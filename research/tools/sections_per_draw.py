"""Stage B microseconds per draw in heavy frames (GOW3_SECTIONS=1 with GOW3_SLOW_FRAMES): for comparing builds
at the same spot (A-B-A). Per frame: a section's ms / draws; prints median and mean over frames with >= min draws.

    py -3.14 sections_per_draw.py <min_draws> <log> [<log> ...]
"""
import re
import statistics
import sys

min_draws = int(sys.argv[1])
keys = ('packet', 'draw', 'bind', 'textures', 'descriptors', 'vertex', 'ObtainBuffer', 'begin render')
for log in sys.argv[2:]:
    rows = []
    draws = None
    for line in open(log, encoding='utf-8', errors='replace'):
        if line.startswith(('Slow frame', 'Frame sample')):
            draws = int(re.search(r': (\d+) draws', line).group(1))
        elif line.startswith('  stage B ms:') and draws:
            sec = {k.strip(): float(v) for k, v in re.findall(r'\s*([A-Za-z][A-Za-z_ ]*?) (\d+\.\d+)', line[13:])}
            if draws >= min_draws:
                rows.append((draws, sec))
            draws = None
    if not rows:
        print(f'{log}: no frames with >= {min_draws} draws')
        continue
    print(f'{log.split("/")[-1]}: {len(rows)} frames, {statistics.mean(d for d, _ in rows):.0f} draws')
    for k in keys:
        per = [1000 * s.get(k, 0) / d for d, s in rows]
        print(f'   {k:13} us/draw median {statistics.median(per):.3f}  mean {statistics.mean(per):.3f}')
