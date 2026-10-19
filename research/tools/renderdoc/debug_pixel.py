# Run inside qrenderdoc:  qrenderdoc.exe --python debug_pixel.py
# RD_CAPTURE, RD_OUT, RD_EID, RD_PIXEL=x,y: debugs the pixel shader of that draw at that pixel and
# logs every image sample / gather / fetch instruction with the values of the variables it changes
# and the operands it reads (texture coordinates), plus the shader's final outputs.
# V1 (where the shadow projection samples the shadow atlas, 2026-10-18).
import os
import traceback

import renderdoc as rd

lines = []


def value(var):
    try:
        n = var.columns * var.rows
        if n == 0:
            return ''
        vals = var.value.f32v[:min(n, 4)]
        return '(' + ', '.join(f'{v:.5f}' for v in vals) + ')'
    except Exception:
        return '?'


try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    eid = int(os.environ['RD_EID'])
    x, y = (int(v) for v in os.environ['RD_PIXEL'].split(','))
    controller.SetFrameEvent(eid, True)
    pipe = controller.GetPipelineState()
    refl = pipe.GetShaderReflection(rd.ShaderStage.Pixel)
    targets = controller.GetDisassemblyTargets(True)
    disasm = controller.DisassembleShader(pipe.GetGraphicsPipelineObject(), refl, targets[0]).split('\n')
    lines.append(f'eid {eid} pixel {x},{y}; disassembly target {targets[0]!r}, {len(disasm)} lines')
    inputs = rd.DebugPixelInputs()
    trace = controller.DebugPixel(x, y, inputs)
    if trace.debugger is None:
        lines.append('debugging not possible for this pixel / shader')
    else:
        # Every variable's latest value, to read the operands of the sample instructions.
        latest = {}
        step = 0
        while True:
            states = controller.ContinueDebug(trace.debugger)
            if not states:
                break
            for state in states:
                step += 1
                info = trace.instInfo[state.nextInstruction] if state.nextInstruction < len(trace.instInfo) else None
                line_no = info.lineInfo.disassemblyLine if info else 0
                text = disasm[line_no - 1].strip() if 0 < line_no <= len(disasm) else ''
                for change in state.changes:
                    latest[change.after.name] = change.after
                if any(op in text for op in ('ImageSample', 'ImageGather', 'ImageFetch', 'ImageRead', 'ImageDref')):
                    operands = [tok.strip(',') for tok in text.replace('=', ' ').split() if tok.startswith('%')]
                    known = [f'{o}={value(latest[o])}' for o in operands if o in latest]
                    lines.append(f'step {step} line {line_no}: {text[:160]}')
                    if known:
                        lines.append('    operands: ' + '; '.join(known))
                    for change in state.changes:
                        lines.append(f'    -> {change.after.name} {value(change.after)}')
        lines.append(f'{step} steps')
        outs = [n for n in latest if 'out' in n.lower()]
        for n in outs[-6:]:
            lines.append(f'output {n} {value(latest[n])}')
        controller.FreeDebugger(trace.debugger)
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
