# Run inside qrenderdoc:  qrenderdoc.exe --python view_ub.py
# UE4 4.21 View uniform buffer (FViewUniformShaderParameters, 3,296 bytes) at the given draws: finds every
# shader-bound buffer range of RD_UB_SIZE bytes (default 3296) and decodes the members the DLSS / FSR plan uses
# (offsets from study-mgs2-temporal-for-ue4.md section 3.3). Also writes the raw bytes (RD_RAW_DIR, optional) so
# two captures can be compared. RD_CAPTURE, RD_OUT, RD_EIDS (comma list), RD_REMOTE (optional).
# V1 (2026-11-08, TAAU frame map).
import hashlib
import os
import struct
import traceback

import renderdoc as rd

lines = []
SIZE = int(os.environ.get('RD_UB_SIZE', '3296'))
MEMBERS = [  # offset, name, kind (m = float4x4, f4, f3, f2, f, u)
    (384, 'ViewToClip', 'm'), (448, 'ViewToClipNoAA', 'm'), (832, 'ViewForward', 'f3'), (848, 'ViewUp', 'f3'),
    (864, 'ViewRight', 'f3'), (912, 'InvDeviceZToWorldZTransform', 'f4'), (928, 'ScreenPositionScaleBias', 'f4'),
    (944, 'WorldCameraOrigin', 'f3'), (1200, 'PrevViewToClip', 'm'), (1824, 'ClipToPrevClip', 'm'),
    (1888, 'TemporalAAJitter', 'f4'), (1920, 'FieldOfViewWideAngles', 'f2'), (1952, 'ViewSizeAndInvSize', 'f4'),
    (1968, 'BufferSizeAndInvSize', 'f4'), (2004, 'PreExposure', 'f'), (2112, 'NearPlane', 'f'),
    (2128, 'MaterialTextureMipBias', 'f'), (2132, 'MaterialTextureDerivativeMultiply', 'f'),
    (2140, 'FrameNumber', 'u'), (2144, 'StateFrameIndexMod8', 'u'), (2148, 'CameraCut', 'f'),
]


def fmt(data, off, kind):
    if kind == 'm':
        rows = [struct.unpack_from('<4f', data, off + 16 * r) for r in range(4)]
        return ' | '.join(' '.join(f'{v:.7g}' for v in row) for row in rows)
    if kind == 'u':
        return str(struct.unpack_from('<I', data, off)[0]) + f' (as float {struct.unpack_from("<f", data, off)[0]:.7g})'
    n = {'f4': 4, 'f3': 3, 'f2': 2, 'f': 1}[kind]
    return ' '.join(f'{v:.7g}' for v in struct.unpack_from(f'<{n}f', data, off))


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
    raw_dir = os.environ.get('RD_RAW_DIR')
    if raw_dir:
        os.makedirs(raw_dir, exist_ok=True)
    for eid in [int(e) for e in os.environ['RD_EIDS'].split(',') if e.strip()]:
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        found = {}
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel, rd.ShaderStage.Compute):
            if pipe.GetShaderReflection(stage) is None:
                continue
            for fn in (pipe.GetReadWriteResources, pipe.GetReadOnlyResources, pipe.GetConstantBlocks):
                try:
                    used = fn(stage, True)
                except TypeError:
                    used = fn(stage)
                for u in used:
                    d = u.descriptor
                    if getattr(d, 'byteSize', 0) == SIZE:
                        found.setdefault((int(d.resource), d.byteOffset), []).append(str(stage).split('.')[-1])
        lines.append(f'==== eid {eid}: {len(found)} range(s) of {SIZE} bytes')
        for (rid, off), stages in found.items():
            res = [r for r in controller.GetBuffers() if int(r.resourceId) == rid][0].resourceId
            data = bytes(controller.GetBufferData(res, off, SIZE))
            lines.append(f'  buffer {rid} +{off} ({",".join(sorted(set(stages)))}) md5 {hashlib.md5(data).hexdigest()[:12]}')
            if raw_dir:
                with open(os.path.join(raw_dir, f'view_{eid}_{rid}_{off}.bin'), 'wb') as h:
                    h.write(data)
            for o, name, kind in MEMBERS:
                lines.append(f'    {o:5d} {name}: {fmt(data, o, kind)}')
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
