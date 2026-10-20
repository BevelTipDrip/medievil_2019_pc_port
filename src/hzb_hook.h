/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEDIEVIL_HZB_HOOK_H
#define MEDIEVIL_HZB_HOOK_H
#include <stdint.h>
#include <string.h>

/* MediEvil (CUSA11227 01.00) HZB occlusion remap (research/docs/study-hzb-double-buffer.md,
 * GOW3_HZB_REMAP=1): UE4's FHZBOcclusionTester::MapResults (image offset 0x15340a0) has one
 * caller, 0x158c4ac (rdi = FSceneViewState + 0x170, rsi = RHICmdList; the caller keeps rdx in
 * rbx across the call). The call goes to a stub that runs MapResults, then the host helper with
 * the tester, which may point the tester's results (+0x40) at a remapped host copy. */
enum {
    MEDIEVIL_HZB_CALL = 0x158c4ac,
    MEDIEVIL_HZB_MAP_RESULTS = 0x15340a0,
    MEDIEVIL_HZB_IS_VISIBLE = 0x1534260,
    MEDIEVIL_HZB_HISTORY_WALK = 0x158d4df,
};
static const unsigned char medievil_hzb_call_bytes[] = {
    0x48,0x8d,0xba,0x70,0x01,0x00,0x00, /* lea rdi, [rdx+0x170] */
    0x48,0x89,0xd3,                     /* mov rbx, rdx */
    0xe8,0xef,0x7b,0xfa,0xff,           /* call MapResults */
    0x48,0x89,0xda                      /* mov rdx, rbx */
};
/* MapResults: [rdi+0x48] ValidFrameNumber. */
static const unsigned char medievil_hzb_map_bytes[] = {
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18
};
/* IsVisible: ResultsBuffer at [rdi+0x40], 8x8 blocks of 4-byte texels, 256 a row. */
static const unsigned char medievil_hzb_visible_bytes[] = {
    0x55,0x48,0x89,0xe5,0x89,0xf1,0x89,0xf0,0x83,0xe6,0x07,0xc1,0xe8,0x08,0xc1,0xe9,0x03,0x89,0xca,
    0x25,0xf8,0xff,0x3f,0x00,0x83,0xe1,0x07,0x09,0xc1,0x48,0x8b,0x47,0x40
};
/* The game's occlusion history lookup: hash at ViewState+0x13a0 (0: inline at +0x1398), size
 * +0x13a8, elements 0x58 bytes, PrimitiveId +0, CustomIndex +0x44, next +0x50. */
static const unsigned char medievil_hzb_walk_bytes[] = {
    0x48,0x8b,0x8e,0xa0,0x13,0x00,0x00, /* mov rcx, [rsi+0x13a0] */
    0x8b,0xb6,0xa8,0x13,0x00,0x00,      /* mov esi, [rsi+0x13a8] */
    0x48,0x85,0xc9,0x48,0x0f,0x44,0x94,0x24,0xf8,0x01,0x00,0x00,0xff,0xce,0x21,0xc6,0x48,0x63,0xc6,
    0x48,0x09,0xca,0x8b,0x04,0x82,0x83,0xf8,0xff,0x0f,0x84,0xb2,0x01,0x00,0x00,
    0x48,0x8b,0x8c,0x24,0x20,0x01,0x00,0x00,0x48,0x8b,0x19,0x0f,0x1f,0x80,0x00,0x00,0x00,0x00,
    0x48,0x98,0x4c,0x6b,0xe0,0x58,0x42,0x39,0x3c,0x23,0x75,0x0a,0x42,0x8b,0x44,0x23,0x44
};

static inline int medievil_hzb_signatures_match(const unsigned char *image, uint64_t size) {
    if (size < MEDIEVIL_HZB_CALL + 0x40) return 0;
    return !memcmp(image + MEDIEVIL_HZB_CALL - 10, medievil_hzb_call_bytes, sizeof(medievil_hzb_call_bytes)) &&
           !memcmp(image + MEDIEVIL_HZB_MAP_RESULTS, medievil_hzb_map_bytes, sizeof(medievil_hzb_map_bytes)) &&
           !memcmp(image + MEDIEVIL_HZB_IS_VISIBLE, medievil_hzb_visible_bytes, sizeof(medievil_hzb_visible_bytes)) &&
           !memcmp(image + MEDIEVIL_HZB_HISTORY_WALK, medievil_hzb_walk_bytes, sizeof(medievil_hzb_walk_bytes));
}

/* push rdi; call map_results; mov rdi, [rsp]; call helper; pop rdi; ret. rsp is 16-byte
 * aligned at both calls (8 at entry, the push makes 0). */
enum { MEDIEVIL_HZB_STUB_LENGTH = 31 };
static inline void medievil_hzb_build_stub(unsigned char code[MEDIEVIL_HZB_STUB_LENGTH],
                                           uintptr_t map_results, uintptr_t helper) {
    static const unsigned char pattern[MEDIEVIL_HZB_STUB_LENGTH] = {
        0x57,                               /* push rdi */
        0x48,0xb8,0,0,0,0,0,0,0,0,          /* movabs rax, map_results */
        0xff,0xd0,                          /* call rax */
        0x48,0x8b,0x3c,0x24,                /* mov rdi, [rsp] */
        0x48,0xb8,0,0,0,0,0,0,0,0,          /* movabs rax, helper */
        0xff,0xd0,                          /* call rax */
        0x5f,                               /* pop rdi */
        0xc3                                /* ret */
    };
    memcpy(code, pattern, sizeof(pattern));
    memcpy(code + 3, &map_results, 8);
    memcpy(code + 19, &helper, 8);
}
#endif
