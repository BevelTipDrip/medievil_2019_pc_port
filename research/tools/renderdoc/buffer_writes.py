# Run inside qrenderdoc:  qrenderdoc.exe --python buffer_writes.py
# RD_CAPTURE, RD_OUT, RD_BUFFER (substring of the buffer's name), RD_BEFORE (event id): the
# events before RD_BEFORE that write the buffer (compute / pixel RW, copies, clears), and for the
# last few compute writes, which buffer ranges their RW descriptors cover. V1 (2026-10-18).
import os
import traceback

import renderdoc as rd

lines = []
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    names = {r.resourceId: r.name for r in controller.GetResources()}
    before = int(os.environ['RD_BEFORE'])
    sf = controller.GetStructuredFile()
    actions = {}

    def walk(list_):
        for a in list_:
            actions[a.eventId] = a
            walk(a.children)
    walk(controller.GetRootActions())
    target = next(rid for rid, n in names.items() if os.environ['RD_BUFFER'] in n)
    usage = controller.GetUsage(target)
    writes = [u for u in usage if u.eventId < before and any(k in str(u.usage) for k in ('RW', 'Copy', 'Clear', 'Resolve'))
              and 'Src' not in str(u.usage)]
    lines.append(f'{names[target]}: {len(usage)} uses, {len(writes)} writes before eid {before}')
    for u in writes[-25:]:
        a = actions.get(u.eventId)
        lines.append(f'  eid {u.eventId:6d} {str(u.usage):24s} {a.GetName(sf)[:60] if a else "?"}')
    for u in [w for w in writes if 'CS' in str(w.usage)][-6:]:
        controller.SetFrameEvent(u.eventId, True)
        pipe = controller.GetPipelineState()
        try:
            rw = pipe.GetReadWriteResources(rd.ShaderStage.Compute, True)
        except TypeError:
            rw = pipe.GetReadWriteResources(rd.ShaderStage.Compute)
        for used in rw:
            d = getattr(used, 'descriptor', used)
            if getattr(d, 'resource', None) == target:
                lines.append(f'    eid {u.eventId} writes {names[target][:30]} bytes +{d.byteOffset:#x} size {d.byteSize:#x}')
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
