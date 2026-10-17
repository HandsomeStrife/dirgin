# Decoder status — living truth table

## GROUND TRUTH UNBLOCKED (2026-10-15) — PCSX2 headless capture WORKS
The rooms had NO ground truth all session (no room rips) — that's why the room decode
stayed unvalidated and I kept over-claiming from renders. FIXED: with the PS2 BIOS +
EU ISO Dan provided, PCSX2 now runs the real game HEADLESS on this server (Xvfb +
llvmpipe GL + systemd-user-service for persistence), navigable via xdotool, and
**GS dumps capture** (the exact primitive stream the game draws = real triangles).
Captured 4 dumps incl. a Deepground POD-ROOM interior + a Vincent scene:
/srv/shared/dirge-port/gs-dumps/doc_gsdump_01..04.gs.zst (+ paired PNG screenshots).
Full how-to + the next step (parse .gs.zst GIF packets -> triangles -> validate the
BMD decoder numerically) in notes/pcsx2-headless-setup.md. This is the answer-key
mechanism rooms always lacked. NOTE: GS XYZ is screen-space (post-VU Mvp); to compare
to world-space BMD decode, either reconstruct the camera or validate the rendered
image. The PNG screenshots are already real ground-truth images of DoC rooms.

### GS DUMP TOOLKIT BUILT + VALIDATED (2026-10-15) — tools/gsdump/
Parsed the dumps end to end. The toolkit (parser/rasterizer/texture-decode/
texture-binding) is in `tools/gsdump/` with a README; outputs for Dan in
`/srv/shared/dirge-port/gs-dumps/analysis/`.
- Parser is byte-exact: walks the full ~12MB packet stream and lands EXACTLY on EOF
  (any mis-parse desyncs long before). Header crc/dims/serial all match.
- Rasterizer reproduces the embedded screenshot's UI layout (dump 04 Analysis screen).
- Texture decode WORKS incl. the PSMT8 "upload-as-CT32 fast path" (deswizzled via the
  canonical GS interleave = `dtex_unswizzle8`, CSM1 CLUT): decodes to RECOGNISABLE
  room/char textures (Deepground soldier atlases, window fire, Vincent cape, blue-lit
  panels). => also independently VALIDATES the project's `dtex_unswizzle8` vs ground truth.
- `gsbind.py` produces the texture<->geometry BINDING table (per sampled TEX0, the
  triangle count) = the runtime binding oracle the BMD rooms lack (FORMAT.md §6).
HONEST FINDING (no over-claim): **the 4 captured dumps are the PROLOGUE area, NOT the
decoded zone-0696 BMD rooms**. Rigorous check: of 32 correctly-decoded dump-03 textures
vs 1635 on-disk zone textures, exactly ONE is pixel-identical (diff 0.0) and it is a
shared global EFFECT texture (0696ff4f_z150_**eff**); ZERO room-geometry textures
(z150m walls/panels/consoles) match. Shared effects are bundled into many zones' TXDs,
so that is expected and does NOT indicate room correspondence. (Earlier fuzzy-correlation
"matches" were false positives — analysis/texture_match_FALSE_POSITIVE.png.) So these
dumps CANNOT
validate the 0696 room geometry/binding directly. Doing that needs a dump captured IN a
0696 room: a PCSX2 savestate parked there (Dan), loaded headless via `-statefile`, then
Shift+F8 -> gsbind.py reads the binding. The toolkit is the ready-to-use reader for that.

## RUN SUMMARY (2026-10-15, autonomous)
BIG WIN: the **SMD skinned-character model format is SOLVED** — the format the DoC
community explicitly never cracked (they used PCSX2 + 3DRipperDX GPU-captures). From
"overstated + broken" this morning to a from-scratch decoder that handles **ALL 286
containers that actually hold a skinned mesh (SM2)** — 279 reproduce the community
rips EXACTLY (verts+tris), 7 near-exact (±1-2% tri, render fine) — with correct
thickness, connectivity, textures and the game's own normals. Baked into
render_skinned.py. (Triage: of 316 containers, 286 have SM2, 1 has MOD, 29 are NOT
models at all — textures/tables/encrypted/skeleton-only. So "279/316" understated it;
the true figure is 286/286 skinned meshes decoded.)
Key unlocks: Dan's "inflate thickness" insight (= /2048 scale), the rooms agent's
0x8000 list/strip flag, header-anchored enumeration + greedy num==count association,
and using the V4-8 packed normals. Validated visually on Vincent/Weiss (Dan-confirmed
pixel-match) + Azul/Rosso.
ALSO: rooms assessed — textures + floor/walls correct, complex central geometry still
messy (deep work). Record (STATE-OF-THE-PORT.md) corrected.
ALSO (2nd pass): ROOM "MANGLED TEXTURES" ROOT-CAUSED. It is NOT a geometry/position
bug (position decode proven robust: VIF-state persistence hypothesis disproven,
reset==persistent for all 363 packets). It is the texture<->submesh BINDING: the
per-packet header carries no material id, there is no on-disk per-submesh material
index, the 16 real material IDs sit at payload+0x1900 but are mapped to submeshes at
RUNTIME by the loader handle table (now PROVEN — baked EE pointers 0x22de50 found).
So the decoder's dominant-order texture guess scatters atlases => mangled look.
Fixing it needs the ORACLE (dump the loader's assignment), not a decoder tweak.
Did NOT change grouping code on a guess. Full probe trail: session notes Section 11.
ALSO (3rd pass): NATIVE GPU PIPELINE PROVEN END-TO-END. SOLVED decoder -> DMSH ->
native/gpu/render_char on real AMD GPU (EGL/Mesa GL 4.6, not Blender) renders a
coherent lit Vincent (7327 tris). The shipping path works on real decoded content.
Renders: geometry-debug/NATIVE-GPU_VINCENT_*.png.
ALSO (4th pass): ROOM BLACK-GASH BUG FIXED. STRIP submeshes are QUAD GROUPS (VU1
'strip 4-loop'), not continuous strips — continuous stripping bridged separate wall
quads with long spanning triangles (the gashes). Fixed in export_bmd_textured.py:
room96 gashes 31->8, renders as a coherent Shinra interior now (geometry-debug/
room96_QUADFIX_3q.png vs _TEXTURED.png). Room geometry (positions+connectivity) now
correct; only the texture<->submesh binding remains (the runtime/oracle item).
ALSO (5th pass, 2026-10-16): BINDING INVESTIGATION DEEPENED + a false doc corrected.
(a) PROVEN the giftag field FORMAT.md §3.4 called the "material selector" is the
    per-submesh VERTEX COUNT: qw1.x & 0x7fff == unpacked V3-16 num for ALL 1273
    packets of z128 06800212 (probe_giftag.py). The "16 selectors 0x03..0x20" are the
    16 distinct vertex counts (room is tessellated into tiny quad/tri submeshes).
    => export_bmd_textured.py's 16 "material groups" are VERTEX-COUNT BUCKETS, not
    materials: a second, deeper root cause of the mangled textures. DECODER-STATUS
    already had this right ("no on-disk per-submesh material index"); FORMAT.md §3.4
    and the export docstring were wrong and are now CORRECTED in-repo.
(b) Confirmed the real binding chain from decomp: FUN_002e62b0 reads a RUNTIME group
    table M (M=*(sceneNode+0x18) via FUN_002e5c60): group count *(u16)(M+0x40), group
    array *(M+0x60), per group subcount@+6, MATERIAL SELECTOR u16 @+8, sub-geom array
    @+0x20; selector->FileCode via handle table context+0x24. BMD file node graph
    (FUN_0026abe8 hdr -> FUN_0026ba88 nodes -> FUN_0026f518 leaves, leaf ptr-array
    @+0x40 -> VIF blocks) is file-followable; mapping it to M+0x60 needs the
    model-register trace (the clearly-defined general-fix task; offsets above).
(c) FALSIFIED the simple draw-order binding via the dump (probe_draworder.py): textures
    are heavily interleaved (dominant tbp 0x2e50 = 10003 tris in 2202 runs). The oracle's
    room atlases are only moderately contiguous (4-24 runs); the hugely-fragmented ones
    are character(Deepground soldiers/Vincent)+effect draws. Room total in the dump is
    ~6180 visible tris vs one chunk's 12647 => this dump is a CULLED, MULTI-CHUNK,
    character-laden view. So per-submesh binding is NOT reliably recoverable from THIS
    dump (culling gaps + chunk-mixing defeat both order- and UV-alignment). Unblock via
    the group-table parse (b), or a cleaner single-room dump/savestate.
(d) KEY: the binding CHAIN is already ported + parity/mutation-tested in
    native/engine/scene/material.{c,h} (doc_material_resolve_texture: draw_word ->
    group/submesh -> material selector -> layer tags -> ctx+0x24 slot -> texture
    descriptor; from FUN_002e62b0/2e8830/2ebc48/2ebb18/0030ab10). It is ONLY fed
    synthetic fixtures (test_material.c, main.c demo) — no loader parses a real BMD
    into DocMaterialModel, and export_bmd_textured.py doesn't use it. So the remaining
    work is concrete engineering, not research: (i) write a BMD->DocMaterialModel
    loader (group table + material-desc table + layer lists from file bytes; find the
    model-register that builds M+0x40/+0x60/+0x64 from the node graph), and (ii) supply
    the ctx+0x24 slot->FileCode handle table (TXD-load port, or the GS oracle per room).
NEXT (deep, each its own effort): room texture binding via oracle (characterised,
ready to hand off); the 7 SMD near-misses (cosmetic, render fine); the ~29 non-SMD
MOD-format models; skeleton animation (gameplay rig).
Images for review: /srv/shared/dirge-port/geometry-debug/ (char_*_lit_3q, room96_*).



Rule: nothing is "WORKING" here unless backed by an objective check (batch match
vs rips, or Dan-confirmed visual). "Looks right" in a grey render is NOT working.
Updated as work proceeds. Last update: 2026-10-15.

## Legend
- ✅ WORKING — validated (batch exact-match count or Dan-confirmed)
- 🟡 PARTIAL — works for a subset / close but not exact
- 🔴 BROKEN / NOT DONE
- ⚪ NOT STARTED

## Character models (data/chr/*/models.rfd — 316, all have rips as answer key)

| Component | Status | Evidence |
|---|---|---|
| Archive/RFD unpack | ✅ | kel.c byte-exact; RFD parsed |
| Skeleton (SK2) load | ✅ | load_skeleton works for rigged models |
| Scatter-pool vertex positions | ✅ | build_group_pool; 3482/3484 unique vs Vincent rip |
| **Position scale (/2048)** | ✅ | Dan-confirmed: Vincent + Weiss pixel-match rips. Was /4096 (2x too small) |
| Per-submesh connectivity flag (0x8000 list/strip) | ✅ | VU program_A idx43; idx==count invariant |
| Submesh enumeration (header-anchored, word-aligned) | ✅ | finds all headers; robust to STROW/STMASK |
| Header↔index association (greedy, num==count) | ✅ | handles interleaved AND separated layouts |
| **Geometry batch (SMD models vs rips)** | ✅ **286/286 SM2 containers decoded** | 279 exact vs rips + 7 near-exact (still decode SMD, ±1-2% tri). EVERY container that actually holds a skinned mesh (SM2) is handled. |
| 29 "neither" containers — TRIAGED (not models) | ✅ resolved | Of 316: 286 have SM2, 1 has MOD, 29 have NEITHER. The 29 are NOT models: textures (texture.txd/TIM2/*_arst), data tables (bt_v0NN), ENCRYPTED blobs (data/chr/g/ 0c..b1ca high-entropy), skeleton-only (SK2+anm), u16 index tables. They are size-match false positives from my models.rfd-size container id step, NOT an SMD decode gap. (scratchpad/triage29.py) |
| 1 MOD-format model (0c38440c, evm012) | ✅ (decodes w/ existing doc_model.c) | the ONLY genuine non-SMD character model (data/chr/n/0/2/6, evm = event prop). Existing native/assets/models/doc_model.c decodes it: version 19, MOD, 81v/79t, coherent bbox, wrote OBJ+glTF. (uv[0..0] + it may read 1 of 4 MOD sub-blocks — minor, 1 tiny prop.) MOD format fully documented in FORMAT.md 2.2-2.3. |
| 7 near-misses (±1-2% tri) | 🟡 (won't-fix, cosmetic) | SMD models, render perfectly. VARIED small causes: e001 07650041 = 1 false header (384 hdr vs 383 STCYCL) -> +32 tris over; 07700001 = under-capture (-65, equal hdr count); 07703241 = header off-by-one. Each needs per-case handling; counts off ~1-2%, invisible in render. Not worth chasing. |
| UV/colour association (separated layout) | ✅ | captured after each idx; e002 renders textured=yes |
| **Baked into render_skinned.py** | ✅ | assemble() = header-anchored + greedy assoc + uv/col; 279 through repo, Vincent 7327/Weiss 3127 exact |
| Winding / normals (lit shading) | ✅ | FIXED: use the game's own per-vertex normals (V4-8 xyz = packed normal, bone-rotated). Baked into build_group_pool/assemble/export_mesh/render. Vincent lit render clean, no dark backfaces |
| Texture/UV render | ✅ | uv/col associated per idx; all layouts render textured |
| Visual generalization | ✅ | Vincent + Weiss (Dan-confirmed pixel-match), Azul e001 + Rosso e003 (coherent lit renders). char_*_lit_3q in /srv/shared/dirge-port/geometry-debug/ |
| **Native GPU pipeline (shipping path)** | ✅ **PROVEN end-to-end** | SOLVED decoder -> DMSH -> native/gpu/render_char (real AMD GPU, EGL/Mesa GL 4.6, NOT Blender) renders a coherent, correctly-lit Vincent (cloak, headband, golden clawed gauntlet all right), 7327 tris. Built `make char`; mesh from render_skinned.py (Vincent 5602v/7327t exact). Renders: /srv/shared/dirge-port/geometry-debug/NATIVE-GPU_VINCENT_*.png |

### SUMMARY: ALL character-model containers decode. 286/286 SM2 skinned meshes via render_skinned.py (279 exact vs rips + 7 cosmetic near-exact, community-unsolved format) + 1 MOD prop via doc_model.c. The other 29 of 316 containers are NOT models (textures/tables/encrypted/skeleton-only). Normals+textures correct; proven end-to-end on the real native GPU (coherent Vincent). Character model extraction is effectively COMPLETE. Remaining: 7 cosmetic near-miss tri-counts (render fine) + skeleton ANIMATION (separate, not started).
| Skeleton animation (gameplay rig) | ⚪ | not started this session |

## Rooms (BMD, data/zone/*/model.rfd — NO rips, oracle-validated)
BMD is a DIFFERENT format from SMD chars: STROW/STMOD delta-decoded positions,
no scatter pool, FACE normals (not game normals). Decoder: export_bmd_textured.py.
| Component | Status | Evidence |
|---|---|---|
| Per-submesh list/strip flag | ✅ | rooms agent; VU-confirmed; 06960212 = 171 list/192 strip |
| **STRIP = QUAD GROUP (black-gash fix)** | ✅ **FIXED 2026-10-15** | The VU1 'strip 4-loop' means STRIP submeshes are QUAD GROUPS: every 4 verts = 1 planar quad, restart every 4 — NOT a continuous strip. Continuous stripping bridged the separate quads with long spanning triangles = the room BLACK GASHES. Evidence: every strip count is mult-of-4 (192/192, 584/584, 1586/1586 across 3 rooms), 97-99% of each 4-group is coplanar, the gash tris are exactly the ones straddling a 4-group boundary. Fix in export_bmd_textured.py quad_tris()/packet_tris(): room96 gashes 31->8 (remaining 8 = legit large quads), tris 4753->3157. Re-rendered: gashes gone, coherent Shinra interior. before/after: geometry-debug/room96_TEXTURED.png vs room96_QUADFIX_3q.png |
| Coherent room render | 🟡 | DIAGNOSED: top-down wireframe shows COHERENT rectangular rooms/floors/walls (positions/delta-decode OK). CENTRAL machinery is jumbled overlapping slabs. Removing 188 needle-slivers did NOT fix the center -> it's overlapping/mis-placed complex geometry, NOT just needles. Top-down coherent + 3q messy => likely a HEIGHT(Y)/placement issue on the complex central packets (delta accumulate or per-object Y offset). Floor/walls are right; complex geometry needs deep work. Images: room96_geometry_culled.png, room96_topdown_wire.png, room96_deneedled.png |
| Room TXD atlas decode | ✅ | TXD atlases decode (224 PNGs for 06960212); the raw textures are correct Shinra interior panels/consoles/numbers. (dark because Shinra textures are dark.) |
| Room per-vertex data (pos+UV+baked colour) | ✅ | all three are exact decoded data, 363/363 packets lock (7491 verts, 0 mismatch) |
| Room texture<->submesh BINDING | 🟢 **SOLVED + WIRED INTO ENGINE for z143** (54.5% authoritative: 52.3% strong + the one missing column atlas 068fff12, 2026-10-17; + 1-NN back-face fill to ~100%); loader feeds material.c; whole room renders textured. The residual grey is NOT missing textures — it is geometry never rasterised in the 18 GS angles (back-faces); the visual fix is a game-matching camera, not a bigger oracle. | **2026-10-16 LATEST — LOADER + WHOLE-ROOM RENDER DONE.** The recovered binding is now consumed by the engine and rendered. (a) `native/assets/models/emit_loader_manifest.py` bakes a decode-order manifest (per-slot FileCode + dtex `--info` w/h/psm/cpsm/clut); (b) `native/engine/scene/material_loader.{c,h}` synthesizes a DocMaterialModel from it and feeds the UNMODIFIED material.c resolve chain — `tests/test_material_loader.c` parity-checks all 13 chunks (15332 packets, 8018 matched, per-slot histograms == recovered binding, texture metadata matches; mutation-tested; engine suite 11/11); sharding handles >512-packet FileCodes; `--heuristic` confident-fill reaches ~100%. (c) `native/assets/models/export_room_bound.py` unions all 13 chunks (shared world frame), merges by FileCode, and renders textured via `native/gpu/render_env_textured` (EGL+llvmpipe): oracle 52.9% tris, heuristic 99.3%; synced to /srv/shared/dirge-port/binding-renders/whole-room/. Binding subsystem COMPLETE end-to-end. Back-face fill = cosmetic free-camera only (source='heuristic', 75.2% validated), never folded into the authoritative 52.3%. — **2026-10-16 UPDATE — SOLVED for a clean room.** A clean single-room GS dump (z143 Mako Reactor 0, Vincent only, US SLUS-21419) + the right method cracked it: build a TBP0->FileCode oracle (18 room textures at corr~1.0; US dump == EU assets), then **UV-triple identity match** (decoder UV=raw/4096 ≡ dump ST/Q) — STABLE over Q 256..4096, binds **8020/15332 packets (52.3%) across all 13 room chunks, 18 textures, ≤4 vote-conflicts** by UNIONing camera angles decode-free (grow_fingerprint.py: seed master UV-triple→FileCode from oracle frames, derive each new angle's TBP0→FileCode by near-unanimous UV-triple agreement, harvest). Consumable: `export_bmd_bound.py` -> per-FileCode DMSH meshes (52.9% tris textured) -> rendered textured via `native/gpu/render_env_textured` (out/bound_renders/, synced to /srv/shared/dirge-port/binding-renders/). What made the EARLIER dump fail was NOT the method but the scene: z128/Kalm was culled+multi-chunk+character-laden. Draw-order and UV-overlap matching are still wrong; UV-TRIPLE is the method that works. **GS-DUMP METHOD SATURATED 2026-10-16: 18 angles = same 52.3% as 7** (+11 fresh angles → +2 packets; fingerprint doubled to 11251 triples but all new surfaces fell on already-covered packets). Residual ~48% = geometry no in-room camera rasterises (back-faces/structural/far-LOD); chunks 068f0712 (96% dark) & 068f0d12 (97% dark) unreachable from all 18 angles. More angles cannot help. SCALABLE GAP (all rooms, visibility-independent): recover the full runtime material model `M`. STATIC RECOVERY DISPROVEN 2026-10-16 (2 deep investigations): the `_KEL/noPath` extracts are post-relocation memory images with STUB selectors (mapid/cid=collision; VIF stream has zero TEX0 writes; FED 068f0N14 selectors=0; BMD group-selector field=uniform stub 0x0022de50 — the very 0x22de50 handle ptr noted below). Binding is runtime-only: keyed by per-draw command word in FUN_002e62b0, selector=u16(group+8)→context+0x24 handle built at TXD load. So only 2 sources: GS-dump oracle (visible 52%, SATURATED) or ONE z143 EE-RAM savestate/live breakpoint on FUN_002e62b0 → populated model+0x60 selectors + context+0x24 handles = 100% incl. dark chunks. Walker scratchpad/mwalk.py ready; scene roots globals 0x4EE2E0/0x4EE3C0 (heads 0x4EE358/0x4EE438), node+0x18=M. The 8020 bound packets are the ground-truth check set. See notes/session-2026-10-15-gsdump.md + out/z143_room_uvtriple_binding.json. — ORIGINAL (z128, 2026-10-15): THIS is the "mangled textures" Dan saw. Binding is runtime-resolved (per-packet header has no material id; 16 material IDs @payload+0x1900; handle records @0x1940, EE ptr 0x22de50; FORMAT.md §6). **NOW HARD-PROVEN with a real GS dump:** the prologue GS dumps are zone **z128 "Kalm"** (10 pixel-identical textures), and comparing the decoder's dominant-order guess vs the dump's ground-truth texture->triangle binding across ALL 13 z128 chunks: **0/9 drawn room textures are correctly bound** — 8 of 9 are extracted but assigned to NO group anywhere, 1 to a too-small group. The heuristic extracts 138 textures/chunk but assigns the same WRONG 16. Oracle data saved: native/assets/models/out/z128_dump03_binding_oracle.json (texture 0680ffNN -> visible tris). UPDATE 2026-10-16: the exporter's groups are VERTEX-COUNT buckets (giftag field proven = vertex count, not a material selector, 1273/1273), so binding is wrong on two counts. Simple dump-based recovery (draw-order AND UV overlap) is NOT viable from this dump — it is a culled, multi-chunk, character-laden view (~6180 room tris vs a chunk's 12647; dominant textures fragmented into thousands of runs). REAL FIX PATHS: (1) parse the runtime group table from the BMD file — FUN_002e62b0 layout: M+0x40 count, M+0x60 group array, group+6 subcount, group+8 material-selector u16, group+0x20 sub-geom; needs the model-register trace mapping the node graph (FUN_0026abe8/ba88/f518) to M; or (2) capture a cleaner single-room GS dump (savestate parked in a room, few characters). See notes/session-2026-10-15-gsdump.md (2026-10-16 addendum). |
| Room position decode robustness | ✅ | VIF-state persistence hypothesis DISPROVEN: all 363 packets re-emit own STROW+STMOD(2), reset==persistent decode (0 move). Central jumble is NOT a position bug |
| A "room" = MULTI-CHUNK world-space ASSEMBLY | ✅ understood 2026-10-15 | ONE BMD container is a CHUNK, not a whole room (objective: single-container enclosure only ~50-65% by raycast). Zone 0696 has ~11 chunks (06960012..0a12) ALL in shared world space (bboxes tile; big 112/812 = outer shell, 212/312/412/512/612 = interior machinery/consoles). Per-chunk world placement = BMD payload+0x1990 transform. ASSEMBLING them + 360 pano from inside shows a RECOGNISABLE enclosed Shinra interior (vaulted ceiling, mako tanks, console banks, patterned floor) matching the real screenshot. geometry-debug/room96_ASSEMBLED_360pano.png. => native engine must assemble all zone chunks. scripts: scratchpad/room_assemble.py, enclosure.py |
| Room overall | 🟡 (decode correct; assembly understood) | per-chunk geometry (positions + quad-group connectivity + UVs + baked colours) all correct; black gashes FIXED; and a ROOM is the multi-chunk assembly (above), which renders as a coherent enclosed Shinra interior. Residual: (a) texture<->submesh binding still approximate (runtime/oracle gap) (b) a few more chunks (712/912/a12) for 100% enclosure. Blender: scratchpad/room_tex.py / room_assemble.py. NOTE: port the quad-group fix to export_bmd_mesh.py if still used; doc_model.c MOD path uses its OWN validated strip (FORMAT.md 2.3), leave it. |

## Key facts (don't re-derive)
- Vincent p005 = extract/_KEL/noPath/07700141.bin = rip file13148/13215.obj (5602v/7327t)
- Weiss e000 = 07650001.bin = rip file10068.obj (3016v/3127t)
- scale value f32@+0x2c=1.732957; sc=f32/2048
- Blender: /home/dan/blender-4.2.0-linux-x64/blender + OCIO env (NOT /usr/bin/blender)
- Working decoder: scratchpad/robust6.py (counts only) — needs uv/col for full render
- Containers list: scratchpad/containers.txt (316, path|absbin)
- Rips: scratchpad/int_models/*.obj (315) + *.smd (316, with skeleton+weights)

## Current task queue (in order)
1. Bake robust6 enumeration+association into render_skinned.py assemble() WITH uv/col
   (must keep Vincent/Weiss textured render working — re-verify after).
2. Static-model decode path (~30 no-skeleton models).
3. The 7 near-misses.
4. Winding consistency.
5. Re-run batch after each; update this table with numbers.
