# GS dump toolkit — the room/texture ground-truth reader

Parses PCSX2 `.gs.zst` dumps (the exact GIF primitive stream the real game sends
to the Graphics Synthesizer) into triangles, textures, and the **texture↔geometry
binding** — i.e. the runtime "oracle" the BMD room decoder was always missing.

Built + validated 2026-10-15. Dumps live in `/srv/shared/dirge-port/gs-dumps/`
(`doc_gsdump_01..04.gs.zst`). Capture how-to: `notes/pcsx2-headless-setup.md`.

## Pipeline (run with `python3 -I`, needs numpy+PIL; decompress first)
```
zstd -dc doc_gsdump_03.gs.zst > d03.bin
python3 -I src/gsparse.py          d03.bin              # parse: packets, draws, prim/tex stats
python3 -I src/extract_screenshot.py d03.bin shot.png   # the exact-frame embedded 640x480 screenshot
python3 -I src/gsrender.py         d03.bin render.png    # rasterize triangles (painter order, vertex colour)
python3 -I src/gsrender2.py        d03.bin oracle.png --native   # faithful textured frame (the oracle, see below)
python3 -I src/gstex.py            d03.bin --table        # list all texture/CLUT uploads
python3 -I src/gstex_decode.py     d03.bin outdir/        # decode uploads -> PNGs + montage
python3 -I src/gsbind.py           d03.bin outdir/        # TEXTURE<->GEOMETRY BINDING table + material montage
python3 -I src/match_tex.py        outdir/ '/path/tex_*/*.png'   # match GS textures vs on-disk extracted textures
```

## gsrender2: the frame oracle (2026-10-24)
Default mode reproduces one displayed frame; `--legacy` keeps the old
all-frames/all-buffers render byte-for-byte. What it does and why:
- **One frame** (`--frame=last|all|N`): a dump holds several frames split by
  VSync packets (hero dump 163235: 4). Default = last frame ending in a VSync;
  frame 0 inherits pre-dump register state and differs on ~57% of pixels.
- **Scene framebuffer only** (`--fb=main|N`): reads DISPFB from the privileged
  regs (displayed FBP 112, CT24, FBW 448), follows the blit into it back to its
  source (FBP 224, copied in 32px strips), and also renders the offscreen
  buffers 224 samples (352 flame layer, 336 bloom/shadow, 256x256 CT16/CT32).
- **Textures as of each draw** (`--tex=live|largest`): uploads are captured in
  stream order; a draw uses the latest upload that started before it. Sixteen
  addresses get different content within one frame. A texture at a render
  target's base samples that buffer (render-to-texture).
- **GS memory aliasing Dirge relies on** (page-level model): the frame clear is
  drawn at FBW 4 over the FBW-7 scene buffer, and its "Z" (= the clear colour,
  ZBP 280) lands in the buffer's second half; the scene Z buffer (ZBP 0) is
  cleared by the display blit's Z write. Also SCISSOR, alpha test, TEXA/AEM and
  destination-alpha blending, and FST sprites keep their UVs (gsrender.py's
  TriInterp zeroes them; fixed only in faithful mode / `build(..., sprite_uv=True)`).
- **Size:** drawable is **432x512** (scene SCISSOR_1 = X 0..431, Y 0..511, written
  in the stream); FBW 448 is 64-alignment. The CRTC shows only **425.5** columns
  (DISPLAY DW+1=2553 VCK / MAGH+1=6). `--crop=display` crops to that before the
  4:3 stretch; it matches the PCSX2 screenshot best (width sweep optimum 424-426).
- Hero dump vs PCSX2 screenshot (640x480): legacy MAE 74.6 / PSNR 7.0 dB;
  default 14.4 / 21.6 dB; `--crop=display` 14.0 / 21.9 dB.
- Known gaps: some PSMT4 room textures (wall planks), the cape and sky clouds
  render flat or missing (texture decode, not this renderer); HUD digits missing;
  draws that reinterpret a buffer's format, or textured draws at a mismatched
  FBW, are skipped (counted in the stats line).

## Validation (why these are trusted)
- **Parser:** walks the entire ~12 MB packet stream and lands **exactly on EOF**
  with zero bad packet-type bytes — any mis-parse would desync long before the end.
  Header fields (crc 0x33F7D21A, 640x480, serial SLES-54185) all match byte-for-byte.
- **Rasterizer:** reproduces the UI layout of the embedded screenshot (dump 04's
  "Analysis" screen: text box, scan-bands, bars all in the right screen positions).
- **Texture decode:** PSMT8 textures (uploaded as CT32 fast-path, deswizzled with
  the canonical GS PSMT8 interleave = `native/assets/textures/swizzle.c
  dtex_unswizzle8`, CSM1 CLUT) decode to **recognisable** images — Deepground
  soldier armour/face atlases, the window fire, Vincent's red cape, blue-lit
  industrial wall panels. This also **independently validates** the project's
  `dtex_unswizzle8` against real GS ground truth.

## GS dump binary format (verified vs PCSX2 GSDump.cpp/.h + our bytes)
```
u32 marker 0xFFFFFFFF
u32 header_size = 36 + serial_size + screenshot_size
GSDumpHeader (36B, all u32 LE): state_version, state_size, serial_offset,
    serial_size, crc, ss_w, ss_h, ss_offset, ss_size
serial bytes ; screenshot bytes (ss_w*ss_h*4, RGBA)
freeze/state data (state_size bytes = frozen GS regs + 4MB GS local mem)
GSPrivRegSet (0x2000 bytes)
packets: u8 type {0 Transfer,1 VSync,2 ReadFIFO2,3 Registers}
    Transfer: u8 path, u32 size, size bytes GIF data   (texture uploads + draws)
    VSync: u8 field ; ReadFIFO2: u32 size ; Registers: 0x2000 bytes
packets_at = 8 + header_size + state_size + 0x2000
```
GIF data = GIFtag(128b: nloop,eop,pre,prim,flg,nreg,regs) + register writes.
PACKED(flg0) 16B/reg by 4-bit descriptor; REGLIST(flg1) 8B/reg; IMAGE(flg2) raw
pixel bytes (host->local uploads are LINEAR raster, so decodable without the GS
write-swizzle; only the PSMT8/T4 upload-as-CT32 case needs deswizzle on read).
Context set via A+D (desc 0xE: data64 + addr8). TEX0 fields: TBP0 0-13, TBW 14-19,
PSM 20-25, TW 26-29, TH 30-33, CBP 37-50, CPSM 51-54.

## Key findings (honest)
- **CORRECTION (2026-10-17): the 4 dumps are zone z143 "Mako Reactor 0"
  (FileCode 068f), confirmed by Dan AND by texture correlation 1.0 vs on-disk
  `068f*` extracts (`out/z143_mako_dump_binding_oracle.json`) AND by the dumps'
  own embedded framebuffers (= the game-reference frames: red-cloak Vincent in
  the reactor).** The earlier "these are the PROLOGUE area, not a decoded room"
  conclusion below was WRONG: it compared the dumps against the wrong zone
  (z150/0696 — which correctly did NOT match) and leapt to "prologue". That stale
  note caused a multi-session cycle of binding z143 with heuristic GUESSES and
  asking Dan for dumps he'd already given. Do not trust it. The dumps ARE this
  room; the real gap is that we under-extract their bindings (see next bullet).
  ~~The 4 captured dumps are the PROLOGUE area, NOT the decoded zone-0696 BMD
  rooms. Texture matching vs on-disk tex_0696* was negative. These dumps cannot
  validate the 0696 room directly.~~ (struck through — wrong, see correction.)
- **dump 03 is the room binding goldmine:** `gsparse.py d03.bin` = 17,468 draws,
  ~16k triangle primitives (11002 tristrip + 5623 tri + 259 trifan), and **56
  distinct (TBP0,PSM) textures** bound to that geometry in ONE frame. The BMD
  reconstruction only ever applied ~18 textures / 52% coverage — most of the real
  binding is still unextracted in the dumps. dump 04 = 46 textures; dumps 01/02
  are point/sprite-heavy (effects/UI) with only 2 textures, not room geometry.
- **GS XYZ is screen-space** (post-VU projection, 12.4 fixed). World-space BMD
  comparison needs camera recovery; transform-invariant checks (UVs, texture
  binding, topology) are the usable path.
- **When a 0696-room dump exists, `gsbind.py` is the binding oracle:** the k-th
  drawn group's TEX0 → the material that submesh uses. Games draw submeshes in
  order, so draw-order ≈ BMD submesh order resolves the runtime binding the BMD
  file doesn't store (FORMAT.md §6).

## Limits / TODO
- T4 (PSMT4) textures currently skipped in gsbind (port `dtex_unswizzle4`).
- ~60% of dump-03 tris sample a resident/framebuffer page (TBP0=0x1c00) not in the
  per-frame uploads; reading it needs the frozen 4MB GS memory (parse the freeze
  block) or the GS write-swizzle.
- Rasterizer is painter-order vertex-colour (no z-test/alpha/texture) — enough to
  confirm geometry; full fidelity is what PCSX2 already renders (embedded shot).
