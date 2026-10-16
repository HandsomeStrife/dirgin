# Build log — native port

## WS-G Graphics (GS/VIF/GIF)  — DONE (skeleton + design)
- port/gs/ builds clean (-Wall -Wextra), `make check` PASS on headless server.
- GIF/GS command interpreter works: parses GIFtag + A+D register writes + texture
  transfers end-to-end; hand-built textured-triangle packet (37 qwords) decodes to
  1 draw with correct 12.4->screen coords, RGBA/UV, TEX0/ALPHA, HOST->LOCAL transfer.
- gs.h interface: gs_submit_packet/giftag/dma_chain, gs_write_display, gs_do_transfer,
  gs_flip + pluggable gs_backend vtable (headless log stub). Design in port/gs/DESIGN.md.
- GS subset DoC uses (evidence-backed): draw-env FRAME/ZBUF/XYOFFSET/SCISSOR/TEST/
  COLCLAMP/DTHE (FUN_004408e0); 2D TEX0/TEX1/TEXA/ALPHA/RGBAQ/PRIM/UV/XYZ (FUN_004115b8);
  transfer BITBLTBUF/TRXPOS/TRXREG/TRXDIR/FINISH (FUN_00441278/4415a0/441720);
  PSM: CT32/24/16 + PSMT8/PSMT4 paletted(CLUT).
- HARD (can't fake): (1) 4MB VRAM overlap/aliasing — render-to-tex, fb-as-tex,
  needs overlap tracker + readback barriers; (2) PSM swizzle must be bit-exact.
  => likely need a SOFTWARE reference rasterizer as ground truth before GPU backend.
- Env gap: GL headers present but no GLFW/GLEW/SDL on server -> GPU backend not
  buildable here yet; interface is backend-agnostic so parsing half is proven headless.

## WS-V VU microcode — DONE (extracted + disassembled + interface)
- VU1 pipeline recovered STATICALLY. port/vu/ has: microcode/ (vutext.bin 43KB +
  4 reconstructed programs A/B/C/D), disasm/ (9.6k lines), tools/vudis.py (standalone
  VU1 micro disassembler — Ghidra EE ext only does VU0 macro, no VU1 micro).
- .DVP.overlay sections are ZERO-filled in ELF; real microcode is in .vutext tail;
  ovlytab describes 18 chunks -> 4 DVP programs (A 8xXGKICK main geom, B 6x, C
  transform-only/no kick, D 7x) loaded at micromem 0x0/0x800/...
- Pipeline confirmed (all 4 same shape): XTOP dbl-buffer input -> 2-matrix MULA/MADDA
  transform -> DIV Q perspective -> CLIP frustum -> OPMULA/OPMSUB cross(normal/backface)
  -> FTOI4 to GS 12.4 -> SQ assemble GIFtag/A+D -> XGKICK to GS -> [E] end.
- REC: build VU1 INTERPRETER first (oracle + runs runtime-uploaded MPG we can't see
  statically; model dual-issue, int pipe, 1-slot branch delay, EFU Q/P latency,
  XGKICK/XTOP). Then recompile the 4 resident programs to C/compute (program C first,
  no XGKICK). Cache recompiled by hash of uploaded micro bytes; interpreter fallback.
- vu1.h interface written: upload_micro/upload_data/set_top/run/set_xgkick_cb
  (XGKICK output = GIF packets -> handed to WS-G). 16KB micromem, 16KB VuMem1, 32vf/16vi.
- VU0 macro ops fully enumerated for WS-R (COP2 intrinsics): _lqc2(6483) _sqc2(3848)
  _vaddbc(2311) _vmul(1387) _vadd(944) _vopmula/_vopmsub(202) _vrsqrt(419) _vclip(404)
  _vdiv(276) _vftoi0/4 _vitof* _vwaitq(759) _vcallms(11)=VU0 microprog ... (full list in report).
- GAP: runtime-MPG microprograms unseen until we dump a live DMA chain.

## WS-R Recompiler — DONE (foundation + contract frozen + prototype verified)
- Approach: OWN a minimal R5900->C lift driven by our Ghidra boundaries (option c+b).
  Evaluated both existing tools with real build/run: ran-j/PS2Recomp builds after a
  1-line fix, INGESTS OUR ELF via its Ghidra-CSV path, 0 unhandled on test funcs =>
  kept as fallback/oracle; snuri00 web-recomp more capable but WASM/WebGPU-coupled =>
  too invasive to retarget. Owning the lift lets us fix the contract the other WS need.
- CONTRACT FROZEN: port/recomp/ee_runtime.h. Memory = one 32MB mmap, EE addr=offset,
  ee_canon masks kseg mirrors, scratchpad @0x70000000, MMIO 0x10000000-0x1FFFFFFF.
  Reg file ee_reg = 128-bit LE lane union (.ud/.uw/.uh/.ub). Func sig:
  void func_00ADDR(ee_state*); args from $a0-3/stack, ret $v0; direct calls ->
  func_TARGET(ee), indirect -> ee_lookup/ee_call_indirect + gen table. Loads/stores via
  ee_lbu..lq/ee_sb..sq (LE host, no swap; LWL/LWR merge). MMIO store top-nibble 1 ->
  ee_mmio_store{32,64,128} (addr carries reg identity: GS 0x12000000, DMAC 0x1000A000,
  VIF 0x10003800, GIF 0x10006000, INTC/SIF 0x1000F000). SYSCALL -> ee_syscall(ee,code).
  COP2/VU0 macro -> intrinsic calls ee_cop2_* on ee_vu0 (not inline SIMD). Delay slots
  resolved at recompile time (no runtime notion).
- Prototype (port/recomp/, make run = ALL PASS, VERIFIED by me): tools/recompile.py
  lifts real leaf funcs; FUN_00417940 (LEB128) + FUN_0036c338 (list walk, incl BNEL
  branch-likely nullify) match reference on all inputs; mutation-tested (0x7f->0xff
  goes red correctly); PS2Recomp gives byte-equiv semantics (cross-check).
- Limits: only integer+load/store+control subset done; MMI/FPU/COP2 -> ee_trap_unimpl.
- Boundary table analysis/ee_boundaries.csv (name,start,end,size, 13,089 rows) ready.

## FOUNDATION COMPLETE (R+V+G). Next wave: WS-R-scale + WS-K(kernel/SIF/input).
## WS-R-scale — DONE. Whole EE program translates.
- ALL 13,089 funcs translate, 0 ee_trap_unimpl across 509,473 instrs. 13,084 emitted
  into 44 TUs (~297 each) + function_table (5 SIF funcs routed to HLE via #define).
  Compiles -O2 -Wall -Wextra CLEAN. generated_all/ + unimpl_report.txt (0) + manifest.
- ISA closed: integer/ld-st/control + LQ/SQ/LWC1/SWC1/LQC2/SQC2/LDL..SDR/CACHE/MFSA..
  + MMI (34 helpers, MMI0-3/PMFHL/MULT1/DIV1, scalar-lane exact for simple ops;
  parallel mul/div best-effort vs PS2Recomp ref, NOT hw-verified) + COP1 FPU (EE
  single rules: no NaN/Inf, overflow->±max, denorm flush; 10,028 sites) + COP2/VU0
  macro (41 ee_cop2_* intrinsics, raw word -> WS-V decodes).
- Call graph: 31,701 direct func_<addr>(ee); 1,853 ee_call_indirect via binary-search
  function_table (13,089 rows). Fixed real SYSCALL/BREAK delay-slot bug (was dropping
  next instr); 52 shared-delay-slot cases handled.
- Diff-tested: memset FUN_00447c30 vs libc 4000/4000; FPU rules 20000 pairs; mutation-
  proven (break pcpyld -> memset 3744/4000 red; break FPU /0 -> red; restore clean).
- WS-K deltas folded in: (1) COP0 cop0_status/cause/count/epc + ee_cop0_mfc0/mtc0 +
  ee_ei/di/eret/sync (boot FUN_00241478 lowers correctly, Status 0x10000 reads clear);
  (2) syscall: always passes 20-bit code (0 for li $v1,n;syscall -> HLE uses $v1);
  (3) SIF funcs NOT emitted, func_<addr> #defined to sif_* ; (4) :652 warning fixed.
- Make: `make emit-all` (~3s), `make build-all` (per-file compile, fast verify),
  `make scale` (diff harness; -O2 whole link ~2m45s).
- CAVEAT: parallel mul/div MMI (pmaddh/pmulth/pmultw/pdiv*) not hw-verified (feeds
  fixed-point geometry -> cross-check before M3). COP0 MTC0/ERET/TLB stubbed (WS-K owns).

## === FOUNDATION + all M1 pieces COMPLETE (R, V, G, R-scale, K) ===

## M1 INTEGRATION — IT RUNS (partial boot; well-defined blocker)
- port/boot/ (main.c: mmap 32MB+SPR, load 5 PT_LOAD segs, argv cdrom0:\SLES_541.85;1,
  guest thread @0x100000, scheduler; env DIRGE_FRAMES/FAST/TRACE/WATCHDOG/KEL_ORIGIN;
  watchdog dumps guest func_ backtrace), runtime_hooks.c (cop0/ei/di/eret/sync/
  call_indirect), sif_glue.c (EE SIF hooks->device api), vu1_stub.c, Makefile.
- port/cop2/vu0_macro.c: ALL ~80 ee_cop2_* VU0-macro intrinsics implemented (FMAC/EFU/
  convert/clip, dest-mask/broadcast/fsf-ftf; CLIP exact; rare ops best-effort+logged).
- Integrated build does NOT link recomp/ee_runtime.c or ee_stubs.c (standalone only);
  hooks come from boot/ + hle/ + cop2/. No dup/missing symbols.
- RUN (cd port/boot && make && DIRGE_FAST=1 DIRGE_TRACE=1 ./dirge-m1):
  crt0 -> main FUN_00248528 -> SIF InitCmd/InitRpc -> boot-mode switch (via new
  jump-table support) -> CDVD/disc RPC binds+calls -> GS CRT init (a GS SPRITE
  framebuffer CLEAR actually decoded through DMAC->GIF->GS-log!) -> into critical init
  FUN_00244db0. COP2 math working, 15 ops exercised no crash (vadd/vsub/vmul/vmulq/
  vaddbc/vmulbc/vmaddbc/vmulabc/vmaddabc/vmr32/vdiv/vsqrt/vrsqrt/vwaitq).
- BUGS found+fixed (documented in-code): (1) sif name clash -> sif_dev_*; (2)
  SetupThread(0x3c) returned 0 as $sp -> whole program ran sp=0, now stack+size;
  (3) recompiler had NO jump-table analysis -> every switch jr -> ee_call_indirect
  trap; added conservative jump-table resolver (189 tables resolved; WS-R make scale
  still ALL PASS); (4) COP0 EIE bit modeled on ee_ei/di (was hardcoded clear);
  (5) scheduler no priority preempt -> boot worker handshake deadlock; added preempt;
  (6) vblank busy-poll INTC_STAT auto-arms; (7) sceSifBindRpc completed synchronously.
- BLOCKER: critical init binds/calls IOP RPC servers sid 0x80000001 (fno 255 init /
  19 disc-type = CDVD disc info), 0x80000592, 0x80000003 — UNHANDLED by M1 SIF HLE
  (only file-read func#1 + fileio func#5 done). No disc-info -> init diverges into
  terminal FUN_0024fbb8; null callback DAT_0046ba1c corrupted -> indirect-call abort.
  => IOP RPC emulation gap beyond file I/O. No real sif->kel file read happened yet
  (kel_origin still untested).
- NEXT: implement CDVD/disc-info IOP RPC servers in sif/ (disc present, PS2 DVD,
  PAL region) for sid 0x1 (fno 255/19), 0x592, 0x3 -> let FUN_00244db0 complete ->
  advance to FILELIST load (exercises kel_origin) + per-frame loop FUN_00248110.

## M1 CONTINUATION — oracle reaches the GAME LOOP (then PARKED per purist pivot)
- Added disc-info RPC (sid 0x592 = PS2 DVD present/region normal, sid 0x3 accepted);
  sid 0x1 custom IOP FS module given clean rejection (honest, not fake success).
- RESULT: per-frame loop FUN_00248110 IS REACHED and iterates (120 frames, 120 flips,
  scene-state 0->0x1d) with DIRGE_SOFT_INDIRECT=1 (neutralizes 2 garbage fn-ptr calls
  on service threads). Proves CPU+kernel+sched+MMIO/DMAC+GS-log through loop+state
  machine. Per-frame matrix math heavy & correct (COP2 17 ops: vmr32 14544,
  vmaddabc 11648, vmulabc/vmaddbc, vsub/vadd, vmulq/vrsqrt, vdiv/vsqrt/vwaitq...).
- REAL BLOCKER (diagnosed): this title does NOT use the generic sceCd path
  (sif_cd_read never called). ALL file/disc/stream IO goes through a CUSTOM IOP FS
  RPC module sid 0x1 (client 0x506440: open=0, read=0x13, stat=0xff, ~15 funcs),
  IOP DMAs structured results back. Unimplemented -> FS device-dispatch fn-ptr tables
  (~0x47ba28) never init -> service threads call garbage ptrs 0x41f32c/0x2511b8
  (mid-func jr ra from uninit tables). DAT_0046ba1c is 0 (not corruption). Async
  FILELIST load dispatches through 0x2511b8 (dest buf 0x800000) -> never reaches
  sif_cd_read, so kel_origin still untested (that path unused by this game).
- To FINISH hybrid M1 (if ever needed as a loading oracle): implement sid 0x1 custom
  IOP FS server in sif/ backed by src/kel.c (handle table + open/read/stat/close,
  DMA inflated bytes to dest EE buf). Sizable but scoped. PARKED — not pursued under
  purist pivot (hybrid only needs to run the loop, which it does, to serve as oracle).
- Hand-written sources compile -Wall -Wextra clean; recomp make scale still ALL PASS.

## === PURIST PIVOT: hybrid parked as oracle. Active work = native/ asset reversal (P1). ===
## P1 MODEL (BMD/MOD) — DONE (partial, real model exported)
- native/assets/models/: FORMAT.md (spec, cited fn addrs), doc_model.{h,c},
  export_obj.c, export_gltf.c, main.c, Makefile (C99, -Wall -Wextra clean).
- Reversed from game's parser: RFD container FUN_002585e0/2587e0 (header + offset
  table + {name8,ext4,size4}, payload @entry+0x10; ext-int dispatch: "bmd"0x646d62
  ->FUN_0026abe8, "mod"0x646f6d->FUN_005aecf8, +smd/skd/cid/rst). MOD header
  FUN_005aecf8 (ver@+4 >=16; top count@+0x10; entry-offset array@+0x14; 2-level
  group->submesh table indexed by runtime FUN_002e62b0/2e8830). BMD header
  FUN_0026abe8/26ba88/26bf88/26f518 (ver@+4>=4; 2 node arrays; 8-way bone/matrix
  tree @+0x20 with GIF/VIF draw templates).
- MOD geometry (9-strip variant) RECOVERED: tri-strip stream, 6-byte records
  [b0 b1 b2 b3 b4 idx], idx=(k+1)%count (loader only accepts matching idx col =>
  no fabrication). b0-2=int8 pos, b3-4=attrs(UV/nrm/col, provisional).
- VALIDATED: real file 0c064400.bin (MOD v19 km01vnw) -> 1 obj, 54 strips, 486 verts,
  378 tris, bbox (-128..127), UV[0..1], 96.8% non-degenerate, 9 material groups =
  matches game parser's +0x14 count. glTF well-formed. Exported out/evm000.{obj,gltf}.
- GAPS (FORMAT.md §6): (1) BMD vertex blocks = raw VIF/VU1 packets, unresolved;
  (2) MOD non-strip/v16 variant different layout; (3) int8->world DEQUANT + attr bytes
  live in VU1 microcode (have disasm in port/vu/disasm -> can resolve); (4) skin
  weights + material->texture binding not wired. Strip-length-9 from one family =>
  provisional until 2nd model + VU dump confirm.
- NEXT for geometry: cross-ref port/vu/disasm (VU1) to pin dequant + decode BMD
  vertex packets; or oracle VU-input dump for a known model.

## P1 TEX (RST/TM2) — DONE (verified with coherent images incl. readable text)
- native/assets/textures/: libdtex.a + dtex CLI. Core: txd.c (TXD/RFD unwrap), tim2.c,
  rst.c, swizzle.c (GS deswizzle+CLUT), decode.c, png.c (self-contained), Makefile.
  C11, dependency-free. RST-FORMAT.md documents layout + swizzle, cites fn addrs.
- TIM2/3/4: fully solved (standard PS2 TIM2 header; PSMT8 deswizzle, PSMT4 linear;
  CLUT-placement quirk: 256-col palette at start, 16-col at end). 
- RST (custom): reversed from parser FUN_0030a188 (descriptor: PSM@+0x12 w@+0x14 h@+0x16),
  FUN_002f55d0/002f25d0/00441278/0030ab10. Sig 0x0012FA38@+4(or+0x14). host->local
  BITBLTBUF => stored in GS transfer order. PSMT8 deswizzled via canonical 8-bit
  page/block/column interleave + CSM1 CLUT reorder + alpha x255/128. 98.8% decode.
- VALIDATED (PNGs in validation/, I VIEWED them): hm_box_l rusty crate (correct),
  z134_a_1 metal panels, m000_1 weapons atlas, shinra01 神羅 logo, menu8 UI atlas,
  cpr000 copyright TEXT READS CORRECTLY ("(C)2006 SQUARE ENIX... TETSUYA NOMURA") =
  decisive proof swizzle+CLUT correct.
- GAP: RST PSMT4 >=128px wide (~59% of RST) uses an uncracked pre-swizzle -> block-
  scrambled; page-geometry-driven (not a header flag), transfer-order sibling of the
  working PSMT8 case. Direct PSMCT* coded but unexercised (game is all paletted).

## P1 MODEL DEEPENED (VU1-driven) — positions + BMD resolved
- Position DEQUANT RESOLVED: ITOF4 = /16 (VU1 program_A idx49/51/52 ITOF4 feeding
  MULAx/MADDAy/MADDAz; program_C idx12-14). Texcoord stream uses ITOF12=/4096
  (program_A idx50/56/57) => position-only is /16. MOD km01vnw bbox now ±8 world units.
- MOD scan bug fixed (scan whole payload, not from first group dataOffset): km01vnw
  now 62 strips / 558 verts / 434 tris (was 54/486/378). Pos = int8 b0,b1,b2.
- BMD VERTEX DECODE RESOLVED (was 0): VIF UNPACK V4-32 = 4x float32 (x,y,z,w=1),
  world units, no dequant. Evidence FUN_0026bf88 seeds draw-template 0x6c080000
  (UNPACK V4-32 NUM=8, STCYCL 0x01000404). BMD node graph FUN_0026abe8->26ba88->
  26f518 leaf carries absolute EE ptrs (post-reloc in file). Exports: 06080112 ->
  6700 verts/5084 tris (6.2% degen, real mesh); 06010112(v4) -> 584 verts. Valid OBJ+glTF.
  => coherent split: BMD = static float world geom; MOD = int8 skinned (VU-dequant).
- MATERIAL->TEXTURE binding traced (FORMAT.md §6): FUN_002e62b0 group=(word>>10)&0x7f
  sub=word&0x1ff; group table @*(model+0x60); material sel *(u16)(group+8); layer tags
  matdesc+0x24 (FUN_002ebc48) index per-model texture-handle table @context+0x24
  (FUN_002ebb18); FUN_0030ab10 packs into GS TEX0. TBP0 is transient VRAM page (not
  FileCode); stable id = context+0x24 slot (slot->FileCode set at TXD load = oracle pt).
- UNRESOLVED (oracle items): MOD b3,b4 attr semantic (UV/normal/colour; packed by VU0
  vcallms 0x198, not in VU1 disasm); int16(V3-16) skinned-MOD variant; BMD prim/winding
  flags; MOD strip->group assignment; skin weights. => UVs are the main gap for texturing.

## P3 RENDERER — FIRST NATIVE RENDER (BMD model coherent). MILESTONE.
- native/renderer/: renderer.c (~450 LOC portable C99 software rasterizer, no deps)
  + Makefile + README (GPU Vulkan/D3D backend = later Windows milestone, validates
  vs this). Links doc_model.c + libdtex.a unmodified (reuses dtex_write_png).
- Pipeline: load model -> lookAt+perspective auto-frame from bbox sphere, orbit
  azim/elev; barycentric raster + z-buffer + perspective-correct UV; strip winding
  flip; two-sided flat N.L directional shading; nearest/bilinear tex sample w/ fallback
  to shaded; optional backface cull. CLI: renderer <model> [tex] [out.png]
  [--angle/--elev/--size/--mode textured|shaded|wire/--nearest/--cull]. -Wall clean.
- OUTPUT (native/renderer/out/, I VIEWED bmd shaded): BMD env 06080112 (6700v/5084t)
  renders as COHERENT 3D form (layered platform/floor env piece), correct perspective/
  depth/lighting; 2nd angle + wireframe + cull confirm genuine 3D. == P3 PROOF: real
  DoC model rendered by our own native code, zero emulation.
- MOD char 0c064400 renders SPIKY — NOT a renderer fault (same raster draws BMD
  perfectly); it's the loader's provisional MOD strip CONNECTIVITY (9-byte variant,
  strip->group, winding) + provisional UVs. Fixes land in doc_model.c, renderer
  unchanged. Textured MOD approximate (UVs provisional).

## P1 TEX COMPLETE — RST wide-PSMT4 CRACKED (textures 100%)
- dtex_unswizzle4 in swizzle.c: RST payload = linear source of a GS PSMCT32 host->local
  BITBLTBUF write, re-sampled as index PSM. Proof: composing GS CT32-write + PSMT8-read
  reproduces dtex_unswizzle8 bit-for-bit (0 mismatch, 128-512px) => model confirmed,
  PSMT4 is identical composition read as T4. Per-pixel: GS_PSMT4_read_nibble_addr ->
  word/k -> inverse_GS_PSMCT32_write -> src_nibble=8*i+k. CT32 region Wc=(h>=128)?w/2:h/2.
  Canonical PCSX2 columnTable4/blockTable4/columnTable32/blockTable32 (not invented).
- VALIDATED (viewed 0680ff03 stone wall = coherent, was scramble): all 15 wide-PSMT4
  dims decode; full library 5850 containers decode no crashes (3064 T4-deswz, 1775
  T8-deswz, 968 narrow-T4 linear, 11 CT32). Fails SAFE to linear on odd geometry
  (never fires on real data). No regression to T8/narrow paths.
- => TEXTURES COMPLETE: TIM2/3/4 + RST all PSM paths. ~100% of game textures decode.
- (agent reverted its build-log edit per dir-scope; recorded here by main.)

## === PURIST STATUS ===
## DONE & verified: archive unpack (18,651); textures 100% (native decoder, images
##   confirmed incl. readable text); file loader native; BMD geometry + first NATIVE
##   RENDER (coherent 3D). Engine fully mapped; EE recompiled (oracle) boots to game loop.
## MOD CHARACTER — bug fixed, bind-pose blocker precisely identified (still shards)
- BUG FIXED (doc_model.c scan_strips): MOD position = UNSIGNED (b0,b2,b4)/16, NOT
  signed (b0,b1,b2). Measured per-strip smoothness on 0c064400: b1~85,b3~74 jump ~1/3
  range/vert = NOT spatial (b1-as-Y drew the vertical spikes); b0,b2,b4 behave as
  coords (b2 cleanest, always [2,127]=height axis). Spikes gone; mesh still shards.
- Connectivity/winding CORRECT (BMD renders perfect, same tessellation; idx col exact).
- UVs NOT in record: real UVs = separate 16-bit ITOF12(/4096) texcoord stream (WS-V),
  not b3/b4. UV left 0.
- ROOT BLOCKER = BIND POSE (confirmed from parser FUN_005b1260/5b2240): MOD positions
  are in BONE-LOCAL space. Per-strip 8B control record {s16 boneIndex, u8 cntA@+2,
  u8 cntB@+3, s16 qwc@+4, s16 stride@+6}; loads ONE 0x40 (4x4) bone matrix at
  *(param_2+0x20)+boneIndex*0x40, then _lqc2x4 + vcallms(0x198) = VU0 micro rigidly
  transforms the strip by that bone. boneIndex IS on disk; BIND MATRICES ARE NOT —
  built at runtime from a separate SKELETON resource (FUN_005a7560/5a7390, FourCC
  0x646f6d) + animation. km01vnw payload has no matrix run.
- TO FINISH: (a) find+parse the skeleton resource for rest matrices + apply per-strip
  boneIndex; OR (b) oracle: dump *(param_2+0x20) bone palette + VU0@0x198 I/O from the
  booting recomp build, bake boneMatrix*pos per strip. Also disassemble VU0 micromem
  0x198 (same ISA as VU1 -> point vudis.py at it; locate VU0 micro bytes in ELF) =
  highest-value: resolves attributes + UV source + exact skin math at once.
## CHARACTER BIND-POSE — VU0 skin math RESOLVED, but bind matrices are RUNTIME-ONLY (blocked)
- VU0 skin micro @0x198 DISASSEMBLED (= the blob mis-filed as "VU1 program C";
  annotated port/vu/disasm/vu0_skin_micro.txt). Skin: world = BoneMatrix·
  ((localInt/4096)·meshScale, 1); 0x198 sets matrix (translation from col4, rows scaled
  by vf31), 0x120 per-vertex ITOF12(/4096)+matrix -> emits vf15=pos, vf16=normal.
  bone=control-record boneIndex, matrix=*(ctx+0x20)+boneIndex*0x40. CPU ref FUN_0026138c
  (4x4·vec4), weighted variant FUN_005b06d0.
- DEQUANT CORRECTION: skinned MOD position = ITOF12(/4096), not /16 (/16 is BMD/VU1
  visible-stream). UVs = SEPARATE stream merged into GS A+D later (NOT b1/b3 = packed
  normal/weight). MOD UV = oracle item; b1/b3 ruled out.
- BLOCKER: NO skd/smd skeleton file exists anywhere (2094 char RFDs = chrid.chi + mod
  parts only; FUN_005a7560/5a7390 resolve composite sub-mod parts, not a skeleton). Bone
  palette *(ctx+0x20) is per-instance, filled each frame FUN_005b2550->5b0d30 from
  model+0x24 (ABSOLUTE post-reloc ptrs, like BMD node graph). So char can't bind-pose
  statically; 62 bone-local ribbons pile into [0,16]^3 -> shards. Decode is sound; only
  the transform is missing.
- FORWARD: (a) rebase model+0x24 ptrs (if rest matrices present) OR (b) scoped oracle
  (relocate km01vnw in recomp runtime, call skin path, dump *(ctx+0x20)). Apply-math known.

## P4 ENGINE DECOMP — first brick (methodology + math + scene-graph) DONE
- native/engine/: DECOMP-METHOD.md (6-step Ghidra->clean-C pipeline w/ oracle-parity +
  mutation validation); math/ (dmath vec/mat4/quat + vu0.{h,c} = VU0 macro ops as clean
  dest-masked fns, ITOF4=/16); scene/scene_graph.{h,c} (reauthored FUN_005a85f8=
  doc_scene_object_set_draw_mode, FUN_002dbfc8=parts flag fan, FUN_002ebee8=recursive
  model-node tree walk; structs DocSceneObject/ModelParts/ModelNode/Mesh(stub)/
  RenderQueue w/ offset maps). CMake (engine + optional top-level native/), 2/2 ctest
  pass incl. oracle-parity diff + mutation-verified. -Wall -Wextra -pedantic clean.
- ORACLE BUG FOUND: port/cop2/vu0_macro.c ee_cop2_vopmsub subtracts the SAME products
  VOPMULA added -> opmula+opmsub = 0 instead of cross product (real VOPMSUB subtracts
  SWAPPED products). ~202 sites (normals/backface). vu0_opmsub is correct. BACKLOG: fix oracle.
  - FIXED 2026-10-15: ee_cop2_vopmsub in port/cop2/vu0_macro.c now subtracts the swapped
    products (fd.x=acc.x-fs.z*ft.y, fd.y=acc.y-fs.x*ft.z, fd.z=acc.z-fs.y*ft.x) = fs x ft,
    with read-before-write temporaries so fd may alias fs/ft. Numeric regression test
    port/cop2/test_vu0_cross.c (3 cases incl. in-place) PASSES on fix, FAILS (all 0) before.
    Build: gcc -Irecomp -o t port/cop2/test_vu0_cross.c port/cop2/vu0_macro.c -lm && ./t.
    Fix is in the shared macro intrinsic, so all ~202 call sites corrected at once.
- P4 NEXT SUBSYSTEMS (agent's ordered list): 1 scene-object list traversal (FUN_00408758
  + walkers 002ebdb8/2ebe38); 2 material->texture binding (FUN_002e62b0/2e8830/2ebc48/
  2ebb18/0030ab10 -> ties to native texture decoder); 3 per-frame state machine
  (FUN_00247508/241d38/241378); 4 skeleton/bind-pose+VU0 skin; 5 draw-queue flush seam
  (FUN_0030b768/b670/b868). Run ONE engine agent at a time (native/engine/ serial).
## LONG HAUL (P4/P5): clean-decomp 13k-fn engine -> native/engine; full GPU renderer;
##   JVM/audio/input/anim native; wire to a running game. Multi-year, unbounded.
## Launching: geometry-deepening (VU1 disasm -> dequant/BMD/attrs/material binding) +
## RST wide-PSMT4 swizzle. Then P3 offscreen renderer of a real textured model.

## WS-K Kernel + SIF + Input — DONE (standalone tests pass, mutation-verified)
- port/hle/ (kernel.c scheduler/sema/threads/INTC/vblank/ee_syscall; mmio.c DMAC +
  GS-priv/INTC/SIF dispatch; ee_mmio_regs.h), port/sif/ (RPC HLE), port/input/ (pad).
  Builds warning-clean vs frozen ee_runtime.h (not modified).
- Syscalls: enumerated from corpus (64 libkernel wrappers @0x451ae0-452310, named by
  Ghidra, e.g. CreateSema syscall(0x40)). Implemented FlushCache, Setup{Thread,Heap},
  En/DisIntc/Dmac, Add/Rem IntcHandler, Create/Delete/Start/ExitDelete/Terminate/
  ChangePrio/RotateReadyQueue/ReferThreadStatus/Sleep/Wakeup Thread, Create/Delete/
  Signal/Wait/Poll/ReferSema, SetGsCrt, Gs{Get,Put}IMR, SetVSyncFlag, Exit,
  _LoadExecPS2, SIF dma/reg set. Struct offsets grounded in game's own accesses.
- Threads: cooperative ucontext (guest thread can block mid-call in WaitSema);
  per-switch GPR/FPU snapshot; counting sema direct-handoff. Synthetic VBLANK drives
  INTC handlers -> iSignalSema releases loop's WaitSema on DAT_004b8ad0; paced 50Hz
  PAL (SetGsCrt mode3) / 59.94 NTSC; hz=0 = unpaced for logic tests.
- SIF file read: intercept CD RPC (server 0x494c90 func#1, arg {LBA,count,dst}@0x493bc0)
  -> sif_cd_read: byte_off=(LBA-kel_origin)*0x800, copy count*0x800 from KEL.DAT to
  guest dst. fileio RPC (server 0x509b80 func#5) -> kel_read (inflates) / host FS.
  IOPRP reboot -> noop+ready (SMFLAG 0x40000, SyncIop polls). SIF lib funcs HLE'd.
- DMAC: CHCR STR writes run sync then clear STR (busy-polls exit). GIF ch2 ->
  gs_submit_dma_chain/packet; VIF1 ch1 -> ring chain -> VIF1 sink (WS-V VU1 plugs in);
  GS-priv -> gs_write_display.
- Input: 32-byte libpad buffer (active-low btn word @+2), 2 ports, pad_fill_buffer
  injection; M1 default released. Live padman RPC id still to pin.
- TESTS pass+mutation-verified: (A) 2 coop threads ping-pong 2 sema; (B) vblank->
  SignalSema releases WaitSema 50x = 1.000s@50Hz; (C) CD-read bytes == kel.c (raw RPC
  argblock + inflate fileio); (D) real ee_sw to GIF CHCR kicks DMAC->GS parses A+D.
- GAPS: fileio func#5 full protocol (open/lseek/stat) best-effort (CD func#1 exact);
  VIF1 stops at sink (VU1=WS-V); pad stub; FIFO sched (prio stored not honored);
  not yet run vs real recompiled corpus (WS-R-scale pending).
- CONTRACT DELTAS NEEDED from WS-R (relayed): (1) COP0 Status+EI/DI/SYNC (boot path
  FUN_00241478 spins on Status&0x10000); (2) confirm ee_syscall num-from-$v1-when-code0;
  (3) route SIF lib funcs to sif/ not recomp; (4) fix :652 -Wmisleading-indentation.

## 2026-10-16 — VU0 cross-product fix re-review (rebuild + retest)
Re-review of the 2026-10-15 vopmsub fix (cop2/vu0_macro.c), per "re-review after fixing".
- Oracle rebuilt: obj/vu0_macro.o recompiled from the fixed .c (zero warnings), oracle-cli
  / oracle-bones / oracle-test relinked. `./oracle-test` => ALL SELF-TESTS PASS (memset
  FUN_00447c30, matvec FUN_0026138c) — no regression from the fix.
- Cross-product regression (cop2/test_vu0_cross.c) rebuilt vs current source => RESULT: PASS
  (all 3 cases incl. in-place fd==fs).
- vu0_macro.c compiles clean (-Wall -Wextra -std=c11) for both the oracle and boot include
  sets.
- Removed an unreferenced ORPHAN stale object cop2/vu0_macro.o (Oct-14, pre-fix) — no
  Makefile targets/links it (boot/ and oracle/ both compile cop2/vu0_macro.c into their own
  obj dirs), so it was the only lingering artifact carrying the old buggy cross product.
