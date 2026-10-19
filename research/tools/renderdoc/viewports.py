# Run inside qrenderdoc:  qrenderdoc.exe --python viewports.py
# RD_CAPTURE, RD_OUT, RD_EIDS: per draw, viewports and scissors (Vulkan state) and the depth / colour
# attachment sizes. V1 (shadow atlas placement, 2026-10-18).
import os
import traceback

import renderdoc as rd

lines = []
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    names = {r.resourceId: r.name for r in controller.GetResources()}
    for eid in [int(e) for e in os.environ['RD_EIDS'].split(',')]:
        controller.SetFrameEvent(eid, True)
        vk = controller.GetVulkanPipelineState()
        lines.append(f'==== eid {eid}')
        for i, vs in enumerate(vk.viewportScissor.viewportScissors):
            v, s = vs.vp, vs.scissor
            lines.append(f'  viewport[{i}] x {v.x} y {v.y} w {v.width} h {v.height} depth {v.minDepth}..{v.maxDepth}; '
                         f'scissor x {s.x} y {s.y} w {s.width} h {s.height}')
        try:
            pipe = controller.GetPipelineState()
            d = pipe.GetDepthTarget()
            rid = getattr(d, 'resource', getattr(d, 'resourceId', None))
            lines.append(f'  depth target {names.get(rid, rid)}')
        except Exception as error:
            lines.append(f'  (depth target: {error})')
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
