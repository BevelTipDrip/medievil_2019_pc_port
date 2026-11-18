# Run inside qrenderdoc:  qrenderdoc.exe --python lut_probe.py
# RD_CAPTURE (.rdc), RD_OUT (text report). HDR grading LUT check (research/docs/plan-hdr-sdr-match.md, run 26,
# 2026-11-17): lists every draw that writes or samples a 32^3 3D texture (our grading LUT pass: 3 vertices x 32
# instances; the game's builder; the HDR tonemapper and the UI composite: a sampled 3D LUT), and prints the grey
# diagonal (texel i, i, i) of each such LUT after the writing draws and at the sampling draws, as the stored 10-bit
# values and as nits (PQ x 1.05, the tonemapper's scale).
import os
import struct
import traceback

import renderdoc as rd

out_path = os.environ['RD_OUT']
out = open(out_path, 'w', encoding='utf-8')


def log(text):
    out.write(text + '\n')
    out.flush()


def pq_to_nits(v):
    v = max(v, 0.0)
    p = v ** (1.0 / 78.84375)
    num = max(p - 0.8359375, 0.0)
    den = 18.8515625 - 18.6875 * p
    return 10000.0 * (num / den) ** (1.0 / 0.1593017578125) if den > 0 else float('inf')


def diagonal(controller, tex, label):
    """The texels (i, i, i), i = 0..12, 16, 24, 31 of a 32^3 R10G10B10A2 texture."""
    parts = []
    for i in list(range(0, 13)) + [16, 24, 31]:
        data = controller.GetTextureData(tex.resourceId, rd.Subresource(0, i, 0))
        row = tex.width * 4
        plane = row * tex.height
        # RenderDoc returns the whole volume for a 3D texture (run 26: slice i read as slice 0 otherwise).
        at = i * plane + i * row + i * 4 if len(data) >= tex.depth * plane else i * row + i * 4
        if len(data) < at + 4:
            parts.append(f'{i}: short data {len(data)}')
            continue
        word = struct.unpack_from('<I', data, at)[0]
        r, g, b = (word & 1023) / 1023.0, (word >> 10 & 1023) / 1023.0, (word >> 20 & 1023) / 1023.0
        parts.append(f'{i}: g {g:.4f} ({pq_to_nits(g * 1.05):.2f} nits) r {r:.4f} b {b:.4f}')
    log(f'  {label} ({len(data)} bytes per read):')
    for p in parts:
        log('    ' + p)


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ['RD_CAPTURE'], '', None) != rd.ResultCode.Succeeded:
        log('cannot open capture')
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log('cannot replay: ' + str(result))
        return
    textures = {t.resourceId: t for t in controller.GetTextures()}
    names = {r.resourceId: r.name for r in controller.GetResources()}
    luts = {rid for rid, t in textures.items() if t.width == 32 and t.height == 32 and t.depth == 32}
    log(f'32^3 3D textures: {[str(r) + " " + names.get(r, "") + " " + textures[r].format.Name() for r in luts]}')
    draws = []

    def walk(actions):
        for a in actions:
            if a.flags & rd.ActionFlags.Drawcall:
                draws.append(a)
            walk(a.children)
    walk(controller.GetRootActions())
    log(f'{len(draws)} draws')
    events = []
    for a in draws:
        writes = [o for o in a.outputs if o in luts]
        controller.SetFrameEvent(a.eventId, False)
        pipe = controller.GetPipelineState()
        try:
            ro = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel, True)
        except TypeError:
            ro = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel)
        reads = []
        for used in ro:
            d = getattr(used, 'descriptor', used)
            rid = getattr(d, 'resource', None)
            if rid in luts:
                reads.append(rid)
        if writes or reads:
            log(f'eid {a.eventId}: {a.numIndices} x {a.numInstances} writes {[str(w) for w in writes]} '
                f'reads {[str(r) for r in reads]} outputs {[str(o) for o in a.outputs if o != rd.ResourceId.Null()]}')
            events.append((a.eventId, writes, reads))
    for eid, writes, reads in events:
        for rid in writes:
            # After the write: the next event shows its result.
            controller.SetFrameEvent(eid + 1, True)
            diagonal(controller, textures[rid], f'after eid {eid} wrote {rid}')
        for rid in reads:
            controller.SetFrameEvent(eid, True)
            diagonal(controller, textures[rid], f'at eid {eid} reading {rid}')
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log(traceback.format_exc())
os._exit(0)
