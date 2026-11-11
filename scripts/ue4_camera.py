"""The game camera's automatic moves, each one switchable off (study-mouse-input.md, "Camera moves").

GOW3_CAMERA_OFF lists the moves to turn off, comma-separated (empty: the game's camera). Each move is
one or two same-size edits of a camera Blueprint (bytecode or a float constant), placed in the
override pak (ue4_override.py); the package's export sizes stay valid. Only the free camera areas
(BP_CameraAreaFree, FreeSuggestDirection, FreeSimple) and the camera manager's velocity swing are
touched: fixed, spline and boss cameras keep the game's behaviour. An edit given as a list holds one
variant per game version (01.02, then 01.00); a move whose bytes are not found is left on, alone.
"""
from ue4_override import apply_edits

# The switch (double quotes: research/tools/patchmap.py finds switches in "..." literals).
SWITCH = "GOW3_CAMERA_OFF"

FREE = 'OVERBITE/Content/Blueprints/Camera/BP_CameraAreaFree.uexp'
# FreeSimple (one area, 06_PG StartingCam; a copy of 01.00's Free, the same file in 01.00 and 01.02)
# lives in a pak mounted at OVERBITE/Content/: read under its short name, written under the full one.
SIMPLE = 'OVERBITE/Content/Blueprints/Camera/BP_CameraAreaFreeSimple.uexp'
SIMPLE_IN_PAK = 'Blueprints/Camera/BP_CameraAreaFreeSimple.uexp'
SUGGEST = 'OVERBITE/Content/Blueprints/Camera/BP_CameraAreaFreeSuggestDirection.uexp'
BASE = 'OVERBITE/Content/Blueprints/Camera/SupportClasses/BP_CameraAreaBaseClass.uexp'
MANAGER = 'OVERBITE/Content/Blueprints/Camera/BP_CameraManager.uexp'

# Free / FreeSimple ComputeCamera: yaw = moving ? (camera within 400 of Dan or strafing ? control yaw :
# atan2(camera -> Dan)) + InputRotation x 2 : control yaw. 400 -> 1e30 keeps control yaw while moving
# (no chase); InputRotation x 2 -> x 0, else the stick turned 4.5x as fast moving as standing (2.5x,
# control yaw), and the mouse twice its setting.
FOLLOW = [(bytes.fromhex('1e0000c843'), bytes.fromhex('1ecaf24971'), 1),
          (bytes.fromhex('1e00000040'), bytes.fromhex('1e00000000'), 1)]
# Free / FreeSimple CheckMovingState: JumpIfNot(moving and not yawing) -> Jump to the run-end test,
# so IsMoving is never set: no run-start zoom / tilt (boom 900 -> 675-1260, pitch -20 -> -4..-36), no
# run-end goals.
RUN_ZOOM = [(bytes.fromhex('1607300b0000'), bytes.fromhex('1606300b0000'), 1)]
# Free / FreeSimple ubergraph: JumpIfNot(moving or attacking) around "pitch target 0": the player's
# vertical look no longer eases back to the middle while Dan moves or attacks.
PITCH_RETURN = [[(bytes.fromhex('160775020000'), bytes.fromhex('160675020000'), 1),
                 (bytes.fromhex('160750020000'), bytes.fromhex('160650020000'), 1)]]

MOVES = {
    # The camera chasing Dan's direction while he moves.
    'follow': {FREE: FOLLOW, SIMPLE: FOLLOW},
    # BP_CameraManager.GetVelocityYaw: Dan's sideways speed x the area's VelocityYawWeight (0.025 on
    # the free areas, 0 elsewhere) added to the shown yaw, ~20 degrees at a sideways run. Its
    # JumpIfNot(weight == 0) target 0x67 -> 0x30: always the weight-0 path (offset 0).
    'swing': {MANAGER: [(bytes.fromhex('0000160767000000008a0000'),
                         bytes.fromhex('0000160730000000008a0000'), 1)]},
    # FreeSuggestDirection ComputeCamera: JumpIfNot(UseSuggest) -> Jump: no turn toward the area's
    # arrow on entry (13 of 31 placed areas use it, 0.2-1.2 s).
    'suggest': {SUGGEST: [(bytes.fromhex('160738050000'), bytes.fromhex('160638050000'), 1)]},
    # BP_CameraAreaBaseClass.ProcessKeepOuts (free areas only): JumpIfNot -> Jump, the loop never runs:
    # no yaw push around keep-out posts (2 placed areas). The spring arm still shortens at them.
    'keepout': {BASE: [(bytes.fromhex('1607fd010000'), bytes.fromhex('1606fd010000'), 1)]},
    'run_zoom': {FREE: RUN_ZOOM, SIMPLE: RUN_ZOOM},
    'pitch_return': {FREE: PITCH_RETURN, SIMPLE: PITCH_RETURN},
}
READ_NAMES = {SIMPLE: SIMPLE_IN_PAK}


def parse(text):
    """The move names of a GOW3_CAMERA_OFF value, in MOVES order; unknown names raise ValueError."""
    names = {n.strip() for n in text.split(',') if n.strip()}
    unknown = names - set(MOVES)
    if unknown:
        raise ValueError(f'unknown camera move(s): {", ".join(sorted(unknown))} (known: {", ".join(MOVES)})')
    return [n for n in MOVES if n in names]


def files(read, moves):
    """({pak path: edited data}, [moves done]) for the moves to turn off. `read(name)` returns a game file.
    A move is all or nothing: if one of its edits does not match, it is skipped with a message."""
    edited, done = {}, []
    for move in moves:
        trial = {}
        try:
            for name, edits in MOVES[move].items():
                data = edited.get(name) or trial.get(name) or read(READ_NAMES.get(name, name))
                trial[name] = apply_edits(name, data, edits)
        except (ValueError, FileNotFoundError) as error:
            print(f'Camera: "{move}" not available for this game build ({error})', flush=True)
            continue
        edited.update(trial)
        done.append(move)
    return edited, done
