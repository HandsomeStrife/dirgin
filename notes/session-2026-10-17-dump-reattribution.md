# Session 2026-10-17 — the z143 dumps were mis-attributed; that was the cycle

## What went wrong (root cause of repeated "still looks wrong" cycles)
A stale "honest finding" in `tools/gsdump/README.md` claimed the 4 GS dumps were
"the PROLOGUE area, NOT a decoded room." It was WRONG — it compared the dumps vs
zone z150/0696 (correctly no match) and jumped to "prologue." **The dumps are
z143 "Mako Reactor 0" (FileCode 068f).** Confirmed three ways:
1. Dan: "the dumps I have given you are from the Mako Reactor 0 level."
2. `out/z143_mako_dump_binding_oracle.json`: TBP0→068f with corr 1.0 ("US dump
   textures match EU on-disk 068f extracts, assets region-identical").
3. The dumps' embedded framebuffers ARE the `game-reference/` frames (red-cloak
   Vincent in the reactor).

Acting on the stale note, prior/this session bound z143 with heuristic guesses,
declared "wrong room," and asked Dan for dumps he'd already given. Corrected the
stale note in `tools/gsdump/README.md` and the shared gallery README.

## The lever we've been leaving on the table
The dumps contain the COMPLETE runtime binding; we only extracted ~52%.
`python3 -I tools/gsdump/src/gsparse.py d03.bin`:
- dump 03 = **17,468 draws, ~16k triangle prims (11002 tristrip + 5623 tri + 259
  trifan), 56 distinct (TBP0,PSM) textures** — essentially the whole visible room,
  fully bound, in one frame.
- dump 04 = 46 textures (room + menu overlay).
- dumps 01/02 = point/sprite heavy (effects/UI), only 2 textures — not geometry.

The BMD reconstruction applied only ~18 textures. gsbind.py already maps
draw-order TEX0 → submesh; the full 56-texture binding from dump 03 should push
coverage far above 52%.

## The validation we never ran (the pass/fail)
`tools/gsdump/src/gsrender.py d03.bin render.png` rasterizes the dump's own
triangles+textures → should reproduce `d03_shot.png` (the embedded screenshot)
≈ the game frame. GS XYZ is screen-space (post-VU, 12.4 fixed), so no camera
recovery is needed to reproduce the frame. This closed loop (rasterized dump vs
its own screenshot) is the honest arbiter — not free-orbit eyeballing.
Decompressed dumps + screenshots are staged at
`<scratchpad>/gs/d0{1..4}.bin` + `d0{1..4}_shot.png` (regenerate with
`zstd -dc <dump>.gs.zst`).

## Done this session
- Identified e000=Weiss (wrong) vs p005=Vincent (correct); built a textured
  Vincent p005 mesh (`native/assets/models/out/p005/vincent_p005.mesh` +
  `vincent_p005_combined_tex.rgba`). Red cloak OK; **body UVs still wrong** (Dan
  confirmed "textured wrong").
- Added a raw-RGBA (`.rgba`) `-t` path to `render_scene_textured.c` so the
  multi-atlas Vincent cloak renders on GPU (commit 15ff730, pushed).

## Two work items for the next agent
A. ROOM: extract the FULL per-draw binding from dump 03 (+04) via gsbind.py;
   reproduce the frame via gsrender.py and diff vs d03_shot.png / game-reference;
   then apply the complete binding to the BMD room (replace heuristic guesses).
B. CHARACTER: fix Vincent p005 BODY UVs (cloak atlas path already works). Validate
   against the Vincent rip and game-reference, not self-assessment.
