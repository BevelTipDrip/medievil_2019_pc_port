# Run inside qrenderdoc:  qrenderdoc.exe --python save_texture.py
# RD_CAPTURE (.rdc), RD_EID (event id), RD_RESOURCE (texture resource id number), RD_OUT (raw file). Saves the texture
# as it is at that event, mip 0 / slice 0, raw stored texel bytes (no conversion, no view swizzle: see
# LANDMINES.md 28), and RD_OUT + ".txt" with its width, height and format. For sampled inputs (e.g. a grading LUT
# texture) that save_target.py (colour outputs only) does not reach. V1 (HDR grading 9f6bac6cba984956, 2026-11-21).
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
    controller.SetFrameEvent(int(os.environ['RD_EID']), True)
    want = int(os.environ['RD_RESOURCE'])
    textures = {int(t.resourceId): t for t in controller.GetTextures()}
    t = textures.get(want)
    with open(os.environ['RD_OUT'] + '.txt', 'w') as info:
        if t is None:
            info.write('texture not found\n')
            return
        data = controller.GetTextureData(t.resourceId, rd.Subresource(0, 0, 0))
        with open(os.environ['RD_OUT'], 'wb') as f:
            f.write(bytes(data))
        info.write(f'{t.width} {t.height} {t.format.Name()} {len(data)}\n')
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    with open(os.environ['RD_OUT'] + '.txt', 'w') as info:
        info.write(traceback.format_exc())
