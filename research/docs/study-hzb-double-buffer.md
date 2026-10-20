# Study: double-buffered HZB occlusion results

Status: study, 2026-10-20. Read-only reverse engineering, nothing built or run. Parent: [START-HERE.md](START-HERE.md).
Background: [plan-stage-b-split.md](plan-stage-b-split.md) T-HZ1 / T-HZ2b / T-HZ3, [plan-frame-pacing.md](plan-frame-pacing.md)
O2 / O3. Evidence: Ghidra 12.1.4 headless on a copy of the project (`research-data\ghidra\proj-hzb`), scripts and raw
outputs in `research-data\ghidra\hzb\` (`run.sh <out> <Script.java> args`; `FindStr`, `RipRefs`, `DecompSite`, `Dis`,
`Pat`, `Vtabs`, `PtrRefs`, `Qw`). **Addresses are eboot image offsets** (the port's `guest+0x...`; Ghidra shows them at
+0x1000000, `patches/MediEvil.xml` at +0x400000).

## Summary

- Everything UE4's `FHZBOcclusionTester` does is in real (not inlined) functions at 0x1533e70-0x1535300, each with
  one caller in the visibility code (table below). The tester lives at **FSceneViewState+0x170**.
- The consistency problem is real and is the T-HZ1 bug: each frame every tested primitive gets a **new** index
  (`AddBounds` appends; `Submit` empties the list) and its occlusion history keeps **one** index (+0x34) and **one**
  test frame (+0x2c). Frame N+1 reads results with frame N's indices. Serving an older texture (N-1) without remapping is
  exactly what `stale_readback=1` did in T-HZ1 (random culling).
- When the results are not valid for a primitive (`IsValidFrame` false), the game treats it as **visible, not
  definite** (verified at 0x15d84d1-0x15d84eb). An "invalid but not waiting" variant is therefore safe but, with our GPU
  always ~1 frame behind, would mean no HZB culling at all: useless alone, useful as the fallback.
- **Recommended:** one host hook after `MapResults` (single call site 0x158c4ac) plus a texture-cache export: take the
  newest *published* copy of the results (age known from the readback queue, no wait), and **remap** it by primitive key
  (`PrimitiveId`, `CustomIndex`) from the frame that produced it to frame N's indices, into a host buffer the game's
  `IsVisible` reads. Unmatched primitives read visible. A pure `MediEvil.xml` byte patch cannot do this (needs a hash
  remap and the port's readback state).
- Expected: the T-HZ3 culling (~2,250-2,300 draws, camera still) without its ~1.7 ms wait: **~150-165 FPS** heavy view
  (T-HZ1 no wait 169-171, T-HZ3 ~125, queries off ~130-139). Latency 2 frames instead of 1 (same as T-O2c / T-O3c's
  `r.NumBufferedOcclusionQueries=2`, where the user saw no pop-in). Confidence: locations high; design medium-high;
  FPS estimate medium.

## Locations (CUSA11227 01.00)

### FHZBOcclusionTester (FRenderResource, 0x50 bytes, at FSceneViewState+0x170)

| Offset | Field | Evidence |
|---|---|---|
| +0x00 | vtable (0x4f61d60+0x10) | ctor 0x1533e70 |
| +0x08 | FeatureLevel (int, 4 = Num -> GMaxRHIFeatureLevel 0x5605d50) | ctor, InitDynamicRHI |
| +0x28 / +0x30 / +0x34 | `Primitives` TArray<FOcclusionPrimitive>: data / Num / Max; element 0x18 = Center (3 floats), Extent (3 floats) | AddBounds 0x1534030, Submit |
| +0x38 | `ResultsTextureCPU` TRefCountPtr<IPooledRenderTarget>; `[+0x38]+0x10` = ShaderResourceTexture | InitDynamicRHI, MapResults |
| +0x40 | `ResultsBuffer` (const uint8*) | MapResults / UnmapResults / IsVisible |
| +0x48 | `ValidFrameNumber` (0xffffffff = invalid; mask 0x7fffffff) | 0x1533eb0-0x1533ef0 |

| Function | Address | Caller(s) | Notes |
|---|---|---|---|
| ctor | 0x1533e70 | | zeroes +0x10..+0x48, ValidFrameNumber = -1 |
| SetInvalidFrameNumber | 0x1533eb0 | | `mov [rdi+0x48], -1` |
| IsValidFrame(n) | 0x1533ec0 | 0x158d592, 0x15d84d1 | `(n & 0x7fffffff) == [rdi+0x48]` |
| SetValidFrameNumber(n) | 0x1533ee0 | 0x158edb1 | `[rdi+0x48] = n & 0x7fffffff` |
| IsInvalidFrame | 0x1533ef0 | | |
| InitDynamicRHI | 0x1533f00 | vtable 0x4f61d80 | `GRenderTargetPool(0x5bbf8d0).FindFreeElement(0x144f9c0)` 256x256, format 2 (PF_B8G8R8A8), name "HZBResultsCPU" (0x3d4db7a) into +0x38 |
| ReleaseDynamicRHI | 0x1533ff0 | | FreeUnusedResource(+0x38) (0x1454510) |
| AddBounds(center, extent) | 0x1534030 | 0x158d983 (single-threaded fetch), 0x158e4bb (gather of parallel fetch packets) | `Index = Num++` (no bound check), returns Index |
| MapResults(RHICmdList) | 0x15340a0 | **0x158c4ac only** | below |
| UnmapResults(RHICmdList) | 0x15341d0 | 0x158ed95 | flush, `GDynamicRHI->vtbl+0x308` (empty on Gnm, 0x68c3d0), ResultsBuffer = 0 |
| IsVisible(index) | 0x1534260 | 0x158d5a7, 0x15d84e6 | `ResultsBuffer[4*(x + 256*y)] != 0`, 8x8 blocks, 32 blocks a row: x = (i&7) \| ((i>>3)&0xf8), y = ((i>>3)&7) \| ((i>>8)&~7) |
| Submit(RHICmdList, View) | 0x1534340 | 0x11573ec (render, per view, only if `ViewState && [ViewState+0x1a0] != 0` i.e. Primitives.Num) | below |

**MapResults 0x15340a0** (decompiled/disassembled, `dis-3.txt`):
```
if ([this+0x48] != -1) {
    t0 = Cycles();                                   // 0x3bf0680
    tex = [[this+0x38]+0x10];
    RHICmdList flush (0xf7ca70 / 0xf7f650 / 0xf7fb40, ImmediateFlush 0xf7f460(cmd, 1) = FlushRHIThread);
    GDynamicRHI(0x5b2e758)->vtbl[0x300](tex, &[this+0x40], &w, &h);   // RHIMapStagingSurface
    GRenderThreadIdle[WaitingForGPUQuery] (0x5b3c498+4) += Cycles() - t0;  GRenderThreadNumIdle (0x5b3c4a4+4)++
}
if ([this+0x40] == 0) { [this+0x40] = FirstFrameBuffer (0x5606ac8, {0xff}); [this+0x48] = -1; }
```

**Gnm RHIMapStagingSurface 0x68c2e0** (slot 0x300 of the Gnm RHI vtable 0x4ed67b8, found through slot 0x328 =
0x689ed0 RHIGetRenderQueryResult; `dis-9.txt`): `fence = gnmTex+0x140`; unless the copy was issued in the current Gnm
frame (`GGnmManager(0x58053c0)+0x2d8 == gnmTex+0x148`), spins `while (*fence != 0) FPlatformProcess::Sleep(0)`
(0x8d7b20); then returns width/height from the `sce::Gnm::Texture` and `OutData = getBaseAddress() << 8` (no copy:
the CPU reads the texture memory itself, **0x20010c0000** in T-HZ1's log). So the guest waits for the copy's
**label**, and the port writes labels after `ProcessDownloadImages` (liverpool.cpp `ReleaseMem`, `RunEventWriteEop`,
EOS): when the guest's map returns, frame N's copy is already queued as a readback; the T-HZ3 wait is the later CPU
read of the page (`ReadMemory` → `DrainDrawPipe` + `ResolveReadbacks` → `scheduler.Wait`, texture_cache.cpp:183).

**Submit 0x1534340** (`dec-2.txt`, `dis-10.txt`): pooled "HZBBoundsCenter" / "HZBBoundsExtent" (A32B32G32R32F) and
"HZBResultsGPU" targets; uploads Primitives in 64-entry 8x8 blocks with `GDynamicRHI->vtbl+0x530`
(RHIUpdateTexture2D; extent.w = 1.0, unused block entries zero); `Primitives.Empty()` (Num = 0, free 0x157e6b0); test
pass `DrawRectangle` 0x13f5040 256x256; `GVisualizeTexture.SetCheckPoint` 0x145aa30; then an
FRHICommandCopyToResolveTarget (vtable 0x4ea3a48) from ResultsGPU to `[[this+0x38]+0x10]`. One copy per Submit.

### Visibility side

| What | Where |
|---|---|
| r.HZBOcclusion (`GHZBOcclusion` int) | **0x5bc8ec8** (registered at 0x160d678); read once at 0x158c6ab (with a shader-platform check, `platform-0x19 < 2` excluded) → byte `[rsp+0xc8]` = bHZBOcclusion |
| `ICVar r.HZBOcclusion` cached by the renderer | 0x5b598d0 (0x11574f8) |
| OcclusionFrameCounter++ | `inc [ViewState+0x24c]` at 0x158a332 (in 0x1589e30, PreVisibilityFrameSetup-like, called from 0x15945f1) |
| Occlusion cull (Map / fetch / AddBounds / Unmap / SetValid) | 0x158c2e0 (called from 0x1594612): Map 0x158c4ac, single-threaded fetch 0x158d4c0-0x158d9a5, gather + AddBounds 0x158e480-0x158e4d6, Unmap 0x158ed95, `if (bSubmitQueries [rsp+0x27]) SetValidFrameNumber([ViewState+0x24c])` 0x158edb1 |
| Parallel fetch packet | 0x15d7760 (FetchVisibilityForPrimitives_Range-like), HZB lookup 0x15d84c6-0x15d84eb |
| HZB build + Submit per view | 0x1157240: per view (FViewInfo stride 0x28a0) 0x1463790, 0x10de180, 0x1535740 (BuildHZB), then Submit(ViewState+0x170) |

FSceneViewState: +0x170 HZBOcclusionTests; +0x24c OcclusionFrameCounter; +0x1360.. PrimitiveOcclusionHistorySet
(TSet: +0x1360 element data, +0x1368 Num, +0x1394 NumFreeIndices, +0x1398 inline hash, +0x13a0 hash pointer (0 →
inline), +0x13a8 HashSize; bucket = hash[(HashSize-1) & (keyhash)], chain via element+0x50, -1 ends; walk as at
0x158d4ca-0x158d53e). FViewInfo: +0x600 PreViewTranslation (the HZB bounds are translated: center = origin + this),
+0x60c ViewOrigin, +0x1460 ViewState.

FPrimitiveOcclusionHistory (TSet element 0x58): +0x00 PrimitiveId, +0x08.. pending queries, +0x20.. their frame
numbers, **+0x2c LastTestFrameNumber**, +0x30 LastConsideredFrameNumber, **+0x34 HZBTestIndex**, +0x38
LastProvenVisibleTime, +0x3c LastConsideredTime, +0x40 LastPixelsPercentage, +0x44 CustomIndex (key with +0x00),
+0x48 flags (bit 7 OcclusionStateWasDefiniteLastFrame, bit 6 WasOccludedLastFrame), +0x4c grouped-query flag, +0x50
HashNextId, +0x54 HashIndex.

### How index, frame number and results tie together (frame N)

1. 0x158a332: `OcclusionFrameCounter = N`.
2. 0x158c4ac: `MapResults`: the texture should hold frame N-1's results; `ValidFrameNumber` = N-1 (set in step 5 of
   frame N-1).
3. Fetch, per primitive with a history (`dec-4.txt` lines ~330-360):
   ```
   if (bHZBOcclusion) {
       if (Tests.IsValidFrame(H->LastTestFrameNumber))            // +0x2c == N-1
           { occluded = !Tests.IsVisible(H->HZBTestIndex); definite = 1; }   // +0x34, an index into N-1's list
       else
           { occluded = 0; definite = 0; }                         // visible
   }
   ```
4. Re-test (every frame unless the box is near the camera, which is visible + definite without a test, 0x15d8a80
   in the decompile): `H->LastTestFrameNumber = N` (0x158d95f / 0x15d89bd), `H->HZBTestIndex = AddBounds(...)`
   (0x158d98f / 0x158e4d0): **both overwritten every frame**; `H->flags = definite<<7 | occluded<<6`.
5. `UnmapResults`; `SetValidFrameNumber(N)`.
6. Later (0x11573ec): `Submit`: frame N's list → test → copy into the one results texture; `Primitives.Empty()`.

Results frame N-2 cannot be indexed with what the histories hold at frame N (indices and test frame are N-1's), and
the list is rebuilt every frame in fetch order (primitive order, or packet order for the parallel fetch), so indices
shift as soon as the set of tested primitives changes (camera motion, LOD, streaming).

## What IsVisible does when results are not valid (deliverable 3)

`IsValidFrame` false → `occluded = 0, definite = 0`: **drawn**. Also `MapResults` with a NULL map → `FirstFrameBuffer
{0xff}` + invalid → everything drawn. So "invalid instead of waiting" can never hide anything (safe), but it removes
the culling for that frame. Our GPU is about one frame behind at every map (T-HZ3: ~1 wait a frame), so a pure
"invalid when not ready" variant ≈ queries off plus the HZB build/test cost: no gain. It is the right fallback inside
the double buffer (results too old or missing).

## Options

### (a) Binary patch (`patches/MediEvil.xml` style): rejected

The XML patches are fixed byte replacements applied at load (the "4K display metrics at boot" patch nops two `je`).
A correct double buffer needs, per history, the index of the frame whose results are read (the struct has one, +0x34),
or a remap by primitive identity: a hash table, extra memory and the readback queue's state. Two tester instances
alternated per frame do not help on their own: the histories still hold only the latest index. No useful byte-only
variant was found (skipping the fence spin at 0x68c31f gives T-HZ1's wrong culling; "test every other frame" leaves
the odd frames unculled, alternating draw counts).

### (b) Host hook + remap: recommended

The runtime already patches guest call sites into host stubs (`src/orb_hook.h`: an `E8` call replaced with a stub
that calls a `sysv_abi` host helper, signature-checked bytes first). Guest code runs natively, so the guest can read a
host buffer.

1. **Hook** the single `MapResults` call at 0x158c4ac (`e8` rel32 to 0x15340a0; rdi = ViewState+0x170, rsi =
   RHICmdList; caller restores rdx from rbx after the call, so caller-saved registers are free). The stub calls the
   original, then `hzb_after_map(tester)`.
2. **Port export** (texture cache, under `readback_mutex`): `int gow3_readback_snapshot(VAddr addr, u64 size, void *dst)`:
   `RetireReadbacks()` (non-blocking, publishes copies the GPU has finished), copy the bytes from guest backing (no
   fault, works with `stale_readback` 0 or 1), return the number of readbacks still pending over the range = **age**
   (0: frame N's results, 1: N-1's, ...). This is exact because the guest's map only returns after frame N's copy
   label, which the port writes after queueing that copy (above).
3. **`hzb_after_map`** (render thread, before the fetch touches the histories): if `[t+0x48] == -1` return. N =
   `[t+0x48]`. Walk the history TSet (ViewState+0x1360..0x13a8, hash chains) and build `map_N`: key (PrimitiveId,
   CustomIndex) → HZBTestIndex for every history with `(LastTestFrameNumber & 0x7fffffff) == N` (exactly the ones the
   fetch will look up). Keep the last 4 maps per tester (ring by N). Snapshot the result rows actually used (max index
   → rows; ~16 KB for 4,096 entries, 256 KB worst case) with the export; source frame = the age-th older frame that had a
   non-empty map (no Submit when the list was empty). Write `remap[4*pix(i)] = src[4*pix(map_src[key])]` for every
   (key, i) in `map_N`, `0xff` when the key is absent or the source map is gone (age > 3). Set `[t+0x40] = remap`
   (host buffer, 256 KB per tester). `UnmapResults` is unaffected (Gnm unmap is empty; it clears +0x40).
4. Per tester state keyed by the tester address (scene captures with a ViewState have their own). Reset the ring when
   `[t+0x38]` changes (RHI re-init on a resolution change) or N jumps.

Why not match by bounds instead of keys: centers include PreViewTranslation (View+0x600), so they change whenever
the camera moves; exact matching would fail exactly while panning (a PVT-corrected tolerant match is possible from a
`Submit` hook, but the key walk needs one hook and is exact). Why not hook `AddBounds`: it gets no history; its two
call sites (0x158d983: history in r13; 0x158e4bb: history at `[rsi-8]`, entry layout +0 history, +8 center, +0x14
extent) could record keys instead of the TSet walk, at the cost of two more stubs.

Cost: TSet walk + hash map over a few thousand histories, ~0.1-0.3 ms on the render thread (estimate), against the
~1.7 ms a frame T-HZ3 waited. The guest's own label spin in `RHIMapStagingSurface` stays (it was small in T-HZ1:
170 FPS).

### (b') Variants considered

- **Two results textures alternated by the hook** (a shadow tester calling InitDynamicRHI 0x1533f00 for a second
  pooled target, swapping `[t+0x38]` per frame): removes the age query but still needs the remap and adds guest RHI
  object handling (re-init, release). No advantage over the export.
- **Serve the older copy without remap**: T-HZ1 (wrong culling). No.

## Correctness risks

| Risk | Assessment |
|---|---|
| Wrong culling from a wrong age | The age comes from the queue under its lock after `RetireReadbacks`; the guest map has already waited for frame N's copy label. Guard: if the export reports more pending copies than frames in the ring → all visible. Log the age histogram (expect mostly 1). |
| Pop-in (disocclusion) | Results 2 frames old instead of 1 (age 1). At ~160 FPS that is ~12 ms, less than one PS4 frame. T-O2c / T-O3c (HW queries, 2 frames) showed no pop-in to the user. UE4's margins stay: r.ExpandAllOcclusionTestedBBoxesAmount, r.ExpandNewlyOcclusionTestedBBoxesAmount / r.FramesToExpandNewlyOcclusionTestedBBoxes (in the binary) can be raised if needed. |
| New / moving primitives | New in view: no key in the source map → visible (as UE4's first frame). Moving (Dan, enemies): key matches, result is for the box two frames ago: same kind of lag as UE4's own one frame. |
| TSet layout | Walk = the game's own lookup (0x158d4ca); keys are the set's keys. Signature-check the hook site and the offsets used (0x158c4ac bytes, 0x15340a0 prologue, 0x158d4df-0x158d53e loads). |
| Camera cuts / disabled submissions | Unchanged game logic (no SetValidFrameNumber when `[rsp+0x27]` is 0; histories then fail IsValidFrame → visible). |
| > 65,536 tested primitives | AddBounds has no check in this build; unchanged by the hook. |

## Expected performance

| Run | Heavy view | Draws/frame | Wait |
|---|---|---|---|
| Queries off (default) | ~130-139 FPS, p99 ~8.5-9.5 ms | ~2,635-2,800 | - |
| T-HZ1 HZB stale (wrong culling) | 169-171 FPS, p99 6.9-7.1 ms | ~2,200 | none |
| T-HZ3 HZB precise + R1 | ~125 FPS still, p99 8.5 ms; panning worse | ~2,290 | ~1.7 ms/frame |
| **HZB double-buffered (estimate)** | **~150-165 FPS still** | ~2,250-2,350 | none (age > ring: rare) |

Stage B is the limit (~350-370k draws/s), so FPS follows draws: 2,300 draws ≈ 155-160 FPS. HZB culls less than real
queries (T-O3c: ~1,450 draws, 63-73% hidden; HZB ~17% fewer draws than none) because it is conservative, but it adds no
per-query draws, so panning should cost less than O2/O3 did.

## Test (needs the user's go; one change)

Build with the hook behind `GOW3_HZB_REMAP=1` (default off), `GOW3_UE4_CVARS='r.AllowOcclusionQueries=1;r.HZBOcclusion=1'`,
`stale_readback` as default (1). Stats line every 5 s: maps/s, age histogram (0/1/2/3/>3), keys remapped / unmatched /
total, remap time max/avg, `IsValidFrame` frames. Heavy view still and panning, then the far views: FPS, p99, draws per
frame against T-HZ3 / queries off; the user watches for Dan, ground or enemies vanishing (the T-HZ1 symptom) and for
pop-in. A/B in the same launch: toggle the remap off live (falls back to the stale T-HZ1 behaviour) only if a toggle
bit is cheap; otherwise a second launch. Expect age 1 nearly always; if age 0 is common the GPU is keeping up and the
remap is a copy.

## Open

- Whether the parallel fetch (0x15d7760) or the single-threaded one (0x158d4c0) runs in MediEvil (both are covered by
  the TSet walk).
- The cost of the guest's label spin in `RHIMapStagingSurface` with the hook (it remains; measure with
  `GOW3_WAIT_SITES=1`, Sleep(0) at 0x8d7b20 from 0x68c334).
