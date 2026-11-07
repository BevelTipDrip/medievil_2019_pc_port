# Run inside qrenderdoc:  qrenderdoc.exe --python pixel_draw_textures.py
# Which draw wrote a pixel of a target, and what that draw sampled: the pixel history of RD_TEX at
# RD_X, RD_Y (up to RD_EID), then for the last draw that wrote it every bound read-only texture with
# the view's format and component swizzle against the image's own format, each saved as PNG (to see
# whether a texture is swizzled wrongly, e.g. red and blue swapped).
# RD_CAPTURE, RD_REMOTE (optional, see pass_images.py), RD_TEX (resource id), RD_X, RD_Y, RD_EID,
# RD_OUT, RD_IMG_DIR. V1 (2026-11-06).
import os
import traceback

import renderdoc as rd

lines = []
remote = None


class Log(list):
    def append(self, text):
        super().append(text)
        with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
            f.write('\n'.join(self) + '\n')


lines = Log()
try:
    path = os.environ['RD_CAPTURE']
    if os.environ.get('RD_REMOTE'):
        result, remote = rd.CreateRemoteServerConnection(os.environ['RD_REMOTE'])
        if result != rd.ResultCode.Succeeded:
            raise RuntimeError(f'remote server: {result}')
        no_preference = getattr(rd.RemoteServer, 'NoPreference', 0xFFFFFFFF)
        result, controller = remote.OpenCapture(no_preference, path, rd.ReplayOptions(), None)
    else:
        cap = rd.OpenCaptureFile()
        cap.OpenFile(path, '', None)
        result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        raise RuntimeError(f'open: {result}')
    textures = {t.resourceId: t for t in controller.GetTextures()}
    by_int = {int(r): r for r in textures}
    target = by_int[int(os.environ['RD_TEX'])]
    x, y, eid = int(os.environ['RD_X']), int(os.environ['RD_Y']), int(os.environ['RD_EID'])
    controller.SetFrameEvent(eid, True)
    sub = rd.Subresource(0, 0, 0)
    history = controller.PixelHistory(target, x, y, sub, rd.CompType.Typeless)
    writer = None
    for mod in history:
        col = mod.postMod.col.floatValue
        lines.append(f'eid {mod.eventId} passed {mod.Passed()} post ({col[0]:.3f} {col[1]:.3f} {col[2]:.3f} {col[3]:.3f})')
        if mod.Passed():
            writer = mod.eventId
    if writer is None:
        raise RuntimeError('no draw wrote the pixel')
    controller.SetFrameEvent(writer, True)
    state = controller.GetPipelineState()
    lines.append(f'writer eid {writer}')
    lines.append(f'pipeline state read')
    out_dir = os.environ['RD_IMG_DIR']
    os.makedirs(out_dir, exist_ok=True)
    stages = [rd.ShaderStage.Vertex, rd.ShaderStage.Pixel]
    seen = set()
    for stage in stages:
        for used in state.GetReadOnlyResources(stage, True):
            d = used.descriptor
            rid = d.resource
            if rid == rd.ResourceId.Null() or rid in seen:
                continue
            seen.add(rid)
            t = textures.get(rid)
            sw = d.swizzle
            swz = ''.join('RGBA01'[int(c)] if int(c) < 6 else '?' for c in (sw.red, sw.green, sw.blue, sw.alpha))
            image_fmt = t.format.Name() if t else '?'
            view_fmt = d.format.Name()
            dims = f'{t.width}x{t.height} mips {t.mips} x{t.arraysize}' if t else ''
            lines.append(f'  {stage} slot {used.access.index} res {int(rid)} image {image_fmt} view {view_fmt} swizzle {swz} {dims}')
            if t is not None and t.width >= 16:
                save = rd.TextureSave()
                save.resourceId = rid
                save.destType = rd.FileType.PNG
                save.mip = 0
                save.slice.sliceIndex = 0
                save.alpha = rd.AlphaMapping.Discard
                file = os.path.join(out_dir, f'eid{writer}_res{int(rid)}_{image_fmt}_{t.width}x{t.height}.png')
                controller.SaveTexture(save, file)
    if remote is not None:
        remote.CloseCapture(controller)
        remote.ShutdownConnection()
    else:
        controller.Shutdown()
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
