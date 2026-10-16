# Session 2026-10-15 (autonomous) — GS dump toolkit, zone IDs, VU0 oracle fix

Autonomous run while Dan was away. Goal was to turn the PCSX2 GS dumps into
validated ground truth for the room decoder. Honest outcome below.

## Delivered (all validated or honestly bounded)

### 1. GS dump toolkit — `tools/gsdump/` (+ README)
Parser → triangles → textures → texture↔geometry binding, for PCSX2 `.gs.zst`.
- **Parser is byte-exact:** walks the entire ~12 MB packet stream and lands on EOF
  with zero bad packet bytes (a desync would hit a bad type long before the end).
  Header crc 0x33F7D21A / 640×480 / serial SLES-54185 all match byte-for-byte.
- **Rasterizer** reproduces the embedded screenshot's UI layout (dump 04).
- **Texture decode works** incl. the PSMT8 "upload-as-CT32 fast path" (deswizzle +
  CSM1 CLUT): decodes to recognisable textures — Deepground soldier atlases, the
  window fire, Vincent's cape, blue-lit industrial panels. Outputs for review:
  `/srv/shared/dirge-port/gs-dumps/analysis/`.
- Cross-validates the project's `native/assets/textures/swizzle.c`: my independent
  PSMT8 deswizzle matches `dtex_unswizzle8`, and my CLUT unswizzle is mathematically
  equivalent to `dtex_read_clut32`'s CSM1 swap — both producing correct images. So
  swizzle.c's PSMT8 + CSM1 paths are now confirmed against real GS ground truth.

### 2. The prologue dumps ARE zone z128 "Kalm" — and prove the binding is broken
First I ruled out the two *already-decoded* zones (0696=z150, 068f=z143): pixel-level
match found only ONE shared global EFFECT texture, 0 room matches. But then I built the
FileCode->zone map (below) and found the real zone: scanning candidate zones, **z128
"Kalm" (FileCode 0680) has 10 pixel-identical textures with dump-03** (9 room atlas
`z128_a_*` + 1 effect) vs 0 for z142/z154. So the prologue = Kalm, and I HAVE its BMD
containers on disk (0680*). That unblocks real room validation:
- **Decoder generalises to z128:** all 13 Kalm room chunks (06800012..0c12) decode to
  coherent geometry (e.g. 06800212 = 12647 tris, 16 materials, sensible bbox), same as
  0696.
- **Texture extraction validated vs ground truth:** all 10 dump-drawn textures are
  pixel-identical (diff 0.0) to the decoder's own extraction for the chunk.
- **Texture BINDING proven broken vs ground truth (the "mangled textures"):** comparing
  the decoder's dominant-order guess to the dump's real texture->triangle binding across
  all 13 chunks: **0/9 drawn room textures are correctly bound** — 8 of 9 extracted but
  assigned to NO group anywhere; 1 to a too-small group (210<380 visible). The heuristic
  extracts 138 textures/chunk but assigns the same wrong 16. Oracle saved:
  `native/assets/models/out/z128_dump03_binding_oracle.json`.
FIX PATH (clear, not done): align each dump draw-group to a decoder material-selector by
transform-invariant **UV overlap** (UVs survive the camera), then read off the true
selector->FileCode map. Deferred rather than rushed (needs care: dump is a culled,
multi-chunk view mixing characters).

### 3. Zone map decoded — `notes/zone-map.md`
Decoded `zonelist.txt` (Shift-JIS). **The decoded BMD rooms are specific levels:**
- FileCode **0696 = z150 = "W.R.O. Research Facility 2"** (mid/late game)
- FileCode **068f = z143 = "Mako Reactor 0 (part 1)"**
Both mid-game → unreachable by scripted cold-boot play. To ground-truth the rooms,
Dan parks a savestate in one of these levels (ideally WRO Research Facility 2),
drops the `.p2s`; load headless via `-statefile`; Shift+F8; then `gsbind.py` reads
the texture↔submesh binding (the runtime oracle the BMD file doesn't store).

### 4. VU0 oracle cross-product bug FIXED + regression test
`port/cop2/vu0_macro.c ee_cop2_vopmsub` subtracted the SAME products VOPMULA added
→ cross product always 0 (~202 sites: normals/backface). Now subtracts the swapped
products (`fd = fs × ft`), alias-safe (read-before-write temporaries). Numeric test
`port/cop2/test_vu0_cross.c` (3 cases incl. in-place) PASSES on fix, FAILS (all 0)
before. Oracle now agrees with the always-correct native `vu0_opmsub`. Also audited
the rest of `vu0_macro.c` (FMAC, FTOI/ITOF, CLIP, VDIV/VSQRT/VRSQRT, VMFIR/VMTIR)
and the `ee_runtime.h` PS2 float helpers — all correct; vopmsub was the only bug.

## What's left for the room binding (clear, bounded — next session)
The oracle is captured; the FIX is not written. Recover the true submesh->texture map:
  1. From the dump (tools/gsdump/gsbind.py): each drawn texture's triangles + their
     UV(s,t) coords — transform-invariant.
  2. From the decoder (export_bmd_textured.py): each material-selector submesh's UVs.
  3. Match dump-group <-> decoder-selector by UV-set overlap -> assign the dump's real
     texture (FileCode) to that selector. Replaces the dominant-order guess.
  4. Validate: re-render; the 9 z128 textures should land on the right surfaces.
Caveats to handle: the dump is a culled, multi-chunk view that also contains CHARACTER
draws; match per-chunk and ignore non-room draws. A z150/z143 savestate from Dan would
give a cleaner single-zone oracle, but z128/Kalm already works as the proving ground.

## Still needs Dan (optional, for a cleaner oracle)
- A savestate parked in z150 ("W.R.O. Research Facility 2") or z143 ("Mako Reactor 0")
  would ground-truth the *already-decoded* zones directly. Not required — z128 works.

## 2026-10-16 addendum — binding root-cause deepened, a false doc corrected

Continued the binding fix. Before matching anything I ground-truthed the exporter's
premise and found it broken deeper than the oracle already showed:

1. **The giftag "material selector" is the VERTEX COUNT (proven 1273/1273).**
   `native/assets/models/probe_giftag.py` on z128 06800212: `qword1.x & 0x7fff ==
   unpacked V3-16 num` for every one of 1273 packets. The "16 selectors 0x03..0x20"
   FORMAT.md §3.4 reported are the 16 distinct vertex counts (3,4,6,…,32) — small
   because the room is tessellated into tiny quad/tri submeshes. So there is **no
   per-submesh material tag in the VIF draw chain**, and `export_bmd_textured.py`'s
   16 "material groups" are **vertex-count buckets, not materials** — a second,
   deeper root cause of the mangled textures on top of the dominant-order guess.
   Corrected in-repo: FORMAT.md §3.4 (giftag + Texture-binding notes) and the
   export_bmd_textured.py docstring. (DECODER-STATUS already had it right.)

2. **Real binding chain confirmed from decomp.** `FUN_002e62b0`: draw-request word →
   `group=(w>>10)&0x7f`, `sub=w&0x1ff`; runtime model-instance `M=*(sceneNode+0x18)`
   (via `FUN_002e5c60`, scene list at `ctx+0x78`); group count `*(u16)(M+0x40)`, group
   array `*(M+0x60)`; per group: subcount@+6, **material selector u16 @+8**, sub-geom
   array @+0x20; selector→FileCode via handle table `context+0x24`. The BMD file node
   graph is file-followable (`FUN_0026abe8` hdr → `FUN_0026ba88` nodes: subcount s16@0,
   geocount s16@2, sub-array int@0x08, geo-array int@0x0c → `FUN_0026f518` leaves:
   count int@0x04, VIF-block ptr-array @0x40). Mapping that graph to `M+0x60` is the
   one remaining trace (the model-register function) — the defined general-fix task.

3. **Simple dump-based recovery FALSIFIED.** `tools/gsdump/src/probe_draworder.py`:
   textures are heavily interleaved (dominant tbp 0x2e50 = 10003 tris in **2202**
   runs). The oracle's room atlases are only moderately contiguous (4–24 runs); the
   massively fragmented textures are the Deepground-soldier/Vincent character + effect
   draws. Room total in dump-03 ≈ 6180 visible tris vs one chunk's 12647 ⇒ this dump
   is a **culled, multi-chunk, character-laden** view. Both draw-order and UV-overlap
   alignment are defeated by the culling gaps + chunk-mixing. So the binding is NOT
   reliably recoverable from this dump.

4. **The binding chain is ALREADY ported & parity-tested — it's just never fed real
   file data.** `native/engine/scene/material.{c,h}` reauthors the whole chain
   (`doc_material_resolve_texture`: draw_word → group/submesh → material selector →
   layer tags → `context+0x24` slot → texture handle/descriptor), transcribed from
   `FUN_002e62b0/2e8830/2ebc48/2ebb18/0030ab10` with every guest offset annotated
   (group_count@0x40, groups@0x60, materials@0x64, selector@grp+8, subcount@grp+6,
   subs@grp+0x20, layer_lists@matdesc+0x24, stage0@matdesc+0xe, handles@ctx+0x24,
   override@handle+0x54). Tests `tests/test_material.c` give it 5/5 parity+mutation.
   BUT: `DocMaterialModel` is only ever built from SYNTHETIC fixtures (test_material.c
   and main.c's "minimal, resolves OK" demo) — **no loader parses a real BMD into it**,
   and `export_bmd_textured.py` (which rendered the rooms) doesn't use material.c at all.

**Net:** the binding gap is now precisely diagnosed end-to-end. The LOGIC is done and
tested; what's missing is exactly two concrete pieces:
  (i) a **BMD→DocMaterialModel loader** that parses the group table + material
      descriptor table + layer lists from the file bytes (data believed on-disk:
      material IDs ~payload+0x1900 per DECODER-STATUS; geometry grouping in the node
      graph `FUN_0026ba88/f518`). Still needs the file layout of the group/material
      tables verified and assembled (M is runtime-built from the node graph by the
      model-register; find that function to map file→M+0x40/+0x60/+0x64).
  (ii) the **slot→FileCode handle table** (`context+0x24`, built at TXD load) — the one
      genuine runtime item; the GS-dump oracle supplies it per room (z128 captured).
Alternative to (i)+file-RE: a cleaner single-room GS dump (savestate, minimal
characters) feeds gsbind.py directly. I deliberately did NOT ship a guessed binding
(false-positive risk, and Dan is the visual judge).

New/changed files: `native/assets/models/probe_giftag.py`,
`tools/gsdump/src/probe_draworder.py`, FORMAT.md §3.4 (corrected),
export_bmd_textured.py docstring (corrected), DECODER-STATUS.md (5th pass).

## Files
- `tools/gsdump/{src,README.md}` (GS dump toolkit)
- `notes/zone-map.md` (zonelist), `notes/filecode-zone-map.md` (FileCode prefix->zone,
  fills the "BMD selector->FileCode" gap)
- `native/assets/models/out/z128_dump03_binding_oracle.json` (ground-truth binding)
- `port/cop2/{vu0_macro.c (fixed), test_vu0_cross.c (new regression test)}`
- `/srv/shared/dirge-port/gs-dumps/analysis/*.png` (validation images)
- Status docs updated: DECODER-STATUS.md, STATE-OF-THE-PORT.md, build-log.md,
  native/engine/DECOMP-METHOD.md, pcsx2-headless-setup.md.

## 2026-10-16 (cont.) — BMD file-structure RE checkpoint (toward a file-based binding)
Goal: reconstruct the binding from file bytes (submesh→group→selector→material→tag→
slot→FileCode) so rooms texture without runtime, with the z128 GS oracle as validation.
Progress (probe_bmd_struct.py + ad-hoc dumps on 06800212):
- RELOCATION CONVENTION pinned: BMD in-file pointers are PAYLOAD-RELATIVE; deref =
  stored_value + payload_base (payload @ file 0x30 here). (Earlier file-absolute guess
  gave garbage.) FUN_0026abe8 adds base to +0x14/+0x10/+0x1c/+0x18/+0x20/+0x18a8/+0x18b8.
- Header (payload-rel): magic "BMD"@0, version=16@+4, cntB(u16)@+8=2, cntA(u16)@+0xe=16,
  arrA@+0x14=0x2074, arrB@+0x1c=0x8f00, bone@+0x20=0x95d0, v>=6 tbl@+0x18a8=0x18fc.
- NODE GRAPH walks cleanly: arrA → 16 node pointers (payload-rel) → 16-byte nodes
  (s16 subcnt@0, s16 geocnt@2, sub_arr@+8, geo_arr@+0xc). Only 2 non-empty: node[10]
  (7 subs) + node[11] (2 subs) = 9 sub-entries. The 9 sub pointers are 0x21c0,0x2340,
  0x24d0,0x2650,0x27d0,0x2950,0x5930,0x74c0,0x7ca0 (first 6 spaced ~0x180).
- Each sub-entry is a V4-32 FLOAT VERTEX block (positions, (x,y,z,1.0)); e.g. 0x21c0
  starts (-1374.2,-96.5,-1117.6,1), all within the chunk bbox X[-1720,8]Y[-440,31]
  Z[-1881,5]. So the node graph = the §3.2 position-only runs (the grey path), NOT the
  attributed/textured VIF chain, and carries no material selector.
- IMPORTANT LEAD: 9 position blocks ≈ the 9–10 room textures in the oracle. Still
  unlocated: M+0x60 group table (matsel@group+8) and M+0x64 materials[] — M is a runtime
  struct (confirmed: payload+0x40/+0x60/+0x64 are 0/float/0, so M≠payload). From
  FUN_002e63d0: materials=*(M+0x64)[selector]; material desc has tagcount@+0x18,
  tag-array ptr@+0x20, format byte@+0xe; tags index global DAT_0046cab0 for mode.
NEXT: find where the group/material tables live in the file (likely reachable from arrB
@+0x1c, the bone-tree draw-template palette @+0xb0 that FUN_0026bf88 seeds, or the
v>=6 table @+0x18a8) and how the model-register copies them into M+0x40/+0x60/+0x64.
Probes: scratchpad/probe_bmd_struct.py, native/assets/models/probe_giftag.py.

### BMD draw-table found (payload+0x18fc) — strong lead, not yet fully decoded
The v>=6 table pointer @payload+0x18a8 (deref file 0x192c = payload+0x18fc) heads a
160-record array, stride 12: record = {u8/u32 type@+0, ptrA@+4, ptrB@+8} (ptrs
payload-relative, +0x30). This is the region DECODER-STATUS flagged ("material IDs
~payload+0x1900; handle records; EE ptr 0x22de50" — and indeed rec[159] contains
0x0022de50 and points at the node array).
- Type-byte histogram: 146×0x30 + 14 singletons (0,4,8,0x28,0x29,0x2d,0x2e,0x32,0x33,
  0x35,0x36,0x48,0x82,0x87). So it's a dominant draw-record type (0x30) + 14 specials,
  NOT 16 even material buckets (earlier read corrected).
- For a 0x30 record: ptrA → a 0x84-byte block of packed per-vertex attributes (repeated
  4-byte groups like 0x80202325 = RGBA-ish, 0x4c151312) ending with tag 0x65 (= the
  draw-node type FUN_002e63d0 writes: *puVar10=0x65) + GIF A+D data. ptrB → a VIF/GIF
  draw TEMPLATE (contains STCYCL 0x01000404 and MSCNT 0x17000000, unpack cmds).
- So this 160/146 table is the per-submesh DRAW table (attributes + VIF template per
  submesh). Open question: 146 records vs 1273 STCYCL-0x01000404 blocks decode_packets
  finds — reconcile (likely decode_packets over-splits; true submesh count may be ~146),
  and find where each record's MATERIAL ID / texture slot lives (the type byte? a field
  in ptrA's 0x65 node? the 14 specials?).
NEXT CONCRETE STEPS (resumable):
  1. Map the 146 draw records to the 1273 VIF packets (match ptrB template offsets to the
     STCYCL chain) to get the true submesh count + grouping.
  2. Find the material/texture-slot field per record (decode the 0x65 draw-node in ptrA;
     cross-ref FUN_002e63d0 which reads materials=*(M+0x64)[selector], tagcount@matdesc
     +0x18, tags@+0x20).
  3. Resolve slot→FileCode via the handle region (the ptrA/ptrB targets around 0x445e0,
     0x30460, 0x3d400, 0x4c550 — check whether these are in-file texels or TXD-load slots).
  4. Validate the whole decode against the z128 GS oracle (texture→tris).
HIGH-VALUE INPUT FROM DAN (would ~10x this): a PCSX2 savestate parked in ANY room with
few characters on screen → headless GS dump → gsbind.py gives the submesh→texture binding
as direct ground truth AND validates the file decode above. (notes/pcsx2-headless-setup.md
has the load-via -statefile recipe.)

### Reconciliation: 146 draw records vs 1273 VIF blocks
- 1273 STCYCL-0x01000404 blocks span file 0x2270..0x9b278 (nearly whole file).
- The 146 type-0x30 draw records' ptrB all fall WITHIN that region (146/146) but NONE
  land exactly on a STCYCL (0/160) — they point mid-stream at the GIF A+D register-setup
  portion of each submesh draw (e.g. ptrB=0x44664 → "0a8f07f0…" A+D data, then STCYCL+
  MSCNT a few qwords later). ptrB groups: runs spaced 0x10 apart then jumps (5×0x446xx,
  then 0x304d4, …) — a per-submesh index into the VIF stream.
- Interpretation: the 146 records are the true SUBMESH DRAWS (each = attributes ptrA +
  GIF/VIF setup ptrB); the 1273 STCYCL blocks are the finer unpack sub-components
  (so decode_packets over-splits ~8–9×). The material/texture identity is encoded per
  record (type byte and/or the GIF A+D setup at ptrB — likely a baked TEX0, though TBP0
  is transient). CRACKING THE BINDING = decode the per-record GIF setup / type to a
  texture slot, map slot→FileCode (handle region near 0x445e0/0x30460/0x3d400/0x4c550),
  validate vs z128 oracle. This is the precise remaining RE; entry points all mapped.

## 2026-10-16 (cont.) — CLEAN GROUND TRUTH: Mako Reactor 0 (z143) dump from Dan
Dan captured a GS dump in Mako Reactor 0 (US build SLUS-21419) with only Vincent on
screen — a clean single-character room, exactly what the binding needs. Delivered:
- Parser handles the US v9 GS-dump format byte-exact (lands on EOF, 0 warnings).
- gsbind decoded the T8 room textures; I ported PSMT4 via a C harness that REUSES the
  validated native dtex_unswizzle4 (scratchpad/t4harness.c + swizzle.c; decode_t4_dump.py)
  — 13/14 T4 textures decode (the one skip is a 32x32, below the wide-T4 path).
- match vs on-disk 068f extracts (all 67 TXD containers, dtex): **18 room textures bound
  to their FileCodes at corr≈1.0** (T8 + T4), e.g. the big pipes tbp0x2af0(5416tri)=
  068fff26, 0x2c88=068fff39(rust), 0x2b00=068fff34(pipe). Oracle saved:
  native/assets/models/out/z143_mako_dump_binding_oracle.json.
- **US dump textures match EU on-disk extracts at corr 1.000 => room assets are
  byte-identical across US/EU regions.** So this US dump is a fully valid oracle for the
  EU 068f decode. (Also independently re-validates dtex_unswizzle4 AND dtex_unswizzle8
  against real GS ground truth.)
This is a CLEAN, near-complete texture-binding oracle for a real room — the thing the
earlier culled prologue dumps could not provide. NEXT: (1) compare the 068f0612 decoder's
dominant-order guess to this oracle (quantify the gap on a clean room); (2) UV-match dump
draws -> decoder submeshes to assign the real FileCode per submesh (now low-confound);
(3) use it to validate/crack the file-based binding (146-record draw table @payload+0x18fc
-> material -> layer tag -> slot -> FileCode; slot order likely == sorted TXD/FileCode
order — testable against this oracle).

---

## 2026-10-16 (cont.) — UV-triple binding: whole-room ground truth

**Result: first real BMD env texture→submesh binding, ground-truthed.**

- **Point-cloud UV match REJECTED** as false-positive-prone: per-FileCode UV
  point-cloud subset scoring was unstable across quantisation (068fff34 swung
  73→5→74 packets over Q=256/128/64) because tiled [0,1] UVs collide across
  textures. Did NOT trust it.
- **UV-triple identity match adopted.** A triangle's 3 UV pairs is a strong
  fingerprint. decoder UV = rawV2-16/4096 ≡ dump texcoord = ST/Q (FST=0), so a
  true surface's triple matches exactly. STABLE over Q=256/1024/4096 (201 pkts,
  identical distribution, 1-2 vote conflicts of 201 on 068f0612). Script:
  `scratchpad/uvtriple.py`; batch `scratchpad/uvbatch.py`.
- **Whole-room: 5403/15332 packets matched (35.2%)** across the 13 `*12`
  room-geometry chunks (only `068f0N12` parse as attributed geometry; `*10/*11/
  *14/*00..26` yield no VIF packets). All 18 room FileCodes present, whole-room
  vote-conflicts <5. 068fff26 dominates (2023 pkts; biggest oracle atlas 5416tri).
  Chunks 068f0912/0a12/0b12 give near-identical distributions ⇒ instanced pipe
  assemblies (independent 3× cross-validation). 35% ceiling = single-frame dump
  culling (back-face/occluded/off-frame); more camera angles raise it.
- Repeating motif in packet order: `068fff34(×4) → 068fff32(×2)` ⇒ ~4-5 decoder
  packets per true submesh (691/146≈4.7), all getting the SAME FileCode from the
  independent match = internal validation + submesh grouping showing through.
- Artifacts: `out/z143_room_uvtriple_binding.json`, `out/z143_pktfc/pf_*.json`.
- **giftag is a fixed A+D template** (qword0 NLOOP 7/0, `0x303ec000…`), NOT
  per-submesh TEX0. So scalable binding must go through the file node→group→
  selector→material→slot chain (reauthored in engine/scene/material.c), not the
  VIF giftags.

**NEXT (scalable, all-rooms fix):** RE the file loader `FUN_0026abe8` group-table
construction so the file node struct → runtime group (selector @group+8) mapping
is known; extract per-submesh selector from 068f0612; cross-tab selector vs the
known FileCode (5403 ground-truth pkts) to confirm selector→FileCode (hypothesis:
slot order = sorted TXD/FileCode load order). `scratchpad/walk.py` already yields
per-submesh VIF offsets (leaf ptr-array @+0x40) but the file *node* struct ≠
runtime *group* struct, so the loader transform must be read, not guessed.
Also: decode 2nd Mako dump (…115733) when it syncs → more angles → higher recall.

### Loader RE progress (toward scalable file-based binding)
- `FUN_0026abe8` = RELOCATION pass only (adds load base to pointer fields;
  confirms payload-relative convention). Recurses: arrA nodes via `FUN_0026ba88`,
  arrB via `FUN_0026bc28`, bone via `FUN_0026bf88`, ver>=8 table @+0x18b8
  (cnt @+0x18b4). Structs are used IN PLACE (no rebuild here).
- `FUN_0026ba88` node (short*): s16@0 subcount, s16@2 leafcount, int@8 sub-array,
  int@12 leaf-array. Each sub gets +0x9c backref set; each leaf via `FUN_0026f518`
  (int@4 count, ptr array @+0x40 = per-submesh VIF offsets, +8 zeroed).
- **DISPROVEN: node sub-array @+8 entries are NOT material groups.** Read as
  group{+6 subcnt,+8 selector,+0x20 subarr} they give garbage (subcnt 49096,
  sel 39322, subarr=0xbf200000=-0.625f) — the sub entries hold TRANSFORM/matrix
  floats. So runtime group table M+0x60 is NOT the file node sub-array.
- `FUN_0026bc28` arrB entries (cntB=22, big ~0xe0 structs) = DRAWABLE INSTANCES:
  +0xc4 u16 = arrA node index, +0xc8 count, builds per-draw table @+0xd0
  (stride 0x10 {ptr,0,self}). Not the M+0x40/+0x60/+0x64 material model.
- **CONCLUSION: M (the group/material model, `M=*(sceneNode+0x18)`, M+0x40 group
  count / M+0x60 groups / M+0x64 materials, read by FUN_002e62b0/63d0) is built at
  RUNTIME by an init not yet located — it is NOT at a fixed file offset.**
  NEXT: trace callers of FUN_002e62b0 / the scene-load that assigns sceneNode+0x18
  and populates M+0x60/+0x64, to find which file section (arrB instances? the
  ver>=8 table of 8? a material block) feeds the selector + layer tags. Validate
  the recovered selector→FileCode against the 5403-packet UV-triple ground truth.
- DISPROVEN too: node leaf VIF arrays (FUN_0026f518, leaf+0x40) do NOT map 1:1 to
  the decoder's 691 STCYCL packets — only 8 nodes expose 32 leaf VIF offsets and
  none equal a packet start. The leaf pointers and the raw VIF-chain packets are
  different granularities; node->packet grouping is not recoverable this way.
  So "is FileCode constant per arrA node?" is untestable via leaf arrays; needs
  the M-init trace. (scratchpad: groupsel.py, nodefc.py — both negative, kept.)

### Consumable artifact + camera-overlay dead-end
- NEW exporter `native/assets/models/export_bmd_bound.py`: groups packets by the
  REAL per-packet FileCode (out/z143_pktfc/pf_<chunk>.json) and binds the actual
  TXD container, replacing export_bmd_textured.py's vertex-count-bucket placeholder.
  Emits per-FileCode DMSH + manifest + .list. Ran all 13 chunks ->
  out/bound_<chunk>_*. Whole room: 65980/178898 tris TEXTURED (36.9%); unbound
  packets emitted as one textureless 'unbound' group (honest, not guessed). By
  tris the coverage > the 35% packet figure (bound packets skew larger, e.g.
  068f0212 = 47.6% of tris).
- Camera-overlay validation ATTEMPTED and deferred (scratchpad/camera.py): tried
  to recover a model->screen projection from UV-triple correspondences and overlay
  bound centroids on the real frame PNG. FAILS because UV-triples are NOT unique in
  screen space (heavy texture tiling -> same 3 UVs recur at many locations), so UV
  gives no geometric correspondence; also the dump PNG is 1213x910 (upscaled), not
  PS2 framebuffer space. Proper fix = geometric RANSAC over same-UV candidate
  matches. NOT needed for binding correctness (already validated: stable triple
  match, 0 conflicts, corr 1.0, instanced-pipe 3x cross-check) -> deferred.

### Multi-angle recall boost (7 dumps) — 35% -> 52%
Dan provided 5 more z143 GS dumps from different camera angles/positions (same
area), + the 2 originals = 7 total. KEY: the new dumps use DIFFERENT TBP0 VRAM
layouts (only 3-12 of 18 oracle pages present per frame), so the z143 oracle does
NOT transfer directly. Solved WITHOUT decoding any textures via
`scratchpad/grow_fingerprint.py`:
  1. seed master fingerprint {UV-triple->FileCode} from the 2 oracle-labelled frames;
  2. for each new dump, DERIVE its TBP0->FileCode by UV-triple agreement with the
     current master (a textured tri whose UV-triple is already labelled says "this
     frame's page P shows FileCode F"); accept a page only on a near-unanimous vote
     (collision guard MINV>=3, MARGIN>=3x);
  3. harvest that dump's newly-visible surfaces into the master; 2 passes.
Result: master grew 1858 -> 5098 UV-triples; WHOLE-ROOM recall 5403/15332 (35.2%)
-> 8018/15332 (52.3%), conflicts <=4, all 18 FileCodes. Main shells: 068f0112
1607/1803=89%, 068f0212 1733/2497=69%. By tris: 52.9% textured (94657/178898).
PRECISION check: 068fff2a surged 56 oracle-tris -> 528 pkts in 068f0212; ROBUST to
a strict guard (MINV=10,MARGIN=8 -> still 528) => a genuine large surface culled in
the original 2 frames, not a bootstrap artifact. Artifacts refreshed:
out/z143_room_uvtriple_binding.json, out/z143_pktfc/pf_*.json, out/bound_* meshes,
out/bound_renders/*_grown_* (synced to /srv/shared/dirge-port/binding-renders/).
Remaining unmatched = geometry not visible from ANY of the 7 angles (far/ceiling);
more angles would raise it further. Scalable all-rooms fix (oracle-dump runtime M)
unchanged.

---

## 2026-10-16 (cont.) — GS-dump binding method SATURATED at 52.3%; pivot to runtime-M recovery

Dan supplied 11 MORE z143 camera angles (zip drive-download-20261016T151307Z, decompressed to scratchpad/newdumps2/d2_*.bin). Ran grow_fingerprint.py over the FULL 18-angle set (2 oracle seeds + 5 first-batch d_143*.bin + 11 new d2_*.bin) vs all 14 068f0?12 chunks → scratchpad/pktfc_18angle/.

**Result: 8020/15332 packets (52.31%).** Prior 7-angle was 8018 (52.30%). So +11 fresh angles = **+2 packets**, while the master fingerprint MORE THAN DOUBLED (5098 → 11251 distinct UV-triples). Interpretation: the new angles re-saw already-covered surfaces (added triple density, not new packet coverage). **The GS-dump method is saturated** — recall is capped by what any in-room camera can rasterise.

Per-chunk unmatched (18-angle), worst:
- 068f0712: 1366 pkts, 48 matched → **96% dark**
- 068f0d12: 956 pkts, 28 matched → **97% dark**
These two stayed ~97% dark across ALL 18 angles = back-face/structural/far-LOD geometry the player camera never points at. Other chunks' residuals are scattered back-faces within mostly-covered shells.

**Decision:** stop collecting GS angles for z143 (diminishing returns proven numerically — Dan's "validate numerically not by eye" discipline). The scalable, visibility-independent fix is to recover the full runtime material model `M`:
- Draw-chain struct fully mapped: FUN_002e62b0 `M+0x40`=groupcount, `M+0x60`=group-ptr array, group+6=subcount, group+8=selector, group+0x20+sub*4=submesh; FUN_002e63d0 `matdesc = *(M+0x64)[selector]`, layercount `matdesc+0x18`, layerlist ptr `matdesc+0x20` (ushort[]), format byte `matdesc+0xe`, LOD thresh `matdesc+4`; layer→texture via runtime callback `((uint*)cbctx)[9](layer, cbctx)`.
- OPEN FORK (subagent tracing): is `M+0x60`/`M+0x64` payload-relative in the relocated BMD (→ STATIC recovery, all rooms, no dump) or runtime-synthesized (→ needs one EE-RAM savestate per... no, one savestate gives the WHOLE room's M since M is built at scene load regardless of camera)?
- The 8020 bound packets (out/z143_pktfc/, out/z143_room_uvtriple_binding.json) are the ground-truth CHECK SET for whichever recovery path lands.

Artifacts this session: scratchpad/newdumps2/ (11 new bins), scratchpad/pktfc_18angle/ (18-angle pf_*.json + room_binding_summary.json), scratchpad/grow18.log.

## 2026-10-16 (cont. 2) — static-binding hunt: FED mechanism confirmed payload-relative, but room fileset mapped & FED is NOT the texture carrier

Decomp trace (subagent) on how the draw-time model `M` (sceneNode+0x18) is built:
**VERDICT PAYLOAD-RELATIVE.** `M` is the `FED` resource blob itself, used in place; relocator `FUN_002e5550` only rebases stored offsets (`new = blobbase + (stored - param_2)`), no malloc/memcpy. So the binding, wherever it lives, IS static in the file. Scene-list roots for locating M in EE RAM if ever needed: globals `0x004EE2E0`(head 0x4EE358) and `0x004EE3C0`(head 0x4EE438); walk head→node, `node+0x18`=M, `node+0x20`=sibling, match `M+4` vs `id & 0xfffc0000`.

**z143 per-sub-object fileset mapped** (code `068f0N??`):
- `068f0N10` tiny (0.8–3 KB) index/header blob.
- `068f0N11` = **`mapid`/`rfd`/`cid`** nested container, LARGE (214–392 KB). ← prime binding candidate.
- `068f0N12` = `model`/`rfd` = geometry (VIF packets; already decoded).
- `068f0N14` = `FED` scene-node/LOD model (id `0x20000000 + N*0x40000`).
- `068fffNN` = `txd` texture containers.

**FED (`068f0N14`) parsed** (tool scratchpad/fed.py, from relocator spec): header +0x40 u16 groupcount, +0x42 matdesccount, +0x60 grouptab, +0x64 matdesctab; group +6 subcount, +8 selector, +0x20 submesh resource-id refs (`0x20040000|…`); matdesc has nested layer arrays (+0x14 structs cnt@+0xa, +0x20 u16 layer-type list cnt@+0x18). **BUT every group's selector parses as 0** → draw consumer `FUN_002e63d0` line 73 treats matdesc[0] (LOD thresh 0) as skip. A room shell that's 89% visible cannot be all-skip ⇒ **the FED is NOT the visible room's texture binding** (it carries bounding/LOD + geometry resource refs). So static recovery must come from `mapid`/`rfd` (068f0N11), not FED.

STATUS: GS-dump track = DONE & validated (52.3%, saturated). Static track = binding proven static-in-file, carrier narrowed to `mapid`/`rfd`, exact field not yet cracked (handed to a focused subagent to reverse `mapid` consumer + validate submesh→FileCode against the out/z143_pktfc oracle). Tools ready: scratchpad/mwalk.py (generic M walker, self-test PASS), scratchpad/fed.py (FED parser).

## 2026-10-16 (cont. 3) — STATIC BINDING DISPROVEN (definitive); binding is runtime-only, confirmed

Second focused subagent reversed the `mapid` lead + validated numerically against the oracle. **Negative, multiply-confirmed:**
- `068f0N11` `mapid`→`rfd`→`cid` = COLLISION data. Loader dispatch 002585e0:41-42 magic `"cid"`→FUN_00271208 (a pointer-relocator; FORMAT.md labels it collision). 068f0111 holds 3386 `0x3f800000` (float 1.0) = vec4 w's = ~3386 collision verts. NOT materials.
- `068f0N12` geometry VIF stream = pure UNPACK (0x6c V4-32 ×1804, pos/col/uv, STCYCL/STMOD/STROW/MSCNT). **ZERO DIRECT/GIF packets, ZERO TEX0 writes** → stream sets no texture. Confirms the giftag field = vertex count (`qw1.x = 0x8000|vtxcount`), not a selector.
- BMD group table (FUN_0026abe8 +0x14 array, 128/143/10 groups for chunks 1/2/7): material-selector field `u32(grp+8)` is a UNIFORM relocated STUB `0x0022de50` (125/128, 136/143, 8/10 identical). Count matches neither ~13 materials nor ~1803 packets.
- FED `068f0N14` selectors all 0 (prior finding).
- Exhaustive array scans (scan_bij.py/scan_func.py over 0112/0111/0114/0110, widths 1/2/4, strides 1/2/4/0x10): ZERO arrays map packet-order→oracle FileCode consistently. Packet counts matched oracle exactly (1803/2497/1366/956) so indices were aligned — the mapping simply isn't in the files.

**ROOT CAUSE:** the `_KEL/noPath` extracts are POST-RELOCATION MEMORY IMAGES (pointer fields hold absolute 0x0022xxxx/0x003xxxxx runtime addrs). The per-submesh selector, group→submesh sub-pointers, and `context+0x24` handle table are all runtime-constructed and left as stubs/defaults. The real binding is keyed by the per-draw command word (param_3) consumed by FUN_002e62b0 — not materialized in these files. This VINDICATES the original methodology call (STATE line ~337): runtime-built → must be oracle-dumped, not statically reversed.

**So the binding's only two sources are:** (a) GS-dump oracle (authoritative, visible-only, 52.3% — SATURATED), or (b) ONE z143 EE-RAM savestate / live PCSX2 breakpoint on FUN_002e62b0 to read the POPULATED model+0x60 selectors + context+0x24 handle table → complete 100% binding incl. the dark chunks. mwalk.py ready; scene-list globals 0x4EE2E0/0x4EE3C0 (heads 0x4EE358/0x4EE438), node+0x18=M. Oracle handles upload in FileCode order (0x2a04→0f … 0x2e2c→42) so selector order ≈ FileCode order — a strong cross-check once real selectors are read.
Negative-result scripts: scratchpad/{scan_bij,scan_func,pkthdr,tbp_test,vifscan}.py.

## 2026-10-16 (cont. 4) — SAVESTATE WORKS: live FED has REAL selectors; binding IS in EE RAM

Dan captured a PCSX2 savestate at the same frame as the latest z143 GS dump: `SLUS-21419 (44A5FA15).01.p2s` (zstd-zip). Extracted eeMemory.bin (32 MB) via manual zip-slice + `zstd -d` (python zipfile reads method-93 metadata; 7z/unzip can't decompress zstd-zip). At scratchpad/savestate/eeMemory.bin. EE addr → file offset = addr & 0x1FFFFFF.

Scene-list globals from the earlier trace (0x4EE2E0/0x4EE3C0) were NULL in this state (session-specific). Instead located models directly: **searched EE RAM for `FED\x08` blobs → 24 found**, of which the z143 ones carry ids 0x2000/0x2004/0x2010/0x2014/0x201c/0x2024…0000 (sub-objects), flags now 0xff0000 (loaded+relocated) and table pointers are ABSOLUTE EE addresses.

**The live FED for the main shell (id 0x20040000 = 068f0112/0114) @0xe90130 has REAL, DISTINCT selectors** — the cold-file stub problem is gone when loaded:
- group[0] sub=32 SEL=0 ; [1] sub=16 SEL=1 ; [2] sub=13 SEL=2 ; [3] sub=1 SEL=3 ; [4] sub=2 SEL=4 ; [5] NULL ; [6] sub=4 SEL=5. (68 submeshes; 6 matdescs.)
- Each submesh @group+0x20 points to a struct whose +0 = geometry sub-resource id (0x20040000|K<<10) + a bounding box (floats).
- Each matdesc (matdesctab[sel]) has real layer data: +0xa u16 cnt of +0x14 layer-struct ptr array; +0x18 u16 cnt of +0x20 u16 layer-TYPE list. matdesc0 layer-types=[3,4,7,a,e,11,3d,3e]. Texture is resolved at draw by layer-type → `context+0x24` handle table (FUN_002ebb18: handle=*(context+(tag&0xffff0fff)*4+0x24)); TBP0 then via oracle.

So the complete binding (incl. invisible chunks 0712/0d12) is recoverable from this one savestate. Remaining decode (handed to focused subagent): (a) locate the context+0x24 handle table → layer-type→TBP0→FileCode (oracle has TBP0→FileCode); (b) map geometry packets↔submeshes (sub-resource-id 0x20040000|K ↔ decoded packet runs); (c) derive group/selector→FileCode, VALIDATE vs out/z143_pktfc oracle on visible packets, extend to all. The savestate EE-RAM pipeline generalizes to every room.

## 2026-10-16 (cont. 5) — CONCLUSION: wall binding is runtime-draw-only; FED models are EFFECTS, not walls

Deep dive into the savestate's live FED models (fully reversed the chain: group→selector→matdesc+0x24 tag lists→ mode0 `ctx+0x24[slot]` handle / mode8 shared-global; handle +0x30=TBP0, +0x58→TIM2 name; model texture-name list = FED **t68** @+0x68, cnt@+0x44, array of ptrs→TIM2 desc, name at +0x50).

**Decisive: extracted the t68 texture-name list of ALL 24 loaded FED models. NONE reference the wall atlases (z143k_*/z143h_*).** Every z143 FED is an EFFECT/FIXTURE model:
- 0x20040000/0x20000000/0x20240000: lamp, light, flash, moku, uvsmk_07, z143_eff_mako
- 0x20140000: madowaku(window frame), water_13, shibuki2_2(splash)
- 0x201c0000: nami_04/05(waves), sibuki_01, moku, moya_03(haze)

So the ROOM WALLS (068f0N12 geometry, textured with the 18 z143k_*/z143h_* atlases resident in the TXD catalog ~EE 0x154xxxx) are drawn **without any FED material model**. Their per-packet texture assignment exists only in the transient runtime draw-command stream (param_3: group/sub), established per-frame at draw time. Exhaustively ruled out as static: not in the VIF (zero TEX0), not in any geometry/model/mapid field (scan_bij/fieldcorr: 0 hits over all 1803+2497 packets), not in loaded model structs (no FED binds walls; wall per-packet index isn't resident). This triply vindicates the original FORMAT.md methodology call.

### FINAL VERDICT on z143 room texture binding
- **SOLVED for everything the game displays:** the GS-dump UV-triple oracle binds 8020/15332 packets (52.3%) = all camera-visible wall surfaces, validated at corr~1.0, mechanism fully understood.
- **The remaining ~48% is geometry the game never rasterises from any reachable in-room camera** (back-faces, structural, far-LOD; chunks 0712/0d12 ~97% of it). It has **NO static binding anywhere** — only ever bound transiently IF drawn — so it is not recoverable from files or a savestate. True back-faces can never be captured.
- **GS dump is THE authoritative source** (as FORMAT.md always said). Savestate confirmed the mechanism + definitively closed the static-recovery question; it does NOT extend wall coverage (walls have no FED; wall binding isn't resident when not drawing).
- **For the enhanced free-camera port**, the only way to texture culled back-faces is a heuristic (e.g. inherit the front-face neighbour's material) — a cosmetic future enhancement, not a correctness gap.

Binding track = effectively COMPLETE. Artifacts: scratchpad/savestate/ (eeMemory.bin + scripts), scratchpad/z143_live_binding_result.json (full reversed chain + 3 active models' handle tables).

## cont. 6 — back-face texturing heuristic VALIDATED + shipped as tool (2026-10-16)

The predicted "inherit the front-face neighbour's material" heuristic for the
never-drawn ~48% is now built and numerically validated. Tool:
`native/assets/models/backface_heuristic.py` (+ `normal_variants.py`,
`probe_space.py` diagnostics). Reproducible from the project; writes
`out/z143_backface/{result.json,assignments.json}`.

- **Method that wins: plain centroid 1-NN over the BOUND set, global, k=1, NO
  normal gating.** k>1 (3-NN 68.5%, 5-NN 63.2%) and same-hemisphere normal
  gating (70.7%) both make it WORSE — they reach across material boundaries.
  Within-chunk ≈ global (75.1 vs 75.2) because the nearest bound packet is
  in-chunk anyway (chunks share one world frame — probe_space.py).
- **Accuracy (Dan's rule: numeric, 5-seed 20% hold-out of oracle-bound packets,
  predict from the rest, compare to oracle truth): 75.2% ± 0.5% exact-match**
  vs 26.3% majority-class baseline. Accuracy is FLAT with distance (78.7% @≤10u →
  75.2% @∞): the ~75% ceiling is how finely materials interleave on adjacent
  panels, NOT distance — so a cutoff buys COVERAGE, not accuracy.
- **Coverage of the 7314 unbound packets:** nearest-bound median 21u, 95th 74u;
  ≤50u=90.1%, ≤100u=96.5%, ≤200u=98.4%, ≤500u=100%. The two dark chunks are
  fully coverable (0712 n=1318 med 29u; 0d12 n=928 med 29u — flush against bound
  front-faces). 068f0012 is the only isolated set (274 unbound, 59% within 200u)
  → flag low-confidence for manual override.
- **67.5% of unbound packets have a bound packet within 60u with an ANTI-PARALLEL
  normal** — i.e. two-thirds are literally the back side of a textured wall whose
  correct texture is the co-located front face 1-NN already grabs. (Diagnostic;
  true back-face accuracy is unverifiable — no oracle for back-faces.)
- **Recommended cutoff 200u used as a confidence FLAG, not a filter** (`confident`
  field in assignments.json; `source='heuristic'` vs `'oracle'`).
- **VERDICT: SHIP as a labelled cosmetic fallback for the free camera ONLY.**
  75% vs 26%, ~98% coverage, trivial, errors land between visually-similar
  Mako-reactor atlases on surfaces retail NEVER rendered — any plausible texture
  beats black. **Never fold into the oracle-bound 52.3%; never call it recovered
  binding.** The engine consumes assignments.json: oracle fc where source=oracle,
  heuristic fill (optionally gated on `confident`) otherwise.

Binding track now TRULY complete: 52.3% authoritative (all camera-visible) +
validated cosmetic fill for the rest. Nothing further recoverable statically.
