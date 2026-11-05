"""Per-frame stage B sections (GOW3_SECTIONS=1 with GOW3_SLOW_FRAMES) from a run log: for heavy frames, the
mean of every section and the frames where "render state" is a spike, with what grew in them; with the task_*
sections, "tasks" split by kind; with more than one toggle state in the run (deck.sh ab), the sampled heavy frames'
main sections by state (frames in the stats window of a switch left out).

    py -3.14 sections_frames.py <log> [min_draws 900] [spike_ms 0.5]
"""
import re
import statistics
import sys

# Top-level ordered-task sections (they do not nest in each other); "tasks" minus their sum is the rest.
TASK_KINDS = ('task_eop', 'task_eos', 'task_wdata', 'task_dma', 'task_flip', 'task_submit', 'task_retire',
              'task_signals', 'task_occl')
log = sys.argv[1]
min_draws = int(sys.argv[2]) if len(sys.argv) > 2 else 900
spike_ms = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
frames = []
head = None
mask, word2, switching = 0, 0, False
for line in open(log, encoding='utf-8', errors='replace'):
    if m := re.match(r'Runtime: disabled optimizations (mask|second word)=(\d+)', line):
        if m.group(1) == 'mask':
            mask = int(m.group(2))
        else:
            word2 = int(m.group(2))
        switching = True
        continue
    if line.startswith('Frame stats:'):
        switching = False
    if line.startswith(('Slow frame', 'Frame sample')):
        m = re.match(r'(\S+ \S+) ([\d.]+) ms: (\d+) draws', line)
        head = dict(kind=m.group(1), ms=float(m.group(2)), draws=int(m.group(3)),
                    texset=int(re.search(r'(\d+) texture sets', line).group(1)),
                    rstate=int(re.search(r'(\d+) render states', line).group(1)),
                    lookups=int(re.search(r'(\d+) image lookups', line).group(1)),
                    gen=int(m2.group(1)) if (m2 := re.search(r'\((\d+) generation only\)', line)) else 0,
                    state=f'{mask} {word2}', mixed=switching)
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
    if TASK_KINDS[0] in keys:
        # The ordered tasks by kind (builds with the task_* sections): what "tasks" is made of.
        part = {k: statistics.mean(f['sec'].get(k, 0) for f in grp) for k in TASK_KINDS}
        tasks = statistics.mean(f['sec'].get('tasks', 0) for f in grp)
        print(f'  tasks {tasks:.3f} ms: ' + ', '.join(f'{k[5:]} {v:.3f}' for k, v in part.items()) +
              f', not attributed {tasks - sum(part.values()):.3f}')
spikes = [f for f in heavy if f['sec'].get('render state', 0) >= spike_ms]
print(f'\nrender state >= {spike_ms} ms: {len(spikes)} of {len(heavy)} heavy frames')
for f in sorted(spikes, key=lambda f: -f['sec']['render state'])[:12]:
    s = f['sec']
    print(f"  {f['kind']:12} {f['ms']:6.2f} ms {f['draws']:5} draws: render state {s.get('render state', 0):.2f}, rs_target "
          f"{s.get('rs_target', 0):.2f}, find_image {s.get('find_image', 0):.2f}, cache_lock {s.get('cache_lock', 0):.2f}; "
          f"lookups {f['lookups']} ({f['gen']} gen only), render-state misses {f['rstate']}")
states = sorted({f['state'] for f in heavy})
if len(states) > 1:
    cols = ('packet', 'draw', 'textures', 'texset', 'texset emit', 'texslow', 'vertex', 'ObtainBuffer',
            'obt_resident', 'obt_sync', 'obt_upload', 'clamp', 'tasks')
    print('\nFrame samples by toggle state (heavy, switch windows left out), mean ms:')
    print(f'{"state":<14}{"n":>4}{"frame":>8}{"draws":>7}' + ''.join(f' {c:>12}' for c in cols))
    for state in states:
        grp = [f for f in heavy if f['state'] == state and not f['mixed'] and f['kind'] == 'Frame sample']
        if not grp:
            continue
        print(f'{state:<14}{len(grp):>4}{statistics.mean(f["ms"] for f in grp):8.2f}'
              f'{statistics.mean(f["draws"] for f in grp):7.0f}' +
              ''.join(f' {statistics.mean(f["sec"].get(c, 0) for f in grp):12.3f}' for c in cols))
