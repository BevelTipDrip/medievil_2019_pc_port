"""Reflection capture cubemap arrays from cube_array_dump.py, side by side: one row per capture slot
(its 6 faces, mip 2 tonemapped), the broken frame left and the clean one right, and per slot the mean
colour of each and their difference.
python cube_array_sheet.py <a.bin> <b.bin> <size> <out.png> [layers_used]. V1 (Hall of Heroes light,
2026-11-09)."""
import struct
import sys

from PIL import Image, ImageDraw


def load(path, size):
    data = open(path, 'rb').read()
    per = size * size * 8
    layers = []
    for i in range(len(data) // per):
        vals = struct.unpack_from(f'<{size * size * 4}e', data, i * per)
        layers.append(vals)
    return layers


def tone(v):
    v = max(0.0, v)
    return int(255 * (v / (1 + v)) ** (1 / 2.2))


def face(vals, size):
    im = Image.new('RGB', (size, size))
    im.putdata([(tone(vals[i]), tone(vals[i + 1]), tone(vals[i + 2])) for i in range(0, len(vals), 4)])
    return im


def mean(vals):
    n = len(vals) // 4
    return tuple(sum(vals[c::4]) / n for c in range(3))


a_path, b_path, size, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
a, b = load(a_path, size), load(b_path, size)
used = int(sys.argv[5]) if len(sys.argv) > 5 else len(a)
slots = used // 6
cell = size + 2
sheet = Image.new('RGB', (cell * 12 + 260, slots * cell), 'black')
d = ImageDraw.Draw(sheet)
report = []
for s in range(slots):
    ma = [mean(a[s * 6 + f]) for f in range(6)]
    mb = [mean(b[s * 6 + f]) for f in range(6)]
    for f in range(6):
        sheet.paste(face(a[s * 6 + f], size), (f * cell, s * cell))
        sheet.paste(face(b[s * 6 + f], size), (6 * cell + 20 + f * cell, s * cell))
    avg_a = tuple(sum(m[c] for m in ma) / 6 for c in range(3))
    avg_b = tuple(sum(m[c] for m in mb) / 6 for c in range(3))
    diff = sum(abs(avg_a[c] - avg_b[c]) for c in range(3))
    line = f'slot {s:2d} broken {avg_a[0]:.3f} {avg_a[1]:.3f} {avg_a[2]:.3f}  clean {avg_b[0]:.3f} {avg_b[1]:.3f} {avg_b[2]:.3f}  diff {diff:.3f}'
    report.append(line)
    d.text((12 * cell + 30, s * cell + 4), f'{s:2d} d={diff:.2f}', fill='yellow' if diff > 0.05 else 'gray')
sheet.save(out)
print('\n'.join(report))
