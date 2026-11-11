// SPDX-License-Identifier: GPL-2.0-or-later
// MediEvil: UE4 4.21 finds a pixel's forward light grid cell (reflection captures, local lights of
// lit translucency) as uint(SvPosition.xy - ViewRectMin) >> LightGridPixelSizeShift, the shift
// read from ForwardLightData. The grid is built for the full view (64-pixel cells). Separate
// translucency below 100% (effects resolution) draws at that fraction of the view but does not
// scale the position, so it reads the top-left part of the grid: reflections with hard edges on
// 64-pixel lines, shrunk toward the top-left (Dan's Crypt water, research/patches card
// light-grid-scale). The PS4 draws it at 100%.
//
// Every fragment shader with this lookup is marked (Info::uses_light_grid); the pipeline cache
// picks a variant with the scale for draws at effects resolution (light_grid_scale_x / _y), where
// the position is multiplied by it before the conversion.

#include <bit>
#include <cstdio>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

namespace {
/// The FragCoord component (0: x, 1: y) a float value is computed from through a few float
/// operations (the view rectangle's origin subtracted), else -1.
int FragCoordComponent(const IR::Value& value, int depth) {
    if (value.IsImmediate()) {
        return -1;
    }
    const IR::Inst* inst = value.TryInst();
    if (!inst) {
        return -1;
    }
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetAttribute: {
        if (inst->Arg(0).Attribute() != IR::Attribute::FragCoord) {
            return -1;
        }
        const u32 comp = inst->Arg(1).U32();
        return comp < 2 ? static_cast<int>(comp) : -1;
    }
    case IR::Opcode::FPAdd32:
    case IR::Opcode::FPSub32:
    case IR::Opcode::FPNeg32:
    case IR::Opcode::FPMul32:
    case IR::Opcode::FPFma32:
    case IR::Opcode::FPMax32:
    case IR::Opcode::FPMin32: {
        if (depth == 0) {
            return -1;
        }
        int found = -1;
        for (size_t i = 0; i < inst->NumArgs(); ++i) {
            const int comp = FragCoordComponent(inst->Arg(i), depth - 1);
            if (comp >= 0) {
                if (found >= 0 && found != comp) {
                    return -1;
                }
                found = comp;
            }
        }
        return found;
    }
    default:
        return -1;
    }
}
} // namespace

void LightGridScalePass(IR::Program& program, const RuntimeInfo& runtime_info) {
    const auto& fs = runtime_info.hw.fs;
    const float scale[2] = {std::bit_cast<float>(fs.light_grid_scale_x),
                            std::bit_cast<float>(fs.light_grid_scale_y)};
    const bool scaled = fs.light_grid_scale_x != 0 && fs.light_grid_scale_y != 0;
    u32 found = 0;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            // cell = uint(position) >> shift, the shift a value from memory (not a constant).
            if (inst.GetOpcode() != IR::Opcode::ShiftRightLogical32 || inst.Arg(1).IsImmediate()) {
                continue;
            }
            const IR::Inst* convert = inst.Arg(0).TryInst();
            if (!convert || inst.Arg(0).IsImmediate() ||
                convert->GetOpcode() != IR::Opcode::ConvertU32F32) {
                continue;
            }
            const int comp = FragCoordComponent(convert->Arg(0), 4);
            if (comp < 0) {
                continue;
            }
            ++found;
            if (!scaled) {
                continue;
            }
            // A new conversion of the scaled position: the original may have other users.
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            const IR::F32 position{convert->Arg(0)};
            const IR::F32 grid_position{ir.FPMul(position, ir.Imm32(scale[comp]))};
            inst.SetArg(0, ir.ConvertFToU(32, grid_position));
        }
    }
    if (found == 0) {
        return;
    }
    program.info.uses_light_grid = true;
    static u32 logged = 0;
    if (logged++ < 64) {
        std::printf("Shader: light grid lookup in %016llx (%u), scale %.4f x %.4f\n",
                    static_cast<unsigned long long>(program.info.pgm_hash), found,
                    scaled ? scale[0] : 1.0, scaled ? scale[1] : 1.0);
    }
}

} // namespace Shader::Optimization
