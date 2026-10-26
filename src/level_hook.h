/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEDIEVIL_LEVEL_HOOK_H
#define MEDIEVIL_LEVEL_HOOK_H
#include <stdint.h>
#include <string.h>

/* MediEvil (CUSA11227 01.00) level change: the Blueprint OpenLevel native
 * (UGameplayStatics::execOpenLevel, 0x290ed50) calls UGameplayStatics::OpenLevel (0x209f690) at
 * 0x290eeda with rdi WorldContextObject, rsi LevelName, edx bAbsolute, rcx Options. The game never
 * stops its intro movie after the skip (research/docs/study-intro-movie.md): the stub suspends the
 * movies still playing (medievil_level_change), then continues to OpenLevel. */
enum { MEDIEVIL_OPEN_LEVEL_CALL = 0x290eeda, MEDIEVIL_OPEN_LEVEL = 0x209f690 };
static const unsigned char medievil_open_level_call_bytes[] = {
    0x48,0x89,0xdf,                     /* mov rdi, rbx */
    0x4c,0x89,0xfe,                     /* mov rsi, r15 */
    0x44,0x89,0xf2,                     /* mov edx, r14d */
    0x4c,0x89,0xe1,                     /* mov rcx, r12 */
    0xe8,0xb1,0x07,0x79,0xff            /* call OpenLevel */
};
static const unsigned char medievil_open_level_bytes[] = {
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xd8,0x00,0x00,0x00
};
static inline int medievil_level_signatures_match(const unsigned char *image, uint64_t size) {
    if (size < MEDIEVIL_OPEN_LEVEL_CALL + 0x40) return 0;
    return !memcmp(image + MEDIEVIL_OPEN_LEVEL_CALL - 12, medievil_open_level_call_bytes,
                   sizeof(medievil_open_level_call_bytes)) &&
           !memcmp(image + MEDIEVIL_OPEN_LEVEL, medievil_open_level_bytes, sizeof(medievil_open_level_bytes));
}
/* Saves the argument registers, calls the helper, restores them and jumps to OpenLevel. rsp is
 * 16-byte aligned at the helper call (8 at entry, 6 pushes, 8 more). */
enum { MEDIEVIL_LEVEL_STUB_LENGTH = 48 };
static inline void medievil_level_build_stub(unsigned char code[MEDIEVIL_LEVEL_STUB_LENGTH], uintptr_t helper,
                                             uintptr_t open_level) {
    static const unsigned char pattern[MEDIEVIL_LEVEL_STUB_LENGTH] = {
        0x57,0x56,0x52,0x51,                /* push rdi; push rsi; push rdx; push rcx */
        0x41,0x50,0x41,0x51,                /* push r8; push r9 */
        0x48,0x83,0xec,0x08,                /* sub rsp, 8 */
        0x48,0xb8,0,0,0,0,0,0,0,0,          /* movabs rax, helper */
        0xff,0xd0,                          /* call rax */
        0x48,0x83,0xc4,0x08,                /* add rsp, 8 */
        0x41,0x59,0x41,0x58,                /* pop r9; pop r8 */
        0x59,0x5a,0x5e,0x5f,                /* pop rcx; pop rdx; pop rsi; pop rdi */
        0x48,0xb8,0,0,0,0,0,0,0,0,          /* movabs rax, open_level */
        0xff,0xe0                           /* jmp rax */
    };
    memcpy(code, pattern, sizeof(pattern));
    memcpy(code + 14, &helper, 8);
    memcpy(code + 38, &open_level, 8);
}
#endif
