# VU1 RENDER / SHADING MATH — Dirge of Cerberus (SLES_541.85)

Purist native-port reference for the per-vertex **rendering, lighting, colour, fog,
UV and GIF-output** math done on VU1. Companion to the earlier TRANSFORM writeup
(matrix·vertex → clip → FTOI4 → XGKICK). Everything below is traced to specific
disassembled instructions in `port/vu/disasm/`; line citations are `file:line`
(the file's own `idx` column is also given, since that is the micro-PC).

> Scope note. The VU1 resident programs are **A, B, D**. The blob previously filed
> as "program C / transform-only" is actually the **VU0 skinning image**
> (`disasm/vu0_skin_micro.txt` header, lines 1-60) — it has no XGKICK and every
> EE `_vcallms` lands in it. So VU1 rendering = A/B/D; VU0 = skinning. This matters
> for lighting: the **normal** that VU1-D lights is produced by VU0 skinning first.

---

## 0. TL;DR for the GLSL author

| Aspect            | What the PS2 actually does                                                                 | Programs |
|-------------------|--------------------------------------------------------------------------------------------|----------|
| Position          | `clip = Mvp · v`; perspective `Q = 1/clip.w`; screen `xyz = clip.xyz·Q` → `FTOI4` (12.4)    | A,B,D    |
| Guard/frustum     | `CLIP` op on clip-space xyz vs ±w; AABB (B) or per-vert (A,D) trivial-reject via clip flags | A,B,D    |
| Backface          | screen-space edge cross product (`OPMULA`/`OPMSUB`), sign of z, gated by a two-sided flag   | A,D      |
| UV / texcoord     | int16 `ITOF12` (÷4096) → `+ bias` → `· Q` (perspective) → GS **ST** (s/w, t/w)              | A,B,D    |
| Vertex colour     | int RGBA `ITOF0` (×1, i.e. 0..255) read from a per-vertex stream, written back `FTOI0`      | A,B,D    |
| **Dynamic light** | **3 directional lights + ambient, matrix form**, `⊗` vertex colour, saturate — **D only**  | **D**    |
| Fog               | per-vertex `F` from a function of clip `w`, clamped `[fogmin, I]`, packed into **XYZF2.F**  | A,B      |
| GS output         | GIFtag (asset-authored, copied from VuMem top) + per vertex { **ST**, **RGBAQ**, **XYZF2** }| A,B,D    |

**The single most important finding:** there is **no Lambert N·L term in the
world/static path (A, B)** — those draw `texture × vertexColour` with fog. Only the
**character/actor path (D)** computes real per-vertex lighting, and it does it as a
pair of 3×3 matrices (light directions, then light colours) with an ambient add.
A flat-lit placeholder is wrong for D and *roughly right* for A/B (minus fog).

---

## 1. LIGHTING MODEL (program D)

### 1.1 The instruction chain (one vertex), `disasm/program_D.txt`

Constants loaded once at entry (`program_D.txt:20-36`, idx 17-33) from VuMem:

```
vf01..vf04  = 0x00,0x10,0x20,0x30   Mvp (4x4 clip matrix)
vf05,vf06,vf07 = 0x40,0x50,0x60     L  = LIGHT-DIRECTION matrix (3x3)
vf08,vf09,vf10 = 0x70,0x80,0x90     C  = LIGHT-COLOUR matrix (3x3)
vf11           = 0xa0               A  = AMBIENT colour (used as the w/MADDw term)
vf12,vf13      = 0xb0,0xc0          fog / clip-guard constants
vf14..vf17     = 0xd0..0x100        second 4x4 (dual output, see §3.3)
```

Per-vertex lighting (first unrolled vertex; the loop repeats it at idx 124-126,
156-159, 265, 305):

```
idx  file:line   instruction                         meaning
 85  :88  LQ.xyz vf26, 0x10(vi07)                     n  = vertex NORMAL (int, from VU0-skinned stream)
 88  :91  MULAx.xyz  ACC, vf05, vf26x                 ┐
 89  :92  MADDAy.xyz ACC, vf06, vf26y                 │ d = L · n   (3 dot products, one per light dir)
 90  :93  MADDz.xyz  vf26, vf07, vf26z                ┘   (rows of L = the 3 light directions)
 94  :97  MAX.xyz    vf28, vf26, vf00                 d = max(d, 0)        ← LAMBERT CLAMP (vf00.xyz = 0)
 98  :101 MULAx.xyz  ACC, vf08, vf28x                 ┐
 99  :102 MADDAy.xyz ACC, vf09, vf28y                 │ lit = C · d + A·1
101  :104 MADDAz.xyz ACC, vf10, vf28z                 │   (cols of C = the 3 light RGB colours,
102  :105 MADDw.xyz  vf28, vf11, vf00w                ┘    vf11 = ambient, vf00.w = 1.0)
 87  :90  ITOF0.xyzw vf19, vf27  (vf27=LQ 0x30(vi12)) col = per-vertex RGBA (0..255 float)
111  :114 MUL.xyz    vf18, vf28, vf19                 rgb = lit ⊗ vertexColour
115  :118 MINIi.xyz  vf19, vf18, I                    rgb = min(rgb, I)    ← SATURATE (I ≈ 255 / 127)
126  :129 FTOI0.xyzw vf26, vf19                        → integer RGBA for the GS
```

The repeat of exactly this chain per vertex is at `program_D.txt:127` (idx 124
`MAX.xyz vf28,vf26,vf00`), `:159` (idx 156), `:268` (idx 265), `:308` (idx 305).
`grep -E "MAX[a-z]*\.[xyzw]+ vf[0-9]+, vf[0-9]+, vf00 "` returns **13** hits in D
and **0** in A/B/C — the Lambert clamp is unique to D. (Several of those 13 are
`MAX.xyz vfNN, vf00, vf00` = `max(0,0)`, i.e. accumulator zeroing at idx 76/77,
249/250, 370/371, 1161/1162; the five genuine `max(L·n,0)` sites are the ones with
`vf26`/`vf24` as the middle operand.)

### 1.2 What the model is, in equation form

```
d_i   = max( Ldir_i · n , 0 )                 i = 0,1,2     (3 directional lights)
lit   = Σ_i  Lcol_i · d_i   +  ambient                        (per-channel RGB)
rgb   = saturate( lit ⊗ vertexColour )                        (component multiply, clamp)
```

* **Number of lights: 3, directional.** The 3×3 shape is exact: three dot products
  (one MADD chain, `vf05/06/07`), three colour columns (`vf08/09/10`). No per-vertex
  light-position subtraction or distance/attenuation appears → **no point lights** in
  the resident microcode.
* **Ambient** = `vf11` (VuMem 0xa0), folded in by the `MADDw …, vf00w` (×1) term.
* **Colours combine additively** (`C·d`), then **multiply** the vertex colour.
* **Saturation** by `MINIi …, I` (idx 115/147). The `I` immediate and the actual
  light constants are **runtime VuMem uploads**, so their *values* are not in the
  static image — see Gaps §6.

### 1.3 Where the light matrices come from

`L` (light directions) is pre-rotated into the object/bone frame on the EE/VU0 side
before the draw. The VU0 skinning image builds the scaled bone basis + translation
(`vu0_skin_micro.txt:30-49`); the light-direction basis is uploaded alongside the
clip matrix into VuMem 0x40-0xa0. The microcode only *consumes* `L`, `C`, `A`.

### 1.4 The OPMULA/OPMSUB cross products are NOT lighting

In A and D the `OPMULA`/`OPMSUB` pairs compute a **screen-space edge cross product
for backface culling**, operating on the three *post-perspective-divide* vertices:

```
program_A.txt:100-101  OPMULA.xyz ACC, vf24, vf20 ; OPMSUB.xyz vf11, vf20, vf24
      (vf24 = v0-v1, vf20 = v1-v2 screen edges → vf11 = face normal in screen space)
program_A.txt:103      ABS.xyz vf00, vf11          (take |normal|; sign of z → facing)
program_D.txt:104-105  OPMULA.xyz ACC, vf21, vf29 ; OPMSUB.xyz vf27, vf29, vf21
program_D.txt:109      MUL.z vf00, vf27, vf22      (z of cross · orientation const → cull test)
```

The result feeds the `FMAND`/`IBNE` branch that skips emitting the primitive
(`program_A.txt:109` `IBNE … -> 162`). This is culling, not shading. The cross
products inside the VU0/skin image (`disasm/program_C.txt:186-187` etc.) are
basis/quaternion-to-matrix construction, also not lighting.

---

## 2. VERTEX COLOUR / MATERIAL (all programs)

* Every render program reads a **per-vertex RGBA** from an attribute stream with
  `ITOF0` (int→float, **no scale**, so values are 0..255) and writes it back with
  `FTOI0`:
  * A: `program_A.txt:416-421` (ITOF0 vf28/vf26/vf27/vf22/vf24), `:455-463` (FTOI0).
  * B: census `ITOF0`×11, `FTOI0`×10.
  * D: `program_D.txt:90` (ITOF0 vf19), `:129` (FTOI0 vf26).
* **A and B pass that colour through essentially unchanged** (no N·L). Any shading
  variation in world geometry is therefore **baked into the model's vertex colours**
  plus the fog term — consistent with the game's pre-lit environment art.
* **D multiplies** the vertex colour by the computed lighting (§1.1 idx 111).
* **Alpha**: the RGBA is carried as a full xyzw quad (`ITOF0.xyzw`/`FTOI0.xyzw`), so
  the **A channel rides through untouched** by lighting (lighting is `.xyz` only —
  note every lighting op above is `.xyz`). Alpha is the vertex alpha → GS RGBAQ.A,
  used by the GS alpha blend/test configured in the asset's GIFtag A+D.
* **Material colour** per draw is not a separate VU constant multiply; it is folded
  into either the light colour matrix `C` (D) or the baked vertex colours (A/B).

### 2.1 The Q / perspective term

```
program_A.txt:73  DIV Q, vf00w, vf20w      Q = 1.0 / clip.w     (= 1/w)
program_D.txt:92  DIV Q, vf00w, vf18w       same
```

`Q = 1/w` is then multiplied into **both** the screen xyz (perspective divide) and
the UV (`MULq`, §4). On the GS it is written as **RGBAQ.Q** so the rasteriser does
perspective-correct texturing (GS divides ST by Q at the pixel). So the RGBAQ
register carries `{R,G,B,A, Q=1/w}`.

---

## 3. GIF OUTPUT VERTEX FORMAT (XGKICK)

### 3.1 How the packet is assembled

* The **GIFtag (and its A+D / register descriptor) is authored in the asset** and
  copied verbatim from the top of VuMem into the output packet head:
  * D: `program_D.txt:58-59` `LQ vf26,0x3fe0 / LQ vf23,0x3ff0` then `:67,:69`
    `SQ vf26,0x0(vi07) / SQ vf23,0x10(vi07)` — two control qwords at the packet head.
  * A: the per-draw GIFtag is read from the input at `program_A.txt:31`
    `LQ.xyw vf11,0x0(vi08)` + `:32` `LQ.z vf11,0x10(vi08)`, stashed `:42`.
  * B: the vertex **count (NLOOP)** is patched into the GIFtag at draw end
    (`program_B.txt:176` `ISW.x vi10,0x0(vi09)`) immediately before the single
    `XGKICK vi09` (`:177`).
* Therefore the **exact PRIM (tri / tri-strip), NREG, REGS order and A+D register
  writes are data, not code** — the microcode only fills the per-vertex slots. This
  matches the P1 BMD/MOD finding that models carry "GIF/VIF draw templates".

### 3.2 Per-vertex register layout (what the microcode fills)

The VU writes **3 qwords per vertex**, stride `0x30` (3 qwords), interleaved:

```
program_A.txt:265-269 (block 2, 4 verts, stride 0x30):
   +0x00  SQ vf15  = ST     (perspective UV, see §4)
   +0x10  (RGBAQ)  = colour — NOT written in this inner loop ⇒ pre-placed / template slot
   +0x20  SQ vf14  = XYZF2  (FTOI4 screen pos 12.4, + fog F in .w)
program_A.txt:262-264 write the ST qwords; :266-269 write the XYZF2 qwords.
```

For program D the three filled slots per vertex are:
```
ST    ← ITOF12 UV · Q         (e.g. program_D.txt:120 MULq vf25 ; :186/:189 SQ)
RGBAQ ← FTOI0 lit⊗col         (program_D.txt:129 FTOI0 vf26 ; :114/:190 SQ vf26)
XYZF2 ← FTOI4 clip·Q          (program_D.txt:106/:135/:170 FTOI4 ; :114/:189 SQ)
```

So the standard packed triple is **ST, RGBAQ, XYZF2 (NREG=3)**, PRIM = triangle or
triangle-strip (asset-selected). The per-vertex **ADC / "kick" flag** is stamped
into the XYZ qword's `.w` integer lane:
* A: `program_A.txt:270-280` ILW/IOR/ISW of `0x2(vi11).w` with `vi05=0x8000`.
* B: `program_B.txt:156-159` `MFIR.w vfNN, vi06` writes the w from an int reg.
* D: `program_D.txt:127` `ISW.w vi07,-4(vi15)` after OR-ing the clip/guard flags.

### 3.3 XYZF2 vs XYZ2, and the dual output

* The 4th lane of the position is populated and meaningful (`FTOI4.xyzw`,
  `program_A.txt:109-111`, `program_D.txt:106`), and A/B compute a **fog F** into it
  (§5) ⇒ the primitive register is **XYZF2** (X,Y 12.4 ; Z ; F fog) for the fogged
  paths. Where no fog is packed it degenerates to XYZ2 + ADC.
* D emits a **second transformed position** per vertex via the `vf14..vf17` matrix
  (`program_D.txt:92-97,117` → `SQI vf23,(vi14++)`), i.e. a dual-stream draw
  (second viewport / shadow / depth pass). Treat the second stream as a separate
  draw with the same colour/UV; flag in §6.

---

## 4. TEXTURE COORDINATES (UV) — confirmed

The earlier note (ITOF12 ÷4096 texcoord stream) is confirmed and completed:

```
program_A.txt:50  ITOF12.xyw vf16, vf17     raw int16 UV → float ÷4096        (vf17 = LQ.xyw 0xa0)
program_A.txt:87  ADD.xy vf16, vf16, vf10   + UV bias (vf10, from VuMem 0x80.zw / 0x180)
program_A.txt:90  MULq.xyz vf15, vf15, Q    × Q (=1/w) → perspective-correct s/w, t/w
program_D.txt:91  ITOF12.xyw vf25, vf25     same ÷4096
program_D.txt:117 MULq.xyz vf25, vf25, Q    × Q
program_B.txt:117 MULq.xy vf17, vf20, Q     same
```

Path: **int16 texel coord → `ITOF12` (÷4096) → add bias `vf10` → multiply by
`Q=1/w` → store as GS `ST` (= s/w, t/w)**. Paired with `RGBAQ.Q = 1/w`, the GS
recovers perspective-correct `(s,t)`. The `÷4096` means UVs are authored in **1/4096
texel-fraction fixed point** (so 1.0 in UV = 4096 units); the bias `vf10` is a
half-texel / atlas offset constant. Values land in `[0,1]`-ish per the P1 MOD export
(UV[0..1]).

---

## 5. FOG (program A, B)

```
program_A.txt:94-99   MULw.x vf22,vf09,vf20w ; MULw.y vf22,vf09,vf22w ;
                      MULw.z vf22,vf10,vf24w ; ADDw.xyz vf22,vf22,vf09w
program_A.txt:102/104 MINIi.xyz vf24,vf22,I ; MAXz.xyz vf24,vf24,vf09z   (clamp [vf09.z, I])
program_A.txt:105-107 MULx.w vf21,…vf24x …  → fog packed into the .w of each screen vertex
```

Per-vertex **fog factor F is an affine function of clip `w`** (`vf09.* · w + vf09.w`),
**clamped** to `[vf09.z, I]`, then placed in the position's `.w` so `FTOI4.xyzw`
emits **XYZF2** with F. `vf09` (VuMem 0x80/0x180) holds the fog scale/offset/min; its
run-time values are not in the static image (§6). D uses the analogous `vf12/vf13`
guard/fog constants (`program_D.txt:106,139,171`).

---

## 6. HONEST GAPS / things to confirm against a live capture

1. **Actual light constants (D).** `L` (3 dirs), `C` (3 colours), `A` (ambient) and
   the `MINIi` saturate `I` are VuMem uploads (0x40-0xa0), not in the ELF. Dump VuMem
   right before an actor `MSCAL` to read the real numbers. The *form* (3 dir + ambient,
   `max(0)`, ⊗ vcol) is certain; the *values* are not.
2. **GIFtag exact bits.** PRIM type, NREG, REGS order (assumed ST,RGBAQ,XYZF2),
   context, ABE/alpha and ADC semantics live in the asset template (VuMem top /
   input head). Capture one real GIF packet to lock PRIM and the register list.
   The `+0x10` per-vertex slot in A is never written by the inner loop — confirm it
   is the RGBAQ template slot (constant colour) vs. filled elsewhere.
3. **A vs B.** Both are unlit (texture×vcol+fog). B batches the whole mesh then does
   one `XGKICK` and patches NLOOP (`program_B.txt:176-177`); A/D kick per strip. B's
   `MAX/MINI` chain (`program_B.txt:129-140`) is **AABB build + guard-band CLIP
   (trivial reject)**, not lighting — verified (no `max(·,0)`). Exact B vertex count
   per primitive (list vs strip) worth confirming from a capture.
4. **Dual output stream (D, §3.3).** Whether `vf14..vf17` is a shadow pass, a second
   viewport, or a depth prepass is not determinable statically — two packets are
   emitted; render both or identify the second's GIFtag.
5. **Specular / emissive.** None found. Lighting is `.xyz` diffuse only. If the game
   shows specular it is baked into vertex colour or a second textured pass.

---

## 7. EQUIVALENT MODERN SHADER (sketch)

Drop-in GLSL that reproduces the above. Two variants are needed:
`DIRGE_LIT` (program D) and unlit (A/B). See `shaders/dirge_purist.vert` and
`shaders/dirge_purist.frag` next to this file. Key fidelity points:

* Do the 3-light math **per vertex** (Gouraud), not per pixel — the PS2 lit per
  vertex and interpolated. `max(dot,0)`, sum of 3 coloured lights, + ambient, then
  **multiply by the per-vertex RGBA**, then **saturate**.
* Keep **perspective-correct ST** (hardware interpolators already do `s/w` — just
  pass raw `s,t`; do NOT pre-divide in the modern pipe, the GS pre-divide is only a
  PS2 artefact).
* Fog: `F = clamp(a·w + b, fogmin, 1)` per vertex, `mix(fogColour, lit, F)` — or feed
  the native fog unit. Use the captured `vf09` constants.
* Vertex positions dequantise ÷16 (`ITOF4`, static/world) or ÷4096 (`ITOF12`,
  skinned actors) — see `vu0_skin_micro.txt:52`. UV ÷4096.
* Alpha from vertex colour drives blend/alpha-test as the asset GIFtag dictates.
</content>
</invoke>
