# Handoff — sea-of-flames scene (updated 2026-10-24)

**Rule (Dan, 2026-10-26): faithful port, not a remake. Decode and translate the game's own logic; no invented behaviour; mark stand-ins `// GUESS:`. See /CLAUDE.md.**

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
  STQ fixed 2026-10-27 (worker 0086): it applied Q twice to S,T on every FST=0 draw (room AND characters), so
  pre-fix renders had no cape, no planks/railings and smeared hair; that was the "texture decode gap". Now
  23.0-24.5 dB vs PCSX2 on all 3 dumps; nearest-only (no bilinear), so clouds look blocky. See tools/gsdump/README.md.
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
  DECODED since (worker 0085): the "point light" is light 2, the room light picked from the BMD
  light grids (06800111/06800211, FUN_0026a710 -> FUN_00270d08), colour rgb*k*100/d^2, not x
  +0x450; MP+0x450 = 122 in the save state. play.html lights body + hair this way
  (export_web_light.py -> data/vincent/light.json); the cape stays prelit (its normal source
  is not decoded). Rows: notes/fidelity-audit.md "Character lighting".
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
  Parts drawn: frame0 + barrel1 (barrel0 is never drawn: worker 0091; dump UV triples + geometry; barrel3/4 share UVs).
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

## Display output (worker 0082, 2026-10-27)
- native/web/output.js = the PS2 PCRTC merge the game sets every frame (FUN_0040d228 from the flip FUN_0040ce28): 128/255·fb(y) + 127/255·fb(y−1 line, x−1/6 px). On by default in play.html and index.html, `?outblur=0` = debug off. Scripts that render by hand must call `__play.output.render(root, cam)` / `__view.render()`, not `renderer.render`. PCSX2 (both configs) runs `pcrtc_antiblur = true`, so its captures lack this blend; set it to false for a true reference.
- Native resolution (worker 0087, 2026-10-27): play.html/index.html now render the game's 432x512 framebuffer with the game's projection (FUN_0040d478/FUN_00263c70: scrz 299.97 = 448/2/tan(73.5/2), ay 1.5, centre 213,256; struct 0x4fc7d0), merge at VCK resolution (2553x512), show it 4:3 in the canvas (linear, GUESS). `?res=full` = old sharp canvas render (improvement option). Report picks map canvas px -> framebuffer px (picks[].fb); __idBuffer is 432x512 in native. Hero frame vs GS oracle: SSIM 0.79, shift 0.1 px (full: 0.70). Review: /srv/shared/dirge-port/review/native-res-compare.png. Open: GS sample point (+0.5?) needs a PCSX2 native capture.

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

- Actions (2026-10-26): player.js holds the state machine and actions/index.js loads actions/*.js (ACTIONS.md). Reload is on F (worker 0056) and aim/shoot on RMB/Q + LMB (0060; bullet speed, range and rate are guesses). The ladder is on E with W/S (0059): ladders come from 06800001 section 8 via export_web_ladders.py into data/z128_ladders.json. The game snaps the character origin 5.6 from the face, and the climb clips add a 5.5 forward body offset (c_ld_u bone-1 z) that the in-place export removes, so the web port puts the feet 5.6 − 5.5·scale ≈ 0.3 from the face. Rungs sit on a fixed grid from the bottom trigger (4.5 units each). `?ladder=<index>` (0 = the ladder by the start deck) warps there for testing. Open: top/bottom pairing is nearest-top with no one-to-one check, and the exporter has no bounds checks on the records.

- Walk mesh (workers 0055 + 0061, Dan: "Collision is good!"): z128 sets script flag bit 13 (06800001 section 4 +0x44 bit 0; FUN_0053c140). FUN_0034db80 then puts the player in physics-body mode 0xc, which walks the sub-tree-0 tag-0 convex polygons (FUN_00357d48). Neighbour links per edge sit at +0x10, flags at +4. An edge with no neighbour is a wall (slide); 0x10 = one-way drop (ladders); 0x02 = chunk crossing (argument inferred); 0x80 = wall only for bodies with flag 0x200 (`block80`). The mode has no slope, step, ledge or headroom test; walk mode 10's 7.5 step / 0.2 follow / 5 below applies elsewhere. Airborne, the body keeps walking the same mesh, so jumps can't clear the railings. cid.c also decodes the tag-1 primitives (sphere/box/cylinder, unit 10, matrix +0x60). DCOL v2 adds a walk-mesh section. node native/web/test_collision.mjs: parity plus 22 absolute checks; sweep_collision.mjs gives the reachability sweep (0 pass-throughs). Follow-up (worker 0062): the 0x02 crossing step is decoded (MIPS at 0x358c60, FUN_00359f38, search from 7.5 above to 200 below, no flags) and tested on all 22 seams. An airborne body with no polygon stays put. The DCOL loaders reject corrupt files. Open (latent, block80 bodies only): a crossing can land a block80 body on a 0x80 polygon, which findPoly then skips, so it gets pinned. The game keeps the landed polygon (body+0x110); the port should too, if NPCs ever use block80.

- Hair vs cape (worker 0068, approved): there is no hair-over-cape rule. In all three dumps every Vincent draw has ZTE=1, ZTST=GEQUAL, ZMSK=0, and the cape and hair alpha share TEST 0x5364d. The order (FUN_005b2550/26b0) puts the cape before S02.g0 and the hair alpha groups; the glb drawOrder and play.js depth state match. In the dump frames (front/side views) the cape never wins depth over the hair. On the web, seen from behind, 0.3-2.4% of hair pixels sit behind the cape. Cause: the cape's shape (bone-posed, weights trimmed 5 to 4, no cloth sim). The faithful fix is porting the cloth sim.
- Hair vs cape, measured (worker 0080): NOT a port defect. In the character frame our cape targets (all 5 weights, clip at the trace's frame) match the game's cape_target to median 0.05 (idle, walk shape); 0074's 0.3 walk / 1.2 run offset is one frame of movement, and FUN_005ab8f0 passes the same view x MP+0x10 matrix to FUN_005b2ef8 for the body AND the cape, so it doesn't move the cape against the hair. Hair vs dump (hero, h_st f91): median 0.05. The 'dumps show 0' was because the dump cameras face him; at the play camera the game's own geometry (dump hair + dump cape) hides 1.0% of the hair from behind, 1.8% at 45 deg, 0% side; walking (decoded body + the trace's cloth offset) 2.7% / 0.4% / 0%. play.html: idle 0.8% / 0% / 0%, walk 2.1% / 0.2% / 0%. The hair ends go inside the collar in the game too. Image: review/hair-cape-compare.png.
- Aim while jumping (worker 0067): the game has no overlays or bone masks. Each state plays whole-body clips via FUN_00345818; the aim pitch is a 2-bit zone (char+0x590) choosing a level/up/down pair. Jump while aiming (FUN_00598f50) plays the h_jp*_a sets (slots 0xbc-0xcb) when slot 0xbc is loaded. Fire in the air (FUN_00596830) plays h_jp_f (0xc1) when it is loaded. Container 0770000b lacks these, so the game would play the plain jump. Which container the level loads is not yet decoded.

- Gameplay oracle (worker 0069): `tools/trace/trace.py scripts/X.json` writes `out/X.jsonl` (gitignored): one record per 50 Hz frame from the sea-of-flames state, with input injected by a pad-read hook (0x257ebc) in a copy of the state. Two runs are bit-identical. Fields and confidence are in tools/trace/README.md. Measured: run 60 u/s (1.0/tick), walk 15 u/s, walk/run chosen by a threshold (only full stick runs). Clips advance by rate x weight/128 x step (FUN_005aa2a8, called from FUN_005aa5b8); rate 0.5 gives 30 clip frames/s for h_rn, and loops reset to 0. PAL step = 1.2 ticks per vblank. Cape: a cloth step (FUN_005b0d30 -> b38 integrate, 09f8 x4, 0c50 pull to keyed) runs once per drawn frame during a settle window (h_wk 60, h_rn 28, a turn 100 frames), keyed otherwise; the window does NOT re-arm on loops (p000_cloth.py is wrong there). Jump apex 13.09, 36 frames in the air. A mid-air shot gives NO hop (actions-research's 3.3 hop is wrong). The first R1 press only raises the gun. The browser's walk/run/clip rates are 1.6-2x too fast.
- Fidelity audit (worker 0070): notes/fidelity-audit.md, 54 behaviours: 15 decoded, 4 data, 8 guess, 24 invented, 3 missing. Player path: mode dispatch FUN_0034c388 (char+0x1c8: 10 walk, 0xb air, 0xc physics body -> FUN_003590c8 -> FUN_00357d48 -> FUN_0035a440); intent FUN_005747a0 -> FUN_005786e8 -> FUN_00578748 (move) and FUN_00579570 (weapon); input FUN_00333150/FUN_003320c0. z128's jump takes the physics-body branch of FUN_00575768 (gravity ~0.065, height from cfg+0xc4, air control in FUN_00578748), so jump.js's walk-mode values are wrong for z128.
- Inventions stripped (worker 0078, 2026-10-27): play.html now runs only the translated locomotion (player.js), the cape cloth, a static gun (gun.js) and the ladder (Space = ✕). jump.js, shoot.js and reload.js are deleted (git has them): the jump waits for FUN_00575768's physics-body branch, shooting/reload for worker 0077's weapon translation. The camera is a labelled GUESS orbit with only the decoded ±54° pitch limit. Audit sections 2-3: INVENTED 24 → 0; every remaining stand-in is a `// GUESS:` row.
- Worker 0065's shoot-layer changes (aim layer, overlays, weapon values) were NOT committed: mostly invented. They are saved as /srv/shared/dirge-port/patches/w0065-shoot-layer.patch, and the decoded parts get re-applied via the weapon state machine translation (FUN_00579570).

- **Bring-your-own-ISO build** (worker 0071): `tools/byo/build_web.sh /path/to/your.iso` builds
  native/web/data from a user's own PAL disc (no GS dumps, no PCSX2); `tools/byo/serve.sh` serves it
  on 127.0.0.1. User docs: docs/BROWSER.md; instructions to hand an agent: docs/AGENT-SETUP.md.
  The model scripts take repo-relative paths and a `--no-dump` path. Differences from the capture
  build: no sky, and Vincent's placement is a GUESS (`NODUMP_PLACEMENT`, 3.5% larger); both listed
  in notes/fidelity-audit.md. The build runs `python3 -I` (a pip --user install is not seen).

- Body pose / placement (worker 0083, 2026-10-27): the game's bone matrices are *(MP+0x3f8) (`TRACE_FIELDSET=pose`, scripts c_run_pose / c_run_pose_blend). play.html now builds the locomotion pose the game's way (player.js poseLocal: local blend at MP+0x408 then compose; vincent_local.bin), zeroes stale clip weights, places the model at unit scale with its origin on the character position (no 0.9625, no pivot, no FEET_OFFSET; glb rootT = 0), and skins the cape with the previous frame's character matrix (MP+0x10, written by FUN_00343de8 -> FUN_005a6f40; lag traced). Cape target vs game (page replay, mean): idle 0.63 -> 0.000, walk 0.94 -> 0.014, run 3.19 -> 0.003, turn 0.93 -> 0.014. NODUMP_PLACEMENT s = 1.0 (decoded); the dump build's static index.html keeps its dump-mesh fit (s 0.9625 is that mesh's own fit; the clip-posed hero fit gives 0.9966).

- Hair vs cape while moving (worker 0088, 2026-10-28): NOT the cape logic, it's the camera. Per tick, page cloth vs game cloth (cw_walk/run/turn, all 82 verts): pos 0.011/0.002/0.010 mean; hair covered from behind by the cloth: page 0.154 vs game 0.156 (run). Rendered in-between frames at 60/144 Hz stay inside the tick values (no dip from ctrl.lerp, crossfades, the MP+0x10 lag or the heading lerp). Hair (j05-j12) and collar (j33/j35/j68/j70, prims 13/15/16) bones match the game (median 0.000). The traced game camera while moving sits at head height and looks level (17.2 up, 38 back); the page's GUESS FollowCamera sits ~30 up / ~50 back, looking down 17°, onto the collar top where the hair goes in. Same geometry from the game's camera hides it (turn-walk hair-over-collar share 0.129 page cam vs 0.029 game cam). Fix = translate FUN_0032ace0 (camera). No code changed. Image: review/hair-cape-moving.png (the "game" rows are page geometry from the traced game camera, not an emulator capture). Scripts: worker 0088 scratchpad moving.py / camview.py.
- Follow camera translated (worker 0089, 2026-10-28): player.js `FollowCamera` is now FUN_00330478 -> FUN_0032ace0 for camera type 0xc / view mode 1 (base follow, look-at 17 up, eye 38 back, follow nudge, auto turn-behind, look stick + the game's USB-mouse path, 5-ray collision with slow ease-out). It ticks once per game frame BEFORE ctrl.step; the keys use its matrix. Checked on six new PCSX2 traces (`TRACE_FIELDSET=cam`, scripts in worker 0089 scratchpad/scripts): eye <= 0.0075, look-at <= 0.001, yaw <= 6e-6 rad, open and closed loop; locomotion replays unchanged. RAM != ELF for DAT_0046e3f0 (73.5), 0x46e500 (60), 0x46e3f8 (50): read the save state, not the ELF. GUESS left: camera collision file set [1,2], 1 px = 1 mouse count, spawn = save-state camera. Hair over collar while moving now equals the game camera's (turn-walk 1.3% vs 1.3%; was 12.9%). Image: review/camera-translated.png. Audit: "Camera" rows.
- Camera review fixes (worker 0090, 2026-10-28): the base-follow |dy| < 15 test now wraps the rate block as in FUN_0032ace0 (15+ drop: base lands on the player; FUN_003264e0 = DAT_0046b1d8 == 0, 1 in RAM, so rates x step capped at 1). reset() is FUN_003285d0 (snap flag 0x2000, dist 1000 -> measured with collision, ramp 0); the ladder warp and H reset through ctrl.onTeleport. Translated FUN_00253f50 (off in the state), the 0.5 look deadzone -> FUN_00324440(10) bit (sx unchanged), FUN_00254ac8's pad-release of the mouse latch. Detached ?cam= view takes no mouse. Traces and locomotion replays unchanged. GUESS: reset angles (view 1), FUN_00326068(7) gate. Test: worker 0090 scratchpad test_drop.mjs.
- Jump on the page (worker 0102, 2026-10-28): Space = ✕ runs jump_core.js inside PlayerController.step (ladder first; take-off before the stick, motion state after the intent, no move / turn while h_jp0 plays); the jump clock is the animator's head layer; `jump` pack loaded by default. test_jump_page.mjs: 7 jump traces through the real step, every frame exact on state / ground / clip / frame / blend, position <= 3.7e-3 (no re-sync), 18/18 mutants killed. review/jump-page.png (game vs page: wind-up, near apex, landing; apex matches, landing camera framing differs = the jump camera hooks, not ported). Open: armed h_jp*_a set (GUESS: plain set), c_wjp/c_bj clips missing from the exports, jump camera hooks, sounds. Notes: jump-research.md "On the page".
- Aiming on the page (worker 0098, 2026-10-28): worker 0077's weapon (aim.js / aim_core.js, FUN_00579570, bgd.bin stats, 21-piece reticle) merged onto the current page (3-way from the staging copy; player.js's setTable / playRequest / aimTurn / weaponHook, play.js `?aim=0`, shoot pack). Its GUESS GameCamera is gone: player.js `FollowCamera` now runs FUN_0032ace0's weapon (0xd) and first-person (0xe) paths through `follow.weapon` (gain decay, look table 0.66, flag 0x8000 off = no nudge/recede, base rates per type enter, ease-out 0.03/0.2, the switch lerp from LAST frame's eye/forward, FUN_0032aaf8 switch angles + ramp = 1 leaving first person (replaces the fastExit GUESS), FUN_00331160 shoulder drop, recoil in the view pitch, eye kick along forward). Several constants came from disassembling the enter functions in RAM (capstone; Ghidra has no decomp for them). Reticle drawn INTO the frame via output.js `hud` (native fb or full-HD target, before the PCRTC merge, so reports capture it). Tests (native/web): `AIM_TRACES=... node --import ./test_hooks.mjs test_aim.mjs` and `test_aim_camera.mjs` (all 8 aim traces + new w98_aim: eye <= 0.005, angles <= 7e-6); 17 mutations caught. Page vs game: review/aim-page.png (eye within 0.002 at both frames). OPEN: camera after a jump with the gun out (FUN_0032a0f8 flag 0x1000000 / CAM+0x220, reader not found; jump not wired on the page); reticle texture (decoded by worker 0100); aimed clips use the mixer pose (no local_pose export for the shoot pack: export_web_vincent.py). Scripts: worker 0098 scratchpad (aimgrab.py, scripts/w98_aim.json, pagecap.py, mutate.py; grab/ has the dumps + trace).
- Aim review follow-ups (worker 0099, 2026-10-28): the view 1 near-eye ease-out (player.js `place()`) is now checked: new PCSX2 traces cam_near_back / cam_near_turn (run toward the camera into the wall behind, then run away / turn the camera) and aim_i_fp_holster (first person -> circle) match per tick (ease-out <= 0.005; aim_i eye <= 0.005); with the rule off they fail by 0.06 / 0.15 / 11. New test `native/web/test_cam_traces.mjs` replays all camera traces from `CAM_TRACES` (default tools/trace/out/cam, gitignored; 0089's six copied there, cam_run re-recorded and cam_lturn taken from 0089's out2, since the old ones lacked want_dir_speed); scripts in tools/trace/scripts/cam_*.json. Found, not fixed: at the wall behind the spawn the page's camera collision measures 0.048 closer than the game; the walk-away x0.1 test ties when pinned (game f32 rounding decides). test_aim_camera now asserts the page's wiring order matches its hand-built frame. Ladder mount: aim_core `ladderMount` (P+0x29c = 4; no drawn test in FUN_00576548 or its callers, so state 0xe with the gun away is the game's). Audit rows updated.
- Reticle texture (worker 0100, 2026-10-28): DECODED. 'menu2' in data/etc/menudata.bin; its TIM regions start with a zeroed slot, so the texels are the LAST 0x2000 bytes of the image region (0077 read 0x80 early). They are the GS CT32 upload image (dump: 64x32 DBW 1 -> TBP 10792, CLUT 8x2 -> CBP 10788, read as PSMT4 TBW 2); export_web_aim.py `menu_texture` writes reticle.json `textures` (equal to the dump's GS memory at the draws, byte for byte). aim.js draws textured quads: UVs per vertex from FUN_003b1048, GS modulate (Cf Ct >> 7) and (Cs - Cd) As + Cd, TEX1 nearest (full HD: nearest texels at the target resolution). review/reticle.png (game | outlines | texture): green SSIM 0.34 -> 0.83 / 0.88, edges 0.07 -> 0.72 / 0.86. Tool: GS dumps carry the 4 MB GS memory at freeze +0x1a9 (worker 0100 scratchpad gsmem.py replays uploads into it). Open: add data/etc/menudata.bin (sha256 b851a405...4eb6) to tools/byo/disc.py INPUTS (not in this worker's allowed files); GS blend rounding not reproduced.

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
- Layer check (worker 0091, Dan's reports e1d5 / cb61): (A) hair over the cape is GEOMETRY, not draw order. At Dan's cam/pose (h_rn 0.13), vincent_13 (alpha hair, drawn after the cape, depth test on) is 0.9-3.0 units (median 1.17) in front of the cape on 199 rays (y 1034-1112). The game shows the same from behind (walk 185126). The collar's dark ellipse is the open collar top: 0/615 rays hit any cape surface even double-sided, and culling hides 0 of 1228 frontmost cape hits. No change. (B) Gun: BackSide (dump-decoded, like the body) and barrel0 dropped (only argmin ties had selected it). Rect z-fight 2545 → 107 px. Still open: the gun's 5 alpha-test draws (ATE+AFAIL like the hair) are drawn opaque. Review: /srv/shared/dirge-port/review/layer-fix.png.
- Hair from behind + body skin weights (worker 0092, 2026-10-28): the hair shape is NOT wrong. At walk dump 185126's own camera the game's hair geometry and ours cover the same pixels (1568 vs 1562, rows 219-312), no hair draw is missing, culling agrees, and the page (h_wk f4, cloth=0) shows the locks hanging down the back like the game. layer-fix.png compared two different cameras; the "block at the collar" is Dan's higher viewpoint, for which no game capture exists (next: a PCSX2 dump from the play camera behind him). Found and fixed on the way: body skinning used one bone per vertex; the game blends (FUN_005b1190 two loops + VU0 0x120 accumulate, weights = each batch's S-32 stream). render_skinned.build_group_pool and export_web_vincent now do it (top 4 joints). Hero h_st f91 p90 0.27 -> 0.077. Image: review/hair-back.png (game | page before | after). Scripts: worker 0092 scratchpad (zb.py, wind2.py, runw.py).
- Hair vs collar from behind+above, running (worker 0093, 2026-10-28): the game does the SAME as the page; no defect, no code change. Captured the missing evidence: PCSX2 from the sea-of-flames state, full-stick run (pinned on the railing from ~frame 55, h_rn keeps playing, cape keyed), ry +1 pulses pitch the camera down; 4 GS dumps at pitch -6.5/-12.7/-23.8/-41 deg (Dan's report: -16). Each dump's camera fitted from room vertices (median 0.03-0.04 px) and its tick fixed by the character crop (ticks 112/141/172/207, h_rn f12.0/9.0/7.2/7.8). The page replays the same pad script closed loop from spawn: position <= 0.0002, clip frame exact, pitch exact (eye 0.3-0.44 off, so renders use the fitted camera), cape 0.09 mean (mostly a uniform 0.08 shift). Head/collar crops, hair pixels inside the collar region game/page: 202/212, 374/384, 504/500, 879/878; hair IoU 0.82-0.88, SSIM 0.82-0.83 (control, page 3 ticks off: IoU 0.41-0.57, SSIM 0.37-0.56). Visible difference: the gun on his back (game alpha-tested and dotted, page solid; the open gun ATE item). Image: review/hair-cape-above.png. Scripts and dumps: worker 0093 scratchpad (gsgrab.py with full-cape fields, scripts/above_run2.json, camfit.py, page_above.py, metrics.py, run2/d1-4.bin).
- Gun draw modes (worker 0097, 2026-10-28): GS dumps hero/185126/185138: every gun draw is T8 TCC 1 MODULATE, ZTST GEQUAL, Z write on, right after Vincent's last hair draw; exactly the descriptor-flag-bit-1 triangles (5 drawn: 1 frame0, 4 barrel1) add ATE GEQUAL 100 / AFAIL RGB_ONLY / ABE (Cs-Cd)*As+Cd. gun.js now draws those as play.js's hair two passes (texture alpha kept in the atlas; meta.runs from export_web_gun.py, triangles in game draw order, 0 breaks vs the dumps). The remaining z-fight was a DECODER over-read, not game twins: FUN_005b2240 sends each descriptor as DMA ref qwc = u16 desc+6, but parse_drawpackets reads a group's last descriptor to EOF; on the gun 120/1058 tris leak from the next part (frame0.g1 d2: 16 topmt0 UV triples on frame0 verts = the same-place twins). export_cerberus.decode_gun now stops at qwc (none of the 120 is drawn in any dump; Vincent p000 unaffected). render_skinned.py still has the bug for other models. Gun textures are per triangle from the files in both export modes. Review: /srv/shared/dirge-port/review/gun-alpha.png.
- Hair vs collar while running (worker 0094, 2026-10-28): ALREADY FIXED by worker 0092's multi-bone skin weights; no code change. Dan's reports e896/e1d5 were taken before it (data re-exported 13:25). Evidence: PCSX2 run on an open path from the sea-of-flames state (scripts w94a/b/z in the worker 0094 scratchpad; 1.2 u/frame through a left and a right turn), 5 GS dumps (frames 24/29/34/78/83, pitch -19 deg). Page closed loop from the SAVE-STATE spawn (the page's own spawn is the hero-dump placement, 0.69 off and 42 deg turned: set ctrl.spawn from the trace): position <= 0.003, bones <= 0.005, every drawn game vertex within 0.04-0.08 of the page, 0 page tris without a game counterpart, also between ticks at 120 Hz. Pre-0092 data: collar prims 13/15/16 up to 1.2-1.7 units high (median 0.3-0.45), reproducing Dan's view. Open (audit rows): page spawn != save state; idle cape target 3.4 off at the save state; the 100-frame cloth window on a turn from standing is not armed. Image: review/hair-collar-running.png. Scripts: worker 0094 scratchpad (gsgrab.py, camfit.py, dumpv.py, whichframe.py, page_cap.py, cmp_frames.py, vcmp2.py, extra.py, dancam.py).

## Pack local poses (worker 0101, 2026-10-28; loader patch applied by the manager)
export_web_vincent.py --pack now also writes `vincent_<pack>_local.bin` (vincent_local.bin's
format, records t3 s3 q4) and a `local_pose` entry in vincent_<pack>.json (same fields as
vincent.json's). build_web.sh fails if a pack's file is missing. Base outputs (vincent.glb/json,
vincent_local.bin, cloth.json) and the pack glbs are byte-identical to before; the pack JSONs
only gain `local_pose`. data/vincent re-exported. The loader change below is applied to player.js / play.js:

```diff
--- player.js.orig	2026-10-28 18:13:58.796587423 +0000
+++ player.js	2026-10-28 18:13:58.806587561 +0000
@@ -883,9 +883,8 @@
     this.mixer.update(0);
     // The game's own pose build replaces the mixer's model-space mix for the locomotion
     // clips (skipped while an action's clip fades out: that crossfade is the mixer's GUESS).
-    // GUESS (port plumbing): clips without local-pose records (the pack clips, e.g. the aimed
-    // h_st_a / h_wk_a: export_web_vincent.py local_pose covers the main glb's clips only) keep
-    // the mixer's model-space mix (notes/fidelity-audit.md)
+    // Clips without local-pose records (an old export, or a pack exported before packs carried
+    // vincent_<pack>_local.bin) keep the mixer's model-space mix (notes/fidelity-audit.md)
     const hasLP = k => !!this.lp.clips[this.names[k]];
     if (this.lp && !this._fadeOut && hasLP(this.head.key) && (!this.out || hasLP(this.out.key)))
       this.poseLocal(this.names[this.head.key], fr(this.head),
@@ -911,10 +910,22 @@
                 S: new Float64Array(n * 3), P: new Float64Array(n * 3), q: new THREE.Quaternion(), v: new THREE.Vector3() };
   }
 
+  // a clip pack's local TRS (vincent_<pack>.json local_pose + vincent_<pack>_local.bin,
+  // export_web_vincent.py --pack; same record format): its clips join the table and read
+  // their own file. A pack for another bone count is refused.
+  addLocalPose(info, buffer) {
+    const L = this.lp;
+    if (!L) return false;
+    if (info.bones !== L.n) throw new Error(`local pose: pack has ${info.bones} bones, rig ${L.n}`);
+    const data = new Float32Array(buffer);
+    for (const [name, c] of Object.entries(info.clips)) if (!L.clips[name]) L.clips[name] = { ...c, data };
+    return true;
+  }
+
   // FUN_005af6c0 at a fractional clip frame: t/s lerp, q nlerp with hemisphere flip between
   // the bracketing keys (the export samples every integer frame, so the keys are frames).
   sampleLocal(name, frame, out) {
-    const L = this.lp, c = L.clips[name], d = L.data, n = L.n;
+    const L = this.lp, c = L.clips[name], d = c.data || L.data, n = L.n;
     let i = Math.floor(frame);
     i = Math.max(0, Math.min(i, c.frames - 2));
     const u = frame - i, o0 = (c.offset + i * n) * 10, o1 = o0 + n * 10;
--- play.js.orig	2026-10-28 18:13:58.798587450 +0000
+++ play.js	2026-10-28 18:14:14.029798659 +0000
@@ -461,6 +461,15 @@
       const g = await loader.loadAsync(`data/vincent/vincent_${n}.glb`);
       anim.addClips(g.animations);
       Object.assign(motions, side.clips || {});
+      // the pack clips' local TRS (player.js addLocalPose); without it they use the mixer pose
+      if (side.local_pose && anim.lp) {
+        const rl = await fetch('data/vincent/' + side.local_pose.file);
+        if (!rl.ok) console.warn(`${side.local_pose.file}: HTTP ${rl.status}: pack ${n} uses the mixer pose`);
+        else {
+          try { anim.addLocalPose(side.local_pose, await rl.arrayBuffer()); }
+          catch (e) { console.warn(`clip pack ${n}: ${e.message}: mixer pose`); }
+        }
+      } else if (!side.local_pose) console.warn(`clip pack ${n}: no local_pose (old export): mixer pose`);
       got.push(`${n} (${g.animations.length})`);
     } catch (e) {
       console.warn(`clip pack ${n} not loaded:`, e.message);
```

Check (new trace tools/trace/scripts/w101_aim_pose.json, TRACE_FIELDSET=pose: R1 raise h_st -> h_st_a,
then h_wk_a -> h_rn_a; bone matrices *(MP+0x3f8), 3x4 part, element max abs diff, layer state
from the previous record): per-frame mean-bone error median 0.155 -> 0.067, mean 1.142 -> 0.110,
max 16.5 -> 1.75 (worst bones before: 56-61 right arm/gun ~1.6 mean, 16 max; after: 79 0.34,
56-61 ~0.28). c_run_pose_blend unchanged (median 0.0019). Mutation (sampleLocal reading the base
file for pack clips) -> median 1.54, caught. Remaining aimed error, steady h_rn_a 0.05-0.07 mean:
the aim-pitch partner layers (h_rn_ad etc., layer +0x8 partner, weight ~0.027 in this trace) that
the page doesn't blend. Not decoded here. Harness: worker 0101 scratchpad h/cmp.mjs (real three
0.169 + GLTFLoader; mixer = page's own update()). The play.js half (fetch in loadPacks) was not
exercised in a browser.
- Jump landing flicker fixed (worker 0103, 2026-10-29): standing landings flashed the air pose at the hand-back. Two causes: (1) the page advanced the blend-out layer with a loop wrap always; FUN_005aa2a8 wraps only layers with the loop flag (+0x1e bit 0, traced lflags 0x87) and holds every other at n - 1 (j_stand f78-88: h_jp4 stays 14.0 under the h_st blend). Layers now carry `loop` (SpeedAnimator.advLayer); a queued jump clip taking over leaves the old one at n - 1 (j_double f65 c_wjp2 8.0). (2) the page's frame count for one-shot pack clips came from the glb duration, one short (h_jp4 14 vs 15, so it wrapped at once); addClips now takes the pack's `clips[name].frames`. Also: play.js runs no game frame until the clip packs load (a ✕ during the download played the whole jump on a frozen idle pose: Dan's "body didn't animate" once). test_jump_page.mjs checks the blend-out layer's clip/frame on every blend frame and the pack frame counts (HEAD player.js: 10 FAIL). review/jump-landing.png: page before/after through the hand-back. Running jumps were fine because they hand back before h_jp4 ends.
- Armed jump + c_ clips (worker 0104, 2026-10-29): with the gun out the page plays the h_jp*_a sets (player.js `armed` = weapon.drawn()). aim_g's "skipped h_jp2_a" was the R1 at f48: a shot in the air (FUN_00577480 -> FUN_00560b58) switches to the air-shot class 0x4e (FUN_00596830: h_jp3_a, blend 1, slot 0xc1 empty) then the fall class 0x22 (FUN_00599820). jump_core: armed phases fixed (play now, FUN_00345818 blend rule via FUN_00346150 = head still blending, 0x80 same-clip drop), AirFireMotion, FallMotion, Jump.airShot; player.js runs aim.js's new hook `weapon` (FUN_00579570) before the motion update (FUN_005786e8 order), clips stay in `post`. Jump pack now has h_jp*_a (0770320b), c_wjp*/c_bj*/c_bwj* (07703200) via export_web_vincent.py PACK_ONLY_SETS + build_web.sh PACK_JUMP; base / ladder / shoot / cloth exports byte-identical. test_jump_page: aim_g armed exact on clip / frame / blend / blend-out; 17/17 mutants. review/jump-clips.png. Open: jump camera hooks. (disc.py has the 07703200 / 0770320b hashes, added by the manager; h_jp3's container settled by worker 0105, below.) Notes: jump-research.md "Armed jump, air shot".
- Clip sources (worker 0105, 2026-10-29): z128 loads Vincent's clips from 07703200 / 0770320b / 0770320c / 0770320d, not 077000xx. FUN_005a7390 (every clip request: FUN_003449a0 / FUN_005a91c8) looks the name up in MP+0x3bc[0..7]'s resources (+0x14 handle), then MP+0x3c8, first hit wins; the save state's handles 0x17-0x1a are those four files (each image matches its file), the rest -1. export_web_vincent.py CLIP_SETS is now that family (PACK_ONLY_SETS gone); disc.py has 0770320c/0d/0f, 07703211. Re-exported data/vincent (dump build): changed h_wk, h_jp3, h_div (33 -> 31 frames, CLIP_FRAMES unit), c_ld_lid/rid/uel/uer, 24 shoot clips (h_st_a, h_rn_a, h_*_au/ad, h_brn/lrn/rrn_*, h_fwk_rd, h_sqm_rd, ...); h_st / h_rn / the other jump, ladder and shoot clips are byte-identical. cloth.json: the aimed and jump clips now have the 077032xx cloth windows (e.g. h_wk_a 2x30, h_jp4_a 2x50). Pose traces (new scripts w105_walk_pose / w105_jump_pose / w105_runjump_pose, TRACE_FIELDSET=pose; harness worker 0105 scratchpad posecmp.mjs, layers from the previous record): walk mean 0.0141 -> 0.0094, h_jp3 0.023-0.035 -> 0.002 per frame, w101_aim_pose median 0.067 -> 0.047 (h_st_a 0.085 -> 0.076), c_run_pose_blend unchanged. Locomotion replays unchanged (they don't read clip data). Also: armed back jump phases 6/7 use FUN_00345818 flags 0x83 (jump_core aimedReq; test "armed back jump", old code fails it), AirFireMotion no longer throws when slot 0xc1 is loaded (warns once). Image: review/clip-sources.png. Open: export_p000_gpu2.py and p000_cloth.py still name 0770000b (not my files; h_st is identical, the cloth sim's run_clip default isn't used by the page); the mesh 07703201 differs from 07700001 (skeleton equal), which one z128 draws is unchecked.
- Mesh source (worker 0106, 2026-10-29): z128 draws Vincent's model from **07703201** (mesh) + **07703202** (textures), not 07700001/02. Evidence: the save state's RAM holds 07703201's image at 0x18dfe80 (37718/39492 words equal, header rfd size 0x26910; 07700001 matches 1579); 07703202 at 0x1906800 (34936/35040). Hero GS dump draws the envmaps at TBP 0x2ee8/0x2eec CBP 0x2e64 = 07703202's layout (07700002 has an extra 64x64 'p000_ta' eye texture + 'eye' texanm header, which push them to 0x2f0c/0x2f10). The two meshes are the same bytes 0xa0 apart (shorter texanm header) except build junk: decoded body (2037 v, 2389 tris), skeleton, cape (skin, cloth groups, draw tris), materials used (0-5) and those textures are identical. Switched: render_p000_bound CONT/TEXC, p000_cape/p000_cloth CAPE_BLOCK 0x19f70 -> 0x19ed0 (export_p000_gpu2 now uses C.CAPE_BLOCK; it had a literal 0x19f70), p000_cloth CONT/CLIP_CONT, export_p000_gpu2 CLIP_CONT -> 0770320b; disc.py has 07703201/02 hashes (verify 180/180). Gun 07770580/81/82 are the files in RAM (no alternative). Re-export (dump build): every file the page loads in data/vincent is byte-identical (glbs, local bins, cloth.json, light.json, cerberus_web.*); only _ref labels (p000_L01@660), the _ref clip container name and motions.json (was stale, now names 077032xx) changed. So page pixels can't differ; no mesh-source.png made. Root data/vincent.bin / t_vincent.rgba (static viewer, build_web 'p000_static' + export_web_room) not rebuilt: out/byo isn't on disk. Old scripts export_p000_gpu.py, render_p000_dumpbound.py still name 07700001 (not used by the page).
- Jump camera (worker 0107, 2026-10-29): plain jumps needed nothing new. Fed the game's position and state, the translated FollowCamera already follows standing / running / second jumps (eye <= 9e-3, open loop) and the back jump (per tick, angles <= 7e-8; eye 0.048 = the known railing collision residual). The |dy| < 15 rates and the snap were already right. jump-page.png's landing row compared different moments (the game grab was still in the air). Gun-out jump, decoded and translated: the CAM+0x220 reader is FUN_0032a180 / FUN_0032a1d8, called at FUN_0032ace0 0x32eb98 / 0x32ebe4 (FUN_00326500() true). The eye offset blends toward DAT_0046e320 = (2.8, 4, -36) by (1 - cos(pi a)) / 2 before the y scale. CAM+0x220 then steps: in view 2 with flag 0x1000000 it rises +0.06 x step; without the flag it falls 0.03 (CAM+0x1c0 < 25 and flag 0x80 = camera collision hit last frame, l.2029/2488), falls 0.0075 when |left| + |look| stick > 0.001, else holds; in other views it falls 0.1. The flag is set at take-off (FUN_00575768, P+0x2a8 bit 0) and cleared by the enter slot (+0xc) of player states 2 / 3 (FUN_00537e70 / FUN_005380a0, state table FUN_00537628), so after landing the offset stays until a stick moves. Code: aim_core camJump / enterFoot / jumpOffset / camJumpFrame; player.js place() + `hit`; jump_core takeOff / landing. Traces: 7 new `cam_j_*` (scripts in tools/trace/scripts; out/cam). fields.py CAM set adds p_state and p_2a8. test_cam_traces has a closed-loop 'page' mode (controller + jump + weapon + camera as play.js / aim.js; 17 cases ok). test_aim_camera now compares aim_g_air after the jump too. Mutants 15/17 killed; the survivors are the draw's clear and the in-air holster clear (neither changes anything visible). Image: review/jump-camera.png. Open: player.js's GUESS 9 <-> 2 switches don't call the state-2 enter; ctrl.P.flags2a8 bit 0 is never set when drawn (jump_core's armed second-jump height - 0.1 reads it); the page's landing state is 2 where the game's armed landing is 3.
- Mid-air draw (worker 0108, 2026-10-29): FUN_005766c0 passes its a0 / a1 to FUN_00576548, so an R1 / R3 draw in the air runs FUN_0032a0f8(CAM, 1, 1) (flag on, CAM+0x220 = 1); the player goes to state 9 (enter FUN_00538b50 leaves the camera alone), the body restarts its fall with height DAT_0046ee88 = 1.0 (0 after a second jump within 0.1 of the apex: C+0x484 / 0x498 = body.t / rise), and P+0x2a8 bit 3 is set. R1 in the air is allowed while bit 3 is clear (FUN_003320c0 l.288). The page's P+0x2a8 bit 0 now follows the draw / holster. Type 0xc's enter FUN_00330240 zeroes the stick-idle counter CAM+0x1e2 (sb zero at 0x330420; aim_core enterType -> FollowCamera). FollowCamera.reset no longer steps CAM+0x220; the ladder mount passes the ground bit. Code: jump_core weaponSwitch, aim_core draw / holster / input(.., player). Traces cam_j_draw_r1 / _r3 / _gate (test_cam_traces page; it now also checks P+0x2a8 bits 0/3). Mutants 12/12 killed (aim.js's ladder call has no page test). Not ported: FUN_00565d78 (P+0x100 bit 44 / C+0x590 bit 24).
- Take-off bits (worker 0109, 2026-10-29): P+0x100 bits 46-47 = P.jumpBits. Set by the take-off (FUN_00575768 -> FUN_00565df8: 1, or 2 for kind 2), cleared by any state change into a non-air state (FUN_0055b8e0's tail; jump_core enterState, now used for every player-state write). weaponSwitch (FUN_005766c0) restarts the fall only with bits != 0 and the ground bit clear, and picks state 9 when bits != 0 or the ground bit is clear; so a walk-off draw keeps falling, and a draw/holster in the h_jp0 wind-up goes to state 9 with no restart. New: FUN_0055b8e0(P, 9, 1, 0) forces motion 0x22 (state 0's vt+0x24 -> jumptable 0x589e70[9]); jump_core Jump.forceFall. From the wind-up this ends the freeze (rise + h_jp3_a two frames early, traced). aim_core: enterFoot follows the returned state, not the ground bit. FUN_00330240 decompiled (no function in the Ghidra DB: 0x330240-0x33044b, created read-only) -> analysis/decomp; 0x330420 = sb zero, 0x1e2(s0), s0 = CAM: 0108's claim holds. Traces: cam_j_draw_windup, cam_j_gun_back, cam_j_gun_double, cam_j_gun_windup_holster, cam_j_gun_windup_r3 (out/cam), aim_j_draw_windup (out/aim). Tests: test_cam_traces page checks the bits per frame; test_jump_page has the walk-off draw (hand-built: no reachable ledge/ladder) and the wind-up clip check; test_aim has the aim.js ladder-mount wiring. Mutants 11/12 killed; survivor = draw's enterFoot by ground bit (equivalent: the flag can't be set when a draw is possible). Open: the page's GUESS 9 -> 2 landing is one frame early (bits clear a frame early after a mid-air draw).
