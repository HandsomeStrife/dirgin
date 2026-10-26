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
- Export: `python3 -I native/assets/models/export_p000_gpu2.py tools/gsdump/dumps/z128_hero_163235.bin native/assets/models/out/p000_gpu3`
  (hero GS dump, frame 163235; gitignored game data, keep it)
  (writes mesh, atlas and `placement.json` with the render args). Old output kept in out/p000_gpu2.
- Cape = mesh block **0x19f70** of 07700001.bin (smd 'p000_L02', 112 tris), NOT a procedural
  object. It has 0 skinning batches, so the body decoder dropped it silently; it is skinned
  through its own path instead (see Cape skinning below). `p000_cape.py`: topology/UV/colour from the file, this frame's
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
  region): RGB 90,75,57 vs game 89,68,54, |d| 11 (was 102). (Measured prelit; with the lit
  render the 0x2ee8 glow pass IS needed, see Glow pass below.)
- Render (hero camera), then crop:
  env EGL_PLATFORM=surfaceless GALLIUM_DRIVER=llvmpipe ./native/gpu/render_scene_textured \
    -l native/assets/models/out/z128_room_v2/room.list \
    -m native/assets/models/out/p000_gpu3/vincent_p000.mesh \
    -t native/assets/models/out/p000_gpu3/vincent_p000_tex.rgba \
    -s 0.9639 -yaw 164.45 -x -1395.63 -y -98.75 -z -1110.51 -colscale 1.0 -charprelit \
    -charatest 0.77734 \
    -W 2628 -H 2048 $(camera_args from out/p000_gpu3/placement.json) -o OUT -tag T
  (OUT must exist). Hero camera fitted 2026-10-24 to 5164 dump room vertices (near error
  4.5 px -> 0.05 px): -fov 59.2749 -eye -1419.227,-115.571,-1129.797
  -at -631.348,-77.227,-515.163 -up 0.03023,-0.99926,0.02358. Placement for that camera:
  -s 0.9639 -yaw 164.45 -x -1395.63 -y -98.75 -z -1110.51 (always take both from
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

- Character lighting (2026-10-25, review 48): opt-in, in place of -charprelit:
    $(python3 -I native/assets/models/bmd_env.py --rig extract/_KEL/noPath/06800012.bin 0 123 hero) -chartwosided 3
  VU1 program D + FUN_005ab0b0 rig: env slot light A (dir = -light_dir1) + ambient
  (light_col_b), x obj+0x450/128 (=123 for Vincent), plus a runtime point light
  (HERO_POINT_LIGHT in bmd_env.py, FITTED to the dump's vertex colours, not decoded; source
  FUN_0026a710 / FUN_0030e208). Body vertex colours match the dump to median 0.9 levels.
  Grade (mean |d|, prelit -> lit): body 9.7 -> 0.9, mantle 2.6 -> 1.7, cape 4.4 -> 1.6,
  arms 6.1 -> 26.6 (too dark until the glow pass below). -chartwosided 3 lights the side
  facing the eye (cape winding is opposite to the body's).
- Glow pass (2026-10-25, review 49): opt-in, add to the lit render
    -charglow out/p000_gpu3/vincent_p000_glow.mesh out/p000_gpu3/vincent_p000_glow_tex.rgba
  0x2ee8 = ALPHA (Cs-0)*Ad+Cd, TEST ATST NEVER/AFAIL RGB_ONLY, ZTST GEQUAL, FGE on; Ad = the
  body pass's alpha = arm CLUT 0x2dd4 texel alpha (the glow mask). p000_glow.py bakes glow
  texel x mask per triangle; 64/149 tris reuse our body tris, 85 sit at dump positions (body
  UVs missing from our decode). Arms |d| 30.1 -> 16.3, SSIM 0.615 -> 0.811; boots 20.1 -> 21.6
  (glow overlap with the game only 28%: boot geometry). Dump-frame UVs/colours: dump pose only.
- UV matching (2026-10-25, review 50): dump<->mesh UVs match on the native 1/4096 grid
  (export_p000_gpu2.py sets char_dumpbind.ROUND = 4096 for itself; older scripts keep 512).
  The 1/512 grid failed 40% of dump body tris on exact rounding halves. Binding: exact 1684,
  uvmap 387, neighbour 240, group 15, unbound 2 (of 2328). Pose: dump 1559 / bone 232 /
  rejected 213; 59/71 bones solved. Placement changed (take it from placement.json).
  Glow: mirrored parts share UV triples (boots x2, some arm tris x4), so p000_glow picks the
  body tri nearest the dump position (median 0.003 units). 99/149 on our body tris, 50 at dump
  positions (48 mix UVs across index positions/sub-packets in ways no reading of our
  sub-packets produces; open). Lit+glow: boots 18.2/0.772 (was 22.1/0.710), glow overlap
  73% (was 19%); arms 12.0/0.876.
- Cape normals are smooth (default; sign-aligned per triangle so the front and back copies
  don't cancel); --cape-flat-normals = the old flat output. Cape 6.0/0.791 -> 5.5/0.805.
- SCREENSHOT REGISTRATION (2026-10-25, Vincent AND room): sample the PCSX2 shot at
  (1.015x+1, 1.00y) at 640x480 (= the 432/425.5 CRTC crop). The older "+9,-3" alignment is
  wrong (SSIM ~0.2); numbers graded with it before 2026-10-25 are suspect.
- Clips (phase 1b, 2026-10-24, review 44): rotation decode SOLVED in anim_rot.py (replaces
  anim_eval's quaternion formula; track walk unchanged). VU0 micro at vcallms 0x218: each
  TRACK Q key is an axis in spherical angles (x, y: 12 bits over [0, pi/2], octant from the
  sign bits) plus half-angle z (13 bits over [0, pi]); unit pi/8192; sin/cos via Taylor
  coeffs at 0x5b3ae0/af0. q = sin z*(cx*cy, cy*sx, sy)*signs, w = cos z. Between keys:
  hemisphere flip, nlerp. Replaces the local bone rotation. Error vs 55 dump-solved bones:
  6.8 deg median (bind 16.2, old 72). Best fit h_swtch f58 (0770000b), not confirmed as the
  clip playing at 163235; h_wk 15-17 deg every frame; hands worst (36-39 deg).
- Clips (phase 1c, 2026-10-25, review 46/47): translation + scale decoded in anim_rot.py
  (FUN_005aefd0 + micro 137..216). P1 = local translation /256 (empty -> bind T; bone 1 root
  motion, bone 2 hips), P2 = per-axis local SCALE /256 (empty -> 1); S_w = S_parent*s,
  p_w = p_parent + R_parent(S_parent*t), skin basis axis k scaled by S_w[k]. The game does NOT
  clamp the interp factor (sample() extrapolating is correct).
  `export_p000_gpu2.py dump.bin OUT --clip h_swtch --frame 58 [--in-place] [--no-cape]` poses
  the body from a clip (cape stays dump-posed; default = dump pose, unchanged). h_swtch f58 vs
  dump-posed verts: median 0.109 / rms 0.251 units (bind 0.861); f59 0.089. Undecoded: vf26-28
  previous-pose blend; clip playing at 163235 unconfirmed (h_st untested).
- Cape skinning (2026-10-25, reviews 51-54): smd p000_L02 @0x19f70 has a weighted skin path at
  +0x18: 28 records (FUN_005b06d0), bone-local xyz x meshScale/4096, bones 28-79 incl. the
  cape's own chains. Draw slots -> the 82 cloth vertices via the draw-group +0x14 list
  (FUN_005b1260). `--clip X --frame F` now poses the cape from its bones (default); without
  --clip it stays dump-posed (unchanged). h_swtch f58 skinned vs dump cape: median 0.61 (pinned
  0.23). Code: p000_cloth.py (decode, sim, validate), p000_cloth_render.py (render/grade helper).
- Cloth sim, decoded (FUN_005b0d30), opt-in `--cape-cloth`: group +0x24: gravity 0.2/tick,
  velocity = movement x 0.1, 4 passes over 336 stretch-only constraints (gain 0.8), 20 pinned.
  Free vertices pulled toward the skinned target by max(k, 0.1), k = (1 - rem/total)^2; window
  2 x u16 at clip +0x1a when clip byte +0x19 == 2 (h_wk 60), re-armed on every clip (re)start
  incl. loops and on turns > ~7 deg/tick; outside it the cape sits on its target. World space.
  Dan judged our sim STIFFER than the bones alone (review 53), so it is off by default; two
  attempts used. Unproven: clip frames per game tick (assumed 1). Next evidence: a PCSX2
  capture of Vincent walking. --cape-cloth with --frame past the clip length loops the cape
  but not the body.
- Cerberus gun (2026-10-26, review 58): export_cerberus.py <dump> <p000_export_dir> <outdir> [--cam CAM.json]
  -> cerberus.mesh / cerberus_tex.rgba / cerberus.json (Vincent model space). Render: add
  -charprop OUT/cerberus.mesh OUT/cerberus_tex.rgba (lit/prelit like the body; opt-in).
  Attach DECODED: FUN_00316270 weapon slot N -> marker N+0xf (gun: 15); FUN_005ab8f0 draws
  slots after the body with child world = player world x FUN_0030bef8(palette[bone] x
  T(marker xyz)). mk2: +4 u16 count, +8 -> {f32 xyz, s32 bone}. Vincent marker 15 = bone 56
  (socket under hand 55, no verts, offset 0). Palette56 = bone 55's bind->posed rigid fit
  from vincent_p000.mesh x bind56 (hook: export_p000_gpu2 could write per-bone matrices).
  Parts drawn: frame0 + barrel0 + barrel1 (dump UV triples + geometry; barrel3/4 share UVs).
  Textures = 07770582 w022_1..3, identical to the dump uploads.
  Vertex error vs dump gun tris (median world units): hero 0.085, 185138 0.114, 185126 0.078
  (bone-55 mutation: 2.6). Grade: cerberus_grade.py (region SSIM drops with the gun:
  the background dominates the region). Hero: hair hides most of the gun.
  Gun's own skeleton is bind (motions 07770580 not decoded).
  --cam takes a capture's _cam.json (tools/gsdump/dumps); default is the hero camera.
- Walk dumps (2026-10-25, reviews 55-57): tools/gsdump/dumps/z128_walk_185126/185138 (.bin/.png/_cam.json).
  Same room z128, other cameras: fit_dump_camera.py (RANSAC on UV-matched room tris vs the hero
  dump; 0.03 px, fov 59.27, depth scale 1.000). Vincent's TBPs move per capture (185126 body
  0x2e60 / cape 0x2ff4 / glow 0x3074; 185138 0x2d4c / 0x2ee0 / 0x2f60): export with
  --dump-camera <dump>_cam.json. Its "bind_ref" (out/p000_gpu3/binding.json) lets the hero's
  stronger exact/uvmap CLUT evidence override a capture's weaker one (mirror entries skipped).
  Textures are read as they were at draw time (DrawTimeTex): in 185126 the arm/boots CLUT 0x2f60
  and head CLUT 0x2e5c are overwritten later in the frame (the old largest-upload pick was garbage).
  Vincent is STANDING in both (body static over the 4 vsyncs): h_st f160 (0.06) / f94 (0.03);
  h_wk best 0.43. The hero frame is h_st f91 too (0.059 vs h_swtch f58 0.109; cape 0.25 vs 0.46).
  p000_clip_scan.py ranks clip frames (approximate, rigid per bone).
- Cape vs cloth: the dump cape sits 0.08-0.35 off the skinned target and closes ~15%/vsync.
  The decoded sim either sags (up to 2.5) or snaps in 1-2 ticks; it only fits in its last 2-4
  window ticks. Sim vs skinned: hero 0.167 vs 0.296, 185126 0.116 vs 0.154, 185138 0.085 vs
  0.068. Gravity off improves the sim (hero 0.127). So the stiffness is likely a porting error
  (suspects: gravity units, blend step). Cloth steps every vsync; body ~2 clip frames per 3
  vsyncs (unproven).
- Lit+glow, Vincent region: 185126 8.17/0.815, 185138 9.40/0.712. Our room is wrong from the
  185138 view (a near facade where the game shows rooftops); glow reads too strong in 185126.
- Mirrored-UV pose fix (2026-10-26, review 59): legs/arms share UVs, so p000_pose could take the
  LEFT thigh's dump points for bone 87 (right thigh), in all three dumps (bone 87 at 102-117 deg
  vs its parent; visible only in 185138). `_midline()` finds the body midline from mirrored vertex
  pairs; each vertex keeps its bind side, the plane moves with the nearest central bone (81, 3).
  Wrong-side candidates are dropped when the vertex has some on its own side, and on limb bones
  also when ALL are > STRICT_MARGIN=0.5 across. Bone 87 now 27-28 deg. Silhouette IoU: 185138
  0.856 -> 0.908, hero 0.847 -> 0.860, 185126 0.831 -> 0.830. Cost: thin fringe on the near shin
  (bone 84) in hero/185126 (walking feet land near the midline; margins 0.1/0.3 were worse).
- Decoder: right-side pieces (2026-10-26, reviews 60-62): the right knee pad / crotch / inner hip
  (and a chest/arm patch) were missing because of a DECODER bug, not mirroring. Group header
  byte +2 = draw-descriptor count; render_skinned.enum_descs stopped at the first header that
  wasn't 0xffff000N or a cnt/next DMA tag (L01.g0 d1 = 0x80010001), and the VIF parser then read
  d2's header 0xffff0003 as a 2040-byte UNPACK, skipping 7 index sub-packets (bones 86/87).
  S01.g1 lost a 6th descriptor the same way. Fix: enum_descs reads the count (falls back to the
  scan). p000 2328 -> 2389 tris, 0 unreferenced pool slots. Game-wide: 90 containers / 501 groups
  change (+22.8k tris; p005 S01.g2/g3 0 -> 384 each). The dump-driven mirror workaround from
  reviews 60/61 was removed; placement.json has no "mirrored" key. Gun error 0.086 / 0.117.
  Open: the last descriptor of a group still parses to EOF (can leak into the next group; fix
  touches p000_cape/p000_cloth call sites). Group header carries per-vertex blend weights
  (0.8, 0.6...) that our skinning ignores (shared slots are overwritten): unexplored.
- Binding from files (2026-10-26, review 63): export_p000_gpu2.py ... --bind-from-file (opt-in;
  default unchanged). smd_material.py: draw-descriptor byte 0 = material slot (FUN_005b19b8: TEX0
  = table[byte] @0x5b37c0; +4 bit 1 = alpha test AREF 100, desc[2] = env-map slot). Table =
  FUN_00259058 over the texture container's entries in order (skip rst whose +0x22 mask has the
  variant bit; palette-only entries, 0 images / +0x2f parent, reuse the parent image with their
  own CLUT; VRAM bump allocator FUN_00414730, CLUT 4 blocks then image). p000: 0-3 = p000_1_a
  with CLUT a/b/c/d, 4 = p000_2, 5 = p000_3 (cape). Agrees with 100% of 'exact' tris in
  hero/185126/185138 (slot+1 mutation: 0%); file textures = dump texels. Hero Vincent region
  6.25/0.8161 -> 6.25/0.8167. Uncaptured models: smd_material.py --render CONT TEX OUT (p005
  7348/7348 bound; Dan: face/head/hair look wrong there, hair maybe missing transparency).
  Open: glow TEX0 = envmap1 at p000_ta's in-order address; the glow may be env-mapped (desc 0x40).
- Uncaptured-model render (2026-10-26, review 64, Dan: "Perfect!"): smd_material.py --render CONT TEX
  OUT draws like the game: no culling (strip winding is inconsistent; winding cull blanked half of
  p005's face), parts last-to-first (FUN_005b2ef8; the hero dump draws the hair first), descriptor
  flag bit 1 = ABE (Cs-Cd)*As+Cd + ATE GEQUAL 100 / AFAIL RGB_ONLY (only As >= 100 writes Z),
  flag 0 = opaque; confirmed on every p000 draw in the hero dump. p005: 1314 flagged tris (hair
  slots 5/6, cloak 7); L01@cf00 is a separate detailed face block, not a copy. Viewer light only.
  Open: the p000 GPU export draws the hair opaque (268 flagged tris; 38% of p000_2 texels < 100):
  needs flagged tris in an atlas region that keeps texture alpha, plus the -charatest pass.
- p000 hair alpha + game draw order (2026-10-26, review 65): flag-bit-1 descriptors (301 body tris:
  hair cards, some cloth edges) map to an atlas copy ('A', tex0) that keeps texture alpha; drawn
  with -charatest like the cape. Draw order DECODED: FUN_005b2550 draws a group at once unless
  group header word bit 0 is set; those go on bucket (w&0x3e)>>1 (LIFO), drawn by FUN_005b26b0
  after all parts (bucket 0 first). p000: opaque groups part by part (last part first), then
  L02.g1, cape, S02.g0, hair alpha (buckets 5/10/20/30); matches the hero dump run for run
  (smd_material.group_draw_keys; --render now uses it, moving 1-6% of the p005 preview pixels).
  The mesh stays in binding.json order (export_cerberus, swap_cape); render_args carries
  -charruns S:C,... (render_scene_textured: ranges drawn in order, must cover all, max 64; p000 16).
  Head+hair box (285,189)-(356,296) lit 5.61/0.768 -> 5.05/0.793; Vincent lit 5.02/0.831 -> 4.94/0.834.
  Open: the dump has ~half our tris per run; the VU may cull back faces (unverified).
- File binding is the default (2026-10-26, review 66): export_p000_gpu2.py binds every triangle from
  the files; --bind-from-dump = old behaviour. Every run prints "file binding vs dump 'exact'
  triangles" (100% in hero/185126/185138; slot+1 mutation 22.6%). bind_ref applies only with
  --bind-from-dump (redundant otherwise). Switch alone: 79/76/17 tris change texture/CLUT,
  136/65/0 px, pose vertices unchanged. Gun 0.086/0.079/0.117.

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
- Fog (2026-10-24; renderer b8d529f; exporter + bmd_env.py committed with this note): the game
  fogs. 7914 draws have FGE=1 (all room + deck). bmd_env.py reads the BMD header env block
  (FUN_0026afe8: 32 slots x 0xC4 at +0x24) and refuses BMD version <= 3 at +4. export_z128_room.py
  puts `@fog 38 38 47 0 810 0.2` and `@nofog room_sky.mesh` at the top of room.list, from slot 0
  of ENV_CHUNK = 06800012. Matches the hero dump: FOGCOL 38,38,47 and floor 51 exact,
  ramp F = 254.56 - 0.3150*w (within 1 level of the hand fit; Dan approved, review 41).
  CAVEAT: the 13 chunks disagree on slot 0:
    06800012/112/212 (35.7k tris)  38,38,47  far 810   floor 51   <- used
    06800312         (26.2k tris)  39,39,46  far 640   floor 128
    06800412-0a12    (93.3k tris)  39,39,46  far 640   floor 240
    06800b12/0c12    (22.2k tris)  20,20,29  far 1280  floor 240
  Which chunk's env is live at runtime (by area/camera?) is NOT decoded; 06800012 is chosen
  because the dump confirms it. Every build warns "chunks disagree"; --env-chunk/--env-slot
  override. far <= near and floor >= 255 write no @fog (with the reason); a negative floor is
  clamped with a warning. Lighting fields (2 light colours, 2 directions, int at +0x90) only
  feed character lighting (room colours are pre-baked); using them means dropping -charprelit.
  NOTE: live out/z128_room_v2/room.list predates this; re-export to get the @fog lines.
- 47.9% of triangles bound (verified). The rest (unbound group room_g40) are NOT UV-matched;
  "dump-unseen" is UNVERIFIED. Left grey on purpose.
- UNVERIFIED: deck (box fill, recover_z128_deck.py) and "seed+grow" bindings are not
  UV-checked; likely source of wrong textures (e.g. lower-right planked facets).
  The seed binding also matched the excluded 0..1-UV quads, so generic UVs can fool it.
- DMSH mesh header is 36 bytes (magic + 2 uint32 + 6-float bbox), then 12 floats/vertex.

- Chain fill (2026-10-24, review 43): bmd_chains.py gives each decoded packet its
  chain id (raw STCYCL starts; a gap after MSCNT = chain trailer). decode_packets now records
  `off` (stream offset) so dropped geometry-less packets don't shift indices (the earlier probe
  skipped chunks where counts differed; all 13 chunks are covered now). export_z128_room.py:
  an unbound packet takes its chain's FileCode only when every bound packet of the chain
  (after BIND_OVERRIDE, excluded packets ignored) agrees. --no-chain-fill turns it off. Manifest:
  chain_fill.filled, chain_fill.mixed_chains_left_unbound (14 chains, 25 packets, left grey).
  Bound tris 47.93% -> 51.77% (768 packets).
- Suspect audit: 50 within-chain disagreements (v3, all chunks) checked with dump UV triple +
  vcol within 12 + screen position within 4 px (hero camera projection); TBP -> FileCode by
  texture corr over all 138 0680ff* (agrees with 627/653 existing bindings it can see).
  BIND_OVERRIDE added: 06800012/179 -> 0680ff32; 06800112/1095,1096,1107,1108 -> 0680ff05;
  06800212/579 -> 0680ff2b (off-screen); chain anchors 06800112/1082,1093 and 06800212/1048
  -> 0680ff05 (without them the fill copied a wrong 0680ff24 onto 7 dump-visible packets).
  TBP 10756 = 0680ff05 (corr 1.000), new; this is the pkt 424 overlay texture ("matches no
  FileCode" above was a search over bound textures only). The other suspects are dump-unseen, left as-is.
- Hero view: one region changes, the pale patch behind the far-right tower (x544-605 y141-175
  at 640x480): mean 37,37,44 -> 31,31,37 (game 31,30,36), |d| 7.0 -> 2.3, SSIM 0.79 -> 0.91.
  Elsewhere < 5 levels. Backup: out/z128_room_v2_PRE43_bak/. room.list now has the @fog lines.
- Dump-contradicted bindings (2026-10-24, review 45): 15 of the 16 fixed as BIND_OVERRIDE
  (same standard: UV triple + vcol within 12 + screen within 4 px; matches 0.1-0.6 px, vcol
  diff 0; no hit on the old FileCode there; all non-ABE). List and n/m counts in
  export_z128_room.py. 06800212/1064 is the weakest (1/10 tris; chain-mates are ff33).
  Skipped: 06800212/818 (0.8 px but vcol off by 31). Hero view: 0 px change; the game draws
  these 54 tris but all are covered (removing them from the dump render: 0 px; control 6318).
- bmd_chains.py walks VIF commands by size to each packet's MSCNT (the old 0x17 byte search
  hit vertex data: 54 false breaks). Unparseable packet -> warning + chain break (fails toward
  less fill); chunk with no trailer -> warning. Chains are 1-47 packets. Chain fill 768 -> 774
  (0 dump-contradicted), mixed chains 14 -> 13. 310 fills rest on a single anchor (warning,
  manifest chain_fill.single_anchor); a chain holding a BIND_OVERRIDE that was NOT applied is
  not filled (manifest override_skipped_chains_left_unbound). Backup: out/z128_room_v2_PRE44_bak/.
- Suspect: 06800912/171 is pf-bound to the sky texture 0680ff35 (small ABE object, off-screen);
  chain fill spreads it to 168-170. Unverified.

- Browser client (2026-10-26, workers 0046 + 0048, review 67/70): `python3 native/assets/models/export_web_room.py` writes `native/web/data/` (gitignored via `native/web/data/**`): scene.json, float32 12-float .bin meshes, raw .rgba textures (formats in the script docstring). Viewer `native/web/index.html` + `main.js`: three.js 0.169.0 (jsdelivr), RawShaderMaterial copying SHADE_FS (MODULATE x2, per-vertex screen-linear fog, two-pass GS ATE, native draw order). Served by `/srv/tools/dirge-play` (nginx; port 8796 on LAN + tailnet only). Hero camera vs render_scene_textured -colscale 1.0 -charprelit -charatest 0.77734: mean |d| 0.15, SSIM 0.998. The HUD shows eye/dir/yaw/pitch; `?cam=ex,ey,ez,yaw,pitch` restores a view. The Report button (R; `report.js`) saves to /srv/shared/dirge-port/reports/<id>/ (marked.png, plain.png, meta.json with the camera, box, note and GPU/ray picks of draw + triangle). The backend is the dirge-report container (report_api.py, application/json only), behind nginx at /api/report. Flicker: unbound room_g41 is coplanar with textured walls (g19/g04/g11/g25) and z-fights (natively too, just outside the hero view); main.js pushes untextured draws back with polygon offset (1, 4), and `?zprio=0` turns it off. Remaining: overlapping same-texture triangles within one group (~0.03% of the screen).

- Collision (worker 0045, 2026-10-26): z128 is 13 files, 0680{00..0c}11.bin, 100% decoded (no AABB fallbacks). `make -C native/assets/collision z128web` writes out/web/z128_collision.bin (DCOL v1, FORMAT.md §8); `cid_query.py` holds the reference ground/capsule/segment queries. **The world is −Y up** (FORMAT.md used to say Y-up, which was wrong). Vincent's start feet at −98.7 sit on the deck top at −98.44 (error 0.22–0.33 over 3 dumps). Player set = sub-tree +0x58 minus mask bit 0x80; that mask rule is UNPROVEN (see the FORMAT.md note). Floors: (b−a)×(c−a) has n.y ≥ 0.6.
- Web rig (worker 0047): `export_web_vincent.py export <dump>` writes native/web/data/vincent/vincent.glb + vincent.json. It is a skinned glTF with flat joints, each joint's animation carrying its model-space TRS, because the game's non-uniform scale inheritance can't be a standard node hierarchy. Clips h_st (160 frames), h_wk (40) and h_rn (20) run in place at an assumed 60 fps, with root motion in the JSON (walk 0.525, run 1.665 units/frame). `verify` checks the glb against `export_p000_gpu2 --clip X --frame N --in-place`: the body is exact; the cape is off by up to 0.19, from trimming 5 weights to 4.
- Browser collision (worker 0049): `native/web/collision.js` ports cid_query.py; `node native/web/test_collision.mjs` matches it bit-exactly (7,270 checks), and the 0x80-mask and floor-threshold mutants both fail the test.
- Playable page (worker 0050, review 68): `native/web/play.html` + play.js + player.js. Fixed 60 Hz controller, speed-driven animation (rate = ground speed / root speed; walk 0.505 and run 1.60 units/tick after the 0.9625 char scale), and a follow camera that pulls in on col.segment hits. Dan, review 68: movement is right, but he doesn't sit on the platform correctly and falls through platforms that should be solid. Follow-up is worker 0053.

- Room textures (worker 0051, 2026-10-26, commit 732bc8b): chain header word +0x98 is a texture slot into the BMD's own texture list (FUN_0026ce60 / FUN_00265bf0); 720/762 dumped packets agree, and most of the rest are fire flipbook frames (0680ff04-06, pinned via HERO_FRAME; the flipbook table isn't decoded). Untextured room triangles went from 48.2% to 0.01%. Header +0xa9 = second-pass (additive) switch, 0 for all z128 chains. Header +0xaa = visibility group, hidden via the mask at EE 0x004e8710 (story state; sea-of-flames hides groups 0/19/25/56/57). Exact cross-chunk duplicates and coarse coplanar wall copies are dropped (room_overlap.py). `--bind pf --show-hidden-groups --keep-duplicates --keep-covered --no-underlay` reproduces the pre-0051 output.
- Floor flicker (worker 0054, report 04b2): later chunks store coarse copies of surfaces lying on an earlier chunk's plane (e.g. 06800c12's flat 160-unit ff24 floor over 06800112's tiles); the game shows the earlier chunk. Those triangles go to `<key>@under` groups (room.list column 5, scene.json under: true), which main.js draws with polygon offset (`?zunder=0` turns it off). Unbound triangles never cover anything. PS2 room pass: PSMZ24, ZTST GEQUAL, Z writes on, no decal bias, so depth precision is not the cause. Flicker: 1903 -> 547 px over 24 views, report view 1377 -> 4. Left: bevel slivers inside one chain at the deck edge (06800212 chain 5).

- Play fixes (worker 0053, review 1793034993921252799): the game's packet test is `mask & ~(flags|0x100) != 0` (machine code 0x26f934). Probe flags 0x18d/0x18b/0x187 select class bits 0x02/0x04/0x08. Player set = sub-tree 1 with any of those bits. Floors are two-sided, |n.y| >= 0.55. Sub-tree 0 floats 1-4.8 above the visible walkway, so it is not used for floors. Fall-through spots from the start: 213 -> 2. The glb origin is 11.67 below his soles, so play.js measures the sole at load. Gravity (0.05/tick², cap 6, not from the game) and a Report button were added to play.html. Known issue: two-sided floors let him step up through a slab underside within 5 units, and ceilings no longer push the capsule (worker 0055).

## Open
- Post foot vs cape (review 40, left on purpose by Dan): the dump draws the near post
  (54050-54113) before the cape, and the hem triangles over its foot (= ours exactly, texel
  alpha 127) pass depth GEQUAL, covering 221-247 of 250 foot px in all 4 dump frames. Ours
  matches the dump; the screenshot is likely a different cloth frame. The cape/body average
  all 4 dump frames. Revisit with real animation/cloth, or if clipping shows elsewhere.
1. Pose is a single-frame snapshot (body and cape both from 163235); any other frame or
   motion needs the animation system. Arm region reads ~20% too bright.
2. Glow: 50/149 tris still at dump positions (body tris our sub-packet reading doesn't produce).
3. Room: unverified bindings and grey roof facets/fascias (needs more angles or the static
   binding table).
4. Sky dome fits the hero camera only. Its guard (export_z128_sky.py) checks the room bbox,
   not depth, so other cameras can put the sky in front of walls; on failure, edit SKY_SCALE.
   Room packet exclusions are by index: re-check them if the packet decoder changes.
