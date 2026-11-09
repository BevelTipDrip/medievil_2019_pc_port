# Plain Python (not qrenderdoc):  python spv_cache_scan.py <cache dir> [--all]
# Lists the compute shaders (or, with --all, every shader) in the port's shader cache folder
# (user\cache\CUSA11227: <pgm_hash>_<perm>.spv, written by RegisterShaderBinary,
# vk_pipeline_serialization.cpp) with execution model, local size, SPIR-V size, number of sampled /
# storage image types, image stores and the file's last write time (= when the port last compiled it:
# shaders first compiled under new settings show up by time). Decompile one with
# `spirv-cross <file> --vulkan-semantics` (MSYS2 clang64\bin).
# V1 (2026-11-08, TAAU frame map: the TAA compute shader of a temporal upsampling run).
import datetime
import os
import struct
import sys

MODELS = {0: 'VS', 1: 'HS', 2: 'DS', 3: 'GS', 4: 'PS', 5: 'CS'}


def scan(path):
    data = open(path, 'rb').read()
    words = struct.unpack('<%dI' % (len(data) // 4), data[:len(data) // 4 * 4])
    i, model, local, sampled, storage, stores = 5, None, None, 0, 0, 0
    while i < len(words):
        op, n = words[i] & 0xffff, words[i] >> 16
        if n == 0:
            break
        if op == 15 and model is None:  # OpEntryPoint
            model = words[i + 1]
        elif op == 16 and words[i + 2] == 17:  # OpExecutionMode LocalSize
            local = tuple(words[i + 3:i + 6])
        elif op == 25:  # OpTypeImage: sampled operand 1 = sampled, 2 = storage
            sampled += words[i + 7] == 1
            storage += words[i + 7] == 2
        elif op == 99:  # OpImageWrite
            stores += 1
        i += n
    return model, local, len(data), sampled, storage, stores


def main():
    folder = sys.argv[1]
    every = '--all' in sys.argv
    rows = []
    for name in sorted(os.listdir(folder)):
        if not name.endswith('.spv'):
            continue
        path = os.path.join(folder, name)
        model, local, size, sampled, storage, stores = scan(path)
        if every or model == 5:
            rows.append((os.stat(path).st_mtime, name, model, local, size, sampled, storage, stores))
    for mtime, name, model, local, size, sampled, storage, stores in sorted(rows):
        print(f'{datetime.datetime.fromtimestamp(mtime):%Y-%m-%d %H:%M:%S} {name:28s} {MODELS.get(model, model)} '
              f'local {local} {size:7d} bytes, {sampled} sampled / {storage} storage image types, {stores} image stores')
    print(f'{len(rows)} shaders')


main()
