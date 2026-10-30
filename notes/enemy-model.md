# Enemy model decode: e027 (worker 0115, 2026-10-29)

Enemy job 1 from notes/enemy-research.md. Files: `native/assets/models/export_web_enemy.py`
(exporter), `native/assets/models/enemy_dump_check.py` (dump check), tools/byo step
`enemy_e027` (+ 3 inputs in disc.py). Output (gitignored): `native/web/data/enemies/`
`e027.glb/json` (variant 0), `e027_v1.*`, `e027_v2.*`, `e027_local.bin` (r_gst local TRS,
vincent_local.bin format).

## Why it decoded 1 triangle

e027's draw sub-packets set `STCYCL 0x3000` (CL=0, WL=48) before a masked `V4-8 x48`
(cmd 0x7e). That is VIF **filling** mode: the unpack reads (num // WL)·CL + min(num % WL, CL)
= 0 elements from the stream. `render_skinned.parse_drawpackets` ignores STCYCL, consumed
192 bytes, landed in the UV data and stopped. Vincent's colour unpack is `STCYCL 0x0102,
V4-8 xN` with data, so his decode was never affected.
Fix: `parse_drawpackets_cycl` (new; old helper untouched) tracks CL/WL. On 07703201 it returns
the same 334 sub-packets as the old parser (`export_web_enemy.py selftest`). Vincent's
vincent.glb/json/vincent_local.bin md5 identical before/after (764f0845…, 18726784…, 2739a76d…).
The 2026-10-15 "separated layout" theory is not needed for e027: descriptors (enum_descs) are fine.

## Vertex colour (decoded)

FUN_005b0ee0 (called from FUN_005b2ef8 before a model's groups) sends VIF1
`STCOL 0x80,0x80,0x80,0x80` and `STMASK 0xaaaaaa0f`. Mask rows 1–3 = column register on
every lane, and the colour quadwords are cycles >= 1, so each vertex colour = 128 = 1.0.

## Variants = LODs (decoded draw rule; selector not decoded)

FUN_005b2ef8 draws only smd blocks whose s16 at smd +0x12 equals the object's byte +0x44b,
walking the smd list last-to-first. FUN_00259058 picks textures with the same byte
(rst entry mask bit). e027:

| variant | blocks | pool v | tris | texture |
|---|---|---|---|---|
| 0 | em027_l0 ×2 | 891 | 1500 | e027_1 256×256 (mask 0xe → only var 0) |
| 1 | e027_L01/S01 (+0x12=1) | 292 | 498 | e027lw_1 128×128 (mask 0x9 → var 1, 2) |
| 2 | e027_L01/S01 (+0x12=2) | 211 | 300 | e027lw_1 |

Open: who writes obj+0x44b (distance LOD?). The hero dump draws the nearest soldier at
var 0 and five far ones at var 2; var 1 is never drawn in the three dumps (unverified).

## Check against the GS dumps (last full frame, gsrender2.pick_frame)

Texture pages are identified by texels: dump TEX0 0x3a04/0x3a00 = e027_1 and 0x2c48/0x2c44 =
e027lw_1, mean |d| = 0.0.

| dump | variant | dump tris | UV-triple matched | instances |
|---|---|---|---|---|
| hero 163235 | 0 | 809 | 809 (100%) | 1 |
| hero 163235 | 2 | 752 | 752 (100%) | 5 (142–161 tris each; the rest culled) |
| walk 185126 | 2 | 735 | 735 (100%) | 5 |
| walk 185138 | — | 0 | no e027 draws in the dump | — |

Shape (pose-independent, bind-pose edge lengths vs dump world edges, w = 1/q):
dump/export scale 0.9986 / 0.9990 / 1.0004. Triangles that are **rigid** (one joint, weight 1):
hero var 0 median max-edge error 0.91%, 97% within 5%, 100% within 10%; far var 2 soldiers
2.6–3.5% (≥ 350 units away: GS precision). Triangles skinned across joints differ by pose.

Pose: the hero-frame soldier is not in r_gst (crouched, rifle out). A whole-body fit over all
r_gst frames leaves median 1.05 units. With the export posed per joint from the dump (rigid fit
per joint; check only, not animation), matched corners have median 0.058 units, 90% 0.97 units
(figure 18.8 units). The comparison image is made by `enemy_dump_check.py DUMP --png OUT`.
Mirrored texturing: most UV triples occur 2× or 4× (left/right halves), with no duplicate triangles.

## Winding (worker 0120)

`enemy_dump_check.py` collects the dump's ADC=1 triangles too (XYZ3, sent but not drawn) and takes
each matched triangle's dump screen corners in the glb's index order (corners paired by UV). The
mirror side (most UV triples name 2-4 export triangles with opposite winding) is resolved by
position: per instance, whole-body fit, then pose_from_dump, then nearest candidate (< 1 unit,
next >= 3x farther). Screen y down, cw = WebGL back face:

| dump | variant | drawn cw / ccw (ambiguous) | ADC cw / ccw (ambiguous) |
|---|---|---|---|
| hero 163235 | 0 | 756 / 2 (41) | 5 / 639 (38) |
| hero 163235 | 2 | 664 / 11 (31) | 2 / 635 (35) |
| walk 185126 | 2 | 646 / 5 (33) | 8 / 648 (28) |

So the page draws THREE.BackSide, as Vincent's. UV-unique triangles alone agree (drawn cw 8/0,
49/0, 57/1; ADC ccw 50/1, 37/2).

## Stand-ins (same as Vincent's, already in fidelity-audit.md)

- top-4 skin weights (three.js): 3 vertices of var 0 had a 5th bone (max dropped 0.077).
- FPS 60 assumed for clips.

## Rifle (worker 0128, 2026-10-30)

**Which weapon (D).** FUN_00315e90 (status; non-player: +0x60 bit 0x200 clear) -> for slots 0-3
FUN_00316058 copies the parameter set's (stage section 0) name at +0x44 / +0x4c / +0x54 / +0x5c;
FUN_00316270 loads it (FUN_00408b30), resets its matrix (FUN_005a6e98(0,0,0, w+0x10)) and attaches
it: FUN_005a7698(owner MP, w, slot + 0xf, slot). Nothing when set +0x92 bit 4. z128: records 44 / 45
(set 31) slot 0 = **w003**, slots 1-3 empty; 95 e027 records carry w003, 31 carry w004
(e027_weapons.json). Files (FUN_00249cb8): w003 = 077700c1 (models: marker, w003_S01 x4, w003_S02 x2,
5-bone skeleton) + 077700c2 (w003_1 256, w003lw_1); w004 = 07770101 / 07770102.

**Where (D, as Vincent's Cerberus).** FUN_005ab8f0 draws each attached child right after the body:
world = owner world x FUN_005ad508 = palette[bone] x T(marker xyz), with the owner's light rig.
e027 marker 15 = **bone 26** (child of the right hand 16; no skinned vertices), offset (0,0,0).

**Scale (D).** w003's smd blocks are version 3 (byte +4 = 3) and have no header float at +0x2c
(it is 0x10). The VU scale is per group: FUN_005b16c0 (version 3) / FUN_005b1260 (version > 3,
DAT_005b3bf8 from FUN_005b0ee0) load `*(group +0xc) + 4` with y = 32.0; positions x that / 4096.
On version-4 models it equals +0x2c / 2048 (e027 3.33525, Cerberus 3.152425, p000 3.465913), so
export_web_enemy.decode now uses `group_scale` for every model: e027 glb/json/local md5 unchanged.
w003 1.960384 -> rifle 10.1 u long.

**Checks (hero dump 163235, scratch scripts riflecheck.py / attachcheck.py, worker 0128).** The dump
draws w003 **var 1** (w003lw_1): 290 triangles, all UV-matched, all 99 export triangles covered; var 0
and w004 are not drawn. Six rifle instances: similarity fit scale 0.9925-1.0005, RMS 0.15-0.32 u.
Attach: whole-body fit of the nearest e027 (var 2, r_gst / r_gst_lk frames, UV-unique corners) then
the rifle through that pose's bone 26: median 0.09 / 0.06 / 0.04 / 0.12 u, max <= 1.04 (four
soldiers incl. #66 and #67); one has 6 body corners (0.80), one has no body on screen. Winding
(UV-unique drawn tris, export index order): cw 145 / ccw 14 -> BackSide.

Inputs for tools/byo/disc.py INPUTS (not added: outside this job's files):
077700c1 3651ff2ee1a90b9ff2c40de1078d9a1d4d829c31c8edb178a100792c082fed4e,
077700c2 b3872f96ba1ca583b327c0edcd0279638928356d0acc734f4e2d88fecbac11a5,
07770101 abddbc0ed50cdcf956cf807ffde04258e9a21fb0916f1168376384e7e6a3aab9,
07770102 241f2c235d69e0b2f04b132279aff9d0d889527220348da94337b029c8387dfd.
