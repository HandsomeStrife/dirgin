# Handoff — sea-of-flames scene (updated 2026-10-24)

Read this first instead of re-deriving. Review images: notes/review/ (README explains each).

## Scene basis (settled)
- Sea-of-flames = room **z128** (FileCode 0680, Kalm castle-town) + Vincent **p000**
  (geom extract/_KEL/noPath/07700001.bin, tex 07700002.bin). NOT z143/p005.
- GS dumps: /srv/shared/dirge-port/save-states/sea-of-flames-gs-dumps/*.gs.zst
  (hero frame 163235). Decompress: `zstd -dq <file>.gs.zst -o dump.bin`.

## Oracle (grade against this, never eyeball)
- `python3 -I tools/gsdump/src/gsrender2.py dump.bin out.png --native`
  GS drawable is 432x512, stretched to 4:3. Without --native, geometry lands shifted/thin.
- Check placement by overlaying a silhouette on the game frame, not by looking.

## Vincent (done)
- Export: `python3 -I native/assets/models/export_p000_gpu2.py dump.bin native/assets/models/out/p000_gpu2`
- Key facts: decoder UVs are CORRECT. Body texture TBP0 0x2cd4 is drawn with 4 CLUTs
  per frame (CBP 0x2dd8 red mantle; 0x2cd0/0x2ddc/0x2dd4 grey body), so binding must
  carry full TEX0 per triangle (UV-triple match to the dump). Use ALL LOD families
  (L01 alone drops the thighs). Vertex colour = vcol/128 (GS modulate).
- Render (hero camera), then crop:
  env EGL_PLATFORM=surfaceless GALLIUM_DRIVER=llvmpipe ./native/gpu/render_scene_textured \
    -l native/assets/models/out/z128_room_v2/room.list \
    -m native/assets/models/out/p000_gpu2/vincent_p000.mesh \
    -t native/assets/models/out/p000_gpu2/vincent_p000_tex.rgba \
    -s 2.6 -yaw 165 -x -1374 -y -85 -z -1086 -colscale 1.0 -charprelit \
    -W 2628 -H 2048 -fov 59.28 -eye -1419,-116,-1130 -at -631,-77,-515 \
    -up 0.030,-0.999,0.023 -o OUT -tag T
  convert OUT/T_freecam.png -crop 2592x2048+36+0 +repage -resize 640x480! final.png
- Room is -Y-up; place_character handles it via cam up_y. -charprelit avoids blonde hair.

## Room
- out/ is gitignored. Rebuild: export_z128_room.py + recover_z128_deck.py.
  Deck = brown planks 0680ff87 (recovered). Pre-deck backup: out/z128_room_v2_PREDECK_bak/.
- ~48% of surfaces bound; the rest (unbound group room_g42) are dump-unseen. Left grey on
  purpose: a wrong texture is worse than an honest grey.
- DMSH mesh header is 36 bytes (magic + 2 uint32 + 6-float bbox), then 12 floats/vertex.

## Open
1. Procedural cape (separate cloth object, not in the skinned mesh). Biggest gap.
2. Bind pose vs the game's walking pose.
3. Dump-unseen grey roof facets/fascias (needs more angles or the static binding table).
