---
name: renderdoc-replay
description: Replay and read MediEvil RenderDoc captures (.rdc) with the research/tools/renderdoc scripts - finding a draw by position when shader names are absent, a draw's inputs and outputs, pixel history, saving targets - and the channel-order rule (RenderDoc texel values are stored order, shaders see swizzled views). Use before any RenderDoc analysis, before reasoning about red / blue / colour channels from captures or screenshots, and before writing a host pass that reads or writes a game image.
---

# RenderDoc replay (no game launch)

## Before every replay

- **The game must not be running.** A replay loads the GPU for minutes and skews the user's run. Check right
  before each replay, not once per session (the user may have started the game since):
  `Get-Process medievil-runtime -ErrorAction SilentlyContinue` (PowerShell tool; empty = not running).
  A build whose last step says `cp: cannot create regular file 'out/medievil-runtime.exe': Device or resource busy`
  means the game is running: stop, tell the user, copy `out/gpu-release/medievil-runtime.exe` (+ `medievil-gpu-check.exe`,
  `medievil-play.exe`) to `out/` after they close it.
- Captures live in `research-data/<topic-date>/*.rdc` (1-2 GB each). Notes on what each shows are in the study doc
  that made them (grep its name in `research/docs`). Ask the user which capture shows what before guessing.

## Running a script

Every script runs inside qrenderdoc, configured by environment variables (header comment of each script):

```bash
RD_CAPTURE='C:\...\menu_frame19708.rdc' RD_OUT='C:\...\scratchpad\x.txt' RD_EIDS=9318 \
  timeout 900 "/c/Program Files/RenderDoc/qrenderdoc.exe" --python 'C:\Users\Ty\MediEvil\medievil_2019_pc_port\research\tools\renderdoc\draw_io.py' >/dev/null 2>&1
```

Windows paths in the variables, output to the scratchpad. Python scripts of your own in the scratchpad go in a
subfolder (a stray `dis.py` there once shadowed the standard library).

## Which script

| Question | Script |
|---|---|
| A draw's shaders, every bound image (view format + swizzle), buffers, targets, blend | `draw_io.py` (RD_EIDS) |
| Every draw with its shader names (find the draw) | `shader_draws.py` with `RD_LIST=1` |
| A shader's draws by program hash, inputs saved as PNG | `shader_draws.py` (RD_SHADERS) or `RD_EIDS` |
| Which events touched a pixel of a target, values before / after | `pixel_history.py` (RD_PIXEL, RD_TARGET) |
| Every use of one image or buffer | `resource_usage.py` (RD_RESOURCE) |
| A target after a draw, raw bytes | `save_target.py` |
| A texture a draw samples, raw bytes (stored order) | `save_texture.py` (RD_CAPTURE, RD_EID, RD_RESOURCE, RD_OUT) |
| A draw's pipeline state incl. sampler filtering | `draw_state.py` |
| Render passes in order with PNGs | `pass_images.py` |
| Shader disassembly + bound buffer floats | `disasm.py` |
| Per-instruction values at one pixel | `debug_pixel_values.py` |

**Shader names are often absent** (the port names modules only with markers on): `shader_draws.py` matches
nothing. Find the draw by position instead: `RD_LIST=1`, then the draws into the target you care about (e.g. the
UE4 UI composite is the last non-swapchain draw before the swapchain draws; its inputs are the scene and UI images).
`draw_io.py` with `RD_SPV_DIR=...\user\cache\CUSA11227` maps a module back to `<pgm_hash>_<perm>`.

## Channel order (LANDMINES.md 28)

RenderDoc's pixel values, pixel history and saved PNGs show the image's **stored** channel order. The game's shaders
often read through views with a swizzle (UE4's tonemapper output and Slate's UI image: `bgra`), and colour exports
swap back. So:

1. Read the swizzle: `draw_io.py` prints `fmt ... swizzle ...` per view and flags `<<< CHANNEL ORDER`.
2. A host pass that replaces or adds to a game draw: read through the game's own view (keep `view_info.mapping`),
   and write in the order the game's shader writes (its IR in `user/shader/dumps/fs_0x<hash>.irprogram.txt`: which
   input component feeds `SetAttribute RenderTarget0, ..., #0`). The HDR UI composite and the meter patch shader
   both write `.bgr` / `.zyx` for this reason (their inputs and targets store blue first).
3. Never claim an order from texel values or RenderDoc PNGs: for these images they show red and blue swapped (the
   title logo looks icy blue and the title's sky brown-orange in RenderDoc; on screen the logo is orange and the night
   sky blue, the user 2026-11-19). Judge colours only from the screen or the user's screenshots, and ask the user what
   a colour should be rather than assuming. Mixing maths (gamut matrices, luminance weights) on swapped channels
   shifts hues (greens turn mint) even when a pass-through looks right.

## Screenshots (the user's NVIDIA JXRs)

`C:\Users\Ty\Videos\NVIDIA\medievil-runtime.exe\*.jxr`, scRGB float (1.0 = 80 nits, BT.709). No Python JXR decoder
is installed; decode with WPF (PowerShell `System.Windows.Media.Imaging.BitmapDecoder`, convert to `Rgba128Float`,
`CopyPixels` to a raw file), then numpy for nits and an SDR preview (nits / 100, 1/2.2).
