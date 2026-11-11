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
