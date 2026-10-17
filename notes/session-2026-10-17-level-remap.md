# Session 2026-10-17 (pt 6) — LEVEL BASIS RE-MAPPED: sea-of-flames = z128 + p000

## The correction (this supersedes the z143/p005 framing for the sea-of-flames scene)
Dan provided a `sea-of-flames.p2s` savestate + 8 multi-angle GS dumps
(`/srv/shared/dirge-port/save-states/sea-of-flames-gs-dumps/`, SLES-54185) and
flagged that prior work had the wrong level. Re-mapped from the clean data:

- **Environment = zone z128 (FileCode `0680`), the burning Kalm castle-town** — NOT
  z143 Mako Reactor (`068f`). Confirmed: the dump's castle brick + slate-roof
  textures correlate 0.95-0.99 with on-disk `tex_0680*` `z128_a_*` atlases (Dan
  visually confirmed). z128 is already decoded on-disk.
- **Vincent = p000 (geom `07700001`, tex `07700002`), NOT p005 (`07700141/2`).**
  Confirmed: his cape texture matches `p000_3` (corr 1.000) and body matches
  `p000_1_a` (256x256, vs p005's 256x512); and **p000 geometry matches the
  sea-of-flames dump at 39% UV-triple identity vs p005's 1%.** p005 was the *Mako*
  Vincent. (The two are different models; p000 is lower-poly, 1699 tris.)
- The scene also contains Deepground soldiers (`0x2c48` armour).

## Method (reusable)
- Identify zone/character: correlate dump textures (decode via `tools/gsdump` TexCache,
  clean per-draw CLUT; `gstex_decode` garbles CLUT atlases) against on-disk decoded
  textures (`match_tex.py`, or dtex on character containers `0770xx02`).
- Per-group binding: `native/assets/models/char_dumpbind.py` matches a model's
  UV-triples (raw/4096) to dump draws' UV-triples (s/q, t/q). Point it at p000 by
  setting `render_p005.CONT/TEXC`.
- Decode geometry with the SOLVED decoder `render_skinned.assemble` (= `/2048`
  "inflate thickness" scale, from `DECODER-STATUS.md`). `render_p005.decode_groups`
  gives the same geometry + group IDs (needed for binding).

## Vincent p000 — state
`native/assets/models/render_p000_dumpbound.py` renders p000 with its own atlases,
per-group binding from the dump. Pitfall fixed: the `uv_span>0.35` filter dropped an
arm on this low-poly model — removed it; both arms present. Groups: body→`0x2cd4`
(p000_1_a), hair→`0x2de4` (p000_2). **The long red cape is NOT in p000's skinned
mesh** — it's a separate non-skinned object (p000's container set `07700000`=m000
RFD of 49 MOD meshes, `0770000a-f` = no-skeleton MOD; the cape is a MOD/cloth mesh,
a separate decode). So Vincent renders complete (body/mantle/legs/hair/arms/pouch)
MINUS the flowing cape. Cape colour also reads dark (atlas is dark-red + software
shading); the game lights it.

## z128 room — state (agents)
Camera for frame 163235 recovered by DLT+RANSAC-PnP (0.03px reproj): the game uses an
**off-center/anamorphic projection** (intrinsics fx=300 fy=450, cx=213 cy=256 in
640x480). Reproduced via an oversized symmetric cover-frustum + exact crop (the
`-W/-H` additive flags in `render_scene_textured.c`). Coverage **47.8%** (union of
all 8 dumps; the static in-file binding table is not safely decodable yet → unseen
surfaces stay grey, honest). Builds: `out/z128_room_v2/`, binding
`out/z128_pktfc_union/`, renders `out/z128_room/renders/attempt_03/`.

## Scene composite
`/srv/shared/dirge-port/level-remap/11_FULL_SCENE_vs_game.png` — camera-matched z128
room + p000 Vincent vs game 163235. Room aligns (roof/railing/battlements/tower);
Vincent recognizable but lacks the cape; 2D composite (not in-engine).

## Next
1. Cape: decode the non-skinned MOD cape mesh (FORMAT.md §2.2) + texture p000_3, OR
   cloth-sim / dump-snapshot for a frame-match.
2. In-engine 3D placement of p000 at the recovered camera (vs 2D mock).
3. Room coverage: needs the static binding table or more angles; cape colour/lighting.
See [[session-2026-10-17-convergence]] for the general runtime-binding context.
