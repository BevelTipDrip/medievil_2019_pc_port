# Run inside qrenderdoc:  qrenderdoc.exe --python trigger_capture.py
# Triggers a RenderDoc capture in a game running under renderdoccmd elsewhere (the Steam Deck,
# which has no capture key): its target control port reached through an SSH tunnel
# (ssh -N -L 38920:localhost:38920 deck). RD_HOST (default localhost), RD_FRAMES (default 1),
# RD_OUT: the result (one line per capture: frame number and the path on the game's machine,
# copied over SSH afterwards). V1 (2026-11-06).
import os
import time
import traceback

import renderdoc as rd

lines = []
try:
    host = os.environ.get('RD_HOST', 'localhost')
    ident = rd.EnumerateRemoteTargets(host, 0)
    if not ident:
        raise RuntimeError(f'no RenderDoc target at {host} (game not under renderdoccmd, or no tunnel)')
    control = rd.CreateTargetControl(host, ident, 'claude-trigger', True)
    if control is None:
        raise RuntimeError(f'cannot connect to target {ident} at {host}')
    lines.append(f'target {control.GetTarget()} pid {control.GetPID()} api {control.GetAPI()}')
    frames = int(os.environ.get('RD_FRAMES', '1'))
    control.TriggerCapture(frames)
    deadline = time.time() + 120
    got = 0
    while got < frames and time.time() < deadline:
        message = control.ReceiveMessage(None)
        if message.type == rd.TargetControlMessageType.NewCapture:
            got += 1
            lines.append(f'capture frame {message.newCapture.frameNumber} {message.newCapture.path}')
        elif message.type == rd.TargetControlMessageType.Disconnected:
            lines.append('disconnected')
            break
        time.sleep(0.1)
    if got < frames:
        lines.append(f'only {got} of {frames} captures within 120 s')
    control.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
