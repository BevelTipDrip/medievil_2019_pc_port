# Run inside qrenderdoc:  qrenderdoc.exe --python covering_buffers.py
# RD_CAPTURE, RD_OUT, RD_ADDRESS (guest address, hex), RD_BEFORE (event id): every buffer whose name
# ("Buffer 0x<start>:0x<size>") covers the address, and the events before RD_BEFORE that use it as
# a destination (RW / copy / clear) or source. V1 (2026-10-18).
import os
import re
import traceback

import renderdoc as rd

lines = []
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    sf = controller.GetStructuredFile()
    actions = {}

    def walk(list_):
        for a in list_:
            actions[a.eventId] = a
            walk(a.children)
    walk(controller.GetRootActions())
    address = int(os.environ['RD_ADDRESS'], 16)
    before = int(os.environ['RD_BEFORE'])
    for r in controller.GetResources():
        m = re.search(r'0x([0-9a-fA-F]+):0x([0-9a-fA-F]+)', r.name)
        if not m or 'Image' in r.name:
            continue
        start, size = int(m[1], 16), int(m[2], 16)
        if not (start <= address < start + size):
            continue
        usage = controller.GetUsage(r.resourceId)
        lines.append(f'{r.resourceId} {r.name!r}: offset of the address {address - start:#x}; {len(usage)} uses')
        for u in usage:
            if u.eventId >= before:
                continue
            kind = str(u.usage)
            if any(k in kind for k in ('RW', 'Copy', 'Clear', 'Resolve', 'VertexBuffer', 'IndexBuffer')):
                a = actions.get(u.eventId)
                lines.append(f'  eid {u.eventId:6d} {kind:30s} {a.GetName(sf)[:60] if a else "?"}')
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
