// SPDX-License-Identifier: GPL-2.0-or-later
// MediEvil: UE 4.21's HDR tonemapper (fs 12178a9314cb761e) indexes its 32^3 grading LUT at
// PQ(scene * 0.0001) * 0.96875 + 0.015625: texels at scene 0, 0.023, 0.108, 0.297, ... so every colour
// darker than ~0.02 falls in the first cell, where trilinear blending with texels of other hues turns
// dark tones purple (study-hdr.md H38). Later UE versions index PQ(scene * 0.01): the same LUT, 100x
// finer near black, scene 100 at the last texel.
//
// The variant with HwFragmentRuntimeInfo::hdr_lut_x100 multiplies the three FPMul32 #0.0001 (the scene
// channels, each used only by the PQ encode before the LUT sample) by 100. The pipeline cache picks it
// while our LUT pass (Ue4HdrGrading, ue4_hdr_lut.frag) built this frame's LUT for that index; the game's
// own builder (the ACES look) assumes 0.0001 and keeps the original.

#include <array>
#include <bit>
#include <cstdio>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

void HdrLutScalePass(IR::Program& program, const RuntimeInfo& runtime_info) {
    if (program.info.pgm_hash != Ue4HdrTonemapPs || !runtime_info.hw.fs.hdr_lut_x100) {
        return;
    }
    constexpr u32 SceneScaleBits = 0x38d1b717u; // 0.0001f
    struct Use {
        IR::Inst* inst;
        u32 arg;
    };
    std::array<Use, 4> found{};
    u32 count = 0;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::FPMul32) {
                continue;
            }
            for (u32 i = 0; i < 2; ++i) {
                const IR::Value arg = inst.Arg(i);
                if (arg.IsImmediate() && std::bit_cast<u32>(arg.F32()) == SceneScaleBits) {
                    if (count < found.size()) {
                        found[count] = {&inst, i};
                    }
                    ++count;
                    break;
                }
            }
        }
    }
    static u32 logged = 0;
    if (count != 3) {
        // Another shader under this hash, or the translation changed: the game's index, as is.
        if (logged++ < 4) {
            std::printf("HDR tonemapper: %u x 0.0001 found where 3 were expected; the game's LUT index kept\n",
                        count);
        }
        return;
    }
    for (u32 n = 0; n < count; ++n) {
        found[n].inst->SetArg(found[n].arg, IR::Value{0.01f});
    }
    if (logged++ < 4) {
        std::printf("HDR tonemapper: LUT index at PQ(scene x 0.01) (3 constants)\n");
    }
}

} // namespace Shader::Optimization
