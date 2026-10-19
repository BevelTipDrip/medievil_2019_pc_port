# Run inside qrenderdoc:  qrenderdoc.exe --python list_actions.py
# RD_CAPTURE, RD_OUT, RD_FROM, RD_TO, RD_MATCH (optional substring): the actions in an event range
# with their name, index count, outputs and depth target. V1 (2026-10-18).
import os
import traceback

import renderdoc as rd

lines = []
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    names = {r.resourceId: r.name for r in controller.GetResources()}
    sf = controller.GetStructuredFile()
    lo, hi = int(os.environ['RD_FROM']), int(os.environ['RD_TO'])
    match = os.environ.get('RD_MATCH', '')
    found = []

    def walk(list_):
        for a in list_:
            if lo <= a.eventId <= hi:
                found.append(a)
            walk(a.children)
    walk(controller.GetRootActions())
    for a in sorted(found, key=lambda a: a.eventId):
        name = a.GetName(sf)
        if match and match not in name:
            continue
        outs = [names.get(o, '')[:44] for o in a.outputs if o != rd.ResourceId.Null()]
        depth = names.get(a.depthOut, '')[:44] if a.depthOut != rd.ResourceId.Null() else '-'
        lines.append(f'eid {a.eventId:6d} {name[:60]:60s} {a.numIndices:6d} idx outputs {outs} depth {depth}')
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
