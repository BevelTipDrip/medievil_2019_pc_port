// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: the SDR look in HDR, with highlights (research/docs/plan-hdr-sdr-match.md step 2). Shared by ue4_hdr_lut.frag
// (the grading LUT) and ue4_pattern.frag (meter patches through the same curve). Needs ue4_grade.glsl (GradeScene).
//
// 1. SDR look: the game's graded colour G as an SDR screen showed it: sRGB-encoded (the game's device 0), decoded by a
//    `gamma` display (0 = the sRGB curve as is), x game brightness. Measured on target (study-hdr.md H29-H30).
// 2. Highlights: below the knee (Y of the untonemapped colour U at HdrKneeScene) the output is the SDR look exactly.
//    Above it, luminance continues the SDR curve's own log-log slope at the knee instead of the film curve's shoulder
//    (the game's contrast, no compression); colour and saturation stay G's (the SDR highlights' look). The knee point
//    and its slope are the same pixel graded at knee brightness (GradeScene again: per-area grading is kept).
//    `highlights` (0..1) blends the extended luminance in: 0 = the SDR look only.
// 3. Roll-off to the display's peak: identity up to half the peak, then an extended Reinhard shoulder that reaches the
//    peak at 4x the peak (HdrRollOff). Luminance-based (BT.709 Y), so hue is kept.

const vec3 HDR_Y709 = vec3(0.2126, 0.7152, 0.0722);
const float HdrKneeScene = 0.5;   ///< Y(U) where highlights start (the SDR look's ~0.5 of white: ~100 nits at 203)
const float HdrSlopeStep = 1.05;  ///< scene step for the knee's slope

// The sRGB curve, continued as a power law above 1 (G may exceed 1: no saturate in UE 4.21's chain).
vec3 SrgbEncode(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, 12.92 * c, lessThan(c, vec3(0.0031308)));
}

vec3 PqEncode(vec3 nits) {
    const vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));
}

// G as the SDR screen showed it (1.0 = SDR white).
vec3 HdrSdrLook(vec3 graded, float gamma) {
    return gamma > 0.0 ? pow(SrgbEncode(graded), vec3(gamma)) : max(graded, vec3(0.0));
}

// Luminance roll-off (nits): identity below half the peak; above, extended Reinhard in units of the headroom,
// x (1 + x / W^2) / (1 + x) with W = (4 peak - knee) / (peak - knee) = 7: slope 1 at the knee, the peak at 4x peak.
float HdrRollOff(float nits, float peak) {
    const float knee = 0.5 * peak;
    if (peak <= 0.0 || nits <= knee) {
        return nits;
    }
    const float head = peak - knee;
    const float x = (nits - knee) / head;
    const float w2 = 49.0;
    return knee + head * min(1.0, x * (1.0 + x / w2) / (1.0 + x));
}

// Scene-linear colour (the LUT's decode) -> linear BT.709 nits.
vec3 HdrSceneToNits(vec3 scene, bool zero_black_clip, float game_nits, float gamma, float peak, float highlights) {
    vec3 untonemapped, graded;
    GradeScene(scene, zero_black_clip, untonemapped, graded);
    vec3 look = HdrSdrLook(graded, gamma);
    const float y = dot(look, HDR_Y709);
    const float yu = dot(untonemapped, HDR_Y709);
    if (highlights > 0.0 && yu > HdrKneeScene && y > 0.0) {
        // The same pixel graded at knee brightness, and one step brighter: the SDR curve's point and slope there.
        const float s = HdrKneeScene / yu;
        vec3 u0, g0, u1, g1;
        GradeScene(scene * s, zero_black_clip, u0, g0);
        GradeScene(scene * (s * HdrSlopeStep), zero_black_clip, u1, g1);
        const float y0 = dot(HdrSdrLook(g0, gamma), HDR_Y709);
        const float y1 = dot(HdrSdrLook(g1, gamma), HDR_Y709);
        const float yu0 = dot(u0, HDR_Y709);
        const float yu1 = dot(u1, HDR_Y709);
        if (y0 > 0.0 && y1 > y0 && yu0 > 0.0 && yu1 > yu0) {
            const float slope = clamp(log(y1 / y0) / log(yu1 / yu0), 0.0, 1.5);
            const float extended = y0 * pow(yu / yu0, slope);
            // Never darker than the SDR look (the shoulder only ever compresses).
            look *= max(1.0, mix(y, extended, highlights) / y);
        }
    }
    vec3 nits = look * game_nits;
    const float yn = dot(nits, HDR_Y709);
    if (yn > 0.0) {
        nits *= HdrRollOff(yn, peak) / yn;
    }
    return nits;
}

// Linear BT.709 nits -> BT.2020 PQ.
vec3 HdrNitsToPq(vec3 nits) {
    const mat3 to_2020 = mat3(0.6274040, 0.0690970, 0.0163916,
                              0.3292820, 0.9195400, 0.0880132,
                              0.0433136, 0.0113612, 0.8955950);
    return PqEncode(to_2020 * nits);
}
