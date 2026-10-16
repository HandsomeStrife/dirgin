# Dirge of Cerberus — native PC port: build plan

## Architecture decision: STATIC RECOMPILATION + hardware HLE
Chosen over full hand-rewrite. The EE executable (SLES_541.85, 13,089 funcs) is
statically recompiled MIPS->native C. The recompiled original code does ALL game
logic, asset parsing (BMD/MOD/RST/ATRAC source data), the embedded JVM, and .class
DES decryption BY ITSELF. We implement only the HARDWARE the game touches.

Consequence: JVM reimpl, BMD/MOD/RST decode, DES reversal, audio-format decode are
NOT on the critical path (the game does them). They become optional modding/debug
tooling. Critical path = make the EE run + emulate the hardware it hits.

## Target layout (port/)
  src/        shared runtime (kel.c loader = DONE, verified byte-exact)
  recomp/     EE static recompiler + generated C   [WS-R]
  hle/        kernel: memory, syscalls, threads/sema, DMAC, interrupts/vblank [WS-K]
  sif/        SIF/IOP HLE: file RPC -> kel_read; SPU2 audio; IOPRP = noop    [WS-K]
  gs/         Graphics Synthesizer rasterizer -> OpenGL/Vulkan               [WS-G]
  vu/         VU0 (macro, via recomp intrinsics) + VU1 microcode engine      [WS-V]
  input/      pad HLE -> keyboard/gamepad
Each workstream builds standalone first, integrates behind clean C interfaces.

## Workstreams
- WS-R  EE recompiler: evaluate existing (ran-j/PS2Recomp, snuri00 wasm) vs lift from
        our Ghidra DB; stand up prototype that emits compilable C for the EE ELF and
        runs trivial funcs. Defines the calling/memory contract everything else uses.
- WS-K  HLE kernel + SIF: 32MB EE RAM + scratchpad model; syscalls seen in map
        (FlushCache, EI, Create/Wait/SignalSema, thread ops, SetGsCrt, GsPutIMR);
        DMAC channels; vblank semaphore; SIF RPC: file read(func#1) -> kel_read,
        IOPRP reboot -> noop, fileio -> local fs. (We know exact call sites.)
- WS-G  GS/VIF/GIF backend: interpret the DMA display-list chains (transport
        FUN_0030b670/768/868; GIF kicks FUN_00442100) -> translate GS regs/prims +
        texture xfers (FUN_00441278/4415a0) to a modern GPU. Frame flip FUN_00440e68.
- WS-V  VU: extract .vutext + .DVP.overlay VU1 microcode from ELF, disassemble (EE
        Ghidra ext has VU support), decide interpret vs recompile; VU0 macro ops are
        handled by WS-R as intrinsics. VU1 is the long pole.
- WS-I  Input: pad buffers read by FUN_00254638 <- keyboard/gamepad.
- WS-AUD SPU2 audio HLE (under SIF/IOP): voice/ADPCM/ATRAC mixing. (game feeds it.)

## Integration contract (set by WS-R, provisional)
- Guest memory = one mmap'd 32MB block; EE addresses are offsets into it.
- Recompiled funcs: `void func_<addr>(ee_state*)` style; loads/stores go through
  mem helpers; syscalls/COP2(VU0) via intrinsic calls into hle/.
- MMIO writes (REG_GS_*, REG_DMAC_*, REG_VIF1_*) trap to hle/ handlers -> gs/ vu/.

## Milestones
M1 Headless boot: recompiled EE + HLE kernel + SIF(loader) runs boot path, parses
   FILELIST, reboots-IOP(noop), executes init far enough to request first assets.
   Rendering/VU/audio stubbed. PROVES CPU+kernel+IO.
M2 First pixels: GS backend shows the boot/flip output (clears, 2D, logo).
M3 VU1 online: geometry transforms -> 3D renders.
M4 Playable slice: input + audio + one scene/field running.

## Status
- DONE: assets extracted (18,651); file loader native+verified (port/src/kel.c);
  full engine map (notes/engine-map.md); EE Ghidra DB (ghidra-proj/DOC, 13,089 fns).
- NOW: WS-R (recompiler foundation), WS-V (VU extraction), WS-G (GS backend design).

## ====================================================================
## PIVOT (user decision): PURIST NATIVE PORT — zero emulation, zero translation
## ====================================================================
User set the bar to the Mario64/OoT-style from-scratch port: a native Windows game
built from reauthored source + a from-scratch renderer. NO cycle emulation, NO
command-translation layer. Honest: unprecedented for a stripped late-gen PS2 title;
multi-year; no guaranteed playable end. We pursue it with verifiable milestones.

### Status of prior (hybrid recomp+HLE) work -> becomes the REFERENCE ORACLE
The booting recompiled build (port/) is NOT the shipping target anymore, but is kept
as the ground-truth oracle: run it to observe real behavior (files loaded, VU inputs/
outputs, GS packets, state transitions) and validate the native reimplementation
against it. Keeps building. The native file loader (port/src/kel.c) carries over as-is
(native, fits purist). Engine map, Ghidra DB, VU disasm all reused.

### New architecture (native/ tree)
  native/assets/models/    BMD/MOD geometry -> native loader + glTF/OBJ export
  native/assets/textures/  RST/TM2 (+PS2 swizzle/CLUT) -> native decoder -> PNG
  native/assets/anim/       skeleton + animation (later)
  native/renderer/         from-scratch GPU renderer (Vulkan/OpenGL) of DoC scenes
  native/engine/           reauthored game logic (clean-source decomp, AI-assisted)
Method: the GAME'S OWN decompiled parser (analysis/decomp/) is the authoritative
format spec — reverse formats from the code that reads them, validated against the
oracle, not guessed from hex.

### Purist milestones
P1 Reverse model (BMD/MOD) + texture (RST/TM2) formats; native loaders; export a real
   model to glTF + textures to PNG and view it. [FIRST — tractable, visual, verifiable]
P2 Scene graph + materials + skeleton/animation reversed natively; VU1 transform/
   lighting understood as native math/shaders.
P3 NATIVE RENDER of one real DoC model w/ textures via modern GPU. Purist "hello world".
P4 Progressive clean-source decomp of game logic (reauthor Ghidra output); reimplement
   the JVM subset, audio (ATRAC3 native), input, timing natively.
P5 Wire renderer to the game's scene; a scene/field running as a native game.

### NOW: P1 — model format (BMD/MOD) + texture format (RST/TM2) reversal.
