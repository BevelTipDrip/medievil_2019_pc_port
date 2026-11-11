# Run inside qrenderdoc:  qrenderdoc.exe --python depth_dump.py
# Saves depth targets as raw little-endian float32 (depth only, row-major) after an event, to compare a
# downsampled depth with the full-size one (e.g. UE4's separate translucency depth at 75%).
# RD_CAPTURE, RD_OUT (report), RD_OUT_DIR, RD_EID, RD_TEXTURES (comma list of resource id numbers).
# Each texture goes to RD_OUT_DIR/depth_<id>_<eid>_<w>x<h>.f32. V1 (Dan's Crypt water, 2026-11-10).
import os
import struct
import traceback

import renderdoc as rd

lines = []
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        raise RuntimeError(f'open: {result}')
    eid = int(os.environ['RD_EID'])
    controller.SetFrameEvent(eid, True)
    wanted = [int(x) for x in os.environ['RD_TEXTURES'].split(',')]
    textures = {int(t.resourceId): t for t in controller.GetTextures()}
    out_dir = os.environ['RD_OUT_DIR']
    for rid in wanted:
        t = textures[rid]
        sub = rd.Subresource(0, 0, 0)
        data = controller.GetTextureData(t.resourceId, sub)
        n = t.width * t.height
        stride = len(data) // n if n else 0
        lines.append(f'{rid} {t.format.Name()} {t.width}x{t.height} bytes {len(data)} stride {stride}')
        # D32S8: RenderDoc returns depth as float32 followed by stencil per texel (stride 8) or
        # a packed float32 plane; take the first 4 bytes of each texel.
        out = bytearray(n * 4)
        for i in range(n):
            out[i * 4:i * 4 + 4] = data[i * stride:i * stride + 4]
        path = os.path.join(out_dir, f'depth_{rid}_{eid}_{t.width}x{t.height}.f32')
        with open(path, 'wb') as f:
            f.write(out)
        lines.append(f'  -> {path}')
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
