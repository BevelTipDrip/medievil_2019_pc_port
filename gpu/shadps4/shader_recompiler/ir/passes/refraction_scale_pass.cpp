// SPDX-License-Identifier: GPL-2.0-or-later
// MediEvil: UE4 4.21's distortion accumulate shaders (refraction of water and glass) compute the
// screen shift as normal.xy * (IOR - 1) * ... * DistortionParams * 0.00023, where DistortionParams is
// (proj00, aspect, UnscaledViewW, UnscaledViewH): the output size, so the shift as a share of the
// screen grows with the output resolution. They test the scene depth at the shifted spot (a closer
// object drops the shift) and store the shift x4 in RGBA8, which holds at most 0.25 of the screen.
// At 4K the Sleeping Village water's shift is 0.31-0.34: stored as 0.25, so the apply pass samples a
// spot the depth test never saw (Dan's sword copied into the water, a hard line) where the PS4 at
// 1080p shifts 0.13-0.21 and tests what it samples (research/patches card refraction-scale).
//
// Every fragment shader with the constant in such a product is marked
// (Info::uses_refraction_scale); the pipeline cache picks a variant with the reference width
// (refraction_ref_width, ini refraction_ps4) that multiplies the constant by reference / the
// UnscaledViewW the shader reads, so the shift is the share of the screen a 1920-wide output gets.
// The same shaders carry the material's time pulse: rippling for half of a 10 s cycle, still for the
// other half, when the refraction's hard line at the top edge stands out. refraction_ripples
// (ini water_ripples_always) makes it ripple through both halves.

#include <cmath>
#include <cstdio>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

namespace {
constexpr float DistortionConstant = 0.000230000005103647708892822265625f;

struct ParamsRead {
    const IR::Inst* read{}; // a ReadConstBuffer of DistortionParams (index 0: proj00)
    bool has_proj{};        // index 0 reached
    bool has_size{};        // index 2 or 3 reached
};

/// Walks a product (FPMul32, at most `depth` levels) for DistortionParams dwords read as floats:
/// BitCastF32U32(ReadConstBuffer(handle, #index)), all from one buffer.
void FindParams(const IR::Value& value, int depth, ParamsRead& found) {
    if (value.IsImmediate()) {
        return;
    }
    const IR::Inst* inst = value.TryInst();
    if (!inst) {
        return;
    }
    if (inst->GetOpcode() == IR::Opcode::BitCastF32U32) {
        const IR::Inst* read = inst->Arg(0).IsImmediate() ? nullptr : inst->Arg(0).TryInst();
        if (!read || read->GetOpcode() != IR::Opcode::ReadConstBuffer ||
            !read->Arg(0).IsImmediate() || !read->Arg(1).IsImmediate()) {
            return;
        }
        if (found.read && found.read->Arg(0).U32() != read->Arg(0).U32()) {
            return;
        }
        found.read = found.read ? found.read : read;
        const u32 index = read->Arg(1).U32();
        found.has_proj |= index == 0;
        found.has_size |= index == 2 || index == 3;
        return;
    }
    if (inst->GetOpcode() != IR::Opcode::FPMul32 || depth == 0) {
        return;
    }
    FindParams(inst->Arg(0), depth - 1, found);
    FindParams(inst->Arg(1), depth - 1, found);
}

/// The side (0, 1) of a multiply by +-DistortionConstant, else -1.
int ConstantSide(const IR::Inst& inst) {
    if (inst.GetOpcode() != IR::Opcode::FPMul32) {
        return -1;
    }
    for (int side = 0; side < 2; ++side) {
        const IR::Value arg = inst.Arg(side);
        if (arg.IsImmediate() && arg.Type() == IR::Type::F32 &&
            std::abs(arg.F32()) == DistortionConstant) {
            return side;
        }
    }
    return -1;
}
} // namespace

void RefractionScalePass(IR::Program& program, const RuntimeInfo& runtime_info) {
    const u32 reference = runtime_info.hw.fs.refraction_ref_width;
    u32 found = 0;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            const int side = ConstantSide(inst);
            if (side < 0) {
                continue;
            }
            ParamsRead params;
            FindParams(inst.Arg(1 - side), 4, params);
            if (!params.read || !params.has_proj || !params.has_size) {
                continue;
            }
            ++found;
            if (reference == 0) {
                continue;
            }
            // constant * reference / max(UnscaledViewW, 1), UnscaledViewW read again right here.
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            const auto info = params.read->Flags<IR::BufferInstInfo>();
            const IR::U32 width_bits{ir.ReadConstBuffer(params.read->Arg(0), ir.Imm32(2u), info)};
            const IR::F32 width{ir.BitCast<IR::F32, IR::U32>(width_bits)};
            const IR::F32 scale{ir.FPMul(ir.Imm32(static_cast<float>(reference)),
                                         ir.FPRecip(ir.FPMax(width, ir.Imm32(1.f))))};
            inst.SetArg(side, ir.FPMul(ir.Imm32(inst.Arg(side).F32()), scale));
        }
    }
    if (found == 0) {
        return;
    }
    program.info.uses_refraction_scale = true;
    // The material's time pulse, saturate(sin(fract(time * rate))) (GCN v_sin with the clamp
    // modifier): 0 for half of each cycle, still water. abs() ripples through that half too.
    u32 pulses = 0;
    if (runtime_info.hw.fs.refraction_ripples) {
        for (IR::Block* const block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() != IR::Opcode::FPSaturate32 || inst.Arg(0).IsImmediate()) {
                    continue;
                }
                const IR::Inst* sin = inst.Arg(0).TryInst();
                if (!sin || sin->GetOpcode() != IR::Opcode::FPSin || sin->Arg(0).IsImmediate()) {
                    continue;
                }
                const IR::Inst* fract = sin->Arg(0).TryInst();
                if (!fract || fract->GetOpcode() != IR::Opcode::FPFract32) {
                    continue;
                }
                inst.ReplaceOpcode(IR::Opcode::FPAbs32);
                ++pulses;
            }
        }
    }
    static u32 logged = 0;
    if (logged++ < 64) {
        std::printf("Shader: refraction constant in %016llx (%u), reference width %u%s, ripple "
                    "pulses %u\n",
                    static_cast<unsigned long long>(program.info.pgm_hash), found, reference,
                    reference ? "" : " (game)", pulses);
        std::fflush(stdout);
    }
}

} // namespace Shader::Optimization
