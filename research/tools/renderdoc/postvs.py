# Run inside qrenderdoc:  qrenderdoc.exe --python postvs.py
# RD_CAPTURE, RD_OUT, RD_EIDS: per draw, the vertex shader's output positions (post-VS, clip space
# and after the perspective divide: min / max / spread, NaN / inf counts, how many land outside the
# view volume) and its vertex inputs (buffers, formats). V1 (MediEvil box shadows, 2026-10-18).
import math
import os
import struct
import traceback

import renderdoc as rd

capture_path = os.environ['RD_CAPTURE']
out_path = os.environ['RD_OUT']
eids = [int(e) for e in os.environ['RD_EIDS'].split(',')]
lines = []


def log(text=''):
    lines.append(str(text))


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(capture_path, '', None) != rd.ResultCode.Succeeded:
        log('cannot open')
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log('cannot replay: ' + str(result))
        return
    names = {r.resourceId: r.name for r in controller.GetResources()}
    for eid in eids:
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        log(f'==== eid {eid}')
        try:
            for attr in pipe.GetVertexInputs():
                log(f'  input {attr.name} buffer {attr.vertexBuffer} offset {attr.byteOffset} '
                    f'format {attr.format.Name()} instanced {attr.perInstance}')
            for i, vb in enumerate(pipe.GetVBuffers()):
                if vb.resourceId != rd.ResourceId.Null():
                    log(f'  vbuffer {i}: {names.get(vb.resourceId, vb.resourceId)} offset {vb.byteOffset} stride {vb.byteStride}')
        except Exception as error:
            log(f'  (inputs: {error})')
        mesh = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        if mesh.vertexResourceId == rd.ResourceId.Null() or mesh.numIndices == 0:
            log('  no post-VS data')
            continue
        data = controller.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, 0)
        stride = mesh.vertexByteStride
        count = min(len(data) // stride, 200000)
        xs, ys, zs, bad, outside = [], [], [], 0, 0
        for i in range(count):
            x, y, z, w = struct.unpack_from('4f', data, i * stride)
            if not all(math.isfinite(v) for v in (x, y, z, w)) or w == 0:
                bad += 1
                continue
            nx, ny, nz = x / w, y / w, z / w
            if not (-1 <= nx <= 1 and -1 <= ny <= 1 and 0 <= nz <= 1):
                outside += 1
            xs.append(nx); ys.append(ny); zs.append(nz)
        log(f'  post-VS vertices {count} (stride {stride}), NaN/inf/w=0 {bad}, outside the view volume {outside}')
        if xs:
            for axis, v in (('x', xs), ('y', ys), ('z', zs)):
                v = sorted(v)
                log(f'    {axis}: min {v[0]:.4f} p5 {v[len(v)//20]:.4f} median {v[len(v)//2]:.4f} '
                    f'p95 {v[len(v)*19//20]:.4f} max {v[-1]:.4f}')
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log(traceback.format_exc())
with open(out_path, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
os._exit(0)
