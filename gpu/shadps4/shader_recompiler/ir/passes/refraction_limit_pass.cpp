// SPDX-License-Identifier: GPL-2.0-or-later
// MediEvil: UE4 4.21's distortion apply pass reads the accumulated offset buffer (RGBA8, the shift
// stored as x = -dx, y = +dy, z = +dx, w = -dy, times 4) and samples the scene at
// uv + (texel.xy - texel.zw) * 0.25, falling back to uv when that leaves the view. On the Sleeping
// Village water every pixel gets an up / down shift of 0.13-0.25 of the screen (sideways 0.001-0.008),
// so the water shows a translated copy of the scene above it, cut by a hard line where the fallback
// starts (research/patches card refraction-limit). The PS4 shows none of it; why is still open.
//
// Every fragment shader with this offset read is marked (Info::uses_refraction_offset); the pipeline
// cache picks a variant with the limit (refraction_limit, ini refraction_limit), where the offset is
// shortened to that length (a fraction of the screen), its direction kept.

#include <bit>
#include <cstdio>
#include <unordered_map>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

namespace {
struct OffsetRead {
    IR::Inst* texel{};   // the sample whose components hold the offset
    IR::Inst* axis[2]{}; // (texel[axis] - texel[axis + 2]) * 0.25 for x, y
};

/// The component of a vector a value is extracted from (-1: none), and the vector.
int ExtractedComponent(const IR::Value& value, IR::Inst*& vector) {
    if (value.IsImmediate()) {
        return -1;
    }
    IR::Inst* inst = value.TryInst();
    if (!inst || inst->GetOpcode() != IR::Opcode::CompositeExtractF32x4 ||
        inst->Arg(0).IsImmediate() || !inst->Arg(1).IsImmediate()) {
        return -1;
    }
    vector = inst->Arg(0).TryInst();
    return vector ? static_cast<int>(inst->Arg(1).U32()) : -1;
}

/// The axis (0: x, 1: y) of an offset component (texel[a] - texel[a + 2]) * 0.25, else -1.
int OffsetAxis(const IR::Inst& mul, IR::Inst*& texel) {
    if (mul.GetOpcode() != IR::Opcode::FPMul32) {
        return -1;
    }
    for (int side = 0; side < 2; ++side) {
        const IR::Value scale = mul.Arg(side);
        const IR::Value diff = mul.Arg(1 - side);
        if (!scale.IsImmediate() || scale.Type() != IR::Type::F32 || scale.F32() != 0.25f ||
            diff.IsImmediate()) {
            continue;
        }
        const IR::Inst* sub = diff.TryInst();
        if (!sub || sub->GetOpcode() != IR::Opcode::FPSub32) {
            continue;
        }
        IR::Inst* a = nullptr;
        IR::Inst* b = nullptr;
        const int ca = ExtractedComponent(sub->Arg(0), a);
        const int cb = ExtractedComponent(sub->Arg(1), b);
        if (a && a == b && (ca == 0 || ca == 1) && cb == ca + 2) {
            texel = a;
            return ca;
        }
    }
    return -1;
}
} // namespace

void RefractionLimitPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    const u32 limit_bits = runtime_info.hw.fs.refraction_limit;
    const float limit = std::bit_cast<float>(limit_bits);
    std::unordered_map<IR::Inst*, OffsetRead> reads;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            IR::Inst* texel = nullptr;
            const int axis = OffsetAxis(inst, texel);
            if (axis < 0) {
                continue;
            }
            auto& read = reads[texel];
            read.texel = texel;
            read.axis[axis] = read.axis[axis] ? read.axis[axis] : &inst;
        }
    }
    u32 found = 0;
    for (auto& [texel, read] : reads) {
        if (!read.axis[0] || !read.axis[1]) {
            continue;
        }
        ++found;
        if (limit_bits == 0 || !(limit > 0.f)) {
            continue;
        }
        // Right after the sample, from new extracts, so the capped offset is defined before any
        // user of either component.
        IR::Block& block = *texel->GetParent();
        IR::IREmitter ir{block, std::next(IR::Block::InstructionList::s_iterator_to(*texel))};
        const IR::Value vector{texel};
        const IR::F32 quarter{ir.Imm32(0.25f)};
        IR::F32 offset[2];
        for (int a = 0; a < 2; ++a) {
            const IR::F32 hi{ir.CompositeExtract(vector, a)};
            const IR::F32 lo{ir.CompositeExtract(vector, a + 2)};
            offset[a] = IR::F32{ir.FPMul(ir.FPSub(hi, lo), quarter)};
        }
        // scale = min(1, limit / max(length, tiny)): offsets within the limit stay as they are.
        const IR::F32 length_sq{ir.FPFma(offset[0], offset[0], ir.FPMul(offset[1], offset[1]))};
        const IR::F32 length{ir.FPSqrt(length_sq)};
        const IR::F32 inv{ir.FPRecip(ir.FPMax(length, ir.Imm32(1e-6f)))};
        const IR::F32 scale{ir.FPMin(ir.Imm32(1.f), ir.FPMul(ir.Imm32(limit), inv))};
        for (int a = 0; a < 2; ++a) {
            read.axis[a]->ReplaceUsesWith(ir.FPMul(offset[a], scale));
        }
    }
    if (found == 0) {
        return;
    }
    program.info.uses_refraction_offset = true;
    static u32 logged = 0;
    if (logged++ < 64) {
        std::printf("Shader: refraction offset read in %016llx (%u), limit %s%.4f\n",
                    static_cast<unsigned long long>(program.info.pgm_hash), found,
                    limit_bits ? "" : "off ", limit_bits ? limit : 0.0f);
    }
}

} // namespace Shader::Optimization
