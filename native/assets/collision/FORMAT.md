# CID map-collision format (Dirge of Cerberus, PS2) — reversed spec

Reversed from the game's own EE code (`/srv/games/dirge-port/analysis/decomp/`,
13,089 funcs) plus byte-level cross-checks against real samples in
`/srv/games/dirge-port/extract/_KEL/noPath/`. Every field below cites the parser
or collision-query function it comes from, or a byte pattern consistent across
files. Nothing here is guessed.

Coordinates are float32 world units in the same space as the map's visual BMD
(the exported OBJ overlays the BMD without an axis flip). **−Y is up** (gravity
is +Y): on z128 the sky dome (room_sky.mesh) spans y −1101..−356, Vincent's GS-dump placement (pivot
= feet = max model Y, no flip) puts his feet on the deck top at y=−98.44, and
y=0 lies 98 units below that deck. (Earlier revisions of this note said "Y-up";
that was wrong — corrected 2026-10-26, worker 0045.)

---

## 0. Container → CID payload

CID collision lives inside a `mapid.rfd` RFD container (FileCode `..0111`). The
RFD directory walker / relocator is **`FUN_002585e0`** (and the unload mirror
`FUN_002587e0`, load-time variant `FUN_002588c8`). Each entry is
`{ char name[8]; u32 ext; u32 size; }` with payload at `entry+0x10`; the
ext-int dispatch routes **`"cid"` = `0x646963`** to the CID relocator
**`FUN_00271208(entry+0x10, base)`** (`FUN_002585e0:41-43`).

Container layout (verified on `06010111.bin`, `06cc0111.bin`):

| off  | field                                             |
|------|---------------------------------------------------|
| 0x00 | container name `"mapid\0\0\0"`                     |
| 0x08 | container ext `"rfd\0"` = `0x646672`               |
| 0x0c | container total size                              |
| 0x10 | `u16` entry count, `u8` flag @0x12                |
| 0x14 | `u32` entry-offset table (count entries)          |
| …    | entries; the one with ext `0x646963` is the CID   |

For every shipped `mapid.rfd` the single entry is the CID and its payload begins
at file **0x30**. The loader stores the CID chunk pointer in the map-loader
object at **`slot+0x44`** (state machine `FUN_00266ae0` case 4, via find-by-tag
`FUN_00258f20(container,0x646963,0)`); the three loader slots are
`DAT_004e4c60 / 004e4cd0 / 004e4d40` (stride 0x70). During a query the payload is
cached in `DAT_004e88a0`. *(container→slot path confirmed by the query-code
survey.)*

## 1. CID payload header + version gating

`FUN_00271208` is a **version-gated relocator**; `payload+0x04` is the version.
It relocates pointer fields and node arrays only above certain versions:

```
if (ver >  2)   reloc ptr @+0x24
if (ver >= 4)   reloc ptr @+0x28
if (ver >= 9)   FUN_0026fd90(payload+0x38)   // node-array sub-tree
if (ver >= 9)   FUN_0026fd90(payload+0x58)   // node-array sub-tree
if (ver >= 0x10) FUN_0026fd90(payload+0x78)
if (ver >= 0x11) reloc ptr @+0x30
if (ver >= 0x12) FUN_0026fd90(payload+0x98)
if (ver >  0x12) reloc ptr @+0xbc + array of (payload+0xb8) ptrs
```

`FUN_0026fd90` relocates a 0x20-byte sub-tree descriptor: `[+0]=nodeCount`,
`[+8]=vertexPool`, `[+0xc]=nodePtrArray`, `[+0x10]=grid`; it walks each node and
dispatches on the node tag (`0 → FUN_0026f300`, `2 → FUN_0026f518`).

**Version census (452 mapid files):** `v19 ×425`, `v18 ×14`, `v17 ×4`, `v9 ×6`,
`v2 ×2`. So two structurally different families ship:

* **v2 (2 files, tutorial/debug)** — inline quad-soup. `FUN_00271208` is a no-op
  for v2 (all branches are `> / >=` the version), i.e. the data is self-contained
  and uses file-relative offsets. See §2.
* **v9–v19 (450 files, all real maps)** — grid-accelerated, vertex-indexed tree
  with VU-packed leaves. This is what movement/physics actually queries. See §3.

All stored offsets in both families are **relative to the CID payload base**
(what `FUN_00271208` relocates by adding the runtime load address).

## 2. v2 — inline quad-soup leaves

Header floats (self-contained; not pointers at v2): `+0x20` root bbox centre
(xyz), `+0x30` root half-extent (xyz), `+0x1c` = offset to the post-geometry tail.
Geometry is a stream of records (a flattened tree); the geometry-bearing records
are **type-7 leaves**:

Leaf header (0x34 bytes):

| off  | field                                   |
|------|-----------------------------------------|
|+0x00 | centre.xyz, +0x0c = `1.0f`              |
|+0x10 | half.xyz,   +0x1c = `1.0f`              |
|+0x20 | `0`                                     |
|+0x24 | `faceCount`                             |
|+0x28 | `7` (leaf tag — constant)               |
|+0x2c | `0`,  +0x30 `0`                          |

Then `faceCount` **quads**, each 4 vertices (xyz float32) = 0x30 bytes, with a
`0x00000000` separator word between consecutive quads (the last quad omits it →
record stride 0x34; leaf size = `0x34 + faceCount*0x34 - 4`). Verified exactly:
on `06010211.bin` the single leaf is a 10×10×10 box = 6 quads at centre
(330,-300,0) half (5,5,5); on `06010111.bin`, 361 leaves / 2400 quads / 4800 tris,
bbox [-800..800, -200..0, -800..800]. **All 2039 inter-quad separators are `0`**
→ v2 carries **no per-face surface flag**; surface class is geometric/implicit.

Leaves are located by the signature scan `w(+0x0c)==1.0 && w(+0x1c)==1.0 &&
word(+0x28)==7 && 0<count<100000` (robust to the interior-node records between
leaves). The parser splits each quad `[v0 v1 v2 v3]` as `(0,1,2)+(2,1,3)`.

## 3. v9–v19 — grid-accelerated vertex-indexed tree

The runtime query path (confirmed end-to-end):

* Thin map API `FUN_0026a320`→`FUN_002714d0` (floor/segment ray; uses sub-tree
  `+0x58`), `FUN_0026a668`→`FUN_00270ff8` (segment; sub-tree `+0x38`),
  `FUN_0026a710`→`FUN_00270ed8` (nearest; sub-tree `+0x98`). Each gates on
  `*(payload+4) >= 5/8/0x12`.
* **`FUN_00270088` — uniform-grid DDA raycast.** The grid header at
  `*(subtree+0x10)`: `+0x00` cell size (float), `+0x04/+0x08` origin (x,z),
  `+0x0c/+0x0e` grid dims (shorts X,Z), `+0x10` cell-pointer array
  `cell[z*dimX + x]`. Each cell: `+0x00` item count, `+0x04/+0x08` cell Y-bounds,
  `+0x10` item-pointer array. (Grids verified: `06cc0111` +0x38 tree = 40u cells
  88×37; +0x58 tree = 60u cells 66×35.)
* **`FUN_00270038` — AABB slab reject** (VU0 `_vclip`): tests
  `(p - node+0x10) * node+0x30 ∈ [-1,1]` → node `+0x10` = **centre**,
  `+0x20` = **half-extent**, `+0x30` = **1/half-extent** (precomputed reciprocal).

Node header (both leaf kinds share the bbox block):

| off  | field                                                   |
|------|---------------------------------------------------------|
|+0x00 | tag (`0` indexed, `2` VU-packed)                        |
|+0x04 | face / packet count                                     |
|+0x08 | visit stamp (runtime scratch; `0` on disk)              |
|+0x10 | centre.xyz (+w)                                          |
|+0x20 | half-extent.xyz (+w)                                     |
|+0x30 | 1/half-extent.xyz (+w)                                   |
|+0x40 | tag-0: face records / tag-2: packet-pointer array       |

### 3a. tag-0 — vertex-indexed faces (DECODED)

`FUN_0026f750`: `faceCount = *(node+4)`; face records at `node+0x40`, **stride
0x20**. Each record: word0 low 4 bits = **vertex count** `(w0<<0x1c)>>0x1c`
(seen 3 and 4); ushort **indices** at record `+0x08`; each vertex =
`vertexPool + index*0x10` read as 4 float32 (x,y,z,w). `vertexPool = *(subtree+8)`
(the field `FUN_0026fd90` relocates). The ray-vs-convex-polygon test
`FUN_0026f578` computes the plane normal `N = (v1-v0)×(v2-v0)` (VU0
`_vopmula`/`_vopmsub`) and `t = dot(start-v0, N) / -dot(N, dir)` with an
edge-sidedness point-in-polygon test → the parser triangulates each polygon as a
fan from v0 (matching that winding). Verified: `06cc0111` 979 tag-0 leaves →
8380 tris, bbox [-1997..1490, -137..26, -641..808], 0 bad vertices.

The high bits `word0>>4` are carried through as the triangle's `surface` word
(see §5); their exact meaning for tag-0 is unconfirmed.

### 3b. tag-2 — VU/VIF-packed packets (DECODED)

`FUN_0026f8a0`: `packetCount = *(node+4)`; packet-pointer array at `node+0x40`.
Each packet: `+0x08` **surface-type mask** (`short`), `+0x0e` **QWC** (`short`);
the packet is DMAed to VIF0 and run through **VU0 microprogram `_vcallms(0xa90)`**
which does the ray/triangle math on-chip.

The packet is a self-describing **VIF0 code+data list**, so its geometry decodes
from the VIF stream alone (no VU emulation needed). Decoded layout, with the
int16 `/16` dequant confirmed (and independently corroborated by the model VU
micro's `ITOF4` int16 unpack in `.vutext`):

| VIFcode              | contents                                                   |
|----------------------|------------------------------------------------------------|
| `UNPACK S-16` ×6 @0  | header: `[Ntri, 0, surfMask, 0xffff, 0, QWC]`             |
| `UNPACK S-32` ×Np @6 | Np **per-polygon** words (Np = total polygon count); high short = per-face material/surface index |
| `STROW` + `STMOD 2`  | base position + difference mode (for the int16 verts)      |
| `UNPACK V3-16` ×Nv   | int16 vertex **deltas**; world = `(base + Σdeltas)/16`     |
| *(rare, ~1%)* `UNPACK V3-32` ×Nv | absolute int32 verts (no STROW/STMOD); world = `v/16` |
| `MSCAL`              | run the ray/triangle micro                                 |

Polygon reconstruction (exact on every map): `Nquad = Np − Ntri`, and
`Nv = 3·Ntri + 4·Nquad`. Vertices are polygon-major, **triangles first then
quads**: the first `3·Ntri` are `Ntri` triangles (3 consecutive verts each),
then `4·Nquad` are `Nquad` quads (4 verts each, fanned `(0,1,2)+(0,2,3)`).
Same Y-up RH space as tag-0 / the BMD.

**Validated over the whole shipped corpus** (449 v≥9 maps): 537,893 tag-2
packets, **100% decoded**, 3,869,888 triangles, **0 vertices outside their
leaf's AABB**, only `V3-16`/`V3-32` vertex encodings exist, 0.07% zero-area
source degenerates (skipped on emit). `06cc0111`: 2962 packets → 17,982 tag-2
tris (replacing the old 1637 AABB boxes); total 26,362 tris with tag-0. The
exact VU0 0xa90 image could not be isolated in `.vutext` (it is a VU0 program
uploaded outside the graphics VU1 blobs there); see
`port/vu/disasm/vu0_collision_micro.txt` for that analysis — the decode does not
depend on it. The parser keeps the exact AABB box as a fallback only for leaves
that yield no triangles (a handful of empty placeholder leaves in tiny maps).

## 4. Where collision results go (for the future physics layer)

Query output globals (set by `FUN_0026f750`/`FUN_0026f8a0`): `DAT_004e889c` hit
flag, `DAT_004e88a4` hit `t` / nearest dist, `_DAT_004e8810` normalized hit
normal, `DAT_004e8820/..` contact block, `DAT_004e8870` vertex pool,
`DAT_004e8874` hit face node. Getters: `FUN_0026f1d8/1e8/1f8/208/218/228`.
Character floor/edge probes (e.g. `FUN_00551e80`, `FUN_00552088`) build a short
vertical segment and call `FUN_0026a320(start, end, 0x18e)` — `0x18e` is the
surface-class/flags mask.

## 5. Surface classification (floor vs wall vs stairs)

**Not** a runtime normal-Y compare. Each tag-2 **packet** carries a
**surface-type bitmask at packet `+0x08`** (= header short[2]); a packet is
tested only if `(mask & ~DAT_004e8898) != 0`, where `DAT_004e8898 =
callerFlags | 0x100` (`FUN_0026f8a0:51`, mask set in `FUN_00271408`). So the
caller selects floor/wall/stairs by the flags word it passes (movement uses
`0x18e`). In addition, the packet's **per-polygon S-32 words** (VU addr 6, one
per polygon) carry a **per-face material/surface index in the high 16 bits**
(seen `1..5` on maps with a populated table; constant `0x0007` where the table
is empty). On a hit the material record is looked up in the **surface table at
payload `+0x20` (count) / `+0x24` (array ptr, stride 0x10)** by the index the VU
returns. The native decoder carries the full per-polygon word on each triangle
as `cid_tri.surface` (falling back to the packet mask when absent).

**Packet-mask test** (decoded, worker 0053, 2026-10-26; machine code
`0x26f934..0x26f960`: `nor t5,zero,v1` / `lh v0,8(a1)` / `and` / `beqz` skip): a
packet is tested only if `(mask & ~(callerFlags|0x100)) != 0`, so the caller's
flags are an exclusion set. `FUN_0026a320` passes `a2` (the flags) through
unchanged to `FUN_00271408`. Every character probe flag word is
`0x180 | (0xf minus one bit)`, so each probe selects exactly one **class bit**
of the mask:

| flags   | tests packets with | callers |
|---------|--------------------|---------|
| `0x18d` | bit `0x02`         | ground ray `FUN_002db1f0` (segment y−1 → y+100, returns the hit y), `FUN_002d68f0`, `FUN_003590c8`; `FUN_0054fc38` table indices 0,7,10,12,13 |
| `0x18b` | bit `0x04`         | `FUN_0054fc38` index 6 |
| `0x187` | bit `0x08`         | `FUN_0054fc38` indices 1,2,3,8 |
| `0x18e` | bit `0x01`         | edge probes `FUN_00551e80`/`FUN_00552088`, `FUN_00346fc0`, `FUN_003470d8`; indices 4,5,9 |
| `0x10f` | bit `0x80`         | `FUN_00264ee0` |
| `0`     | anything but `0x100` alone | index 11 |

(`FUN_0054fc38` is a virtual method, vtable entry at `0x58cb5c`; its flag table is
14 words at `0x58c990`.) So bit `0x80` does not exclude a packet: `0x0184` is
tested by `0x18b`. Masks with no bit in `0x0f` (`0x0080`, `0x0180`, such as the
403×347 plane at y=−86, 12 units under the start deck) are never tested by a
character probe. z128 class census (sub-tree +0x58, by area): `0x0a` (bits 1+3)
and `0x0e` are the visible geometry; `0x04` alone is 88% walls, including the
invisible blocker plane at the deck railing (`0x0104`, z=−1120.1, y −109..+10);
`0x80` alone is 97% floors. Which index the player's wall probe uses is NOT
decoded (it is behind the virtual call).

**Winding:** with normal `(v1−v0)×(v2−v0)`, most walkable tops have **n.y > 0**
(pointing +Y = down, into the floor); undersides/ceilings have n.y < 0. Checked
under Vincent's start point (deck top −98.44 n.y=+1, deck bottom −94.5 n.y=−1).
The winding is NOT consistent: the stairs at x≈−1430, z −705..−647 and parts of
the walkway rim east of the start are wound the other way. The game does not
care: the tag-0 ray/polygon test `FUN_0026f578` accepts the hit when all edge
signs agree in either direction (two-sided), and the ground ray takes the first
hit along the ray.

**Gaps:** the exact bit meanings of the surface mask and the
surface-table entry fields are still not enumerated (most shipped maps have an
empty table). v2 has no per-face flag at all.

## 6. What the native parser (`cid.c`) handles

* RFD unwrap → CID payload (or a raw CID payload), fully bounds-checked.
* **v2**: inline type-7 quad leaves → triangles (exact, validated).
* **v9–v19**: all four sub-trees (`+0x38/+0x58/+0x78/+0x98`); **tag-0 indexed
  faces decoded to triangles** via the vertex pool; **tag-2 VU/VIF-packed leaves
  decoded to real triangles** (int16/int32 `/16` positions, tris+quads, per-face
  surface word), with the exact AABB box kept only as a fallback for leaves that
  yield no geometry; surface-table size.
* Output: `cid_mesh { verts, tris(+surface,+source,+mask,+subtree), boxes,
  bbox }` (`mask` = tag-2 packet surface mask, `subtree` = 0..3 for
  +0x38/+0x58/+0x78/+0x98); OBJ export
  (`--boxes` adds any fallback AABBs) grouped by source (`inline_quad` /
  `indexed` / `vu_packed`).

## 7. Open gaps (summary)

1. ~~tag-2 VU-packed geometry~~ — **DONE** (§3b): decoded from the VIF stream,
   int16/int32 `/16`, validated corpus-wide (537,893 packets, 0 verts outside
   AABB, 3.87M triangles). Remaining sub-gap: the VU0 `0xa90` micro image itself
   is not statically isolated (not needed for the decode; see
   `port/vu/disasm/vu0_collision_micro.txt`).
2. **surface-mask / surface-table semantics** — structure known (per-packet mask
   at `+0x08`; per-face index in the high short of the per-polygon S-32 words),
   bit meanings not enumerated (most shipped maps have an empty table).
3. **v17/v18** — same family as v19; parsed by the same tree path but not
   byte-diffed against v19 field-by-field.
4. Interior (non-leaf) node fields of the v2 tree (child offsets) are skipped;
   the leaf signature scan recovers all v2 geometry regardless.

## 8. Web export: DCOL v1 (`cidweb`, `cid_query.py`)

`make z128web` → `out/web/z128_collision.bin` (gitignored: derived game data).
`cidweb <out.bin> <mapid.rfd>...` merges any number of CID files (z128 = the 13
chunk files `0680{00..0c}11.bin`; 00 and 0c are empty) and welds vertices by
exact float bits. Little-endian; every section is 4-byte aligned, so each can
be wrapped directly as a JS typed array.

| off  | type            | field                                        |
|------|-----------------|----------------------------------------------|
| 0x00 | char[4]         | `"DCOL"`                                     |
| 0x04 | u32             | version = 1                                  |
| 0x08 | u32             | nverts                                       |
| 0x0c | u32             | ntris                                        |
| 0x10 | f32[3]          | bbox min (x,y,z)                             |
| 0x1c | f32[3]          | bbox max                                     |
| 0x28 | u32             | nfiles merged                                |
| 0x2c | u32             | reserved (0)                                 |
| 0x30 | f32[nverts*3]   | vertices x,y,z (game units, **−Y up**)       |
| …    | u32[ntris*3]    | triangle vertex indices                      |
| …    | u32[ntris]      | surface word (per-polygon S-32 word, §5)     |
| …    | u32[ntris]      | info: bits 0–1 sub-tree (0=+0x38, 1=+0x58, 2=+0x78, 3=+0x98); bits 2–3 source (`cid_tri_source`); bits 8–15 input file index; bits 16–31 packet mask |

File size = `0x30 + 12·nverts + 20·ntris`. z128: 96,131 verts, 143,920 tris,
4,032,020 bytes.

**Player set** (what `cid_query.py` and `collision.js` use, worker 0053):
sub-tree 1 (+0x58) packets that some character probe tests, i.e.
`mask & ~(f|0x100) != 0` for f in `0x18d, 0x18b, 0x187` (= `mask & 0x0e != 0`)
→ 140,105 tris on z128. That is the earlier `mask & 0x80 == 0` set plus 112
far-off floor tris (masks 0x84/0x88/0x8c/0x8e). Floor = **|n.y| ≥ 0.55** in either
winding (two-sided, like the game; 0.55 takes the 45° ramps and the 55° walkway
bevels, n.y 0.57); ground = the first floor at or below the feet (smallest
y ≥ feet−step); walls = everything else, pushed out horizontally by a capsule.

Sub-tree 0 (+0x38, 3,625 tris, tag-0 indexed, mostly flat) is NOT used. The
character physics `FUN_0035a440` does collect it (`FUN_0026a548` → `FUN_00271668`
→ `FUN_00270938(+0x38, …, 2)`) and pushes a body sphere out of its faces, and
under the start point its deck (−98.64) matches the dumped feet slightly better
(0.02–0.13 vs 0.22–0.33). But as a floor it sits 1 unit above the visible
walkway east of the start and rises to 4.8 above it near x=−1252, where
sub-tree 1 matches the visible surface exactly. Its gameplay role is open.

`python3 -I cid_query.py out/web/z128_collision.bin` runs the self-test
(ground under the three known Vincent positions, 8-direction walk);
`… X Z [FEET_Y]` prints the ground under a point.

