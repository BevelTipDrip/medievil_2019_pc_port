# Run inside qrenderdoc (its Python 3.8 and renderdoc module):
#   qrenderdoc.exe --python shadow_usage.py
# Reads RD_CAPTURE (a .rdc) and writes RD_OUT (a text report) plus PNGs next to it: for the
# shadow map candidates (16-bit depth / R16 textures up to 1024 wide, cube or 2D), which events
# write and read them, the reading draws' outputs, and the candidates' contents at those draws.
# V1 (MediEvil box shadows, 2026-10-18).
import os
import sys
import traceback

import renderdoc as rd

capture_path = os.environ['RD_CAPTURE']
out_path = os.environ['RD_OUT']
out_dir = os.path.dirname(out_path)
lines = []


def log(text=''):
    lines.append(text)


def flush():
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(capture_path, '', None) != rd.ResultCode.Succeeded:
        log('cannot open ' + capture_path)
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log('cannot replay: ' + str(result))
        return
    names = {r.resourceId: r.name for r in controller.GetResources()}
    textures = controller.GetTextures()
    actions = {}

    def walk(list_):
        for a in list_:
            actions[a.eventId] = a
            walk(a.children)
    walk(controller.GetRootActions())
    log(f'{capture_path}: {len(textures)} textures, {len(actions)} actions')

    def fmt(t):
        return t.format.Name()

    candidates = []
    for t in textures:
        name = fmt(t)
        if t.width <= 1024 and t.height <= 1024 and ('D16' in name or name.startswith('R16_UNORM')):
            candidates.append(t)
    log(f'{len(candidates)} candidates (16-bit depth / R16 UNORM, <= 1024):')
    for t in candidates:
        log(f'  {t.resourceId} {names.get(t.resourceId, "")!r} {fmt(t)} {t.width}x{t.height} '
            f'arraysize {t.arraysize} mips {t.mips} cube {t.cubemap}')

    for t in candidates:
        usage = controller.GetUsage(t.resourceId)
        if not usage:
            continue
        log('')
        log(f'== {t.resourceId} {names.get(t.resourceId, "")!r} {fmt(t)} {t.width}x{t.height}x{t.arraysize}: '
            f'{len(usage)} uses')
        reads = []
        for u in usage:
            a = actions.get(u.eventId)
            label = a.GetName(controller.GetStructuredFile()) if a else '?'
            log(f'  eid {u.eventId:6d} {str(u.usage):28s} {label[:90]}')
            if 'Resource' in str(u.usage) and 'RW' not in str(u.usage):
                reads.append(u.eventId)
        # The reading draws: what they render into, and the candidate's contents there.
        for eid in reads[:6]:
            controller.SetFrameEvent(eid, True)
            a = actions.get(eid)
            if a:
                outs = [names.get(o, str(o)) for o in a.outputs if o != rd.ResourceId.Null()]
                depth = names.get(a.depthOut, str(a.depthOut)) if a.depthOut != rd.ResourceId.Null() else '-'
                log(f'  read at eid {eid}: {a.numIndices} indices x {a.numInstances}, outputs {outs}, depth {depth}')
            save = rd.TextureSave()
            save.resourceId = t.resourceId
            save.destType = rd.FileType.PNG
            save.mip = 0
            save.slice.sliceIndex = 0
            save.comp.blackPoint = 0.0
            save.comp.whitePoint = 1.0
            png = os.path.join(out_dir, f'{os.path.basename(capture_path)}_{t.resourceId}_eid{eid}.png'.replace('::', '_'))
            png = png.replace('ResourceId', 'res')
            ok = controller.SaveTexture(save, png)
            stats = controller.GetMinMax(t.resourceId, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            log(f'    saved {os.path.basename(png)}: {ok}; min {stats[0].floatValue[0]:.4f} max {stats[1].floatValue[0]:.4f}')
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log(traceback.format_exc())
flush()
os._exit(0)
