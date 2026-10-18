# Run inside qrenderdoc:  qrenderdoc.exe --python pixel_history.py
# RD_CAPTURE (.rdc), RD_OUT (text report). Without RD_PIXEL: saves the colour output of the last
# draws that wrote each 3840x2160 colour target (RD_OUT dir, <capture>_final_<res>.png) and lists
# them. With RD_PIXEL=x,y[;x,y...] and RD_TARGET=<resource id number>[,...]: the pixel history of that target
# (every event that touched the pixel: shader output, blend result, pass / fail).
# V1 (MediEvil box shadows, 2026-10-18).
import os
import traceback

import renderdoc as rd

capture_path = os.environ['RD_CAPTURE']
out_path = os.environ['RD_OUT']
out_dir = os.path.dirname(out_path)
lines = []


def log(text=''):
    lines.append(str(text))


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(capture_path, '', None) != rd.ResultCode.Succeeded:
        log('cannot open')
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log('cannot replay: ' + str(result))
        return
    names = {r.resourceId: r.name for r in controller.GetResources()}
    by_number = {int(r.resourceId): r.resourceId for r in controller.GetResources()}
    sf = controller.GetStructuredFile()
    actions = []

    def walk(list_):
        for a in list_:
            actions.append(a)
            walk(a.children)
    walk(controller.GetRootActions())
    pixel = os.environ.get('RD_PIXEL')
    if not pixel:
        last = {}
        for a in actions:
            for o in a.outputs:
                if o != rd.ResourceId.Null():
                    last[o] = a.eventId
        textures = {t.resourceId: t for t in controller.GetTextures()}
        for rid, eid in sorted(last.items(), key=lambda kv: kv[1]):
            t = textures.get(rid)
            if not t or t.width != 3840:
                continue
            controller.SetFrameEvent(eid, True)
            save = rd.TextureSave()
            save.resourceId = rid
            save.destType = rd.FileType.PNG
            png = os.path.join(out_dir, f'{os.path.basename(capture_path)}_final_{int(rid)}.png')
            log(f'{int(rid)} {names.get(rid, "")!r} last written at eid {eid}: {controller.SaveTexture(save, png)}')
    else:
        controller.SetFrameEvent(actions[-1].eventId, True)
        for point in pixel.split(';'):
          x, y = (int(v) for v in point.split(','))
          for number in os.environ['RD_TARGET'].split(','):
            target = by_number[int(number)]
            history = controller.PixelHistory(target, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            log('')
            log(f'pixel history of {names.get(target, target)} at {x},{y}: {len(history)} modifications')
            for m in history:
                a = next((b for b in actions if b.eventId == m.eventId), None)
                name = a.GetName(sf) if a else '?'
                pre = m.preMod.col.floatValue
                post = m.postMod.col.floatValue
                sh = m.shaderOut.col.floatValue
                log(f'  eid {m.eventId:6d} {name[:40]:40s} prim {m.primitiveID:5d} '
                    f'shader ({sh[0]:.3f},{sh[1]:.3f},{sh[2]:.3f},{sh[3]:.3f}) '
                    f'before ({pre[0]:.3f},{pre[1]:.3f},{pre[2]:.3f},{pre[3]:.3f}) '
                    f'after ({post[0]:.3f},{post[1]:.3f},{post[2]:.3f},{post[3]:.3f}) '
                    f'passed {m.Passed()}')
                if a:
                    outs = [names.get(o, '')[:60] for o in a.outputs if o != rd.ResourceId.Null()]
                    log(f'          {a.numIndices} indices x {a.numInstances}; outputs {outs}')
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log(traceback.format_exc())
with open(out_path, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
