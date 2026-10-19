# Run inside qrenderdoc:  qrenderdoc.exe --python resource_usage.py
# RD_CAPTURE, RD_OUT, RD_RESOURCE (resource id number): every use of the resource, with the
# action's name, index count and outputs. V1 (2026-10-18).
import os
import traceback

import renderdoc as rd

lines = []
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    names = {r.resourceId: r.name for r in controller.GetResources()}
    by_number = {int(r.resourceId): r.resourceId for r in controller.GetResources()}
    sf = controller.GetStructuredFile()
    actions = {}

    def walk(list_):
        for a in list_:
            actions[a.eventId] = a
            walk(a.children)
    walk(controller.GetRootActions())
    rid = by_number[int(os.environ['RD_RESOURCE'])]
    lines.append(f'{names.get(rid, rid)}')
    for u in controller.GetUsage(rid):
        a = actions.get(u.eventId)
        outs = [names.get(o, '')[:42] for o in (a.outputs if a else []) if o != rd.ResourceId.Null()]
        lines.append(f'  eid {u.eventId:6d} {str(u.usage):30s} {a.GetName(sf)[:40] if a else "?":40s} '
                     f'{a.numIndices if a else 0:6d} idx outputs {outs}')
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
