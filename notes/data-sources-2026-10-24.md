# Data sources that would replace the per-scene dump patches (2026-10-24)

Read-only investigation. No tracked files edited. Probe scripts are in
`/tmp/claude-1000/-srv-games-dirge-port/e52e900c-0df1-4080-ae73-7ac57e2ac891/scratchpad/`
(probe_*.py, run with `python3 -I`, hero dump decompressed there as hero.bin).
Answer keys: hero dump 163235 (last frame, FBP 224), `out/z128_pktfc_{union,v3}` per-packet
FileCodes, and the hand patches in export_z128_room.py / p000_pose.py / export_z128_sky.py.

## Ranking

| # | Source | Evidence it's on disk | Job | Fixes across all levels |
|---|--------|-----------------------|-----|-------------------------|
| 1 | Material flags + fog/light (4) | **Found** (BMD header env block; ABE bit already decoded) | Small | Fog, alpha test and light rig for every room. Removes ALPHA_TEST and the fog gap |
| 2 | Animation (1), cloth separate | **Found** (112 gameplay clips, rig matches p000) | Small–medium (clips); medium–large (cloth) | Pose for any frame, every character on this rig. Cape still needs cloth or a dump |
| 3 | Visibility/LOD (5) | Structure found (per-chain bbox trailers, pointer lists, octree) | Medium | Removes EXCLUDE_PACKETS; tells which grey geometry is never drawn |
| 4 | Static binding table (2) | **Not found** (now narrowed to one texture per *chain*) | Large, uncertain | Biggest payoff (≈50% grey) if it exists. Interim: chain fill +3.9 pts |
| 5 | Sky model (3) | Not found | Small to locate, if it is where I think | Sky in outdoor levels |

**Do first: #1** (cheapest, applies to every room, and two of its fields already match the dump
exactly). **Then #2** (clip decode is mostly existing code, and the dump pose is a ready answer key).

---

## 1. Material flags, fog, light (question 4): in the files, two places

### 1a. Alpha test / blend: one global rule keyed on the packet's ABE bit
- The ABE bit is already in each packet's template PRIM (`decode_packets` → `abe`).
- Hero frame, main framebuffer, textured tris, grouped into passes (probe_runs.py):
  - tris 44–4039: **all opaque room** (3996), ATE off.
  - 4040–4205: sky TBP 11504, additive (Cs·As+Cd), ATST NEVER + AFAIL FB_ONLY (no z write).
  - 4206–4357 and 5302–5408: **every room ABE tri (259/259)**: ATE GEQUAL 96, AFAIL RGB_ONLY,
    (Cs−Cd)·As+Cd, over TBPs 10756/10792/10828/10960/11220/11288/11800.
  - 4358–5301: TBP 11836 additive, 944 tris (effect; probably the flames; not room).
  - Then characters and effects (other refs, 64 and 100), post, clouds 14272/15872.
  - 16310–16867: 532 ABE tris with ATE **off** on TBP 0/10792/10828/10960. These are
    **HUD sprites** (FST=1, PRIM sprite, drawn last) reusing VRAM pages.
- So the handoff's "0680ff30/28 have a few ABE draws with ATE off that the BMD can't tell
  apart" is a TBP-reuse misattribution. In this frame every room ABE packet takes the same
  test and blend, drawn after opaque + sky. The per-FileCode `ALPHA_TEST` table can become
  "ABE packet ⇒ alpha-test pass". This doesn't contradict the 0680ff36 note: those packets
  are non-ABE.
- Gap this would close: ABE packets currently bound to FileCodes *not* in ALPHA_TEST are drawn
  opaque: 0680ff1f 148, ff48 108, ff2d 71, ff27 44, ff34 14, ff35 12, ff85 11, ff3d 8,
  ff24 5 (≈420 packets), plus 2423 unbound ABE packets.
- UNVERIFIED: AREF 96 for rooms in other levels. One frame, one level. Check with one dump
  from another level before going global.

### 1b. Fog + light: environment block in every BMD header
- Bytes 26 26 2f (= FOGCOL 38,38,47) sit at a fixed offset in all 13 z128 room BMDs. Layout,
  relative to the `BMD\0` magic (two blocks per file, at +0x2c and +0x588):
  `rgb, rgb, [rgb], vec3 (unit vector: light direction), 0, f32, f32, rgb (fog colour), 0, f32, f32`
  - z128 chunks 00–02: `(84,90,96) (48,52,64) dir(0.094,0.983,0.157) 0 1135 (38,38,47) 0 81 51`
  - z128 chunks 03–0c (2nd block): `(128,128,128) 0 dir(-0.494,-0.650,0.578) 25 500 (38,38,47) 0 81 51`
  - Other levels differ, as they should: 06960212 fog (102,46,43) far 1000; 068f0112 fog
    (193,214,255) far 1000; 06080112 (32,48,52) far 135; 06a00112 (255,255,255) far 2000.
- Checked against the dump (probe_fog2.py, 53k fogged vertices): fog colour = 38,38,47 **exact**;
  fog floor F = **51 exact** (w ≥ 800 all 51, and the header's last float is 51.0). The ramp
  is F≈245 at w<50 down to 51 at w≈620 (medians: 182 @200–300, 145 @300–400, 82 @500–600).
  Neither 25→500 nor 0→1135 fits that ramp directly. **Open:** what 81 is, which of the two
  blocks is live, and whether w needs a scale. F's upper plateau is 245, not 255.
- Light: the unit vectors and colour pairs look like the directional light + ambient that
  STATE-OF-THE-PORT says are "runtime-only, placeholder rig". They may be the level default
  (scripts also call KerberosLight.set_* thousands of times, so scripts can override them).
- Job: small. Parse ~12 words, fit the fog formula against the hero dump (one evening), add
  fog to render_scene_textured (the shader model already has F=clamp(a·w+b)).
  Effect: fog on far walls/battlements in every room; removes a per-scene constant.

## 2. Animation and cloth (question 1)

### Clips: found, and the old "rig mismatch" blocker doesn't apply to p000
- Vincent's container set: `07700000` (49 `c_*` clips: ladder, squat, …), `0770000b` (63
  `h_*`: h_wk, h_rn, h_st… = handgun), `0770000c` (63 `m_*`), `0770000d` (63 `r_*`),
  `07700011` (14 `bv*`). Their names match the player state machine in `06800009` ("bmd0":
  h_st, h_wk, c_ld_*, h_jp*…).
- Header = FORMAT.md §9.2 exactly (`+8` bones = **93**, `+0xc` frames, `+0x10` segs, `+0x14`
  seg table), but `+4` version = **0x13**, which FORMAT.md §9.1 and `anim_eval.find_clip`
  treat as "mesh". p000's own `skeleton.skd` (07700001) also reports **93** bones.
- Self-validating walk (P1, P2, Q tracks × 93 bones): **112/112** clips end 0–15 bytes before the
  next entry (alignment padding) (probe_clips.py). So the §9 decoder reads them unchanged.
  FORMAT §9.1's "14090 v0x13 = meshes" count therefore includes clips; recount.
- §9.6's blocker (event clip km0102vn on the event rig vs runtime-only gameplay rig) was about
  `km*` clips. p000 ships its own skd + clips with matching bone counts.
- Answer key: p000_pose.py's 57 solved bones / 1312 dump-positioned vertices for 163235. Search
  clip × frame (h_wk 40 f, h_rn 20 f, …) for the lowest bone-rotation error. A good fit
  proves the decode end to end and replaces the dump pose.
- Job: small–medium. Drop the version gate, compose with skd_skeleton.py, skin with the
  existing smd path. Unknowns: root motion/scale, blend between layers (`char+0x380`).
- Also in 07700001: `texanm` (tad) = texture animation, `marker` (mkd/mk2) = attachment points
  (gun, effects). Both are undecoded and small.

### Cloth (cape): runtime simulation, parameters not located
- The cape block (smd `p000_L02` @0x19f70) has 0 skin batches; a second `p000_L02` entry has
  extra non-zero header words (`0x52001c, 1`), maybe cloth parameters (unverified).
  Solver: `FUN_005b2550` → `FUN_005b0d30` (`FUN_005b0b38/09f8/0c50`), distance-constraint
  verlet, anchor `char+0x44` (FORMAT §7.5, §9.5).
- Job: medium–large (decomp the solver + find anchors/constraints). A sim never matches a
  single frame exactly, so keep the dump cape for 163235 and use the sim elsewhere.
  Interim: pin the cape to its nearest bones.

## 3. Visibility / LOD (question 5)

- **Unit = chain** of 1–10 draw packets. Each chain ends in a **176-byte trailer**: centre +
  8 box corners (V4 floats, w=1) + 2 int qwords: w0 (count), w2, w5 (offset), w6 hi (0–254 or
  255, looks like a node index; 255 = none?). probe_blk.py / probe_trl.py.
- Trailers are referenced (7453/7500) from qword pointer lists `{ptr,0,0,0}`. The zero slots
  look runtime-filled. Plus the BMD 8-way tree at header+0x20 (`FUN_0026bf88`, an octree),
  and node arrays A/B (FORMAT §3.3: "only partly reversed").
- So the game frustum-culls per-chain boxes via an octree. That explains "in view but not
  drawn" only if the box or node says so. The 4 hand-excluded packets (06800a12 p29, p30–31;
  06800c12 p70, p71) are their own chains, but nothing tested in their trailers (w2 lo, w6)
  differs from drawn chains. **Not explained yet.**
- `0680NN10` (120 B–12 KB, empty in a10/c10) is NOT visibility. It's small records with
  keyframe times (0.1/0.2/0.3/0.4) and a runtime stub `0x0022dd00`: likely material/UV
  animation (flicker/scroll). Worth a look for the sky/cloud scroll.
- Job: medium. Walk the node graph and octree, list chains per node, then test against the dump:
  for the hero camera, which in-frustum chains are not drawn, and what do their nodes share?
  Payoff: removes EXCLUDE_PACKETS for every level. It also splits the grey ≈50% into "never
  drawn from anywhere" (needs no texture) and "drawn but unbound".

## 4. Static material/texture binding table (question 2)

Prior work (notes/session-2026-10-15-gsdump.md, cont. 2–5) searched hard: VIF stream (no TEX0),
mapid = collision, FED = effects only (no wall atlases), EE-RAM savestate (no resident wall
binding). It concluded "runtime draw stream only". New today:

- **The binding unit is the chain, not the packet.** Adjacent bound packets in the same chain
  share a FileCode 4042/4081 (**99%**); across a chain boundary they differ 2350/2615 (**90%**)
  (probe_within.py). The old exhaustive scans searched packet-indexed arrays, the wrong unit.
- Ruled out at chain level today: every trailer word/half (best non-unique purity 0.48 for w2 lo;
  baseline 0.19), the words around each chain's pointer-list entry (≈0.2), and the packet
  template / pre-packet words (probe_hdr.py, ~0.2). `eib` in 06800011 is 3 KB, chunk 00 only.
- Most likely remaining place: whatever fills the `{ptr,0,0,0}` slots or emits TEX0 between
  chains at draw time. That means a decomp trace of the BMD draw path (`FUN_0026bf88`'s +0xb0
  draw templates, `FUN_0026bc28` node array B), not the FED path `FUN_002e62b0`. Large and
  uncertain, so don't start here.
- **Interim win, all levels, no decode:** chain fill (give unbound packets their chain's
  FileCode). z128: bound tris **46.9% → 50.8%** (union) / 47.0 → 51.2% (v3); 744–774 packets.
  The same rule audits heuristic bindings: the 39 within-chain disagreements are where to
  look for wrong deck-fill/seed+grow bindings.
- Vincent's palettes/grey were a *character* binding issue (smd groups, 4 CLUTs). This table
  wouldn't fix them; it's for rooms (grey surfaces, wrong bindings like p370).

## 5. Per-level sky model (question 3)

- Sky textures exist per level (58 files with kumo/sky/sora names; z128: 0680ff35
  `z128_a_kumo1`). Nothing references them by name; binding is by slot.
- Dome geometry: not in room BMDs by UV (export_z128_sky.py: 1/165) **or by position**. The only
  room packets beyond 4000 u are 20 in 06800012 (radius 6.8k–11.7k), but they're non-ABE,
  vcol 69, texture 0680ff6f: a distant backdrop, not the additive dome.
- The 12 room packets v3 binds to 0680ff35 (06800212 p37–47, 06800912 p171) are small ABE
  objects **at the origin** (local space), orange vcol (127,70,35) with varying alpha: an
  instanced prop/effect, probably flames, not sky. So the 0680ff35 binding there is likely
  wrong. It also shows that some BMD chunks hold local-space props placed at runtime: that's
  more placement data we lack.
- Candidates: a camera-centred dome in a shared FileCode (search by topology: 166 tris, ring
  structure), a FED model, or the zone files 06800001 (`bzd048`, 1.27 MB, undecoded) /
  0680fd00–02 (`map` TIM4, 3×263 KB). Clouds (TBP 14272/15872) are a post layer.
- Job: small if a topology search hits; medium otherwise. Payoff: one sky per outdoor level,
  but it's one draw. Rank last.

## Answer-key summary

| Patch today | Replaced by | Checked here |
|-------------|-------------|--------------|
| ALPHA_TEST per FileCode | ABE bit ⇒ GEQUAL 96 / RGB_ONLY / (Cs−Cd)As+Cd | 259/259 room ABE tris, hero frame |
| Fog (not done) | BMD env block | colour exact, floor 51 exact, ramp not yet fitted |
| Vincent pose from dump | p000 clips (h_*/m_*/r_*/c_*) | format lock 112/112; pose fit not run |
| Cape from dump | cloth solver | not attempted |
| EXCLUDE_PACKETS | chain bbox / octree / node flags | structure found, 4 packets unexplained |
| per-packet binding + overrides | chain-level key (not found); chain fill now | 99% / 90% chain rule |
| Sky dome from dump | unknown source | ruled out room BMDs by position |
