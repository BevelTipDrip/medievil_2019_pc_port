# Run inside qrenderdoc:  qrenderdoc.exe --python draw_state.py
# RD_CAPTURE (.rdc), RD_OUT (text report), RD_EIDS (comma-separated event ids): per draw, the pixel
# shader's resources (image name, view format / type / subresources), its samplers (filter, compare,
# address modes), blend / depth state, and the colour output saved before and after the draw.
# V1 (MediEvil box shadows, 2026-10-18).
import os
import traceback

import renderdoc as rd

capture_path = os.environ['RD_CAPTURE']
out_path = os.environ['RD_OUT']
eids = [int(e) for e in os.environ['RD_EIDS'].split(',')]
out_dir = os.path.dirname(out_path)
lines = []


def log(text=''):
    lines.append(str(text))


def fields(obj, names):
    parts = []
    for n in names:
        try:
            parts.append(f'{n}={getattr(obj, n)}')
        except Exception:
            pass
    return ' '.join(parts)


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(capture_path, '', None) != rd.ResultCode.Succeeded:
        log('cannot open ' + capture_path)
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log('cannot replay: ' + str(result))
        return
    names = {r.resourceId: r.name for r in controller.GetResources()}
    sf = controller.GetStructuredFile()
    for eid in eids:
        controller.SetFrameEvent(eid, True)
        action = controller.GetAction(eid) if hasattr(controller, 'GetAction') else None
        pipe = controller.GetPipelineState()
        log(f'==== eid {eid} {action.GetName(sf) if action else ""}')
        if action:
            log(f'  {action.numIndices} indices x {action.numInstances}')
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
            refl = pipe.GetShaderReflection(stage)
            log(f'  {stage}: shader {pipe.GetShader(stage)} entry {pipe.GetShaderEntryPoint(stage)}')
            if refl is None:
                continue
            try:
                ro = pipe.GetReadOnlyResources(stage, True)
            except TypeError:
                ro = pipe.GetReadOnlyResources(stage)
            for used in ro:
                d = getattr(used, 'descriptor', used)
                rid = getattr(d, 'resource', None)
                log(f'    read  {names.get(rid, rid)} | ' + fields(d, ['format', 'textureType', 'firstMip', 'numMips',
                                                                       'firstSlice', 'numSlices', 'byteOffset', 'byteSize']))
            try:
                samplers = pipe.GetSamplers(stage, True)
            except TypeError:
                samplers = pipe.GetSamplers(stage)
            for used in samplers:
                s = getattr(used, 'sampler', getattr(used, 'descriptor', used))
                log('    sampler ' + fields(s, ['filter', 'compareFunction', 'addressU', 'addressV', 'addressW',
                                               'minLOD', 'maxLOD', 'mipBias', 'borderColorValue', 'seamlessCubemaps']))
            try:
                rw = pipe.GetReadWriteResources(stage, True)
            except TypeError:
                rw = pipe.GetReadWriteResources(stage)
            for used in rw:
                d = getattr(used, 'descriptor', used)
                log(f'    write {names.get(getattr(d, "resource", None), "")} | ' + fields(d, ['byteOffset', 'byteSize', 'textureType']))
        try:
            vk = controller.GetVulkanPipelineState()
            cb = vk.colorBlend
            for i, b in enumerate(cb.blends):
                log(f'  blend[{i}] enabled={b.enabled} color={b.colorBlend.source}*{b.colorBlend.operation}*{b.colorBlend.destination} '
                    f'alpha={b.alphaBlend.source}*{b.alphaBlend.operation}*{b.alphaBlend.destination} mask={b.writeMask}')
            ds = vk.depthStencil
            log('  depth ' + fields(ds, ['depthTestEnable', 'depthWriteEnable', 'depthFunction', 'depthBoundsEnable',
                                        'stencilTestEnable']))
            log('  raster ' + fields(vk.rasterizer, ['cullMode', 'frontCCW', 'depthClampEnable', 'depthBiasEnable']))
        except Exception as error:
            log(f'  (vulkan state: {error})')
        # Colour output before and after the draw.
        outputs = [o for o in pipe.GetOutputTargets() if getattr(o, 'resource', getattr(o, 'resourceId', None)) != rd.ResourceId.Null()]
        for o in outputs[:1]:
            rid = getattr(o, 'resource', getattr(o, 'resourceId', None))
            for when, at in (('before', eid - 1), ('after', eid)):
                controller.SetFrameEvent(at, True)
                save = rd.TextureSave()
                save.resourceId = rid
                save.destType = rd.FileType.PNG
                png = os.path.join(out_dir, f'{os.path.basename(capture_path)}_eid{eid}_{when}.png')
                log(f'  output {names.get(rid, rid)} {when}: {controller.SaveTexture(save, png)} -> {os.path.basename(png)}')
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log(traceback.format_exc())
with open(out_path, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
