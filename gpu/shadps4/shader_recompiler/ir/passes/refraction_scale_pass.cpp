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
// The apply pass falls back to the unshifted image where the shifted spot leaves the screen: a hard
// line, past it the pond bed with no ripples. refraction_edge_stretch (ini refraction_edge_stretch)
// squeezes the shifted spot smoothly into the screen before the depth test instead: near the edge
// an almost exact copy, stretching further from it.

#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

namespace {
constexpr float DistortionConstant = 0.000230000005103647708892822265625f;
// Edge stretch: the view as buffer UV 0-1 (the port's views fill their render targets), a 15% band.
constexpr float EdgeLo = 0.001f, EdgeHi = 0.999f, EdgeBand = 0.15f;

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
/// The shifted spot t squeezed into [EdgeLo, EdgeHi]: within EdgeBand of an edge,
/// edge + band * exp((t - (edge + band)) / band), continuous with t in value and slope.
IR::F32 SoftEdge(IR::IREmitter& ir, const IR::F32& t) {
    constexpr float k = 1.4426950408889634f / EdgeBand; // log2(e) / band
    const IR::F32 lo_band{ir.Imm32(EdgeLo + EdgeBand)};
    const IR::F32 below{ir.FPFma(ir.Imm32(EdgeBand),
                                 ir.FPExp2(ir.FPMul(ir.FPSub(t, lo_band), ir.Imm32(k))),
                                 ir.Imm32(EdgeLo))};
    const IR::F32 t1{ir.Select(ir.FPLessThan(t, lo_band), below, t)};
    const IR::F32 hi_band{ir.Imm32(EdgeHi - EdgeBand)};
    const IR::F32 above{ir.FPFma(ir.Imm32(-EdgeBand),
                                 ir.FPExp2(ir.FPMul(ir.FPSub(hi_band, t1), ir.Imm32(k))),
                                 ir.Imm32(EdgeHi))};
    return IR::F32{ir.Select(ir.FPGreaterThan(t1, hi_band), above, t1)};
}

/// Index of each instruction of a block, in order.
std::unordered_map<const IR::Inst*, size_t> Order(IR::Block& block) {
    std::unordered_map<const IR::Inst*, size_t> order;
    for (const IR::Inst& inst : block.Instructions()) {
        order.emplace(&inst, order.size());
    }
    return order;
}
} // namespace

void RefractionScalePass(IR::Program& program, const RuntimeInfo& runtime_info) {
    const u32 reference = runtime_info.hw.fs.refraction_ref_width;
    u32 found = 0;
    // The shift's multiplies by their per-axis factor: one feeds the depth test's spot
    // (FPAdd with the pixel's UV), the other the stored shift.
    std::unordered_map<const IR::Inst*, std::vector<IR::Inst*>> axes;
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
            if (const IR::Inst* factor = inst.Arg(1 - side).TryInst()) {
                axes[factor].push_back(&inst);
            }
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
    u32 edges = 0;
    if (runtime_info.hw.fs.refraction_edge_stretch) {
        for (auto& [factor, muls] : axes) {
            // The depth test's spot: uv + shift (FPAdd32 user of one multiply).
            IR::Inst* add = nullptr;
            IR::Inst* tested = nullptr;
            for (IR::Inst* mul : muls) {
                for (const IR::Use& use : mul->Uses()) {
                    if (!add && use.user->GetOpcode() == IR::Opcode::FPAdd32) {
                        add = use.user;
                        tested = mul;
                    }
                }
            }
            if (!add) {
                continue;
            }
            const IR::Value uv = add->Arg(0).TryInst() == tested ? add->Arg(1) : add->Arg(0);
            // The stored shift's multiplies must come after the add, in its block.
            IR::Block& block = *add->GetParent();
            const auto order = Order(block);
            std::vector<IR::Inst*> stored;
            bool placed = true;
            for (IR::Inst* mul : muls) {
                if (mul == tested) {
                    continue;
                }
                const auto it = order.find(mul);
                placed &= it != order.end() && it->second > order.at(add);
                stored.push_back(mul);
            }
            if (!placed || stored.empty()) {
                continue;
            }
            const std::vector<IR::Use> add_uses(add->Uses().begin(), add->Uses().end());
            IR::IREmitter ir{block, std::next(IR::Block::InstructionList::s_iterator_to(*add))};
            const IR::F32 spot = SoftEdge(ir, IR::F32{IR::Value{add}});
            const IR::F32 shift{ir.FPSub(spot, IR::F32{uv})};
            for (const IR::Use& use : add_uses) {
                use.user->SetArg(use.operand, spot);
            }
            for (IR::Inst* mul : stored) {
                mul->ReplaceUsesWith(shift);
            }
            ++edges;
        }
    }
    static u32 logged = 0;
    if (logged++ < 64) {
        std::printf("Shader: refraction constant in %016llx (%u), reference width %u%s, ripple "
                    "pulses %u, stretched axes %u\n",
                    static_cast<unsigned long long>(program.info.pgm_hash), found, reference,
                    reference ? "" : " (game)", pulses, edges);
        std::fflush(stdout);
    }
}

} // namespace Shader::Optimization
