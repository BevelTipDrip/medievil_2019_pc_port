// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: the SDR look in HDR, with highlights (research/docs/plan-hdr-sdr-match.md step 2). Shared by ue4_hdr_lut.frag
// (the grading LUT) and ue4_pattern.frag (meter patches through the same curve). Needs ue4_grade.glsl (GradeScene).
//
// 1. SDR look: the game's graded colour G as an SDR screen showed it: sRGB-encoded (the game's device 0), decoded by a
//    `gamma` display (0 = the sRGB curve as is), x game brightness. Measured on target (study-hdr.md H29-H30).
//    Per channel, as a real screen decodes, gamma darkens a colour's dim channels more than its bright one: in dark,
//    bluish tones blue's share grows (the Graveyard's "blue push", study-hdr.md H37). Brightness only (the default)
//    keeps the gamma screen's luminance with the sRGB curve's colour; the two differ only below ~10 nits.
// 2. Highlights: below the knee (Y of the untonemapped colour U at `knee`) the output is the SDR look exactly.
//    Above it, luminance continues the SDR curve's own log-log slope at the knee instead of the film curve's shoulder
//    (the game's contrast, no compression); colour and saturation stay G's (the SDR highlights' look). The knee point
//    and its slope are the same pixel graded at knee brightness (GradeScene again: per-area grading is kept).
//    `highlights` (0..1) blends the extended luminance in: 0 = the SDR look only.
// 3. Roll-off to the display's peak: identity up to `rolloff_start` of the peak (half by default), then an extended
//    Reinhard shoulder that reaches the peak at 4x the peak (HdrRollOff). Luminance-based (BT.709 Y), so hue is kept.
//
//    Shape (study-hdr.md H39, live): `knee` (Y(U) where highlights start; 0.5 before H39), `target` (0: the SDR slope
//    at the knee continued, as before; > 0: the colour's own SDR curve wherever it is steeper than `target`, eased in
//    over one stop above the knee, else `target`: HdrHighlightBoost) and `rolloff_start` (fraction of the peak where
//    the roll-off starts; 0.5 before).
// Or (study-hdr.md H40, the "Piecewise" tone curve): HdrPiecewiseNits below, which replaces 1-3.

const vec3 HDR_Y709 = vec3(0.2126, 0.7152, 0.0722);
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

// G as the SDR screen showed it (1.0 = SDR white). `per_channel`: each channel through the gamma curve, as the screen
// did; else the gamma curve's luminance with the colour of the sRGB curve (G itself).
vec3 HdrSdrLook(vec3 graded, float gamma, bool per_channel) {
    const vec3 srgb = max(graded, vec3(0.0));
    if (gamma <= 0.0) {
        return srgb;
    }
    const vec3 screen = pow(SrgbEncode(graded), vec3(gamma));
    if (per_channel) {
        return screen;
    }
    const float y = dot(srgb, HDR_Y709);
    return y > 0.0 ? srgb * (dot(screen, HDR_Y709) / y) : screen;
}

// Luminance roll-off (nits): identity below `start` x peak; above, extended Reinhard in units of the headroom,
// x (1 + x / W^2) / (1 + x) with W = (4 peak - knee) / (peak - knee) (7 at start 0.5): slope 1 at the knee, the peak at
// 4x peak.
float HdrRollOff(float nits, float peak, float start) {
    const float knee = clamp(start, 0.05, 0.95) * peak;
    if (peak <= 0.0 || nits <= knee) {
        return nits;
    }
    const float head = peak - knee;
    const float x = (nits - knee) / head;
    const float w = (4.0 * peak - knee) / head;
    return knee + head * min(1.0, x * (1.0 + x / (w * w)) / (1.0 + x));
}

// Y of the SDR look of a scene colour.
float HdrLookY(vec3 scene, bool zero_black_clip, float gamma, bool gamma_per_channel) {
    vec3 untonemapped, graded;
    GradeScene(scene, zero_black_clip, untonemapped, graded);
    return dot(HdrSdrLook(graded, gamma, gamma_per_channel), HDR_Y709);
}

// H39 highlights (target > 0): log2 of the boost over the SDR look for a colour `stops` above the knee (`s` scales it
// to the knee). Walks the same colour's SDR curve up from the knee in half stops (at most four stops; beyond, the last
// slope continues) and wherever its log-log slope falls below the target (eased in over the first stop) adds the
// difference: the SDR curve where it is steeper, the target where the SDR shoulder flattens.
float HdrHighlightBoost(vec3 scene, float s, float stops, float target, bool zero_black_clip, float gamma,
                        bool gamma_per_channel) {
    float boost = 0.0;
    float slope = 0.0;
    float t0 = 0.0;
    float y_prev = HdrLookY(scene * s, zero_black_clip, gamma, gamma_per_channel);
    for (int k = 1; k <= 8 && t0 < stops; ++k) {
        const float t1 = min(0.5 * float(k), stops);
        const float y_k = HdrLookY(scene * (s * exp2(t1)), zero_black_clip, gamma, gamma_per_channel);
        slope = (y_prev > 0.0 && y_k > 0.0) ? log2(y_k / y_prev) / (t1 - t0) : 0.0;
        const float w = smoothstep(0.0, 1.0, 0.5 * (t0 + t1));
        boost += w * max(target - slope, 0.0) * (t1 - t0);
        y_prev = y_k;
        t0 = t1;
    }
    return boost + max(target - slope, 0.0) * max(stops - t0, 0.0);
}

// Scene-linear colour (the LUT's decode) -> linear BT.709 nits.
vec3 HdrSceneToNits(vec3 scene, bool zero_black_clip, float game_nits, float gamma, bool gamma_per_channel,
                    float peak, float highlights, float knee, float target, float rolloff_start) {
    vec3 untonemapped, graded;
    GradeScene(scene, zero_black_clip, untonemapped, graded);
    vec3 look = HdrSdrLook(graded, gamma, gamma_per_channel);
    const float y = dot(look, HDR_Y709);
    const float yu = dot(untonemapped, HDR_Y709);
    if (highlights > 0.0 && yu > knee && y > 0.0) {
        const float s = knee / yu;
        if (target > 0.0) {
            const float extended =
                y * exp2(HdrHighlightBoost(scene, s, log2(yu / knee), target, zero_black_clip, gamma,
                                           gamma_per_channel));
            look *= max(1.0, mix(y, extended, highlights) / y);
        } else {
            // Before H39: the same pixel graded at knee brightness, and one step brighter: the SDR curve's point and
            // slope there, continued.
            vec3 u0, g0, u1, g1;
            GradeScene(scene * s, zero_black_clip, u0, g0);
            GradeScene(scene * (s * HdrSlopeStep), zero_black_clip, u1, g1);
            const float y0 = dot(HdrSdrLook(g0, gamma, gamma_per_channel), HDR_Y709);
            const float y1 = dot(HdrSdrLook(g1, gamma, gamma_per_channel), HDR_Y709);
            const float yu0 = dot(u0, HDR_Y709);
            const float yu1 = dot(u1, HDR_Y709);
            if (y0 > 0.0 && y1 > y0 && yu0 > 0.0 && yu1 > yu0) {
                const float slope = clamp(log(y1 / y0) / log(yu1 / yu0), 0.0, 1.5);
                const float extended = y0 * pow(yu / yu0, slope);
                // Never darker than the SDR look (the shoulder only ever compresses).
                look *= max(1.0, mix(y, extended, highlights) / y);
            }
        }
    }
    vec3 nits = look * game_nits;
    const float yn = dot(nits, HDR_Y709);
    if (yn > 0.0) {
        nits *= HdrRollOff(yn, peak, rolloff_start) / yn;
    }
    return nits;
}

// ---- H40: the piecewise (filmic) tone curve, in place of the game's film curve (study-hdr.md H40) ----
// Hable's piecewise power curve ("Filmic Tonemapping with Piecewise Power Curves", 2017) made for HDR: mid grey pinned
// at the game brightness instead of white at 1.0, so the peak only moves the shoulder. On the luminance of the graded,
// untonemapped colour U (scene-linear, 1.0 = SDR white's scene value):
//   s' = max(Y - toe clamp, 0)
//   middle (s' >= toe length): grey nits ((s' - b) / (0.18 - b))^gamma; b = toe strength x toe length (where the middle
//     would reach black: the toe strength)
//   toe (below): a power curve matched to the middle in value and slope (Hable's SolveAB): toe nits (s' / length)^p,
//     p = gamma length / (length - b)
//   shoulder on those nits from `start` (nits): identity below; above, the span to `end` (the nits where it reaches
//     the peak) mapped to start..peak by a blend of Hable's flipped power curve (angle 0: it flattens out before the
//     peak) and a Reinhard curve (angle 1: it keeps rising to the peak, more detail in the brightest highlights); both
//     have slope 1 at the start and reach the peak at the end.
// Constants (vk_ue4_hdr.cpp PiecewiseFromSettings): pw0 = (toe clamp, toe length, b, gamma), pw1 = (grey nits, p,
// toe nits at the toe length, highlight saturation), pw2 = (shoulder start, shoulder end, peak, angle).
float HdrPiecewiseUnrolled(float y, vec4 pw0, vec4 pw1) {
    const float s = max(y - pw0.x, 0.0);
    if (s >= pw0.y) {
        return pw1.x * pow((s - pw0.z) / (0.18 - pw0.z), pw0.w);
    }
    return s > 0.0 ? pw1.z * pow(s / pw0.y, pw1.y) : 0.0;
}

float HdrPiecewiseShoulder(float n, vec4 pw2) {
    const float start = pw2.x, end = pw2.y, peak = pw2.z;
    if (n <= start || peak <= start) {
        return min(n, max(peak, 0.0));
    }
    if (n >= end) {
        return peak;
    }
    const float k = (end - start) / (peak - start);  // >= 1: the slope at the start in units of the span
    const float t = (n - start) / (end - start);
    const float power = 1.0 - pow(1.0 - t, k);
    const float reinhard = k * t / (1.0 + (k - 1.0) * t);
    return start + (peak - start) * mix(power, reinhard, pw2.w);
}

float HdrPiecewiseY(float y, vec4 pw0, vec4 pw1, vec4 pw2) {
    return HdrPiecewiseShoulder(HdrPiecewiseUnrolled(y, pw0, pw1), pw2);
}

// Scene-linear colour -> linear BT.709 nits with the piecewise curve. The colour is the game's graded colour G (all of
// its grading, the level's LUT included, applies after the film curve), its luminance re-toned: G x curve(Y(U)) / Y(G
// of a grey of the same Y(U)): a grey gets the curve exactly, a colour keeps its luminance relative to grey as the
// game grades it. Highlight saturation (pw1.w) blends in U's colour (before the film curve's fade to white) where the
// grey's SDR value nears white.
vec3 HdrPiecewiseNits(vec3 scene, bool zero_black_clip, vec4 pw0, vec4 pw1, vec4 pw2) {
    vec3 untonemapped, graded;
    GradeScene(scene, zero_black_clip, untonemapped, graded);
    const vec3 u = max(untonemapped, vec3(0.0));
    const float yu = dot(u, HDR_Y709);
    if (yu <= 0.0) {
        return vec3(0.0);
    }
    const float n = HdrPiecewiseY(yu, pw0, pw1, pw2);
    vec3 ug, gg;
    GradeScene(vec3(yu), zero_black_clip, ug, gg);
    const float y_grey = dot(max(gg, vec3(0.0)), HDR_Y709);
    const vec3 g = max(graded, vec3(0.0));
    vec3 nits = (y_grey > 1e-7 && dot(g, HDR_Y709) > 0.0) ? g * (n / y_grey) : u * (n / yu);
    const float w = pw1.w * smoothstep(0.4, 0.9, y_grey);
    if (w > 0.0) {
        const vec3 vivid = u * (dot(nits, HDR_Y709) / yu);
        nits = mix(nits, vivid, w);
    }
    // No channel above the peak (a BT.2020 channel is a positive mix of these, so it fits too). Saturation 0: pulled
    // towards its own luminance until the brightest channel fits (fades to white, as the game's SDR highlights); 1:
    // scaled down whole (keeps its colour, a little darker): at the peak a saturated colour cannot keep both.
    const float y = dot(nits, HDR_Y709);
    const float top = max(max(nits.x, nits.y), nits.z);
    if (top > pw2.z && y > 0.0) {
        const float y_fit = min(y, pw2.z);
        const vec3 to_white = vec3(y_fit) + (nits - vec3(y)) * max((pw2.z - y_fit) / (top - y), 0.0);
        nits = mix(to_white, nits * (pw2.z / top), pw1.w);
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
