# Run inside qrenderdoc:  qrenderdoc.exe --python find_draws.py
# RD_CAPTURE, RD_OUT, RD_INDICES (comma-separated index counts) or RD_VB0 (vertex buffer 0 byte offsets,
# hex, comma-separated): every draw with one of those index counts or vertex buffer 0 offsets, with its outputs / depth target, vertex buffer 0 and index buffer. V1 (2026-10-18).
import os
import traceback

import renderdoc as rd

lines = []
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    names = {r.resourceId: r.name for r in controller.GetResources()}
    wanted = {int(v) for v in os.environ.get('RD_INDICES', '').split(',') if v}
    vb_offsets = {int(v, 16) for v in os.environ.get('RD_VB0', '').split(',') if v}
    found = []

    def walk(list_):
        for a in list_:
            if a.numIndices in wanted or (vb_offsets and a.numIndices):
                found.append(a)
            walk(a.children)
    walk(controller.GetRootActions())
    for a in found:
        controller.SetFrameEvent(a.eventId, True)
        pipe = controller.GetPipelineState()
        vbs = pipe.GetVBuffers()
        vb0 = vbs[0] if vbs else None
        if vb_offsets and not (vb0 and vb0.byteOffset in vb_offsets):
            continue
        ib = pipe.GetIBuffer()
        outs = [names.get(o, '')[:50] for o in a.outputs if o != rd.ResourceId.Null()]
        depth = names.get(a.depthOut, '')[:50] if a.depthOut != rd.ResourceId.Null() else '-'
        lines.append(f'eid {a.eventId:6d} {a.numIndices} idx; vb0 {names.get(vb0.resourceId, "")[:40] if vb0 else "-"} '
                     f'+{vb0.byteOffset if vb0 else 0}; ib {names.get(ib.resourceId, "")[:40]} +{ib.byteOffset}; '
                     f'outputs {outs}; depth {depth}')
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
