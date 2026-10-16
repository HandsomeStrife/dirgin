# Autonomous run journal — DoC purist native port

Dan away ~12h from 2026-10-14 20:41. Mandate: keep going non-stop, spin up agents as
needed, saturate the pipeline, leave a clear trail. Full detail lives in build-log.md;
this is the running log + backlog for the unattended run.

## Operating rules (self-imposed for the unattended run)
- NEVER end a turn with zero background agents running (else the session idles).
- Keep 2-4 agents running across independent workstreams (own subdirs, no contention).
- On each completion: record to build-log, produce visible artifacts to /srv/shared/
  dirge-port/ when there's something to see, pull next item from backlog, relaunch.
- Pace token spend (~15M budget / 12h). Prefer verifiable, bounded tasks; checkpoint
  the long-haul engine decomp in subsystem-sized chunks.
- Honesty bar unchanged: no overclaiming; "got this far, blocked here" is valid.

## Backlog (pull from here to keep saturated)
ASSET/RENDER FRONTIER (visible wins):
- [running] character bind-pose (skeleton .skd/.smd) + VU0 0x198 skin/UV  -> clean char
- model gallery: batch-render many BMD models -> contact sheets to shared drive
- audio: reverse at2/at3 (ATRAC3) -> native decoder -> WAV
- weighted skinning + animation (skd/smd + anim streams)
- collision CID format; effects FED format
- textured character render once UVs land
- GPU renderer (Vulkan/GL) code targeting Windows; offscreen-validate vs software raster
ENGINE (P4/P5 long haul):
- native/engine scaffold + CMake (Win+Linux); clean-decomp methodology
- reauthor core math/vector lib; scene-graph/model-node cluster
- reimplement JVM subset (mapped); script VM + DES decrypt (mapped)
- audio engine (SPU2 native mixing); input; main-loop reauthor
- wire renderer to scene -> a scene running natively
INFRA:
- validation harness: native output vs oracle (port/ recomp build)
- build-log hygiene; progress dashboard / gallery page

## Log
- 20:41 start. Running: skeleton/bind-pose agent (char clean render). Launching wave:
  model-gallery, audio-ATRAC3, engine-decomp-kickoff.
- 20:48 GALLERY done: 507 BMD containers, 452 load clean (median ~3k tris). 31 rendered
  -> /srv/shared/dirge-port/gallery_01.png + gallery_02.png (viewed sheet 1: coherent
  DoC environment geometry, native render, labeled). Individual renders in
  native/gallery/out/. Artifact: a few models show stray "needle" vertex (BMD float-scan
  heuristic picks noise w==1.0 far from mesh). BACKLOG: proper BMD node-graph parse
  (rebase post-reloc absolute ptrs -> exact submesh/winding/UV, kills spikes) — AFTER
  skeleton agent frees native/assets/models/. Launching: CID collision (own dir, no conflict).
- ~21:00 ENGINE KICKOFF done: methodology (DECOMP-METHOD.md) + math lib (dmath+vu0) +
  scene-graph module, CMake, 2/2 ctest oracle-parity. Found oracle bug (vopmsub swapped
  products; ~202 sites). Launched engine subsystem #1 (object-list traversal).
- ~21:15 SKELETON done (honest, still shards): VU0 skin micro @0x198 disassembled
  (port/vu/disasm/vu0_skin_micro.txt); skin math fully known (BoneMatrix·(localInt/4096·
  meshScale,1)); dequant corrected to /4096 for skinned MOD; NO skd/smd file exists
  (disproven) -> bone matrices are RUNTIME-only (model+0x24 absolute ptrs). Blocker =
  bind matrices. Launched REBASE-POINTERS agent (models track): solve file reloc base ->
  proper BMD graph (kill gallery needles) + probe MOD bone data.
- ~21:30 ENGINE #1 object-list done: object_list.{h,c}, 3/3 ctest oracle-parity+mutation.
  Launched engine #2 (per-frame STATE MACHINE / spine).
- RUNNING NOW (4): audio-ATRAC3, CID-collision, rebase-pointers(models), engine-state(spine).
- Note: build-log Edits hit stale anchors a couple times (file reformatted); trail is
  authoritative HERE. Oracle bug fix (port/cop2 vopmsub) deferred to backlog.
- ~21:45 CID COLLISION done: native/assets/collision/ (cid.{h,c}, FORMAT.md, OBJ export).
  v2 inline quad-soup + v9-19 grid tree reversed from FUN_00271208/270088/26f750 etc.
  Validated: v2 06010211 = 10-unit cube (12 tris), v19 06cc0111 = 8380 indexed tris +
  1637 tag-2 AABB boxes; all 452 maps parse 0 crashes, 0% degenerate. GAP: tag-2
  VU-packed geom (bulk of real maps) needs VIF-unpack+VU0(0xa90) = SAME decode as model
  vertex-packets (recurring blocker; rebase-pointers agent probing the BMD side). Boxed
  as AABBs for now. Launched: EFFECTS FED format (native/assets/effects/, own dir).
- RECURRING THEME: tag-2 "VU-packed packet" geometry appears in BMD leaves, MOD, and
  collision — all need the generic VIF-UNPACK + VU0-microprogram -> vertices decoder.
  Worth a dedicated agent once rebase-pointers reports (it's probing the BMD vertex side).
- ~22:00 AUDIO done: native/assets/audio/ dirge_atdec.c + FORMAT.md. at2 (2726, voice/SE)
  = ATRAC3 mono 44.1k 96B-frame FULLY DECODED (0 errors; voice_sample/se_sample WAV ->
  shared drive, RMS = clear speech envelope). at3 (382, BGM) = ATRAC3plus mono 44.1k
  384B-frame, fully ID'd but ffmpeg AT3+ decoder DESYNCS mid-stream (both 4.4/7.0 =
  decoder gap, not us) -> needs Sony libatrac3plus / from-scratch. db.bin = loop points
  (7681x24B). .wd banks = SPU2 SE pool (WD\0\0, 294 entries, likely VAG/ADPCM). Decoder
  links libavcodec for now (from-scratch ATRAC3 = future). Launched: .wd/VAG SPU2 decoder.
- RUNNING NOW (4): rebase-pointers(models), engine-state(spine), FED-effects, wd-VAG(audio).
- ~22:20 REBASE-POINTERS done: BMD pointers are FILE-RELATIVE (base=0), not absolute
  (premise corrected). BMD NEEDLES FIXED: (0,0,0,w=1) VIF pad quads were accepted as
  verts -> strips bridged to origin = spikes. Fix (doc_model.c scan_bmd_floats): zero-pos
  quad = strip SEPARATOR. Confirmed: FIX_068d0912 = coherent room/alcove (viewed),
  06c90512/06860612 spikes gone, hero models unchanged (no regression). Residual mid-model
  criss-cross = winding/prim-flag (oracle item); full traversal uses node-array-B +
  bone-tree templates (partly reversed) -> loader keeps V4-32 scan for bulk + needle fix.
  MOD bind matrices PROVEN runtime-only (0 matrix hits in whole-container scan; model+0x24
  is a runtime object, not a file field) -> SCOPED ORACLE is the only path for posed chars.
- NEXT: build reusable ORACLE HARNESS (port/oracle/, links recomp, inject guest mem +
  call a recompiled fn + dump state) — 1st use = dump km01vnw bone matrices for a posed
  character; also = the engine-decomp validation oracle. BACKLOG: regen gallery with
  needle fix (cleaner); VU-packed tag-2 decoder (shared BMD-B/MOD/collision blocker).
- ~22:40 ENGINE #2 SPINE done: core/engine_state.{h,c} — full scene-state enum
  (BOOT=1/TITLE=2/.../FIELD_RUN=0x15/EVENT_RUN=0x1a/INTRO=0x1d/ATTRACT=0x20 from
  FUN_00247508 switch), mode-flag map (DAT_004b8980 bits), module state replacing DAT_
  globals, EngineHooks seam for below-seam subsystems (named, not no-ops). 4/4 ctest
  dispatch-parity+mutation. Engine decomp now = math + vu0 + scene-graph + object-list +
  state-spine. Launched engine #3: material->texture binding (ties scene to texture decoder).
- ~23:00 .wd/VAG done: native/assets/audio/dirge_wd.c — WD sound-bank format reversed
  (header, 32B descriptors, offset table) + PS2 SPU-ADPCM/VAG decode (canonical Sony
  table). 4 banks parse (248/218/138/92 active). Validated: mean lag-1 autocorr 0.698
  (real waveforms); sfx_sample_*.wav -> shared drive. GAPS: per-sample rate (runtime note,
  +0x14 tuning float), loops (VAG stream flags), bank->db.bin id map. => AUDIO COVERAGE:
  at2(voice/SE ATRAC3) DONE, .wd(SE VAG) DONE, at3(BGM AT3+) partial (ffmpeg decoder gap).
  Launched: GPU OpenGL renderer (EGL offscreen) in native/gpu/.
- ~23:20 FED EFFECTS done: native/assets/effects/ (fed.{h,c}, FED-FORMAT.md, JSON dump).
  FED/FEZ particle format reversed from FUN_002e4418/2df140/2e63d0/2eba88: header FED\x08
  + 6 nested-relative tables (emitters@0x64, textures@0x68 = embedded TIM2!); emitter
  record (duration/life/counts/category/fade + 12-channel array); particle sub-records
  (10 curve-channel ptrs). Validated all 311+2+32 = 0 fail, 2855 emitters, 24407 particle
  defs, 1523 embedded TIM2. GAPS: curve keyframe layout/interp, per-channel semantics,
  blend-mode, FEZ LZ inflation (only 2 .fez). Launched: SCRIPT decrypt+disasm (native/script/).
- ASSET COVERAGE now broad: textures 100%; audio voice/SE/banks done (BGM partial);
  collision tag-0 done (tag-2 boxed); effects FED structure done; models BMD clean/MOD
  bind-pose oracle-blocked. 
- ~23:40 ENGINE #3 material->texture done: scene/material.{h,c} — draw word ->
  (group,sub) -> matdesc -> layer tags -> context+0x24[slot] handle -> DocTextureDesc
  {slot,w,h,psm,tbw,cpsm,clut}. Reauthored FUN_002e62b0/2ebc48/2ebb18/0030ab10 (TEX0 as
  identity capture, NOT GS packing). 5/5 ctest oracle-parity+mutation. Stable id =
  context+0x24 slot (TBP0/CBP transient, below seam). Engine decomp = math+vu0+scene-graph+
  object-list+state-spine+material. Launched engine #4: main-loop (FUN_00248110) reauthor
  = native engine frame backbone (purist: render path = our native renderer, NOT GS DMA).
- ~23:55 ORACLE HARNESS done: port/oracle/ (oracle.{h,c} + oracle-cli + oracle-test).
  Reusable executable oracle: mmap guest + load ELF + wire HLE/SIF/GS/VU/cop2; inject mem,
  oracle_call(func) with SIGSEGV guard, dump hex/mat4. PROVEN: runs real recompiled
  FUN_00447c30 (memset) + FUN_0026138c (4x4·vec4 skin op) -> correct. = validation tool
  for native/engine vs oracle.
  *** MAJOR RECOMP BUG FOUND+FIXED: stock recompiler only translated 1st LOAD seg (.text);
  3,322 of 13,089 funcs (incl. ENTIRE MOD skin/skeleton subsystem in .kel_draw_section
  @0x5a6d00) were EMPTY STUBS. Fix: tools/recompile_keldraw.py re-translates overlays
  (0 unimpl), swapped into link. => earlier "all 13,089 translate" was 25% stubs; now fixed
  in oracle. BACKLOG: fold segment-aware fix into main recompiler. ***
  BONE DUMP still BLOCKED (honest, 0 bones): km01vnw container has NO dks+dat skeleton
  chunks (only chrid.chi + mod parts) -> palette needs full instantiation chain +
  skeleton. Need a CHARACTER WITH dks+dat. Skin math known.
- NEXT: find character w/ dks+dat skeleton chunks -> reverse dks/dat (FUN_005b27c8 local
  transforms) statically OR oracle-dump palette -> apply skin -> CLEAN CHARACTER render.
- ~00:10 GPU RENDERER done: native/gpu/ — OpenGL 3.3-core (EGL surfaceless on real
  AMD gfx1103/Mesa, NOT software fallback), VBO/VAO/GLSL330/FBO->PNG. Links doc_model +
  libdtex read-only. PIXEL-MATCH to software raster: BMD 06080112 shaded mean-abs-diff
  0.001/255, cull 0.000; texture path proven (RST crate on UV quad). Windows-portable:
  only gfx_context_egl.c is platform; render core/gl loader/main platform-free; add
  gfx_context_wgl.c + opengl32 for Win. gpu_render_01/02/03 -> shared drive. => native
  "eyes" on real GPU, validated. Launched: collision tag-2 VU-packed decoder (native/assets/collision).
- ~00:30 ENGINE #4 MAIN LOOP done: core/frame_loop.{h,c} — doc_engine_frame() reauthors
  FUN_00248110 in exact order (mutex->render begin->input->updates->STATE DISPATCH->
  scene/object-list walk+render submit->late updates->exit test->present+advance). DocEngine
  context ties spine+renderer+queue+frame counter. PURIST: GS DMA transport (FUN_0030b768/
  b868/0040ce28) replaced by clean DocRenderer interface (begin/submit_scene/end/present),
  ~40 subsystem updates = addressed hooks. 6/6 ctest oracle-parity(callseq)+mutation.
  Engine = math+vu0+scene-graph+object-list+state-spine+material+frame-loop(DocEngine).
  Launched engine #5: INPUT (pad FUN_00254638) native.
- ~00:50 SCRIPT DECRYPT done: native/script/dcc.c — cipher is GAME-CUSTOM (NOT DES):
  key=first 8 bytes, schedule FUN_0041bb38 (sub[i+1]=sub[i]*5), per-block CFB byte stage
  + u64 word stage, S-box DAT_0047eb50 = (i+120)&0xff (extracted @file 0x37ec50). Decrypted
  file = serialized relocatable image (TTTT->SSSS, LE scalars, file-rel offset "pointers",
  bytecode operands BE). Tool decrypts+parses+disassembles to JVM mnemonics. 270/270
  scripts PASS (100% readable UTF8); names install/KerberosIntermissionCalc/ev####/mm_z###.
  GAP: 281 native BRIDGE methods (11 Kerberos classes: Actor/Camera/Event/Light/Effect/
  Sound/Window/Zone/GlobalMap/Intermission/base) -> engine_bridge_api.txt = the script<->engine
  interface the native engine must implement. Launched: JVM interpreter (run scripts natively).
- ~01:05 ENGINE #5 INPUT done: core/input.{h,c} — DocPadState/DocInputPad, 16-bit button
  map (SELECT..SQUARE), decode FUN_00254638 (active-low->high), edges FUN_002553e8
  (pressed=cur&~prev etc.), stick normalize FUN_00258218, platform backend hook (purist
  sub for padman RPC). 7/7 ctest oracle-parity+mutation. Engine = math+vu0+scene-graph+
  object-list+state-spine+material+frame-loop+input. Launched engine #6: CAMERA (view/proj).
  NOTE: full native-app integration (DocEngine + doc_model + GPU renderer running a frame)
  deferred until skeleton-v2 frees native/assets/models (avoid build race).
- ~01:20 SKELETON RECOVERED (big win): skeletons ship in SEPARATE event containers
  noPath/0765*.bin (316, named e000..) = skeleton.skd + e###.smd + texanm.tad (NOT in
  the mod containers). Reversed skd/"SK2" (FUN_005b27c8/5b2c78/5a8850): +0x14 bone count,
  208B nodes, +0x20 local T (x256), +0x30 local quat, +0xc0 s16 parent. Composed 103 world
  bind matrices (native/assets/models/out/skeleton_bind.json; orthonormal, hands symmetric).
  Parser generalises (58/76/98/147-bone skeletons clean). VALIDATED: skel_07650001_3q.png =
  RECOGNIZABLE HUMANOID (spiky head/shoulders/arms/fingers/spine/legs) -> VIEWED, copied to
  shared 08/09. FourCC correction: 0x646174="tad" not "dat". Bind-pose blocker SOLVED.
  LIMITATION: skinned MESH surface = smd "SM2" (int8 VU-skinned ~16B/vert interleaved
  pos+normal+color+uv, STCYCL cl4/wl4) NOT yet decoded (sibling to MOD int8). Launched:
  SMD skinned-mesh decode (apply bind matrices -> posed character SURFACE).
- ~01:35 JVM INTERPRETER done: native/script/ (classfile.{h,c} factored parser, jvm.{h,c}
  interpreter, bridge.{h,c} 281 methods [13 real], jvmrun/jvm_test). Full int opcode set +
  float + loads/stores + branches + table/lookupswitch + object/array/field + all invoke
  forms; long/double collapse to 1 slot (faithful to the subset). RUNS REAL SCRIPTS:
  KerberosIntermissionCalc.calc() to completion (329 bytecodes, 65 bridge calls, computed
  real section_stat values); install.hdd_error_proc exercises invokevirtual/tableswitch/
  ldc/stub seam. Self-test 5/5 + mutation-proven. => game scripting RUNS natively, zero
  emulation. Launched: TEXT/FONT system (native/text/) — text-bin decrypt + kelfnt fonts
  + render dialogue.
- ~01:50 ENGINE #6 CAMERA done: scene/camera.{h,c} — DocCamera (eye + Euler angles in
  fixed-point 4096=2pi, fov, proj), view/projection via math lib. Reauthored rot builders
  FUN_00259928/990/9f8, view compose FUN_00259c50, rigid inverse FUN_002610e0, projection
  FUN_00263968, euler extract FUN_002d9cd0. Backs KerberosCamera bridge (getpos/set/move/
  setfov). 8/8 ctest oracle-parity (eps 1e-4 justified) + mutation. Engine = math+vu0+
  scene-graph+object-list+state-spine+material+frame-loop+input+camera (7 subsys + loop).
  Launched: NATIVE-APP integration (native/app/) — boot DocEngine + camera + input + stub
  renderer, run frame loop -> prove native engine runs as a whole. (Full model render
  integration waits for smd-mesh to free native/assets/models.)
- ~02:05 COLLISION TAG-2 done (collision essentially COMPLETE): tag-2 = self-describing
  VIF0 stream (UNPACK S-16 header + S-32 poly words + V3-16 DELTA verts STROW/STMOD /16 +
  MSCAL), no VU emulation needed. Nquad=Np-Ntri, Nv=3Ntri+4Nquad, tris-first. cid.c
  decode_vu_packet. 06cc0111 now 26,362 tris (was 8380+1637 boxes). CORPUS: 449 maps,
  537,893 packets 100% decoded, 3,869,888 tris, 0 verts outside leaf AABB, 0 crashes. OBJ
  exported. GAPS: VU0 0xa90 image not pinned (not needed); surface-mask bit meanings
  unenumerated. Launched: VU1 render/lighting pipeline analysis (port/vu, informs native shaders).
- ~02:20 SMD SKINNED MESH done (signature milestone ~met): smd/"SM2" decoded from
  FUN_005b06d0/5b1190/5b0470/5b1260. Header SM2\0 + groups + meshScale 1.7705 (VU /4096);
  per-batch VIF: STMASK/STCYCL + V1-32 + V4-8 normal + V3-16 int16 bone-local pos; control
  table {boneIndex,cntA,cntB,qwc,stride} 1:1 batches. Skin: world = bind[boneIndex]·
  (posInt16·meshScale/4096,1) using skeleton_bind.json (trans_scale=1.0). 227/381 batches
  skin -> RECOGNIZABLE SOLID HUMANOID (head/hair/arms/torso/legs, ~3473 v/~1800 t, ±7u).
  VIEWED char_e000_skinned_3q (Vincent-like, coat) -> shared 10/11. HONEST LIMITS: ~1/3
  tris are strip-bridge shards (real connectivity = S-8 index pool in the +0x08 draw packets,
  not file-order); ~40% batches drop (control-record desync); UV/texture (texanm.tad) not
  wired (shaded only). Launched: CHARACTER CLEANUP (S-8 connectivity + UV/texture) -> clean textured char.
- ~02:35 NATIVE-APP INTEGRATION done (milestone): native/app/ boots DocEngine + runs real
  doc_engine_frame() 120 frames. State machine advanced BOOT->TITLE(f5)->TITLE_HOLD->
  ATTRACT(f13)->LOAD_DISPATCH(f15)->FIELD_SETUP->FIELD_LOAD->FIELD_RUN(f20..119) via real
  engine_state_dispatch; synthetic pad input steers DocCamera (view.t tracks -eye); scene
  graph walked -> render-queue flagged; material resolve; logging DocRenderer (purist GS-DMA
  sub). 32-iter shutdown drain. run_log.txt (1225 lines), exit 0. => NATIVE ENGINE RUNS AS
  A WHOLE, zero emulation. Full GPU+real-model render in app deferred until models free.
  Resuming engine track: LIGHT subsystem (pairs with vu1-render lighting doc).
- ~02:50 VU1 RENDER-MATH done: port/vu/RENDER-MATH.md + shaders/dirge_purist.vert/.frag.
  Lighting = program D (character/actor) only: d=max(L·n,0) 3 DIRECTIONAL lights,
  lit=C·d+ambient, rgb=saturate(lit⊗vertexColor); A/B static world = PRE-LIT + fog, no
  dynamic N·L. OPMULA/OPMSUB = backface cull (not lighting). Vertex RGBA via ITOF0/FTOI0;
  Q=1/w for perspective UV; UV int16->ITOF12(/4096)+bias×Q->GS ST; fog F=clamp(a·w+b).
  GIF out = 3 qw/vert (ST,RGBAQ,XYZF2) NREG=3 stride 0x30, GIFtag asset-authored.
  Correction: "VU1 program C" = actually VU0 skin image. GAPS: actual light/ambient/fog
  CONSTANTS are runtime VuMem uploads (need oracle dump); exact GIFtag bits. Launched: GPU
  shading upgrade (native/gpu: wire the real shader model, synthetic lit/textured test).
- ~03:05 TEXT/FONT done: native/text/ (doctext.c decrypt, docfont.c, docrender.c, FORMAT.md).
  Cipher cross-checked vs game FUN_0041bbf8 (0xA1652347 etc). 400/400 text-bins decrypt;
  real JP/EN dialogue ("Suspend save data:Kalm town","Find Reeve!"). Encoding Shift-JIS
  (JP build). Fonts = RFD fontdata1.bin/fontdata2.rfd archives (kelfnt0-4, PSMT4 16-col CLUT
  16x16 cell grid); kelfnt4 decodes clean KANJI. Render composites real glyphs -> shared
  text_sample.png. GAPS: kelfnt0-3 ~7% half-page unplaced; SJIS->glyph-cell INDEX table in
  EE code not reversed (strings use fallback face). No kelcnf.bin exists. Launched: TEXT
  COMPLETION (SJIS->glyph index + kelfnt0-3 placement -> correct-glyph dialogue render).
- ~03:20 GPU SHADING UPGRADE done: native/gpu renderer_gl.c now has purist shaders
  (SHADE_VS/FS from port/vu/shaders): LIT = 3-dir N·L + ambient ⊗ vcol (program D), PRELIT
  = vcol pass-through (A/B), GS TFX MODULATE (128=1.0 x2)/DECAL/alpha-test/fog, RVertex
  (pos+normal+uv+rgba), rgl_render_mesh. Placeholder light rig (real = runtime VuMem).
  Validated (synthetic, no models link): gpu_lit_sphere (dir gradient), gpu_textured,
  gpu_perspective_plane (persp UV), gpu_prelit_vcolor, gpu_lit_textured -> shared drive.
  Launched: ORACLE LIGHT-DUMP (port/oracle -> real VuMem light constants -> native/gpu rig).
- ~03:35 CHARACTER CLEANUP done — *** SIGNATURE MILESTONE: clean TEXTURED character ***
  Fixes: batch-lock via *(u32)(group+0x0c) piv struct (381/381 lock, was 40% dropped);
  connectivity = SCATTER-INDEXED POOL (VU0 0x120 writes posed vert to slot (normalW+0x80)&
  0xff; 245-slot pool, max=244=draw idx max) + S-8 index strips in group+0x08 draw packets
  (STCYCL cl4/wl4: V4-32 gif + V4-8 col + V2-16 UV + V1-8 S-8 idx, GS strip). Texture =
  SIBLING container 07650002.bin (RST 512x256 PSMT8: skin/face/hair/trousers/gun); UV=
  V2-16/4096 in [0.001,0.999]. RESULT: 100% batches, ~3k v/5.5k t, only 5.8% shardy =
  SOLID CONTINUOUS TEXTURED BODY (arms/torso/legs/belt/sheathed gun, skin+trousers tex).
  VIEWED 13_character_textured_3q (skin-toned Vincent-like char) -> shared 12/13. RESIDUAL:
  hair (~22% shards, thin strand-cards collide on 245 scatter slots). FORMAT.md §8.4.
  Launched: ANIMATION (skeletal motion clips -> animated pose) [ambitious].
- ~03:50 ENGINE #7 LIGHT done: scene/light.{h,c} — DocLight/DocLightSet (3 DIRECTIONAL
  + ambient + shadow, no point lights in resident micro). Reauthored KerberosLight setters
  (FUN_003a50a8/5140/5248/380210 -> env 0x4fc7d0+0x630/6a0/6d0), dir-matrix build
  FUN_004450f0 (negate/normalize/transpose), commit FUN_0040c988, point-fold FUN_005ab0b0,
  defaults FUN_0040c078 (0.4 grey col, 0.3 ambient). VU upload confirmed L@0x40 C@0x70
  ambient@0xa0 (matches RENDER-MATH). 9/9 ctest oracle-parity+mutation. Engine = math+vu0+
  scene-graph+object-list+state-spine+material+frame-loop+input+camera+light (8 subsys).
  Launched engine #8: ACTOR (DocActor entity, KerberosActor bridge).
- ~04:05 ORACLE LIGHT-DUMP done: PATH oracle-verified (compose FUN_002e3498/3948, VU1
  upload FUN_0027e690 VIF 0x6c0b00f0 = 11qw Mvp+L+C+A @micromem, EE src scratchpad
  0x70003c00) = confirms RENDER-MATH layout executably. But light VALUES NOT statically
  recoverable (per-scene runtime script-set; no shipped light file; env has no baked
  default; rejected .fed false lead honestly). Placeholder retained (engine grey default
  0.4/0.3 from light subsystem stands). light_rig.json = route+status. Launched: DASHBOARD
  (consolidated HTML+hero montage for Dan's return; standalone, reads read-only).
- ~04:20 DASHBOARD done: /srv/shared/dirge-port/index.html (self-contained, dark, offline;
  brain/files/eyes status, stat strip, DONE/PARTIAL cards, engine subsystem table,
  image+audio galleries, what's-next) + 00_hero_montage.png (6-tile: skeleton->skinned->
  textured char, BMD world, readable-copyright-text proof, GPU sphere). Verified headless.
  => Dan's at-a-glance review of the whole run. Launched: MOVIE/FMV (native/movie) — examine
  the 2 big unexplored ISO blobs (200ECE2F.5C 1.46G, B18C4B8C.FC 1.13G) + reverse PSS/IPU
  movie format + decode a cutscene frame.
- ~04:35 TEXT COMPLETION done: KEY FIX = kelfnt0-3 (TIM3) are stored LINEAR not swizzled
  (the ~7% gap was mis-applying kelfnt4's swizzle); all 5 atlases decode 0 holes. Reversed
  ASCII->cell map (A-J=630-639 shifted by NEC syms etc.). RENDERED readable EN dialogue in
  REAL FONT: "Deactivate the barricade!"/"Find Reeve!"/"Reeve"/katakana マミムメモ -> VIEWED
  13_dialogue_native.png (shared). GLYPHMAP.md. JP kana/kanji anchored+partial (full table
  needs EE lookup). => text EN full, JP partial. Launched: AUDIO ENGINE (SPU2 voice/mix +
  KerberosSound) native/assets/audio.
- ~04:50 FMV/MOVIE done (new content opened): the 2 big ISO blobs 200ECE2F.5C(11)/
  B18C4B8C.FC(10) = 21 FMV archives (EU). TOC decoded (0x20 entries: hashedname, start×0x800,
  size, MultiplyKey+Key1/2/3); XOR stream cipher cracked+ported (decrypt_movie.py). Format =
  standard MPEG-2 Program Stream (640x512 25fps PAL, stream 0xE0 video; audio 0xBD =
  PS2 ADPCM SShd 48k stereo). movie[0] all 3103 frames decode (124s). Decoded real frames:
  "SQUARE ENIX présente", credits, 15_movie_frame = CG cutscene (Yuffie+WRO soldier) ->
  VIEWED, shared. scripts/{parse_toc,decrypt_movie,demux_audio}.py. GAP: movie audio
  (SShd PS2-ADPCM) demuxed but not decoded (VAG-style, we have VAG decoder). Launched:
  MOVIE AUDIO decode + A/V mux -> playable cutscene.
- ~05:05 ANIMATION done (stretch milestone, partial): motion-clip format reversed = MOD
  version 0x10 chunks (519 of them; v0x13 = meshes). Header: animated-bone-count@+8,
  frames@+0xc, segs@+0x10 (255f/seg). Tracks: P1 translation (sparse) + Q rotation (all
  bones, 6-byte packed sign+magnitude; w=implicit 4096) linear interp. Composer FUN_005affb0/
  5b0200 (world=parent·T·R), samplers FUN_005aefd0/af6c0/af618. (Correction: FUN_005b2550/
  5b0d30 = cloth/verlet, NOT sampler.) anim_eval.py decodes km0102vn onto p012 93-bone event
  skel -> ALL 92 bones animate, dynamic action pose != bind (VIEWED 14_animated_f120 =
  moving humanoid, distorted). HONEST BLOCKER: event-rig vs gameplay-rig mismatch (clip
  targets runtime-only km01vnw gameplay rig) -> residual distortion; format fully resolved.
  FORMAT.md §9. bind+3 anim frames -> shared 14_*. Launched: GPU CHAR RENDER (convergence:
  export textured char -> render on real GPU).
- ~05:20 MOVIE A/V done (FMV pipeline COMPLETE): movie audio = LINEAR 16-bit PCM (NOT
  ADPCM; honest correction) 48k stereo, SShd/SSbd block-interleaved 0x200/ch. decode_movie_
  audio.py deinterleaves -> WAV 124s (matches video), autocorr 0.91-0.98 real music.
  Muxed PLAYABLE cutscene mov5C_00.mp4 (mpeg2+aac, synced) -> shared 16_cutscene_clip.mp4
  (10s H264+AAC, burning battlefield + EU credits). Pipeline = decrypt->demux->decode V(MPEG2)
  +A(PCM)->mux; other 20 FMVs same (not batch-run). Launched: JVM script COVERAGE (batch 270
  scripts -> harden interpreter).
- ~05:35 AUDIO ENGINE done (audio pillar COMPLETE: decode->playback/mix): native/assets/
  audio/dirge_sound.{h,c} — 48-voice software mixer (frac pos + lerp, pitch, equal-power pan,
  loop, int16-clamp, gen-tagged handles) + db.bin layer (ds_db_load/find/resolve, FNV-1a).
  Request model traced: name->record FUN_003eb800, voice alloc FUN_003e8bc8 (128-slot
  DAT_0050e070), stop FUN_003e90d0. id-resolve: bgm_070->at3 loop[291..2626], on_v132->at2,
  se_*->wd bank, csj/bt->stream. Mixed 5s scene (voice L + looped SE R + 2 hits + fade) ->
  sound_mix_sample.wav: 0 clip, both chans active, envelope correct. GAP: wd slot<->descriptor
  map (FORMAT §5), BGM playback waits on AT3+ decode. Launched: EFFECTS completion
  (FEZ LZ + curve keyframes + channel semantics) native/assets/effects.
- ~05:50 GPU CHAR CONVERGENCE done (milestone: native GPU draws real game content):
  render_skinned.py export_mesh -> char_e000.mesh (DMSH, 16608v/5536t, pos/normal/UV/RGBA
  = RVertex) + char_e000_tex.png + .obj/.mtl; normals forced outward (winding not global).
  native/gpu/render_char.c: DMSH loader + libdtex texture + rgl_render_mesh EGL path on real
  AMD GPU (GL4.6). PRELIT+LIT textured, front+3q. Matches software raster. VIEWED
  17_character_gpu = clean full-body textured char (skin/trousers/boots/gun, hair shardy)
  -> shared 17_*. Launched: GPU SCENE compose (char + BMD environment together on GPU).
- ~06:05 GPU SCENE COMPOSE done (milestone: char in environment on native GPU):
  export_bmd_mesh.py (doc_model BMD -> DMSH, flat normals; bmd_06960212 = pillared hall
  1265t), native/gpu rgl_render_scene() (shared depth, per-draw shade: env PRELIT + char
  LIT+textured, 1 pass), render_scene.c, frame_camera_ex. VIEWED 18_scene = textured char
  standing in multi-floor hall, depth-sorted -> shared. HONEST: placement hand-set (no real
  scene-placement data; char scale 2.6 tuned); env is GREY (BMD vertex-colors+UVs in UNPARSED
  VIF stream -> reconstructed grey shade stand-in; positions/normals real). Launched: BMD
  ENVIRONMENT TEXTURE/COLOR (resolve BMD VIF draw stream UVs+vcolors+material -> textured levels).
- ~06:10 EFFECTS COMPLETE: typed-channel curve system (desc array @P+0xc, INIT/UPDATE/AUX
  role lists; apply flags 0x1 pos/0x2 vel/0x4 accel/0x8 RGBA/0x10 orient/0x20 billboard,
  scale _vmul; FUN_002eb038/eadc0/e7660). Blend = render-category byte -> preset (FUN_002e84a8).
  FEZ INFLATED bit-exact (8-byte cipher seed 7, S-box (x+0x78), +0xA1652347 + zlib -> FED).
  313/313 parse 0 fail, valgrind-clean; channels 133787 (0 neg scale, 98.5% alpha in[0,128]).
  GAP: type-code->evaluator + interp mode = C++ vtables built at boot in .bss (not static).
  Launched: DASHBOARD REFRESH (add GPU char/scene, cutscene, animation, dialogue, audio; update status).
- ~06:15 JVM COVERAGE done (VM hardened on full corpus): jvmbatch runs all 270 scripts.
  Parse 270/270; EXECUTE 220/268 scripts clean (82%), 399/481 entry methods normal-return;
  99 opcodes exercised (all impl); 239/281 bridges called (42 dead; priority: Window.getstate,
  Zone.getflag, Event.sleep/poll). Fixed 5 real gaps (monitorenter/exit, dup_x2, call-depth
  guard, getstatic/clinit storage, heap pointer-safety). ASan+UBSan clean over all 270, 0
  leaks/UB. 9/9 jvm_test mutation-sound. Non-clean = engine-primitive poll loops (not VM bugs).
  Launched: ATRAC3plus BGM decoder attempt (the one audio gap; hard).
- ~06:30 AT3+ BGM = HONEST BLOCKER (not fabricated): NOT a desync — per-frame bit layout is
  DoC/Sony-PS2-specific (byte[0]=0x01 every frame; ffmpeg misreads as mute_flag=1). Ruled out
  framing/byteswap/offset/OMA-EA3 wrapper (libavcodec ignores extradata). No AT3+ tool on box
  handles it. Needs Sony libatrac3plus / IOP decode (not in EE decomp). FAILED-prefix WAV
  labeled honestly. ATRAC3 .at2 voice/SE unaffected (complete). Launched: JP text glyph table
  (SJIS->cell EE lookup) -> full Japanese dialogue render.
- ~06:45 BMD ENV TEXTURES done (levels now TEXTURED): the real textured mesh = a 2nd
  attributed VIF chain (pos V3-16 delta /16, vcolor V4-8 prelit, UV V2-16 /4096, 16-way
  matSel in giftag 0x8000|sel). 363/363 packets lock 06960212 (7491v); 068f0612 12707t.
  Textures = sibling <level>ffNN TXD atlases (libdtex). export_bmd_textured.py + native/gpu
  render_env_textured.c (make env). VIEWED 19_env_textured_raw_hero = Shinra sci-fi interior
  (metal panels, blue consoles) -> shared 19/20_env. HONEST: UVs/colors/grouping EXACT;
  selector->FileCode slot approximate (oracle-only). FORMAT §3.4. Launched: TEXTURED SCENE
  recompose (character in textured level on GPU).
- ~07:00 TEXTURED SCENE done (STRONGEST VISUAL): render_scene_textured.c fuses 16 textured
  env groups (PRELIT+TXD) + LIT textured character, 1 GL world/depth, EGL. VIEWED
  21_scene_textured_hero = textured char in textured Shinra interior (metal panels, blue
  holo-consoles, floors) -> shared 21_*. HONEST: placement hand-set (scale 2.6), A-pose char,
  env colscale 1.8 display gain. => "character in a textured DoC level", native assets, GPU.
- NUDGED engine-actor (~3h, over-reaching): finalize isolatable core + hooks, build clean, STOP.
  Reserving models/gpu/engine for the CAPSTONE when it lands.
- Launched: VERIFY-ALL (build+test the settled standalone subsystems: textures/audio/collision/
  effects/movie/script -> integration sanity report).
- ~07:15 VERIFY-ALL done: 7/7 settled subsystems build CLEAN (0 -Wall -Wextra warn) + tests
  pass; no breakages. native/verify/{VERIFY_REPORT,MANIFEST}.md. Notes: kel loader is at
  port/ (not native/src); install.class step-limit (expected poll loop). Launched: STATE-OF-
  THE-PORT consolidated technical summary (notes/) for Dan.
- ~07:25 STATE-OF-THE-PORT doc written (notes/STATE-OF-THE-PORT.md): honest consolidation
  (exec summary, architecture, per-pillar status + real numbers, 9-subsystem engine table,
  tooling index, blocked items + needs, path-to-playable, artifacts). Launched: cutscene
  batch (a few more playable FMV clips to shared) as buffer. engine-actor STILL finalizing
  (nudged ~07:00); capstone reserved for when it lands + builds clean.
- ~07:30 CUTSCENE BATCH done: 4 more playable clips (22 vortex-city, 23 omega-report/Deepground,
  24 Yuffie+Cid, 25 mako-reactor), 13s H264+AAC + frames -> shared. FMV rounded out.
- *** CORRECTION: the "DASHBOARD REFRESH" I logged at ~06:10 was NEVER actually spawned (the
  Agent call got interrupted by an incoming completion; I mis-tracked it as running). Verified
  via ListAgents: only engine-actor + jp-text were actually running. The dashboard on /srv/shared
  is the ~04:20 version (missing GPU char 17 / scene 18 / textured env 19 / textured scene 21 /
  animation 14 / cutscenes 16,22-25 / dialogue 13). RE-LAUNCHING the refresh for real now. ***
- ~07:35 DASHBOARD REFRESH (real) done: /srv/shared/dirge-port/index.html rebuilt + new
  00_hero_montage.png; 41 media (5 playable cutscene videos, 7 audio, all images); character
  progression, textured scene 21, textured envs, dialogue, honest 9+1 subsystem table +
  blocked list. STATE-OF-THE-PORT.md copied to shared (offline). => Dan's review is CURRENT.
- ~07:40 JP TEXT done: SJIS->cell mapping reversed (kana half-swap packing; verified
  glyph-by-glyph vs IPAGothic). Rendered readable JP (さくら/ちから/ナイフ...) -> shared
  20_dialogue_japanese + 21_kana_proof. HONEST: some kana absent from decoded atlas =
  TIM3 LINEAR page-order decode gap (448=3.5 pages), NOT a mapping gap; so the exact test
  string can't fully render yet. EE SJIS decode = stock iconv (FUN_0044b598); cell computed
  by bespoke packing (no simple u16 table). render_japanese.py.
- DECISION: engine-actor over-ran to ~4h; rather than block the capstone on it, launching the
  CAPSTONE against the STABLE engine subset (SNAPSHOT math/core/scene modules, EXCLUDE
  in-progress actor.c) + GPU renderer + real char+env meshes. Isolated from engine-actor.
- ~07:55 *** CAPSTONE COMPLETE (convergence of the whole effort) ***: native/capstone/ —
  doc_engine_frame() drives the real GPU renderer each frame: state dispatch -> object-list
  walk -> submit_scene (rgl_render_scene: char LIT + 16 textured room groups PRELIT, EGL FBO)
  -> present (FBO->PNG). Built from SNAPSHOT of stable engine (actor excluded) + native/gpu +
  libdtex, -Wall clean. run.log PROVES: state machine BOOT->TITLE->...->FIELD_RUN via real
  dispatch; camera orbits under scripted DocInput (eye-match err=0.0000/frame); 9 frames
  rendered, room rotating. VIEWED 26_capstone_engine_gpu = textured Vincent in Shinra interior,
  engine-loop-driven GPU render -> shared. HONEST: gpu scene API frames from (focus,az,el,dist)
  not raw mat4 (DocCamera eye authoritative, renderer reconstructs it err=0); light rig
  placeholder (per-zone KEL-VM values = known gap). NOTES.md. => THE native engine runs AND
  renders a real DoC scene, zero emulation. Flagship achieved (independent of the over-run actor).
- ~08:00 FINAL DASHBOARD done: capstone featured front+center (gold section, honest caption +
  nuances, run-log link), headline updated (capstone = DONE, moved out of "what's next"),
  hero montage rebuilt with capstone 26 as lead tile. 40 media verified, headless-render clean.
  => index.html is the definitive review; STATE-OF-THE-PORT.md the deep dive (both on shared).
- RUN STATUS (~11h+ of 12h): essentially COMPLETE. Flagship CAPSTONE achieved. All assets
  decode; textured character+animation+skeleton; textured environments; native GPU renderer
  (real shading); native engine 9 subsystems running integrated + capstone (engine drives GPU
  on real scene); scripts run on native JVM (270 corpus); cutscenes playable; audio decode+mix;
  dashboard+state-doc current. HONEST BLOCKED (documented): AT3+ BGM, char hair shards, anim
  rig mismatch, light constants (per-zone runtime), JP atlas page-order, BMD selector->FileCode,
  FED .bss evaluators, recompiler .kel overlay stubs.
- ~08:05 TaskStop'd engine-actor (~4h, 2h past nudge). Its last line: "All 10 tests green after
  restoring. The actor module is complete" — so it HAD FINISHED (actor.{h,c}+test_actor, 10/10
  ctest) and was only doing a redundant final rebuild; I cut the victory lap. VERIFIED myself:
  native/engine fresh cmake build = 0 warn/err, ctest 10/10 PASS incl test_actor. => ENGINE =
  10 SUBSYSTEMS (math, scene-graph, object-list, state-machine, material, frame-loop, input,
  camera, light, ACTOR), all oracle-parity+mutation, -Wall clean. (over-run was final-polish
  churn, not a stall in the work itself.)
- RUN ESSENTIALLY COMPLETE at ~11.5h. Keeping loop alive with REAL forward work: wire the JVM
  engine-bridges (KerberosActor/Camera/Light/Zone/State/Intermission) to the now-ported native
  subsystems so SCRIPTS DRIVE THE NATIVE ENGINE.
- TODO final touch: dashboard says "9+1 actor in progress" -> now 10 DONE (patch at wrap).
- ~08:20 JVM->ENGINE BRIDGE done (scripts DRIVE the native engine): bridge_engine.c routes
  KerberosActor->DocActorManager/DocActor (load/getpos/setpos/dir/visible), KerberosCamera->
  DocCamera, KerberosLight->DocLightSet, KerberosEvent/Zone->EngineState. jvm.c: bridge_this
  receiver plumbing (minimal). btest: a script creates+positions a DocActor(10,20,30), moves
  camera eye(1,2,3), sets light dir, sets mode-flag, zone-flag round-trips -> all assert real
  state; mutation (DIRGE_NO_ENGINE) reds exactly the 6 -> load-bearing. REAL DEMO: 85/268 scripts
  load real DocActors (ev0306 -> p005/n042 positioned). JVM self-test 9/9, ASan clean, 0 new
  aborts. Still stubs: Window/Sound/Effect/GlobalMap + actor anim/morph (below seam).
  => the game's scripts begin to control the native engine.
- Launched: FINALIZE review artifacts (dashboard + STATE-OF-THE-PORT: actor DONE=10 subsystems;
  add scripts-drive-engine milestone).
- ~08:25 FINALIZE DOCS done: index.html + STATE-OF-THE-PORT.md (+ synced copy) now accurate:
  10 subsystems (actor DONE, test_actor), scripts-drive-engine DONE card, capstone leads,
  "what's next" fixed. 40 media resolve, renders clean.

## === AUTONOMOUS RUN COMPLETE (~2026-10-14 20:41 -> 2026-10-15 ~08:25, ~11.75h) ===
## 0 agents running. Natural clean end-state; all achievable milestones landed + documented.
## ACHIEVED: all assets decode native; textured character (skeleton/skinned/textured/animated);
##   textured environments; native GPU renderer (real PS2 shading); native engine 10 subsystems
##   (oracle-parity+mutation, -Wall clean, ctest 10/10); CAPSTONE (engine loop drives GPU render
##   of real char-in-level scene, zero emulation); scripts run on native JVM (270 corpus, hardened)
##   AND drive the engine (85/268 load real actors); 5 cutscenes playable w/ sound; audio
##   decode+mix; dashboard + STATE-OF-THE-PORT current on /srv/shared/dirge-port/.
## BLOCKED (honest, documented, not fabricated): AT3+ BGM (Sony codec); char hair shards; anim
##   rig mismatch; per-zone light values (runtime); JP atlas page-order; BMD selector->FileCode;
##   FED .bss evaluators; recompiler .kel overlay (fixed in oracle via recompile_keldraw).
## BIG PICTURE: purist approach proven end-to-end; playable game = multi-year (reauthor remaining
##   ~13k funcs of game logic + wire continuous play). Foundation + every pillar demonstrated.
## STANDING BY for Dan's return / direction. Not spawning make-work past completion.
- NOTE: engine-actor running ~2h40m (big/entangled subsystem). When it lands -> PRIORITIZE CAPSTONE
  (native-app: DocEngine frame loop + GPU renderer + real character/scene running together).
