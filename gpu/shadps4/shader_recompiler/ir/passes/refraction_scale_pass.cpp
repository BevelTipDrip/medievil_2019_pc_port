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
// an almost exact copy, stretching further from it. Modes 2 and 3 also fill the spots its depth
// test drops (a closer object there) with a vertical ripple, mode 3 first trying shorter shifts.

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
// Fallback where the depth test drops the shift: a vertical ripple, 1.2% of the screen down
// plus 6x the sideways shift, at most 3% (RenderDoc replays of the gate frame 22245).
constexpr float FallbackLean = 0.012f, FallbackRipple = 6.f, FallbackMax = 0.03f;

struct EdgeAxis {
    IR::Inst* add;     // uv + shift: the depth test's spot before the stretch
    IR::Value uv;      // the pixel's buffer UV
    IR::Value spot;    // the stretched spot
    IR::Value shift;   // spot - uv
    bool x;            // the +0.00023 axis (x); y has -0.00023
};

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

/// Copies the instructions that lead from the substituted values to a root (the depth test of
/// another spot) in front of `point`. Fails on a phi or an instruction with side effects.
struct ChainCopy {
    IR::Block& block;
    IR::Block::iterator point;
    std::unordered_map<const IR::Inst*, IR::Value> values{}; // substitutions, then the copies
    std::unordered_map<const IR::Inst*, bool> depends{};
    bool failed = false;
    u32 copies = 0;

    bool Depends(const IR::Inst* inst, int depth) {
        if (values.contains(inst)) {
            return true;
        }
        if (const auto it = depends.find(inst); it != depends.end()) {
            return it->second;
        }
        bool result = false;
        if (depth > 0 && inst->GetOpcode() != IR::Opcode::Phi) {
            for (size_t i = 0; i < inst->NumArgs() && !result; ++i) {
                const IR::Value arg = inst->Arg(i);
                const IR::Inst* source = arg.IsImmediate() ? nullptr : arg.TryInst();
                result = source && Depends(source, depth - 1);
            }
        }
        depends[inst] = result;
        return result;
    }

    IR::Value Copy(const IR::Value& value) {
        const IR::Inst* inst = value.IsImmediate() ? nullptr : value.TryInst();
        if (!inst) {
            return value;
        }
        if (const auto it = values.find(inst); it != values.end()) {
            return it->second;
        }
        if (!Depends(inst, 64)) {
            return value;
        }
        if (inst->GetOpcode() == IR::Opcode::Phi || inst->MayHaveSideEffects()) {
            failed = true;
            return value;
        }
        std::vector<IR::Value> args;
        for (size_t i = 0; i < inst->NumArgs(); ++i) {
            args.push_back(Copy(inst->Arg(i)));
        }
        IR::Inst& copy = *block.PrependNewInst(point, *inst);
        for (size_t i = 0; i < args.size(); ++i) {
            copy.SetArg(i, args[i]);
        }
        ++copies;
        const IR::Value result{&copy};
        values.emplace(inst, result);
        return result;
    }
};

/// The multiply of a stored shift by the depth test (FPMul32 user of `shift`), and that test.
IR::Inst* StoredProduct(const IR::Value& shift, IR::Value& test) {
    for (const IR::Use& use : shift.Inst()->Uses()) {
        if (use.user->GetOpcode() == IR::Opcode::FPMul32) {
            test = use.user->Arg(1 - use.operand);
            return use.user;
        }
    }
    return nullptr;
}
} // namespace

void RefractionScalePass(IR::Program& program, const RuntimeInfo& runtime_info) {
    const u32 reference = runtime_info.hw.fs.refraction_ref_width;
    u32 found = 0;
    // The shift's multiplies by their per-axis factor: one feeds the depth test's spot
    // (FPAdd with the pixel's UV), the other the stored shift.
    std::unordered_map<const IR::Inst*, std::vector<IR::Inst*>> axes;
    std::unordered_map<const IR::Inst*, bool> positive; // the multiply's constant sign
    std::vector<EdgeAxis> stretched;
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
            positive[&inst] = inst.Arg(side).F32() > 0.f;
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
            stretched.push_back({add, uv, spot, shift, positive[tested]});
            ++edges;
        }
    }
    // Where the depth test drops the shift (a closer object at the shifted spot), the game shows
    // the bed with no water effect. 2: a vertical ripple leaning down instead, from the sideways
    // shift (all ripple in these shaders); vertical, so it never reaches the occluder's sides.
    // 3: first 2/3, then 1/3 of the shift, each with a copy of the shader's own depth test.
    const char* fallback = "";
    const u32 edge_mode = runtime_info.hw.fs.refraction_edge_stretch;
    if (edge_mode >= 2 && stretched.size() == 2 && stretched[0].x != stretched[1].x) {
        const EdgeAxis& ax = stretched[0].x ? stretched[0] : stretched[1];
        const EdgeAxis& ay = stretched[0].x ? stretched[1] : stretched[0];
        IR::Value test_x, test_y;
        IR::Inst* stored_x = StoredProduct(ax.shift, test_x);
        IR::Inst* stored_y = StoredProduct(ay.shift, test_y);
        IR::Block& block = *ax.add->GetParent();
        const auto order = Order(block);
        const bool placed = stored_x && stored_y && test_x == test_y &&
                            order.contains(stored_x) && order.contains(stored_y);
        if (placed) {
            IR::Inst* first =
                order.at(stored_x) < order.at(stored_y) ? stored_x : stored_y;
            const auto point = IR::Block::InstructionList::s_iterator_to(*first);
            IR::IREmitter ir{block, point};
            const IR::F32 test{test_x};
            const IR::F32 sx{ax.shift}, sy{ay.shift};
            const IR::F32 lean{ir.FPClamp(
                ir.FPFma(sx, ir.Imm32(FallbackRipple), ir.Imm32(FallbackLean)), ir.Imm32(0.f),
                ir.Imm32(FallbackMax))};
            IR::F32 out_x{ir.FPMul(sx, test)};
            IR::F32 out_y{ir.FPFma(ir.FPSub(sy, lean), test, lean)};
            fallback = " ripple fallback";
            if (edge_mode >= 3) {
                const IR::F32 ux{ax.uv}, uy{ay.uv};
                IR::F32 pick_x{ir.Imm32(0.f)}, pick_y{lean};
                bool stepped = true;
                for (const float part : {1.f / 3.f, 2.f / 3.f}) { // the later (larger) one wins
                    const IR::F32 px = SoftEdge(
                        ir, IR::F32{ir.FPFma(ir.FPSub(IR::F32{IR::Value{ax.add}}, ux),
                                             ir.Imm32(part), ux)});
                    const IR::F32 py = SoftEdge(
                        ir, IR::F32{ir.FPFma(ir.FPSub(IR::F32{IR::Value{ay.add}}, uy),
                                             ir.Imm32(part), uy)});
                    ChainCopy copy{block, point};
                    copy.values.emplace(ax.spot.Inst(), px);
                    copy.values.emplace(ay.spot.Inst(), py);
                    const IR::Value part_test = copy.Copy(test_x);
                    if (copy.failed || copy.copies == 0) {
                        stepped = false;
                        break;
                    }
                    const IR::F32 t{part_test};
                    const IR::U1 clear{ir.FPGreaterThanEqual(t, ir.Imm32(0.5f))};
                    pick_x = IR::F32{ir.Select(clear, ir.FPMul(ir.FPSub(px, ux), t), pick_x)};
                    pick_y = IR::F32{ir.Select(clear, ir.FPMul(ir.FPSub(py, uy), t), pick_y)};
                }
                if (stepped) {
                    const IR::U1 full{ir.FPGreaterThanEqual(test, ir.Imm32(0.5f))};
                    out_x = IR::F32{ir.Select(full, out_x, pick_x)};
                    out_y = IR::F32{ir.Select(full, ir.FPMul(sy, test), pick_y)};
                    fallback = " stepped fallback";
                } else {
                    fallback = " ripple fallback (stepped: chain not copied)";
                }
            }
            stored_x->ReplaceUsesWith(out_x);
            stored_y->ReplaceUsesWith(out_y);
        } else {
            fallback = " (fallback: stored shift not found)";
        }
    }
    static u32 logged = 0;
    if (logged++ < 64) {
        std::printf("Shader: refraction constant in %016llx (%u), reference width %u%s, ripple "
                    "pulses %u, stretched axes %u%s\n",
                    static_cast<unsigned long long>(program.info.pgm_hash), found, reference,
                    reference ? "" : " (game)", pulses, edges, fallback);
        std::fflush(stdout);
    }
}

} // namespace Shader::Optimization
