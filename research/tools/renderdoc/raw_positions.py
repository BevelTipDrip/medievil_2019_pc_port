# Run inside qrenderdoc:  qrenderdoc.exe --python raw_positions.py
# RD_CAPTURE, RD_OUT, RD_EID: the draw's vertex buffer 0 as float3 positions (stride from the
# binding), for the vertices its index buffer uses: bounds, spread, NaN / huge counts, and the
# first values. V1 (2026-10-18).
import math
import os
import struct
import traceback

import renderdoc as rd

lines = []
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    eid = int(os.environ['RD_EID'])
    controller.SetFrameEvent(eid, True)
    pipe = controller.GetPipelineState()
    vb = pipe.GetVBuffers()[0]
    ib = pipe.GetIBuffer()
    found = []

    def walk(list_):
        for a in list_:
            if a.eventId == eid:
                found.append(a)
            walk(a.children)
    walk(controller.GetRootActions())
    action = found[0] if found else None
    num = action.numIndices if action else 0
    first = action.indexOffset if action else 0
    width = ib.byteStride or 2
    idx = controller.GetBufferData(ib.resourceId, ib.byteOffset + first * width, num * width)
    indices = struct.unpack(f'<{num}{"H" if width == 2 else "I"}', idx[:num * width])
    base = action.baseVertex if action else 0
    used = sorted({i + base for i in indices})
    lines.append(f'eid {eid}: {num} indices, {len(used)} vertices, index range {used[0]}..{used[-1]}, '
                 f'vb offset {vb.byteOffset:#x} stride {vb.byteStride}')
    data = controller.GetBufferData(vb.resourceId, vb.byteOffset, (used[-1] + 1) * vb.byteStride)
    pts, bad = [], 0
    for v in used:
        x, y, z = struct.unpack_from('<3f', data, v * vb.byteStride)
        if not all(math.isfinite(c) and abs(c) < 1e6 for c in (x, y, z)):
            bad += 1
            continue
        pts.append((x, y, z))
    lines.append(f'non-finite or huge: {bad}')
    for axis in range(3):
        vals = sorted(p[axis] for p in pts)
        lines.append(f'  axis {axis}: min {vals[0]:.3f} p5 {vals[len(vals)//20]:.3f} median {vals[len(vals)//2]:.3f} '
                     f'p95 {vals[len(vals)*19//20]:.3f} max {vals[-1]:.3f}')
    lines.append('first vertices: ' + ', '.join(f'({x:.2f},{y:.2f},{z:.2f})' for x, y, z in pts[:8]))
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
