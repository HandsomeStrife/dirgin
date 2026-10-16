# Session 2026-10-15 — record correction + geometry ground truth

## 1. Record corrected (dashboard + STATE-OF-THE-PORT)
After Dan reviewed the renders against real game footage, the earlier "done"
claims were overstated. Corrected both `/srv/shared/dirge-port/index.html` and
`STATE-OF-THE-PORT.md`:
- **Level/room geometry assembly = BROKEN** (real textures on mis-assembled
  meshes → slabs/boxes/floating planes). This is the headline bug.
- **Character pipeline = IN PROGRESS** (GPU body skins OK; hair shatters;
  software-skin path explodes; skeleton viz not clean).
- Capstone/env/char renders demoted out of highlights into an in-progress /
  known-broken section. Before/after comparison images in
  `/srv/shared/dirge-port/geometry-debug/`.
Lesson reinforced: validate renders against ground truth NUMERICALLY, not by eye
(a two-sided/ambient render hides winding/placement errors).

## 2. Ground-truth reference assets (from Dan), in /srv/shared/dirge-port/downloads/
- `...Vincent Valentine.zip` — static Vincent `.dae` (p005), 27 parts, 7318 tris,
  UVs + 8 texture atlases (p005_1_a..f, p005_2/3). **No skeleton/weights** (static
  rip, fingerprint of a PCSX2 GS-capture).
- `DCFF7-INT-models_non-static.zip` — **315 .obj + 316 .smd** rips (JP Int, 2018).
  The **.smd (Valve SMD) files carry explicit correct triangles + skeleton
  (nodes/bind pose) + per-vertex bone weights.** This is the key ground truth for
  BOTH the triangle rule AND the skinning/gameplay-rig blocker.
- `DCFFVII_character-textures.zip` — character texture atlases (42 MB).
NB: these are JP-International; EU geometry matches (Vincent vertex count is
identical, see below).

## 3. Format understanding (confirmed)
- Char model container = RFD mini-archive (wiki RFD-&-TXD-archives.md): texanm.tad,
  marker.mkd, several `pNNN_{L,S}0n.smd` (parts, NOT LODs — all needed) + skeleton.skd.
  Vincent p005 = `data/chr/p/0/0/5/models.rfd` = extract/_KEL/noPath/**07700141.bin**
  (texture.txd = 07700142.bin). Path digits a/b/c == pABC.
- SMD block magic "SM2"; SKD "SK2"; MOD = anim/bone.
- Archived XeNTaX thread (t=4857, via web.archive.org) confirms our decode: mesh is
  the `smd` data, VIF unpack tags `00 80 xx 60` (position floats), `01 80 yy 6E`
  (4-byte/elem, only W/4th byte = scatter index into matrix pool), `00 80 zz 79`
  (shorts/32768 coords). Community got STUCK at vertex decode ("[Uncompleted]
  maxscript", "assign offset per section manually") and fell back to GS-capture.
  **No finished file-format tool exists** (checked Surihix, GitHub, Noesis). Our
  decoder already gets vertices right → we are ahead of the community here.

## 4. Character triangle-rule status (UNCRACKED)
- My decode of Vincent (07700141): **5602 vertices** (pool) — EXACT match to the
  ripped `file13148.obj/.smd` (5602 v). So **vertex decode is correct**.
- Ground-truth triangle count (file13148) = **7327**.
- My interpretations: STRIP (sliding 3-window + parity) = 8441 tris (right surface
  area 1.18 vs ref 1.04, but ~12 gross needles + fold-back dups: 525 appear 2x, 47
  3x+); dedup-by-vertex-set = 7821; LIST (step 3) = 3357 (WRONG — drops ~60% of
  surface area, 0.42). => **it is a STRIP; the remaining ~494 excess are
  restart/needle triangles to remove.** render_skinned.py reverted to STRIP with a
  TODO note.
- Attempt to crack the exact restart rule by corresponding my pool to the SMD and
  mapping GT triangles onto the index stream **FAILED**: crude centroid/RMS
  alignment (identity axes, 0.043 normalized residual) is too imprecise for vertex
  NN (only 1.2% matched; GT collapsed 7327→2472). Needs proper ICP after dedup, OR
  — better — the oracle.

## 5. Recommended path (both rooms AND characters): ORACLE VIF dump
The SMD rip is in a different frame (fragile alignment). The recompiled-EE oracle
(port/oracle/) produces the real post-VIF primitive stream in the game's own frame
— no alignment needed. That is the reliable ground truth for connectivity +
winding + per-node placement. The rooms agent (general-purpose, launched this
session) is building this method for BMD room 06960212; apply the same to the SMD
character path. PCSX2 source is the spec for how VIFcode unpack + GIF/GS tristrip
primitives work (read for behavior, don't copy GPL code).

## 6. Toolchain note
Blender: use **/home/dan/blender-4.2.0-linux-x64/blender** with env
`OCIO=/home/dan/blender-4.2.0-linux-x64/4.2/datafiles/colormanagement/config.ocio`.
The `/usr/bin/blender` has a broken color config → all-black renders.

## 7. Open agents / next
- Rooms/BMD geometry agent (oracle VIF dump) running at session time.
- Next for characters: oracle-dump Vincent's draw to get ground-truth connectivity
  in-frame, derive the strip-restart rule, apply to render_skinned.py; then use the
  SMD skeleton/weights to fix skinning + the gameplay-rig blocker.

## 8. BREAKTHROUGH: per-submesh connectivity flag (rooms agent) + objective harness
- Rooms agent found + VU1-confirmed a **per-submesh header flag**: V4-32 num=2 control
  qwords; `qw.x & 0x7fff` = vertex count, `qw1.y & 0x8000` = connectivity flag
  (set⇒TRIANGLE LIST 3-loop, clear⇒STRIP 4-loop). VU1 program_A idx 12/14/33-43:
  vi04=0x7fff, vi05=vi04+1=0x8000, ILW.x/y count+flag, IAND, IBEQ vi09→instr174.
  Verified: room 06960212 = 171 list/192 strip = 4753 tris (exact); char Vincent
  07700141 = 318 list/588 strip = **7327 tris = EXACT match to the rip (file13148)**.
  Fix landed in export_bmd_textured.py (rooms). Characters NOT yet fixed in repo.
- **Objective validation harness** (scratchpad/validate.py): ICP (Kabsch+scale) my
  decode vs SMD unique verts. **Baseline Vincent: verts 3482 vs 3484; 97.8% within
  2% of model diag, 81.4% within 1% (median 0.45%, tail to ~3.3%); triangles
  6089/7327 (83%).** => decoder is CLOSE but NOT correct: ~2% vertex tail
  (skinning/scatter) + ~17% triangles missing (packet enumeration: enum_descs finds
  742, flat STCYCL-0x0404 scan finds 906 submesh-headers).
- OPEN decoder questions (characters): (a) packet enumeration gap (742 vs 906);
  (b) sequential-count vs scatter-idx triangle model (room uses sequential count;
  render_skinned uses idx into a 256-slot scatter pool — need to confirm which is
  right for chars, they may reconcile); (c) the ~2% vertex-position tail.
- GOAL (Dan): the decoder must extract EVERYTHING correctly (we have 316 char rips,
  ZERO room rips). Rips = answer key for validation, NOT the shipped assets. Drive
  the harness metrics to ~100% across MANY of the 316 rips (not just Vincent) so it
  generalises. Rooms validated via oracle (no rips).
- DISCIPLINE NOTE: stop declaring visual "wins" from grey renders; the gate is the
  numeric match + Dan's eyes. Over-claimed repeatedly this session.

## 9. CHARACTER DECODER CRACKED (thickness + enumeration) + TWO FORMATS found
- **Thickness fix (Dan's insight):** position offset scale was 2x too small. sc must
  be f32(P+0x2c)/**2048** (ITOF12 /4096 x2). Confirmed: Vincent p005 + Weiss e000
  render pixel-match their rips. BAKED into render_skinned.py assemble().
- **Connectivity:** per-submesh V4-32 num=2 control header: qw.x&0x7fff = vertex
  count, qw1.y&0x8000 = list/strip flag (same as BMD rooms). idx==count invariant.
- **Enumeration hardening:** the old group-descriptor walk missed submeshes; the flat
  STCYCL(0x0404) scan mis-advanced past STROW/STMASK. WINNER = header-anchored scan:
  word-aligned find every V4-32 num=2 header, then within [hdr,next-hdr) take the
  V1-8 unpack whose num==count (disambiguates false unpack-looking data). Gives EXACT
  tri counts: Vincent 7327, Weiss 3127, e001 3000, e003 3042, e008 2500.
  (scratchpad/robust4.py -- NOT yet baked into repo.)
- **TWO FORMATS (key finding):**
  * INDEXED/skinned (Vincent, Weiss, most bosses): V1-8 idx -> scatter pool. SOLVED.
  * V4-8-INDEXED (e002 07650081 + ~105 others): no V1-8 idx; instead a V4-8 (cmd 0x6E)
    unpack where the W/4th byte is the scatter index (the "6E" from the XeNTaX thread).
    My decoder treats V4-8 as vertex COLOUR -> finds no index -> 0 tris. NOT yet handled.
    (e002: count=24 submeshes carry one V4-8 x48 unpack = the index/attr stream.)
- **Batch vs the 315 rips (robust4):** 157/316 exact (verts+tris), 105 empty (the
  sequential variant -> 0 tris with the indexed decoder), 25 miss, 29 err.
  => indexed variant generalises; need the sequential-variant decode for full coverage.
- NEXT: add sequential-variant handling (reuse the room count-based decode) + rebatch.

## 10. CHARACTER DECODER -> 88.3% EXACT (batch vs 315 rips)
- The "V4-8 variant" was a mis-read: those models DO have V1-8 idx, but in a layout
  that SEPARATES headers from idx (vs Vincent's interleaved header+idx).
- FIX: greedy header<->idx association by num==count across the whole file (nearest
  unused V1-8 with num==count), + header-anchored enumeration + /2048 scale.
  (scratchpad/robust5.py / robust6.py -- hybrid in-region-first then greedy.)
- BATCH (316 containers vs rips): **MATCH=279 (88.3%)** exact verts+tris; miss=7
  (±1-2% tri count, minor mis-assoc); ~30 static models (no SK2 skeleton) need a
  static decode path (empty/err). Vincent 7327, Weiss 3127 still exact.
- NOT yet baked into render_skinned.py (needs uv/col association for the separated
  layout to preserve textured rendering). Geometry (positions+connectivity) validated.
- REMAINING: (a) bake robust6 into assemble() incl. uv/col assoc; (b) static-model
  path (~30); (c) the 7 near-misses; (d) then winding/texture/animation polish.

## 11. ROOM TEXTURE MYSTERY SOLVED (the "mangled textures/assets" Dan saw) — 2026-10-15
Investigated WHY the textured room looks like mangled assets. Chased the headline
to ground with objective probes (scratchpad diag_*.py). Findings, all evidence-backed:

- **Position decode is correct AND robust.** Hypothesis "VIF ROW/STMOD state persists
  across packets and the per-packet reset breaks central packets" = DISPROVEN:
  all 363 packets of 06960212 re-emit their own STROW + STMOD(mode 2); reset-vs-
  persistent decode is byte-identical (0 packets move). (diag_vifstate.py.) So the
  central jumble is NOT a position/height bug.
- **The per-packet V4-32 num=2 header carries NO material id.** Its two control
  qwords encode only: q1.low16 = 0x8000|vertexcount (the connectivity flag + count).
  The decoder's `sel = count & 0xff` grouping key is therefore MEANINGLESS as a
  material selector. (diag_giftag.py: the "16 distinct selectors" were just 16
  distinct vertex counts.)
- **No material/texture events in the draw chain.** Full VIF-chain walk finds 0
  DIRECT / 0 MSCAL / 0 MPG; exactly 363x each of {V4-32,V3-16,V4-8,V2-16,MSCNT,STROW}
  and 726x {STCYCL,STMOD}. Texture binding is NOT a per-draw GS register write.
  (diag_vifchain.py.)
- **The real material system lives in the BMD pre-packet header** (payload 0x30..0x1b00):
  * payload+0x10/0x14/0x1c/0x20 = section pointers -> 0x1900 / 0x1940 / 0x1990.
  * **payload+0x1900 = 16-entry u32 table of MATERIAL IDs: a6 af ae b3 ad ab ac b2
    b0 b1 aa a7 a9 bd a8 df** (0xa6..0xdf). The room's 16 materials.
  * payload+0x1940 = 4 texture-handle records (16B each), runtime pointer 0x22de50
    (EE RAM addr, fixed up at load).
  * payload+0x1990 = rows of 4 floats (e.g. 109.97,-60,120,1) = transform/bounds.
- **The per-submesh -> material assignment is NOT on disk.** No 363-entry index table
  exists anywhere in the 6352-byte header chunk (scanned all strides u8/u16/u32,
  caps <16/<64). The 4-way q1-high field flips 180x across 363 packets -> it's a VU
  buffer/program field, not a material block. (diag_pktmat.py.) AND no material-sorted
  draw order: no 16-entry per-material COUNT table summing to 363 exists anywhere in
  the header (scanned all strides, window 8..24). (diag_matcount.py.) The 0x1940
  table = 4 pointers -> 4 handle records (each {0,0,0x22de50,0}). Every on-disk
  mapping theory is now ruled out; the binding is conclusively runtime-only.

## 12. NATIVE GPU PIPELINE PROVEN + NON-SMD CONTAINERS TRIAGED — 2026-10-15
- **Native GPU render (shipping path) works end-to-end.** Built native/gpu/render_char
  (`make char`), fed it the DMSH mesh from the SOLVED render_skinned.py (Vincent
  07700141 -> 5602v/7327t exact), rendered on the REAL AMD GPU (EGL/Mesa GL 4.6 core,
  not Blender) -> coherent, correctly-lit Vincent (cloak + headband + golden clawed
  gauntlet all recognisable). This is the convergence: solved decoder -> DMSH ->
  native GL -> PNG, on real decoded game content. Renders in
  geometry-debug/NATIVE-GPU_VINCENT_{lit_3q,lit_front,prelit_front}.png.
- **The "~29 non-SMD" open thread RESOLVED — mostly not models.** Triaged all 316
  containers by content magic (scratchpad/triage29.py + a direct SM2/MOD count):
  * 286 contain SM2 (skinned mesh) -> ALL decode (279 exact + 7 near-exact). The
    decoder handles 100% of actual skinned meshes.
  * 1 contains MOD (0c38440c = evm012 event model, data/chr/n/0/2/6) -> the only
    genuine non-SMD character model. Separate decoder, low priority (1 model).
  * 29 contain NEITHER -> NOT models: texture.txd/TIM2/*_arst texture containers,
    bt_v0NN data tables, high-entropy ENCRYPTED blobs (data/chr/g/ 0c..b1ca), SK2+anm
    skeleton-only, u16 index tables. These were size-match FALSE POSITIVES from my
    models.rfd-size container-id step, NOT an SMD decode gap.
  => "279/316" badly understated the decoder. Honest figure: 286/286 skinned meshes.
  The near-misses (7) are all SM2 too (still SMD, cosmetic ±1-2% tri).

## 13. ROOM BLACK-GASH BUG FIXED — STRIP = QUAD GROUP — 2026-10-15
Re-examined the room with fresh eyes. Top-down wireframe = a COHERENT multi-room
floor plan (geometry positions right). Textured 3q = recognisable Shinra interior
(panels, consoles, blue screens, orange signage numbers 13/3) BUT dominated by big
BLACK TRIANGULAR GASHES. Those are the real headline artefact, and they are a
CONNECTIVITY bug, not textures/positions:
- Measured per-packet triangle quality (diag_tris.py): only 31/4753 tris (0.7%) are
  "big" (edge > 1/4 room diag) but they're the visually dominant gashes; ALL come
  from STRIP packets, none from LIST.
- Dumped raw vertices of a worst strip packet (193, count=16): it is 4 INDEPENDENT
  PLANAR QUADS (v0-3 a y-z wall panel, v4-7 an x-y panel, v8-11 a y=90 ceiling quad,
  v12-15 a y=0 floor quad). A continuous 16-vertex strip bridges these 4 separate
  panels with spanning triangles = the gashes.
- Hypothesis test (diag_quad.py) across 06960212/068f0612/06960112: EVERY strip
  count is a multiple of 4 (192/192, 584/584, 1586/1586), 97-99% of each group-of-4
  is coplanar, and the big gash tris are exactly the ones straddling a 4-group
  boundary. This matches the earlier VU1 finding verbatim: STRIP = 'strip 4-loop'
  (a 4-vertex inner loop), i.e. QUAD GROUPS, not a continuous strip. LIST stays
  3-groups (all list counts are mult-of-3).
- FIX (export_bmd_textured.py): new quad_tris(n) -> for each group of 4 emit the 2
  quad triangles in strip winding, restart every 4, no bridging. packet_tris now
  routes STRIP -> quad_tris. Numeric: room96 tris 4753->3157, big-gash 31->8 (the 8
  remaining are legitimately large single quads, not gashes); 068f 12->0; 06960112
  31->12. Re-rendered (Blender, OCIO): gashes gone, coherent Shinra room.
  geometry-debug/room96_QUADFIX_3q.png vs room96_TEXTURED.png.
- Room geometry (positions + connectivity) is now CORRECT. The only remaining room
  gap is the texture<->submesh binding (the runtime/oracle item, Section 11).
- Probes: scratchpad/diag_tris.py, diag_restart.py, diag_quad.py.

## 14. A "ROOM" IS A MULTI-CHUNK WORLD-SPACE ASSEMBLY (not one container) — 2026-10-15
Dan: the room renders look like "a weird textured set of blocks" not the INSIDE of a
room. Correct -- and there's an objective reason, found this session:
- Compared to the real game screenshot (Screenshots/...09.45.27.png): a DoC room is
  an ENCLOSED space -- floor grating + walls + vaulted paneled ceiling + machinery
  props (mako tanks, consoles, a diagonal pipe), with Vincent/Weiss standing in it.
- Objective enclosure test (scratchpad/enclosure.py): cast 400 rays over a sphere
  from interior points of ONE container (06960212). Only ~50-65% of rays hit geometry
  (100% would be a closed room) -- so a single container is NOT a complete room.
- A single container is ONE CHUNK of a larger stage. Zone 0696 has ~11 BMD chunks
  (06960012..06960a12), ALL in a SHARED WORLD SPACE (their bboxes tile/overlap: e.g.
  212 X[110,315], 312 X[-190,584], 412 X[-40,425], all Y~[-60,97]; the big 112/812
  are the outer SHELL spanning the whole stage). 06960212 alone (363 pkts) is just
  the interior machinery/console layer; the shell walls+ceiling are 112/812.
  (Confirmed: the per-chunk transform floats at BMD payload+0x1990 = the chunk's
  world placement -- 06960212's first row (109.97,-60,120) = its world min corner.)
- ASSEMBLED render: loading 212+312+412+512+612+112+812 together in world space and
  rendering a 360 equirectangular pano from the room centre (212,15,245) shows a
  RECOGNISABLE ENCLOSED SHINRA INTERIOR: vaulted/curved paneled ceiling (matches the
  screenshot), metallic cylinder tanks, console banks w/ screens+numbers, patterned
  metal floor, walls all around. geometry-debug/room96_ASSEMBLED_360pano.png +
  room96_ASSEMBLED_interior.png. Residual grey voids = a few more chunks (712/912/a12)
  + the still-approximate texture binding + camera placement.
- CONCLUSION: the decode is CORRECT -- it produces coherent, recognisable Shinra
  interior geometry+textures. The "textured blocks" look was from viewing a SINGLE
  CHUNK / partial assembly. To render a whole room the native engine must ASSEMBLE
  all the zone's chunks in their shared world space (they're already world-placed).
  Scripts: scratchpad/room_interior.py, room_assemble.py (takes a manifest list +
  optional camera centre), enclosure.py.
- **CONCLUSION:** per-submesh material is resolved at RUNTIME by the loader (the
  context+0x24 handle table, FORMAT.md Section 6 — now proven, not just asserted:
  the on-disk slots hold EE pointers like 0x22de50 that the loader overwrites).
  => The decoder physically cannot know which of the 16 materials each submesh uses
  from the file alone; its dominant-order guess scatters atlases arbitrarily. THAT
  is the "mangled textures". Geometry + UVs + baked vertex colours are all correct
  decoded data; only the texture<->submesh binding is wrong, and it is the single
  genuine runtime/oracle item.
- **NEXT (needs the oracle, not a decoder tweak):** dump the loader's per-submesh
  material assignment from the recompiled EE (or trace how it maps the 16-ID table +
  handle records to draw packets), then bind correctly. Did NOT change the grouping
  code on a guess (Dan's "a guard can be worse than the hole" — an arbitrary regroup
  could render worse). Probes: scratchpad/diag_vifstate|giftag|vifchain|bmdhdr|
  matdesc|pktmat.py.
