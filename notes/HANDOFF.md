# Handoff — sea-of-flames scene (updated 2026-10-24)

Read this first instead of re-deriving. Review images: notes/review/ (README explains each).

## Scene basis (settled)
- Sea-of-flames = room **z128** (FileCode 0680, Kalm castle-town) + Vincent **p000**
  (geom extract/_KEL/noPath/07700001.bin, tex 07700002.bin). NOT z143/p005.
- GS dumps: /srv/shared/dirge-port/save-states/sea-of-flames-gs-dumps/*.gs.zst
  (hero frame 163235). Decompress: `zstd -dq <file>.gs.zst -o dump.bin`.

## Oracle (grade against this, never eyeball)
- `python3 -I tools/gsdump/src/gsrender2.py dump.bin out.png --native [--crop=display]`
  Renders the last full frame, scene framebuffer only, textures as of each draw
  (fixed 2026-10-24; old render = `--legacy`). Hero dump vs PCSX2 shot: PSNR 21.6 dB
  (was 7.0). Drawable is 432x512 (scene SCISSOR), stretched to 4:3; the CRTC shows
  425.5 of those columns (`--crop=display`, best match). FBW 448 is just alignment.
  Wall planks / cape / clouds still flat: texture decode gap. See tools/gsdump/README.md.
- Check placement by overlaying a silhouette on the game frame, not by looking.
- Scope (Dan, 2026-10-24): the oracle is for PLACEMENT and draw attribution (where, how
  deep, which draw made a pixel). Grade COLOUR against the PCSX2 screenshot. Its texture/HUD
  gaps are parked on purpose; don't fix them unless we need colour from un-screenshotted angles.

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
  (bone), 529 no dump candidate (bone). 57/71 bones solved (>=4 points AND 2nd principal spread >= 0.05 units; the hand bones 53-61 are
  >= 0.087). 14 copy their nearest solved ancestor (18,24,28,32,34,36,38,43,47,62,67,69,71,75); of those,
  the colinear ones (24,32,34,67,75) keep their fitted direction and position and take only the twist
  from the parent. 0 bones lack a solved ancestor. placement.json pose stats list all of this.
  Silhouette IoU vs game 0.81 -> 0.90; mantle region diff 42 -> 10; arm region 105 -> 53 (colinear-bone fix: 51.9 -> 52.9, accepted)
  (now slightly too bright: 106,90,67 vs 89,68,54). Valid for frame 163235 only.
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
    -s 0.9641 -yaw 163.63 -x -1395.59 -y -98.76 -z -1110.57 -colscale 1.0 -charprelit \
    -charatest 0.77734 \
    -W 2628 -H 2048 $(camera_args from out/p000_gpu3/placement.json) -o OUT -tag T
  (OUT must exist). Hero camera fitted 2026-10-24 to 5164 dump room vertices (near error
  4.5 px -> 0.05 px): -fov 59.2749 -eye -1419.227,-115.571,-1129.797
  -at -631.348,-77.227,-515.163 -up 0.03023,-0.99926,0.02358. Placement for that camera:
  -s 0.9641 -yaw 163.63 -x -1395.59 -y -98.76 -z -1110.57 (always take both from
  placement.json). The old rounded camera put near geometry ~4.5 px off. export_z128_sky.py
  still has its own copy of the old camera (0.07 px at sky depth; should import HERO_CAM).
  convert OUT/T_freecam.png -crop 2592x2048+36+0 +repage -resize 640x480! final.png
- Cape alpha (fixed 2026-10-24, review 38; not committed): the game alpha-tests the cape
  (0x2e68: ATE GEQUAL AREF 100, AFAIL RGB_ONLY, blended by As; 20% of its texels are 0 =
  the torn hem/fold gaps). We drew it opaque -> pale pink fringe, and the opaque hem covered
  the near post's foot (the hem IS 0.15 units in front of it in the dump too: 603 px dump,
  537 ours; the game hides it because those texels are transparent). The atlas keeps the
  cape's alpha as A*255/128 (body 255; body draws aren't alpha-tested); vertex alpha =
  128/255. Render with -charatest <char_alpha_ref from placement.json> (= 0.77734), which
  runs the room's two-pass GS ATE draw for the character. Crop pink px 239 -> 5 (game 62),
  SSIM 0.508 -> 0.532. Cape is drawn once per frame: the 224 dump tris = 4 frames x 56.
- UNVERIFIED: "decoder UVs are correct" for the ~60% of body tris with no exact dump match;
  whether rendering ALL LOD families (vs one) is right.
- Review images now live in /srv/shared/dirge-port/review/ (queue.json lists them):
  23_audit_fixes.png (cape/palettes/placement), 24_arms.png (arm + boots), 25_pose.png (pose).
  Add queue items with the command in /srv/tools/review-board/README.md.
  The old level-remap/ folder is gone. Game reference = the PCSX2 PNG next to the hero dump
  in save-states/sea-of-flames-gs-dumps/ (resize to 640x480).

## Room
- out/ is gitignored. Rebuild (in native/assets/models, run -I):
    recover_z128_deck.py --in out/z128_pktfc_union --out out/z128_pktfc_v3
    export_z128_room.py --pf out/z128_pktfc_v3 -o out/z128_room_v2/room       (room bbox for the sky check)
    export_z128_sky.py <hero dump.bin> out/z128_room_v2/room_sky.mesh
    export_z128_room.py --pf out/z128_pktfc_v3 -o out/z128_room_v2/room --sky room_sky.mesh
  Deck = brown planks 0680ff87 (recovered). Backups: out/z128_room_v2_PREDECK_bak/,
  out/z128_room_v2_PRE28_bak/ (before the sky/wall/facet fixes).
- Grading: the hero dump holds 4 identical frames (vsyncs at tris 16876/33752/50629/67501).
  Grade with the LAST frame and the main framebuffer only (FRAME FBP 224, FBW 7); rasterizing
  all frames/targets (as `gsrender2 --legacy` still does) mixes post-process passes into the image.
  Main-pass order: clear (38,38,47) -> room -> sky dome 11504 (additive) -> post (fb 336/352
  read back as TBP 10752/11264) -> additive cloud layer 14272 + 15872 -> HUD.
- Fixed 2026-10-24 (review 27 = evidence, 28 = result; not committed):
  - Sky: export_z128_sky.py takes the dump's TBP 11504 draws (166 tris) as a dome, texture
    0680ff35 (corr 1.000 with the dump texture). It isn't in any 0680* BMD. In-game it is
    ADDITIVE with vcol 5/128 over the clear, so our opaque copy bakes vertex colour to keep
    the mean. Scaled 0.2 about the eye, because the renderer's near plane = 0.0005 x far from
    the bbox of ALL groups (the full-size dome clipped the deck). Valid for the hero camera only.
  - Upper-right wall: EXCLUDE_PACKETS in export_z128_room.py (06800a12 p29/31, 06800c12
    p70/71): big x=-829 quads with 0..1 UVs, in view but with no dump vertex within 3u.
    Explicit list only; no general "no dump match" rule (it would also drop hidden surfaces).
  - Grey facet: BIND_OVERRIDE 06800212 p370 -> 0680ff85 (dump TBP 12996, depth ratio 1.01).
    Per-packet, so its second small tri is bound too.
  - Result: upper-right depth match vs dump 37% -> 99.2% (ours in front 28% -> 0.1%).
    Sky patch (x320-640,y0-40 of 640x480) 24,26,32 -> 34,35,44, game 53,54,66.
    Upper right 25,28,31 -> 26,25,27, game 38,38,45. Mid-left reference matches within 1.
- Remaining sky/UR gap: the additive cloud layer TBP 14272 (vcol ~93,102,140, alpha ~53)
  and the post passes aren't reproduced. The renderer has no additive blend and a fixed
  clear colour (24,26,32).
- Alpha test (fixed 2026-10-24, review 31; not committed): ALPHA_TEST in export_z128_room.py
  lists FileCodes whose dump ABE draws use ATE GEQUAL 96 + AFAIL RGB_ONLY + (Cs-Cd)*As+Cd
  (TBP->FC by texture corr 1.000; reasons recorded per entry). Their ABE packets go to
  separate groups listed AFTER the sky (game order) with the ref in room.list column 4;
  render_scene_textured draws those blended, depth only where As >= (96-0.5)/128. Five groups:
  0680ff31 (battlements: g21 opaque 650 tris + g43 alpha 630 tris), 0680ff30, 0680ff28, 0680ff32, 0680ff54 (0680ff07's own 37
  packets are non-ABE; it is in the table only for the 3 rebound wash packets, see deck fix below). Not global on purpose: 0680ff36 has 16% cut-out texels but the
  game draws it opaque. Vertex alpha checked: 128 on the battlement ATE pass; 0680ff30 also
  has Av 76-78, which caps As below the ref, so those faces blend semi-transparent, as in-game.
  Result: 8309 px change at 640x480 exact (4300 with 2% tolerance), all in the battlement band + lower-right 0680ff30 strip. Gap px vs
  game |d| 20.5 -> 14.3 (they now show our sky, which is darker than the game's); gap spacing
  22 -> 11.5 px across the band vs game teeth 19 -> 13 px; 0680ff30 region 8.6 -> 6.1; 0680ff28
  only 5 px in view; sky patch identical. Control: the same list without column 4 = old image,
  0 px different. Gaps: 0680ff30/28 have a few ABE draws with ATE off that the BMD can't tell
  apart (they get the test); As in (0.75,1) blends instead of being opaque. Backup:
  out/z128_room_v2_PRE31_bak/. Groups renumbered: unbound is now room_g40.
  Fallbacks are loud (stderr "warning:"): bad/garbled/out-of-range col 4 (valid: absent, -1,
  0..1) -> group drawn opaque; col-4 group with no texture -> test dropped; ALPHA_TEST entry
  matching no ABE packet (exporter). A clean rebuild + render prints none of them.
- Dark deck at Vincent's feet (fixed 2026-10-24, review 33; not committed): NOT the deck
  fill. 06800112 pkts 585/986/1027 (ABE, the only 0680ff06 packets) were bound to 0680ff06
  (near-black texture, corr 0.06 with the dump) and drawn opaque. The game draws those
  pixels with TBP 10792 = 0680ff07 (corr 1.000), same vertex colours (RGB 12-26, A 0-66),
  ATE GEQUAL 96 + (Cs-Cd)*As+Cd: a faint dark wash over the deck 0680ff87 (TBP 13228).
  Fix: BIND_OVERRIDE -> 0680ff07, and 0680ff07 added to ALPHA_TEST (its 37 non-ABE packets
  stay opaque). 986/1027 are out of view (same signature, dump-unverified). Floor mask
  (5328 px): mean 10,8,9 -> 33,21,17 (game 49,34,26), |d| 27.5 -> 12.6, SSIM 0.25 -> 0.51;
  strip left of Vincent (18.7k px) |d| 27.8 -> 5.5. Sky/mid-left/upper-right identical.
  The rest of the gap is shared with the oracle (35,26,20 there), so it's post/fog, not the wash.
  Gradient corr vs game ~0 before AND after (and with the wash removed): plank structure
  doesn't line up. Deck UVs (spatial fill) are still unverified. Grading scripts were scratch-only.
  Backup: out/z128_room_v2_PRESKY32_bak/ (= HEAD output, verified byte-identical).
- Grey banner along the walkway edge (fixed 2026-10-24, review 34; not committed): the pale
  beam by Vincent's railing was the UNBOUND group (flat vertex colour), not a wrong bind:
  06800212 pkts 0 and 4 (1 tri each), the deck's vertical outer edge (z=-1117.6, y -98.4..-94.5).
  The UV-triple seed left them null; recover_z128_deck.py skips them (|ny| < 0.8). Hero dump
  draws them with TBP 13228 = 0680ff87 (corr 1.000): pkt 0 UV triple identical to draw 3408,
  pkt 4 shares UVs with draws 3412/3413, vertex colours within 6. Fix: BIND_OVERRIDE -> 0680ff87.
  Found with an ID render (per-tri colour R=group+1, GB=tri index, -notex). Beam (4319 px):
  mean 66,51,51 -> 34,18,11 (game 30,17,12), |d| 36.2 -> 12.9; SSIM 0.31 -> 0.25, gradient corr
  0.10 -> -0.09 (flat slab vs textured planks whose lines don't align with the game's, like the
  floor). Other regions: 7 px change outside the beam (+2px). Backup: out/z128_room_v2_PRE34_bak/.
  Also in the beam, NOT fixed: 06800212 pkt 424 (x=-1141, below the deck, ~280 px), an ABE
  overlay the game draws with TBP 10756 (matches no FileCode, best corr 0.69) over 0680ff36.
  BIND_OVERRIDE entries are now (FileCode, expected pf binding, expected ABE). On mismatch the
  export warns ("warning: BIND_OVERRIDE ...") and does NOT apply it; manifest records `applied`.
  Mutation-tested (wrong expected pf / wrong ABE -> warning, not applied).
- Fog (checked 2026-10-24): the game fogs. 7914 draws have FGE=1 (all room + deck), FOGCOL
  38,38,47 (= clear colour), F falls with depth w=1/q: w<50 F~245 (4% toward FOGCOL),
  w 200-400 F~160 (37%), w 400-800 F~111 (56%), w 800-1600 F=51 (80%). Not the floor's cause
  (floor w~40). Likely matters for the far walls/battlements. Neither our renderer nor gsrender2 fogs.
- 47.9% of triangles bound (verified). The rest (unbound group room_g40) are NOT UV-matched;
  "dump-unseen" is UNVERIFIED. Left grey on purpose.
- UNVERIFIED: deck (box fill, recover_z128_deck.py) and "seed+grow" bindings are not
  UV-checked; likely source of wrong textures (e.g. lower-right planked facets).
  The seed binding also matched the excluded 0..1-UV quads, so generic UVs can fool it.
- DMSH mesh header is 36 bytes (magic + 2 uint32 + 6-float bbox), then 12 floats/vertex.

## Open
1. Pose is a single-frame snapshot (body and cape both from 163235); any other frame or
   motion needs the animation system. Arm region reads ~20% too bright.
2. 0x2ee8 second pass (16x16 CT24, ABE on, 596 tris over the arm/boots): not reproduced;
   effect unmeasured, colour already matches without it.
3. Room: unverified bindings and grey roof facets/fascias (needs more angles or the static
   binding table).
4. Sky dome fits the hero camera only. Its guard (export_z128_sky.py) checks the room bbox,
   not depth, so other cameras can put the sky in front of walls; on failure, edit SKY_SCALE.
   Room packet exclusions are by index: re-check them if the packet decoder changes.
