# MediEvil combine-LUT builder (fs a4152b3be93c832a) and UI LUT builder (fs c401ca35ab81eb39): stage map

Read-only analysis, 2026-11-17. Sources: scratchpad `hdr/lut_a415.glsl` (3316 lines), `hdr/fs_c401ca35ab81eb39.glsl`
(1855 lines), `hdr/fs_12178a9314cb761e.glsl` (HDR tonemapper, for the consumer side), IR dump
`user/shader/dumps/fs_0xa4152b3be93c832a.irprogram.txt`, captured buffers
`research-data/hdr-ui-20261117/lutbuf/eid3495_b{0,1}_*.bin` (menu, device 3, gamut 2), RenoDX `bfdb52d`
(`src/games/unrealengine/common.hlsl`, `lutbuilders/sm5/lutbuilder_0x30AD540E.ps_5_x.hlsl`), marat `11089c9`
(`src/games/ue-extended/lutbuilder/lutbuilderoutput.hlsli`, `SKILL.md`).
Tags: [V] read in code / data, [I] inference (names from UE, reasons).

## Resources of a415 [V]

- binding 0: SSBO `uint data[]` = parameter block (144 dwords, offset `buf0_dword_off` = push_data.buf_offsets0.x
  bits 0-7 >> 2).
- binding 1: SSBO aliased as uint/float/u16/u8 (`ssbo_2_*`), 448 bytes = the shader's immediate constant table (ACES
  spline coefficients, offset `buf1_dword_off` = buf_offsets0.x bits 8-15 >> 2). Captured contents [V]:
  bytes 0-39 ODT-2000 c9 coefsHigh (0.801995 .. 3.327431), 40-79 ODT-2000 coefsLow (-2.30103 .. 1.291187),
  80-223 RRT c5 coefsHigh/Low (-0.718548, 2.081031, 3.668124, 4,4,4 / -4,-4,-3.157377,-0.48525,1.847732,1.847732;
  three copies), 224-263 ODT-1000 coefsHigh (0.808913 .. 3.012739), 264-303 ODT-1000 coefsLow (-4.970622 .. 1.291187),
  304-447 RRT copies. Device 3 reads 224-272 (ODT 1000) and 304-432 (RRT); device 4 reads 4-48 and 80-208.
- push constant block `AuxData push_data` (shadPS4's own: buffer offsets; nothing of the game's).
- input: `fs_in_attr0_p[3]` (pervertexEXT, location 0) interpolated by hand with `gl_BaryCoordNoPerspEXT` -> UV;
  `gl_Layer` (GCN `RenderTargetIndex`) -> slice. Output: `frag_color0` (location 0).
- No textures, no samplers: this is the BLENDCOUNT=1 permutation (no LUTWeights / blend LUT textures).
- Same hash in the SDR dump (`user/shader/dumps-sdr-20261029`); it is the only combine-LUT builder in either dump
  (white-balance constant 6996.1 found only in a415) [V]. So SDR (device 0) and HDR (device 3) run the same code.

## Stage map of a415

| Lines | Stage | Detail |
|---|---|---|
| 120-130 | setup | `_298` = dword 138 (OutputDevice); `_327 = device >= 3`; `_329 = 0.0322581*gl_Layer` (= layer/31); `_331/_332 = 1.032258*uv` (32/31). LUT size 32 [V]. u->R, v->G, layer->B. |
| 131-160 | HDR input decode (device >= 3) | `x = (32/31)uv - 0.5/31` (layer/31 for B, no offset needed); ST2084 inverse: `10000 * ((max(0,E^(1/78.84375) - 0.8359375) / (18.8515625 - 18.6875 E^(1/m2)))^(1/0.1593))` -> nits, **no LinearToNitsScaleInverse (x0.01)**: scene linear 1.0 = 1 "nit" [V]. Matches the HDR tonemapper 12178, which encodes `PQ(scene * 1e-4)` and samples at `PQ*31/32 + 0.5/32` [V]. |
| 161-181 | SDR input decode (device < 3) | LogToLin with offsets folded: `0.18*exp2(14*x - 6.0762) - 0.0026677` (= LogToLin(x) - LogToLin(0); 14*(x-444/1023)); R/G use `-6.30205` = the -0.5/31 texel shift folded in [V]. |
| 183-418 | WhiteBalance (UE) | dword 52 WhiteTemp (daylight vs Planckian at 4000 K, `1.00055635*T`, threshold 6996.1), dword 53 WhiteTint (x0.05 along the isotherm), Bradford CAT (0.8951 ..), sRGB<->XYZ (0.4124 / 3.2410 ..). Result balanced linear sRGB `_751/_763/_770` (409/418/424). No bIsTemperatureWhiteBalance switch (4.21) [V]. |
| 419-434 | sRGB -> AP1 | `_779/_782/_785` (ungraded AP1; RenoDX SetUngradedAP1 point) [V]. |
| 407, 436-497 | Expand gamut | gated by `_749 = data[39] != 0` (integer compare in GCN IR: `INotEqual32 #0, %508`); ExpandAmount = (1-exp2(-4*ChromaDist^2))*(1-exp2(-4*ExpandGamut*Luma^2)), ExpandGamut = dword 141; lerp to the "wide" matrix. -> `_861/_862/_863` [V]. |
| 498-733 | ColorCorrectAll | Luma = dot(AP1, 0.2722/0.6741/0.0537) (`_1037`); per range: sat lerp around luma, max(0), pow(x/0.18, contrast)*0.18, pow(x, 1/gamma), *gain + offset; range value x global value (sat/contrast/gamma/gain multiply, offset add, `.xyz*.w` / `.xyz+.w`); weights: shadows `1 - smoothstep(0, d136, luma)` (1092-1098), highlights `smoothstep(d137, 1, luma)` (1099-1183), midtones `1 - S - H`. **U (untonemapped AP1) = `_1524` (710), `_1534` (718), `_1556` (733)** [V]. |
| 734-742 | GradedColor = AP1->sRGB(U) | `_1559` (735), `_1562` (737), `_1570` (742). This is what UE stores "for legacy tonemap and linear HDR"; **devices 3/4/5/6/7 all start from it** [V]. (The earlier note's "line 735" is this.) |
| 743-762 | Blue correction | inside `if (_749)`: `AP1 += BlueCorrection*(BlueCorrectAP1 - I)*AP1`, BlueCorrection = dword 140 [V]. |
| 763-1077 | FilmToneMap (UE ACES-fitted film curve) | AP1->AP0 (763-781); RRT glow (sat 1e-10/0.01 guards, `sigmoid_shaper`, 0.025 gain, 0.08 mid, 0.16/0.48 thresholds) (782-814); hue via atan2 polynomial (815-860); red modifier: hueWeight = smoothstep(0,1, 1 - abs(h)/67.5)^2 (0.0148148 = 2/135), `r += w*sat*(0.03 - r)*0.18` (861-878); AP0->AP1, max(0) (879-904); pre-desat 0.96 (909-918); film curve (897-1062) with dwords **40 Slope, 41 Toe, 42 Shoulder, 43 BlackClip, 44 WhiteClip**: ToeScale = 1 + BlackClip - Toe (`_1856`), ShoulderScale = 1 + WhiteClip - Shoulder (`_1858`); ToeMatch: Toe > 0.8 ? (0.82 - Toe)/Slope + log10(0.18) : log10(0.18) - 0.5*ln((1+bt)/(1-bt))*ToeScale/Slope with bt = (0.18 + BlackClip)/ToeScale - 1 (919-944); StraightMatch = (1 - Toe)/Slope - ToeMatch (`_1902`); ShoulderMatch = Shoulder/Slope - StraightMatch (`_1906`); Log = log10(c); Toe = -BlackClip + 2*ToeScale/(1+exp(-2*Slope/ToeScale*(Log - ToeMatch))); Shoulder = 1 + WhiteClip - 2*ShoulderScale/(1+exp(2*Slope/ShoulderScale*(Log - ShoulderMatch))); Straight = Slope*(Log + StraightMatch); blend with smoothstep t over [ToeMatch, ShoulderMatch] (flipped if ShoulderMatch < ToeMatch); post-desat 0.93 (1063-1077), max(0). **No ToneCurveAmount lerp** (4.21) [V]. |
| 1078-1120 | Uncorrect blue, AP1->sRGB | BlueCorrectInv (1.06537..) x dword 140; AP1->sRGB; FilmColor = max(0, ..) -> `_2110/_2111/_2112` (1118-1120). RenoDX SetTonemappedBT709 point = the pre-max values `_2102/_2104/_2106` (1113-1117) [V]. |
| 1131-1229 | Legacy / "mobile" tonemapper | `if (!_749)`: from GradedColor `_1559..`: ColorMatrix R/G/B (8-10, 12-14, 16-18), ColorShadow_Luma (28-30), Tint1 (32-34), Tint2 (36-38), ColorCurve Cm0/Cd0/Cd2/Ch0Cm1/Ch3 (20-23), Cd1 (11), Cd3Cm3 (15), Cm2 (19), Ch1/Ch2 (24-25), -0.002 [V]. Unused when dword 39 != 0. |
| 1232-1245 | MappingPolynomial | `y = (d0*x + d1)*x + d2` on linear FilmColor [V]. No saturate, no sRGB round trip (no LUT blending in this permutation) [V]. |
| 1247-1265 | ColorScale / Overlay | `lerp(x*ColorScale, Overlay.rgb, Overlay.a)`: ColorScale 45-47, Overlay 48-51 [V]. |
| 1266-1272 | InverseGamma.y | `pow(max(0,x), d5)`: log2 form `_2425-_2427`, linear `_2428/_2429/_2430` (1270-1272). **= RenoDX's GenerateOutput(graded) point (G, linear BT.709)** [V]. |
| 1281-1328 | device / gamut setup | `_2377 = device != 0`; gamut matrix AP1->output selected by dword 139 (stored as 9 uints `_2530.._2538`, anti-diagonal order: row R = (_2532,_2535,_2538), G = (_2531,_2534,_2537), B = (_2530,_2533,_2536)): 0/other -> sRGB (1.70505 ..), 1 -> P3 D65 (1.37916 ..), 2 -> Rec.2020 (1.02580, -0.02005, -0.00577 ..), 3 -> AP0 (0.69545 ..), 4 -> identity (AP1) [V]. |
| 1336-2158 | **device 3 / 5: ACES 1000-nit, PQ** | from GradedColor (no blue correction, no film, no polynomial / ColorScale / Overlay / InverseGamma): sRGB->AP0 (0.65955 ..) (1340-1353); RRT: glow (1354-1386), hue (1387-1440), red modifier with **cubic_basis_shaper** hue weight (0.0296 = 4/135, +-67.5 deg) (1441-1502), clamp [0, 65535] (1503-1520), AP0->AP1, desat 0.96 (1521-1540), c5 spline per channel (1541-1811; table bytes 304-447; 0.18 / 47185.88 bounds, -4 low fallback 3229614080u = -4.0, 4.0 high), AP1->AP0 = OCES (1812-1823); ODT 1000: AP0->AP1 (1824-1846), c9 spline per channel (1847-2115; bounds 0.0001418 / 4.8 / 4505.08, high slope 0.70891, table bytes 224-303; below min: `0.90309*log2 + 7.545`), `- 3.5073844e-5` (2116-2121), x AP1->gamut matrix (2117-2134), x1e-4, PQ encode m1 0.1593, c1 0.8359375, c2 18.8515625, c3 18.6875, m2 78.84375 (2135-2155) [V]. No dark-surround / dim-surround steps (4.21 ODT_1000nits) [V]. |
| 2160-2163 | flags | `_3602 = device != 7`, `_3603 = device 4 or 6`, `_3604 = !(3 or 5)`; `_3620 = device == 1` [V]. |
| 2237-3054 | device 4 / 6: ACES 2000-nit, PQ | same as 3/5 with ODT-2000 c9 (bounds 4.8 / 5771.856, slope 0.684143, table bytes 0-79) [V]. |
| 3069-3108 | device 2: explicit gamma | FilmColor^InvGamma.y (`_2428..`) -> sRGB->AP1 -> AP1->gamut -> `pow(x, d6)` (InverseGamma.z) [V]. |
| 3118-3174 | device 7: linear, no tone curve | GradedColor -> sRGB->AP1 -> AP1->gamut -> PQ(x*1e-4) (no 100 scale) [V]. |
| 3191-3193 | exp2 of the log2 forms (2 / 4 / 6 / 7 PQ results) | |
| 3219-3260 | device 1: Rec.709 | FilmColor^InvGamma.y -> AP1 -> gamut -> `min(4.5x, 1.099*max(0.018,x)^0.45 - 0.099)` [V]. |
| 3270-3281 | device 0: sRGB | on `_2425..` (log2 of FilmColor^InvGamma.y): `x < 0.0031308 (log2 < -8.3193) ? 12.92x : 1.055x^(1/2.4) - 0.055`; no saturate (the RGB10A2 UNORM target clamps on write) [V shader; I format]. |
| 3308-3314 | write | `frag_color0.rgb = 0.952381 * c` (= /1.05), `.w = 0.0` [V]. The tonemapper multiplies the LUT sample by 1.05 [V, 12178 lines 434-443]. |

Dispatch summary [V]: device 0 sRGB; 1 Rec.709; 2 gamma d6; 3/5 ACES1000 PQ; 4/6 ACES2000 PQ; 7 linear graded PQ.
Input decode PQ for device >= 3, log for < 3.

## Parameter block (buffer 0, 144 dwords) [V reads + captured value; I names]

| dw | role | read at | captured (menu, HDR) |
|---|---|---|---|
| 0, 1, 2 | MappingPolynomial a, b, c (`(a x + b) x + c`) | 1232-1245 | 0, 1, 0 |
| 3 | pad (MappingPolynomial.w) | unread | 6.9e-9 |
| 4 | InverseGamma.x (1/DisplayGamma) | unread | 0.428247 |
| 5 | InverseGamma.y (2.2/DisplayGamma), SDR / Rec709 / gamma paths | 1266 | 0.942144 (DisplayGamma 2.335: the brightness setting? [I]) |
| 6 | InverseGamma.z, device 2 exponent | 3101 | 1.0 |
| 7 | pad | unread | 1e-6 |
| 8-10, 12-14, 16-18 | ColorMatrixR/G/B .xyz (legacy) | 1139-1159 | identity |
| 11, 15, 19 | ColorCurve Cd1, Cd3Cm3, Cm2 (.w) | 1179-1182 | -0.11625, -0.0054, 1.03 |
| 20-23 | ColorCurve_Cm0Cd0_Cd2_Ch0Cm1_Ch3 | 1167-1196 | 0.0271, 0.1129, 0.9512, -0.9263 |
| 24, 25 | ColorCurve_Ch1_Ch2 .xy | 1201-1206 | 0.0257, -0.0244 |
| 26, 27 | pad | unread | 0 |
| 28-30 | ColorShadow_Luma | 1133-1136 | 6.80, 22.89, 2.31 |
| 31 | pad | unread | 0 |
| 32-34 | ColorShadow_Tint1 | 1150-1158 | 1, 1, 1 |
| 35 | pad | unread | 0 |
| 36-38 | ColorShadow_Tint2 .xyz | 1149-1157 | 0 |
| 39 | **film-path flag** (ColorShadow_Tint2.w slot; != 0 -> expand gamut + blue correction + FilmToneMap; 0 -> legacy curve). Integer test [V]; name: UE's "not mobile tonemapper" encoded in Tint2.a [I] | 407 | 0x3f800000 (1.0f) |
| 40-44 | FilmSlope, Toe, Shoulder, BlackClip, WhiteClip | 897-1001 | 0.88, 0.55, 0.26, 0, 0.04 |
| 45-47 | ColorScale | 1247-1253 | 1, 1, 1 |
| 48-51 | OverlayColor rgba | 1250-1259 | 0 |
| 52, 53 | WhiteTemp, WhiteTint | 183-217 | 6500, 0 |
| 54, 55 | unknown | unread | 0.000824, 0.005973 |
| 56-75 | global Saturation, Contrast, Gamma, Gain, Offset (float4 each) | 498-732 | neutral |
| 76-95 | Shadows (same five) | | neutral |
| 96-115 | Midtones | | neutral |
| 116-135 | Highlights | | neutral |
| 136 | ColorCorrectionShadowsMax | 558 | 0.09 |
| 137 | ColorCorrectionHighlightsMin | 563 | 0.5 |
| 138 | OutputDevice (uint) | 123 and switch | 3 |
| 139 | OutputGamut (uint) | 1292 | 2 |
| 140 | BlueCorrection (film path only) | 753, 2076 | 0.6 |
| 141 | ExpandGamut | 451 | 1.0 |
| 142, 143 | unused | unread | 0, 0xffffffff |

## B. Is the SDR path complete in this shader? [V]

Yes. Everything that produces the SDR colour runs for every texel regardless of the output device: expand gamut
(436-497, flag 39), ColorCorrectAll (498-733), blue correction + FilmToneMap + inverse blue + AP1->sRGB (743-1120,
flag 39), MappingPolynomial / ColorScale / Overlay / InverseGamma.y (1232-1272). The sRGB OETF (3270-3281) only sits
inside the device-0 branch but is two lines of maths. No textures exist in this permutation, so there is no blended
LUT to miss, and the SDR game uses this same hash. So `G_linear = (_2428, _2429, _2430)` (and `G_srgb` = the
3272-3280 formula on it) can be computed in a device-3 run from the same block.

Constants meaningful only for the SDR result: 40-44 (film), 140 (blue correction), 0-2 (polynomial), 45-51 (scale /
overlay), 5 (InverseGamma.y), 39 (flag), and the legacy 8-38 (only when 39 == 0). In the HDR capture they are all
filled with SDR-sensible values (film = UE defaults, scale 1, overlay 0, polynomial identity, InvGamma.y 0.942):
nothing is zeroed in HDR [V, one menu capture]. Device 3 itself ignores all of them. Open [I]: InverseGamma.y is
2.2/2.335, i.e. the game's brightness / display gamma; an SDR-run capture of the same block would confirm it holds the
same values in SDR (no SDR capture of buffer 0 exists).

Note: expand gamut (141) and ColorCorrectAll / white balance act in both paths; U already includes them.

## C. UI LUT builder fs c401ca35ab81eb39 [V]

- Inputs: buffer 0 dword 0 = OutputDevice, dword 1 = OutputGamut (only two dwords read); buffer 1 = the same
  immediate spline table; no textures / samplers; `gl_Layer` + barycentric UV as in a415.
- Per texel: `x = (32/31)uv - 0.5/31`, layer/31 -> ST2084 inverse x 10000 -> L (nits-domain linear, same convention as
  the scene LUT). Gamut matrix from dword 1 (same constants; here stored in row order `_358..._366`).
- Device 3/5 (103-990): `1.5 * L` -> sRGB->AP0 (0.43970 ..) -> RRT (glow, cubic-basis red modifier, clamp, desat, c5
  spline) -> ODT 1000 (c9) -> `- 3.507e-5` -> AP1->gamut -> x1e-4 -> PQ.
- Device 4/6 (1006-1790): same, `1.5 * L`, ODT 2000.
- Other devices: identity (writes the PQ coordinate back).
- Write: `0.952381 *` (/1.05), alpha 0 (1846-1854).
- No sRGB or 2.2 decode of the UI colour inside this builder. The composite (c401ca35f9bceb8e, line 137) does
  `PQ(UI.rgb * 1e-4)` straight from the UI texture sample, LUT, x1.05 (162-164), PQ -> nits, x cb[0]. Whether the UI
  value is sRGB-decoded first depends on the UI texture's format / view (hardware decode), not on shader maths [I].
  So the UI is: v -> ACES-1000 RRT+ODT of (1.5 v, sRGB primaries) -> PQ, i.e. the UI goes through the ACES tone
  scale and its hue / saturation modifiers (glow, red modifier, 0.96 desat) [V maths; I effect].

## D. RenoDX hook points vs this shader

| RenoDX (common.hlsl / lutbuilder_0x30AD540E) | a415 | match |
|---|---|---|
| input: `(uv - 0.5/32)*32/31`, PQ decode **x100** (LinearToNitsScaleInverse folded) | PQ decode **x10000**, tonemapper encodes PQ(scene*1e-4) | differs: keep MediEvil's x10000 [V] |
| bIsTemperatureWhiteBalance switch | absent (white balance only) | 4.21 |
| `SetUngradedAP1` after sRGB->AP1 | `_779/_782/_785` (430-434) | 1:1 |
| expand gamut (always) | gated by dword 39 | differs |
| `SetUntonemappedAP1` after ColorCorrectAll | `_1524/_1534/_1556` (710/718/733) | 1:1 |
| `bUseMobileTonemapper != 0` -> legacy | dword 39 **== 0** -> legacy (inverse sense, float 1.0 in the Tint2.w slot) | differs in encoding |
| blue corr, FilmToneMap, **ToneCurveAmount lerp**, inverse blue, AP1->sRGB, `SetTonemappedBT709` | same, no ToneCurveAmount; pre-max `_2102/_2104/_2106` (1113-1117); legacy `_2324/_2326/_2328` (1216-1220) | 1:1 minus ToneCurveAmount |
| saturate + sRGB encode + LUT texture blend (LUTWeights) + sRGB decode | absent (no textures; FilmColor only max(0)) | differs: G not clamped to 1 here |
| MappingPolynomial, ColorScale/Overlay, InverseGamma.y -> `GenerateOutput(r3, OutputDevice)` then return | 1232-1272 -> `_2428/_2429/_2430`; hook before 1281 | 1:1 |
| devices 0-9 (8/9 linear scRGB) | 0-7 (5/6 = 3/4 maths) | 4.21 |
| `GenerateOutput`: PQ BT.2020 (scaling 1 or diffuse white), `/1.05`, alpha 1 | /1.05 needed (tonemapper x1.05), alpha 0 (unused: tonemapper reads .xyz) | 1:1 |

marat's ue-extended injects at the same untonemapped-AP1 point, recomputes the filmic curve itself
(`ApplyFilmicToneMap` with the cb film values), then MappingPolynomial -> ColorScale/Overlay (same order as 1232-1265),
forces ExpandGamut 0 / BlueCorrection 0 when missing / ToneCurveAmount 1 [V, SKILL.md + lutbuilderoutput.hlsli].

Other notes:
- The LUT texel order is RGB; the HDR tonemapper writes `out.x = LUT.z*1.05` (BGR swizzle for the swap chain) [V
  code; I reason].
- The scene LUT stores PQ in what looks like a 10-bit UNORM target (the UI LUT is `A2B10G10R10` [V study H9]; the scene
  LUT's format not checked here); RenoDX upgrades to RGBA16F, not needed for PQ output [I].

## E. The builder with a level grading LUT, fs 9f6bac6cba984956 [V] (2026-11-21, study-hdr.md H36)

Hilltop Mausoleum's grading draw (capture `research-data/hdr-grading-20261121/cap_frame82572.rdc`, eid 15063;
decompile `research-data/hdr-grading-20261121/port9f6b/lut_9f6b.glsl`). UE 4.21's BLENDCOUNT=2 permutation of a415.

- binding 0: the parameter block, 164 dwords (656 bytes) = a415's 144 with 20 dwords inserted at 45: a415 dword d is
  9f6b dword d for d <= 44, d + 20 from 45 on (OutputDevice 138 -> 158). The inserted 45-64 are LUTWeights (float4s);
  read are [0] at 48 and [1] at 52 (captured 0.65 / 0.35). binding 1: the same 448-byte ACES table.
- image 0: the level LUT, a 256x16 RGBA8 texture (16^3: 16 slices of 16x16 side by side), view swizzle `bgra`;
  sampler linear / linear / point mip, clamp to edge.
- Stage, after the film curve (legacy or filmic) and before MappingPolynomial (lines ~1220-1320): clamp 0-1, sRGB
  encode (`>= 0.00313067`), u = (r 0.9375 + floor(15 b)) / 16 + 1/512, v = 0.9375 g + 1/32; two samples (u and
  u + 1/16) lerped by fract(15 b); `w1 * lut + w0 * encoded`; max 6.1035e-05; sRGB decode (`0.04045 <` x).
- Devices >= 1 (the HDR ODTs) read the colour from before the film curve (`_1572` / `_1575` / `_1583`, line 1358 on):
  the level LUT appears only in SDR (device 0) output.
- The port: `ue4_grade.glsl` `GradeLutBlend` under `GRADE_LUT_BLEND`, our block in a415's layout with the weights at
  142 / 143 (`Ue4HdrGrading::RemapBlendBlock`); equal to the game's shader on all 32^3 texels offline (12 cases).
