/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEDIEVIL_LEVEL_HOOK_H
#define MEDIEVIL_LEVEL_HOOK_H
#include <stdint.h>
#include <string.h>

/* MediEvil (CUSA11227) level change: the Blueprint OpenLevel native (UGameplayStatics::execOpenLevel,
 * 01.00: 0x290ed50) calls UGameplayStatics::OpenLevel (0x209f690) at 0x290eeda with rdi
 * WorldContextObject, rsi LevelName, edx bAbsolute, rcx Options. The game never stops its intro
 * movie after the skip (research/docs/study-intro-movie.md): the stub suspends the movies still
 * playing (medievil_level_change), then continues to OpenLevel. Per game build
 * (research/docs/plan-0102.md); the build whose signatures match is used. */
typedef struct { uint32_t call, open_level; } MedievilLevelSites;
static const MedievilLevelSites medievil_level_builds[] = {
    {0x290eeda, 0x209f690}, /* 01.00 */
    {0x2911c9a, 0x20a2310}, /* 01.02 */
};
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
/* The build whose OpenLevel call (checked by its target: the displacement differs between builds)
 * and OpenLevel prologue match, or NULL. */
static inline const MedievilLevelSites *medievil_level_find(const unsigned char *image, uint64_t size) {
    for (size_t i = 0; i < sizeof(medievil_level_builds) / sizeof(medievil_level_builds[0]); ++i) {
        const MedievilLevelSites *s = &medievil_level_builds[i];
        if (size < s->call + 0x40 || size < s->open_level + sizeof(medievil_open_level_bytes)) continue;
        int32_t rel;
        memcpy(&rel, image + s->call + 1, 4);
        if (!memcmp(image + s->call - 12, medievil_open_level_call_bytes, 13) &&
            (int64_t)s->call + 5 + rel == (int64_t)s->open_level &&
            !memcmp(image + s->open_level, medievil_open_level_bytes, sizeof(medievil_open_level_bytes)))
            return s;
    }
    return NULL;
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
/* The level's name (per-level settings, research/docs/plan-hdr-final.md; research-data/fname-research-20261122):
 * OpenLevel's LevelName is an FName by value (rsi: ComparisonIndex in the low 32 bits, Number in the high 32).
 * Its text is read from UE 4.21's name table: the TNameEntryArray pointer at names_ptr (a lazily created heap
 * object: FNameEntry **chunks[128] at +0, int32 count at +0x400; 16384 entries a chunk), entry = chunks[index >> 14]
 * [index & 0x3fff], FNameEntry: int32 flags (+0, bit 0 wide), characters at +0x10 (ANSI or UTF-16); Number != 0
 * appends "_<Number - 1>". The sites: OpenLevel +0x82 calls FName::ToString(), which calls
 * FName::ToString(FString&) at +0x3e; that one loads names_ptr (disp32 at +0x0e) and its body (+0x54) has the
 * layout above. Checked as a chain per build. */
typedef struct { uint32_t open_level, to_string, to_string_out, names_ptr; } MedievilNameSites;
static const MedievilNameSites medievil_name_builds[] = {
    {0x209f690, 0x90cdd0, 0x90cef0, 0x5a13890}, /* 01.00 */
    {0x20a2310, 0x90f990, 0x90fab0, 0x5a138d0}, /* 01.02 */
};
static const unsigned char medievil_to_string_head[] = {
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53, /* push rbp .. push rbx */
    0x48,0x8b,0x1d                                          /* mov rbx, [rip + disp32] (names_ptr) */
};
static const unsigned char medievil_to_string_body[] = {    /* at +0x54 */
    0x41,0x8b,0x06,                     /* mov eax, [r14]            ComparisonIndex */
    0x89,0xc1,0xc1,0xf9,0x1f,0xc1,0xe9,0x12,0x01,0xc1,0x89,0xca,
    0x81,0xe1,0x00,0xc0,0xff,0xff,      /* and ecx, 0xffffc000       16384 a chunk */
    0xc1,0xfa,0x0e,                     /* sar edx, 14 */
    0x29,0xc8,0x48,0x63,0xca,0x48,0x98,
    0x48,0x8b,0x0c,0xcb,                /* mov rcx, [rbx + rcx*8]    chunk */
    0x48,0x8b,0x1c,0xc1,                /* mov rbx, [rcx + rax*8]    FNameEntry* */
    0x48,0x85,0xdb,0x0f,0x84,0x93,0x00,0x00,0x00,
    0x45,0x8b,0x66,0x04,                /* mov r12d, [r14 + 4]       Number */
    0xf6,0x03,0x01,                     /* test byte [rbx], 1        wide */
    0x0f,0x85,0xb9,0x00,0x00,0x00,
    0x48,0x8d,0x7b,0x10                 /* lea rdi, [rbx + 0x10]     ANSI characters */
};
static inline int64_t medievil_rel_target(const unsigned char *image, uint64_t at, uint64_t next) {
    int32_t rel;
    memcpy(&rel, image + at, 4);
    return (int64_t)next + rel;
}
/* The build whose chain matches, or NULL. */
static inline const MedievilNameSites *medievil_name_find(const unsigned char *image, uint64_t size) {
    for (size_t i = 0; i < sizeof(medievil_name_builds) / sizeof(medievil_name_builds[0]); ++i) {
        const MedievilNameSites *s = &medievil_name_builds[i];
        if (size < (uint64_t)s->names_ptr + 8 || size < (uint64_t)s->open_level + 0x87 ||
            size < (uint64_t)s->to_string + 0x43 || size < (uint64_t)s->to_string_out + 0x95) continue;
        const unsigned char *f = image + s->to_string_out;
        if (image[s->open_level + 0x82] == 0xe8 &&
            medievil_rel_target(image, s->open_level + 0x83, s->open_level + 0x87) == s->to_string &&
            image[s->to_string + 0x3e] == 0xe8 &&
            medievil_rel_target(image, s->to_string + 0x3f, s->to_string + 0x43) == s->to_string_out &&
            !memcmp(f, medievil_to_string_head, sizeof(medievil_to_string_head)) &&
            medievil_rel_target(image, s->to_string_out + 0x0e, s->to_string_out + 0x12) == s->names_ptr &&
            !memcmp(f + 0x54, medievil_to_string_body, sizeof(medievil_to_string_body)))
            return s;
    }
    return NULL;
}
#endif
