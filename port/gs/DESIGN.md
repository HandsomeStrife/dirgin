# WS-G: GS backend design

Native PC port of *Dirge of Cerberus: FFVII*. This is the graphics seam: it
takes the DMA/GIF command stream the recompiled EE + VU1 produce and turns it
into draw calls for a modern GPU. All register/primitive claims below are
derived from the decompiled corpus (`analysis/decomp/`), not assumed; the
evidence function is named each time.

## 1. Data flow

```
 guest RAM (32MB mmap, WS-K)
   DMA chain in scratchpad ring @0x700000  (built inline by renderers)
        │
        │  transport opens the chain:  FUN_0030b768(path)
        │    path arg0 -> VIF1 ring, CHCR=0x105   (VIF1 -> VU1 -> GIF -> GS)
        │    path arg1 -> GIF  ring, CHCR=0x104   (GIF-direct -> GS)
        │  append qwords:              FUN_0030b670  (73 callers, workhorse)
        │  close + kick + wait:        FUN_0030b868 / single kick FUN_00442100
        ▼
   ┌─────────────── VIF1 path ───────────────┐   ┌──── GIF-direct path ────┐
   │ VIF1 unpack / MPG (UNPACK, MSCAL...)     │   │ DMAtag chain of raw GIF │
   │   -> VU1 microcode runs (WS-V)           │   │   packets (no VU)       │
   │   -> VU1 XGKICK emits GIF packets        │   │                         │
   └──────────────────┬───────────────────────┘   └───────────┬────────────┘
                       ▼                                        ▼
            ======================  GIF packet stream  ======================
                                     (this module)
                       ▼
   gs_submit_packet() / gs_submit_dma_chain()
     parse GIFtag (NLOOP/EOP/PRE/PRIM/FLG/NREG/REGS)
       FLG=PACKED  -> per-descriptor unpack (A+D, PRIM, RGBAQ, ST, UV, XYZ*)
       FLG=REGLIST -> raw 64-bit register writes (VU1 vertex streams)
       FLG=IMAGE   -> VRAM payload (texture/CLUT upload)
       ▼
   GS register state machine  (gs_core.c: apply_reg + vertex fifo)
     - context regs: FRAME/ZBUF/TEST/ALPHA/SCISSOR/XYOFFSET/TEX0/TEX1...
     - vertex kick on XYZ2/XYZF2 -> assemble primitive -> emit gs_draw
     - BITBLTBUF/TRXPOS/TRXREG/TRXDIR + IMAGE -> emit gs_tex_transfer
       ▼
   gs_backend vtable  (pluggable: log / software / OpenGL / Vulkan)
       ▼
   PCRTC flip: PMODE/DISPFBn/DISPLAYn written via MMIO
     FUN_00441f78 (regs) + FUN_00442100 (draw-env kick), from FUN_00440e68
       -> gs_write_display() + gs_flip()
```

Key architectural point confirmed by the map: the game **hand-assembles GIF
packets qword-by-qword inline** in each renderer (e.g. `FUN_004115b8`,
`FUN_0040e568`) — there is no generic tag builder to intercept. But *transport*
is centralised in a handful of functions, so the viable strategy is **capture
the DMA chain and reinterpret it** here rather than hand-retranslating every
inline packet builder. VU1 output (the 3D vertex stream) arrives as GIF packets
too; recovering VU1 (WS-V) is the long pole, but this module is what consumes
its output either way.

## 2. GS register subset DoC actually uses (with evidence)

The game touches the **privileged/PCRTC registers via MMIO** and everything
else **inside GIF packets** (A+D format). MMIO counts across the corpus:

| MMIO GS register | uses | role |
|---|---|---|
| `REG_GS_CSR` | 15 | sync / FINISH / reset (`FUN_00441720`, `FUN_00440468`) |
| `REG_GS_PMODE` | 3 | PCRTC blend of circuits 1/2 |
| `REG_GS_DISPFB1/2` | 2+2 | display framebuffer base/width |
| `REG_GS_DISPLAY1/2` | 2+2 | display rectangle/timings |
| `REG_GS_SMODE2` | 1 | interlace/field mode |
| `REG_GS_BGCOLOR` | 1 | background colour |
| `REG_GS_EXTDATA` | 1 | feedback write |
| `REG_GS_BUSDIR` | 3 | GS<->host bus direction for readback (`FUN_00441720`) |

Flip path = `FUN_00441f78` (writes PMODE/SMODE2/DISPFBn/DISPLAYn/BGCOLOR/
EXTDATA) called from `FUN_00440e68`.

**Drawing registers (via GIF A+D)** — proven by the inline packet builders:

| reg (addr) | evidence |
|---|---|
| FRAME_1 (0x4c) | `FUN_004408e0` param_1[1]; `FUN_004115b8` +0x38 |
| ZBUF_1 (0x4e) | `FUN_004408e0` param_1[3]; `FUN_004115b8` +0x48 |
| XYOFFSET_1 (0x18) | `FUN_004408e0` param_1[5] (window centre 0x800<<4) |
| SCISSOR_1 (0x40) | `FUN_004408e0` param_1[7] |
| PRMODECONT (0x1a) | `FUN_004408e0` param_1[9] |
| COLCLAMP (0x46) | `FUN_004408e0` param_1[0xb] |
| DTHE dither (0x45) | `FUN_004408e0` param_1[0xd] |
| TEST_1 (0x47) | `FUN_004408e0` param_1[0xf]; `FUN_004115b8` +0x68 |
| ALPHA_1 (0x42) | `FUN_004115b8` +0x58 (40 funcs write 0x42) |
| ALPHA_2 (0x43) | `FUN_004115b8` +0xb8 |
| TEX0_1 (0x06) | `FUN_004115b8` +0x78 |
| TEX1_1 (0x14) | `FUN_004115b8` +0x98 (59 funcs write 0x14) |
| TEXA (0x3b) | `FUN_004115b8` +0x88 (46 funcs write 0x3b) |
| RGBAQ (0x01) | `FUN_004115b8` +0xa8 |
| PRIM/XYZ/UV | vertex loop in `FUN_004115b8`, coords in 12.4 fixed (`<<4`) |

**Texture/VRAM transfer registers (via GIF A+D)** — `FUN_00441278` (build) +
`FUN_004415a0` (send MEM->GS) + `FUN_00441720` (GS->MEM readback,
"sceGsExecStoreImage"):

| reg | evidence |
|---|---|
| BITBLTBUF (0x50) | `FUN_00441278` param_1[3] |
| TRXPOS (0x51) | `FUN_00441278` param_1[5] |
| TRXREG (0x52) | `FUN_00441278` param_1[7] |
| TRXDIR (0x53) | `FUN_00441278` param_1[9] |
| FINISH (0x61) | `FUN_00441460` param_1[0x16] (store-image env) |

**Pixel storage modes** the transfer/store functions branch on
(`FUN_00441278` / `FUN_00441720` switch cases): PSMCT32 (0x00), PSMCT24 (0x01),
PSMCT16/16S (0x02/0x0a), **PSMT8 (0x13), PSMT4 (0x14)** (paletted), PSMT8H
(0x1b), PSMT4HL/HH (0x24/0x2c), and the Z variants (0x30-0x3a). The presence of
PSMT8/PSMT4 confirms **paletted textures with CLUT** are in active use.

**GIF primitive types**: the fixed-function 2D paths emit SPRITE and TRIANGLE
(gouraud/textured/blended) directly; the 3D geometry primitives (TRI_STRIP most
likely) come out of VU1 and are therefore data-driven — the interpreter
implements the full PRIM set (POINT/LINE/LINE_STRIP/TRI/TRI_STRIP/TRI_FAN/
SPRITE) so whatever VU1 emits is handled.

## 3. The C interface

See `gs.h`. Summary:

- `gs_create(backend, w, h)` / `gs_destroy` — lifecycle.
- `gs_submit_packet(buf, nqwords)` — parse a flat GIF packet buffer.
- `gs_submit_giftag(buf, avail)` — parse exactly one GIFtag + payload.
- `gs_submit_dma_chain(mem, size, addr)` — walk a DMAtag source chain
  (GIF-direct transport) and feed each segment.
- `gs_write_display(disp)` — PCRTC/MMIO registers (flip-time).
- `gs_do_transfer(t)` / internal TRXDIR+IMAGE path — VRAM texture transfer.
- `gs_flip()` — present.
- `gs_backend` vtable `{ init, draw, transfer, display, flip, shutdown }` makes
  the rasterizer target pluggable. `gs_backend_log()` is the headless stub.

## 4. GS semantics that are HARD to reproduce faithfully on a PC GPU

Ordered by risk. These are where "looks right" and "is right" diverge.

1. **4MB VRAM with overlapping buffers — HIGH risk / HIGH effort.** The GS has
   one flat 4MB pool; FRAME, ZBUF, textures and CLUTs are all just base
   pointers (word addresses `<<13`) into it, and the game freely overlaps them
   (render-to-texture, reading last frame's framebuffer as a texture, CLUT in
   the gap after a texture). A PC GPU has typed, non-aliasing resources. Faithful
   emulation needs a VRAM-tracker that detects overlap and inserts
   copy/round-trip barriers. `FUN_00441720` readback (`REG_GS_BUSDIR`) proves
   the game does GS->MEM round-trips. This is the single biggest correctness
   hazard.

2. **PSM swizzling — HIGH risk / HIGH effort.** VRAM is not linear: every PSM
   has a page/block/column swizzle pattern (32x32 pixel pages, 8x8/16x8 blocks,
   column interleave). An upload at TBP≠FBP, or a buffer re-read as a different
   PSM, only lines up if the swizzle is reproduced bit-exactly. The address math
   in `FUN_00441720` (the `& 0x1ffc`, `*3>>4` block arithmetic) is exactly this.
   Needed both to deswizzle uploaded textures and to honour overlap (item 1).

3. **Paletted textures (CLUT) — MED/HIGH.** PSMT8/PSMT4 in use (confirmed).
   CLUT lives in VRAM, has its own storage mode (CSM1/CSM2), CSA offset and CBP,
   and can be updated independently of the texture. On the GPU this is a second
   indexed sampler + palette texture, with cache-invalidation semantics
   (TEXFLUSH) the game relies on.

4. **Per-pixel TEST + ALPHA blend model — MED.** ALPHA is a fixed
   `(A-B)*C>>7 + D` equation with A/B/C/D selecting Cs/Cd/0 and As/Ad/FIX
   (`FUN_004115b8` writes 0x42/0x43); TEST combines alpha-test, destination-
   alpha test (DATE/DATM) and depth-test (`FUN_004408e0` writes 0x47). Most maps
   to GPU blend/stencil/depth state, but **DATE** (destination-alpha test) and
   **alpha-fail modes** (AFAIL = FB_ONLY / ZB_ONLY) have no direct equivalent and
   need stencil tricks or a shader discard pass.

5. **24.8 / 12.4 fixed-point coordinates & the 2048 window — MED/LOW.** XY are
   12.4 subpixel fixed offset by XYOFFSET (default 0x8000 = 2048.0, confirmed
   `FUN_004408e0`: `0x800 - w/2`). Z is 24- or 32-bit integer depth, not float.
   Sub-pixel rounding and the integer Z range must match or z-fighting and seams
   appear. The interpreter already converts `(fx-ofx)/16`; getting rasterisation
   rules bit-identical is the hard part.

6. **Dithering — LOW/MED.** DTHE + the 4x4 DIMX matrix (`FUN_004408e0` writes
   0x45) dithers 16-bit output. Cosmetic, but the game's 16-bit buffers banding
   pattern is part of its look; emulate in a post/shader pass, defer.

7. **Misc exactness — LOW each, additive.** COLCLAMP (wrap vs clamp of colour
   overflow), fog as a per-vertex register, PABE per-pixel alpha blend enable,
   AA1 edge antialiasing, FBA alpha correction, mipmap LOD (TEX1 L/K/MXL). None
   individually blocking; collectively they are the difference between
   "recognisable" and "faithful".

**Verdict:** the command-parsing half (this skeleton) is straightforward and
done. The rasterisation half is the real wall, and within it VRAM
aliasing + swizzle (items 1–2) is the part that cannot be faked — it dictates
whether a software reference rasteriser is needed as the ground-truth backend
before any GPU backend can be trusted.

## 5. Files

- `gs_regs.h` — register addresses, GIF descriptors, PRIM/PSM enums (evidence-annotated).
- `gs.h` — public interface + backend vtable + decoded value types.
- `gs_core.c` — GIFtag interpreter + GS register state machine + vertex assembly + DMAtag walker.
- `backend_log.c` — headless stub backend (no GPU).
- `test_triangle.c` — hand-built GIF packet (env + textured triangle + texture upload + flip).
- `Makefile` — `make check` builds and runs the test headlessly.
