# Dirge of Cerberus texture formats (reverse-engineering notes)

Milestone P1 (textures) for the purist native port. Covers the TXD/RFD
containers and their two payload kinds — the standard PS2 **TIM2 family**
(`.tm2`) and the game's custom **RST** raster (`.rst`) — and the PS2 GS
swizzle / CLUT handling needed to turn both into RGBA8.

All of this is reversed from the game's own decompiled code
(`analysis/decomp/`) and validated by exporting real textures to coherent PNGs
(`native/assets/textures/validation/`). Every field below is either cited to a
decompiled function or marked as empirically recovered.

---

## 1. Containers — TXD / RFD

Documented in `notes/doc-research-wiki/RFD-&-TXD-archives.md`; bytes verified.

```
0x00  char[8]   container name
0x08  char[4]   "txd" or "rfd"
0x0C  u32       file size
0x10  u16       file count
0x13  u8        type flag (1 or 2)  -> entry header size 0x20 or 0x10
0x14  u32[cnt]  offset of each entry header
```

Each entry header begins `name[8] + ext[4]`; the payload follows the entry
header (0x20 bytes for type 1, 0x10 for type 2) and runs to the next entry
(or EOF). `.rst` payloads carry the custom raster, `.tm2` payloads carry a
`TIM2`/`TIM3`/`TIM4` blob. (`txd.c`)

---

## 2. GS upload path — the format spec in code

The game pushes a texture to GS VRAM with a **host→local** BITBLTBUF transfer.
The GIF A+D packet is assembled in **`FUN_00441278`** (writes GS registers
`BITBLTBUF=0x50`, `TRXPOS=0x51`, `TRXREG=0x52`, `TRXDIR=0x53`) and kicked by
**`FUN_004415a0`**. The qword-count switch in `FUN_00441278` is the canonical
PS2 PSM table (the same codes the renderer must honour):

| PSM (arg4) | name      | transfer size (qwords) | bytes/pixel |
|-----------:|-----------|------------------------|-------------|
| 0x00, 0x30 | PSMCT32   | `w*h >> 2`             | 4           |
| 0x01, 0x31 | PSMCT24   | `w*h*3 >> 4`           | 3           |
| 0x02,0x0a,0x32,0x3a | PSMCT16/16S | `w*h >> 3`   | 2           |
| 0x13, 0x1b | PSMT8 / T8H | `w*h >> 4`           | 1           |
| 0x14,0x24,0x2c | PSMT4 / T4HL/HH | `w*h >> 5`     | 0.5         |

`FUN_0030a5c0` is the generic uploader (args: src MADR, DBP, DBW, DPSM, DSAX,
DSAY, W, H); `FUN_0030ab10` builds the `TEX0` sample register and takes the
**sample PSM from the same field it used as the upload DPSM** — i.e. a texture
is uploaded and sampled in the same PSM. `FUN_00441720` is `sceGsExecStoreImage`
(GS→MEM readback).

A host→local transfer feeds **linear raster** data and the GS applies its page
swizzle on write; when upload-PSM == sample-PSM the write and read swizzles
cancel, so such a payload is linear. Where a payload is **not** linear it is
stored directly in GS page order and must be deswizzled on the CPU — see §5.

---

## 3. RST layout (recovered)

The game loads an RST blob to EE RAM (`DAT_004ee948 = 0xeb0000`, set in
`FUN_002f25d0`) and parses it in place. `FUN_002f55d0(base,type,i)` returns the
per-texture sub-headers: type 2 (image) at `base + *(base+0x08) + i*0x20`,
type 3 (CLUT) at `base + *(base+0x0c) + i*0x10`; `FUN_0030a188` reads PSM / dims
/ data pointers out of those into a GS-upload descriptor;
`FUN_0030aa68`→`FUN_0030a5c0` upload image then CLUT.

On disk a payload is:

```
[optional 16-byte texture name]          (present when the container entry is named)
top header (0x30 bytes):
   +0x04  u32  signature 0x0012FA38       <-- constant in EVERY rst file (the magic)
   +0x0c  4    flags (typically 01 01 01 ff)
{ descriptor(0x40) + data } blocks, in order:
   CLUT block first (for paletted textures), then the image block.
descriptor (0x40 bytes):
   +0x00  u32  GS DBP (VRAM dest block)
   +0x12  u16  PSM        (0=CT32 … 0x13=T8, 0x14=T4)
   +0x14  u16  width
   +0x16  u16  height
   +0x18  u16  DBW (upload buffer width)
   +0x1a  u8   log2(width)      (TEX0 TW)
   +0x1b  u8   log2(height)     (TEX0 TH)
data: w*h*bytes-per-pixel bytes, immediately after the 0x40-byte descriptor.
```

The **name-prefix** is detected by where the signature lands: `+0x04` → no
prefix, `+0x14` → a 16-byte name precedes the header. CLUT is a 32-bit RGBA
table (PS2 alpha 0..0x80). The image block's PSM is one of PSMT8 / PSMT4 (the
two paletted modes actually used) or a direct PSMCT* mode. (`rst.c`)

Observed real distribution (256 containers, all `.rst`): PSMT4 ≈ 69 %,
PSMT8 ≈ 31 %; no direct-colour RST textures were seen in the sampled set.

---

## 4. TIM2 family (`TIM2`/`TIM3`/`TIM4`)

Standard PS2 TIM2. File header 16 bytes (`magic, version, align/format(u8@5),
picture count`); when byte 5 == 0 the picture header starts at 0x10, else 0x80.
Picture header (≥0x30):

```
0x00 u32 totalSize  0x04 u32 clutSize  0x08 u32 imageSize
0x0C u16 headerSize  0x0E u16 clutColors
0x10 u8 pictFormat  0x11 u8 mipmapCount  0x12 u8 clutType  0x13 u8 imageColorType
0x14 u16 width  0x16 u16 height
0x18 u64 GsTex0  0x20 u64 GsTex1  0x28 u32 GsRegs  0x2C u32 GsTexClut
```

`imageColorType`: 1=RGBA16, 2=RGB24, 3=RGBA32, 4=PSMT4, 5=PSMT8.
`clutType` low 6 bits: 1=16bpp, 2=24bpp, 3=32bpp; bit 7 set = CSM2 (else CSM1).
Image data (`imageSize`) comes first, then the CLUT.

**CLUT placement quirk (recovered):** the CLUT region is at `img_off+imageSize`.
For **256-colour** (PSMT8) palettes the colours are at the region **start**; for
**16-colour** (PSMT4) palettes they are at its **end** (after a zero-padded gap).
The decoder detects an all-zero head and shifts to the last `clutColors*4` bytes.
Verified: `copyright.rfd/cpr000` (TIM3, 256×64 PSMT4) decodes to the readable
"© 2006 SQUARE ENIX … CHARACTER DESIGN: TETSUYA NOMURA" line. (`tim2.c`)

The game's TIM textures are all paletted (PSMT8 dominant, a few PSMT4); the TIM
image data is **linear raster** — PSMT8 pictures still need the GS deswizzle in
§5 (they are stored in page order), PSMT4 pictures are plain linear.

---

## 5. PS2 GS deswizzle + CLUT

### PSMT8 deswizzle (solved, validated)
PSMT8 payloads (both RST and TIM) are stored in GS page order and are
deswizzled with the canonical PS2 8-bit page/block/column interleave
(`dtex_unswizzle8` in `swizzle.c`): 128×64-texel pages of 16×16 blocks, each
block four 16×4 columns with odd column-pairs swapped on alternate rows. This is
the documented GS layout (as PCSX2 uses), transcribed not invented. Proof: the
512×512 `m000_1` RST deswizzles to a clean weapons/items atlas and the 512×512
`menu8` TIM4 to a clean UI/emblem/font atlas; without it both are block-scramble.

### CLUT (CSM1 reorder for 256-colour)
32-bit CLUTs are read with PS2 alpha scaling (`a*255/128`, clamped). The sample
`TEX0` built in `FUN_0030ab10` does not set the CSM bit, i.e. **CSM1**, so
256-entry palettes get the standard reorder: inside every 32-colour block the
two middle groups of 8 are swapped (`dtex_read_clut32`). 16-colour palettes need
no reorder.

### PSMT4 — SOLVED (`dtex_unswizzle4`, validated)

PSMT4 is handled **linearly** for the cases that are genuinely linear:
* every TIM PSMT4 texture (TIM stores linear raster — `cpr000` proves it),
* small / sub-page RST PSMT4 textures **<128 px wide** (e.g. the 64×64
  `shinra01` Shinra-logo decodes perfectly with no deswizzle), and
* the rare `PSMT4HL` / `PSMT4HH` high-nibble modes.

RST PSMT4 textures **≥128 px wide** (≈3064 of the game's RST textures, ~53 %)
are stored pre-swizzled and are now deswizzled by **`dtex_unswizzle4`**.

**The generative model (how the layout was cracked).** The RST payload order is
reproduced *bit-for-bit* by treating the file as the **linear source of a GS
host→local PSMCT32 BITBLTBUF write**, then **reading it back as the sample PSM**.
This was proven on the working 8-bit case: composing a canonical GS PSMCT32 write
with a canonical GS PSMT8 read equals `dtex_unswizzle8` *exactly*, at every size
128–512 px (0 mismatches). That is what "transfer order, not VRAM page order"
means — the file is the transfer **source**, and a CT32 texel (4 bytes) carries
a 2×2 tile of 8-bit pixels. The PSMT4 sibling is the identical composition read
back as PSMT4 (a CT32 texel carries 8 nibbles):

```
deswizzle PSMT4(x,y):
  na = GS_PSMT4_read_nibble_addr(x, y, TBW=w)      # canonical blockTable4/columnTable4
  word = na >> 3 ;  k = na & 7                       # 8 nibbles per 32-bit word
  i    = inverse_GS_PSMCT32_write(word)              # canonical blockTable32/columnTable32
  src_nibble_index = 8*i + k                         # file is the CT32 linear source
```

The CT32 transfer region is **page-geometry driven** (this is why wide needs it
and narrow does not — a GS PSMT4 page is 128×128):

```
Wc = (h >= 128) ? w/2 : h/2        # full-page-tall tiles by width; sub-page by height
Hc = (w*h/8) / Wc                  # w*h/8 = file words
DBW(CT32 write) = Wc ,  TBW(read) = w
```

All four tables (`blockTable4`, `columnTable4`, `blockTable32`, `columnTable32`)
are the **canonical PS2 GS layout transcribed from PCSX2** (`GSTables.cpp`), not
invented. `swizzle.c` carries them verbatim.

**Proof (viewed PNGs in `validation/`):** every wide PSMT4 decodes to a coherent
image where it was block-scramble before — `068cff16` 512×512 metal-panel atlas,
`0680ff03` 256×128 stone-block wall, `0608ff2b` 256×256 timber-frame wall,
`069aff84` 128×512 scaffold, `0608ff1e` 128×64 perforated panel. Validated across
all 15 distinct wide-PSMT4 dimensions present in the game (128/256/512 wide ×
32…512 tall, square and non-square, W>H and W<H); all 3064 decode with no missing
words and no out-of-bounds. The C output is pixel-identical to the Python
reference used to search the layout.

---

## 6. What decodes correctly

| Source | Format            | Status |
|--------|-------------------|--------|
| TXD/RFD unwrap          | —        | ✅ |
| TIM2/3/4 PSMT8          | deswizzled + CSM1 | ✅ validated (menu8, scenepic) |
| TIM2/3/4 PSMT4          | linear   | ✅ validated (cpr000) |
| TIM2/3/4 direct 16/24/32| linear   | ✅ coded (no sample in data) |
| RST PSMT8               | deswizzled + CSM1 | ✅ validated (m000, z134, hm_box_l) |
| RST PSMT4, <128 wide    | linear   | ✅ validated (shinra01) |
| RST PSMT4, ≥128 wide    | CT32-transfer-order deswizzle | ✅ validated (`dtex_unswizzle4`; 3064 textures, all 15 dims) |
| RST direct PSMCT*       | linear   | ✅ coded (no sample in data) |
