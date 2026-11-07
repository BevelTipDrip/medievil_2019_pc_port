# Run inside qrenderdoc:  qrenderdoc.exe --python resource_names.py
# For each resource id in RD_IDS (comma list): its debug name (the port names images "Image WxHxD
# tiling format guest_address:size ...") and its uses in the frame (event, usage); plus, with
# RD_ADDR (hex), every image whose name holds that guest address (other images at the same
# address: a texture lookup can find the wrong one). RD_CAPTURE, RD_REMOTE (optional), RD_OUT.
# V1 (2026-11-06).
import os
import traceback

import renderdoc as rd

lines = []
remote = cap = None
try:
    if os.environ.get('RD_REMOTE'):
        result, remote = rd.CreateRemoteServerConnection(os.environ['RD_REMOTE'])
        result, controller = remote.OpenCapture(getattr(rd.RemoteServer, 'NoPreference', 0xFFFFFFFF),
                                                os.environ['RD_CAPTURE'], rd.ReplayOptions(), None)
    else:
        cap = rd.OpenCaptureFile()
        cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
        result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        raise RuntimeError(f'open: {result}')
    resources = {int(r.resourceId): r for r in controller.GetResources()}
    for text in os.environ.get('RD_IDS', '').split(','):
        if not text.strip():
            continue
        r = resources.get(int(text))
        if r is None:
            lines.append(f'{text}: no such resource')
            continue
        uses = controller.GetUsage(r.resourceId)
        lines.append(f'{text}: "{r.name}", {len(uses)} uses')
        for u in uses[:40]:
            lines.append(f'    eid {u.eventId} {str(u.usage)}')
        if len(uses) > 40:
            lines.append(f'    ... last eid {uses[-1].eventId}')
    address = os.environ.get('RD_ADDR', '').lower()
    if address:
        for rid, r in resources.items():
            if address in r.name.lower():
                lines.append(f'at {address}: {rid} "{r.name}" uses {len(controller.GetUsage(r.resourceId))}')
    if remote is not None:
        remote.CloseCapture(controller)
        remote.ShutdownConnection()
    else:
        controller.Shutdown()
        cap.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
