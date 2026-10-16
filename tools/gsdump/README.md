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
python3 -I src/gstex.py            d03.bin --table        # list all texture/CLUT uploads
python3 -I src/gstex_decode.py     d03.bin outdir/        # decode uploads -> PNGs + montage
python3 -I src/gsbind.py           d03.bin outdir/        # TEXTURE<->GEOMETRY BINDING table + material montage
python3 -I src/match_tex.py        outdir/ '/path/tex_*/*.png'   # match GS textures vs on-disk extracted textures
```

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
- **The 4 captured dumps are the PROLOGUE area, NOT the decoded zone-0696 BMD
  rooms.** Texture matching vs on-disk `tex_0696*` was negative (false positives
  only). So these dumps **cannot** validate the 0696 room geometry/binding
  directly — that still needs a dump captured *in* a 0696 room (a PCSX2 savestate
  parked there, loaded headless with `-statefile`; see pcsx2-headless-setup.md).
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
