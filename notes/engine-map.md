# Dirge of Cerberus — EE engine map (from decompiled corpus)

Corpus: analysis/decomp/<addr>_<name>.c (13,089 funcs). Project: ghidra-proj/DOC.
Anchors: entry=0x100000; main=FUN_00248528; memset=FUN_00447c30; malloc≈FUN_002a8070.

## 1. Boot → per-frame loop  (agent: spine)
- entry 0x100000 (crt0): bss clear -> heap FUN_00451168 -> FlushCache -> main.
- main = FUN_00248528: INIT then calls the loop once, then may _LoadExecPS2 the
  next overlay ELF (engine is split across chained ELFs; overlay at 0x00aa0000).
  INIT highlights: FUN_00414978 (sys init); memset state block @0x50ed00 (0x9684 B);
  FUN_00242650 parse argv[0]/boot path; disc/region/boot-mode detect
  (FUN_00242450/2424a0/242440 + FUN_00447108); FUN_00244db0 critical (fatal if <0).
- PER-FRAME LOOP = FUN_00248110 (while(1)):
    FUN_0042d228(0x4bcda8)  frame mutex LOCK
    FUN_0030b768(0)         RENDER open: begin VIF1 DMA display list  <-- handoff
    FUN_002548d8->FUN_00254638  INPUT poll (2 pads; SDK pad syms stripped)
    [subsystem updates FUN_00257670..0026a0c0]
    FUN_00247508            STATE DISPATCH (switch on scene state)
    FUN_002413f0()          optional per-frame hook (ptr DAT_0046b1d4; setter unresolved)
    FUN_0030b868(0)         RENDER kick: close list, start VIF1, wait DMA  <-- handoff
    FUN_0042d390(0x4bcda8)  frame mutex UNLOCK
    FUN_0040ce28(0x4fc7d0,0) VBLANK wait (FUN_00241ad0 -> WaitSema on DAT_004b8ad0)
                             + framebuffer FLIP (FUN_0040d228 writes PCRTC PMODE/DISPFB/DISPLAY)
    FUN_00241a20(0)         advance frame
- Frame pacing = vblank-semaphore (no VSync() syscall). Flip = PCRTC regs.
- STATE MACHINE:
  (a) scene state int DAT_004b8b2c (prev DAT_004b8b30); set FUN_00241d38(n) (n<0 = pop);
      get FUN_00241d68; dispatched by FUN_00247508 (states 1,2,3,4,5,9,0x1d:
      boot/title/load/field...).
  (b) mode-flag bitmask DAT_004b8980; set FUN_00241378(bit,on); test FUN_002413c0(bit)
      (bit9 diagnostic loop, 0x1a loop-keepalive...).
- Render handoff for rendering agent: FUN_0030b768 (open, VIF1 path arg0 / GIF arg1)
  and FUN_0030b868 (kick+wait). Draw calls in between fill the GS packet buffer.

## 2. File loader / IO            (agent: loader) — FULLY MAPPED, portable
- Public API: open-by-FileCode FUN_0024fe88, open-by-name FUN_0024fef0 (e.g.
  "filelist.bin"). Returns 0x250-byte handle (pool of 32 @ DAT_004c2460). ASYNC:
  submit cmd via ring (FUN_0024f298) -> file-server thread FUN_00249900 ->
  worker FUN_0024e178; caller polls handle+0x23a (3=done).
- FileCode->offset: FUN_0024af08 binary-searches parsed FILELIST table DAT_0046b970
  (sorted {FileCode,chunkIdx}); inflates owning index chunk, parses documented
  pos:usize:csize:path. If code absent -> synthesise path from FileCode bitfields
  FUN_0024a828 (templates "data/chr/%c/%d/%d/%d/%s", "texture.txd", etc).
- FILELIST load: FUN_0024fef0("filelist.bin") -> FUN_0024a6d0 relocates 2 self-rel
  header ptrs. Beta: "flstbeta.bin".
- Device read: worker case3=read, case5=read+inflate. FUN_0024de38 holds 0x800
  sector math (bytes>>11 whole sectors + 1 bounce sector for tail); base LBA =
  file_sector(handle+0x244) + KEL origin DAT_004df920. On HW = SIF RPC to IOP
  (FUN_00446af0 -> sceSifCallRpc FUN_00453d20); NO EE-side sceCd. Device type 4 =
  SIF fileio (cdrom0:/pfs1:/host0:) FUN_00457c90.
- Decompression: FUN_0042b810 = stock zlib inflate (error strings confirm);
  inflateInit FUN_002498a8, inflateEnd FUN_0042b5c0. Index chunks via FUN_0024a6f8;
  archive files streamed in 0x80000 windows via FUN_0024cdc8.
- IOP: no per-.irx SifLoadModule; engine reboots IOP from IOPRP.IMG ("rom0:UDNL ...",
  sceSifRebootIop FUN_004517f0 / SyncIop FUN_00451958). All IO modules inside that img.
- PC-PORT BRIDGE (recipe): parse FILELIST -> resolve FileCode (or synth path) ->
  pread(KEL.DAT, dst, csize, (KEL_origin+base_sector)*0x800) -> if csize!=usize zlib
  inflate. This subsystem is cleanly replaceable (matches our tools/kel_unpack.py).
## 3. Script .class VM            (agent: vm) — it's an embedded JVM subset!
- .class files = Java 1.2 classfiles (major 0x2e) with magic "TTTT" (DES-encrypted
  body) that decrypts in place to "SSSS". Standard Java constant-pool tags (1..12,
  Long/Double take 2 slots). Big-endian readers.
- Encryption: FUN_003a8320 decrypts bytes [8..EOF]; DES-family 8-byte block cipher
  (key schedule thunk_FUN_0041bb38, block FUN_0041c0d8, wrapper FUN_0041c1d8),
  KEY = file's first 8 bytes (magic+version 54 54 54 54 00 00 2e 00). GAP: exact
  DES variant/S-boxes not fully reversed.
- Interpreter: dispatcher FUN_003ad288 (opcode=*PC, 256-entry handler table @ VA
  0x00470b80 / file off 0x370c80, 202 populated = JVM ops 0x00-0xca). Loop driver
  FUN_003a99f0; call-in FUN_003a9b88; native-vs-bytecode split FUN_003a9c58
  (method+0x0c = native ptr). invokedynamic(186) empty => pre-Java-7.
- Subset: int/float/reference ops are REAL; ALL long ops -> stub 0x003aa048, ALL
  double ops -> stub 0x003aa050 (scripts avoid 64-bit types).
- Frame struct: +0xf010 locals base (8-byte tagged slots {u16 tag;u32 val@+4}),
  +0xf014 operand stack (grows down), +0xf018 call depth, copying GC FUN_003a9140
  (semispace toggle +0xf020), yield flag +0xf024 (per-frame cooperative yield).
- Class loader FUN_003a7700 (32 slots @ DAT_004ef498).
- GAP: native-method registry (bytecode->engine bridges: play-sound/load-model...).
  High-level event/scene command handlers likely reached via natives:
  FUN_005661e8 (145-case, cmds 0x12c+), FUN_00545b20 (137-case).
- PORT: reimplement a small JVM (classfile parser + 2-stack tagged frames + copying
  GC + 202 opcode handlers). Well-understood; far better than a bespoke VM.
## 4. Rendering VU1/VIF/GS        (agent: render) — the wall, but a strategy exists
- libgraph cluster 0x440000-0x443600 (sceGs* inlined; VU1 microcode stripped).
  Frame flip = FUN_00440e68 (writes GS PMODE/DISPFB/DISPLAY via FUN_00441f78 +
  sends draw-env GIF packet FUN_00442100). CRT init FUN_00440468. Drawsync
  FUN_00440f60 (36 callers; spins VIF1/GIF CHCR, VIF1_STAT, VU1 via _cfc2, GIF_STAT).
- Transport (centralized!): open chain FUN_0030b768 (VIF1 ring @0x700000, CHCR=0x105;
  GIF variant 0x104); append-to-ring FUN_0030b670 (73 callers, workhorse); close
  FUN_0030b868; single GIF kick FUN_00442100. Thin API FUN_002e14b8/14a8;
  queue select FUN_002f0878 (by DMAtag id).
- Packets hand-assembled qword-by-qword inline in each renderer (e.g. FUN_0040e568:
  DMAtags/GIFtags/GS A+D). No generic tag builder.
- VU1: uploaded as MPG VIFcode packets inside the DMA chain (data-driven, stripped,
  NO C to port). VU0 macro-mode SIMD inline in 819 files (_lqc2/_vmul/_qmtc2...).
- Textures: build xfer FUN_00441278 (BITBLTBUF/TRXPOS/TRXREG/TRXDIR, PSM switch);
  send FUN_004415a0 (MEM->GS); readback FUN_00441720 (=sceGsExecStoreImage).
- Scene graph ABOVE the seam (portable C, no hardware): object list FUN_00408758 ->
  per-object draw FUN_005a85f8 (54 callers = cleanest "draw model" chokepoint) ->
  FUN_002dbfc8 (8 parts) -> FUN_002ebee8 (model-node tree, render-queue flagging
  into contexts 0x4ee2e0 family).
- BELOW the seam (must reimplement): inline VIF/GIF/DMAtag emission + VU0 SIMD +
  VU1 microcode + GS rasterizer. Woven through, not behind one API.
- STRATEGY: keep logic+scene-graph, intercept at FUN_005a85f8/render-queue; replace
  everything queue-flush-down. Transport is centralized (few funcs), so
  "capture the DMA chain and REINTERPRET it on a PC GPU" is viable and less risky
  than hand-retranslating every inline packet builder. VU1 recovery = the long pole.

## VERDICT (after full 5-agent map)
Three pillars: FILES, BRAIN, EYES.
- FILES: solved. 18,651 extracted; loader is portable (pread+zlib, matches our tool).
- BRAIN: tractable. 13,089 funcs decompile cleanly; boot->loop->state machine mapped;
  scripting is a JVM SUBSET (reimplementable, known tech). Caveats: game logic also
  lives in EE code overlays (.pex/.enc, some encrypted) beyond the main ELF; DES key
  for .class + JVM native-method registry still to reverse.
- EYES: still the wall. GS rasterizer + VU1 microcode must be reimplemented; VU1 is
  stripped (recover from binary). BUT transport is centralized and there's a clean
  scene-graph seam (FUN_005a85f8), so DMA-chain-capture-and-reinterpret is a real plan.
Net: more of the engine is cleanly portable than first feared. A HEADLESS vertical
slice (loader + JVM + HLE boot, rendering stubbed) is genuinely reachable and is the
recommended next milestone to prove the approach before tackling VU1/GS.

## RECOMMENDED NEXT MILESTONE
"Headless boot": C/C++ harness that reimplements the file loader (have the recipe),
a minimal PS2 EE HLE (syscalls, threads/sema, memory), and the JVM subset, then runs
the engine's init path far enough to parse FILELIST and execute an init .class script
with rendering/VU/GS stubbed. Proves BRAIN+FILES+VM end-to-end without the wall.

## 5. Asset formats               (agent: assets)
Split: 3,940 pathed + 14,343 noPath (named by FileCode). noPath are mostly CONTAINERS.
SOLVED / standard: RFD/TXD container unwrap (wiki, bytes verified); text-bin decrypt
  (magic 45 90 AF F9, DoCCryptTool; 1739 files = all dialogue/UI); TM2/TIM3/TIM4
  textures (standard PS2); .irx ELF; png/ico.
UNDOCUMENTED (reverse needed), priority order for a port:
  1. RST textures (inside TXD) — GS-swizzled raster; cross-ref the TM2 in same containers.
  2. BMD (magic BMD\0, in model.rfd, 900+) and MOD (MOD\0, in evm###.rfd, char/event)
     = the two 3D MODEL formats. All visuals depend on these.
  3. .at2 (2726) + .at3 (384) audio = headerless ATRAC3 family (voice+BGM); need
     channel/rate/blocksize to decode via vgmstream. No header, no existing tool.
  4. CID collision (inside mapid.rfd) — needed for movement/physics.
  5. .class VM bytecode (270, magic TTTT) is ENCRYPTED/high-entropy — likely needs
     polkey.dat (64KB key blob). For a reimpl port we may re-author logic instead.
  6. FED/FEZ effects, .wd sound banks — polish, defer. IPU .pss movie — cosmetic.
IMPORTANT: .pex/.ovl/.dvl/.enc in bin/ are EE CODE OVERLAYS (per-boss/zone/debug
  game logic), NOT assets. => significant game logic lives OUTSIDE the main SLES ELF,
  loaded as overlays. Full decompilation will need these too (some .enc are encrypted).
.pol is a red herring: memown.pol = memory-ownership table, not geometry.
