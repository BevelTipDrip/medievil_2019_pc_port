// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: included by ue4_hdr_lut.frag (vk_ue4_hdr.cpp; research/docs/plan-hdr-sdr-match.md). Checked against the
// game's translated shader on all 32^3 texels by the offline harness in research-data/hdr-ui-20261117/port (not in git:
// it compiles the decompiled game shader).

// ue4_grade.glsl -- clean port of MediEvil's UE 4.21 combine-LUT builder grading chain
// (PS4 fragment shader hash a4152b3be93c832a, BLENDCOUNT=1 permutation).
//
// Source of truth: the spirv-cross decompile of the game shader (research-data/hdr-ui-20261117/port; map in research/docs/notes-ue421-lut-builder-map.md). Every function below names the
// decompile line range it reproduces. Constants are copied from the decompile (they are the game's float
// constants, sometimes folded by the PS4 compiler, e.g. the white-balance CAT and the blue-correction (M - I)
// matrices). Operation order follows the decompile where it is cheap to do so; elsewhere (ColorCorrectAll)
// the UE source order is used, which differs only by float rounding.
//
// GLSL 450, no main(), self-contained. The includer must declare, before including this file:
//     float GradeParam(uint i);   // = uintBitsToFloat(parameter dword i) of the 144-dword block (buffer 0)
// Parameter dwords used (names after UE 4.21 PostProcessCombineLUTs.usf, see notes-ue421-lut-builder-map.md):
//   0-2 MappingPolynomial, 5 InverseGamma.y, 8-38 legacy ColorMatrix/ColorCurve/ColorShadow, 39 film-path flag
//   (integer test), 40-44 FilmSlope/Toe/Shoulder/BlackClip/WhiteClip, 45-47 ColorScale, 48-51 OverlayColor,
//   52 WhiteTemp, 53 WhiteTint, 56-135 ColorCorrect (global/shadows/midtones/highlights x
//   Saturation/Contrast/Gamma/Gain/Offset, float4 each), 136 ShadowsMax, 137 HighlightsMin,
//   140 BlueCorrection, 141 ExpandGamut. 138 OutputDevice / 139 OutputGamut are not read by GradeScene.
//
// Spaces: "srgb" = linear Rec.709/sRGB primaries, "ap1" = ACEScg primaries, "ap0" = ACES2065-1 primaries.
// Scene-linear convention of this LUT: 1.0 = 1 nit in the HDR (PQ x10000) decode; no LinearToNitsScale.

// ------------------------------------------------------------------------------------------------
// Constants (decompile values)
// ------------------------------------------------------------------------------------------------

const vec3 UE_AP1_RGB2Y = vec3(0.272228718, 0.674081743, 0.0536895171);

// sRGB -> AP1 (lines 412-434)
const vec3 UE_SRGB_TO_AP1_R = vec3(0.613191545, 0.33951214, 0.0473663509);
const vec3 UE_SRGB_TO_AP1_G = vec3(0.0702069178, 0.916335762, 0.013450007);
const vec3 UE_SRGB_TO_AP1_B = vec3(0.0206188727, 0.109567299, 0.869606793);

// AP1 -> sRGB (lines 722-742, 1097-1117)
const vec3 UE_AP1_TO_SRGB_R = vec3(1.70505154, -0.621790588, -0.0832584798);
const vec3 UE_AP1_TO_SRGB_G = vec3(-0.130257189, 1.14080274, -0.0105485283);
const vec3 UE_AP1_TO_SRGB_B = vec3(-0.0240032785, -0.12896876, 1.15297186);

// AP1 -> AP0 used by the film path (lines 765-785)
const vec3 UE_AP1_TO_AP0_R = vec3(0.695452213, 0.140678689, 0.163869053);
const vec3 UE_AP1_TO_AP0_G = vec3(0.0447945818, 0.859671116, 0.0955343246);
const vec3 UE_AP1_TO_AP0_B = vec3(-0.00552588282, 0.00402521016, 1.00150073);

// AP0 -> AP1 used by the film path (lines 879-896)
const vec3 UE_AP0_TO_AP1_R = vec3(1.45143926, -0.236510754, -0.214928567);
const vec3 UE_AP0_TO_AP1_G = vec3(-0.0765537769, 1.17622972, -0.0996759236);
const vec3 UE_AP0_TO_AP1_B = vec3(0.00831614807, -0.00603244966, 0.997716308);

// Expand gamut "wide" matrix, applied as (M - I) (lines 461-479)
const vec3 UE_WIDE_R = vec3(1.37041283, -0.329291314, -0.0636828393);
const vec3 UE_WIDE_G = vec3(-0.0834342614, 1.09709084, -0.0108615672);
const vec3 UE_WIDE_B = vec3(-0.02579326, -0.098625645, 1.20369446);

// BlueCorrectAP1 and its inverse, applied as (M - I) (lines 748-785 and 1074-1100)
const vec3 UE_BLUE_R = vec3(0.938639402, -4.54019755e-09, 0.0613606423);
const vec3 UE_BLUE_G = vec3(6.77537173e-08, 0.830794215, 0.169205904);
const vec3 UE_BLUE_B = vec3(-9.31322575e-10, -2.32830644e-10, 1.0);   // third entry: decompile has no B term
const vec3 UE_BLUE_INV_R = vec3(1.06537473, 1.45181548e-06, -0.0653710365);
const vec3 UE_BLUE_INV_G = vec3(-2.57161446e-07, 1.20366347, -0.20366770);
const vec3 UE_BLUE_INV_B = vec3(1.86264515e-08, 2.0954758e-08, 0.999999642);

// sRGB <-> XYZ for white balance (lines 283-406)
const vec3 UE_SRGB_TO_XYZ_COL0 = vec3(0.412456393, 0.212672904, 0.0193339009);
const vec3 UE_SRGB_TO_XYZ_COL1 = vec3(0.357576102, 0.715152204, 0.119191997);
const vec3 UE_SRGB_TO_XYZ_COL2 = vec3(0.180437505, 0.0721750036, 0.950304091);
const vec3 UE_XYZ_TO_SRGB_R = vec3(3.2409699, -1.5373832, -0.498610765);
const vec3 UE_XYZ_TO_SRGB_G = vec3(-0.969243646, 1.8759675, 0.0415550582);
const vec3 UE_XYZ_TO_SRGB_B = vec3(0.0556300804, -0.203976959, 1.05697155);

// Bradford CAT folded by the compiler: CAT[i][j] = dot(K_ij, 1 / SrcConeResp), with
// K_ij[k] = InvConeResponse[i][k] * D65ConeResp[k] * ConeResponse[k][j] (lines 262-331).
const vec3 UE_CAT_K00 = vec3(0.831668377, 0.114781074, 0.00678112498);
const vec3 UE_CAT_K01 = vec3(0.247521475, -0.262166619, -0.0119410567);
const vec3 UE_CAT_K02 = vec3(-0.149962336, -0.00561512355, 0.179481924);
const vec3 UE_CAT_K10 = vec3(0.364272803, -0.404598534, 0.00208954839);
const vec3 UE_CAT_K11 = vec3(0.108415015, 0.924126446, -0.00367953908);
const vec3 UE_CAT_K12 = vec3(-0.0656838715, 0.0197930764, 0.0553058907);
const vec3 UE_CAT_K20 = vec3(-0.00718652597, -0.0312548205, 0.0410560071);
const vec3 UE_CAT_K21 = vec3(-0.0021388568, 0.0713878125, -0.0722965747);
const vec3 UE_CAT_K22 = vec3(0.00129583885, 0.00152899476, 1.08666492);

// Output gamut matrices AP1 -> output, selected by dword 139 (lines 1302-1327; uint bit patterns decoded)
const vec3 UE_AP1_TO_SRGB_OUT_R = vec3(1.70505154, -0.621790588, -0.0832584798);   // 0 / other
const vec3 UE_AP1_TO_SRGB_OUT_G = vec3(-0.130257189, 1.14080274, -0.0105485283);
const vec3 UE_AP1_TO_SRGB_OUT_B = vec3(-0.0240032785, -0.12896876, 1.15297186);
const vec3 UE_AP1_TO_P3_R = vec3(1.37915885, -0.308850706, -0.0703467727);          // 1
const vec3 UE_AP1_TO_P3_G = vec3(-0.0693352893, 1.0822922, -0.012962047);
const vec3 UE_AP1_TO_P3_B = vec3(-0.00215925928, -0.0454653911, 1.04775965);
const vec3 UE_AP1_TO_2020_R = vec3(1.02579927, -0.0200525038, -0.00577139854);     // 2
const vec3 UE_AP1_TO_2020_G = vec3(-0.0022350112, 1.00458252, -0.00235230662);
const vec3 UE_AP1_TO_2020_B = vec3(-0.00501400419, -0.0252933856, 1.03044021);
const vec3 UE_AP1_TO_AP0_OUT_R = vec3(0.695452213, 0.140678704, 0.163869068);      // 3
const vec3 UE_AP1_TO_AP0_OUT_G = vec3(0.0447945632, 0.859671116, 0.0955343172);
const vec3 UE_AP1_TO_AP0_OUT_B = vec3(-0.00552588282, 0.00402521016, 1.00150073);
                                                                                    // 4: identity

// The LUT write scales by 1/1.05; the HDR tonemapper (12178) multiplies the sample by 1.05 (line 3308).
const float UE_LUT_OUTPUT_SCALE = 0.952381015;

// ------------------------------------------------------------------------------------------------
// Small helpers
// ------------------------------------------------------------------------------------------------

vec3 GradeParam3(uint i) { return vec3(GradeParam(i), GradeParam(i + 1u), GradeParam(i + 2u)); }

// Row-major 3x3 multiply: (dot(r0,c), dot(r1,c), dot(r2,c)).
vec3 UeMul3(vec3 r0, vec3 r1, vec3 r2, vec3 c) { return vec3(dot(r0, c), dot(r1, c), dot(r2, c)); }

// (M - I) * c in the compiler's evaluation order: row i = (m_i0*c.x - c_i) + m_i1*c.y + m_i2*c.z.
vec3 UeMulMinusIdentity(vec3 r0, vec3 r1, vec3 r2, vec3 c)
{
    return vec3(((r0.x * c.x - c.x) + r0.y * c.y) + r0.z * c.z,
                ((r1.x * c.x - c.y) + r1.y * c.y) + r1.z * c.z,
                ((r2.x * c.x - c.z) + r2.y * c.y) + r2.z * c.z);
}

vec3 SrgbToAp1(vec3 c) { return UeMul3(UE_SRGB_TO_AP1_R, UE_SRGB_TO_AP1_G, UE_SRGB_TO_AP1_B, c); }
vec3 Ap1ToSrgb(vec3 c) { return UeMul3(UE_AP1_TO_SRGB_R, UE_AP1_TO_SRGB_G, UE_AP1_TO_SRGB_B, c); }

// ------------------------------------------------------------------------------------------------
// LUT input decode
// ------------------------------------------------------------------------------------------------

// Device >= 3 (lines 129-160). lut_coord01 = texel-centre coordinate (i + 0.5) / 32 per channel.
// x = (32/31) * coord - 0.5/31 = i/31, then ST2084 inverse x 10000 (1.0 = 1 nit, no x0.01).
// The original takes B directly as layer/31 (line 129); same value up to float rounding.
vec3 LutDecodeHdr(vec3 lut_coord01)
{
    vec3 e = 1.03225803 * lut_coord01 + (-0.0161290318);
    vec3 ep = exp2(0.0126833133 * log2(abs(e)));                                   // E^(1/m2)
    vec3 n = (1.0 / fma(vec3(-18.6875), ep, vec3(18.8515625))) * max(vec3(0.0), ep + (-0.8359375));
    return 10000.0 * exp2(6.27739477 * log2(abs(n)));                               // ^(1/m1) x 10000
}

// Device < 3 (lines 167-182): UE LogToLin with the LogToLin(0) offset and the half-texel shift folded:
// 0.18 * exp2(14 * (32/31 coord) - 6.30205) - 0.0026677. The original uses the equivalent
// 14 * layer/31 - 6.07625 for B (line 175).
vec3 LutDecodeSdr(vec3 coord01)
{
    vec3 x = 1.03225803 * coord01;
    return fma(vec3(0.180000007), exp2(fma(vec3(14.0), x, vec3(-6.3020525))), vec3(-0.00266771927));
}

// Bit-exact variants taking the interpolated UV (texel centre, (i + 0.5) / 32) and the slice index the way
// the original does (B = layer * (1/31), lines 129-131 / 173-175). Use these in a replacement shader:
// near-neutral dark colours are ill-conditioned (cancellation in BlueCorrectInv / AP1->sRGB), so a 1-ulp
// input difference in B can move such outputs by ~1e-4 relative.
vec3 LutDecodeHdrTexel(vec2 uv, uint layer)
{
    vec3 e = vec3(1.03225803 * uv.x + (-0.0161290318), 1.03225803 * uv.y + (-0.0161290318),
                  0.0322580636 * float(layer));
    vec3 ep = exp2(0.0126833133 * log2(abs(e)));
    vec3 n = (1.0 / fma(vec3(-18.6875), ep, vec3(18.8515625))) * max(vec3(0.0), ep + (-0.8359375));
    return 10000.0 * exp2(6.27739477 * log2(abs(n)));
}

vec3 LutDecodeSdrTexel(vec2 uv, uint layer)
{
    float b = 0.0322580636 * float(layer);
    vec3 x = vec3(1.03225803 * uv.x, 1.03225803 * uv.y, b);
    return fma(vec3(0.180000007), exp2(fma(vec3(14.0), x, vec3(-6.3020525, -6.3020525, -6.07624626))),
               vec3(-0.00266771927));
}

// ------------------------------------------------------------------------------------------------
// White balance (lines 183-418): UE WhiteBalance(WhiteTemp = dw52, WhiteTint = dw53), Bradford, no
// bIsTemperatureWhiteBalance switch in 4.21.
// ------------------------------------------------------------------------------------------------

vec3 WhiteBalance(vec3 c)
{
    float temp = GradeParam(52u);
    float tint = GradeParam(53u);

    // D_IlluminantChromaticity (Temp *= 1.4388/1.438), Horner in 1/T (lines 185-206, 241)
    float tD = 1.00055635 * temp;
    float rT = 1.0 / tD;
    float dayX = (temp <= 6996.10791)
        ? fma(rT, fma(rT, fma(-4607000064.0, rT, 2967800.0), 99.1100006), 0.244063005)
        : fma(rT, fma(rT, fma(-2006400000.0, rT, 1901800.0), 247.479996), 0.237039998);
    float dayY = fma(dayX, fma(-3.0, dayX, 2.86999989), -0.275000006);

    // PlanckianLocusChromaticity uv (lines 208-216)
    float u = (1.0 / (temp * fma(7.08145137e-07, temp, 0.00084242021) + 1.0))
            * fma(temp, fma(1.28641219e-07, temp, 0.000154118257), 0.860117733);
    float v = (1.0 / (temp * fma(1.61456057e-07, temp, -2.8974182e-05) + 1.0))
            * fma(temp, fma(4.20481676e-08, temp, 4.22806261e-05), 0.317398727);

    // PlanckianIsothermal: offset along the normalized (u,v) by Tint * 0.05 (lines 217-228)
    float k = tint * inversesqrt(v * v + u * u);
    float uIso = (k * v) * (-0.0500000007) + u;
    float vIso = (k * u) * 0.0500000007 + v;

    // uv -> xy (lines 220-247)
    float dP = 1.0 / ((v * (-8.0) + 2.0 * u) + 4.0);
    float planckX = 3.0 * (dP * u);
    float planckY = 2.0 * (dP * v);
    float dI = 1.0 / ((vIso * (-8.0) + 2.0 * uIso) + 4.0);
    float isoX = 3.0 * (dI * uIso);
    float isoY = 2.0 * (dI * vIso);

    // SrcWhite = (T < 4000 ? Planckian : Daylight) + (Isothermal - Planckian) (lines 235-248)
    bool planck = temp < 4000.0;
    float srcX = ((-planckX) + (planck ? planckX : dayX)) + isoX;
    float srcY = ((-planckY) + (planck ? planckY : dayY)) + isoY;

    // xyY -> XYZ (Y = 1), cone response of the source white (lines 249-271)
    float invY = 1.0 / max(1.00000001e-10, srcY);
    float X = invY * srcX;
    float Z = invY * (1.0 + ((-srcX) + (-srcY)));
    vec3 invLms = 1.0 / vec3((0.895099998 * X + (-0.161400005) * Z) + 0.266400009,
                             ((-0.750199974) * X + 0.0366999991 * Z) + 1.71350002,
                             (0.0388999991 * X + 1.02960002 * Z) + (-0.0684999973));

    // CAT = InvCone * diag(D65 / Src) * Cone (folded), WB = XYZ_2_sRGB * CAT * sRGB_2_XYZ (lines 266-406)
    vec3 cat0 = vec3(dot(UE_CAT_K00, invLms), dot(UE_CAT_K01, invLms), dot(UE_CAT_K02, invLms));
    vec3 cat1 = vec3(dot(UE_CAT_K10, invLms), dot(UE_CAT_K11, invLms), dot(UE_CAT_K12, invLms));
    vec3 cat2 = vec3(dot(UE_CAT_K20, invLms), dot(UE_CAT_K21, invLms), dot(UE_CAT_K22, invLms));
    vec3 p0 = vec3(dot(cat0, UE_SRGB_TO_XYZ_COL0), dot(cat0, UE_SRGB_TO_XYZ_COL1), dot(cat0, UE_SRGB_TO_XYZ_COL2));
    vec3 p1 = vec3(dot(cat1, UE_SRGB_TO_XYZ_COL0), dot(cat1, UE_SRGB_TO_XYZ_COL1), dot(cat1, UE_SRGB_TO_XYZ_COL2));
    vec3 p2 = vec3(dot(cat2, UE_SRGB_TO_XYZ_COL0), dot(cat2, UE_SRGB_TO_XYZ_COL1), dot(cat2, UE_SRGB_TO_XYZ_COL2));
    vec3 pc0 = vec3(p0.x, p1.x, p2.x);
    vec3 pc1 = vec3(p0.y, p1.y, p2.y);
    vec3 pc2 = vec3(p0.z, p1.z, p2.z);
    vec3 wb0 = vec3(dot(UE_XYZ_TO_SRGB_R, pc0), dot(UE_XYZ_TO_SRGB_R, pc1), dot(UE_XYZ_TO_SRGB_R, pc2));
    vec3 wb1 = vec3(dot(UE_XYZ_TO_SRGB_G, pc0), dot(UE_XYZ_TO_SRGB_G, pc1), dot(UE_XYZ_TO_SRGB_G, pc2));
    vec3 wb2 = vec3(dot(UE_XYZ_TO_SRGB_B, pc0), dot(UE_XYZ_TO_SRGB_B, pc1), dot(UE_XYZ_TO_SRGB_B, pc2));
    return UeMul3(wb0, wb1, wb2, c);
}

// ------------------------------------------------------------------------------------------------
// Expand gamut (lines 436-497), only when dw39 != 0. ExpandGamut = dw141.
// ExpandAmount = (1 - exp2(-4 * ChromaDist^2)) * (1 - exp2(-4 * ExpandGamut * Luma^2)).
// NaN guard (not in the original): for luma == 0 (exact black, e.g. the HDR texel (0,0,0)) the original
// computes 0 * inf = NaN here and only gets back to U = shadow/global offsets because max(0, NaN) returns 0
// (IEEE maxNum, as GCN v_max_f32). Returning ap1 unchanged gives that same U without depending on the
// driver's max/clamp NaN behaviour (identical results for ap1 == 0).
// ------------------------------------------------------------------------------------------------

vec3 ExpandGamut(vec3 ap1, float expand_gamut)
{
    float luma = dot(UE_AP1_RGB2Y, ap1);
    if (luma == 0.0) return ap1;
    float invLuma = 1.0 / luma;
    float dr = ap1.x * invLuma + (-1.0);
    float dg = ap1.y * invLuma + (-1.0);
    float db = ap1.z * invLuma + (-1.0);
    float chromaTerm = ((-db) * db + (-(dg * dg + dr * dr))) * 4.0;      // -4 * ChromaDist^2
    float lumaTerm = ((-(expand_gamut * luma)) * luma) * 4.0;              // -4 * ExpandGamut * Luma^2
    float a = 1.0 - exp2(lumaTerm);
    float amount = a * (-exp2(chromaTerm)) + a;
    return UeMulMinusIdentity(UE_WIDE_R, UE_WIDE_G, UE_WIDE_B, ap1) * amount + ap1;
}

// ------------------------------------------------------------------------------------------------
// ColorCorrectAll (lines 498-733). UE 4.21 order; the PS4 compiler regrouped the products, so this
// matches to float rounding (not bit-exact).
// ------------------------------------------------------------------------------------------------

vec3 UeColorCorrect(vec3 c, float luma, uint base, uint range_base)
{
    // Range value combined with the global value: sat/contrast/gamma/gain multiply, offset adds.
    vec3 sat = GradeParam3(range_base + 0u) * GradeParam3(base + 0u);
    float satW = GradeParam(range_base + 3u) * GradeParam(base + 3u);
    vec3 con = GradeParam3(range_base + 4u) * GradeParam3(base + 4u);
    float conW = GradeParam(range_base + 7u) * GradeParam(base + 7u);
    vec3 gam = GradeParam3(range_base + 8u) * GradeParam3(base + 8u);
    float gamW = GradeParam(range_base + 11u) * GradeParam(base + 11u);
    vec3 gain = GradeParam3(range_base + 12u) * GradeParam3(base + 12u);
    float gainW = GradeParam(range_base + 15u) * GradeParam(base + 15u);
    vec3 off = GradeParam3(range_base + 16u) + GradeParam3(base + 16u);
    float offW = GradeParam(range_base + 19u) + GradeParam(base + 19u);

    c = max(vec3(0.0), (c - luma) * (sat * satW) + luma);                         // saturation around luma
    c = 0.180000007 * exp2(log2(5.55555534 * c) * (con * conW));                  // pow(c / 0.18, contrast) * 0.18
    c = exp2(log2(c) * (1.0 / (gam * gamW)));                                     // pow(c, 1 / gamma)
    return c * (gain * gainW) + (off + offW);                                     // gain, offset
}

vec3 ColorCorrectAll(vec3 ap1)
{
    float luma = dot(UE_AP1_RGB2Y, ap1);                                          // line 1037

    // Weights (lines 558-578, 638): shadows 1 - smoothstep(0, dw136, luma),
    // highlights smoothstep(dw137, 1, luma), midtones 1 - S - H.
    float ts = clamp((1.0 / GradeParam(136u)) * luma, 0.0, 1.0);
    float wS = 1.0 + (-(ts * ts)) * fma(-2.0, ts, 3.0);
    float hMin = GradeParam(137u);
    float th = clamp((1.0 / (1.0 - hMin)) * (luma - hMin), 0.0, 1.0);
    float wH = (th * th) * fma(-2.0, th, 3.0);
    float wM = 1.0 + ((-wH) + (-wS));

    vec3 cS = UeColorCorrect(ap1, luma, 56u, 76u);    // global 56-75, shadows 76-95
    vec3 cM = UeColorCorrect(ap1, luma, 56u, 96u);    // midtones 96-115
    vec3 cH = UeColorCorrect(ap1, luma, 56u, 116u);   // highlights 116-135
    return (cS * wS + cM * wM) + cH * wH;
}

// ------------------------------------------------------------------------------------------------
// Blue correction (lines 743-785) and its inverse (lines 1074-1111), BlueCorrection = dw140.
// ------------------------------------------------------------------------------------------------

vec3 BlueCorrect(vec3 ap1, float amount)
{
    vec3 d = vec3(((UE_BLUE_R.x * ap1.x - ap1.x) + UE_BLUE_R.y * ap1.y) + UE_BLUE_R.z * ap1.z,
                  ((UE_BLUE_G.x * ap1.x - ap1.y) + UE_BLUE_G.y * ap1.y) + UE_BLUE_G.z * ap1.z,
                  UE_BLUE_B.x * ap1.x + UE_BLUE_B.y * ap1.y);                     // B row of (M - I): no B term
    return amount * d + ap1;
}

vec3 BlueCorrectInv(vec3 ap1, float amount)
{
    return amount * UeMulMinusIdentity(UE_BLUE_INV_R, UE_BLUE_INV_G, UE_BLUE_INV_B, ap1) + ap1;
}

// ------------------------------------------------------------------------------------------------
// FilmToneMap (lines 763-1077): ACES RRT glow + red modifier, 0.96 desat, UE film curve, 0.93 desat.
// ------------------------------------------------------------------------------------------------

// atan2(y, x) in degrees exactly as the PS4 compiler expanded it (lines 838-847): minimax polynomial on
// min/max ratio + quadrant fix-up. Differs from a libm atan2 by ~1e-5 rad.
float UeAtan2Degrees(float y, float x)
{
    float a = (1.0 / max(abs(y), abs(x))) * min(abs(y), abs(x));
    float s = a * a;
    float p = fma(fma(fma(fma(0.020835, s, -0.085133), s, 0.180141), s, -0.330299), s, 0.999866);
    float off = ((abs(x) < abs(y)) ? fma(-2.0, a * p, 1.57079637) : 0.0) + ((0.0 > x) ? -3.14159274 : 0.0);
    float ang = p * a + off;
    float lo = min(y, x);
    float hi = max(y, x);
    return 57.2957764 * (((lo < -lo) && (hi >= -hi)) ? -ang : ang);
}

// One channel of the film curve (lines 949-1065). log_c = log10(c), log2_c = log2(c).
float UeFilmCurve(float log2_c, float slope, float bc, float toe_scale, float shoulder_scale, float white1,
                  float toe_match, float straight_match, float shoulder_match)
{
    float logc = 0.30103001 * log2_c;
    float straight = slope * (log2_c * 0.30103001 + straight_match);

    float toeK = ((-slope) * (1.0 / toe_scale)) * 2.0;
    float toeC = (1.0 / (1.0 + exp2(1.44269502 * (toeK * (logc - toe_match))))) * (2.0 * toe_scale) + (-bc);

    float shK = (slope * (1.0 / shoulder_scale)) * 2.0;
    float shC = (1.0 / (1.0 + exp2(shK * (logc * 1.44269502 + (-1.44269502) * shoulder_match))))
              * ((-2.0) * shoulder_scale) + white1;

    float t = clamp((1.0 / (shoulder_match - toe_match)) * (logc - toe_match), 0.0, 1.0);
    if (shoulder_match < toe_match) t = 1.0 - t;
    t = (t * t) * fma(-2.0, t, 3.0);

    float toeSel = (logc < toe_match) ? toeC : straight;
    float shSel = (logc > shoulder_match) ? shC : straight;
    // NaN guard (not in the original): c == 0 gives log = -inf, straight = -inf and t = 0, so the original's
    // (-inf - toe) * 0 = NaN, later removed by max(0, NaN) = 0. For finite values t == 0 returns exactly what
    // the lerp returns (x * 0 + toe == toe), so this only replaces the NaN.
    if (t == 0.0) return toeSel;
    return (shSel - toeSel) * t + toeSel;
}

vec3 FilmToneMap(vec3 ap1, bool zero_black_clip)
{
    // --- RRT-style hue/sat modifiers in AP0 (lines 765-878) ---
    vec3 aces = UeMul3(UE_AP1_TO_AP0_R, UE_AP1_TO_AP0_G, UE_AP1_TO_AP0_B, ap1);
    float r = aces.x, g = aces.y, b = aces.z;
    float mx = max(max(r, g), b);
    float mn = min(b, min(r, g));
    float sat = (1.0 / max(0.00999999978, mx)) * (max(1.00000001e-10, mx) - max(1.00000001e-10, mn));

    // Glow (lines 792-829): ycSum = 3 * rgb_2_yc, thresholds compared against the un-divided sum.
    float x = 5.0 * sat + (-2.0);
    float sgn = sign(x);
    float t = max(0.0, 0.5 * (-abs(x)) + 1.0);
    float sigmoid = t * (sgn * (-t)) + sgn;
    float glow = 0.0250000004 * sigmoid + 0.0250000004;
    float chroma2 = (r - b) * r + (g * (g - r) + b * (b - g));
    float ycSum = sqrt(chroma2) * 1.75 + ((r + g) + b);
    float glowOut = glow;
    if (ycSum > 0.159999996)
        glowOut = (0.479999989 <= ycSum) ? 0.0 : glow * (0.0799999982 * (1.0 / (0.333333343 * ycSum)) + (-0.5));
    aces *= 1.0 + glowOut;

    // Red modifier (lines 830-878): hue in degrees, centred, smoothstep(0, 1, 1 - |h| / 67.5)^2.
    float hue = 0.0;
    if (!((aces.x == aces.y) && (aces.y == aces.z)))
        hue = UeAtan2Degrees(1.73205090 * (aces.y - aces.z), 2.0 * aces.x + ((-aces.z) + (-aces.y)));
    if (0.0 > hue) hue = 360.0 + hue;
    hue = max(min(360.0, hue), 0.0);           // decompile: max(min(360,h), min(max(360,h),0)) (line 857)
    if (180.0 < hue) hue = -360.0 + hue;
    float hw = clamp(0.0148148146 * (-abs(hue)) + 1.0, 0.0, 1.0);
    hw = (hw * hw) * fma(-2.0, hw, 3.0);
    aces.x = (((sat * hw) * hw) * (0.0299999993 - aces.x)) * 0.180000007 + aces.x;

    // --- AP0 -> AP1, clamp, 0.96 desaturation (lines 879-918) ---
    vec3 c = max(vec3(0.0), UeMul3(UE_AP0_TO_AP1_R, UE_AP0_TO_AP1_G, UE_AP0_TO_AP1_B, aces));
    float l = dot(UE_AP1_RGB2Y, c);
    c = (c - l) * 0.959999979 + l;

    // --- Film curve parameters (lines 897-955) ---
    float slope = GradeParam(40u);
    float toe = GradeParam(41u);
    float shoulder = GradeParam(42u);
    float bc = zero_black_clip ? 0.0 : GradeParam(43u);
    float white1 = 1.0 + GradeParam(44u);
    float toeScale = 1.0 + (bc - toe);
    float shoulderScale = white1 - shoulder;
    float toeMatch;
    if (toe > 0.800000012)
    {
        toeMatch = fma(1.0 / slope, 0.819999993 - toe, -0.744727492);           // (0.82 - Toe)/Slope + log10(0.18)
    }
    else
    {
        float bt1 = (1.0 / toeScale) * (0.180000007 + bc);                        // 1 + bt
        float ratio = (1.0 / (1.0 - (bt1 + (-1.0)))) * bt1;                       // (1 + bt) / (1 - bt)
        toeMatch = fma(-0.346573591, (toeScale * log2(ratio)) * (1.0 / slope), -0.744727492);
    }
    float invSlope = 1.0 / slope;
    float straightMatch = invSlope * (1.0 - toe) + (-toeMatch);
    float shoulderMatch = shoulder * invSlope + (-straightMatch);

    vec3 tone = vec3(
        UeFilmCurve(log2(c.x), slope, bc, toeScale, shoulderScale, white1, toeMatch, straightMatch, shoulderMatch),
        UeFilmCurve(log2(c.y), slope, bc, toeScale, shoulderScale, white1, toeMatch, straightMatch, shoulderMatch),
        UeFilmCurve(log2(c.z), slope, bc, toeScale, shoulderScale, white1, toeMatch, straightMatch, shoulderMatch));

    // --- 0.93 desaturation, clamp (lines 1055-1081) ---
    float tl = dot(UE_AP1_RGB2Y, tone);
    return max(vec3(0.0), (tone - tl) * 0.930000007 + tl);
}

// ------------------------------------------------------------------------------------------------
// Legacy ("mobile") tonemapper, used when dw39 == 0 (lines 1131-1229). Input: GradedColor (linear sRGB).
// ------------------------------------------------------------------------------------------------

vec3 LegacyTonemap(vec3 c)
{
    float inv = 1.0 / (1.0 + dot(GradeParam3(28u), c));                           // ColorShadow_Luma
    vec3 tint = GradeParam3(36u) * inv + GradeParam3(32u);                        // Tint2 * rcp + Tint1
    vec3 m = UeMul3(GradeParam3(8u), GradeParam3(12u), GradeParam3(16u), c);      // ColorMatrix R/G/B
    m = max(vec3(0.0), m * tint);

    float cm0 = GradeParam(20u), cd0 = GradeParam(21u), cd2 = GradeParam(22u), ch3 = GradeParam(23u);
    float ch1 = GradeParam(24u), ch2 = GradeParam(25u);
    float cd1 = GradeParam(11u), cd3cm3 = GradeParam(15u), cm2 = GradeParam(19u);

    vec3 d = max(vec3(0.0), cm0 - m);
    vec3 mid = max(vec3(cm0), min(vec3(cd2), m));
    vec3 h = max(vec3(cd2), m);
    vec3 darkMid = (d * (cd1 * (1.0 / (d + cd0))) + (cm2 * mid + cd3cm3)) + (-0.00200000009);
    return (h * ch1 + ch2) * (1.0 / (h + ch3)) + darkMid;
}

// ------------------------------------------------------------------------------------------------
// GradeScene: scene colour (LUT-decoded linear sRGB) -> U (untonemapped graded, linear sRGB, line 742)
// and G (SDR graded colour, linear sRGB = FilmColor -> polynomial -> scale/overlay -> ^InverseGamma.y,
// line 1272). zero_black_clip forces FilmBlackClip (dw43) to 0 in the film curve.
// ------------------------------------------------------------------------------------------------

void GradeScene(vec3 scene_linear, bool zero_black_clip, out vec3 U_srgb, out vec3 G_srgb)
{
    bool film = floatBitsToUint(GradeParam(39u)) != 0u;                           // integer test (line 407)

    vec3 ap1 = SrgbToAp1(WhiteBalance(scene_linear));                             // lines 183-434
    if (film) ap1 = ExpandGamut(ap1, GradeParam(141u));                           // lines 436-497
    vec3 untonemapped = ColorCorrectAll(ap1);                                     // lines 498-733
    U_srgb = Ap1ToSrgb(untonemapped);                                             // lines 734-742 (GradedColor)

    vec3 tonemapped;
    if (film)
    {
        float blue = GradeParam(140u);
        vec3 t = FilmToneMap(BlueCorrect(untonemapped, blue), zero_black_clip);   // lines 743-1077
        tonemapped = max(vec3(0.0), Ap1ToSrgb(BlueCorrectInv(t, blue)));         // lines 1078-1120
    }
    else
    {
        tonemapped = LegacyTonemap(U_srgb);                                       // lines 1131-1229
    }

    // MappingPolynomial (lines 1232-1246)
    float pa = GradeParam(0u), pb = GradeParam(1u), pc = GradeParam(2u);
    vec3 c = (tonemapped * pa + pb) * tonemapped + pc;
    // ColorScale / Overlay: lerp(c * ColorScale, Overlay.rgb, Overlay.a) (lines 1247-1265)
    vec3 scaled = GradeParam3(45u) * c;
    c = GradeParam(51u) * (GradeParam3(48u) + (-scaled)) + scaled;
    // InverseGamma.y (lines 1266-1272)
    G_srgb = exp2(GradeParam(5u) * log2(max(vec3(0.0), c)));
}

void GradeScene(vec3 scene_linear, out vec3 U_srgb, out vec3 G_srgb)
{
    GradeScene(scene_linear, false, U_srgb, G_srgb);
}

// ------------------------------------------------------------------------------------------------
// Output helpers (device 0 and device 7 of the original)
// ------------------------------------------------------------------------------------------------

// Device 0 sRGB OETF (lines 3270-3280). The original evaluates it on log2(G) (the pre-exp2 value of
// line 1267); here log2 is recomputed from G. No saturate: the UNORM target clamps on write.
vec3 SrgbOetf(vec3 lin)
{
    vec3 lg = log2(lin);
    vec3 hi = fma(vec3(1.05499995), exp2(0.416666657 * lg), vec3(-0.0549999997));
    return vec3((-8.31931305 > lg.x) ? 12.9200001 * lin.x : hi.x,
                (-8.31931305 > lg.y) ? 12.9200001 * lin.y : hi.y,
                (-8.31931305 > lg.z) ? 12.9200001 * lin.z : hi.z);
}

// AP1 -> output gamut by dw139 (lines 1302-1327): 1 P3-D65, 2 Rec.2020, 3 AP0, 4 identity, else sRGB.
vec3 Ap1ToOutputGamut(vec3 ap1, uint gamut)
{
    if (gamut == 1u) return UeMul3(UE_AP1_TO_P3_R, UE_AP1_TO_P3_G, UE_AP1_TO_P3_B, ap1);
    if (gamut == 2u) return UeMul3(UE_AP1_TO_2020_R, UE_AP1_TO_2020_G, UE_AP1_TO_2020_B, ap1);
    if (gamut == 3u) return UeMul3(UE_AP1_TO_AP0_OUT_R, UE_AP1_TO_AP0_OUT_G, UE_AP1_TO_AP0_OUT_B, ap1);
    if (gamut == 4u) return ap1;
    return UeMul3(UE_AP1_TO_SRGB_OUT_R, UE_AP1_TO_SRGB_OUT_G, UE_AP1_TO_SRGB_OUT_B, ap1);
}

// ST2084 encode of nits / 10000 (lines 3150-3170, 3191). Uses |x| like the original (negatives fold).
vec3 PqEncode10k(vec3 nits)
{
    vec3 y = 9.99999975e-05 * nits;
    vec3 p = exp2(0.159301758 * log2(abs(y)));
    return exp2(78.84375 * log2((1.0 / (18.6875 * p + 1.0)) * fma(vec3(18.8515625), p, vec3(0.8359375))));
}

// Device 7 "linear, no tone curve" (lines 3118-3193): GradedColor -> AP1 -> output gamut -> PQ(x 1e-4).
// Multiply by UE_LUT_OUTPUT_SCALE for the value the LUT stores.
vec3 Device7Output(vec3 graded_srgb, uint gamut)
{
    return PqEncode10k(Ap1ToOutputGamut(SrgbToAp1(graded_srgb), gamut));
}
