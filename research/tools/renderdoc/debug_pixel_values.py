# Run inside qrenderdoc:  qrenderdoc.exe --python debug_pixel_values.py
# Debugs the pixel shader of one draw at one pixel and logs every value it computes, in order
# (variable name and up to 4 components), plus the source line of each step: to find which input
# or intermediate is out of range (e.g. a scene colour of 118 from a light map term).
# RD_CAPTURE, RD_REMOTE (optional, see pass_images.py), RD_EID, RD_PIXEL=x,y, RD_OUT.
# V1 (2026-11-06).
import os
import traceback

import renderdoc as rd

lines = []


def value(var):
    try:
        n = var.columns * var.rows
        if n == 0:
            return ''
        if var.type in (rd.VarType.UInt, rd.VarType.SInt):
            return '(' + ', '.join(str(v) for v in var.value.u32v[:min(n, 4)]) + ')'
        return '(' + ', '.join(f'{v:.6g}' for v in var.value.f32v[:min(n, 4)]) + ')'
    except Exception:
        return '?'


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
    eid = int(os.environ['RD_EID'])
    x, y = (int(v) for v in os.environ['RD_PIXEL'].split(','))
    controller.SetFrameEvent(eid, True)
    pipe = controller.GetPipelineState()
    refl = pipe.GetShaderReflection(rd.ShaderStage.Pixel)
    target = controller.GetDisassemblyTargets(True)[0]
    disasm = controller.DisassembleShader(pipe.GetGraphicsPipelineObject(), refl, target).split('\n')
    lines.append(f'eid {eid} pixel {x},{y}')
    trace = controller.DebugPixel(x, y, rd.DebugPixelInputs())
    if trace.debugger is None:
        lines.append('debugging not possible for this pixel / shader')
    else:
        step = 0
        while True:
            states = controller.ContinueDebug(trace.debugger)
            if not states:
                break
            for state in states:
                step += 1
                info = trace.instInfo[state.nextInstruction] if state.nextInstruction < len(trace.instInfo) else None
                line_no = info.lineInfo.disassemblyLine if info else 0
                for change in state.changes:
                    lines.append(f'{step} L{line_no} {change.after.name} = {value(change.after)}')
        lines.append(f'{step} steps')
        controller.FreeDebugger(trace.debugger)
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
