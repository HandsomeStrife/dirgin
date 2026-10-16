# Dirge of Cerberus (PS2) — BMD / MOD model format spec

Milestone P1 (geometry). Reversed from the game's **own** EE parser in
`analysis/decomp/` (read-only). Every field below cites the decompiled function
address that reads or relocates it. Where a claim rests on byte patterns across
files rather than parser code, it is marked **(empirical)**. Unknowns are called
out honestly in §6.

Containers live in the extracted tree at `extract/_KEL/noPath/*.bin` (named by
hex FileCode): `model`-named RFDs carry **BMD**, `evm###`-named RFDs carry **MOD**.

---

## 1. RFD container (the wrapper) — SOLVED

Spec source: **`FUN_002585e0`** (load-time relocation + per-entry dispatch) and
**`FUN_002587e0`** (the inverse, un-relocation on unload). Matches the existing
wiki note and is byte-verified.

Header:

| off  | size | field            | evidence |
|------|------|------------------|----------|
| 0x00 | 8    | archive name     | wiki + bytes |
| 0x08 | 4    | ext ("rfd"/"txd")| `FUN_002585e0` reads entry ext at +8 |
| 0x0C | 4    | total size       | bytes |
| 0x10 | 2    | file count       | `FUN_002585e0` loops `*(u16)(base+0x10)` |
| 0x12 | 1    | reserved (0)     | |
| 0x13 | 1    | type flag        | 0/1/2 observed |
| 0x14 | 4*n  | entry-offset table | `FUN_002585e0`: `*(base+0x14+4i) += base` |

Each **entry** (pointed to by the relocated offset table):

| off  | size | field |
|------|------|-------|
| 0x00 | 8    | file name |
| 0x08 | 4    | **extension** (dispatch key, read as little-endian u32) |
| 0x0C | 4    | size field (unreliable — use next entry offset) |
| 0x10 | …    | **payload** starts here |

`FUN_002585e0` dispatches on the extension-int and calls the payload parser with
`entry + 0x10` (confirming the +0x10 payload rule):

| ext int   | ext   | parser            | meaning |
|-----------|-------|-------------------|---------|
| 0x646d62  | "bmd" | **`FUN_0026abe8`**| 3D model (static/world) |
| 0x646f6d  | "mod" | **`FUN_005aecf8`**| 3D model (character/event) |
| 0x646d73  | "smd" | `FUN_005b0470`    | (skeleton/motion?) |
| 0x646b73  | "skd" | `FUN_005b27c8`    | |
| 0x646b6d  | "mkd" | `FUN_0030b9e0`    | |
| 0x646963  | "cid" | `FUN_00271208`    | collision |
| 0x646174  | "dat" | `FUN_00414170`    | |
| 0x747372  | "rst" | `FUN_00413658`    | texture (see textures/) |

---

## 2. MOD payload — mostly SOLVED (one geometry variant)

Spec source: **`FUN_005aecf8`** (header relocation).

### 2.1 Header

| off  | type | field | evidence |
|------|------|-------|----------|
| 0x00 | 4    | magic "MOD\0" | bytes |
| 0x04 | u32  | **version** | `FUN_005aecf8` requires `>= 0x10` (16). Seen: 16, 19. |
| 0x08 | u32  | (count?) | — |
| 0x0C | u32  | (count?) | — |
| 0x10 | u32  | **top-entry count** | `FUN_005aecf8` loops `*(base+0x10)` |
| 0x14 | u32  | **offset → entry-offset array** | `FUN_005aecf8`: `*(base+0x14)+=base`, then each `array[i]+=base` |
| 0x1c | u32  | (offset) | — |
| 0x20 | u32  | (flags/dim) | often 0x00010000 |
| 0x24 | u32  | **trailer offset** | relocated when version ≥ 0x12; end of geometry stream |

`FUN_005aecf8` only relocates the top array and the trailer — per-entry contents
are self-contained (offsets inside an entry are **payload-relative**, verified by
the strip offsets landing on clean data when read from payload base).

### 2.2 Top entry — "9-vertex-strip" variant (as in `evm000`/km01vnw)

The entry begins with a small prologue, then a strip/group table:

| off (in entry) | type | field | evidence |
|------|------|-------|----------|
| 0x00 | u32  | flags/type | bytes |
| 0x04 | u32  | (bound/scale?) | empirical |
| 0x08 | u32  | (bound/scale?) | empirical |
| 0x14 | u32  | **group count** | matches the game's `*(table+0x40)` group-count used in `FUN_002e62b0`/`FUN_002e8830` |
| 0x18 | 8*n  | **group table**: `{ u32 dataOffset, u32 tag }` | empirical; `tag & 0xff` = material/texture id, `tag>>16` = 1-based group index |

This is the same group/submesh indexing the runtime uses: `FUN_002e62b0`
decodes a draw request as `group = (flags>>10)&0x7f`, `sub = flags&0x1ff`, indexes
`table[+0x60][group]` then `grp[+0x20][sub]` — confirming a two-level
group→submesh table whose counts are read from the file.

### 2.3 Triangle-strip vertex stream **(empirical)** — the viewable geometry

From `dataOffset[0]` to the trailer is a continuous stream of triangle-strip
runs. Each run:

```
[count:u8] [00] [00]   then  count × 6-byte vertex records
```

6-byte vertex record: `b0 b1 b2 b3 b4 idx`

* `idx` = `(k+1) % count` for the k-th vertex (1,2,…,count-1,0). This column is
  **exact and self-validating** — the loader accepts a run only if it matches,
  so there is no guessing.
* **Position = `(b0, b2, b4)`, UNSIGNED u8** (CORRECTED — was wrongly `(b0,b1,b2)`
  signed, which produced the spiky render). Evidence: per-strip smoothness measured
  on the real sample `km01vnw` (0c064400, 558 verts / 62 strips) — average |Δ|
  between consecutive strip vertices per byte column is
  **b0≈15, b1≈85, b2≈2, b3≈74, b4≈8** (out of 255). Bytes **1 and 3 jump ~⅓ of the
  range every vertex** → they are NOT a smooth spatial axis; using `b1` as the Y
  position injected the spikes. The three columns that behave as coordinates are
  `b0,b2,b4`. `b2` is the cleanest axis and is **always in [2,127]** (never negative)
  — the model's vertical/standing axis (feet near 0), mapped to Y. `b0,b4` cross the
  0x7f/0x80 boundary smoothly (signed |Δ| > unsigned |Δ|) → **unsigned 0..255**, not
  signed. Dropping `b1` as a position axis removes the dominant vertical spikes
  (before/after PNGs in `native/renderer/out/mod_0c064400_shaded.png` vs
  `…_FIXED_shaded.png`).
* `b1,b3` = the two **non-spatial per-vertex attribute bytes** (packed normal / skin
  weight — noisy per vertex, NOT smooth texcoords). Their exact meaning is applied in
  the skinning VU0 microprogram (`vcallms 0x198`, **not** in the extracted VU1 disasm)
  and remains **unresolved**. They are preserved in `DocVertex.raw[]` but no longer
  emitted as UV. **Per-vertex UVs for MOD are NOT in this 6-byte record** — they are a
  separate 16-bit texcoord stream (ITOF12 ÷4096 in the VU, per WS-V). The previous
  provisional UV = `b3/255,b4/255` was wrong (`b4` is a position axis).

Observed `count` is a constant 9 in `km01vnw` (a VU1 batch size). All 62 strips
in `km01vnw` are 9-vertex (the earlier loader started its scan at the first group
`dataOffset` and dropped the 8 strips that precede it — now the whole payload is
scanned, recovering all 62).

**Dequantisation (RESOLVED for position).** The VU converts the integer position
to float with an **ITOF4** op — integer → float with 4 fraction bits, i.e.
**divide by 2⁴ = 16**. Evidence in `port/vu/disasm/program_A.txt` idx 49/51/52
(`ITOF4.xyz vf18,vf11` feeding the `MULAx/MADDAy/MADDAz` matrix transform) and
`program_C.txt` idx 12-14/20-23 (`ITOF4.xyz` for every position stream). A
*different* exponent, **ITOF12** (÷4096), is used for the texcoord stream
(`program_A.txt` idx 50/56/57, `ITOF12.xyw`), confirming ITOF4 is position-only.
The loader applies `MOD_POS_SCALE = 1/16`; the sample bbox becomes ±8 world units
(was ±128 int). The scale is uniform, so it preserves proportions regardless; the
exact exponent of the MOD skinning path (VU0 @0x198) is confirmable against the
oracle, but ITOF4 is the exponent every *visible* VU1 position stream uses.

> NOT universal: only some MODs use this encoding. Others (e.g. `km404az`,
> and all version-16 samples scanned) have a different entry layout
> (strip-table count at +0x14 is 0) and a different, unresolved geometry
> encoding. See §6.

---

## 3. BMD payload — header SOLVED, geometry RESOLVED (V4-32 float)

Spec source: **`FUN_0026abe8`** (header relocation), **`FUN_0026ba88`** (node),
**`FUN_0026bf88`** (bone tree), **`FUN_0026f518`** (geometry leaf).

### 3.1 Header

| off  | type | field | evidence |
|------|------|-------|----------|
| 0x00 | 4    | magic "BMD\0" | bytes |
| 0x04 | u32  | **version** | `FUN_0026abe8` rejects `< 4`; gates features at ≥6, ≥7, ≥8. Seen: 4, 16. |
| 0x08 | u16  | count of the +0x1c array | `FUN_0026abe8` loops `*(u16)(base+0x08)` |
| 0x0e | u16  | count of the +0x14 array | `FUN_0026abe8` loops `*(u16)(base+0x0e)` |
| 0x10 | u32  | offset (relocated) | `*(base+0x10)+=base` |
| 0x14 | u32  | **offset → node array A** | each `+=base`, then `FUN_0026ba88` |
| 0x18 | u32  | offset (relocated if ≠0) | |
| 0x1c | u32  | **offset → node array B** | each `+=base`, then `FUN_0026bc28` |
| 0x20 | u32  | **offset → bone/matrix tree** | `FUN_0026bf88` (8-way recursive hierarchy) |
| 0x18a8/0x18b4/0x18b8 | — | version≥6/≥8 fixed-offset tables | `FUN_0026abe8` |

### 3.2 Nodes & geometry

`FUN_0026ba88` node: `s16 @+0` count of a sub-node array at `+0x08`; `s16 @+0x02`
count of a geometry array at `+0x0c`, each entry → `FUN_0026f518`.
`FUN_0026f518` geometry leaf: `int @+0x04` count, array of pointers at `+0x40`
(the actual vertex/VIF blocks).
`FUN_0026bf88` bone node: 8 child pointers at `+0x80`; a draw-template palette at
`+0xb0` seeded with GIF/VIF constants (`0x1000404`, `0x6c080000`).

The BMD leaf vertex blocks do **not** use the MOD 6-byte int8 strip encoding.
They are **VIF UNPACK V4-32** packets — four 32-bit **floats** per vertex
`(x,y,z, w=1.0)`, already in **world units (no dequantisation)**. This is the
coherent split between the two formats: BMD = static/world geometry stored as
float world positions; MOD = skinned character geometry stored as compact int8
dequantised + skinned by the VU.

Evidence:
* `FUN_0026bf88` seeds the per-bone **draw-template palette** at `+0xb0` with the
  constant **`0x6c080000`** = a VIF UNPACK VIFcode: cmd `0x6c` decodes to
  `unpack, vn=V4, vl=32-bit` → **V4-32**, `NUM=8` (an 8-vertex batch), target VU
  addr 0; preceded by STCYCL `0x01000404` (CL=4, WL=4, contiguous). So each block
  is 8 vertices × 16 bytes of 32-bit components.
* Scanning real BMDs for the self-evident pattern (16-byte stride, 4th float
  exactly `1.0`, xyz finite world magnitudes) yields dense, coherent runs:
  `06080112` → 6 700 vertices / 5 084 triangles, **6.2 % degenerate** (a real
  mesh, not noise); `06010112` (ver 4) → 584 vertices, bbox ±800. Runs cluster at
  ~9 vertices, matching the VU batch size.

### 3.3 file_load_base = 0 — the node graph IS followable (CORRECTED)

The earlier claim that the BMD internal pointers are **absolute EE RAM addresses**
(requiring an unknown load base) was a **misdiagnosis**. The header/node-graph
pointers in the extracted files are plain **file-relative offsets** —
i.e. `file_load_base = 0`. Proof (not a guess):

* Every header pointer is a small in-file value: `+0x14` node-array-A, `+0x1c`
  node-array-B, `+0x20` bone tree all land in `[0, payload_len)` directly
  (e.g. `06080112`: `0x1b54 / 0x48f0 / 0x4e84`, file is `0x132490`).
* A node (`FUN_0026ba88`) is 16 bytes and its sub-node-array pointer at `+0x08`
  points **exactly to node+0x10** — the array physically follows the node. That
  adjacency holds **only** at base 0 (`06080112` nodeA[1]@0x1b90 → sub-array
  0x1ba0). Any non-zero base would break it. So the whole graph is followable:
  `FUN_0026abe8` header → `FUN_0026ba88` node (s16@+0 sub-count → `+0x08` array;
  s16@+2 geo-count → `+0x0c` leaf array) → `FUN_0026f518` leaf (`int@+4` count,
  **pointer array at `+0x40`**) → vertex blocks. All pointers validated in-file,
  tree consistent. (Stale out-of-range values appear only in fields of nodes whose
  count is 0, which the parser never dereferences — consistent with base 0.)

**Draw-object layout (confirmed from the bytes).** A node's `+0x08` sub-array
entries point at **draw-objects**, each laid out as:
`{ N × V4-32 float verts ; trailer: u32 vertCount, a self-pointer back to the
vertex buffer, STCYCL 0x01000404, UNPACK 0x6c (V4-32) … }`. The sub-array
pointers land **exactly** on the vertex-buffer starts (verified: the 6 sub-pointers
of `06080112` nodeA[2] == the first 6 V4-32 run offsets `0x25e0,0x2720,…`).

**Why the loader still scans (honest scope).** node-array-A alone reaches only a
minority of the mesh; the **complete** draw traversal also runs through
node-array-B (`FUN_0026bc28`, references node-array-A by index) and the 8-way bone
tree's draw-template arrays (`FUN_0026bf88` `+0xb0`). That full path is only partly
reversed, so the loader keeps recovering vertex blocks by the self-validating
V4-32/`w=1` pattern over the whole payload (this gets the bulk of the coherent
geometry). The graph work above is what nails the draw-object boundaries and
confirms the encoding — it is not yet a full replacement for the scan.

**Needle artifacts — ROOT CAUSE FOUND & FIXED.** Between VIF vertex sub-blocks the
stream is padded with **`(0,0,0, w=1.0)` quadwords** (the unwritten tail of a
fixed-size VU batch — e.g. 4 real verts + 10 zero quads). The old scanner accepted
those zeros as vertices, so the triangle-strip bridged real geometry to the origin
and back — the long thin spikes that converged to a point at `(0,0,0)` (gallery
cases `068d0912`, `06c90512`). **Fix:** a `(0,0,0)` position quad is a strip
**separator**, not a vertex. This removes every origin-bridging spike while
preserving the real vertices (`068d0912`: 7333 → 7293 verts, only the 40 padding
quads dropped; origin-verts-in-strips 40 → 0; `06c90512`: 7567 → 7507, 60 → 0).
Re-rendered: `068d0912` goes from a starburst of needles to a coherent wall/alcove;
`06c90512` from a huge left spike to a clean layered platform; `06080112`/`06010112`
unchanged (they had no zero-pad needles). Exact per-leaf primitive/winding flags
remain an oracle item; connectivity is still taken as a triangle-strip per run.

### 3.4 BMD per-vertex UV + baked vertex colour — RESOLVED (textured environments)

The V4-32 float runs in §3.2/§3.3 are only ONE of the two position
representations a BMD ships, and they carry **no** per-vertex attributes — which
is why the environment rendered coherent but **grey**. The **textured** mesh is a
separate, fully-attributed VIF DMA chain. Reversed by walking the real draw
stream of rooms `06960212` (z134/z150 gallery) and `068f0612` (round chamber),
against the draw-template palette `FUN_0026bf88` seeds at `+0xb0`
(`STCYCL 0x01000404`, `UNPACK 0x6c080000`).

**One draw packet = one submesh**, laid out (verified byte-exact):

```
STCYCL cl=4 wl=4
UNPACK V4-32 num=2 @addr0        GIFtag + PRIM/TEX0 template (2 qwords).
                                 giftag.PRIM type = 3-bit 5 (tri), TME=1 (textured),
                                 IIP/FGE=1, bit6 ABE=1 on the translucent pass;
                                 giftag qword1 word0 = 0x8000 | VERTEX_COUNT.
                                 **CORRECTION (2026-10-16, proven):** this field is
                                 NOT a material selector. It is the per-submesh
                                 VERTEX COUNT with VU flag bit 0x8000 set. Probe on
                                 z128 chunk 06800212 (tools scratchpad probe_giftag.py)
                                 found `qword1.x & 0x7fff == unpacked V3-16 num` for
                                 ALL 1273 packets. The "16 distinct values 0x03..0x20"
                                 earlier read as material IDs are simply the 16
                                 distinct vertex counts (3,4,6,8,9,12,15,16,18,20,21,
                                 24,27,28,30,32) — they cluster small because the room
                                 is tessellated into tiny quad/tri submeshes, which is
                                 what made them look like material indices. THERE IS NO
                                 PER-SUBMESH MATERIAL/TEXTURE SELECTOR IN THE VIF DRAW
                                 CHAIN. See the corrected "Texture binding" note below.
STCYCL cl=4 wl=1 ; STROW b0 b1 b2 0 ; STMOD 2            (difference/accumulate)
UNPACK V3-16 num=N @addr2        POSITION, int16, DELTA-encoded. VIF STMOD=2
                                 ("difference"): out[k] = out[k-1] + raw[k], with
                                 out[-1] = STROW. world = out / 16 (ITOF4). This
                                 reconstructs the IDENTICAL room bbox as the V4-32
                                 float runs (06960212: X[109.9,315.0] Y[-60,90.2]
                                 Z[120,370]) -> same geometry, now with attributes.
STMOD 0 ; UNPACK V4-8  num=N @addr3   RGBA baked PRELIT vertex colour (A=0x80 GS-neutral).
          UNPACK V2-16 num=N @addr4   UV, int16; texcoord = raw / 4096 (ITOF12),
                                      tiling (values run outside [0,1] -> GS wrap).
MSCNT
```

Position / colour / UV are all `num=N`, **1:1 per vertex** and self-validating:
`06960212` locks **363/363 packets, 0 mismatch, 7491 verts**; `068f0612`
**691 packets, 12707 tris**. Connectivity is the triangle STRIP per packet (same
alternating winding the OBJ/scanner path uses). PS2 GS MODULATE doubles the
colour (`128/255` == neutral), so the renderer uses `rgba/255` with the `×2`
combine.

**Texture binding — CORRECTED (2026-10-16).** The earlier claim that a "per-submesh
selector is real and on-disk (giftag)" is **WRONG** (see the giftag correction
above: that field is the vertex count). There is **no** material/texture tag in the
VIF draw packets. The real binding is entirely runtime, via TWO structures neither
of which the per-submesh geometry packet carries:

1. The **group table** `*(model+0x60)` (count `*(u16)(model+0x40)`), indexed by a
   draw-request word: `group = (word>>10)&0x7f`, `sub = word & 0x1ff`
   (`FUN_002e62b0`). Each group holds its **material selector at `+8`** (u16),
   its sub-count at `+6`, and a sub-pointer array at `+0x20` (the geometry). So the
   material is a property of the GROUP, not of the geometry submesh — and groups are
   NOT the per-packet vertex-count buckets the exporter currently builds.
2. The **texture-handle table** `context+0x24` (built at TXD-load time, §6), which
   maps that group material selector → the actual FileCode atlas slot.

Consequence for `export_bmd_textured.py`: its 16 "material groups" are vertex-count
buckets (grouping key = `qw1.x & 0xff`, the vertex count), NOT materials. So its
texture binding was wrong on two independent counts — (a) the groups are not
materials, and (b) even per group the atlas is picked by a dominant-order guess.
GROUND TRUTH (GS dump of z128/Kalm, `tools/gsdump/`): across all 13 Kalm chunks the
dominant-order guess binds **0 of 9** actually-drawn room atlases
(`native/assets/models/out/z128_dump03_binding_oracle.json`). UVs, baked colours and
the per-packet geometry remain EXACT decoded data — only the grouping and the
texture assignment are unresolved, and both now have a clear (runtime-table or
GS-dump-oracle) recovery path.

The room's textures are the sibling TXD containers **`<level>ffNN.bin`** (e.g.
`0696ff00..0696ffdf` → 224 `z134_*/z150_*` zone atlases; z128 = `0680ff*`), all
decoded via `native/assets/textures/libdtex` — texture EXTRACTION is validated
pixel-identical vs the GS dump; only the submesh→atlas BINDING is open.

Decoder/exporter: **`export_bmd_textured.py`** (this dir) → one DMSH mesh per
material group + `*_manifest.json` + a `*.list` sidecar. Renderer:
**`native/gpu/render_env_textured.c`** (one `rgl_scene_draw` per group, PRELIT +
MODULATE + its TXD texture, one GL world). Textured result (walls/floors with real
z134/z150 panel art, blue console lights, metallic/rust surfaces — not grey):
`native/gpu/out/19_environment_textured_*.png` (baked-colour PRELIT) and
`…_raw_*.png` (neutral colour → raw texture), plus `068f0612` as `20_env_*`.

---

## 4. Loader & exporter (this directory)

* `doc_model.h/.c` — RFD unwrap + BMD/MOD header parse + self-validating strip
  scanner → in-memory `DocModel` (objects → strips → vertices, with material tag).
* `export_obj.c` — Wavefront OBJ (strips → triangle list, alternating winding).
* `export_gltf.c` — glTF 2.0 (`.gltf` + `.bin`), one primitive per strip.
* `main.c`, `Makefile`. Build: `make`. Run: `./doc_model <container.bin> out/name`.

All reads are bounds-checked; files are treated as untrusted data (never executed).

---

## 5. Validation (real files)

`./doc_model extract/_KEL/noPath/0c064400.bin out/evm000` (MOD, version 19,
`km01vnw`):

| metric | value |
|--------|-------|
| objects | 1 |
| strips (triangle-strips) | 62 |
| vertices | 558 |
| triangles | 434 |
| bbox (world units, ÷16) | min (-8, -7.94, 0.125) · max (7.94, 7.94, 7.94) |
| UV range (provisional) | u[0..1] v[0..1] (varies per-vertex) |

Positions are now dequantised (ITOF4 ÷16) → a ±8-unit mesh. All 62 nine-vertex
strips are recovered (the previous 54 missed the 8 strips before the first group
`dataOffset`). glTF validated: well-formed JSON, 62 primitives / 186 accessors,
buffer byte-length consistent with all buffer-views.

**BMD geometry now exports** (was 0). V4-32 float vertices:

| file | ver | strips | verts | tris | bbox |
|------|-----|--------|-------|------|------|
| `06080112` (1.25 MB) | 16 | 808 | 6 700 | 5 084 | x[-800,1078] y[-429,10] z[-1081,800] |
| `06010112` (181 KB)  | 4  | 69  | 584   | 446  | ±800 (y −200..0) |

`06080112` is 6.2 % degenerate — coherent world mesh. Both export valid OBJ +
glTF.

---

## 6. Material → texture binding (reversed; runtime-trace confirmed)

How a submesh picks its texture, end to end (all addrs in `analysis/decomp/`):

1. A draw-request word is decoded by **`FUN_002e62b0`** (batch sibling
   `FUN_002e8830`): `group = (word>>10) & 0x7f`, `sub = word & 0x1ff`. The group
   is bounds-checked against `*(u16)(model+0x40)` and indexed into the group
   table at `*(model+0x60)`; the submesh into `group+0x20`. **`*(u16)(group+8)`
   is the per-group material selector.**
2. The material descriptor holds **layer-tag lists at `+0x24`** (up to 12),
   decoded by **`FUN_002ebc48`**. Each tag is a `u16`; its **high nibble is a
   mode**:
   * mode 0 → the low 12 bits index the per-model **texture-handle table at
     `context+0x24`**;
   * mode 8 → `tag & 0xff` is a texture/palette id (shared path).
3. **`FUN_002ebb18`** resolves id → handle: `*(context + (tag & 0xffff0fff)*4 +
   0x24)`, following a `handle+0x54` override if present.
4. **`FUN_0030ab10`** packs the handle into a GS **TEX0** register: `TBW` ←
   `handle+0x32`, `PSM` ← `+0x33`, `TW/TH` ← ceil-log2 of `+0x3c`/`+0x3e`, `CBP`
   ← CLUT alloc, `CPSM` ← `+0x47`; the handle's EE source pointers are `+0x2c`
   (texels) and `+0x40` (CLUT). **TBP0 is a transient GS-VRAM page**, assigned at
   bind time — *not* a FileCode; the stable identity is the `context+0x24` slot.

So a renderer needs, per submesh: `material = (group+8)` → layer tag → slot in
the per-model texture table → the TXD (rst/tim2) entry loaded into that slot. The
loader exposes `DocStrip.material`/`.group` as the file-side hint (see §2.2); the
**slot → FileCode** mapping is established when the TXD is loaded and is the one
piece best confirmed from the oracle (dump `context+0x24[]` vs the TXD FileCode
list).

> The on-disk 32-bit group tag's `tag>>16` (1-based group index) is a
> **loader-side** field; the runtime takes the group index from the draw-request
> word, not from `>>16` of a material tag. Treat `DocStrip.group` as a hint.

---

## 7. Open unknowns (next to resolve)

1. **Attribute bytes `b1,b3` (MOD)** — the two NON-spatial bytes of the int8 record
   (§2.3). Confirmed empirically: within a single strip `b0,b2,b4` vary **smoothly**
   (true ribbon-strip coordinates) while `b1,b3` jump wildly per vertex (packed
   normal / weight). **VU0 micromem 0x198 is now DISASSEMBLED** — see
   `port/vu/disasm/vu0_skin_micro.txt`. The resident VU0 micro image is the blob
   previously filed as "VU1 program C" (`port/vu/microcode/program_C.bin`): every
   `_vcallms(imm)` in the skinning code lands on a clean program entry in it, and it
   has **no XGKICK** (VU0 cannot kick to GS). The per-vertex program at **0x120**
   dequantises the position/second stream with **ITOF12 (÷4096)** and emits exactly
   **two** output quadwords per vertex — `vf15` (transformed POSITION) and `vf16`
   (transformed NORMAL). **There is no UV lane in the VU0 skin output**: per-vertex
   UV/colour is carried by a separate stream merged later in the GS A+D packet, not
   by `b1,b3` and not by the VU0 skin program. So MOD UVs remain an oracle item, but
   the skin math and the position dequant are now resolved from static disasm.
2. **MOD non-strip / V3-16 variant** — `km404az` and the version-16 MODs use the
   `FUN_005b1260` V3-16 (int16) position path, not the int8 self-indexed strip
   format reversed here. Reverse that entry layout + control records
   (`psVar19`: `s16[0]` bone idx, `byte[2]/[3]` sub-strip counts, `s16[2]` QWC,
   `s16[3]` stride).
3. **BMD primitive/winding flags** — vertices recovered (V4-32 float); the
   per-leaf strip/list primitive flags and exact index order are not yet pinned.
4. **MOD strip → material-group mapping** — the 9-group table's `dataOffset`s do
   not line up with strip starts; which strips belong to which group is
   unresolved from the file (needs the runtime draw-request stream / oracle).
5. **Skinning / bind pose (MOD) — BIGGEST remaining gap, blocks a fully clean
   character.** After the position-byte fix (§2.3) the dominant vertical spikes are
   gone, but ~⅓ of the strips (20/62 in `km01vnw`; worse on the dense body mesh
   `km01vnw2`, 2 436 tris) still sit in the wrong place → residual shards. Cause is
   now understood from **`FUN_005b1260`** (the skinning emitter; sibling selector
   `FUN_005b2240`: skinned path taken when bone count `*(obj+0x10) ≥ 1`):

   * The vertex stream is split into per-strip **control records** (8 B each):
     `{ s16 boneIndex, u8 cntA@+2, u8 cntB@+3, s16 qwc@+4, s16 stride@+6 }`.
     `cntA+cntB` = the batch's vertex count (the VIF `NUM`), `qwc`/`stride` drive the
     VIF0 DMA of that batch's raw bytes.
   * For each batch the code loads **one 0x40-byte (4×4) bone matrix**
     `*(param_2+0x20) + boneIndex*0x40`, then `_lqc2 ×4; _vcallms(0x198)` — i.e. the
     VU0 microprogram at micromem **0x198** rigid-transforms that batch's int8
     positions by that single bone's matrix. So the on-disk positions are in
     **bone-LOCAL space**; the posed mesh is assembled only after each batch is
     multiplied by its bone matrix.
   * **The bind matrices are NOT in the MOD file, and NOT in a sibling resource.**
     The earlier lead — "the skeleton is an `.skd`/`.smd` sibling entry in the same
     RFD" — is **DISPROVEN**. A scan of every character container in
     `extract/_KEL/noPath/` shows **no `skd`/`smd`/`skeleton` entry anywhere**: all
     2 094 character RFDs hold only `chi` (an 18-byte `chrid`) + one or more `mod`
     entries (`evm000` = `chrid.chi`, `km01vnw.mod`, `km01vnw2.mod`, `km0102vn.mod`,
     `km0103vn.mod`). `FUN_005a7560`/`FUN_005a7390` (FourCC `0x646f6d` = "mod") just
     resolve a composite character's sub-`mod` parts — they are not a skeleton loader.
   * `*(param_2+0x20)` is a **runtime** bone-matrix palette, allocated per instance
     and filled by the animation/skeleton subsystem. `FUN_005b2550` calls
     `FUN_005b0d30(blend, frame, model, ctx)` each frame to build it; the per-bone
     data lives in the model object at `model+0x24` (count `model+0x10`).
     `FUN_005b0b38/09f8/0c50` are a verlet soft-body pass over that data, not a plain
     rest-matrix compose.
   * **VERIFIED EMPIRICALLY (this run): the bind matrices are NOT in the file, and
     `model+0x24` is a RUNTIME struct, not file data.** Two independent checks on the
     real character container `0c064400` (`km01vnw` + 3 sibling mod parts +
     `chrid.chi`, 135 KB total):
     1. **Whole-container 4×4-matrix scan = 0 hits.** Scanning every 4-byte offset for
        an affine skin matrix (upper-3×3 finite, homogeneous `(0,0,0,1)` tail) in
        **either** row-form **or** column-form (the VU0 skin uses col-4 translation)
        finds **zero** matrix runs anywhere in the file. There is no rest/bind-matrix
        block to recover.
     2. **`model+0x24` is not a file field.** Unlike BMD (which turned out to be
        file-relative, base 0 — §3.3), the MOD runtime model object (`+0x0a` draw
        count, `+0x10` bone count, `+0x1c` matrix base, `+0x20` draw array, `+0x24`
        bone-pointer array; read by `FUN_005b2550`/`FUN_005b0d30`) is **constructed at
        runtime** and is *not* the MOD file top-entry. `FUN_005aecf8` relocates only
        the payload's entry array (`+0x14`) and trailer (`+0x24`); it never builds a
        `+0x20/+0x24` bone block. Reading the file at the alleged `model+0x24` yields
        garbage out-of-bounds offsets (bone count 30890, `+0x24` = 0x2fe28 in a 0x1550
        payload). So there is nothing in the file to rebase — the rebasing lead is a
        dead end for MOD, *because the data is genuinely runtime-only*, not because of
        an unknown base. The `chrid.chi` entry is an 18-byte parts manifest
        (part names + sizes), not a skeleton. **Conclusion: the scoped oracle is the
        only path to the bind pose; this is now proven, not assumed.**
   * So the bind pose needs ONE of: **(a)** reverse the full runtime model-object bone
     block + `FUN_005b0d30` compose (large; the data is runtime-built, not in the file); or
     **(b)** the **oracle** — but the parked recomp build halts at the game loop and
     never loads a character (the custom IOP-FS RPC that streams `evm###` is the
     parked M1 blocker), so it does not currently exercise this path either. A scoped
     oracle would call the recompiled `FUN_005b0d30`/skin funcs directly on a
     relocated `km01vnw` buffer and dump `*(ctx+0x20)`.
   * The per-strip `boneIndex` IS on disk (in the control records), but the control
     records are **interleaved** with the int8 strips and a second int16 (V3-16)
     position stream (see §7.2), and the strip→control mapping is not yet cleanly
     separated, so even static strip-by-bone grouping is not yet recovered.
   * BMD has its own `+0x20` 8-way bone tree (static world geometry, not the gap here).

The reliable anchor for the remaining items is a **scoped oracle**: relocate one
`km01vnw` buffer in the recomp runtime, invoke the recompiled skin path, and dump the
`*(ctx+0x20)` matrices + VU0 I/O — then bake `boneMatrix · (pos/4096·meshScale)` per
batch. The skin math itself (what to do with the matrices) is now fully known
(`port/vu/disasm/vu0_skin_micro.txt`); only the matrices are missing.

> **UPDATE (§8): the bind matrices are now RECOVERED — statically, no oracle.**
> The skeleton is runtime-built *for `km01vnw`* only because that container ships
> no skeleton. The **event containers `extract/_KEL/noPath/0765*.bin` DO ship a
> `skeleton.skd` chunk**, and it is a clean authored rest skeleton. See §8:
> `skd_skeleton.py` parses it and composes real per-bone world bind matrices
> (validated — a recognizable humanoid: `native/renderer/out/skel_e000_*.png`).

---

## 8. Character skeleton `skd` ("SK2") — bind pose RESOLVED (Route (a), static)

The §7.5 claim "the bind pose is genuinely runtime-only" is true **only for
`km01vnw`'s container**, which ships no skeleton. A whole-tree scan of the RFD
containers (parsing each entry's ext int at entry+0x08) shows the skeletons live
in **separate event containers**: `extract/_KEL/noPath/0765*.bin` (named
`e000`..), 316 of them, each carrying `texanm.tad` + `marker.mkd` + one or more
`e###_*.smd` meshes + **`skeleton.skd`**. No container holds *both* `skd` and
`mod`; a complete character = `skd` (here) ∪ its skinned mesh (`smd`, same
container). (Note: the fourCC the instantiation chain `FUN_005a8850` reads as
`0x646174` is **"tad"**, not "dat" — FORMAT.md §1's "dat" row is mislabelled;
little-endian `0x646174` = `t,a,d`.)

### 8.1 skd layout — reversed from `FUN_005b27c8` / `FUN_005b2c78` / `FUN_005a8850`

Offsets relative to the RFD-entry **payload** (= entry + 0x10):

| off | type | field | evidence |
|-----|------|-------|----------|
| 0x00 | u32 | magic **"SK2\0"** (0x00324b53) | bytes |
| 0x04 | u8 | version/flag (seen 3) | `FUN_005b27c8` tests `+2=='D'` variant |
| 0x08 | s16 | group count (named `smd` sub-meshes) | `FUN_005b2c88` loops it |
| 0x0c | u32 | → ASCII group-name table (`e000_L01`…`e000_S02b`) | reloc |
| 0x10 | u32 | → group-pointer array (runtime-filled) | `FUN_005b27c8` reloc |
| 0x14 | **s32** | **BONE COUNT** | `FUN_005b2c78` returns `count<<6` = palette bytes ⇒ one **0x40 (4×4 f32)** matrix/bone |
| 0x18 | u32 | → **bone-node pointer array** (boneCount × u32) | `FUN_005b27c8` reloc loop over `+0x14` |

**Bone node** (208 = 0xd0 bytes). The authored rest transform + hierarchy:

| off | type | field | notes |
|-----|------|-------|-------|
| 0x20 | f32[3] | **LOCAL translation** (xyz) | bone offset in **parent** frame (= limb length along local X; e.g. forearm `(2.606,0,0)`). `FUN_005b27c8` scales a copy by **×256** → world units |
| 0x30 | f32[4] | **LOCAL rotation QUATERNION** (x,y,z,w), unit | ‖q‖=1 verified |
| 0xc0 | **s16** | **PARENT bone index** (−1 = root) | recovered by tree-validity scan: across all bones it is the **only** field giving exactly one root + an acyclic tree with ~99 % parents < index |

The node's pointer fields (+0x40.., +0x7c self-ptr, +0xc8) hold **stale EE-RAM
addresses** (the files are RAM snapshots) — they are *not* usable statically; the
clean static data is (T, Q, parent) above.

### 8.2 Composition (what the skin consumes)

```
local[i]       = Translate(T_i · 256) · Rotate(Q_i)          (column-major, child→parent)
world_bind[i]  = world_bind[parent[i]] · local[i]           (parent chain; root = local)
skin:  world_pos = world_bind[boneIndex] · (posInt/4096 · meshScale, 1)   (§7.5, VU0 @0x198)
```

`skd_skeleton.py` (this dir) implements exactly this and emits
`out/skeleton_bind.json` (per-bone parent + 4×4 world matrix). **Validated**: the
103-bone `e000` skeleton (`07650001.bin`) composes to a symmetric **upright
humanoid** — head (dense facial/hair bone cluster), neck, two shoulders→arms with
finger bones, spine, pelvis, two legs; hands land symmetric at x≈±1670. Rendered
shaded to `native/renderer/out/skel_e000_front.png` / `_3q.png`. The parser
generalises: 58/76/98/147-bone skeletons all recover with one root and a clean
hierarchical tree.

### 8.3 Skinned mesh `smd` ("SM2") — RESOLVED (posed character rendered)

Reversed from the game's own parsers: **`FUN_005b0470`** (smd relocation,
dispatch key `0x646d73`), **`FUN_005b0590`** (per-group relocation),
**`FUN_005b06d0`** (weighted-skin emitter — the decisive one), **`FUN_005b1190`**
(per-vertex VU loop; `cnt = u8@+2 + u8@+3`), **`FUN_005b1260`** (sibling MOD int8
strip emitter, same control-record shape). Parser: `render_skinned.py` (this dir);
posed render: `native/renderer/out/char_e000_skinned{,_3q,_side}.png`.

**smd payload header** (offsets rel. RFD-entry payload = entry+0x10):

| off | type | field | evidence |
|-----|------|-------|----------|
| 0x00 | u32 | magic **"SM2\0"** (0x00324d53) | bytes |
| 0x04 | u8 | version (seen 4; draw needs ≥4) | `FUN_005b0470`/`FUN_005b06d0` test `+4` |
| 0x0a | u16 | **group count** | `FUN_005b0470` loops it over the `+0x20` array |
| 0x20 | u32 | → **group-pointer array** (groupCount × u32, file-rel) | `FUN_005b0470` reloc |
| 0x2c | f32 | **meshScale** (e.g. 1.7705) | `FUN_005b06d0`: `*(f32)(+0x2c) * 1/4096` → VU vf31 |

**Group object** (pointed to by the `+0x20` array; one per sub-mesh):

| off | type | field |
|-----|------|-------|
| 0x08 | u32 | → VIF **draw/attribute sub-array** (colour+uv+strip-index packets); = end of this group's geometry stream |
| 0x20 | s16 | **batch count** (= number of control records) |
| 0x30 | — | **start of the geometry VIF stream** (header is 0x30 bytes) |

**Geometry VIF stream** (per batch, starting at group+0x30). Each batch:
`STMASK 0xc0c0c0c0` (masks the `w` lane → set by STROW) · `STCYCL cl=2 wl=1` ·
`UNPACK V1-32 num=N @0` (N f32 scalars — per-vertex GS `q`/intensity, 0..1) ·
`UNPACK V4-8 num=N @1` (N × **int8×4 = NORMAL**) ·
`UNPACK V3-16(m) num=N @0` (N × **int16×3 = POSITION**, bone-local). `N` = batch
vertex count. (The `(122,122,122,128)` colour and the `V2-16` **UV** + `S-8`
strip-index planes live in the *separate* `+0x08` draw packets, STCYCL cl4/wl4 —
they drive the GS prim, not the skin; UV binding is left as an oracle item.)

**Control-record table** follows the geometry stream (4-byte aligned),
`batchCount` records × **8 bytes**, 1:1 with the batches in order — identical shape
to the MOD path (`FUN_005b1260` §7.5):

```
{ s16 boneIndex, u8 cntA@+2, u8 cntB@+3, s16 qwc@+4, s16 stride@+6 }
cnt = cntA + cntB  == the batch's VIF NUM   (self-validating; used to lock alignment)
```

**Skin (per batch, single rigid bone — what VU0 @0x198/0x120 computes):**

```
world_pos = world_bind[boneIndex] · ( posInt16 · meshScale/4096 ,  1 )   (§8.2)
```

Connectivity: each batch = a triangle **strip** over its `cntA` verts then another
over its `cntB` verts (`FUN_005b1190`'s two loops).

**Result (char e000, `07650001.bin`, 3 smd entries / 34 groups):** 227/381 batches
lock to a valid control record (bone ∈ [0,103), `cntA+cntB == NUM`) and skin to a
**coherent upright humanoid** — head+hair, two outstretched arms, torso, pelvis,
two legs, correctly 3-D in the 3⁄4 view. `~3.5 k` posed vertices, `~1.8 k`
triangles after strip-bridge culling (bbox ≈ ±7 world units).

**Honest limits (as of §8.3).** (1) ~⅓ strip-bridge shards; (2) ~40 % of batches
dropped; (3) no UV/texture. **All three are RESOLVED in §8.4 below.**

### 8.4 Connectivity (S-8 index pool) + UV + texture — RESOLVED

Parser: `render_skinned.py` (rewritten, this dir). Renders:
`native/renderer/out/char_e000_{clean,textured}{,_3q,_side}.png`. Reversed from
`FUN_005b1260` (draw submit), `FUN_005b1190` (VU emit loop) and
`port/vu/disasm/vu0_skin_micro.txt` (the resident VU0 skin image).

**(1) Exact control table — fixes the 40 % batch drop.** The §8.3 "scan for the
table" heuristic was wrong. Each group's control struct is pointed to **directly**
by `*(u32)(group+0x0c)` (= `param_1[3]` in `FUN_005b1260`), file-relative to the
smd payload. That struct `piVar2` is `{ int count@0, _, int streamOff@8, int
rawOff@c }`: the 8-byte control records start at `piVar2 + piVar2[2]`, the raw VIF
batch bytes at `piVar2 + piVar2[3]` (= `group+0x30`). `count == group+0x20`
batch count. This locks **100 %** of batches (was ~60 %); no edge-culling needed.

**(2) The posed vertex pool is SCATTER-indexed, not batch-sequential.** The VU0
per-vertex program (entry `0x120`) writes each transformed vertex to output pool
slot `(normalW + 0x80) & 0xff`, where `normalW` is the **w (4th) byte of the
vertex's `V4-8` normal plane** — `ILW.w vi03, 0x3(vi01)` in the disasm, `+0x80`
double-buffer base (idx 30/32), `& 0xff` (idx 33). Boundary verts shared by
adjacent bone batches collapse onto the **same slot** (that is the skin weld). For
char e000 the 404 batch-records of a group resolve to a **245-slot pool**, and
`max(slot)=244` matches the draw index max exactly — the decisive confirmation.

**(3) Connectivity + UV live in the `+0x08` draw packets.** `group+0x08` points to
a **self-terminating array of draw-descriptor offsets** (stop at the first entry
whose `u16@+2 != 0xffff`). Each descriptor = `{ u16 nSub, 0xffff, u32 vtxCount16,
0, 0, u32 vifPtr }`. Its VIF packets (`STCYCL cl4/wl4`) are a run of **sub-packets**,
each: `UNPACK V4-32 num=2 @0` (GIFtag + PRIM template), `UNPACK V4-8 num=24 @3`
(RGBA colour, `0x7a7a7a80` = 122,122,122,128), `UNPACK V2-16 num=24 @2` (**UV**),
`UNPACK V1-8 num=24 @2` (**S-8 pool index**), `MSCNT`. The 24 S-8 indices form a GS
**triangle strip** into the posed pool (repeated index = degenerate = restart;
parity flip on odd steps). **UV = `V2-16 / 4096`** → verified `[0.001, 0.999]`.

**(4) Texture.** The event container ships only `texanm.tad` — the `_eye` blink
anim (32×16 PSMT8), NOT the body. The body atlas is the **sibling texture
container `0765xxx2.bin`** (here `07650002.bin`): an **RST 512×256 PSMT8** decoded
by `native/assets/textures/libdtex` — a classic DoC character sheet (skin torso,
face w/ blue eyes, grey hair-strand cards, white+blue trousers, a bladed gun).
The smd UVs index straight into it; `render_skinned.py` auto-resolves it.

**Result (char e000).** 381/381 batches lock (100 %). ~3.0 k pooled verts, 5.5 k
triangles, **only 5.8 % shardy** (edge > 2 world-u) vs ~⅓ before — a **solid,
continuous, correctly-3-D body**: both outstretched arms, torso, shoulders, neck,
pelvis, legs, the belt + sheathed gun, all clean and **textured** (flesh arms, the
striped trousers, the gun). Residual shards are confined to the **hair** (dense
L01 group 0, Y≈12, 22 %) and two tiny face-detail S01 groups — the hair is authored
as many thin intersecting strand-cards (visible in the atlas) and a few distinct
tips collide on the 245-slot `normalW` scatter space; this is the one remaining
hard case. Backface culling does *not* help (strip winding is not globally
consistent across groups). The signature milestone — **a clean, textured DoC
character rendered natively** — is met.

---

## 9. Skeletal ANIMATION — motion-clip format RESOLVED (character moves)

The static bind pose (§8) is now driven by real per-bone keyframe tracks. Format
reversed from the game's own EE sampler/composer (`analysis/decomp/`); evaluator
+ renderer = `anim_eval.py` (this dir). Renders:
`native/renderer/out/14_vincent_{bind,anim_f###}*.png` (+ `/srv/shared/dirge-port/`).

### 9.1 Where motion lives — the clip is a `mod` chunk, VERSION 0x10

RFD ext `"mod"` (0x646f6d) covers TWO payloads, split by `*(u32)(payload+0x04)`:
- **version 0x13** (14090 of them) = **MESH** (the §7 character models).
- **version 0x10** (519 of them) = **MOTION CLIP**. (0x11/0x12 are rare mesh
  variants.) Proven: for every v0x10 chunk `segCount == ceil(frameCount/255)`.

Clips are bound to a character at runtime via motion handles (`char+0x3bc` ×8,
`char+0x3c8` ×2) resolved by `FUN_005a7390` (`FUN_00258e40(res,0x646f6d,..)`);
playback cursor/weight/loop live in the layer array at `char+0x380` (stride 0x18,
cursor at `+0x388`), driven by `FUN_005a9db0` → `FUN_005afa30/FUN_005afa58`.

### 9.2 Clip payload header (offsets rel. RFD-entry payload = entry+0x10)

| off | type | field | evidence |
|-----|------|-------|----------|
| 0x00 | u32 | magic "MOD\0" | bytes |
| 0x04 | u32 | **version 0x10** = motion clip | `FUN_005aecf8`/`FUN_005afa58` gate `>0xf` |
| 0x08 | u32 | **animated bone count** (== target skd bone count) | segment walk is exact only at this count |
| 0x0c | u32 | **total frame count** | `FUN_005af8f8` reads `+0xc` |
| 0x10 | u32 | **segment count** (= ceil(frames/255)) | `FUN_005af8f8` `+0x10` |
| 0x14 | u32 | → **segment-ptr table** (segCount × u32, file-rel) | `FUN_005af8f8` `+0x14` |

`FUN_005af8f8`: `seg = frame/255; localFrame = frame - seg*255;` returns
`*(segTable + seg*4)` = that segment's packed key stream (≤255 frames).

### 9.3 Per-bone track layout (stream order) — `FUN_005afa58`/`FUN_005af8a0`

Per bone, in order (this exact shape makes each 255-frame segment walk consume
with ZERO leftover bytes — the self-validating lock):

1. **TRACK P1** (translation / root motion, SPARSE — ~6 bones): `s16 count C`;
   if `(C&0x8000)` keys are 7×s16 (14 B) else 4×s16 (8 B) = `[time,x,y,z]`.
2. **TRACK P2** (translation aux): same encoding; almost always count 0.
3. **TRACK Q** (ROTATION — present on ALL bones; THIS is the pose): `s16 count C`;
   keys are 3×s16 = **6 bytes** each, packed sign+magnitude.

Sampling is **LINEAR** between the two keys bracketing `localFrame`
(`FUN_005aefd0`/`FUN_005af6c0`: factor `(f-t0)/(t1-t0)`); quat path renormalizes
(nlerp). The final quat-normalize + quat→matrix + parent-multiply run in **VU
microprogram entry 0x218** (not in C).

### 9.4 Rotation key (TRACK Q, 6 bytes) → quaternion — `FUN_005af618`

```
byte0 = time ; byte1 = y_hi ; u16@2 = (z_mag<<3)|signbits ; u16@4 = (x_mag<<4)|y_lo
x_mag = u16@4 >> 4            (12b)   sign = bit0 of u16@2
y_mag = (byte1<<4)|(u16@4 & 0xf)(12b) sign = bit1 of u16@2
z_mag = u16@2 >> 3            (13b)   sign = bit2 of u16@2
q = normalize( sx*x_mag, sy*y_mag, sz*z_mag, 4096 )   # 4th (w) = implicit unit 4096
```
Verified: bone0 `(4095,0,0,4096)` → `(.707,0,0,.707)` = a clean 90° root-axis
rotation; every bone yields a UNIT quaternion by construction.

### 9.5 Composition + skin — IDENTICAL to the bind path (§8.2)

```
local[i] = Translate(T_i) . Rotate(Q_anim_i)    (T stays at bind — bone lengths fixed)
world[i] = world[parent[i]] . local[i]
skin:  world_pos = world[boneIndex] . (posInt/4096 . meshScale, 1)
```
Runtime composer = **`FUN_005affb0`** (hierarchy walk; parent matrix at
`(skd+0x20)+parent*0x40`) + **`FUN_005b0200`** (`FUN_00262de0` quat→matrix,
`FUN_00444c88` insert T, `FUN_00444a40` 4×4·4×4). `FUN_005b0380` = reset-to-bind
(`node+0x20 = node+0x40 /256 ; node+0x30 = node+0x50`). NOTE: `FUN_005b2550`→
`FUN_005b0d30` (`FUN_005b0b38/09f8/0c50`) is a SEPARATE **cloth/verlet** pass at
skin time (distance-constraint relaxation), NOT the keyframe sampler — the task's
`+0x34` lead is the cloth anchor (`char+0x44`), not the clip pointer.

### 9.6 Result + honest blocker

`anim_eval.py` decodes clip `km0102vn` (504 f / 2 seg / 93 bones) onto the
93-bone Vincent event skeleton `07700301` (`p012` mesh) and renders frames
0/120/250: **all 92 non-root bones animate with real per-frame variation; the
pose is a coherent humanoid clearly DIFFERENT from bind** (dynamic action stance
vs the bind crouch) — the character moves, from real decoded keyframe data.

Two honest caveats, documented not hidden:
1. **e000's own 103-bone clip is NOT in the static extract.** No v0x10 clip has
   bonecount 103 (nearest 105/107/108); the 519 clips target other rigs. e000's
   motion is bound per-event at runtime and is not an extractable chunk.
2. **Event-rig vs gameplay-rig mismatch.** The 93-bone "vn" clips are authored
   against Vincent's *gameplay* rig (`km01vnw`, whose skeleton is runtime-only —
   §7.5, not renderable), whose bone order differs from the *event* rig
   (`p012`/`07700301`, which ships the renderable skd). Applying the clip to the
   event skeleton therefore leaves residual distortion on a few bones (face/hair/
   one arm spikes; edge-stretch p99≈6–8 vs 1.0 bind). The sampler REPLACES
   `node+0x30` (verified); a delta compose `bind⊗clip` reduces but does not remove
   the mismatch (p99 8.6→5.8), confirming it is a rig difference, not a decode bug.
   A pixel-accurate animated render needs the gameplay-rig skeleton (runtime-only)
   or an event clip authored for `p012`. The **format itself is fully resolved.**
