# Run inside qrenderdoc:  qrenderdoc.exe --python save_target.py
# RD_CAPTURE (.rdc), RD_EID (event id), RD_OUT (raw file). Saves the first colour output of draw RD_EID as it is after
# that draw, mip 0 / slice 0, raw texel bytes (no conversion), and RD_OUT + ".txt" with its width, height and format.
# For HDR frames (R10G10B10A2 PQ) decoded outside RenderDoc (plan-hdr-sdr-match.md, run 27, 2026-11-17).
import os
import traceback

import renderdoc as rd


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ['RD_CAPTURE'], '', None) != rd.ResultCode.Succeeded:
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        return
    eid = int(os.environ['RD_EID'])
    textures = {t.resourceId: t for t in controller.GetTextures()}
    target = None

    def walk(actions):
        nonlocal target
        for a in actions:
            if a.eventId == eid:
                target = next((o for o in a.outputs if o != rd.ResourceId.Null()), None)
            walk(a.children)
    walk(controller.GetRootActions())
    with open(os.environ['RD_OUT'] + '.txt', 'w') as info:
        if target is None or target not in textures:
            info.write('no colour output\n')
            return
        controller.SetFrameEvent(eid, True)
        tex = textures[target]
        data = controller.GetTextureData(target, rd.Subresource(0, 0, 0))
        with open(os.environ['RD_OUT'], 'wb') as f:
            f.write(bytes(data))
        info.write(f'{tex.width} {tex.height} {tex.format.Name()} {len(data)}\n')
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    with open(os.environ['RD_OUT'] + '.txt', 'a') as info:
        info.write(traceback.format_exc())
os._exit(0)
