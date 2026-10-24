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

## Vincent (fixed 2026-10-24; see notes/audit-2026-10-24.md for the audit)
- Export: `python3 -I native/assets/models/export_p000_gpu2.py dump.bin native/assets/models/out/p000_gpu3`
  (writes mesh, atlas and `placement.json` with the render args). Old output kept in out/p000_gpu2.
- Cape = mesh block **0x19f70** of 07700001.bin (smd 'p000_L02', 112 tris), NOT a procedural
  object. It has 0 skinning batches (positions posed at runtime), which is why the skinned
  decoder dropped it silently. `p000_cape.py`: topology/UV/colour from the file, this frame's
  positions from the dump's 0x2e68 draws (vertex UVs identical). Cape IoU vs dump: 0.98
  (drawable), 0.92 in-render (railing occlusion). `decode_groups` now WARNs on skipped blocks.
- Dump -> world: hero camera below, depth w = 1/q (verified: room vertices unprojected with
  w=k/q land on our z128 mesh with a sharp peak at k=1.00).
- VIF parser fixes (render_skinned.py, shared by all character decodes): mask the VIF IRQ bit
  (cmd & 0x7f), skip inline DMA tags (cnt/next) between sub-packets, and accept a DMA-tag
  header on a chain's first descriptor. These had silently dropped 629 tris: most of L01.g0
  (56 -> 335) and all of S01.g1 (0 -> 278) = Vincent's gauntlet arm + boots. Body is now
  2328 tris; all 121 dump 0x2dd4 vertex UVs exist in the mesh (was 24). decode_groups WARNs
  on skipped blocks and on groups that decode 0 tris.
- Binding is per TRIANGLE: exact UV-triple 931 + dump UV-coverage map 1123 = 88.2% from dump
  evidence (was 38.7% with group majority, which smeared red/grey). Neighbour 253, group 19,
  dropped 2. Per-tri binding is saved to out/p000_gpu3/binding.json. The 4 CLUTs on 0x2cd4: 0x2dd8 shoulder mantle, 0x2ddc body/legs,
  0x2dd4 arm + boots, 0x2cd0 small head patch.
- Vertex colour = vcol/255. The shader already doubles (texel*vcol*2, renderer_gl.c:180);
  the old vcol/128 clamp rendered Vincent up to 2x too bright.
- Placement solved from 2793 matched body vertices: `-s 0.9642 -yaw 163.63 -x -1395.37
  -y -99.17 -z -1110.77` (old -s 2.6 put him ~100px high). Whole-figure IoU vs dump 0.04 -> 0.81.
  Always take the args from out/p000_gpu3/placement.json (they move when the mesh changes).
- Pose (p000_pose.py, on by default; POSE=0 exports the bind pose): body posed to the hero
  frame from the dump, no animation decode. Vertices matched to dump corners by UV
  (tri-signature, then same-texture UV); a rigid transform per bone is solved from them and
  poses every vertex of that bone; a matched vertex keeps its dump position if it agrees
  with its bone within 1.0 unit. Of 2004 vertices: 1312 dump position, 163 dump rejected
  (bone), 529 no dump candidate (bone). 62/71 bones solved with >=4 points; 9 copy their parent (18,28,36,38,43,47,62,69,71). placement.json "bones_with_matches"=70 counts any match, not MIN_PTS. Bones 24 and 34 fit on near-colinear points, so their twist is a guess (follow-up: require point spread before solving a bone).
  Silhouette IoU vs game 0.81 -> 0.90; mantle region diff 42 -> 10; arm region 105 -> 52
  (now slightly too bright: 106,89,67 vs 89,68,54). Valid for frame 163235 only.
  Animation data: notes/autonomy-journal.md (~l.294) has the clip format (km0102vn, anim_eval.py);
  not used here.
- Arm/boots (CLUT 0x2dd4) colour, pose-independent (our 0x2dd4 pixels vs the game's 0x2dd4
  region): RGB 90,75,57 vs game 89,68,54, |d| 11 (was 102). The 0x2ee8 pass is NOT needed
  to explain it.
- Render (hero camera), then crop:
  env EGL_PLATFORM=surfaceless GALLIUM_DRIVER=llvmpipe ./native/gpu/render_scene_textured \
    -l native/assets/models/out/z128_room_v2/room.list \
    -m native/assets/models/out/p000_gpu3/vincent_p000.mesh \
    -t native/assets/models/out/p000_gpu3/vincent_p000_tex.rgba \
    -s 0.9642 -yaw 163.63 -x -1395.37 -y -99.17 -z -1110.77 -colscale 1.0 -charprelit \
    -W 2628 -H 2048 -fov 59.28 -eye -1419,-116,-1130 -at -631,-77,-515 \
    -up 0.030,-0.999,0.023 -o OUT -tag T      (OUT must exist)
  convert OUT/T_freecam.png -crop 2592x2048+36+0 +repage -resize 640x480! final.png
- UNVERIFIED: "decoder UVs are correct" for the ~60% of body tris with no exact dump match;
  whether rendering ALL LOD families (vs one) is right.
- Review images now live in /srv/shared/dirge-port/review/ (queue.json lists them):
  23_audit_fixes.png (cape/palettes/placement), 24_arms.png (arm + boots), 25_pose.png (pose).
  Add queue items with the command in /srv/tools/review-board/README.md.
  The old level-remap/ folder is gone. Game reference = the PCSX2 PNG next to the hero dump
  in save-states/sea-of-flames-gs-dumps/ (resize to 640x480).

## Room
- out/ is gitignored. Rebuild: export_z128_room.py + recover_z128_deck.py.
  Deck = brown planks 0680ff87 (recovered). Pre-deck backup: out/z128_room_v2_PREDECK_bak/.
- 47.9% of triangles bound (verified). The rest (unbound group room_g42) are NOT UV-matched;
  "dump-unseen" is UNVERIFIED. Left grey on purpose.
- UNVERIFIED: deck (box fill, recover_z128_deck.py) and "seed+grow" bindings are not
  UV-checked; likely source of wrong textures (e.g. lower-right planked facets).
  Sky layer and the large upper-right wall also differ from the game (cause unknown).
- DMSH mesh header is 36 bytes (magic + 2 uint32 + 6-float bbox), then 12 floats/vertex.

## Open
1. Pose is a single-frame snapshot (body and cape both from 163235); any other frame or
   motion needs the animation system. Arm region reads ~20% too bright.
2. 0x2ee8 second pass (16x16 CT24, ABE on, 596 tris over the arm/boots): not reproduced;
   effect unmeasured, colour already matches without it.
3. Room: unverified bindings and grey roof facets/fascias (needs more angles or the static
   binding table).
