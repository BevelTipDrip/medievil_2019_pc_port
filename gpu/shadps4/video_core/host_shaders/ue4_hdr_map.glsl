// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: our HDR tone curve (research/docs/plan-hdr-final.md; study-hdr.md H40-H42). Included by ue4_hdr_lut.frag (the
// grading LUT). Needs ue4_grade.glsl (GradeScene).

const vec3 HDR_Y709 = vec3(0.2126, 0.7152, 0.0722);

vec3 PqEncode(vec3 nits) {
    const vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));
}

// The piecewise (filmic) tone curve, in place of the game's film curve (study-hdr.md H40).
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
