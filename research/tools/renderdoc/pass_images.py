# Run inside qrenderdoc:  qrenderdoc.exe --python pass_images.py
# Every render pass of a capture (consecutive actions with the same color / depth targets) with its
# event range, action count and targets, and each pass's color targets saved as PNG after its last
# action: look through them in order for the first pass whose output is wrong.
# RD_CAPTURE: the capture (a path on the replay machine with RD_REMOTE), RD_REMOTE (optional): a
# remote server to replay on (e.g. "localhost" through ssh -L 39920:localhost:39920: a Deck
# capture does not replay on another vendor's GPU), RD_OUT: the report, RD_IMG_DIR: where the PNGs
# go (local), RD_MIN_WIDTH (default 256): smaller targets are listed, not saved, RD_MAX_IMAGES
# (default 200). V1 (2026-11-06).
import os
import traceback

import renderdoc as rd

lines = []
remote = None
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
    names = {r.resourceId: r.name for r in controller.GetResources()}
    sf = controller.GetStructuredFile()
    null = rd.ResourceId.Null()

    def describe(rid):
        t = textures.get(rid)
        if t is None:
            return f'{names.get(rid, rid)}'
        return f'{int(rid)} {t.format.Name()} {t.width}x{t.height}' + (f' x{t.arraysize}' if t.arraysize > 1 else '')

    actions = []

    def walk(list_):
        for a in list_:
            if a.children:
                walk(a.children)
            else:
                actions.append(a)
    walk(controller.GetRootActions())
    passes = []  # [first eid, last eid, count, colors, depth, kinds]
    for a in actions:
        colors = tuple(o for o in a.outputs if o != null)
        depth = a.depthOut if a.depthOut != null else None
        kind = ('draw' if a.flags & rd.ActionFlags.Drawcall else 'dispatch' if a.flags & rd.ActionFlags.Dispatch
                else 'clear' if a.flags & rd.ActionFlags.Clear else 'copy' if a.flags & rd.ActionFlags.Copy
                else 'other')
        if passes and passes[-1][3] == colors and passes[-1][4] == depth and kind != 'dispatch' and passes[-1][5] != {'dispatch'}:
            p = passes[-1]
            p[1], p[2] = a.eventId, p[2] + 1
            p[5].add(kind)
        else:
            passes.append([a.eventId, a.eventId, 1, colors, depth, {kind}, a.GetName(sf)[:60]])
    out_dir = os.environ['RD_IMG_DIR']
    os.makedirs(out_dir, exist_ok=True)
    min_width = int(os.environ.get('RD_MIN_WIDTH', '256'))
    budget = int(os.environ.get('RD_MAX_IMAGES', '200'))
    lines.append(f'{len(actions)} actions, {len(passes)} passes')
    for i, (first, last, count, colors, depth, kinds, name) in enumerate(passes):
        lines.append(f'#{i:03d} eid {first}-{last} {count} {"/".join(sorted(kinds))} first "{name}"')
        for k, c in enumerate(colors):
            lines.append(f'    color{k} {describe(c)}')
        if depth is not None:
            lines.append(f'    depth  {describe(depth)}')
        saved = False
        for k, c in enumerate(colors):
            t = textures.get(c)
            if t is None or t.width < min_width or budget <= 0:
                continue
            if not saved:
                controller.SetFrameEvent(last, True)
                saved = True
            save = rd.TextureSave()
            save.resourceId = c
            save.destType = rd.FileType.PNG
            save.mip = 0
            save.slice.sliceIndex = 0
            save.alpha = rd.AlphaMapping.Discard
            file = os.path.join(out_dir, f'p{i:03d}_eid{last}_c{k}_{t.format.Name()}_{t.width}x{t.height}.png')
            ok = controller.SaveTexture(save, file)
            budget -= 1
            lines.append(f'    saved {os.path.basename(file)} {ok}')
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
