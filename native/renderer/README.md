# native/renderer — Milestone P3 offscreen renderer

The **first renderer** for the purist native PC port of *Dirge of Cerberus:
FFVII* (PS2, SLPS-20200). It loads a **real DoC model** (BMD or MOD) plus a
**real decoded texture**, transforms the geometry with a perspective camera,
rasterizes it with a z-buffer, shades it, and writes a **PNG** we can look at.

## This is a SOFTWARE reference rasterizer (on purpose)

This build server is **headless** — GL headers exist but there is no window
system and no GLFW/SDL/GLEW. So this renderer is a **dependency-free, portable C
software rasterizer**: no GPU, no emulation, no external libraries (only libc +
libm). It exists to be the **ground-truth / validation oracle** for geometry and
texture decoding.

The real-time **GPU backend (Vulkan / Direct3D)** is a **later Windows
milestone**. It will be validated *against this renderer's output*, exactly the
way the hybrid recomp build serves as the behavioural oracle for the reauthored
engine. Keeping a tiny, inspectable, from-scratch rasterizer as the reference is
deliberate — a GPU driver bug and a decode bug look identical on screen until
you have an oracle that has no driver.

## What it does

1. **Links the native loaders** (does not modify them):
   - `../assets/models/doc_model.c` → BMD/MOD geometry into
     `objects → strips → vertices (x,y,z, provisional u,v)`.
   - `../assets/textures/libdtex.a` → TXD/TIM2/RST container → RGBA8, plus the
     self-contained PNG writer (`dtex_write_png`) reused for output.
2. **Camera**: right-handed `lookAt` + perspective projection (column-major
   mat4). Auto-frames the model from its bounding sphere; orbit azimuth
   (`--angle`) and elevation (`--elev`) place the eye; 45° vertical FOV with a
   25% margin so the whole model fits.
3. **Rasterizer**: half-space / barycentric triangle fill, **z-buffer** depth
   test, **perspective-correct** UV interpolation (interpolate attr·(1/w), then
   divide). Triangle-strips are expanded with the alternating-winding flip.
   Back-face culling is **optional** (`--cull`); the default is **two-sided**
   because MOD winding flags are still provisional (see caveats).
4. **Shading**: flat per-face normal taken from the triangle geometry (view
   space), two-sided N·L directional light + ambient, so form is visible even
   where per-vertex attributes are provisional.
5. **Texturing**: nearest or **bilinear** (default) sampling of the decoded
   RGBA texture, wrap addressing. If a submesh/model has **no usable UVs**
   (BMD today, or a MOD whose UV range is degenerate) it falls back to flat
   shaded cleanly.
6. **Output**: PNG via the texture lib's writer. CLI below.

## Build

```
make            # builds ./renderer (also builds ../assets/textures/libdtex.a)
```

Portable C99, warning-clean under `-Wall -Wextra`. Only libc + libm.

## Usage

```
renderer <model.bin> [texture.bin] [out.png]
         [--angle DEG] [--elev DEG] [--size WxH]
         [--mode textured|shaded|wire] [--nearest] [--cull]
```

- `model.bin` — an RFD container (`noPath/*.bin`) holding a BMD or MOD payload.
- `texture.bin` — optional RFD/TXD container; the **first** texture is used.
- Positional ending in `.png` is the output; otherwise it is the texture.
- Mode defaults to `textured` when a texture **and** usable UVs are present,
  else `shaded`.

Example:
```
renderer noPath/06080112.bin out/env.png --mode shaded --angle 35 --elev 25
renderer noPath/0c064400.bin noPath/06d1ff0b.bin out/char.png --mode textured
```

## Validation renders (`out/`)

Rendered by this code from real game assets:

| PNG | Model | What it shows |
|-----|-------|---------------|
| `bmd_06080112_shaded.png`      | BMD env `06080112` (6700 v / 5084 t) | Coherent 3D multi-layer platform/floor environment piece, directional-lit. **Primary P3 proof.** |
| `bmd_06080112_shaded_a110.png` | same, orbited to 110° | Same model from another angle — silhouette/perspective change correctly ⇒ genuinely 3D. |
| `bmd_06080112_wire.png`        | same, wireframe | 5084 triangles, correct perspective + depth; dense detailed centre over a flat base plane. |
| `bmd_06080112_cull.png`        | same, `--cull` | Back-face culling path (front faces only). |
| `mod_0c064400_shaded.png`      | MOD char `0c064400` (km01vnw, 558 v / 434 t) | Character-scale int8 geometry, shaded. See caveat. |
| `mod_0c064400_textured.png`    | MOD + RST `06d1ff0b` (hm_box_l, 128×128) | Same geometry with the decoded RST texture sampled (rusty tones) — proves the texture path end-to-end. |

## Honest caveats

- **BMD is the clean proof.** `06080112` is resolved **float world-space**
  geometry (build-log "P1 MODEL DEEPENED"); it renders as a recognizable,
  coherent, correctly-depth-sorted 3D form from any angle. That is the P3
  deliverable: our own native code turning a real DoC model into an image.
- **MOD geometry is still provisional.** The MOD `0c064400` renders with
  spiky/thin triangles. This is **not** a renderer fault — the rasterizer draws
  exactly the `objects→strips→verts` the loader hands it (the BMD, from the same
  rasterizer, is clean). It is the **loader's provisional MOD strip
  connectivity**: the 9-byte strip variant / strip→group assignment and the
  `b3,b4` attribute semantics are flagged UNRESOLVED in `build-log.md` ("P1
  MODEL DEEPENED": packed by VU0 `vcallms 0x198`, not in the extracted VU1
  disasm; "provisional until 2nd model + VU dump confirm"). The positions
  (int8/16 → ±8 world units) are right; which vertices form each triangle is
  not yet. When that lands in `doc_model.c`, this renderer draws it correctly
  with no change here.
- **UVs are provisional**, so textured output is approximate (per build-log: MOD
  `b3,b4` UV/normal/colour semantics and BMD's separate UV VIF stream are
  unresolved). The textured MOD pass proves the **sampling pipeline** works; the
  shaded passes are the unambiguous form proof, as the task directs.

## Files

- `renderer.c` — everything: mat4 math, framebuffer+zbuffer, texture sampler,
  triangle rasterizer, camera auto-framing, CLI.
- `Makefile` — compiles `renderer.c` + the sibling `doc_model.c`, links
  `libdtex.a`.
- `out/` — the validation PNGs above.
