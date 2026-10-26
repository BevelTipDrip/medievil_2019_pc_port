# Study: booting MediEvil at any output resolution

Status: study, 2026-10-25. Read-only reverse engineering, nothing built or run. Parent: [START-HERE.md](START-HERE.md).
Background: [plan-baseline-issues.md](plan-baseline-issues.md) I1 (live `r.SetRes` breaks menu navigation; the 4K boot
fixed it), `patches/MediEvil.xml` ("Native 4K framebuffer pool (512 MB)", "4K display metrics at boot"), `run.py`
~147-175, `src/runtime_extra.c` ~540-625. Evidence: capstone over the 01.00 image (`research-data\ghidra\stageb\py\eimg.py`
loader) and Ghidra 12.1.4 headless **read-only** (`-readOnly`) on `research-data\ghidra\proj-hzb`. Scripts and raw
outputs: `research-data\ghidra\bootres\` (`edis.py <addr> [end]` disassembly with import names and rip targets,
`callers.py <import|addr>`, `riprefs.exe image.bin <target>` (all rip-relative references, built from `riprefs.c`),
`slate190.py`, `run.sh <out> <Script.java> args`). The game configs were read from the paks with `scripts/ue4_override.py`'s
`read_file`. **Addresses are eboot image offsets** (`patches/MediEvil.xml` = image + 0x400000; Ghidra = image +
0x1000000). **[V]** means verified in the 01.00 binary or configs (static reading), **[I]** means inferred (UE 4.21
knowledge or reasoning), **not run in game** in either case.

## Summary

> **Result (2026-10-25): verified at 2560x1440** (T-R1 / T-R2 in [plan-stage-b-split.md](plan-stage-b-split.md)): the
> game renders at 2560x1440 from boot and menu navigation works. Built as fixed presets (option (c)): "Boot display
> size" 1280x720 / 2560x1440 / 3200x1800, selected by `run.py` for the `*_native` resolutions.

- **Yes, the game can boot at any WxH, and it takes two 8-byte writes.** One cached rectangle decides the boot size:
  the PS4 display metrics (0xb68fd0) and the work area (0xb6d640) copy a static `{0,0,1920,1080}` from rodata
  (**0x3cbe750 / 0x3cbe7b0**), or the hard-coded `3840x2160` when the 4K branch runs. `UGameEngine::DetermineGameWindowResolution`
  (0x2082650) then **always** sets the game window to the work-area size (the console "fixed resolution" override).
  The ini's `r.setres`, `ResX=`, `-ForceRes` have no effect on that size. **[V]**
- **The 4K branch is hard-coded:** `movabs rax, 0x87000000f00` (3840x2160) at 0xb69072 / 0xb6d6c9. VideoOut only feeds
  a yes/no (`fullHeight >= 1081`). A 2560x1440 VideoOut gives **1920x1080** without the 4K patch and **3840x2160**
  with it, never 1440p. **[V]**
- **Generic patch:** write `W, H` (two little-endian int32) over the rodata defaults, at XML 0x040be758 and 0x040be7b8.
  This needs no `je` patch and no VideoOut query, and does not depend on the `r.PS4Allow4kOutput` cvar. With it, the
  window, viewport, backbuffers (VideoOut buffer attributes come from the backbuffer texture), Slate geometry and UMG DPI
  all start at WxH, the state the user verified at 4K. **[V]** for the data flow, **[I]** that menus behave as in the
  4K boot.
- UI scale follows the engine default curve (ShortestSide, 1080 → 1.0, linear to 8640 → 8.0). The game does not
  override it. At 16:9 the UI therefore stays the same fraction of the screen at any size (DPI = H/1080). **[V]** config, **[I]**
  effect.
- **Recommendation: (a) the generic boot patch** (low risk), with (c) (the four presets) as its first use. (b), fixing
  the live `r.SetRes`, is high effort with an unknown outcome. First experiment: one launch at 2560x1440 through the rodata
  patch (end of this page).

## 1. How the boot size is decided (01.00)

### The cache and the two readers

| What | Address | Evidence |
|---|---|---|
| `FDisplayMetrics::GetDisplayMetrics`-like (PS4) | 0xb68fd0 | 8 call sites (table below) **[V]** |
| Work area (returns `FPlatformRect` in rax:rdx) | 0xb6d640 | no direct calls (virtual) **[V]** |
| Default rect `{0,0,1920,1080}` (metrics) | rodata **0x3cbe750** | `vmovups xmm0,[rip]` at 0xb69018, its only reference **[V]** |
| Default rect `{0,0,1920,1080}` (work area) | rodata **0x3cbe7b0** | `vmovups` at 0xb6d67a, its only reference **[V]** |
| Cached rect `{L,T,R,B}` | bss 0x5b1d2cc | referenced only in the two functions **[V]** |
| "computed" flag (byte) | bss 0x5b1d2c8 | same **[V]** |
| `r.PS4Allow4kOutput` (int, `ECVF_ReadOnly` = 4) | bss 0x5b1d1d8 | read only by `cmp` 0xb69027 / 0xb6d689. Registered at 0xb751cb (`RegisterConsoleVariableRef`, the name string's only reference) **[V]** |

0xb68fd0, reconstructed from the disassembly (0xb6d640 is the same up to the copy, then returns the rect):

```c
if (!computed /*0x5b1d2c8*/) {
    cached = *(FPlatformRect*)0x3cbe750;              // {0,0,1920,1080}
    if (r_PS4Allow4kOutput /*0x5b1d1d8*/ != 0) {      // je 0xb69032 (the 4K boot patch NOPs it)
        h = sceVideoOutOpen(0xff, 0, 0, 0);
        if (h > 0) {
            if (sceVideoOutGetResolutionStatus(h, &st) == 0 && st.fullHeight >= 0x439 /*1081*/)
                cached.Right = 3840, cached.Bottom = 2160;    // movabs rax,0x87000000f00 (0xb69072)
            sceVideoOutClose(h);
        }
    }
    computed = 1;
}
out->PrimaryDisplayWorkAreaRect = cached;  // +0x18
out->VirtualDisplayRect         = cached;  // +0x28
out->PrimaryDisplayWidth  = cached.Right;  // +0   (Left assumed 0)
out->PrimaryDisplayHeight = cached.Bottom; // +4
sceSystemServiceGetDisplaySafeAreaInfo(&sa);      // title-safe padding +0x40..+0x5c = (1-ratio)*0.5*W/H
```

The cache is computed once and nothing else writes it **[V]**. Nothing outside these two functions uses the cvar, and
no code looks it up by name (the only reference to its name string is the registration) **[V]**.

### Who calls VideoOut for a size

| Function | VideoOut size use | Gate |
|---|---|---|
| 0xb68fd0, 0xb6d640 | `fullHeight >= 1081` → 3840x2160 | `r.PS4Allow4kOutput != 0` |
| 0x516eb0 (default device profile name: "PS4", "PS4_Neo", "PS4_Neo_4k") | `fullHeight >= 1081` → "PS4_Neo_4k" | `sceKernelIsNeoMode()` (port: 0) |
| 0x685e30 (main port open) | none (open, vblank/flip events, device capability) | |

**[V]** These are the only `sceVideoOutGetResolutionStatus` call sites (3). On the port (Neo mode 0, cvar 0), **the game
never reads the VideoOut resolution**. `GOW3_INTERNAL_WIDTH/HEIGHT` is used only by the VideoOut driver
(`video_out.cpp:476`) **[V]**. So upstream 1.3.3's "report the output size in VideoOut" cannot change the display metrics
on its own **[V]**. Whether it fixes their menus is still open (plan-upstream-merge.md).

`PS4DeviceProfiles.ini`: only `[PS4_Neo_4k]` sets `+CVars=r.PS4Allow4kOutput=1`. `[PS4]` sets the streaming pool and
volumetric fog. There is no resolution cvar. **[V]**

### From metrics to window, viewport and backbuffers

| Step | Address | What it does |
|---|---|---|
| `UGameEngine::DetermineGameWindowResolution` | 0x2082650 (strings "ResX=", "ResY=", "ForceRes", "Portrait") | Windowed modes (1, 2) become Fullscreen (0). Clamps the requested size to the display. **Then unconditionally** `ResX = WorkArea.Right - Left`, `ResY = Bottom - Top` (0x208288f-0x20828a7). **[V]** This is UE4's `HasFixedResolution()` console override **[I]**. So the shipped `r.setres=1920x1080` (`Engine/Config/PS4/PS4Engine.ini`) and the override pak's `r.setres=WxH` are dead at boot **[V]** |
| Metrics through Slate | `[0x5b29af0]` vtable+0x190 (else 0xb68fd0) | 4 such call sites in game code: 0x2082650 (above), 0x2082910 (game window creation: "WinX=", "WinY=", "MinWindowHeight"; metrics only when windowed), 0x20b87c0 (`GetDesktopResolution`-like), 0x21a7a90 (supported fullscreen resolutions list via 0x20805a0) **[V]** call sites, **[I]** names |
| Other metrics users | 0xb68e70 / 0xb6e3d0 (bounds rect, defaults to the display), 0xb767a0 (platform `GetInitialDisplayMetrics`), 0xb6b0a0 (safe area) | **[V]** call sites |
| VideoOut buffers | 0x686a60 (first set), 0x687630 (re-register on a mode change) | `sceVideoOutSetBufferAttribute` width/height = the backbuffer texture's packed size (`[tex+0x3c]`, low 16 = W, high 16 = H). Two buffers **[V]**. The backbuffer follows the viewport, the viewport the window **[I]** (the live 1440p/1800p/4K paths already register those sizes) |
| UMG DPI | `[/Script/Engine.UserInterfaceSettings]` | BaseEngine: `UIScaleRule=ShortestSide`, curve keys (480, 0.444), (720, 0.666), (1080, 1.0), (8640, 8.0). The game's DefaultEngine sets only `RenderFocusRule=Always` **[V]** |
| Slate hit-test grid | sized from the SWindow / viewport geometry | **[I]** (mechanism of I1, unchanged) |

The port's presenter letterboxes by the guest frame's aspect (`vk_presenter.cpp:111-120`), so any guest size is shown
correctly in the PC window **[V]**.

## 2. VideoOut at 2560x1440: what the game would do

| VideoOut (`GOW3_INTERNAL_*`) | Patches | Display metrics / boot size | Note |
|---|---|---|---|
| 1920x1080 (default) | none | 1920x1080 | today's `native` |
| any | none | **1920x1080** | VideoOut is not queried (cvar 0) |
| 1280x720 | "4K display metrics at boot" | 1920x1080 | height < 1081: default kept |
| 2560x1440 | "4K display metrics at boot" | **3840x2160** | hard-coded constant: boots 4K, not 1440p |
| 3840x2160 | "4K display metrics at boot" | 3840x2160 | today's 4K boot (verified by the user) |
| any | **rodata WxH** (below) | **WxH** | VideoOut not queried. Report WxH anyway, for consistency |

For the 4K path to give WxH, three things would need patching: the `movabs` immediates (0xb69074 / 0xb6d6cb, 8 bytes
each), the two `je`, and the VideoOut height (or the `cmp ..., 0x439` at 0xb6905f / 0xb6d6c0) for H ≤ 1080. That is
six writes plus a VideoOut dependency, for the same result as two rodata writes. **Use the rodata.**

## 3. The patch set

### Display size (generic)

Two writes, value = `W` then `H` as little-endian int32 over the default rect's Right/Bottom. Format as in
`MediEvil.xml`. Writing into this segment is already done by the pool patch (capacity at 0x04088218), and
`scripts/patches.py` accepts it (inside a PT_LOAD).

```xml
<Metadata Title="MediEvil" Name="Boot display size 2560x1440"
          Note="Starts the game at 2560x1440 (display metrics and work area); no live r.SetRes."
          Author="medievil_pc" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="false">
    <PatchList>
        <!-- display metrics default rect {0,0,1920,1080} (rodata 0x3cbe750): Right, Bottom -->
        <Line Type="bytes" Address="0x040be758" Value="000a0000a0050000"/>
        <!-- work area default rect (rodata 0x3cbe7b0): Right, Bottom -->
        <Line Type="bytes" Address="0x040be7b8" Value="000a0000a0050000"/>
    </PatchList>
</Metadata>
```

| Size | `Value` | Size | `Value` |
|---|---|---|---|
| 1280x720 | `00050000d0020000` | 2560x1080 (21:9) | `000a000038040000` |
| 1920x1080 (stock) | `8007000038040000` | 3440x1440 (21:9) | `700d0000a0050000` |
| 2560x1440 | `000a0000a0050000` | 2560x1600 (16:10) | `000a000040060000` |
| 3200x1800 | `800c000008070000` | 5120x2880 | `00140000400b0000` |
| 3840x2160 | `000f000070080000` | | |

- **Do not combine with "4K display metrics at boot"** while VideoOut reports more than 1080 lines: the 4K branch would
  overwrite the rect with 3840x2160. The rodata patch at 3840x2160 can replace the `je` patch. The 4K boot then also no
  longer opens a second VideoOut handle (the open question in I1).
- Arbitrary sizes: `patches.py` compiles fixed XML entries by name. Either add one entry per preset (720p, 1440p, 1800p,
  4K: the formats above), or let `run.py` pass a generated "WxH" write list (a small `patches.py` extension). Both are
  build work, not research.
- `run.py` for `*_native`: select the size patch plus the pool patch, set `GOW3_INTERNAL_WIDTH/HEIGHT=WxH`, and **do not**
  set `GOW3_UE4_SETRES`, as the 4K boot already does.
- An equivalent without XML: before game start, the runtime writes `{0,0,W,H}` to 0x5b1d2cc and 1 to 0x5b1d2c8.
  The `__cxa_guard` at 0x5b1d2e0 guards nothing else (0xb69133-0xb69169) **[V]**. The rodata write is simpler and fits
  the existing patch path.

### Framebuffer pool per size

The stock pool is 128 MB, with power-of-two buddy blocks over 64 KB pages (MediEvil.xml comment). A block is the next
power of two of the target's size. Tile padding is ignored below. Rows are **[I]**: the block sizes are arithmetic, but
which targets live in the pool was not traced.

| Size | 4 B/px target → block | 8 B/px target → block | Blocks vs 1080p |
|---|---|---|---|
| 1280x720 | 3.7 MB → 4 MB | 7.4 → 8 MB | 0.25-0.5x |
| 1920x1080 | 8.3 → 16 MB | 16.6 → 32 MB | 1x |
| 2560x1440 | 14.7 → 16 MB | 29.5 → 32 MB | **1x** (fits the stock pool if 1080p does) |
| 3440x1440 / 2560x1600 | 19.8 / 16.4 → 32 MB | 39.6 / 32.8 → 64 MB | 2x |
| 3200x1800 | 23.0 → 32 MB | 46.1 → 64 MB | 2x |
| 3840x2160 | 33.2 → 64 MB | 66.4 → 128 MB | 4x (stock pool ran out on a live 4K change) |
| 5120x2880 | 59.0 → 64 MB | 118 → 128 MB | 4x (same blocks as 4K) |

Keep "Native 4K framebuffer pool (512 MB)" (on by default) for every boot size. It covers up to 4K, and probably 5K. Above
1440p, also use the 4K memory settings (`GOW3_DMEM_MB`, `r.GarlicHeapSizeMB` / `r.Streaming.PoolSize` in `ue4_override.apply`):
render targets grow with the area **[I]**.

### UI scale (DPI) and aspect ratio

| Size | Shortest side | DPI | UMG layout space |
|---|---|---|---|
| 1280x720 | 720 | 0.666 | 1922x1081 |
| 1920x1080 | 1080 | 1.0 | 1920x1080 |
| 2560x1440 | 1440 | 1.333 | 1920x1080 |
| 3200x1800 | 1800 | 1.667 | 1920x1080 |
| 3840x2160 | 2160 | 2.0 | 1920x1080 |
| 3440x1440 (21:9) | 1440 | 1.333 | **2580**x1080 |
| 2560x1080 (21:9) | 1080 | 1.0 | **2560**x1080 |
| 2560x1600 (16:10) | 1600 | 1.48 | **1728**x1080 |

- 16:9 at any size: identical layout, sharper. **[I]** (from the curve)
- Ultrawide: anchored widgets move to the screen edges, centred ones stay. The 3D camera uses the default
  `AspectRatio_MaintainXFOV` (BaseEngine; the game does not override it **[V]**): horizontal FOV kept, less vertical
  view. Cameras with `bConstrainAspectRatio` (cutscenes, likely) draw black bars inside the frame **[I]**. 16:10: a
  layout narrower than 1920 may crowd or overlap the HUD **[I]**. Cheap to try: only the two values change. Widths that
  are a multiple of 64 (1280, 1920, 2560, 3200, 3840) are safest for the guest tiling; 3440 pads its pitch. The VideoOut
  driver takes `pitch_in_pixel > width`, but that is untested here **[I]**.

## 4. Fixing the live r.SetRes instead

What is stale after a live change: the cached rect (above). Also, probably, the SWindow geometry made at boot from
`ResX/ResY` (= the rect) **[I]**. In game code, only window creation, `DetermineGameWindowResolution`, the
desktop-resolution query and the resolution list read display metrics through Slate. None is on a resize path **[V]**
(scan of `[0x5b29af0]`+0x190 calls). So writing the cached rect before `ue4_setres` (runtime, 16 bytes at 0x5b1d2cc)
is necessary but probably **not sufficient**. The fullscreen `FSceneViewport::ResizeFrame` path would also have to
resize the SWindow so the hit-test grid follows it. That means finding `FSceneViewport::ResizeFrame` / the PS4 window's
reshape (no strings, virtual calls) and calling or hooking it from the runtime. High effort, with an outcome that needs
several launches. It is worth doing only to switch resolution in game without a restart, which the boot patch makes
unnecessary.

## 5. Other versions

Everything above is **CUSA11227 01.00** (eboot sha256 `a7e976d1…3b828`). A 01.02 update of the remake exists
(upstream `c7a9ef3`: "same code at a different layout", pool sites found by matching bytes; `1ae17cd` finds `r.SetRes`
by code pattern). Re-find by pattern rather than address. Each pattern below is unique in 01.00 **[V]**:

| Site | Pattern (01.00 hit) |
|---|---|
| metrics: cvar check + `je` | `83 3d ?? ?? ?? ?? 00 c5 f8 11 00 74 54 bf ff 00 00 00` (0xb69027) |
| work area: same | `83 3d ?? ?? ?? ?? 00 c4 c1 78 11 04 24 74 48 bf ff 00 00 00` (0xb6d689) |
| 4K constant | `48 b8 00 0f 00 00 70 08 00 00` (0xb69072, 0xb6d6c9) |
| default rects | bytes `00000000 00000000 80070000 38040000` (exactly 0x3cbe750, 0x3cbe7b0). Confirm they are the targets of the `vmovups xmm0,[rip+d]` (`c5 f8 10 05`) just before each cvar check |

## Recommendation (ranked)

1. **(a) Generic boot-at-WxH: the rodata patch** (2 writes per size) plus `run.py` routing for all `*_native` modes,
   with the pool patch, no live `r.SetRes` and VideoOut = WxH. Risk low: it is the 4K boot's mechanism with other
   numbers, and the renderer already runs these sizes through the live path. Unknowns: 16:10 / ultrawide HUD layout,
   non-64-multiple widths, memory above 1440p.
2. **(c) Only the PS4-style presets** (720p / 1080p / 1440p / 1800p / 4K) as fixed XML entries. This is the same
   mechanism, (a) without custom sizes. Ship (c) first, then expose custom WxH once (c) is verified.
3. **(b) Fix the live path.** High effort, unknown outcome (§4). Not recommended.

**First experiment (one launch, the user's go):** add "Boot display size 2560x1440" (above) to `MediEvil.xml`. Launch
with render resolution `native` (no live change) and
`run-game.ps1 -Env @{GOW3_PATCHES='Boot display size 2560x1440'; GOW3_INTERNAL_WIDTH='2560'; GOW3_INTERNAL_HEIGHT='1440'} -Tag boot1440`.
The pool patch comes in by default and "4K display metrics at boot" is not selected. Look for:
- log: `Patches: CUSA11227; 13 writes from ['Native 4K framebuffer pool (512 MB)', 'Boot display size 2560x1440']`;
  then `Patches: 13 writes, ... applied`; **no** `Native output: r.SetRes`; no `second open refused`. The VideoOut
  buffer attribute line (`driver.cpp:269`, `LOG_INFO`) does not appear in our logs at the default level. Confirm the
  size with a `GOW3_FRAME_DUMP_TRIGGER` dump instead (2560x1440 guest buffer).
- in game: 2560x1440 from the first logo. The HUD and menus have the same proportions as at 1080p (not a 1080p-sized UI
  in the top-left three quarters). Pause and Options menus navigable with the DualSense past "controls", and the debug
  menu's right column reachable (if on). Then a level load, to check memory.
- failure signs: UI at 1080p geometry (patch not applied → check the log line), a boot crash (pool / memory → retry
  with the 4K memory settings), or a black screen (presenter / pitch).
