/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEDIEVIL_CAMERA_HOOK_H
#define MEDIEVIL_CAMERA_HOOK_H
#include <stdint.h>
#include <string.h>

/* MediEvil (CUSA11227) free camera: its automatic moves, each switchable live from the menu's Controls tab
 * (research/docs/study-camera-moves.md). The free areas (BP_CameraAreaFree, its subclass
 * FreeSuggestDirection, FreeSimple) call the native MedievilSpringArm.ComputeCameraSimple from their
 * ComputeCamera once a tick; its exec is wrapped through its FNameNativePtrPair slot. On that call (game
 * thread) the FFrame gives the area's ComputeCamera (Node +0x10; its Outer +0x20 is the area class) and the
 * area (Object +0x18). The class's own functions (UStruct Children +0x38, UField Next +0x28, a UFunction's
 * Script.Data +0x48 / Num +0x50, UObject Class +0x10, UStruct SuperStruct +0x30), its parents, the area's
 * class below it, and the camera manager class (owner +0x20 of the InputRotation property that
 * ComputeCamera multiplies by 2) are searched for each edit's original bytes; a move's bytes are then
 * written or put back whenever the menu's choice changes. Jump targets are in-memory offsets in the
 * loaded bytecode and on disk alike, so the patterns are the override-pak ones (both builds). */

enum {
    MEDIEVIL_CAMERA_FOLLOW,       /* the camera chasing Dan's direction while he moves */
    MEDIEVIL_CAMERA_SWING,        /* Dan's sideways speed added to the shown yaw */
    MEDIEVIL_CAMERA_SUGGEST,      /* the turn toward a suggest-direction area's arrow on entry */
    MEDIEVIL_CAMERA_KEEPOUT,      /* yaw pushed around keep-out posts */
    MEDIEVIL_CAMERA_RUN_ZOOM,     /* boom length and pitch changes at run start / end */
    MEDIEVIL_CAMERA_PITCH_RETURN, /* vertical look easing back to the middle while Dan moves or attacks */
    MEDIEVIL_CAMERA_MOVES
};

/* Where an edit is searched: the class that owns ComputeCamera; the area's own class when it is a
 * subclass of that one; that class's parents; the camera manager's class. */
enum { MEDIEVIL_CAMERA_IN_AREA, MEDIEVIL_CAMERA_IN_SUBCLASS, MEDIEVIL_CAMERA_IN_PARENTS, MEDIEVIL_CAMERA_IN_MANAGER };

typedef struct {
    int move, where;
    unsigned char pattern[6], length; /* the original bytes, found exactly once in the searched class */
    unsigned char at;                 /* first changed byte, from the pattern's start */
    unsigned char edit[4], edit_length;
    unsigned char manager;            /* its statement names the camera manager's InputRotation */
    unsigned char together;           /* the move's edits marked so must all be found in the class */
} MedievilCameraEdit;

static const MedievilCameraEdit medievil_camera_edits[] = {
    /* ComputeCamera: Less(VSizeXY(focus - camera), 400.0f) picks control yaw over atan2 (chase);
     * 400 -> 1e30 keeps control yaw. */
    {MEDIEVIL_CAMERA_FOLLOW, MEDIEVIL_CAMERA_IN_AREA, {0x1e, 0x00, 0x00, 0xc8, 0x43}, 5, 1, {0xca, 0xf2, 0x49, 0x71}, 4, 0, 1},
    /* ComputeCamera: Multiply(CameraManager.InputRotation, 2.0f) added while moving; x 0, else moving
     * turns 4.5x the stick (control yaw 2.5x + 2x) against 2.5x standing. EX_EndFunctionParms after. */
    {MEDIEVIL_CAMERA_FOLLOW, MEDIEVIL_CAMERA_IN_AREA, {0x1e, 0x00, 0x00, 0x00, 0x40, 0x16}, 6, 1, {0, 0, 0, 0}, 4, 1, 1},
    /* CheckMovingState: JumpIfNot(moving and not yawing) 0xb30 -> Jump: IsMoving never set. */
    {MEDIEVIL_CAMERA_RUN_ZOOM, MEDIEVIL_CAMERA_IN_AREA, {0x16, 0x07, 0x30, 0x0b, 0x00, 0x00}, 6, 1, {0x06}, 1, 0, 0},
    /* Ubergraph: JumpIfNot(moving or attacking) around "pitch target 0" -> Jump; 0x275 in 01.02's Free,
     * 0x250 in 01.00's Free and in FreeSimple. */
    {MEDIEVIL_CAMERA_PITCH_RETURN, MEDIEVIL_CAMERA_IN_AREA, {0x16, 0x07, 0x75, 0x02, 0x00, 0x00}, 6, 1, {0x06}, 1, 0, 0},
    {MEDIEVIL_CAMERA_PITCH_RETURN, MEDIEVIL_CAMERA_IN_AREA, {0x16, 0x07, 0x50, 0x02, 0x00, 0x00}, 6, 1, {0x06}, 1, 0, 0},
    /* FreeSuggestDirection.ComputeCamera: JumpIfNot(UseSuggest) -> Jump to the plain Free result. */
    {MEDIEVIL_CAMERA_SUGGEST, MEDIEVIL_CAMERA_IN_SUBCLASS, {0x16, 0x07, 0x38, 0x05, 0x00, 0x00}, 6, 1, {0x06}, 1, 0, 0},
    /* BP_CameraAreaBaseClass.ProcessKeepOuts: the loop's JumpIfNot -> Jump, the loop never runs. */
    {MEDIEVIL_CAMERA_KEEPOUT, MEDIEVIL_CAMERA_IN_PARENTS, {0x16, 0x07, 0xfd, 0x01, 0x00, 0x00}, 6, 1, {0x06}, 1, 0, 0},
    /* BP_CameraManager.GetVelocityYaw: JumpIfNot(weight == 0) target 0x67 -> 0x30, the weight-0 path
     * (offset 0); EX_LocalVariable follows. */
    {MEDIEVIL_CAMERA_SWING, MEDIEVIL_CAMERA_IN_MANAGER, {0x16, 0x07, 0x67, 0x00, 0x00, 0x00}, 6, 2, {0x30}, 1, 0, 0},
};
enum { MEDIEVIL_CAMERA_EDITS = sizeof(medievil_camera_edits) / sizeof(medievil_camera_edits[0]) };

/* ComputeCamera's InputRotation x 2 statement around the 2.0f literal at `p` (the 0x1e):
 * EX_Context (19) EX_LocalVariable (00) <camera manager local> <u32 9> <UProperty* InputRotation>
 * EX_InstanceVariable (01) <UProperty* InputRotation> EX_FloatConst (1e). The InputRotation property, or 0. */
static inline uint64_t medievil_camera_input_rotation(const unsigned char *script, int32_t size, int32_t p) {
    if (p < 31 || p + 6 > size) return 0;
    uint64_t ir1, ir2;
    uint32_t skip;
    memcpy(&ir1, script + p - 17, 8);
    memcpy(&ir2, script + p - 8, 8);
    memcpy(&skip, script + p - 21, 4);
    return script[p - 31] == 0x19 && script[p - 30] == 0x00 && skip == 9 && script[p - 9] == 0x01 && ir1 &&
                   ir1 == ir2 ? ir1 : 0;
}

/* Mouse look's pitch (study-camera-moves.md section 8): the free area's ubergraph keeps the player's pitch
 * as TargetLookUpAxisValue (the stick integrates into it; clamped so Target + PitchEffective stays within
 * UserPitchMin / UserPitchMax) and shows SmoothedLookUpAxisValue, which follows it. Their UProperty*, read
 * from the bytecode around the pitch_return JumpIfNot at `jump` (07, or 06 while that move is off), in-memory:
 *   jump + 0x0e  EX_Let (0f) <Target> EX_InstanceVariable (01) <Target> EX_FloatConst (1e)      Target = 0
 *   jump + 0x25  EX_Let (0f) <L> EX_LocalVariable (00) <L> EX_CallMath (68) <FInterpTo>
 *                EX_InstanceVariable (01) <Smoothed> (01) <Target> ...
 * and the one chain of four 46-byte statements `L1 = Add(L0, PitchEffective)`, `L2 = FMin(L1, UserPitchMax)`,
 * `L3 = FMax(L2, UserPitchMin)`, `L4 = Subtract(L3, PitchEffective)`: EX_Let <L> 00 <L> 68 <fn> 00 <arg L>
 * 01 <property> 16, each one's argument the previous one's result, then `Target = L4`
 * (0f <Target> 01 <Target> 00 <L4>). Three in a row match twice (Add..FMax and FMin..Subtract), so the
 * whole chain is the anchor. The same layout in 01.00's and 01.02's Free and in FreeSimple. */
typedef struct {
    uint64_t target, smoothed, effective, max, min;
} MedievilCameraPitch;
static inline uint64_t medievil_camera_ptr(const unsigned char *script, int32_t at) {
    uint64_t v;
    memcpy(&v, script + at, 8);
    return v;
}
/* A 46-byte `let L = fn(L', property)` statement at `a`: its result local, or 0. */
static inline uint64_t medievil_camera_math_let(const unsigned char *script, int32_t a) {
    const uint64_t result = medievil_camera_ptr(script, a + 1);
    return script[a] == 0x0f && script[a + 0x09] == 0x00 && script[a + 0x12] == 0x68 && script[a + 0x1b] == 0x00 &&
                   script[a + 0x24] == 0x01 && script[a + 0x2d] == 0x16 && result &&
                   result == medievil_camera_ptr(script, a + 0x0a) ? result : 0;
}
static inline int medievil_camera_pitch(const unsigned char *script, int32_t size, int32_t jump, MedievilCameraPitch *out) {
    if (jump < 1 || jump + 0x52 > size || (script[jump] != 0x07 && script[jump] != 0x06)) return 0;
    const unsigned char *t = script + jump;
    const uint64_t target = medievil_camera_ptr(script, jump + 0x0f);
    if (t[0x0e] != 0x0f || t[0x17] != 0x01 || t[0x20] != 0x1e || !target ||
        medievil_camera_ptr(script, jump + 0x18) != target)
        return 0;
    if (t[0x25] != 0x0f || t[0x2e] != 0x00 || t[0x37] != 0x68 || t[0x40] != 0x01 || t[0x49] != 0x01 ||
        medievil_camera_ptr(script, jump + 0x4a) != target)
        return 0;
    const uint64_t smoothed = medievil_camera_ptr(script, jump + 0x41);
    int found = 0;
    for (int32_t a = jump; a + 4 * 0x2e + 0x1b <= size; ++a) {
        const uint64_t l1 = medievil_camera_math_let(script, a);
        if (!l1) continue;
        const uint64_t l2 = medievil_camera_math_let(script, a + 0x2e);
        if (!l2 || medievil_camera_ptr(script, a + 0x2e + 0x1c) != l1) continue;
        const uint64_t l3 = medievil_camera_math_let(script, a + 0x5c);
        if (!l3 || medievil_camera_ptr(script, a + 0x5c + 0x1c) != l2) continue;
        const uint64_t l4 = medievil_camera_math_let(script, a + 0x8a);
        if (!l4 || medievil_camera_ptr(script, a + 0x8a + 0x1c) != l3) continue;
        if (medievil_camera_ptr(script, a + 0x25) != medievil_camera_ptr(script, a + 0x8a + 0x25)) continue;
        const unsigned char *s = script + a + 0xb8;
        if (s[0] != 0x0f || s[9] != 0x01 || s[0x12] != 0x00 || medievil_camera_ptr(script, a + 0xb9) != target ||
            medievil_camera_ptr(script, a + 0xc2) != target || medievil_camera_ptr(script, a + 0xcb) != l4)
            continue;
        if (found++) return 0; /* more than one chain: not the layout we know */
        out->effective = medievil_camera_ptr(script, a + 0x25);
        out->max = medievil_camera_ptr(script, a + 0x2e + 0x25);
        out->min = medievil_camera_ptr(script, a + 0x5c + 0x25);
    }
    if (!found || !smoothed || !out->effective || !out->max || !out->min) return 0;
    out->target = target;
    out->smoothed = smoothed;
    return 1;
}

/* ComputeCameraSimple's first two arguments as the exec starts (Stack.Code, after EX_FinalFunction (1c)
 * <UFunction*>): EX_LocalVariable (00) <origin> 00 <rotation>. The rotation local's UProperty*, or 0. */
static inline uint64_t medievil_camera_rotation_local(const unsigned char *code) {
    return code[-9] == 0x1c && code[0] == 0x00 && code[9] == 0x00 ? medievil_camera_ptr(code, 10) : 0;
}

/* UMedievilSpringArm::ComputeCameraSimple's exec slot (FNameNativePtrPair + 8), its thunk and name. */
typedef struct {
    uint32_t slot, exec, name;
    const char *build;
} MedievilCameraSites;
static const MedievilCameraSites medievil_camera_builds[] = {
    {0x4ecc7f8, 0x65b290, 0x3c5627a, "01.00"},
    {0x4ecc7f8, 0x65d570, 0x3c5967a, "01.02"},
};
/* The thunk's first bytes (push rbp; mov rbp, rsp; push r15 ... r12, rbx; sub rsp, imm32), both builds. */
static const unsigned char medievil_camera_exec_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                                              0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec};

/* The build whose slot (relocated: holds image + exec), name and thunk prologue match, or NULL. */
static inline const MedievilCameraSites *medievil_camera_find(const unsigned char *image, uint64_t size) {
    for (size_t i = 0; i < sizeof(medievil_camera_builds) / sizeof(medievil_camera_builds[0]); ++i) {
        const MedievilCameraSites *s = &medievil_camera_builds[i];
        if (size < s->slot + 8 || size < s->name + 20 || size < s->exec + sizeof(medievil_camera_exec_prologue))
            continue;
        uint64_t slot, name;
        memcpy(&slot, image + s->slot, 8);
        memcpy(&name, image + s->slot - 8, 8);
        if (slot == (uint64_t)(uintptr_t)(image + s->exec) && name == (uint64_t)(uintptr_t)(image + s->name) &&
            !memcmp(image + s->name, "ComputeCameraSimple", 20) &&
            !memcmp(image + s->exec, medievil_camera_exec_prologue, sizeof(medievil_camera_exec_prologue)))
            return s;
    }
    return NULL;
}
#endif
