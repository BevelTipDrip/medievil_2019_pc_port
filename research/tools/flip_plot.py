"""Frame-time plot of a flip trace range (Pillow): one dot per frame (present to present), the frames over 1.15x
the local median in red, marked stretches shaded.

    py -3.14 flip_plot.py <flip-trace.csv> <from_s> <to_s> <out.png> [label:a-b ...]

Times are flip-trace seconds. The PNG is for looking at (the user reads it beside RTSS).
"""
import csv, sys, statistics
from PIL import Image, ImageDraw, ImageFont
path, t0, t1, out = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
marks = [(m.split(':')[0], *map(float, m.split(':')[1].split('-'))) for m in sys.argv[5:]]
pres = []
with open(path, newline='') as f:
    r = csv.reader(f); next(r)
    for row in r:
        if row[0] == 'presented':
            t = int(row[2]) / 1e9
            if t0 - 1 <= t <= t1 + 1: pres.append(t)
pres.sort()
ft = [(pres[i], (pres[i] - pres[i-1]) * 1000) for i in range(1, len(pres)) if t0 <= pres[i] <= t1]
W, H, L, R, T, Bm = 1800, 700, 70, 20, 40, 50
ymin, ymax = 3.0, 10.0
img = Image.new('RGB', (W, H), (255, 255, 255)); d = ImageDraw.Draw(img)
font = ImageFont.load_default()
X = lambda t: L + (t - t0) / (t1 - t0) * (W - L - R)
Y = lambda v: T + (1 - (min(max(v, ymin), ymax) - ymin) / (ymax - ymin)) * (H - T - Bm)
for name, a, b in marks:
    d.rectangle([X(a), T, X(b), H - Bm], fill=(225, 238, 255)); d.text((X(a) + 4, T + 4), name, fill=(40, 60, 160), font=font)
for v in (4.0, 4.22, 5.0, 6.0, 7.0, 8.0, 10.0):
    d.line([L, Y(v), W - R, Y(v)], fill=(225, 225, 225) if v != 4.22 else (150, 200, 150))
    d.text((5, Y(v) - 6), f'{v:.2f} ms' if v == 4.22 else f'{v:.0f} ms', fill=(80, 80, 80), font=font)
step = 5 if t1 - t0 > 40 else 1
for s in range(int(t0) - int(t0) % step + step, int(t1) + 1, step):
    d.line([X(s), H - Bm, X(s), H - Bm + 5], fill=(0, 0, 0)); d.text((X(s) - 10, H - Bm + 8), f'{s}', fill=(0, 0, 0), font=font)
d.text((L, H - 18), 'flip trace time (s); dots: frame time (present to present); red: over 1.15x the local median', fill=(0, 0, 0), font=font)
vals = [v for _, v in ft]
for i, (t, v) in enumerate(ft):
    lo = max(0, i - 120); seg = sorted(vals[lo:i + 120]); med = seg[len(seg) // 2]
    spike = v > 1.15 * med and v - med > 0.6
    x, y = X(t), Y(v)
    d.ellipse([x - 1.5, y - 1.5, x + 1.5, y + 1.5], fill=(220, 30, 30) if spike else (60, 60, 60))
d.text((L, 10), f'{sys.argv[1].split(chr(92))[-1]}  {t0:.0f}-{t1:.0f} s, {len(ft)} frames', fill=(0, 0, 0), font=font)
img.save(out); print(out)
