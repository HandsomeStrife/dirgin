# Session 2026-10-17 (pt 2) — the closed loop is CLOSED (gsrender2)

## What this session established (the validation prior sessions never ran)
`tools/gsdump/src/gsrender2.py` is a textured, z-aware rasterizer that rebuilds
the dump's frame from **its own** geometry + decoded textures + per-draw binding,
to grade against the dump's embedded framebuffer (the ground-truth game frame).

Result (`/srv/shared/dirge-port/binding-renders/z143-closedloop/`):
- `d03_closedloop_compare.png` — side-by-side: ground-truth framebuffer vs gsrender2.
- The reproduction shows the SAME scene: window fire glow, the three Deepground
  soldiers foreground-left, Vincent standing on the railed walkway (centre), the
  riveted/industrial reactor interior — built from the dump's real textures.
- **This closes the loop**: parse -> geometry -> texture decode (all 56 textures,
  incl. PSMT4) -> TEX0->geometry binding -> composition are all correct vs the
  actual game frame. The dumps ARE z143 and our whole stack reads them correctly.

## HONEST caveats (it is NOT pixel-perfect — do not over-claim)
- Depth: a true GS z-buffer needs the frozen per-draw TEST/ZBUF state + the
  opaque z=max "sentinel" backdrops. gsrender2's faithful mode is `--layer`
  (coarse big-quad-first / small-detail-last painter ordering) — reproduces the
  composition but is not true depth.
- No lighting/fog; a few large quads mis-colour; the bright blob upper-left is a
  cluster of 54,748 **untextured** (TME=0, zte=0/ALWAYS) degenerate tris crammed
  into ~30px — a real artifact to understand later, not room geometry.

## Decode facts nailed down this session (verify, don't re-derive)
- `gsparse._new_draw` has a latent bug: `tme=(prim>>1)&1`, `fst=(prim>>4)&1` read
  the WRONG bits. TME is PRIM bit 4, FST is bit 8. gsrender's `TriInterp._ctx`
  reads them correctly (bit 4 / bit 8), and gsbind/gsrender2 use that path, so the
  binding tables are fine — but fix gsparse's `_new_draw` before trusting its tme.
- T4 textures in the dumps are uploaded CT32-fast-path and sampled as PSMT4 ->
  decode with the project's `dtex_unswizzle4` (native/assets/textures/swizzle.c),
  which gsrender2 compiles+loads via ctypes. gsbind still skips T4 (TODO: port).
- XYOFFSET must be SEEDED from the frozen GS state (freeze+0x7c / +0xdc for this
  PCSX2 version): the frame inherits it from the prior frame and only rewrites it
  ~24% into the draw stream, so the opening draws otherwise fly offscreen.
- Per-draw state lives in the freeze block (GSDrawingContext): XYOFFSET at ctx
  base +0x7c/+0xdc; TEST/ZBUF/ALPHA also there if full depth is wanted later.

## gsrender2 usage
```
python3 -I src/gsrender2.py d03.bin out.png --layer --painter   # faithful compo
python3 -I src/gsrender2.py d03.bin out.png                     # raw per-draw z
```
Flags: --only-tex, --notex, --flat, --no-sentinel-z, --layer, --painter.
