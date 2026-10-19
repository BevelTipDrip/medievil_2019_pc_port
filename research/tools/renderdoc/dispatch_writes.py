# Run inside qrenderdoc:  qrenderdoc.exe --python dispatch_writes.py
# RD_CAPTURE, RD_OUT, RD_EIDS: per dispatch, its read-write resources (buffers: byte range; images:
# name), its constant ring ranges, and its thread group counts. V1 (2026-10-18).
import os
import traceback

import renderdoc as rd

lines = []
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    names = {r.resourceId: r.name for r in controller.GetResources()}
    actions = {}

    def walk(list_):
        for a in list_:
            actions[a.eventId] = a
            walk(a.children)
    walk(controller.GetRootActions())
    for eid in [int(e) for e in os.environ['RD_EIDS'].split(',')]:
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        a = actions.get(eid)
        lines.append(f'==== eid {eid} groups {tuple(a.dispatchDimension) if a else ""} shader {pipe.GetShader(rd.ShaderStage.Compute)}')
        try:
            rw = pipe.GetReadWriteResources(rd.ShaderStage.Compute, True)
        except TypeError:
            rw = pipe.GetReadWriteResources(rd.ShaderStage.Compute)
        seen = set()
        for used in rw:
            d = getattr(used, 'descriptor', used)
            key = (d.resource, d.byteOffset, d.byteSize)
            if key in seen:
                continue
            seen.add(key)
            lines.append(f'  rw {names.get(d.resource, d.resource)} +{d.byteOffset:#x} size {d.byteSize:#x}')
        try:
            ro = pipe.GetReadOnlyResources(rd.ShaderStage.Compute, True)
        except TypeError:
            ro = pipe.GetReadOnlyResources(rd.ShaderStage.Compute)
        for used in ro:
            d = getattr(used, 'descriptor', used)
            lines.append(f'  ro {names.get(d.resource, d.resource)}')
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
