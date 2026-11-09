# Run inside qrenderdoc:  qrenderdoc.exe --python draw_io.py
# Per draw / dispatch: pipeline, each stage's shader (RenderDoc id, debug name, md5 of its SPIR-V and,
# with RD_SPV_DIR, the shadPS4 cache file with the same bytes: "<stage>_<pgm_hash>_<perm>"),
# viewport / scissor, colour and depth targets (id, format, size, view), depth / stencil state,
# every read-only and read-write resource and constant buffer the shader uses (binding, resource,
# format, size, range).
# RD_CAPTURE, RD_OUT, RD_EIDS (comma list), RD_REMOTE (optional, see pass_images.py),
# RD_SPV_DIR (optional): the port's shader cache folder (user\cache\CUSA11227: <pgm_hash>_<perm>.spv),
# RD_SPV_SAVE (optional): folder to write each stage's SPIR-V as <eid>_<stage>.spv (diff two captures).
# V1 (2026-11-08, TAAU frame map).
import hashlib
import os
import traceback

import renderdoc as rd

lines = []
STAGE = {rd.ShaderStage.Vertex: 'VS', rd.ShaderStage.Pixel: 'PS', rd.ShaderStage.Compute: 'CS'}


def fields(obj, names):
    parts = []
    for n in names:
        try:
            parts.append(f'{n}={getattr(obj, n)}')
        except Exception:
            pass
    return ' '.join(parts)


def used_list(fn, stage):
    try:
        return fn(stage, True)
    except TypeError:
        return fn(stage)


def main():
    remote = cap = None
    if os.environ.get('RD_REMOTE'):
        result, remote = rd.CreateRemoteServerConnection(os.environ['RD_REMOTE'])
        result, controller = remote.OpenCapture(getattr(rd.RemoteServer, 'NoPreference', 0xFFFFFFFF),
                                                os.environ['RD_CAPTURE'], rd.ReplayOptions(), None)
    else:
        cap = rd.OpenCaptureFile()
        cap.OpenFile(os.environ['RD_CAPTURE'], '', None)
        result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        raise RuntimeError(f'open: {result}')
    names = {r.resourceId: r.name for r in controller.GetResources()}
    textures = {t.resourceId: t for t in controller.GetTextures()}
    buffers = {b.resourceId: b for b in controller.GetBuffers()}
    sf = controller.GetStructuredFile()
    spv_index = {}
    spv_dir = os.environ.get('RD_SPV_DIR')
    if spv_dir:
        for f in os.listdir(spv_dir):
            if f.endswith('.spv'):
                with open(os.path.join(spv_dir, f), 'rb') as h:
                    spv_index.setdefault(hashlib.md5(h.read()).hexdigest(), []).append(f[:-4])
        lines.append(f'shader cache: {sum(len(v) for v in spv_index.values())} .spv files from {spv_dir}')
    actions = {}

    def walk(list_):
        for a in list_:
            actions[a.eventId] = a
            walk(a.children)
    walk(controller.GetRootActions())
    save_dir = os.environ.get('RD_SPV_SAVE')
    if save_dir:
        os.makedirs(save_dir, exist_ok=True)

    def res(rid):
        t = textures.get(rid)
        if t is not None:
            return f'{int(rid)} {t.format.Name()} {t.width}x{t.height}' + (f' x{t.arraysize}' if t.arraysize > 1 else '') + \
                   (f' mips {t.mips}' if t.mips > 1 else '')
        b = buffers.get(rid)
        if b is not None:
            return f'{int(rid)} buffer len {b.length}'
        return f'{names.get(rid, rid)}'

    for eid in [int(e) for e in os.environ['RD_EIDS'].split(',') if e.strip()]:
        controller.SetFrameEvent(eid, True)
        action = actions.get(eid)
        pipe = controller.GetPipelineState()
        vk = controller.GetVulkanPipelineState()
        lines.append(f'==== eid {eid} {action.GetName(sf) if action else ""}')
        if action:
            lines.append(f'  {action.numIndices} indices x {action.numInstances}; dispatch {list(action.dispatchDimension)}')
        compute = bool(action and action.flags & rd.ActionFlags.Dispatch)
        pl = vk.compute.pipelineResourceId if compute else vk.graphics.pipelineResourceId
        lines.append(f'  pipeline {int(pl)} "{names.get(pl, "")}"')
        stages = (rd.ShaderStage.Compute,) if compute else (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel)
        for stage in stages:
            refl = pipe.GetShaderReflection(stage)
            if refl is None:
                lines.append(f'  {STAGE[stage]}: none')
                continue
            sid = pipe.GetShader(stage)
            raw = bytes(refl.rawBytes)
            md5 = hashlib.md5(raw).hexdigest()
            match = ','.join(spv_index.get(md5, [])) if spv_dir else ''
            lines.append(f'  {STAGE[stage]}: shader {int(sid)} "{names.get(sid, "")}" spv {len(raw)} bytes md5 {md5}'
                         + (f' cache {match or "no match"}' if spv_dir else ''))
            if save_dir:
                with open(os.path.join(save_dir, f'{eid}_{STAGE[stage]}.spv'), 'wb') as h:
                    h.write(raw)
            ro_refl = list(refl.readOnlyResources)
            rw_refl = list(refl.readWriteResources)
            cb_refl = list(refl.constantBlocks)

            def bind_of(lst, access):
                i = access.index
                if 0 <= i < len(lst):
                    r = lst[i]
                    return f'set {getattr(r, "fixedBindSetOrSpace", "?")} bind {getattr(r, "fixedBindNumber", "?")} "{r.name}"'
                return f'index {i}'
            for kind, fn, lst in (('read', pipe.GetReadOnlyResources, ro_refl),
                                  ('write', pipe.GetReadWriteResources, rw_refl),
                                  ('cbuf', pipe.GetConstantBlocks, cb_refl)):
                for used in used_list(fn, stage):
                    d = used.descriptor
                    a = used.access
                    extra = fields(d, ['byteOffset', 'byteSize']) if kind == 'cbuf' or int(d.resource) in [int(x) for x in buffers] \
                        else fields(d, ['format', 'firstMip', 'numMips', 'firstSlice', 'numSlices'])
                    lines.append(f'    {kind:5s} {bind_of(lst, a)} -> {res(d.resource)} | view {int(getattr(d, "view", 0))} {extra}')
        if not compute:
            for i, vs in enumerate(vk.viewportScissor.viewportScissors):
                v, s = vs.vp, vs.scissor
                lines.append(f'  viewport[{i}] {v.x},{v.y} {v.width}x{v.height}; scissor {s.x},{s.y} {s.width}x{s.height}')
            ra = vk.currentPass.renderArea
            lines.append(f'  render area {ra.x},{ra.y} {ra.width}x{ra.height}')
            for o in pipe.GetOutputTargets():
                rid = getattr(o, 'resource', None)
                if rid is not None and rid != rd.ResourceId.Null():
                    lines.append(f'  color out {res(rid)} | view fmt {o.format.Name()} mip {o.firstMip} slice {o.firstSlice}')
            dt = pipe.GetDepthTarget()
            if dt.resource != rd.ResourceId.Null():
                lines.append(f'  depth out {res(dt.resource)}')
            ds = vk.depthStencil
            lines.append('  depth ' + fields(ds, ['depthTestEnable', 'depthWriteEnable', 'depthFunction', 'stencilTestEnable']))
            if ds.stencilTestEnable:
                for side, f in (('front', ds.frontFace), ('back', ds.backFace)):
                    lines.append(f'  stencil {side} ' + fields(f, ['function', 'reference', 'compareMask', 'writeMask',
                                                                   'passOperation', 'failOperation', 'depthFailOperation']))
            for i, b in enumerate(vk.colorBlend.blends):
                lines.append(f'  blend[{i}] enabled={b.enabled} color={b.colorBlend.source}*{b.colorBlend.operation}*'
                             f'{b.colorBlend.destination} mask={b.writeMask}')
    if remote is not None:
        remote.CloseCapture(controller)
        remote.ShutdownConnection()
    else:
        controller.Shutdown()
        cap.Shutdown()


try:
    main()
except Exception:
    lines.append(traceback.format_exc())
with open(os.environ['RD_OUT'], 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
