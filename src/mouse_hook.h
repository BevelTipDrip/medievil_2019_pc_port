/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEDIEVIL_MOUSE_HOOK_H
#define MEDIEVIL_MOUSE_HOOK_H
#include <stdint.h>
#include <string.h>

/* MediEvil (CUSA11227) mouse yaw (research/docs/study-mouse-input.md, "Step 5 research"). Only Dan's
 * Blueprint (BP_df_character) calls AddControllerYawInput, once per TurnRate event, right after it
 * writes `CameraManager.InputRotation = select(AllowUserCameraControl, stick turn, 0)`; the camera
 * areas add InputRotation x 2 degrees to the camera's yaw each tick (control yaw is mostly
 * overwritten while Dan moves; while he stands, BP_CameraAreaFree keeps control yaw, which only
 * AddControllerYawInput turns: T-MS5). The exec thunk is wrapped through its FNameNativePtrPair slot;
 * the wrapper finds both from Dan's loaded ubergraph bytecode (no names needed):
 *   let_at   EX_Let (0f) <UProperty* InputRotation> EX_Context (19) EX_LocalVariable (00)
 *            <UProperty* camera manager local> <u32 9> <UProperty* InputRotation> EX_InstanceVariable
 *            (01) <UProperty* InputRotation> EX_SwitchValue (69) ... EX_True (27) at +0x46,
 *            EX_LocalVariable (00) <UProperty* stick turn> at +0x4b
 *   call_at  EX_VirtualFunction (1b) <12-byte name> ... EX_SwitchValue (69) at +0xd, EX_LocalVariable
 *            (00) <UProperty* Temp_bool (AllowUserCameraControl)> at +0x14, EX_False (28) at +0x1d,
 *            EX_True (27) at +0x2b, EX_LocalVariable (00) <UProperty* stick turn> at +0x30,
 *            EX_EndFunctionParms (16) at code - 1
 * Offsets are from UStruct::Script.Data (UFunction +0x48, Num +0x50); Stack.Code equals Data + call_at
 * + 0xd when the exec starts (the argument, read by the thunk) and Data + code once it returns.
 * UProperty::Offset_Internal is +0x44 (execLocalVariable), FFrame Node +0x10, Code +0x20, Locals +0x28
 * (the ubergraph's persistent frame). */
typedef struct {
    uint32_t slot, exec;          /* AddControllerYawInput's exec-pointer slot and its thunk */
    uint32_t name;                /* "AddControllerYawInput" */
    int32_t script_size;          /* Dan's ubergraph, in memory */
    uint32_t let_at, call_at, code;
    const char *build;
} MedievilMouseYawSites;
static const MedievilMouseYawSites medievil_mouse_yaw_builds[] = {
    {0x525b6c8, 0x2a26320, 0x3eaec73, 0x5265, 0x1025, 0x1151, 0x1194, "01.00"},
    {0x525bc48, 0x2a290e0, 0x3eb1c23, 0x58fb, 0x11d9, 0x1361, 0x13a4, "01.02"},
};
/* The thunk: mov rax, [r14]; vmovss xmm0, [rbp-0x24]; call [rax+0x730] (APawn::AddControllerYawInput) */
static const unsigned char medievil_mouse_yaw_dispatch[] = {
    0x49,0x8b,0x06,0xc5,0xfa,0x10,0x45,0xdc,0xff,0x90,0x30,0x07,0x00,0x00
};
enum { MEDIEVIL_MOUSE_YAW_DISPATCH_AT = 0x72 };

/* The build whose slot (relocated: holds image + exec), name and dispatch match, or NULL. */
static inline const MedievilMouseYawSites *medievil_mouse_yaw_find(const unsigned char *image, uint64_t size) {
    for (size_t i = 0; i < sizeof(medievil_mouse_yaw_builds) / sizeof(medievil_mouse_yaw_builds[0]); ++i) {
        const MedievilMouseYawSites *s = &medievil_mouse_yaw_builds[i];
        if (size < s->slot + 8 || size < s->name + 22 ||
            size < s->exec + MEDIEVIL_MOUSE_YAW_DISPATCH_AT + sizeof(medievil_mouse_yaw_dispatch)) continue;
        uint64_t slot, name;
        memcpy(&slot, image + s->slot, 8);
        memcpy(&name, image + s->slot - 8, 8);
        if (slot == (uint64_t)(uintptr_t)(image + s->exec) && name == (uint64_t)(uintptr_t)(image + s->name) &&
            !memcmp(image + s->name, "AddControllerYawInput", 22) &&
            !memcmp(image + s->exec + MEDIEVIL_MOUSE_YAW_DISPATCH_AT, medievil_mouse_yaw_dispatch,
                    sizeof(medievil_mouse_yaw_dispatch)))
            return s;
    }
    return NULL;
}

/* Dan's handler bytecode at `script` (checked once per ubergraph): the opcodes above, the three
 * InputRotation pointers equal and non-null, and the same stick-turn local in the let and the call. */
static inline int medievil_mouse_yaw_script_ok(const unsigned char *script, const MedievilMouseYawSites *s) {
    const unsigned char *let = script + s->let_at, *call = script + s->call_at;
    uint32_t skip;
    uint64_t ir1, ir2, ir3, cm, gate, turn1, turn2;
    memcpy(&skip, let + 0x13, 4);
    memcpy(&ir1, let + 0x01, 8);
    memcpy(&cm, let + 0x0b, 8);
    memcpy(&ir2, let + 0x17, 8);
    memcpy(&ir3, let + 0x20, 8);
    memcpy(&turn1, let + 0x4c, 8);
    memcpy(&gate, call + 0x15, 8);
    memcpy(&turn2, call + 0x31, 8);
    return let[0] == 0x0f && let[0x09] == 0x19 && let[0x0a] == 0x00 && skip == 9 && let[0x1f] == 0x01 &&
           let[0x28] == 0x69 && let[0x46] == 0x27 && let[0x4b] == 0x00 && call[0] == 0x1b &&
           call[0x0d] == 0x69 && call[0x14] == 0x00 && call[0x1d] == 0x28 && call[0x2b] == 0x27 &&
           call[0x30] == 0x00 && script[s->code - 1] == 0x16 && ir1 && ir1 == ir2 && ir1 == ir3 && cm &&
           gate && turn1 && turn1 == turn2;
}
#endif
