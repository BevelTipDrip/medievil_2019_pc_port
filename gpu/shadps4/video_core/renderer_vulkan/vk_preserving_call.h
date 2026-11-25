// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <type_traits>

namespace Vulkan {

// gow3 (v2.5.0 start-up crash, 2026-11-25): on some players' systems a component in the Vulkan call
// chain (driver, implicit layer or injected tool; the dump showed OBS's graphics hook in the path)
// returned from vkGetPipelineKeyKHR with rdi overwritten (= 1). The Windows x64 ABI makes rbx, rbp,
// rsi, rdi, r12-r15 and xmm6-xmm15 callee-saved; 2.5.0's code kept `this` in rdi across that call
// and crashed, 2.4.0's kept it in r13 and did not. PreservingCall makes the caller immune: the empty
// asm after the call declares every callee-saved register (but rbp, the frame pointer) clobbered,
// so the compiler saves them in this function's prologue, restores them in its epilogue and keeps
// nothing live in them across the call. Costs a few register saves per call; used only for the
// pipeline-binary functions (start-up and pipeline cache loads / saves, never per frame).
// Elsewhere (System V: rsi, rdi and every xmm are caller-saved anyway) it is a plain call.
template <typename Fn, typename... Args>
[[gnu::noinline]] auto PreservingCall(Fn fn, Args... args) {
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
#define GOW3_PRESERVING_CLOBBERS                                                                   \
    asm volatile("" ::: "rbx", "rsi", "rdi", "r12", "r13", "r14", "r15", "xmm6", "xmm7", "xmm8",   \
                 "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory")
#else
#define GOW3_PRESERVING_CLOBBERS asm volatile("" ::: "memory")
#endif
    if constexpr (std::is_void_v<decltype(fn(args...))>) {
        fn(args...);
        GOW3_PRESERVING_CLOBBERS;
    } else {
        auto result = fn(args...);
        GOW3_PRESERVING_CLOBBERS;
        return result;
    }
#undef GOW3_PRESERVING_CLOBBERS
}

} // namespace Vulkan
