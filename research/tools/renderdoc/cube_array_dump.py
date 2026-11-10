# Run inside qrenderdoc:  qrenderdoc.exe --python cube_array_dump.py
# The reflection capture cubemap array of a UE4 frame: the first R16G16B16A16_FLOAT 128x128 image
# with at least 7 array layers (RD_MATCH="WxH:min_layers", e.g. 128x128:64), every layer of mip RD_MIP
# (default 2) written raw (RGBA16F, row-major) to RD_OUT_BIN one after another, and its id, size
# and layer count to RD_OUT. Read the slices with cube_array_sheet.py. V1 (Hall of Heroes light,
# 2026-11-09).
import os
import traceback

import renderdoc as rd

lines = []
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    mip = int(os.environ.get('RD_MIP', '2'))
    eid = int(os.environ.get('RD_EID', '0'))
    if eid:
        controller.SetFrameEvent(eid, True)
    want = os.environ.get('RD_MATCH', '128x128')
    w, h = (int(v) for v in want.split(':')[0].split('x'))
    min_layers = int(want.split(':')[1]) if ':' in want else 7
    tex = None
    for t in controller.GetTextures():
        if t.width == w and t.height == h and t.arraysize >= min_layers and t.mips > mip and \
                'R16G16B16A16' in str(t.format.Name()):
            tex = t
            break
    lines.append(f'texture {int(tex.resourceId)} {tex.width}x{tex.height} layers {tex.arraysize} mips {tex.mips} '
                 f'format {tex.format.Name()} eid {eid}')
    with open(os.environ['RD_OUT_BIN'], 'wb') as out:
        for layer in range(tex.arraysize):
            sub = rd.Subresource(mip, layer, 0)
            data = controller.GetTextureData(tex.resourceId, sub)
            out.write(bytes(data))
    lines.append(f'mip {mip}: {max(1, tex.width >> mip)}x{max(1, tex.height >> mip)} per layer, written')
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
open(os.environ['RD_OUT'], 'w').write('\n'.join(lines) + '\n')
os._exit(0)
