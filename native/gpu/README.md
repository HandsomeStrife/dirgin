# native/gpu — Milestone P3, the GPU renderer

The **real GPU renderer** for the purist native PC port of *Dirge of Cerberus:
FFVII* (PS2, SLPS-20200) — the foundation of the eventual Windows renderer. It
loads a **real DoC model** (BMD or MOD) plus a **real decoded texture**, uploads
the geometry as a VBO/VAO and the texture as a GL texture, transforms it with a
perspective auto-framed camera, and rasterizes it **on the GPU** through an
OpenGL 3.3-core pipeline into an off-screen framebuffer, then reads the pixels
back and writes a **PNG**.

It is the GPU counterpart to the software reference rasterizer in
`../renderer/`. The camera framing and lighting constants are **deliberately
identical**, so the two outputs can be compared pixel-for-pixel — the software
renderer is the validation oracle (a GPU-driver bug and a decode bug look
identical on screen until you have an oracle with no driver).

## Did EGL offscreen work on this headless box? — YES, on real hardware

This server has no window system (no GLFW/SDL/X), so the context comes from
**EGL surfaceless/pbuffer + an FBO**. It works, and not via a software
fallback — it binds the actual GPU:

```
EGL 1.5 Mesa Project / AMD Radeon Graphics (gfx1103_r1, LLVM 15.0.7) /
OpenGL 4.6 (Core Profile) Mesa 23.2.1 / surface=pbuffer
```

- Display: `EGL_MESA_platform_surfaceless` (falls back to the default display).
- API: desktop **OpenGL**, context requested as **3.3 core** (driver returns a
  compatible 4.6 core, which is fine — we only use 3.3 features).
- Surface: a 1×1-style **pbuffer**; if a config has no pbuffer the code falls
  back to a **surfaceless make-current**. Either way the renderer draws into its
  **own FBO** (RGBA8 colour + depth24), so the default framebuffer is unused.
- All GL entry points are resolved through `eglGetProcAddress`
  (`EGL_KHR_client_get_all_proc_addresses` is present).

No OSMesa fallback was needed. (`libOSMesa.so` is present on the box if a pure-
software path is ever wanted; the context seam below makes that a one-file swap.)

## What it does

1. **Links the native loaders** (read-only, never modified):
   - `../assets/models/doc_model.c` → BMD/MOD → `objects → strips → vertices`.
   - `../assets/textures/libdtex.a` → TXD/TIM2/RST → RGBA8, plus the PNG writer
     (`dtex_write_png`) reused for output.
2. **Geometry upload**: triangle-strips are expanded to an **unindexed triangle
   list** (same alternating-winding flip as the reference) so each triangle gets
   its own flat face normal; uploaded once as a VBO + VAO (`pos.xyz`, `uv`).
3. **Shaders** (GLSL 330 core): vertex stage does the MVP transform and passes
   view-space position + UV; fragment stage reconstructs the **flat face normal
   from screen-space derivatives** of the view-space position (`cross(dFdx,
   dFdy)`), flips it toward the viewer (two-sided, as the reference does),
   applies `ambient + (1-ambient)·max(N·L,0)` directional light, and samples the
   texture (textured mode) or a neutral surface colour (shaded mode).
4. **Camera**: identical to the reference — right-handed lookAt + 45° perspective
   (column-major), auto-framed from the model's bounding sphere, orbit azimuth
   (`--angle`) / elevation (`--elev`), 25 % fit margin.
5. **Texture**: `glTexImage2D` RGBA8, `GL_REPEAT` wrap, bilinear+mipmap
   (`--nearest` for point sampling) — GL gives perspective-correct UV for free.
6. **Output**: render to FBO → `glReadPixels` → row-flip to top-left origin →
   PNG via the texture lib's writer.

## Build

```
make                 # -> ./gpu_renderer   (links libdtex + doc_model, -lEGL -lGL)
# or
cmake -B build && cmake --build build
```

Portable C99, warning-clean under `-Wall -Wextra`. Links EGL + GL + libm.

## Usage (same CLI as the software renderer)

```
gpu_renderer <model.bin> [texture.bin] [out.png]
             [--angle DEG] [--elev DEG] [--size WxH]
             [--mode textured|shaded|wire] [--nearest] [--cull]
```

Example:
```
gpu_renderer noPath/06080112.bin out/env.png --mode shaded --angle 35 --elev 25
```

## Validation — GPU vs software reference (`out/`)

Rendered from real game assets; compared against `../renderer/` at the **same
angle/size**. Per-channel mean-absolute-difference over the full 1000×750 frame:

| View | Mode | Mean abs diff /255 | Max | Pixels differing >16 |
|------|------|-------------------:|----:|---------------------:|
| BMD `06080112` a35 e20  | shaded | **0.001** | 26 | **0.000 %** |
| BMD `06080112` a110 e25 | shaded | **0.004** | 8  | **0.000 %** |
| BMD `06080112` a35 e20  | cull   | **0.000** | 1  | **0.000 %** |
| BMD `06080112` a35 e20  | wire   | 7.18      | 230| 5.99 % |
| MOD `0c064400` a35       | textured→shaded | 0.011 | 178 | 0.000 % |

- **Shaded BMD is a pixel-match** (mean diff 0.001–0.004 /255; zero pixels differ
  by more than 16). The GPU and the from-scratch software rasterizer produce the
  same image of the same real DoC model — the P3 GPU deliverable, cross-validated.
- **Cull** matches to 0.000 — the GL back-face cull (`GL_CCW` front) reproduces
  the reference's facing test exactly.
- **Wire** differs ~6 % of pixels: GL's line rasterizer uses different endpoint/
  coverage rules than the reference's Bresenham lines; the geometry/coverage
  agree (identical silhouette), only pixel-exact edge placement differs. Expected.
- **MOD** falls back to shaded in **both** renderers because the loader does not
  yet emit usable MOD UVs (documented in `../renderer/README.md`); the two agree
  in that fallback.

### Texture-sampling proof — `out/gpu_texture_sampling_proof.png`

Because neither loader emits per-vertex UVs yet, the textured path can't be shown
through a model. `gpu_texture_sampling_proof.png` renders a synthetic UV quad
(UV 0..2) with the real RST `06d1ff0b` crate texture, proving the full GPU
sampler path — `glTexImage2D` upload, `sampler2D`, `GL_REPEAT` wrap (visible as a
2×2 tile), bilinear filtering, directional lighting. When the loader lands real
UVs, textured models render with **no change here**.

Copies for the shared drive: `/srv/shared/dirge-port/gpu_render_01.png` (shaded),
`…_02.png` (orbited), `…_03_texture.png` (texture proof).

## Purist shading — the RENDER-MATH integration

The renderer now carries the **original PS2 per-vertex shading model**, ported
from the reverse-engineered VU1 math in `../../port/vu/RENDER-MATH.md` and its
GLSL sketch `../../port/vu/shaders/dirge_purist.{vert,frag}`. This is a second,
faithful shader program alongside the legacy flat placeholder (which is kept
intact so the DocModel pixel-match above still stands).

### What was added

- **`rgl_render_mesh()`** (`renderer_gl.{c,h}`) — the purist render path. It
  takes a ready-built triangle list of **`RVertex`** (position, **normal**,
  **UV**, **RGBA vertex colour** — the full RENDER-MATH vertex format, where the
  old path carried only position + UV) and renders it with the new shader pair.
- **`SHADE_VS` / `SHADE_FS`** — the RENDER-MATH shader pair:
  - transform `clip = MVP·v`; UV passed through (the rasterizer does
    perspective-correct `s/w,t/w` — the PS2 `MULq`-by-Q pre-divide is a GS
    artefact we must *not* replicate);
  - **LIT (program D / actors)**: `d_i = max(Ldir_i·n, 0)` for 3 directional
    lights, `lit = Σ Lcol_i·d_i + ambient`, `rgb = saturate(lit ⊗ vcol)`,
    diffuse only, no specular/attenuation — exactly RENDER-MATH §1.2. Gouraud
    (per-vertex), as the PS2 did it. Alpha rides through untouched.
  - **PRELIT (programs A/B / static world)**: no N·L — the baked vertex colour
    passes straight through; fog optional. RENDER-MATH §2.
  - fragment: GS `TEX0.TFX` combine (MODULATE with the 8-bit `128 = 1.0`
    doubling / DECAL / HIGHLIGHT), alpha-test, and the affine fog blend.

### A / B / D mode mapping

| RENDER-MATH program | geometry | `rgl_params` | shader behaviour |
|---------------------|----------|--------------|------------------|
| **A, B** (world)    | static, pre-lit | `shade_mode = RGL_SHADE_PRELIT` | vertex colour × texture (+ fog); **no** N·L |
| **D** (actors)      | characters | `shade_mode = RGL_SHADE_LIT` | 3-dir N·L + ambient, ⊗ vertex colour, saturate |

### Placeholder light constants — **not fabricated, flagged**

RENDER-MATH §6 is explicit that the real light matrices (`L` directions, `C`
colours, `A` ambient, the `MINIi` saturate `I`) are **runtime VuMem uploads at
0x40–0xa0, not present in the ELF**. The *form* is certain (3 directional lights
+ ambient, `max(0)` Lambert clamp, ⊗ vertex colour, saturate); the *numbers*
are not. So `rgl_default_lightrig()` ships a clearly-labelled **placeholder**
key/fill/rim rig (`DEFAULT_RIG` in `renderer_gl.c`), chosen only so the model
reads with an obvious directional gradient. **Swap it for a VuMem dump taken
right before an actor `MSCAL`** to get the authentic look; the shape of the code
does not change, only the constants.

### Validation — synthetic meshes (`validate_shading`)

The real model loader does not yet emit per-vertex normals/colour/UVs, and
`../assets/models` is busy — so `validate_shading.c` does **not** link it. It
builds its own meshes in-code (UV sphere with smooth normals, subdivided cube,
tiled ground plane), decodes a **real game texture** through libdtex (the RST
crate `06d1ff0b` / `hm_box_l`, 128×128 PSMT8, read-only), and renders through
the same GL-3.3/EGL core. Build and run:

```
make validate            # -> ./validate_shading  (links libdtex, NOT doc_model)
./validate_shading -o out [TEXTURE_CONTAINER]
```

Outputs in `out/` (copies in `/srv/shared/dirge-port/`):

| PNG | mode | proves |
|-----|------|--------|
| `gpu_lit_sphere.png`        | LIT, white vcol, no tex   | smooth 3-directional-light N·L + ambient gradient (warm key upper-right, cool shadow lower-left) |
| `gpu_textured.png`          | PRELIT + crate tex        | real decoded texture, MODULATE combine, natural brightness via neutral (128) vcol |
| `gpu_perspective_plane.png` | PRELIT, tiled tex, grazing | perspective-correct UV — tiles foreshorten toward the far edge |
| `gpu_prelit_vcolor.png`     | PRELIT, per-face vcol      | A/B vertex-colour pass-through (flat faces, **no** N·L) |
| `gpu_lit_textured.png`      | LIT + crate tex           | full program D — lighting ⊗ texture |

The two required proofs: the **lit sphere** shows correct directional shading
gradients, and the **textured** renders show the real texture with
perspective-correct UVs. Warning-clean under `-Wall -Wextra`.

## Windows portability — the structure

The one and only platform-specific file is the **context backend**; everything
that draws is portable:

| File | Role | Windows |
|------|------|---------|
| `gfx_context.h` | abstract: create a current GL 3.3-core context | unchanged |
| `gfx_context_egl.c` | **Linux/EGL** implementation (surfaceless/pbuffer) | replace with `gfx_context_wgl.c` (Win32 window + `wglCreateContextAttribsARB` + swapchain, or an off-screen pbuffer) |
| `gl_api.{h,c}` | loads all GL entry points via a `getproc` callback | unchanged (pass `wglGetProcAddress`; the ~20 GL 1.1 calls also go through `GetProcAddress(opengl32.dll, …)`) |
| `renderer_gl.{h,c}` | **all** rendering: VBO/VAO, shaders, FBO, camera, readback | unchanged |
| `main.c` | CLI + loaders + PNG | unchanged (swap the one `gfx_context_*` link) |

So the Windows port is: add `gfx_context_wgl.c`, link `opengl32`, and present the
FBO (or render to the swapchain's backbuffer) instead of reading it back to PNG.
`CMakeLists.txt` already exposes `-DGFX_BACKEND=WGL` for that build. The shaders,
the GL 3.3-core pipeline, the camera math and the geometry/texture upload are the
real renderer and move over verbatim.

## Files

- `gfx_context.h` / `gfx_context_egl.c` — platform GL-context seam (EGL here).
- `gl_api.h` / `gl_api.c` — GL 3.3-core function-pointer loader (X-macro list).
- `renderer_gl.h` / `renderer_gl.c` — the portable render core + GLSL shaders.
- `main.c` — CLI, loader glue, PNG output.
- `render_char.c` — **real-character render** (`make char`): loads the posed,
  textured DoC character exported by `../assets/models/render_skinned.py`
  (`out/char_e000.mesh`, a binary `DMSH` RVertex triangle soup byte-identical to
  `RVertex`), decodes its body atlas natively via libdtex (largest image in the
  sibling `0765xxx2.bin`, RST 512×256 PSMT8), and renders it through
  `rgl_render_mesh` — PRELIT+textured and LIT+textured front / three-quarter, plus
  a LIT untextured pass. This is the convergence milestone: the native GL renderer
  drawing real decoded game content. Outputs `out/char_e000_{prelit,lit}_*.png`.
- `render_scene.c` — **composed-scene render** (`make scene`): loads a BMD
  environment mesh AND the character mesh into **one GL world** and frames the
  character standing in the room. See "Composed scene" below.
- `validate_shading.c` — synthetic-mesh shading validation (`make validate`).
- `Makefile`, `CMakeLists.txt` — the two build paths.
- `out/` — the validation PNGs above.

## Real-character convergence (`make char`)

`render_char` closes the loop: the asset pipeline (`render_skinned.py`) exports the
skinned, UV'd, vertex-coloured mesh + resolved texture, and this binary uploads and
renders it on the actual GPU. Normals are reconstructed from geometry and forced
outward from the model centroid (the S-8 strip winding is not globally consistent,
FORMAT.md §8.4, and the LIT shader samples the per-vertex normal directly), giving a
clean two-sided lit body. The GPU output matches the software rasteriser
(`../renderer/out/char_e000_textured*.png`) — same geometry + texture, now
GL 3.3-core + the purist shader. The body is a clean textured humanoid (flesh arms,
white/blue striped trousers, sheathed gun); the hair stays shardy — the known
§8.4 mesh limit, not a GPU issue.

## Composed scene — character in environment (`make scene`)

`render_scene` draws a real DoC **character inside a real DoC environment**, both
native decoded assets, in one framed GPU image — the first scene on this renderer.

Pipeline:

- **Environment** — `../assets/models/out/bmd_06960212.mesh`, a `DMSH` exported by
  the new `../assets/models/export_bmd_mesh.py` from the gallery room `06960212`
  (a multi-floor pillared pavilion: full floor slabs at Y=−60 / 0 / +90 on
  interior partition columns). The exporter runs the native `doc_model` loader to
  decode the BMD to OBJ, then reconstructs per-face flat normals and writes the
  `DMSH`. Rendered **PRELIT** (static world geometry; no runtime light rig).
- **Character** — `out/char_e000.mesh` + body atlas, rendered **LIT + textured**,
  exactly as `render_char`.
- Both meshes go to `rgl_render_scene()` (new, in `renderer_gl.c`): one FBO, one
  **shared depth buffer**, one auto-framed camera. Per-draw `shade_mode` / texture
  lets the environment be PRELIT and the character LIT in the same pass, and the
  shared depth buffer makes them **depth-sort against each other** (the character
  occludes the floor behind it and is occluded by the roof above).

Output: `out/scene_char_in_env_{hero,corner,wide}.png`; the hero shot is copied to
`/srv/shared/dirge-port/18_scene_character_in_environment.png`.

### Honest approximations (documented, not fabricated)

- **Placement is a HAND-SET transform.** We do not yet have the game's scene-
  placement data, and the two assets don't share an authored frame (the MOD actor
  is in bone-local units ~20 tall; the BMD room is world-float ~150 tall). So
  `render_scene` applies a hand-tuned *uniform scale + yaw + translate*
  (`place_character()`: scale 2.6, stood on the Y=0 floor at the open +X/+Z corner,
  feet pinned to the slab) chosen so the character reads as a person at plausible
  scale in the hall. Every number is a CLI flag (`-s -x -y -z -yaw`) — nothing is
  claimed to be the original layout.
- **The environment's PRELIT colours are a stand-in.** BMD world geometry is PRELIT
  from *baked per-vertex colours* (and textured by UVs) that live in a VIF stream
  the native loader does not yet resolve (uv range reads 0..0). Rather than ship a
  flat untextured silhouette, `export_bmd_mesh.py` bakes a reconstructed grey
  directional shade (`ambient + (1−ambient)·|N·L|`, two-sided) into the prelit
  vertex colour so the room reads as 3D. This is a documented placeholder for the
  unrecovered baked colours, derived from the real decoded positions; only the grey
  shade is provisional. The character's own lighting is the same placeholder rig as
  everywhere else (RENDER-MATH §6).

Both meshes are real decoded game assets on the real GPU through the purist shader
pair; the transform and the env's grey shade are the two labelled approximations.
