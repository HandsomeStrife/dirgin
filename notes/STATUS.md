# Dirge of Cerberus — native PC port experiment

Target: SLES_541.85 (PAL, EU) — PS2 Emotion Engine ELF from
`/srv/games/downloads/dirge/Dirge of Cerberus ... .iso`

## Toolchain (installed, user-local, no sudo)
- JDK 21 (Temurin): /home/dan/tools/jdk-21.0.12.1+1
- Ghidra 12.1.4:    /home/dan/tools/ghidra_12.1.4_PUBLIC
- Env file:         ~/.config/dirge-port.env  (source it first)
- RE driver:        rea-agents (npx -y rea-agents@latest), drives Ghidra headless
- `rea doctor` → ghidra healthy once env sourced.

## The e_flags trick (why it's needed)
rea's Ghidra MIPS lane refuses the real ELF: e_flags=0x20924001 has
EF_MIPS_MACH_5900 (0x00920000, the R5900/EE marker) set, which its guard
rejects like "PSP Allegrex". Instruction bytes are standard MIPS, so we feed
rea a COPY with e_flags rewritten to plain MIPS32 (0x50000001):
  elf/SLES_541.85.mips32.elf   (original elf/SLES_541.85 untouched)
rea then pins MIPS:LE:32:default and analysis runs.
Caveat: default MIPS does NOT decode EE 128-bit MMI ops or VU/COP2; o32 ABI is
assumed (EABI64 in reality) so some function signatures are approximate.
Next upgrade: ghidra-emotionengine-reloaded processor extension.

## First-pass findings (rea analyze)
- 10,313 functions recovered in .text (3.4 MB of EE code). Decompiles to readable C.
- 741 strings only (most content lives in the encrypted KEL archives, not the ELF).
- Engine = Square's in-house "KEL". Notable sections:
  .vutext (VU1 microcode), ~20x .DVP.overlay.* (VIF/DMA packet + VU programs),
  .kel_net (207 KB netcode), .kel_sound_section, .kel_draw_section,
  .kel_extended_program_section (506 KB).
- Middleware/format fingerprints: libpng 1.2.3; "SQUARE Inet Library" (online);
  asset paths data/event/scene/ev%04d/... with .rfd (tex), scr%03d.class
  (a bytecode SCRIPT VM), .bin; encrypted bin/kel.enc + bin/kel.pex.enc; KEL.DAT.

## Walls (concrete, in rough order of nastiness)
1. VU1 microcode + GS rasterizer — not MIPS; all rendering geometry runs here.
   This is the same wall PS2Recomp hasn't cleared. Must be reimplemented/emulated.
2. Encrypted KEL archives (KEL.DAT / kel.enc / kel.pex.enc) — reverse to get assets.
3. The .class scene-script VM — must be reimplemented to run events.
4. 10,313 stripped functions → a coherent, correct, buildable C program.
   AI-assisted lift is tractable per-function; whole-program correctness is the grind.

## What's proven
The "lift EE CPU logic to readable C with AI assistance" path is OPEN and working
(step 1 demonstrated). A full playable native port still depends on solving the
VU+GS half (wall #1), which nobody has done generically for PS2.

## Reusable commands
    source ~/.config/dirge-port.env
    export REA_GHIDRA_LANGUAGE_ID="MIPS:LE:32:default"
    E=/srv/games/dirge-port/elf/SLES_541.85.mips32.elf
    npx -y rea-agents@latest analyze   "$E" --format json
    npx -y rea-agents@latest decompile "$E" 0x100000 --format md
    npx -y rea-agents@latest function  "$E" <addr|name> --format md

## UPDATE 2 — assets + EE module
- Installed EE processor module (chaoticgd/ghidra-emotionengine-reloaded v2.1.38,
  build for Ghidra 12.1.4) -> Ghidra auto-detects r5900:LE:32:default on the REAL
  unpacked ELF (no e_flags patch needed for Ghidra-direct).
  KNOWN SNAG: full auto-analysis crashes in analyzer "MIPS-R5900"
  (AddressOutOfBoundsException on 128-bit MMI reg constants). Disabling via
  prescript setAnalysisOption didn't stop it yet -> TODO: disable at import
  (-processor + options) or patch/skip that analyzer; per-function decompile works.
- KEL archive is a SOLVED format (community): tool WhiteBinTools -dirge, plus full
  spec at DoC-Research wiki (cloned to notes/doc-research-wiki/).
- Wrote our own dependency-free unpacker (tools/kel_unpack.py, tools/filelist_parse.py),
  validated against the real FILELIST.BIN:
    * 18,651 files in KEL.DAT, 59 zlib index chunks (all decompress clean).
    * 4,295 have virtual paths; ~14k are noPath (synth from FileCode).
    * extns: at2 2726, fed 391, at3 384, bin 368, rfd 183, pex/dvl/ovl, class, pss...
  End-to-end extraction verified: pulled PNGs (valid, e.g. res/jkt_001.png = cover
  art 256x256), text, .cnf, .class out of extract/KEL.DAT with 0 failures.
- extract/KEL.DAT (1.56GB) pulled from ISO. Full unpack not yet run (sample only).

## Open hard problems (unchanged, now sharper)
- VU1 microcode + GS rasterizer (rendering) — the real wall.
- .class script VM opcodes — undocumented.
- .pol/.pex 3D model formats — undocumented.
- kel.enc / kel.pex.enc executable-blob encryption — keys not public.

## UPDATE 3 — EE decompilation pipeline working + engine map
- ROOT CAUSE of earlier script failures: Ghidra 12 dropped Jython; .py scripts
  silently no-op ("Python not available"). REWROTE all scripts in Java:
  tools/DecompAt.java, DisableEEAddr.java, DumpFuncs.java.
- EE analysis COMPLETES and SAVES (the 2 MIPS-R5900 analyzer exceptions are
  non-fatal; Ghidra recovers). Persisted project: ghidra-proj/DOC (program
  "SLES_541.85"), language r5900:LE:32:default.
    * 13,089 functions (vs 10,313 under generic MIPS).
    * Decompiler uses real EE semantics: EI(), FlushCache(), 128-bit quadword ops.
- Boot chain mapped:
    entry(0x100000): clear bss -> heap init FUN_00451168 -> FlushCache -> main.
    main = FUN_00248528(argc,argv): detects cdrom boot ('c'), region/video-mode
      remap (switch), sets boot-mode DAT_0050b2d8, dispatches modes.
    FUN_00447c30 = memset (main calls it to clear 0x9684 bytes).
- Function inventory exported: analysis/funcs.csv (addr,name,size,callers).
    Hubs: FUN_002f93d8 (35KB, biggest - candidate state machine / interpreter),
          FUN_0032ace0 (21KB).
    Core utils (by caller count): FUN_002a8070 (1111 callers = likely malloc),
          FUN_0042d390 (935), FUN_00447c30 (memset, 360).
- Reusable: analyzeHeadless ghidra-proj DOC -process SLES_541.85 -noanalysis
    -scriptPath tools -postScript DecompAt.java 0x<addr>

## ASSETS — COMPLETE
- All 18,651 files extracted losslessly to extract/_KEL (2.1 GB, 0 failures):
  4,308 pathed + 14,343 keyed by FileCode (noPath/). Bug fixed: blank " " path
  field now -> FileCode name (was overwriting into a single file named " ").
- Nice names for noPath files available later via file.txt hash-map or
  DoCPathsGenerator (deferred; not needed for RE).

## NEXT
1. Trace main -> game main loop (find the per-frame update/render loop).
2. Identify the KEL file loader (FileCode -> load) in the EE code.
3. Locate/understand the .class script VM interpreter (big hub functions).
4. Still-open wall: VU1 + GS rendering reimplementation.

## UPDATE 4 — 5-agent engine map + first native code
- Full engine mapped by 5 parallel agents -> notes/engine-map.md (spine, file loader,
  script VM, rendering, asset formats). Headlines:
    * File loader: cleanly portable (pread + zlib). FileCode resolve FUN_0024af08,
      IOP via SIF RPC (no EE sceCd), IOPRP.IMG reboot.
    * Script .class VM = embedded JVM SUBSET (Java 1.2 classfiles, TTTT/DES, 202
      standard opcodes, int/float/ref only; long/double stubbed). Dispatcher
      FUN_003ad288, table VA 0x470b80. Reimplementable.
    * Rendering = the wall: GS rasterizer + VU1 microcode (stripped) must be
      reimplemented. BUT transport centralized (FUN_0030b670/768/868) + clean scene-
      graph seam (FUN_005a85f8) -> "capture DMA chain + reinterpret on PC GPU" is viable.
    * Game logic also in EE code overlays (.pex/.enc beyond main ELF).
    * Models = BMD/MOD in RFD containers; textures RST/TM2 in TXD; audio ATRAC3
      (at2/at3, headerless). RFD/TXD/text-bin solved; BMD/MOD/RST/at2 undocumented.
- FIRST NATIVE BRICK: port/ C reimplementation of the file loader (src/kel.c, builds
  with `make`, needs libz). Verified byte-identical (same sha256) to reference
  extraction on raw, zlib, and FileCode-addressed files. => BRIDGE #1 done in native code.
- Recommended next milestone: "headless boot" (loader[done] + minimal EE HLE + JVM
  subset -> run engine init + an init .class, rendering stubbed).
