"""Colorimeter readings of test patches shown by the game or by a desktop window (research/docs/plan-meter-measurement.md).

The game side is gow3_patterns.h (GOW3_PATTERN_SERVER=1 in extra-env.txt): this script asks it for a patch, waits for
its "OK" (the patch has been presented), lets the display settle as HCFR does, then reads the meter with ArgyllCMS
spotread (one-shot, -O) and writes a CSV. --target desktop shows the same patches in a full-screen window instead (the
reference outside the game: same patches, same timing, same meter settings).

Sequences:
  sdr-grey       raw SDR codes 0-100% in 10% steps (full range), HCFR's levels
  sdr-nearblack  raw SDR codes 0-4% (and 5-10% in 1% steps)
  hdr-pq         raw HDR10 PQ levels in nits (0 to 1000)
  game-grey      scene-linear greys through the game's grading (target game only)
  custom         --values a,b,c (raw: 0-255 codes for SDR / nits for HDR; game: scene-linear)
Examples:
  py -3 measure_patches.py --target game --sequence sdr-grey
  py -3 measure_patches.py --target desktop --sequence sdr-grey
  py -3 measure_patches.py --target game --sequence game-grey --dry-run     (no meter: patches only)
Python 3.8+, standard library only (tkinter for the desktop window).
"""
import argparse
import csv
import datetime
import os
import re
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
DEFAULT_SPOTREAD = os.path.abspath(os.path.join(ROOT, '..', 'tools', 'argyll', 'Argyll_V3.5.0', 'bin', 'spotread.exe'))
FRAME_END = b'\x02\r'

SEQUENCES = {
    # HCFR: floor(p * 255 + 0.5) for 0-100% in 10% steps (full range).
    'sdr-grey': [0, 26, 51, 77, 102, 128, 153, 178, 204, 229, 255],
    'sdr-nearblack': [0, 3, 5, 8, 10, 13, 15, 18, 20, 23, 26],
    'hdr-pq': [0, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 20, 50, 100, 150, 203, 300, 400, 500, 600, 800, 1000],
    'game-grey': [0, 0.001, 0.003, 0.01, 0.02, 0.05, 0.1, 0.18, 0.3, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0],
}


def pq_code(nits):
    """Normalized ST 2084 code value for an absolute luminance."""
    y = max(nits, 0.0) / 10000.0
    yp = y ** 0.1593017578125
    return ((0.8359375 + 18.8515625 * yp) / (1.0 + 18.6875 * yp)) ** 78.84375


class Game:
    def __init__(self, port, black=True, neutral=False):
        self.black = black
        self.neutral = neutral
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.buffer = b''

    def ask(self, message):
        self.sock.sendall(message.encode('ascii') + FRAME_END)
        while FRAME_END not in self.buffer:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError('the game closed the connection')
            self.buffer += chunk
        reply, self.buffer = self.buffer.split(FRAME_END, 1)
        return reply.decode('ascii', 'replace')

    def info(self):
        reply = self.ask('GOW3:INFO')
        return dict(item.split('=', 1) for item in reply[3:].split(';') if '=' in item)

    def show(self, kind, values, area):
        triple = ','.join('%.6f' % v for v in values)
        flags = (';black' if self.black else '') + (';neutral' if kind == 'game' and self.neutral else '')
        reply = self.ask('GOW3:%s;%s;%.4f%s' % ('RAW' if kind == 'raw' else 'GAME', triple, area, flags))
        if not reply.startswith('OK'):
            raise RuntimeError('the game answered ' + reply)

    def close(self):
        try:
            self.ask('GOW3:OFF')
        finally:
            self.sock.close()


class Desktop:
    """A full-screen black window with a centred patch (SDR codes; the desktop's colour pipeline)."""

    def __init__(self, area):
        import tkinter
        self.area = area
        self.ready = threading.Event()
        self.pending = None
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._run, args=(tkinter,), daemon=True)
        self.thread.start()
        self.ready.wait(10)

    def _run(self, tkinter):
        root = tkinter.Tk()
        root.attributes('-fullscreen', True)
        root.configure(background='black', cursor='none')
        canvas = tkinter.Canvas(root, background='black', highlightthickness=0)
        canvas.pack(fill='both', expand=True)
        root.update()
        w, h = root.winfo_width(), root.winfo_height()
        side = self.area ** 0.5
        pw, ph = int(round(w * side)), int(round(h * side))
        rect = canvas.create_rectangle((w - pw) // 2, (h - ph) // 2, (w + pw) // 2, (h + ph) // 2, fill='black',
                                       outline='')
        self.root = root

        def poll():
            with self.lock:
                job = self.pending
                self.pending = None
            if job:
                colour, done = job
                canvas.itemconfigure(rect, fill=colour)
                root.update_idletasks()
                root.after(50, done.set)  # the next frames show it
            root.after(5, poll)
        root.after(5, poll)
        self.ready.set()
        root.mainloop()

    def show(self, kind, values, area):
        codes = [max(0, min(255, int(round(v * 255)))) for v in values]
        done = threading.Event()
        with self.lock:
            self.pending = ('#%02x%02x%02x' % tuple(codes), done)
        done.wait(5)

    def close(self):
        try:
            self.root.after(0, self.root.destroy)
        except Exception:
            pass


def read_meter(spotread, extra):
    """One spotread reading: (X, Y, Z) in cd/m2."""
    cmd = [spotread, '-e', '-x', '-O'] + extra
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60,
                            stdin=subprocess.DEVNULL)
    text = result.stdout.decode('utf-8', 'replace')
    match = re.search(r'Result is XYZ:\s*([-\d.eE+]+)\s+([-\d.eE+]+)\s+([-\d.eE+]+)', text)
    if not match:
        raise RuntimeError('spotread gave no reading:\n' + text[-800:])
    return tuple(float(v) for v in match.groups())


def readings_for(y):
    """HCFR's low-light averaging ("Adapt"): more readings near black."""
    return 5 if y < 1 else 4 if y < 2 else 3 if y < 5 else 2 if y < 10 else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--target', choices=['game', 'desktop'], required=True)
    ap.add_argument('--sequence', choices=sorted(SEQUENCES) + ['custom'], required=True)
    ap.add_argument('--values', help='custom: comma-separated values')
    ap.add_argument('--kind', choices=['raw', 'game'], help='patch kind (default: game for game-grey, else raw)')
    ap.add_argument('--area', type=float, default=0.1, help='share of the screen (HCFR: 0.1)')
    ap.add_argument('--settle', type=float, default=0.4, help='seconds between the patch and the reading')
    ap.add_argument('--port', type=int, default=21977)
    ap.add_argument('--spotread', default=DEFAULT_SPOTREAD)
    ap.add_argument('--display-type', help='spotread -y (e.g. the letter for a refresh / OLED type; see spotread -?)')
    ap.add_argument('--ccmx', help='spotread -X correction (.ccmx / .ccss); none by default (relative comparisons)')
    ap.add_argument('--label', default='', help='added to the CSV name')
    ap.add_argument('--neutral-brightness', action='store_true',
                    help="game patches: the game's Brightness at its neutral value (power 1, as the HDR pass uses)")
    ap.add_argument('--keep-scene', action='store_true',
                    help='game: leave the rest of the frame as it is (default: black around the patch)')
    ap.add_argument('--dry-run', action='store_true', help='show the patches only (no meter), 2 s each')
    ap.add_argument('--start-delay', type=float, default=0.0,
                    help='seconds to wait before the first patch (time to switch back to the game window)')
    args = ap.parse_args()

    values = [float(v) for v in args.values.split(',')] if args.sequence == 'custom' else SEQUENCES[args.sequence]
    kind = args.kind or ('game' if args.sequence == 'game-grey' else 'raw')
    if args.target == 'desktop' and kind != 'raw':
        sys.exit('the desktop shows raw SDR codes only')
    extra = []
    if args.display_type:
        extra += ['-y', args.display_type]
    if args.ccmx:
        extra += ['-X', args.ccmx]
    if not args.dry_run and not os.path.exists(args.spotread):
        sys.exit('spotread not found: ' + args.spotread)

    display = Game(args.port, not args.keep_scene, args.neutral_brightness) if args.target == 'game'         else Desktop(args.area)
    hdr = False
    if args.target == 'game':
        info = display.info()
        hdr = info.get('hdr') == '1'
        print('Game: %s, %sx%s' % ('HDR10' if hdr else 'SDR', info.get('w'), info.get('h')))
    if kind == 'raw' and args.sequence in ('sdr-grey', 'sdr-nearblack') and hdr:
        print('Note: the game is in HDR10: SDR codes are sent as PQ codes')
    if kind == 'raw' and args.sequence == 'hdr-pq' and not hdr:
        sys.exit('hdr-pq needs the game in HDR10')

    if args.start_delay > 0:
        print('Starting in %g s: switch to the game window now' % args.start_delay)
        time.sleep(args.start_delay)
    out_dir = os.path.join(ROOT, 'research-data', 'meter')
    os.makedirs(out_dir, exist_ok=True)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    name = '%s-%s-%s-%s%s%s.csv' % (stamp, args.target, args.sequence, 'hdr' if hdr else 'sdr',
                                     '-neutral' if args.neutral_brightness and kind == 'game' else '',
                                     '-' + args.label if args.label else '')
    path = os.path.join(out_dir, name)
    print('Readings to', path if not args.dry_run else '(dry run)')
    rows = []
    try:
        for value in values:
            if kind == 'raw':
                code = pq_code(value) if args.sequence == 'hdr-pq' or (hdr and args.sequence == 'custom') \
                    else value / 255.0
                shown = [code] * 3
            else:
                shown = [value] * 3
            display.show(kind, shown, args.area)
            if args.dry_run:
                print('  %-10g shown' % value)
                time.sleep(2.0)
                continue
            time.sleep(args.settle)
            first = read_meter(args.spotread, extra)
            samples = [first]
            for _ in range(readings_for(first[1]) - 1):
                samples.append(read_meter(args.spotread, extra))
            x, y, z = (sum(s[i] for s in samples) / len(samples) for i in range(3))
            total = x + y + z
            cx, cy = (x / total, y / total) if total > 0 else (0.0, 0.0)
            rows.append([args.target, kind, args.sequence, value, '%.6f' % shown[0], '%.5f' % x, '%.5f' % y,
                         '%.5f' % z, '%.4f' % cx, '%.4f' % cy, len(samples)])
            print('  %-10g Y %9.4f cd/m2  xy %.4f %.4f  (%d)' % (value, y, cx, cy, len(samples)))
    finally:
        display.close()
    if rows:
        with open(path, 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['target', 'kind', 'sequence', 'value', 'signal', 'X', 'Y', 'Z', 'x', 'y', 'readings'])
            w.writerows(rows)
        print('Wrote', path)


if __name__ == '__main__':
    main()
