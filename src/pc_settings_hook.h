/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEDIEVIL_PC_SETTINGS_HOOK_H
#define MEDIEVIL_PC_SETTINGS_HOOK_H
#include <stdint.h>
#include <string.h>

/* MediEvil (CUSA11227) "PC Settings" in the pause menu (research/docs/study-pc-settings-entry.md). The override
 * pak (scripts/ue4_pc_settings.py, medievil.ini pc_settings_entry) shows the pause menu's hidden Button_Controls
 * as "PC Settings" and makes its click handler play only the confirm sound: in the pause menu's ubergraph,
 *   let_at   EX_Let (0f) ... EX_Context (19 at +0x12) EX_ObjectConst Default__VibrateWWiseHook ...
 *   call_at  EX_FinalFunction (1c) <UFunction* PostEventAtLocationWithMotion> EX_ObjectConst (20) <sound> ...
 *   jump_at  EX_JumpIfNot (07) <u32 target> EX_InstanceVariable (01) <InCheatMode>; the pak points the target
 *            at pop_at, an EX_PopExecutionFlow (4d), instead of the statement after it (the controls switch).
 * PostEventAtLocationWithMotion's exec is wrapped through its FNameNativePtrPair slot; a call from that site
 * (UFunction Script.Data +0x48 / Num +0x50, FFrame Node +0x10 / Code +0x20; Code is the first argument,
 * call_at + 9, when the exec starts) is the click. Only with the edited jump in memory: without the pak edit
 * the button is hidden and its click would switch the controls. */
typedef struct {
    uint32_t slot, exec;       /* PostEventAtLocationWithMotion's exec-pointer slot and its thunk */
    uint32_t name;             /* "PostEventAtLocationWithMotion" */
    int32_t script_size;       /* the pause menu's ubergraph, in memory */
    uint32_t let_at, call_at, jump_at, pop_at;
    const char *build;
} MedievilPcSettingsSites;
static const MedievilPcSettingsSites medievil_pc_settings_builds[] = {
    {0x4ed3f88, 0x667d20, 0x3c597aa, 0x2253, 0x13d7, 0x13ff, 0x142f, 0x143d, "01.00"},
    {0x4ed4508, 0x66a680, 0x3c5cc3a, 0x226f, 0x14b2, 0x14da, 0x150a, 0x1518, "01.02"},
};
/* push rbp; mov rbp, rsp; push r15; push r14; push rbx; sub rsp, 0x58 (both builds) */
static const unsigned char medievil_pc_settings_prologue[] = {
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x83,0xec,0x58
};

/* The build whose slot (relocated: holds image + exec), name and prologue match, or NULL. */
static inline const MedievilPcSettingsSites *medievil_pc_settings_find(const unsigned char *image, uint64_t size) {
    for (size_t i = 0; i < sizeof(medievil_pc_settings_builds) / sizeof(medievil_pc_settings_builds[0]); ++i) {
        const MedievilPcSettingsSites *s = &medievil_pc_settings_builds[i];
        if (size < s->slot + 8 || size < s->name + 30 || size < s->exec + sizeof(medievil_pc_settings_prologue))
            continue;
        uint64_t slot, name;
        memcpy(&slot, image + s->slot, 8);
        memcpy(&name, image + s->slot - 8, 8);
        if (slot == (uint64_t)(uintptr_t)(image + s->exec) && name == (uint64_t)(uintptr_t)(image + s->name) &&
            !memcmp(image + s->name, "PostEventAtLocationWithMotion", 30) &&
            !memcmp(image + s->exec, medievil_pc_settings_prologue, sizeof(medievil_pc_settings_prologue)))
            return s;
    }
    return NULL;
}

enum {
    MEDIEVIL_PC_SETTINGS_OTHER,    /* another call of the native */
    MEDIEVIL_PC_SETTINGS_ORIGINAL, /* the site, but the handler still switches the controls (no pak edit) */
    MEDIEVIL_PC_SETTINGS_CLICK,    /* the PC Settings row was clicked */
};

/* What a call with this FFrame is: `script` / `script_size` are the calling function's (Node) bytecode and
 * `code` the frame's Code when the exec starts. */
static inline int medievil_pc_settings_call(const MedievilPcSettingsSites *s, const unsigned char *script,
                                            int32_t script_size, const unsigned char *code) {
    if (!script || script_size != s->script_size || code != script + s->call_at + 9) return MEDIEVIL_PC_SETTINGS_OTHER;
    uint32_t target;
    memcpy(&target, script + s->jump_at + 1, 4);
    if (script[s->let_at] != 0x0f || script[s->let_at + 0x12] != 0x19 || script[s->call_at] != 0x1c ||
        script[s->call_at + 9] != 0x20 || script[s->jump_at] != 0x07 || script[s->jump_at + 5] != 0x01 ||
        script[s->pop_at] != 0x4d)
        return MEDIEVIL_PC_SETTINGS_OTHER;
    if (target == s->pop_at) return MEDIEVIL_PC_SETTINGS_CLICK;
    return target == s->pop_at + 1 ? MEDIEVIL_PC_SETTINGS_ORIGINAL : MEDIEVIL_PC_SETTINGS_OTHER;
}
#endif
