# Run inside qrenderdoc:  qrenderdoc.exe --python texture_formats.py
# RD_CAPTURE (.rdc), RD_OUT (text report). Colour-space check (plan-hdr-sdr-match.md, 2026-11-17): the formats of
# every texture in the capture (count and examples), and for the G-buffer draws (3+ colour targets) the view formats
# their pixel shaders sample: an sRGB-encoded colour texture read through a UNORM view brightens and greys the scene.
import collections
import os
import traceback

import renderdoc as rd

out = open(os.environ['RD_OUT'], 'w', encoding='utf-8')


def log(text):
    out.write(text + '\n')
    out.flush()


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ['RD_CAPTURE'], '', None) != rd.ResultCode.Succeeded:
        log('cannot open capture')
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log('cannot replay')
        return
    textures = controller.GetTextures()
    counts = collections.Counter(t.format.Name() for t in textures)
    log('texture formats (resources):')
    for name, n in counts.most_common():
        log(f'  {n:5d} {name}')
    draws = []

    def walk(actions):
        for a in actions:
            if a.flags & rd.ActionFlags.Drawcall:
                draws.append(a)
            walk(a.children)
    walk(controller.GetRootActions())
    views = collections.Counter()
    pairs = collections.Counter()
    tex_by_id = {t.resourceId: t for t in textures}
    gbuffer = [a for a in draws if sum(1 for o in a.outputs if o != rd.ResourceId.Null()) >= 3]
    log(f'{len(gbuffer)} G-buffer draws (3+ colour targets); sampled view formats:')
    for a in gbuffer[:400]:
        controller.SetFrameEvent(a.eventId, False)
        pipe = controller.GetPipelineState()
        try:
            ro = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel, True)
        except TypeError:
            ro = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel)
        for used in ro:
            d = getattr(used, 'descriptor', used)
            rid = getattr(d, 'resource', None)
            fmt = getattr(d, 'format', None)
            tex = tex_by_id.get(rid)
            if tex is None or fmt is None:
                continue
            view = fmt.Name()
            views[view] += 1
            pairs[(tex.format.Name(), view)] += 1
    for name, n in views.most_common():
        log(f'  {n:6d} view {name}')
    log('resource -> view format pairs:')
    for (res, view), n in pairs.most_common():
        log(f'  {n:6d} {res} -> {view}')
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log(traceback.format_exc())
os._exit(0)
