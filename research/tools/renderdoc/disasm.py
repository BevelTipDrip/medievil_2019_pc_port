# Run inside qrenderdoc:  qrenderdoc.exe --python disasm.py
# RD_CAPTURE, RD_OUT, RD_EID, RD_STAGE (Vertex / Pixel): the shader's disassembly (RenderDoc's
# SPIR-V view) and the raw bytes of its read-only buffers / constant ring ranges as floats.
# V1 (2026-10-18).
import os
import struct
import traceback

import renderdoc as rd

lines = []
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    names = {r.resourceId: r.name for r in controller.GetResources()}
    eid = int(os.environ['RD_EID'])
    stage = getattr(rd.ShaderStage, os.environ.get('RD_STAGE', 'Pixel'))
    controller.SetFrameEvent(eid, True)
    pipe = controller.GetPipelineState()
    refl = pipe.GetShaderReflection(stage)
    target = controller.GetDisassemblyTargets(True)[0]
    lines.append(controller.DisassembleShader(pipe.GetGraphicsPipelineObject(), refl, target))
    try:
        rw = pipe.GetReadWriteResources(stage, True)
    except TypeError:
        rw = pipe.GetReadWriteResources(stage)
    for i, used in enumerate(rw):
        d = getattr(used, 'descriptor', used)
        rid = getattr(d, 'resource', None)
        size = min(getattr(d, 'byteSize', 0), 4096)
        if not rid or size <= 0:
            continue
        data = controller.GetBufferData(rid, d.byteOffset, size)
        floats = struct.unpack(f'<{len(data) // 4}f', data[:len(data) // 4 * 4])
        lines.append(f'\n==== buffer {i}: {names.get(rid, rid)} +{d.byteOffset} size {size}')
        for k in range(0, len(floats), 4):
            lines.append(f'  [{k * 4:5d}] ' + ' '.join(f'{v: .6g}' for v in floats[k:k + 4]))
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
