# Run inside qrenderdoc:  qrenderdoc.exe --python shader_draws.py
# Every draw whose vertex or pixel shader module name contains one of RD_SHADERS (comma-separated GCN
# program hashes, as the port names modules: fs_0x<hash>_<perm>), with its shaders and outputs; for the
# last match of each hash, every read-only texture with its name, type, size and min / max (each slice
# of a 3D texture) and a PNG of mip 0 (3D: slices 0, 15, 31) in RD_IMG_DIR.
# RD_CAPTURE, RD_OUT, RD_SHADERS, RD_IMG_DIR. HDR UI composite (study-hdr.md H12, 2026-11-17).
import os
import traceback

import renderdoc as rd

out_path = os.environ['RD_OUT']
lines = []


def log(text):
    lines.append(text)
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ['RD_CAPTURE'], '', None) != rd.ResultCode.Succeeded:
        log('cannot open capture')
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log('cannot replay: ' + str(result))
        return
    wanted = [h.strip().lower() for h in os.environ['RD_SHADERS'].split(',') if h.strip()]
    img_dir = os.environ.get('RD_IMG_DIR', os.path.dirname(out_path))
    names = {r.resourceId: r.name for r in controller.GetResources()}
    textures = {t.resourceId: t for t in controller.GetTextures()}
    draws = []

    def walk(actions):
        for a in actions:
            if a.flags & rd.ActionFlags.Drawcall:
                draws.append(a)
            walk(a.children)
    walk(controller.GetRootActions())
    log(f'{len(draws)} draws')
    last = {}
    for a in draws:
        controller.SetFrameEvent(a.eventId, False)
        pipe = controller.GetPipelineState()
        vs = names.get(pipe.GetShader(rd.ShaderStage.Vertex), '')
        ps = names.get(pipe.GetShader(rd.ShaderStage.Pixel), '')
        if os.environ.get('RD_LIST') == '1':  # every draw's shader names (to see how they are named)
            outs = [names.get(o, str(o)) for o in a.outputs if o != rd.ResourceId.Null()]
            log(f'  eid {a.eventId}: vs {vs!r} ({pipe.GetShader(rd.ShaderStage.Vertex)}) '
                f'ps {ps!r} ({pipe.GetShader(rd.ShaderStage.Pixel)}) -> {outs}')
        for h in wanted:
            if h in vs.lower() or h in ps.lower():
                outs = [names.get(o, str(o)) for o in a.outputs if o != rd.ResourceId.Null()]
                log(f'eid {a.eventId}: vs {vs} ps {ps} {a.numIndices}x{a.numInstances} -> {outs}')
                last[h] = a.eventId
    # RD_EIDS=a,b: inspect these draws instead (shader names are only in captures with the port's
    # Vulkan debug names on).
    if os.environ.get('RD_EIDS'):
        last = {f'eid{e.strip()}': int(e) for e in os.environ['RD_EIDS'].split(',') if e.strip()}
    for h, eid in last.items():
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        log(f'==== last {h}: eid {eid}')
        try:
            ro = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel, True)
        except TypeError:
            ro = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel)
        for i, used in enumerate(ro):
            d = getattr(used, 'descriptor', used)
            rid = getattr(d, 'resource', None)
            tex = textures.get(rid)
            if tex is None:
                log(f'  [{i}] {names.get(rid, rid)} (not a texture)')
                continue
            log(f'  [{i}] {names.get(rid, rid)} {tex.type} {tex.width}x{tex.height}x{tex.depth} '
                f'arraysize {tex.arraysize} mips {tex.mips} {tex.format.Name()}')
            slices = list(range(tex.depth)) if tex.depth > 1 else [0]
            for s in slices:
                sub = rd.Subresource(0, s, 0)
                lo, hi = controller.GetMinMax(rid, sub, rd.CompType.Typeless)
                if tex.depth == 1 or s in (0, 1, 15, 16, 30, 31):
                    log(f'      slice {s}: min {list(lo.floatValue)} max {list(hi.floatValue)}')
            for s in ([0, 15, 31] if tex.depth > 1 else [0]):
                save = rd.TextureSave()
                save.resourceId = rid
                save.destType = rd.FileType.PNG
                save.slice.sliceIndex = s
                save.alpha = rd.AlphaMapping.Preserve
                png = os.path.join(img_dir, f'{h}_eid{eid}_t{i}_s{s}.png')
                log(f'      png slice {s}: {controller.SaveTexture(save, png)} -> {os.path.basename(png)}')
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log(traceback.format_exc())
os._exit(0)
