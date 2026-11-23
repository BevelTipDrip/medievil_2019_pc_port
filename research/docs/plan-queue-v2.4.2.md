# Queue: v2.4.2

Status: started 2026-11-22 (the user). Parent: [START-HERE.md](START-HERE.md); the general to-do list stays
[plan-work-queue.md](plan-work-queue.md). This queue holds the work that goes into v2.4.2, after the v2.4.1 release
(the final HDR implementation, [plan-hdr-final.md](plan-hdr-final.md)). Which of the remaining HDR items stay in v2.4.1
and which move here is decided later (the user, 2026-11-22). The last major feature for v2.4.1 is per-level settings.

States: `open` / `in progress` / `done` (with commit) / `dropped` (with why).

| # | Item | Plan and context | State |
|---|---|---|---|
| 1 | **Fog controls** (fog density, fog brightness / light scattering, fog glow; live, 0-200%, SDR and HDR) | [study-fog.md](study-fog.md): research done 2026-11-22, plan approved by the user and tabled for v2.4.2. Scale the fog constants on the CPU as the game's fog passes upload them (as the Color tab's grading does): the volumetric fog setup pass `cs_0x5ec79c2cb022e7ab` buffer 2 (albedo dw 16-18, emissive 20-22, extinction 23) and the height fog pass `fs_0xbf9bcd9c07f3c6d0` / `ff1529dd7fa4e7c3` buffer 1 (density dw 0 / 8, colour 4-6). Before building: the other captures through `fog_params.py` (permutations, which term dominates). Per level: with v2.4.1's per-level settings. Context: the Graveyard's blown-out HDR look was mostly bloom (H49, fixed in v2.4.1); fog still flattens and lifts (the user) | open |
