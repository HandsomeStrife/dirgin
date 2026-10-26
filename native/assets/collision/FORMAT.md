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

### 3c. tag-1 — oriented primitives (DECODED, worker 0055)

Tag-1 nodes were skipped by `cid.c` until worker 0055. Each holds one primitive
(z128: 247 in sub-tree 0, 996 in sub-tree 2; sub-tree 3 holds 112 tag-3 nodes,
still not decoded). Decoded from `FUN_00270938` (broad phase: shape type at
`+0x40`) and `FUN_0035dcf0` (the character's closest-point test):

| off  | field                                                      |
|------|------------------------------------------------------------|
|+0x10 | AABB centre / +0x20 half / +0x30 1/half (as other nodes)   |
|+0x40 | shape type                                                 |
|+0x44 | class word (sub-tree 0: 1, 2, 3, 4, 0xc, or 0x5a9c94; sub-tree 2: always 0x5a9c94) |
|+0x48 | flag word; bit 0 set = `FUN_0035a440` skips its ground test |
|+0x60 | 4x4 matrix, unit shape -> world (row vectors: `[x y z 1]·M`) |
|+0xa0 | the inverse matrix                                         |

Unit shapes (radius/half-size 10): type 0/1 sphere (`FUN_0025dae8`), type 2 box
`(-10,-10,-10)..(10,10,10)` (`FUN_0025cf00`), type 3/4 radius 10 around local Y
over y −10..10 (`FUN_0025d1b8`; the AABB bounds y at ±10 scaled, so the ends are
flat: a cylinder). `cid.c` tessellates them (box 12 tris, cylinder 16 sides +
caps, sphere 8×16) with `source = 3` (`CID_SRC_PRIM`), `mask = class & 0xffff`,
`surface = flag`. Tessellated boxes match their node AABBs exactly.

Who uses them: sub-tree 0 → the character physics `FUN_0035a440` via
`FUN_0026a548` → `FUN_00271668` → `FUN_00270938(+0x38, …, 2)` (2 = tag-1 only);
it pushes the body sphere out of them and, for flag bit 0 clear, tests them as
ground within 40°. Sub-tree 2 → `FUN_0026a5d8` → `FUN_002716c0`, called only from
a physics-prop class (`0x406378`, `0x407578`; objects built by `FUN_00587b10`
as box/sphere/capsule bodies). On z128 sub-tree 2 holds the deck slabs and every
railing (the north, west and south rails of the start deck are boxes at
z −1087.1, x −1425.9, z −1116.9, 10 units tall) — and nothing else on those
railings exists in any sub-tree.

### 3d. The sub-tree-0 tag-0 faces are the character's walk mesh (DECODED, worker 0061)

**Which movement mode z128 uses.** `FUN_0034db80(char, mode)` turns the walk
mode 10 into mode **0xc** when script flag bit 13 (`DAT_0046e298`) is set (and
ignores the fall mode 0xb). `FUN_0053c140` sets bit 13 from the stage record
`FUN_00311038()` = section 4 of the stage file (`06800001.bin`, record at
0xa6630), field +0x44 bit 0, which is **1** in z128. (Only the script native
`FUN_00326660` could clear it later; not traced. `FUN_003222a0`, the other
setter, has no caller.) Mode 0xc (`FUN_0034c388` case 0xc) runs the physics
body at char+0x3b0: `FUN_00358fb8` then `FUN_003590c8`, a state machine on
body+0xa8 (0 ground, 2 fall, 1/3/4-7 others). Every state moves through
**`FUN_00357d48`**. So worker 0057's walk-mode-10 rules (`FUN_003539b0`: 7.5 step,
0.2 follow, 5 below, 7-unit wall ray) are real but not what z128 runs; the
40-degree ground test (`FUN_0035a440`) is only reached when body flags +0xac
bit 1 or 2 are set (not resolved).

**Face record** (0x20 bytes, sub-tree +0x38 tag-0 nodes):

| off  | field |
|------|-------|
|+0x00 | word0: low 4 bits vertex count (3 or 4); high bits not used by the walker |
|+0x04 | u16 flags (below) |
|+0x06 | u16 (not decoded; z128: 1, 2, 3, 0xc) |
|+0x08 | u16 vertex index x4 into the sub-tree vertex pool |
|+0x10 | neighbour pointer per edge v[i]->v[i+1] (payload offset, relocated by `FUN_0026f300`); 0 = boundary edge |

z128: 3,186 polygons, 7,288 links (all reciprocal, never across files), 2,709
boundary edges. The body keeps its polygon at body+0x110 and the vertex pool at
+0x114; `FUN_0035c918` caches the face plane (+0x120, `FUN_0035d0a0`) and the
edge planes (+0x130.., `FUN_0035d2e0`: edge j through v[j], v[j+1] and v[j] + N,
N = (v1-v0)x(v2-v1), positive = outside).

**`FUN_00357d48(body, move)`**, up to 8 passes:
1. Position outside the polygon (`FUN_002605c8`: any edge plane > 0): snap to
   the closest point on its edges (`FUN_0035d788`).
2. Ground states: project the move onto the polygon plane along the up axis
   (`FUN_002606b8`) and rescale it to its original length (so a slope slows the
   horizontal speed). States 1/3: remove the normal part instead.
3. End point inside: move there (projected onto the plane, `FUN_00260628`), done.
4. Else the first edge plane crossed (`FUN_0035d4e8`: start <= 0.001, end > 0,
   smallest t). Neighbour across it (not the polygon just left): move to the
   crossing point, switch polygon, carry on with the rest of the move.
5. No neighbour (or refused, below): stop at the crossing point; the rest of the
   move is projected onto the edge direction (`FUN_0025a778(0, ...)`), plus
   -0.01 x its length along the edge plane normal (back inside); the character's
   hit-wall bits are set (char+0x1c0 |= 0x400; 0x40000|0x80000 when the move
   met the edge at 45-135 degrees). The fall state does the same flattened to XZ.

Flags (on the neighbour unless noted):

| flag | rule | z128 |
|------|------|------|
| 0x80 | a wall for bodies with flag +0xac 0x200 (char+0x45c; set by `FUN_00559748` from a controller's +0x28 bit 2, not resolved for the player) | 146 polygons, incl. a stair series every 18 units from y -106 to -16 |
| 0x10 | ground states (`FUN_00357798`): the neighbour's shared edge plane (`FUN_0035d0e0`) with n.y < 0 (pointing up) -> enter and start falling (`FUN_00357148`); else a wall. Entering 0x10 from a polygon without it first casts a short probe for raised 0x10 polygons (`FUN_0035cca0`, `FUN_0035cef0`; not ported) | 131: the ladders (ladder 35 is polygon 25, x -1106..-1109.7, joining the landings at y -90.6 and -146: a wall from below, a drop from above) |
| 0x02 | on the CURRENT polygon, at a boundary edge (0x358c60): `FUN_00359f38(body, &done, end)`, `end` = pass start + move (stack slot built at 0x358350; worker 0062 read the MIPS, the decompiler lost the arguments). `FUN_0035cbc8(out, hit, end, 0)` casts from 7.5 above (`DAT_004a2178` = (0,-7.5,0)) to 200 below over the tag-0 faces of every loaded file (`FUN_0026a668` -> `FUN_0026f750`: smallest t, **no flag test**). A polygon other than the current one: the body goes to the hit point on it, `done` = 1 ends the pass loop (the rest of the move is not wall-tested); more than 3 lower than the crossing point -> fall (states 1/3: `FUN_00356fd8`; state 2: nothing). None: the edge is a wall | 22: 10 seam pairs across chunk files (seam drop 0-1) plus 1669 and 2459 one polygon behind a seam |
| 0x100 | ground states: polygon search 8.5 below the end point (`FUN_00359f38`) | none |
| 0x04 | ground states: shared-edge n.y < 0 -> `FUN_00357628` (state 1), else wall | none |
| 0x40 | a wall unless body flag +0xac bit 3 | none |

**No polygon** (body+0x110 == 0): `FUN_00357d48` skips the walk entirely, so the
body does not move horizontally. The ports do the same for an airborne move off the
mesh; the game's airborne body never has no polygon (it keeps the take-off one).
The ports' polygon searches (`find_poly`/`nearest_poly`) skip polygons the walker
refuses to enter (0x04/0x40/0x100, 0x80 for block80); `locate` (= `FUN_0035cbc8`)
does not.

In the fall state (2) the body's ground point keeps walking the mesh (with no
0x10/0x100/0x04 rules), the visual y follows the arc, and it lands when the y
passes the polygon (`FUN_003590c8` state 2). **A jump cannot leave the walk mesh
either.** So the railings, the deck edge and the "flat layer over the pipes"
are all the walk mesh: the deck polygon is x -1417.9..-1355.7, z -1113.4..-1094.8
at y -98.6 (5-10 units inside the visible deck edge), and a 4.2-wide strip at y
-94.4 runs over the pipes. Vincent's three dumped feet lie 0.02-0.13 off it.

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
`0x80` alone is 97% floors. Who tests what (worker 0061, callers of
`FUN_005449f0` by index): index 6 (class 0x04) = `FUN_0032ace0` (the camera) and
`FUN_003a6000`, so the 0x04 blockers are camera collision; class 0x80 is tested
only by `FUN_00264ee0`, a ray 100 down from the camera whose hit surface word
becomes the room visibility mask at 0x004e8710 (`FUN_00269400/410`): the y=−86
plane is a visibility zone, not a kill plane. Walk probes 4/5 (`FUN_003539b0`)
use class 0x02 when `DAT_0046e298` bit 0 is set (it must be in z128: class 0x01
has only the 10 ladder faces). The probe method is vtable slot `+0x17c` of the
character class (vtable `0x58c9e0`). `FUN_0035a440` calls it with index 5
(`0x18e`, class `0x01`; `0x18d` when bit 0 of `DAT_0046e298` is set) for its
object-riding probes; other callers pass 0, 3, 7, 8, 0xb, 0xd. Walk mode 10 has
a horizontal wall probe (`FUN_003539b0`, index 4, 7 + speed ahead at 7.5 and 17
above the feet), but z128 runs mode 0xc, whose walls are the walk mesh's
boundary edges (§3d). In `FUN_0035a440` (reached from `FUN_00357d48` only with
body flags +0xac bit 1 or 2) ground hits count only within **40° of up**
(`acos(up·n) < 0.69813174`, four sites; `FUN_00261038` = angle between vectors).

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
  yield no geometry; **tag-1 primitives tessellated** (§3c, source 3); surface-table size.
* **Walk mesh** (§3d): every sub-tree-0 tag-0 face is also kept as a
  `cid_poly` (vertex indices, +0x04 flags, +0x06 word, neighbour pointers
  resolved to polygon indices; dangling pointers are counted and left as walls).
* Output: `cid_mesh { verts, tris(+surface,+source,+mask,+subtree), boxes,
  bbox }` (`mask` = tag-2 packet surface mask, `subtree` = 0..3 for
  +0x38/+0x58/+0x78/+0x98); OBJ export
  (`--boxes` adds any fallback AABBs) grouped by source (`inline_quad` /
  `indexed` / `vu_packed` / `primitive`).

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

## 8. Web export: DCOL v2 (`cidweb`, `cid_query.py`)

`make z128web` → `out/web/z128_collision.bin` (gitignored: derived game data).
`cidweb <out.bin> <mapid.rfd>...` merges any number of CID files (z128 = the 13
chunk files `0680{00..0c}11.bin`; 00 and 0c are empty) and welds vertices by
exact float bits. Little-endian; every section is 4-byte aligned, so each can
be wrapped directly as a JS typed array.

| off  | type            | field                                        |
|------|-----------------|----------------------------------------------|
| 0x00 | char[4]         | `"DCOL"`                                     |
| 0x04 | u32             | version = 2 (v1 = the same without the walk mesh) |
| 0x08 | u32             | nverts                                       |
| 0x0c | u32             | ntris                                        |
| 0x10 | f32[3]          | bbox min (x,y,z)                             |
| 0x1c | f32[3]          | bbox max                                     |
| 0x28 | u32             | nfiles merged                                |
| 0x2c | u32             | reserved (0)                                 |
| 0x30 | f32[nverts*3]   | vertices x,y,z (game units, **−Y up**)       |
| …    | u32[ntris*3]    | triangle vertex indices                      |
| …    | u32[ntris]      | surface word (per-polygon S-32 word, §5)     |
| …    | u32[ntris]      | info: bits 0–1 sub-tree (0=+0x38, 1=+0x58, 2=+0x78, 3=+0x98); bits 2–3 source (`cid_tri_source`); bits 8–15 input file index; bits 16–31 packet mask (tag-1: class word & 0xffff) |
| …    | u32, u32        | npolys, reserved (0)                         |
| …    | u32[npolys*10]  | walk mesh (§3d), per polygon: `nv | flags<<16` (face +0x04); `word6 | file<<16`; v[4] vertex indices (nv used); nbr[4] polygon index or 0xffffffff |

File size = `0x30 + 12·nverts + 20·ntris + 8 + 40·npolys`. z128: 107,111 verts,
161,716 tris, 3,186 polygons, 4,647,148 bytes.

**Walking** (`move`, `ground_height` / `groundY`; worker 0061): the walk mesh,
as decoded in §3d: polygon found under the feet (the game keeps it; a stateless
caller takes the one nearest the feet between 7.5 above and maxDrop below),
then `FUN_00357d48`'s passes, neighbour crossings, boundary slides and the
0x80/0x10/0x02 flags (0x80 walkable by default, `block80` makes it a wall).
`move(..., air=True)` walks only the airborne ground point (the jump/fall
horizontal step). `ground_height` = the polygon search of `FUN_0035cbc8`, from
step_up (default 7.5) above to 200 below. No slope limit, step height, ledge
or headroom rule: those were worker 0055's stand-ins and are gone.

**Triangle set** (`capsule_push`, `segment_hit`: airborne helpers, camera,
shots; not used for walking), 145,565 tris on z128: sub-tree 1 packets that some
character probe tests (`mask & 0xfe7e != 0`, the game's test over 0x18d, 0x18b,
0x187), plus the sub-tree-0 tag-1 primitives. The sub-tree-2 railing boxes are
dropped: their only users are the physics-joint library (`FUN_00406378`,
vtables near 0x4a8624), not characters.

`sweep_collision.mjs` flood-fills the reachable area from the spawn (1-unit
state bins) and counts steps whose body crosses the visible room meshes.

`python3 -I cid_query.py out/web/z128_collision.bin` runs the self-test
(ground under the three known Vincent positions, 8-direction walk);
`… X Z [FEET_Y]` prints the ground under a point.
