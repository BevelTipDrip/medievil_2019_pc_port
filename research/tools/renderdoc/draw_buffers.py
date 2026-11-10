# Run inside qrenderdoc:  qrenderdoc.exe --python draw_buffers.py
# The buffer ranges a draw's pixel shader binds, saved raw: the first draw whose pipeline name holds
# RD_SHADER (e.g. "fs_0xd3caef1f45107cdd"), or RD_EID. Each read-write / constant buffer binding of the
# pixel shader goes to RD_OUT_DIR/<prefix>_b<bind>_<offset>_<size>.bin; RD_OUT lists them.
# RD_CAPTURE, RD_OUT, RD_OUT_DIR, RD_PREFIX. V1 (Hall of Heroes light, 2026-11-09).
import os
import traceback

import renderdoc as rd

lines = []
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    names = {r.resourceId: r.name for r in controller.GetResources()}
    eid = int(os.environ.get('RD_EID', '0'))
    if not eid:
        want = os.environ['RD_SHADER']
        actions = []

        def walk(list_):
            for a in list_:
                actions.append(a)
                walk(a.children)
        walk(controller.GetRootActions())
        for a in actions:
            if not (a.flags & rd.ActionFlags.Drawcall):
                continue
            controller.SetFrameEvent(a.eventId, False)
            pipe = controller.GetPipelineState()
            if want in names.get(pipe.GetGraphicsPipelineObject(), ''):
                eid = a.eventId
                break
    controller.SetFrameEvent(eid, True)
    pipe = controller.GetPipelineState()
    lines.append(f'eid {eid} pipeline {names.get(pipe.GetGraphicsPipelineObject(), "")}')
    stage = rd.ShaderStage.Pixel
    refl = pipe.GetShaderReflection(stage)
    prefix = os.environ.get('RD_PREFIX', 'buf')
    for used in pipe.GetReadWriteResources(stage, True):
        if used.descriptor.resource == rd.ResourceId.Null():
            continue
        desc = used.descriptor
        if desc.byteSize == 0 or desc.byteSize > 64 * 1024 * 1024:
            continue
        idx = used.access.index
        bind = refl.readWriteResources[idx].fixedBindNumber if idx < len(refl.readWriteResources) else idx
        data = controller.GetBufferData(desc.resource, desc.byteOffset, desc.byteSize)
        path = os.path.join(os.environ['RD_OUT_DIR'], f'{prefix}_b{bind}_{desc.byteOffset}_{desc.byteSize}.bin')
        open(path, 'wb').write(bytes(data))
        lines.append(f'bind {bind} {names.get(desc.resource, "")} offset {desc.byteOffset} size {desc.byteSize} -> {path}')
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
open(os.environ['RD_OUT'], 'w').write('\n'.join(lines) + '\n')
os._exit(0)
