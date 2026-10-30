"""Per-frame stage B sections (GOW3_SECTIONS=1 with GOW3_SLOW_FRAMES) from a run log: for heavy frames, the
mean of every section and the frames where "render state" is a spike, with what grew in them.

    py -3.14 sections_frames.py <log> [min_draws 900] [spike_ms 0.5]
"""
import re
import statistics
import sys

log = sys.argv[1]
min_draws = int(sys.argv[2]) if len(sys.argv) > 2 else 900
spike_ms = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
frames = []
head = None
for line in open(log, encoding='utf-8', errors='replace'):
    if line.startswith(('Slow frame', 'Frame sample')):
        m = re.match(r'(\S+ \S+) ([\d.]+) ms: (\d+) draws', line)
        head = dict(kind=m.group(1), ms=float(m.group(2)), draws=int(m.group(3)),
                    texset=int(re.search(r'(\d+) texture sets', line).group(1)),
                    rstate=int(re.search(r'(\d+) render states', line).group(1)),
                    lookups=int(re.search(r'(\d+) image lookups', line).group(1)),
                    gen=int(m2.group(1)) if (m2 := re.search(r'\((\d+) generation only\)', line)) else 0)
    elif line.startswith('  stage B ms:') and head:
        head['sec'] = {k.strip(): float(v) for k, v in re.findall(r'\s*([A-Za-z][A-Za-z_ ]*?) (\d+\.\d+)', line[len('  stage B ms:'):])}
        frames.append(head)
        head = None
heavy = [f for f in frames if f['draws'] >= min_draws and 'sec' in f]
print(f'{len(frames)} frames with sections, {len(heavy)} with >= {min_draws} draws')
if not heavy:
    sys.exit()
keys = list(heavy[0]['sec'])
for kind in ('Frame sample', 'Slow frame'):
    grp = [f for f in heavy if f['kind'] == kind]
    if not grp:
        continue
    print(f'\n{kind}: {len(grp)} frames, {statistics.mean(f["ms"] for f in grp):.2f} ms, '
          f'{statistics.mean(f["draws"] for f in grp):.0f} draws, misses/frame: texture sets '
          f'{statistics.mean(f["texset"] for f in grp):.0f}, render states {statistics.mean(f["rstate"] for f in grp):.0f}, '
          f'image lookups {statistics.mean(f["lookups"] for f in grp):.0f} '
          f'({statistics.mean(f["gen"] for f in grp):.0f} generation only)')
    print('  mean ms: ' + ', '.join(f'{k} {statistics.mean(f["sec"].get(k, 0) for f in grp):.3f}' for k in keys))
spikes = [f for f in heavy if f['sec'].get('render state', 0) >= spike_ms]
print(f'\nrender state >= {spike_ms} ms: {len(spikes)} of {len(heavy)} heavy frames')
for f in sorted(spikes, key=lambda f: -f['sec']['render state'])[:12]:
    s = f['sec']
    print(f"  {f['kind']:12} {f['ms']:6.2f} ms {f['draws']:5} draws: render state {s.get('render state', 0):.2f}, rs_target "
          f"{s.get('rs_target', 0):.2f}, find_image {s.get('find_image', 0):.2f}, cache_lock {s.get('cache_lock', 0):.2f}; "
          f"lookups {f['lookups']} ({f['gen']} gen only), render-state misses {f['rstate']}")
