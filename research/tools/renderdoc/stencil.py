# Run inside qrenderdoc:  qrenderdoc.exe --python stencil.py
# RD_CAPTURE, RD_OUT, RD_EIDS (draws: their stencil / depth-bounds state), RD_PIXEL=x,y and
# RD_FROM, RD_TO (event range): the depth-stencil target's pixel history there, with stencil values.
# V1 (the preshadow's receiver stencil, 2026-10-18).
import os
import traceback

import renderdoc as rd

lines = []


def face(f):
    parts = []
    for n in ('function', 'reference', 'compareMask', 'writeMask', 'passOperation', 'failOperation', 'depthFailOperation'):
        try:
            parts.append(f'{n}={getattr(f, n)}')
        except Exception:
            pass
    return ' '.join(parts)


try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    names = {r.resourceId: r.name for r in controller.GetResources()}
    sf = controller.GetStructuredFile()
    actions = {}

    def walk(list_):
        for a in list_:
            actions[a.eventId] = a
            walk(a.children)
    walk(controller.GetRootActions())
    for eid in [int(e) for e in os.environ['RD_EIDS'].split(',')]:
        controller.SetFrameEvent(eid, True)
        ds = controller.GetVulkanPipelineState().depthStencil
        a = actions.get(eid)
        lines.append(f'==== eid {eid} {a.GetName(sf) if a else ""} {a.numIndices if a else 0} idx; '
                     f'outputs {[names.get(o, "")[:40] for o in (a.outputs if a else []) if o != rd.ResourceId.Null()]}')
        lines.append(f'  depth test {ds.depthTestEnable} write {ds.depthWriteEnable} func {ds.depthFunction}; '
                     f'bounds {ds.depthBoundsEnable} {getattr(ds, "minDepthBounds", "")}..{getattr(ds, "maxDepthBounds", "")}')
        lines.append(f'  stencil {ds.stencilTestEnable}; front: {face(ds.frontFace)}')
        lines.append(f'                 back:  {face(ds.backFace)}')
    if os.environ.get('RD_PIXEL'):
        x, y = (int(v) for v in os.environ['RD_PIXEL'].split(','))
        lo, hi = int(os.environ['RD_FROM']), int(os.environ['RD_TO'])
        depth = next(r.resourceId for r in controller.GetResources() if '0x201a000000' in r.name and 'Image' in r.name)
        controller.SetFrameEvent(hi, True)
        history = controller.PixelHistory(depth, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        lines.append(f'\ndepth-stencil history at {x},{y} ({names[depth]}), events {lo}..{hi}:')
        for m in history:
            if not (lo <= m.eventId <= hi):
                continue
            a = actions.get(m.eventId)
            lines.append(f'  eid {m.eventId:6d} {a.GetName(sf)[:34] if a else "?":34s} {a.numIndices if a else 0:5d} idx '
                         f'depth {m.preMod.depth:.6f}->{m.postMod.depth:.6f} stencil {m.preMod.stencil}->{m.postMod.stencil} '
                         f'passed {m.Passed()} stencilFail {m.stencilTestFailed} depthFail {m.depthTestFailed} '
                         f'boundsFail {m.depthBoundsFailed}')
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
