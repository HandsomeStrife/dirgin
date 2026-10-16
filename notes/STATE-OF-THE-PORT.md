# STATE OF THE PORT — Dirge of Cerberus: FFVII (purist native PC port)

*Consolidated technical summary — 2026-10-15. Deep-dive companion to the visual
dashboard at `/srv/shared/dirge-port/index.html`.*

This document consolidates the full build record (`notes/build-log.md`,
`notes/autonomy-journal.md`, `notes/PLAN.md`, `notes/engine-map.md`,
`native/verify/MANIFEST.md` + `VERIFY_REPORT.md`, and the per-subsystem
`FORMAT.md`/docs under `native/*` and `port/*`). Numbers are pulled from those
sources; anything uncertain is flagged. Nothing outside this file was modified.

---

## 1. Executive summary (honest)

**What this is.** A *purist* native PC port of the PS2 game *Dirge of Cerberus:
FFVII* (PAL/EU, `SLES_541.85`): a from-scratch, native reimplementation in the
Mario 64 / OoT decompilation tradition — **zero emulation, zero
command-translation**. No cycle-accurate CPU, no GS/VU command interpreter at
runtime. The game's own decompiled parsers are used as the *authoritative
format spec*, reversed into clean native loaders, renderers and engine modules,
and validated against a reference oracle.

**What is PROVEN end-to-end** (each item verified by viewing output or running a
test, not asserted):

- **Assets decode natively.** The 18,651-file archive unpacks losslessly; a
  dependency-free native texture decoder produces coherent images *including
  readable in-game copyright text* ("(C)2006 SQUARE ENIX… TETSUYA NOMURA"), which
  is decisive proof the GS swizzle + CLUT logic is correct. Collision, effects,
  audio, movie, script and text formats all decode from native code.
- **A textured character renders on a real GPU.** A real DoC character mesh
  (skin / trousers / boots / sheathed gun) was skinned onto recovered bind-pose
  bones and drawn by our own OpenGL renderer on actual AMD hardware (not a
  software fallback).
- **A textured level renders on a real GPU.** A real DoC environment (Shinra
  sci-fi interior: metal panels, blue holo-consoles, floors) renders textured,
  and a character composites into it in one depth-sorted GPU pass.
- **Scripts run on a native JVM — and now drive the native engine.** The embedded
  Java-subset VM was reauthored in C and runs real decrypted game scripts to
  completion; its bridge layer routes `KerberosActor`/`Camera`/`Light`/`Zone` to
  the real native subsystems (`DocActor`/`DocCamera`/`DocLightSet`/`EngineState`),
  and 85/268 real scripts load real DocActors (mutation-proven load-bearing).
- **The native engine frame-loop runs integrated.** A standalone native app boots
  the reauthored `DocEngine`, runs the real per-frame loop for 120 frames, and
  drives the real scene-state machine BOOT→TITLE→ATTRACT→FIELD_RUN with camera,
  input, scene-graph walk and material resolution — zero emulation.
- **Cutscenes decode and play.** FMV archives were decrypted, demuxed and
  remuxed into a playable, A/V-synced clip.

**Big-picture honesty.** The *approach is validated* on every pillar — files,
assets, rendering, scripting, engine structure. But a **fully playable game is
far off**. What exists is a broad set of proven *vertical slices* and reusable
tooling, plus 10 reauthored engine subsystems, not an integrated game. The
central remaining work — a clean-source decomp of a ~13,000-function engine, a
full GPU scene renderer wired to real scene/placement data, native animation on
the gameplay rig, and audio/BGM — is a **multi-year, unbounded effort**. Several
hard items are honestly blocked (see §6). This is a strong foundation and a
credible plan, not a near-complete port.

---

## 2. Architecture

### 2.1 The pivot: hybrid → purist
The project began as a **static-recompilation + hardware-HLE hybrid** (recompile
the EE MIPS to C, HLE the PS2 hardware). That build *works* and boots to the game
loop — but the owner set the bar to a **purist from-scratch port** (no emulation,
no translation layer). The hybrid was therefore **parked and repurposed as the
reference oracle**; active work is the native reauthoring under `native/`.

### 2.2 The decomp-to-native pipeline
1. **Ghidra r5900 DB** (`ghidra-proj/DOC`) — the real EE ELF analysed with the
   Emotion Engine processor extension → **13,089 functions**, decompiled to
   readable C in `analysis/decomp/<addr>_<name>.c`.
2. **Format-from-parser** — every asset format is reversed *from the game's own
   code that reads it* (cited function addresses in each `FORMAT.md`), never
   guessed from hex.
3. **Reauthor to clean native C** — `native/engine/DECOMP-METHOD.md` defines a
   6-step Ghidra→clean-C pipeline; each module is validated by **oracle-parity
   diff + mutation testing** under ctest.
4. **The oracle** (`port/`) — the parked hybrid, kept building, runs real
   recompiled functions to observe ground-truth behaviour (memory, VU I/O, state
   transitions) and validate the native reimplementation.

### 2.3 Tree layout
```
analysis/decomp/     13,089 decompiled EE functions (the spec source)
ghidra-proj/DOC      Ghidra r5900 project
extract/_KEL         18,651 extracted assets (2.1 GB)
port/                THE ORACLE (hybrid recomp+HLE) + kel.c loader + VU disasm
  recomp/  hle/  sif/  gs/  vu/  cop2/  input/  boot/  oracle/
native/              THE PURIST PORT (active work)
  assets/            native loaders: models textures audio collision effects
  renderer/          from-scratch software rasterizer (portable C99)
  gpu/               OpenGL 3.3-core GPU renderer (EGL; Windows-portable)
  engine/            reauthored game logic (math/scene/core, ctest+oracle-parity)
  script/            native JVM subset + class decrypt/disasm
  text/  movie/  gallery/  app/  verify/  dashboard/
notes/               build-log, autonomy-journal, PLAN, engine-map, this file
```

---

## 3. Status by pillar

Three original pillars (**FILES / BRAIN / EYES**) plus the asset/content work
that the purist pivot made central (**AUDIO / SCRIPTS / CONTENT**).

### FILES — SOLVED
- **DONE:** All **18,651** files in `KEL.DAT` extracted losslessly to
  `extract/_KEL` (2.1 GB, 0 failures): **4,308 pathed + 14,343 noPath** (keyed by
  FileCode). **59 zlib index chunks**, all decompress clean. Native loader
  `port/src/kel.c` verified **byte-identical (sha256)** to the reference
  extraction on raw, zlib and FileCode-addressed files. Loader logic fully mapped
  (FileCode→offset binary search, zlib inflate, async ring, SIF RPC path).
- **Lives in:** `port/src/kel.c` + `kel_tool`; `tools/kel_unpack.py`,
  `filelist_parse.py`; map in `notes/engine-map.md §2`.
- **Note:** the kel loader is at `port/`, **not** `native/src/` (there is no
  `native/src/`). It is native C and fits the purist bar, so it carries over as-is.

### BRAIN — mapped; reauthoring in progress (the long pole)
- **DONE (mapping):** **13,089** EE functions recovered and decompiled. Boot
  chain, per-frame loop (`FUN_00248110`), and scene-state machine fully mapped
  (`engine-map.md §1`). Recompiler (oracle side) translates **all 13,089
  functions, 0 `ee_trap_unimpl` across 509,473 instructions**, into 44 TUs,
  compiling `-O2 -Wall -Wextra` clean.
- **DONE (reauthoring):** 10 engine subsystems reauthored to clean native C with
  oracle-parity + mutation tests passing (see §4) — the actor subsystem landed,
  so `native/engine` builds `-Wall -Wextra` with 0 warnings and ctest is **10/10
  PASS including `test_actor`**.
- **PARTIAL / BLOCKED:**
  - The oracle recompiler originally emitted **3,322 of 13,089 functions as empty
    stubs** (it only translated the first LOAD segment; the *entire MOD
    skin/skeleton subsystem* in `.kel_draw_section` was blank). **Fixed in the
    oracle** via `tools/recompile_keldraw.py` (overlays re-translated, 0 unimpl);
    **BACKLOG: fold the segment-aware fix into the main recompiler.** The earlier
    "all 13,089 translate" claim was ~25% stubs until this was caught.
  - **Oracle bug (FIXED 2026-10-15):** `port/cop2/vu0_macro.c` `ee_cop2_vopmsub`
    subtracted the same products VOPMULA added (so cross-product = 0) at ~202 sites
    (normals/backface). Now subtracts the swapped products (`fd = fs x ft`), alias-safe
    (read-before-write temporaries). Numeric regression test `port/cop2/test_vu0_cross.c`
    (3 cases incl. in-place) passes on the fix, fails (all 0) before. Oracle now agrees
    with the always-correct native `vu0_opmsub`.
  - Parallel MMI mul/div and COP0 MTC0/ERET/TLB are best-effort / stubbed in the
    oracle (not hw-verified).
  - Game logic also lives in **EE code overlays** (`.pex/.ovl/.dvl/.enc`, some
    encrypted) *outside* the main ELF — not yet decompiled.
- **Lives in:** `analysis/decomp/`, `ghidra-proj/DOC`, `port/recomp/`,
  `native/engine/`.

### EYES — validated vertical slices; full renderer far off
- **DONE:** Two from-scratch renderers, cross-validated to **near pixel-identity**
  (software vs GPU mean-abs-diff 0.001/255):
  - `native/renderer/` — portable C99 software rasterizer (barycentric + z-buffer
    + perspective-correct UV, directional N·L shading), no deps.
  - `native/gpu/` — **OpenGL 3.3-core** via EGL surfaceless on real **AMD gfx1103
    / Mesa (GL 4.6)**, VBO/VAO/GLSL330/FBO→PNG. Windows-portable (only the EGL
    context file is platform-specific; add a WGL file for Win). Purist shader
    model from `port/vu/RENDER-MATH.md`: 3 directional lights + ambient ⊗ vertex
    colour for characters (VU1 program D), prelit + fog for static world (A/B), GS
    TFX MODULATE/DECAL/alpha-test.
  - Proven renders: coherent BMD environment; clean textured character; **textured
    character inside a textured level**, one depth-sorted GPU pass.
- **PARTIAL / BLOCKED:**
  - **Char hair shards** (~22%): thin strand-card tris collide on the 245-slot
    VU0 scatter-index pool; body is a solid continuous textured surface (5.8%
    residual shards).
  - **Light / ambient / fog constants are runtime-only.** Oracle *verified the
    upload path executably* (VU1 `0x6c0b00f0`, 11 qw Mvp+L+C+A), but the *values*
    are per-scene, script-set, with no shipped light file and no baked env default.
    A placeholder rig (grey 0.4 / ambient 0.3, the engine's own default) stands in.
  - **Scene placement is hand-set.** No real scene-placement data is wired yet;
    character scale/position in composed scenes are tuned by hand, and one env
    display-gain (colscale 1.8) is a display tweak.
  - **BMD env texture→submesh binding — GROUND-TRUTHED (partial) 2026-10-16.**
    The giftag field once read as a per-submesh "material selector" is the VERTEX
    COUNT (1273/1273); the per-submesh giftag qword0 is a FIXED A+D register
    template (NLOOP 7/0, `0x303ec000…`), carrying NO per-submesh TEX0 — so the
    binding is NOT in the geometry packet. It is the runtime group table
    (`FUN_002e62b0`: `M+0x60[group]`, material selector @`group+8` → `M+0x64[sel]`
    matdesc → layer tags → `context+0x24` slot), already reauthored in
    `engine/scene/material.c` (test_material PASS) but not yet fed by a file loader.
    **Clean ground truth obtained** from a US (SLUS-21419) GS dump of z143 Mako
    Reactor 0 (clean room, Vincent only): 18 room textures bound TBP0→FileCode at
    corr≈1.0 (`out/z143_mako_dump_binding_oracle.json`); US dump textures = EU
    on-disk extracts at corr 1.0 (assets region-identical). **UV-triple identity
    match** (decoder UV=raw/4096 ≡ dump ST/Q, FST=0) is STABLE over Q 256..4096 and
    binds **8020/15332 packets across all 13 `*12` room-geometry chunks to real
    FileCodes, 18 textures, vote-conflicts ≤4** (`out/z143_room_uvtriple_binding.json`,
    per-packet arrays in `out/z143_pktfc/`). Recall 52.3% whole-room (up from 35.2%
    single-frame) by UNIONing camera angles; new angles use different TBP0 VRAM
    pages, bootstrapped decode-free via UV-triple agreement (grow_fingerprint.py).
    Main shells 068f0112=89%, 068f0212=69%; 52.9% of all tris textured.
    **GS-DUMP METHOD SATURATED (2026-10-16): 18 angles give the same 52.3% as 7**
    (adding 11 fresh angles moved recall +2 packets while the fingerprint doubled
    5098→11251 triples — every new surface fell on an already-covered packet). The
    residual ~48% is geometry NO in-room camera rasterises (back-faces / structural /
    far-LOD): chunks `068f0712` (96% dark) and `068f0d12` (97% dark) stay unreachable
    across all 18 angles. More GS angles cannot recover it. This method needs a dump
    per room AND tops out at camera-visible surfaces.
    **BACK-FACE FILL VALIDATED + SHIPPED (2026-10-16):** the never-drawn ~48% is
    textured for the enhanced free-camera by centroid 1-NN inheriting the nearest
    bound packet's material — **75.2% ± 0.5% exact-match (5-seed 20% hold-out) vs
    26.3% baseline, ~98% coverage** (`native/assets/models/backface_heuristic.py`
    → `out/z143_backface/assignments.json`, per-packet fc tagged `source=oracle|
    heuristic` + `confident` @200u). Labelled cosmetic fallback ONLY — NEVER folded
    into the authoritative 52.3%. k>1 and normal-gating both hurt; simplest wins.
    REMAINING GAP (scalable, all-rooms, visibility-independent fix): recover the full
    runtime material model `M`. **STATIC RECOVERY DISPROVEN 2026-10-16** (two deep
    decomp+data investigations): the binding is genuinely runtime-constructed and NOT
    present in the `_KEL/noPath` extracts. Why: (1) the relocation *mechanism* is
    payload-relative (M = FED blob used in place, FUN_002e5550 only rebases) — BUT the
    `068f0N14` FED's material selectors are all 0 (= draw-skip), so FED is scene/LOD, not
    the texture carrier; (2) `068f0N11` `mapid`/`cid` = COLLISION data (FUN_00271208,
    FORMAT.md §collision), ~3386 verts, not materials; (3) the `068f0N12` geometry VIF
    stream is pure UNPACK with ZERO TEX0/GIF writes — sets no texture; (4) the BMD group
    table's selector field is a uniform relocated STUB (`0x0022de50`) — these extracts
    are POST-RELOCATION MEMORY IMAGES whose per-submesh selectors + `context+0x24`
    texture-handle table are runtime-built and left as defaults. The real bind is keyed
    by a per-draw command word (`param_3`: group=`>>10&0x7f`, sub=`&0x1ff`) consumed by
    FUN_002e62b0, with selector=`u16(group+8)` → `context+0x24[sel]` handle (built at TXD
    load; oracle shows handles upload in FileCode order). That command stream + populated
    tables exist only at runtime. **SAVESTATE TESTED & CHAIN FULLY REVERSED 2026-10-16**
    (Dan's z143 .p2s → eeMemory.bin): the live FED material chain is completely understood
    (group→selector→matdesc+0x24 tags→ctx+0x24 handle→TBP0/TIM2-name). BUT the 24 loaded
    FED models are ALL effects/fixtures (lamp/light/water/waves/window) — **NONE bind the
    wall atlases**. The room walls (068f0N12, textured by the 18 z143k_*/z143h_* atlases)
    have NO FED model; their per-packet texture index lives only in the transient per-frame
    draw stream — not static in any file nor resident in any loaded struct (exhaustively
    ruled out across files AND 32 MB EE RAM). ⇒ FINAL: the GS-dump oracle is THE
    authoritative source. **Binding is SOLVED for 100% of what the game displays** (8020
    packets = all camera-visible wall surfaces). The residual ~48% is geometry the game
    never rasterises from any reachable camera (back-faces/structural/far-LOD, chunks
    0712/0d12) — it has no static binding anywhere and is unrecoverable except by a heuristic
    (inherit front-face neighbour's material) for the enhanced free-camera. Track COMPLETE.
    EXACT and trustworthy throughout: per-vertex positions, UVs, baked vertex
    colours, per-packet geometry. See notes/DECODER-STATUS.md (5th pass).
- **Lives in:** `native/renderer/`, `native/gpu/`, `port/vu/` (disasm +
  RENDER-MATH + shaders).

### AUDIO — decode→mix proven; BGM blocked
- **DONE:** `.at2` voice/SE (**2,726** files) = **ATRAC3 mono 44.1 k, fully
  decoded** (0 errors; clear speech). `.wd` SPU2 sound banks = PS2 ADPCM/VAG
  decoded (4 banks, 248/218/138/92 active; real-waveform autocorr). `db.bin`
  sound-request layer (7,681×24 B loop records) reversed. A **48-voice software
  mixer** (`dirge_sound.c`) mixes a real multi-source scene (voice + looped SE +
  hits + fade) → `sound_mix_sample.wav`, 0 clip.
- **PARTIAL / BLOCKED:**
  - **`.at3` BGM (382 files) = ATRAC3plus — HONEST BLOCKER.** Format fully
    identified (mono 44.1 k, 384 B frame) but the per-frame bit layout is
    DoC/Sony-PS2-specific (`byte[0]=0x01` every frame; ffmpeg misreads it as
    mute). Framing/byteswap/offset/OMA-EA3 wrapper all ruled out. Needs Sony
    `libatrac3plus` or the IOP decoder (not in the EE decomp). WAVs labelled
    `*_FAILED_prefix` / `*_PARTIAL` honestly.
  - `.wd` per-sample rate/tuning, loop flags, and bank↔`db.bin` id map still open.
- **Lives in:** `native/assets/audio/` (`dirge_atdec.c`, `dirge_wd.c`,
  `dirge_sound.c`, `FORMAT.md`). (`dirge_atdec` links libavcodec for now;
  from-scratch ATRAC3 is future.)

### SCRIPTS — RUNS natively
- **DONE:** The custom class cipher (NOT DES — game-specific CFB + S-box
  `(i+120)&0xff`) was cracked; **270/270** scripts decrypt and parse (100%
  readable). A native **JVM subset** (`native/script/jvm.c`) runs real scripts:
  `KerberosIntermissionCalc.calc()` to completion, `install.hdd_error_proc`, etc.
  Full int/float/ref opcode set, all invoke forms, 99 opcodes exercised. Batch
  over the corpus: **220/268 scripts execute clean (82%)**, **399/481 entry
  methods** normal-return; ASan/UBSan clean, 0 leaks. 9/9 self-test,
  mutation-sound.
- **DONE (scripts → engine):** The JVM bridge layer now **drives the real native
  engine**. `native/script/bridge_engine.c` routes `KerberosActor`→`DocActor`
  (load / getpos / setpos / dir / visible via `DocActorManager`),
  `KerberosCamera`→`DocCamera`, `KerberosLight`→`DocLightSet`, and
  `KerberosEvent`/`KerberosZone`→`EngineState` (real mode-flag / zone-flag
  primitive). A bridge test has a real script create+position a `DocActor`,
  move the camera eye, set a light direction and round-trip a flag, asserting
  real engine state; the `DIRGE_NO_ENGINE` mutation reds exactly those assertions,
  proving the wiring is load-bearing. Real-corpus demo: **85/268 scripts load
  real DocActors** (e.g. `ev0306` → `p005`/`n042` positioned). JVM self-test 9/9,
  ASan clean, 0 new aborts. **Still stubbed:** `Window` / `Sound` / `Effect` /
  `GlobalMap` (and actor anim/morph below the seam).
- **PARTIAL:** **239/281 native bridge methods** called (42 dead); the **281-method
  bridge** (11 Kerberos classes: Actor/Camera/Event/Light/Effect/Sound/Window/
  Zone/GlobalMap/Intermission/base) is the **script↔engine interface the native
  engine must implement** (`engine_bridge_api.txt`); the Actor/Camera/Light/Zone
  routes are now wired to real subsystems (above), the rest still need the engine
  behind them. Non-clean scripts are engine-primitive poll loops (e.g.
  `Window.getstate`, `Zone.getflag`, `Event.sleep/poll`), not VM bugs.
- **Lives in:** `native/script/` (`dcc.c`, `jvm.c`, `bridge.c`, `bridge_engine.c`,
  `classfile.c`).

### CONTENT — asset formats broadly solved
| Content | Status | Real numbers |
|---|---|---|
| **Textures** (TIM2/3/4 + RST) | **100%** | 5,850 containers decode no crash (3,064 wide-PSMT4, 1,775 PSMT8, 968 narrow-T4 linear, 11 CT32); readable-text proof |
| **Collision** (CID) | **COMPLETE** | 449 maps, 537,893 packets 100% decoded → **3,869,888 tris**, 0 verts outside leaf AABB, 0 crashes |
| **Effects** (FED/FEZ) | structure solved | 313/313 parse, 2,855 emitters, 24,407 particle defs, 1,523 embedded TIM2, 133,787 channels; FEZ LZ bit-exact |
| **Models — BMD** (static world) | clean | V4-32 float geom; needle/winding fixes landed; real rooms render coherent |
| **Models — MOD/SMD** (skinned char) | mostly | 381/381 batches lock, ~3 k v / 5.5 k t, 5.8% shards (hair ~22%) |
| **Skeleton** (skd/smd/SK2) | recovered | 316 event containers (`noPath/0765*`); 103 world bind matrices; 58/76/98/147-bone skeletons parse clean |
| **Animation** (MOD v0x10 clips) | partial | 519 clip chunks; all 92 bones animate; event-rig vs gameplay-rig mismatch (distortion) |
| **Text / fonts** | EN full, JP partial | 400/400 text-bins decrypt; 5 font atlases; readable EN dialogue + partial kana/kanji |
| **Movie / FMV** | pipeline complete | 21 FMV archives; movie[0] all 3,103 frames (124 s, 640×512 25fps MPEG-2 PS); playable muxed clip |
- **Lives in:** `native/assets/{models,textures,audio,collision,effects}/`,
  `native/text/`, `native/movie/`.

---

## 4. Native engine subsystem table

Reauthored clean-C modules in `native/engine/`, each validated by **oracle-parity
diff + mutation testing** under ctest (ctest counts are cumulative project totals
as each subsystem landed). All **10** subsystems have landed: the **actor**
subsystem is done, so `native/engine` builds `-Wall -Wextra` with 0 warnings and
ctest is **10/10 PASS including `test_actor`**.

| # | Subsystem | Module | Reauthored from | ctest / parity |
|---|---|---|---|---|
| — | Math + VU0 macro | `math/dmath`, `math/vu0.{h,c}` | VU0 macro ops (ITOF4=/16, dest-masked) | 2/2 (with scene) |
| — | Scene-graph | `scene/scene_graph.{h,c}` | `FUN_005a85f8` draw-mode, `002dbfc8`, `002ebee8` tree walk | 2/2 |
| 1 | Object-list traversal | `core/object_list.{h,c}` | `FUN_00408758` + walkers | 3/3 parity+mutation |
| 2 | State machine (spine) | `core/engine_state.{h,c}` | `FUN_00247508` switch (BOOT=1…INTRO=0x1d) | 4/4 dispatch-parity |
| 3 | Material→texture | `scene/material.{h,c}` | `FUN_002e62b0/2ebc48/2ebb18/0030ab10` | 5/5 parity+mutation |
| 4 | Frame loop | `core/frame_loop.{h,c}` (`DocEngine`) | `FUN_00248110` exact order | 6/6 call-seq+mutation |
| 5 | Input | `core/input.{h,c}` | `FUN_00254638` decode, `002553e8` edges, `00258218` stick | 7/7 parity+mutation |
| 6 | Camera | `scene/camera.{h,c}` | `FUN_00259928/9c50/002610e0/00263968` | 8/8 parity (eps 1e-4) |
| 7 | Light | `scene/light.{h,c}` | `FUN_003a50a8/004450f0/0040c988/0040c078` (3 dir + ambient) | 9/9 parity+mutation |
| 8 | Actor | `scene/actor.*` | `DocActor` entity / `KerberosActor` bridge | 10/10 parity+mutation (incl. `test_actor`) |

**Purist seam:** the frame loop replaces GS-DMA transport
(`FUN_0030b768/b868/0040ce28`) with a clean `DocRenderer` interface
(begin/submit_scene/end/present). All builds are `-Wall -Wextra -pedantic` clean.

---

## 5. Reusable tooling index

Pointed to by `native/verify/MANIFEST.md`; all 7 settled standalone subsystems
build **clean (0 warnings)** and pass their tests (`VERIFY_REPORT.md`, 2026-10-15).

**Decoders / loaders**
- `native/assets/textures/` — `libdtex.a` + `dtex` CLI (TIM2/3/4 + RST → PNG);
  `RST-FORMAT.md`; 13 validation PNGs.
- `native/assets/audio/` — `dirge_atdec` (ATRAC3/AT3+→WAV), `dirge_wd` (SPU2
  banks→WAV), `dirge_sound` (48-voice mixer + db.bin); `FORMAT.md`.
- `native/assets/collision/` — `cidtool` (CID → OBJ); `FORMAT.md`.
- `native/assets/effects/` — `fed` (FED/FEZ parser+dumper); `FED-FORMAT.md`.
- `native/assets/models/` — `doc_model.{h,c}` + OBJ/glTF exporters; `FORMAT.md`
  (BMD/MOD/SMD, skeleton bind JSON).
- `native/text/` — text-bin decrypt + kelfnt font decode + dialogue render;
  `FORMAT.md` / `GLYPHMAP.md`.
- `native/movie/scripts/` — `parse_toc` / `decrypt_movie` / `demux_audio` /
  `decode_movie_audio` (stdlib-only FMV pipeline).
- `port/` — `kel_tool` (KEL.DAT list/extract) + `src/kel.c` loader.

**Scripting**
- `native/script/` — `dcc` (class decrypt/disasm), `jvmrun` (interpreter),
  `jvmtest`, `jvmbatch` (corpus coverage); `engine_bridge_api.txt`.

**Renderers**
- `native/renderer/` — portable C99 software rasterizer (ground-truth).
- `native/gpu/` — OpenGL 3.3-core GPU renderer (EGL now; WGL for Windows).
- `native/gallery/` — batch BMD contact-sheet renderer.

**Oracle / recompiler**
- `port/recomp/` — R5900→C static recompiler (contract `ee_runtime.h`;
  13,089-func boundary table; `tools/recompile*.py`).
- `port/oracle/` — reusable executable oracle (`oracle-cli`/`oracle-test`): mmap
  guest, load ELF, wire HLE/SIF/GS/VU, inject memory, call a recompiled function,
  dump state. The validation tool for `native/engine` vs ground truth.
- `port/hle/ sif/ gs/ vu/ cop2/ input/ boot/` — the parked hybrid HLE stack.
- VU: `port/vu/` — `vudis.py` VU1 disassembler, recovered microprograms,
  `RENDER-MATH.md`, purist shaders.

**Verification**
- `native/verify/` — `verify_all.sh`, `VERIFY_REPORT.md`, `MANIFEST.md`,
  `results.tsv`, `logs/`.

---

## 6. Honest open problems + roadmap

### Blocked items (and what each needs)
| Problem | Why blocked | What would unblock it |
|---|---|---|
| **ATRAC3plus BGM** (382 `.at3`) | DoC/Sony PS2-specific per-frame bit layout; no tool on the box decodes it | Sony `libatrac3plus`, or reverse the IOP decoder, or a from-scratch AT3+ decoder |
| **Char hair shards** (~22%) | thin strand-cards collide on the 245-slot VU0 scatter-index pool | finer connectivity model / larger slot handling for strand geometry |
| **Animation rig mismatch** | clips target the runtime-only *gameplay* rig (`km01vnw`); applied to the *event* rig → distortion | recover/bind the gameplay rig (runtime-only `model+0x24` object → scoped oracle dump) |
| **Light / ambient / fog constants** | per-scene, script-set at runtime; no shipped file, no baked default | oracle-dump real VuMem light values per scene; or wire the script light-setters |
| **JP glyph table** | SJIS→glyph-cell index lives in EE code, not reversed | reverse the EE lookup (fonts + EN already done) |
| **BMD env selector→FileCode** | TBP0/CBP are transient VRAM pages, not stable ids | oracle-dump the `context+0x24` slot→FileCode binding at TXD load |
| **FED `.bss` evaluators** | type-code→curve-evaluator + interp are C++ vtables built at boot in `.bss`, not static | oracle-dump the vtables, or reverse the boot-time constructor |
| **Oracle recompiler stubs / vopmsub** | segment-aware fix + cross-product fix live only in the oracle patches | fold `recompile_keldraw.py` and the `vopmsub` fix into the main recompiler |
| **EE code overlays** (`.pex/.ovl/.enc`) | per-boss/zone/debug logic outside the main ELF; some encrypted | decrypt + decompile the overlays (keys for `.enc` not public) |

### The convergence (near-term, tractable)
Wire **engine + GPU + scene** into one running thing: the native app already runs
the `DocEngine` frame loop; the GPU renderer already draws real textured
characters and levels. Joining them — `DocEngine` driving the GPU renderer with a
real character and real scene **through the `DocRenderer` seam** — is the
**capstone** and the highest-value next step. (Deliberately deferred during the
run to avoid build races while `native/assets/models`, `native/gpu` and
`native/engine` had active agents.)

### Finishing animation
Format is fully resolved; the remaining gap is the **gameplay rig**, which is
runtime-only. The unblock is a scoped oracle: relocate the character in the
recomp runtime, call the skin/compose path, dump the bone palette, bake it.

### The long-haul engine reauthoring
10 of ~13,089 functions' worth of subsystems are reauthored (actor now landed).
The bulk — game logic, the full 281-method script bridge, the overlays — remains;
the Actor/Camera/Light/Zone bridge routes now drive the real native subsystems,
while Window/Sound/Effect/GlobalMap stay stubbed. This is the
grind: per-function lift is tractable (methodology + oracle proven), whole-engine
correctness is the multi-year unbounded part.

### What it would take to reach *playable*
Realistically: (1) the convergence capstone (engine loop driving the GPU renderer
with real assets) — months; (2) real scene/placement data wired so levels and
actors position themselves (not hand-set) — significant reversing; (3) the
script↔engine bridge implemented broadly enough that event/field scripts drive
the game (239/281 methods are *called* today, but most need real engine behaviour
behind them); (4) animation on the gameplay rig; (5) audio playback incl. BGM
(blocked on AT3+); (6) input→gameplay, collision→movement/physics wired live; (7)
the decompile-and-reauthor grind across the remaining ~13 k functions and the
encrypted overlays. Each is a substantial project; together they are a **multi-year
effort with no guaranteed playable end** — exactly as scoped at the purist pivot.
The honest headline: **the approach is proven on every pillar; the integration
and the long-tail of game logic are what remain, and they are large.**

---

## 7. Key artifacts

All under **`/srv/shared/dirge-port/`** (auto-syncs to the laptop):

- **`index.html`** — self-contained offline dashboard (brain/files/eyes status,
  stat strip, DONE/PARTIAL cards, engine subsystem table, image + audio galleries).
- **`00_hero_montage.png`** — 6-tile montage (skeleton → skinned → textured char,
  BMD world, readable-copyright-text proof, GPU sphere).
- **Numbered PNGs (progression):** `01–02` first native model render + wireframe;
  `03–07` textures (crate, **copyright text**, weapons atlas, stone wall, rivets);
  `08–09` posed skeleton; `10–11` skinned char; `12–13` clean/textured char +
  native dialogue; `14` animation frames + bind; `15` CG movie frame;
  `17` textured char on GPU; `18` char-in-environment; `19–20` textured env;
  `21` **textured char in textured level (strongest visual)**; plus `gpu_*` and
  `gallery_0{1,2}` contact sheets.
- **Cutscene clip:** `16_cutscene_clip.mp4` (10 s H.264+AAC, A/V synced).
- **Audio WAVs:** `voice_sample.wav`, `se_sample.wav`, `sfx_sample_*.wav` (SPU2
  banks), `sound_mix_sample.wav` (48-voice mixer), `bgm_title_theme_atrac3.wav`
  (ATRAC3 BGM works), and the honestly-labelled `bgm_*_FAILED_prefix` /
  `*_PARTIAL` (ATRAC3plus blocked).
- **Fonts/text:** `kelfnt0_atlas.png`, `kelfnt1_kanji_atlas.png`,
  `text_sample.png`, `13_dialogue_native.png`.

---

*End of state-of-the-port summary. Source of truth for detail: `notes/build-log.md`
+ `notes/autonomy-journal.md`. This file is a read-only consolidation; no source
or other note was modified in producing it.*
