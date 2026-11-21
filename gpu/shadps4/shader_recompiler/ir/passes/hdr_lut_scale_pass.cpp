// SPDX-License-Identifier: GPL-2.0-or-later
// MediEvil: UE 4.21's HDR tonemappers index their 32^3 grading LUT at
// PQ(scene * 0.0001) * 0.96875 + 0.015625: texels at scene 0, 0.023, 0.108, 0.297, ... so every colour
// darker than ~0.02 falls in the first cell, where trilinear blending with texels of other hues turns
// dark tones purple (study-hdr.md H38). Later UE versions index PQ(scene * 0.01): the same LUT, 100x
// finer near black, scene 100 at the last texel.
//
// A tonemapper is recognised by its structure, not its hash (the map screen draws with a second one,
// 52770387e0d6363a, next to the levels' 12178a9314cb761e): exactly three FPMul32 #0.0001 (the scene
// channels), three FPFma32 #0.96875, #0.015625 (the LUT index) and the grain quantization's
// FPMul32 #493013. UE's HDR UI composite (six #0.0001, no grain; it indexes the UI LUT, built by the
// game) and the LUT builders (no index) do not match. Matches are marked (Info::uses_hdr_lut_index) and
// logged once per program; near misses (the index and the grain or the three constants, but not all)
// are logged as not patched, so another tonemapper version shows up in the log instead of as a black
// scene.
//
// The variant with HwFragmentRuntimeInfo::hdr_lut_x100 multiplies the three constants by 100. The
// pipeline cache picks it while our LUT pass (Ue4HdrGrading, ue4_hdr_lut.frag) built the latest LUT for
// that index; the game's own builder (the ACES look) assumes 0.0001 and keeps the original.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <vector>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

namespace {
constexpr u32 SceneScaleBits = 0x38d1b717u; // 0.0001f
constexpr u32 IndexScaleBits = 0x3f780000u; // 0.96875f
constexpr u32 IndexBiasBits = 0x3c800000u;  // 0.015625f
constexpr u32 GrainBits = 0x48f0baa0u;      // 493013.0f

bool IsImmBits(const IR::Value& value, u32 bits) {
    return value.IsImmediate() && value.Type() == IR::Type::F32 && std::bit_cast<u32>(value.F32()) == bits;
}

bool Logged(std::vector<u64>& seen, u64 hash) {
    if (std::ranges::find(seen, hash) != seen.end() || seen.size() >= 32) {
        return true;
    }
    seen.push_back(hash);
    return false;
}
} // namespace

void HdrLutScalePass(IR::Program& program, const RuntimeInfo& runtime_info) {
    struct Use {
        IR::Inst* inst;
        u32 arg;
    };
    std::array<Use, 4> scales{};
    u32 num_scales = 0;
    u32 num_index = 0;
    bool grain = false;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            const IR::Opcode op = inst.GetOpcode();
            if (op == IR::Opcode::FPFma32) {
                if (IsImmBits(inst.Arg(0), IndexScaleBits) && IsImmBits(inst.Arg(2), IndexBiasBits)) {
                    ++num_index;
                }
                continue;
            }
            if (op != IR::Opcode::FPMul32) {
                continue;
            }
            for (u32 i = 0; i < 2; ++i) {
                if (IsImmBits(inst.Arg(i), GrainBits)) {
                    grain = true;
                    break;
                }
                if (IsImmBits(inst.Arg(i), SceneScaleBits)) {
                    if (num_scales < scales.size()) {
                        scales[num_scales] = {&inst, i};
                    }
                    ++num_scales;
                    break;
                }
            }
        }
    }
    const u64 hash = program.info.pgm_hash;
    const bool tonemapper = num_scales == 3 && num_index == 3 && grain;
    if (!tonemapper) {
        // Near miss: the LUT index with the grain or the three scene constants, but not all of it.
        static std::vector<u64> missed;
        if (num_index == 3 && (grain || num_scales == 3) && !Logged(missed, hash)) {
            std::printf("HDR tonemapper-like shader not patched: fs %016llx (%u x 0.0001, %u LUT index, grain %s); "
                        "it keeps the game's LUT index\n",
                        static_cast<unsigned long long>(hash), num_scales, num_index, grain ? "yes" : "no");
        }
        return;
    }
    program.info.uses_hdr_lut_index = true;
    static std::vector<u64> recognised;
    const bool scaled = runtime_info.hw.fs.hdr_lut_x100;
    if (!Logged(recognised, hash ^ (scaled ? 1u : 0u))) {
        std::printf("HDR tonemapper recognised: fs %016llx%s\n", static_cast<unsigned long long>(hash),
                    scaled ? ", LUT index at PQ(scene x 0.01) (3 constants)" : "");
    }
    if (!scaled) {
        return;
    }
    for (u32 n = 0; n < num_scales; ++n) {
        scales[n].inst->SetArg(scales[n].arg, IR::Value{0.01f});
    }
}

} // namespace Shader::Optimization
