#!/usr/bin/env python3
"""cid_query.py -- reference collision queries over a DCOL web collision file.

Pure Python (struct/array/math only) so it ports line-for-line to JavaScript.
Reads the file written by `cidweb` (see FORMAT.md section 8):

    python3 -I cid_query.py out/web/z128_collision.bin            # self-test
    python3 -I cid_query.py out/web/z128_collision.bin X Z [Y]    # ground under x,z

World space = the game's: float32 units, **-Y is up** (gravity is +Y; the sky
dome spans y -1101..-356, the deck Vincent stands on is at y=-98.6, y=0 is below
it). Same frame as the room meshes (out/z128_room_v2) and Vincent's placement
(feet y). A web viewer that wants +Y up negates y (and z, to keep handedness).

WALKING (worker 0061, decoded; FORMAT.md 3d). z128's stage record (06800001
section 4, +0x44 bit 0) sets script flag bit 13 (FUN_0053c140), so
FUN_0034db80 turns the walk mode 10 (FUN_003539b0, the ray-probe walker that
worker 0057 decoded) into mode 0xc: the physics body at char+0x3b0 (FUN_0034c388
case 0xc -> FUN_003590c8). Every body state moves through FUN_00357d48, a
NAVMESH walker over the sub-tree-0 (+0x38) tag-0 face records: convex polygons
with a neighbour pointer per edge (face +0x10, relocated by FUN_0026f300) and
a flag word at face +0x04. The body holds its current polygon (body+0x110). Per
move (up to 8 passes):
  * the move is projected onto the polygon's plane along the up axis and
    rescaled to its original length (FUN_002606b8; ground states);
  * if the end point is inside the polygon (all edge planes <= 0, FUN_002605c8;
    edge plane j passes through v[j], v[j+1] and v[j] + face normal,
    FUN_0035d2e0), the body goes there (projected onto the plane) and stops;
  * else the first edge plane the move crosses is found (FUN_0035d4e8). A
    neighbour across it: the body moves to the crossing point, switches polygon
    and carries on with the rest of the move. The neighbour it just came from
    counts as no neighbour (piStack_c8).
  * no neighbour (a boundary edge): the body stops at the crossing point and
    the rest of the move is projected onto the edge direction plus 1% of its
    length pushed back inside (FUN_0025a778, edge plane x -0.01|m|); the
    character's hit-wall bit (0x400) is set. That is what holds him on the deck
    and the pipe walkway: the polygons end 5-10 units inside the railings.
  * Flags (z128 uses 0x02, 0x10, 0x80): a neighbour with 0x80 is a wall only for
    bodies with flag 0x200 (char+0x45c, set by FUN_00559748 from a controller
    bit; NOT resolved for the player, so 0x80 is walkable here, option
    block80). A 0x10 neighbour (ground states) is a one-way drop: if the
    outward normal of its shared edge points up (n.y < 0) the body enters it
    and starts falling (FUN_00357148), else it is a wall. (Entering 0x10 from a
    polygon without it, the game first casts a short probe for raised 0x10
    polygons, FUN_0035cca0 / FUN_0035cef0; not ported.) A boundary edge of a 0x02 polygon looks for
    another polygon under the end point (FUN_00359f38): that is how the walk
    crosses between chunk files, whose polygons never link to each other; a
    drop of more than 3 starts a fall. 0x04, 0x40 and 0x100 do not occur in
    z128 and are treated as walls.
  * The polygon under a point is found by a vertical segment from 7.5 above it
    to 200 below (FUN_0035cbc8 -> FUN_0035cca0 -> FUN_0026a668, every loaded
    file), first hit from the top.
  * In the air (jump, fall) the body's ground point keeps walking the navmesh
    the same way (FUN_003590c8 state 2 -> FUN_00357d48, with a horizontal edge
    slide), so a jump cannot leave the polygons either. move(..., air=True)
    walks that point and returns the height of the polygon under it.
There is no slope limit, step height, ledge test or headroom test in this
path: the navmesh is the walkable area.

The TRIANGLE set below (capsule_push, segment_hit) is not used by move(); it is
kept for the airborne helpers in player.js / actions and for the camera and
shots. It is sub-tree-1 packets some character probe tests (FUN_0026f8a0:
mask & ~(flags|0x100) != 0 over 0x18d/0x18b/0x187, i.e. mask & 0xfe7e) plus the
sub-tree-0 tag-1 primitives (FUN_0035a440's body push set). The sub-tree-2
railing boxes (worker 0055's stand-in) are dropped: their only users are the
physics-joint library (FUN_00406378, vtables at 0x4a8624), not characters.
"""
import math, struct, sys
from array import array

FLOOR_NY = 0.55     # capsule_push ignores faces this flat (|n.y| >= FLOOR_NY): the ground query handles them
SEARCH_UP = 7.5     # FUN_0035cbc8: polygon search starts 7.5 above the point (0x004a2178 = (0,-7.5,0))
SEARCH_DOWN = 200.0 # ... and ends 200 below it (0x43480000)
DROP_FALL = 3.0     # FUN_00359f38: moving to a polygon more than 3 lower starts a fall
NAV_PASSES = 8      # FUN_00357d48 loop limit
EDGE_EPS = 0.001    # FUN_0035d4e8: start point counts as inside an edge plane up to 0.001
SLIDE_IN = 0.01     # FUN_00357d48: slide move pushed 1% of its length back inside the edge
HEIGHT = 19.0       # body height for capsule_push (a guess)
STEP_UP = 5.0       # capsule_push: walls lower than this above the feet do not block
CELL = 32.0         # XZ grid cell size for the broad phase
INSIDE_EPS = 1e-4   # vertical polygon search: tolerance on the edge planes
# Character probe flag words (FUN_0054fc38 table at 0x58c990, FUN_002db1f0, ...).
PROBE_FLAGS = (0x18d, 0x18b, 0x187)

SRC_PRIM = 3         # info bits 2-3: tag-1 primitive (FORMAT.md 3c)
F_CHUNK = 0x02       # polygon flag: boundary edges look for a polygon in another file
F_DROP = 0x10        # polygon flag: one-way drop
F_NPC = 0x80         # polygon flag: wall for bodies with flag 0x200
F_WALLS = 0x04 | 0x40 | 0x100   # not in z128: treated as walls


def in_player_set(inf):
    """info word (FORMAT.md section 8) -> is this triangle in the triangle set?
    Sub-tree 1 packets some character probe tests (mask & 0xfe7e != 0), plus
    the sub-tree-0 tag-1 primitives."""
    st = inf & 3
    m = inf >> 16
    if st == 0:
        return (inf >> 2) & 3 == SRC_PRIM
    return st == 1 and any(m & ~(f | 0x100) & 0xffff for f in PROBE_FLAGS)


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _plane3(p1, p2, p3):
    """FUN_0025ff10: plane through p1, p2, p3, normal (p2-p1) x (p3-p2) made
    unit, w = -n.p1. None if degenerate."""
    n = _cross((p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2]),
               (p3[0] - p2[0], p3[1] - p2[1], p3[2] - p2[2]))
    ln = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])
    if ln < 1e-12:
        return None
    nx, ny, nz = n[0] / ln, n[1] / ln, n[2] / ln
    return (nx, ny, nz, -(nx * p1[0] + ny * p1[1] + nz * p1[2]))


class Poly:
    __slots__ = ('nv', 'flags', 'word6', 'file', 'v', 'nbr', 'plane', 'edges', 'ok')


class Collision:
    def __init__(self, path, include_all=False, block80=False):
        b = open(path, 'rb').read()
        magic, ver, nv, nt = struct.unpack_from('<4sIII', b, 0)
        if magic != b'DCOL' or ver != 2:
            raise ValueError('not a DCOL v2 file (rebuild: make -C native/assets/collision z128web)')
        o = 0x30
        self.v = array('f'); self.v.frombytes(b[o:o + nv * 12]); o += nv * 12
        tri = array('I'); tri.frombytes(b[o:o + nt * 12]); o += nt * 12
        surf = array('I'); surf.frombytes(b[o:o + nt * 4]); o += nt * 4
        info = array('I'); info.frombytes(b[o:o + nt * 4]); o += nt * 4
        npoly, _ = struct.unpack_from('<II', b, o); o += 8
        pw = array('I'); pw.frombytes(b[o:o + npoly * 40]); o += npoly * 40
        if o != len(b):
            raise ValueError('size mismatch')
        if sys.byteorder != 'little':
            for a in (self.v, tri, surf, info, pw):
                a.byteswap()
        self.block80 = block80
        v = self.v
        # ---- triangle set (capsule_push, segment_hit)
        self.tris = []          # (ax,ay,az, bx,by,bz, cx,cy,cz, nx,ny,nz, surface)
        for t in range(nt):
            inf = info[t]
            if not include_all and not in_player_set(inf):
                continue
            i, j, k = tri[3 * t], tri[3 * t + 1], tri[3 * t + 2]
            ax, ay, az = v[3 * i], v[3 * i + 1], v[3 * i + 2]
            bx, by, bz = v[3 * j], v[3 * j + 1], v[3 * j + 2]
            cx, cy, cz = v[3 * k], v[3 * k + 1], v[3 * k + 2]
            ux, uy, uz = bx - ax, by - ay, bz - az
            wx, wy, wz = cx - ax, cy - ay, cz - az
            nx, ny, nz = uy * wz - uz * wy, uz * wx - ux * wz, ux * wy - uy * wx
            ln = math.sqrt(nx * nx + ny * ny + nz * nz)
            if ln < 1e-9:
                continue
            self.tris.append((ax, ay, az, bx, by, bz, cx, cy, cz,
                              nx / ln, ny / ln, nz / ln, surf[t]))
        self.grid = self._grid([(min(T[0], T[3], T[6]), max(T[0], T[3], T[6]),
                                 min(T[2], T[5], T[8]), max(T[2], T[5], T[8])) for T in self.tris])
        # ---- walk mesh (FORMAT.md 3d)
        self.polys = []
        for q in range(npoly):
            w = pw[10 * q: 10 * q + 10]
            P = Poly()
            P.nv = w[0] & 0xffff
            P.flags = w[0] >> 16
            P.word6 = w[1] & 0xffff
            P.file = w[1] >> 16
            P.v = [(v[3 * w[2 + a]], v[3 * w[2 + a] + 1], v[3 * w[2 + a] + 2]) for a in range(P.nv)]
            P.nbr = [None if w[6 + a] == 0xffffffff else w[6 + a] for a in range(P.nv)]
            vs = P.v
            P.plane = _plane3(vs[0], vs[1], vs[2])        # FUN_0035d0a0
            # FUN_0035d2e0: N = (v1-v0) x (v2-v1) (not unit); edge plane j through
            # v[j], v[j+1], v[j] + N.
            N = _cross((vs[1][0] - vs[0][0], vs[1][1] - vs[0][1], vs[1][2] - vs[0][2]),
                       (vs[2][0] - vs[1][0], vs[2][1] - vs[1][1], vs[2][2] - vs[1][2]))
            P.edges = []
            for a in range(P.nv):
                p1, p2 = vs[a], vs[(a + 1) % P.nv]
                P.edges.append(_plane3(p1, p2, (p1[0] + N[0], p1[1] + N[1], p1[2] + N[2])))
            P.ok = P.plane is not None and all(e is not None for e in P.edges)
            self.polys.append(P)
        self.pgrid = self._grid([(min(p[0] for p in P.v), max(p[0] for p in P.v),
                                  min(p[2] for p in P.v), max(p[2] for p in P.v)) for P in self.polys])

    @staticmethod
    def _grid(boxes):
        """Uniform XZ grid: each item goes in every cell its XZ bbox touches."""
        g = {}
        for idx, (x0, x1, z0, z1) in enumerate(boxes):
            for gx in range(int(math.floor(x0 / CELL)), int(math.floor(x1 / CELL)) + 1):
                for gz in range(int(math.floor(z0 / CELL)), int(math.floor(z1 / CELL)) + 1):
                    g.setdefault((gx, gz), []).append(idx)
        return g

    def cells(self, xmin, zmin, xmax, zmax, grid=None):
        """Item indices whose cells overlap the XZ box (deduplicated)."""
        grid = self.grid if grid is None else grid
        seen = set()
        for gx in range(int(math.floor(xmin / CELL)), int(math.floor(xmax / CELL)) + 1):
            for gz in range(int(math.floor(zmin / CELL)), int(math.floor(zmax / CELL)) + 1):
                for i in grid.get((gx, gz), ()):
                    if i not in seen:
                        seen.add(i)
                        yield i

    # ------------------------------------------------------------ walk mesh
    @staticmethod
    def _pd(pl, x, y, z):
        return pl[0] * x + pl[1] * y + pl[2] * z + pl[3]

    def inside(self, P, x, y, z, eps=0.0):
        """FUN_002605c8: inside every edge plane."""
        for e in P.edges:
            if e[0] * x + e[1] * y + e[2] * z + e[3] > eps:
                return False
        return True

    def find_poly(self, x, z, y_top, y_bot):
        """First walk polygon hit by the vertical segment (x, y_top, z) ->
        (x, y_bot, z), y_top < y_bot (-Y up): the smallest y >= y_top.
        Returns (poly index, y) or None. y_top None: no upper limit."""
        best = None
        for i in self.cells(x, z, x, z, self.pgrid):
            P = self.polys[i]
            if not P.ok or abs(P.plane[1]) < 1e-6:
                continue
            pl = P.plane
            y = -(pl[0] * x + pl[2] * z + pl[3]) / pl[1]
            if (y_top is not None and y < y_top) or y > y_bot:
                continue
            if not self.inside(P, x, y, z, INSIDE_EPS):
                continue
            if best is None or y < best[1]:
                best = (i, y)
        return best

    def nearest_poly(self, x, y, z, max_drop):
        """The polygon a walking body at feet (x,y,z) is on: the game keeps it
        (body+0x110); a stateless caller takes the polygon nearest the feet
        between SEARCH_UP above and max_drop below. (poly, y) or None."""
        best = None
        for i in self.cells(x, z, x, z, self.pgrid):
            P = self.polys[i]
            if not P.ok or abs(P.plane[1]) < 1e-6:
                continue
            pl = P.plane
            py = -(pl[0] * x + pl[2] * z + pl[3]) / pl[1]
            if py < y - SEARCH_UP or py > y + max_drop:
                continue
            if not self.inside(P, x, py, z, INSIDE_EPS):
                continue
            d = abs(py - y)
            if best is None or d < best[2]:
                best = (i, py, d)
        return None if best is None else best[:2]

    def locate(self, x, y, z):
        """The polygon the body at feet (x,y,z) stands on or is above
        (FUN_0035cbc8: from SEARCH_UP above to SEARCH_DOWN below)."""
        return self.find_poly(x, z, y - SEARCH_UP, y + SEARCH_DOWN)

    def _clamp(self, P, x, y, z):
        """FUN_0035d788: closest point on the polygon's edges."""
        best = None
        n = P.nv
        for a in range(n):
            ax, ay, az = P.v[a]
            bx, by, bz = P.v[(a + 1) % n]
            ex, ey, ez = bx - ax, by - ay, bz - az
            ll = ex * ex + ey * ey + ez * ez
            t = ((x - ax) * ex + (y - ay) * ey + (z - az) * ez) / ll if ll > 0 else 0.0
            if t < 0.0:
                q = (ax, ay, az)
            elif t > 1.0:
                q = (bx, by, bz)
            else:
                q = (ax + ex * t, ay + ey * t, az + ez * t)
            d = (x - q[0]) ** 2 + (y - q[1]) ** 2 + (z - q[2]) ** 2
            if best is None or d < best[0]:
                best = (d, q)
        return best[1]

    def _exit(self, P, px, py, pz, cx, cy, cz):
        """FUN_0035d4e8: the first edge plane crossed going from p to c (the move
        flattened onto the polygon plane). Returns (edge, x, y, z); edge -1 and p
        if none."""
        pl = P.plane
        dx, dy, dz = cx - px, cy - py, cz - pz
        dn = dx * pl[0] + dy * pl[1] + dz * pl[2]
        dx, dy, dz = dx - pl[0] * dn, dy - pl[1] * dn, dz - pl[2] * dn
        best_t, best = None, (-1, px, py, pz)
        for a, e in enumerate(P.edges):
            sp = e[0] * px + e[1] * py + e[2] * pz + e[3]
            sc = e[0] * cx + e[1] * cy + e[2] * cz + e[3]
            if sp <= EDGE_EPS and sc > 0.0:
                den = e[0] * dx + e[1] * dy + e[2] * dz
                if den == 0.0:
                    continue
                t = -sp / den
                if best_t is None or t < best_t:
                    best_t = t
                    best = (a, px + dx * t, py + dy * t, pz + dz * t)
        return best

    def _project(self, P, x, y, z):
        """FUN_00260628 along the plane normal: the point on P's plane."""
        pl = P.plane
        s = pl[0] * x + pl[1] * y + pl[2] * z + pl[3]
        return x - pl[0] * s, y - pl[1] * s, z - pl[2] * s

    def nav_move(self, pi, x, y, z, dx, dz, air=False):
        """FUN_00357d48 over the walk mesh from polygon pi at (x,y,z) by
        (dx, 0, dz). Returns (x, y, z, poly, hit_wall, fall): fall = the body
        left the ground (a 0x10 drop or a chunk change more than DROP_FALL
        lower)."""
        P = self.polys[pi]
        mx, my, mz = dx, 0.0, dz
        prev = None
        hit = False
        for _ in range(NAV_PASSES):
            if not self.inside(P, x, y, z):             # FUN_0035d788 at the top of every pass
                x, y, z = self._clamp(P, x, y, z)
            L = math.sqrt(mx * mx + my * my + mz * mz)
            if L < 0.001:
                break
            pl = P.plane
            # FUN_002606b8 (along the up axis) then rescale to the length L
            if abs(pl[1]) > 1e-9:
                my = my - (pl[0] * mx + pl[1] * my + pl[2] * mz) / pl[1]
            l2 = math.sqrt(mx * mx + my * my + mz * mz)
            if l2 > 0.0:
                mx, my, mz = mx / l2 * L, my / l2 * L, mz / l2 * L
            cx, cy, cz = x + mx, y + my, z + mz
            if self.inside(P, cx, cy, cz):
                x, y, z = self._project(P, cx, cy, cz)
                break
            e, ex, ey, ez = self._exit(P, x, y, z, cx, cy, cz)
            if (ex - x) ** 2 + (ey - y) ** 2 + (ez - z) ** 2 > mx * mx + my * my + mz * mz:
                x, y, z = self._clamp(P, cx, cy, cz)
                break
            if e < 0:
                break
            nb = P.nbr[e]
            if nb is not None and (nb == prev or not self.polys[nb].ok):
                nb = None
            fall = False
            if nb is not None:
                N = self.polys[nb]
                if N.flags & F_NPC and self.block80:
                    nb = None
                elif N.flags & F_WALLS:
                    nb = None
                elif not air and N.flags & F_DROP:
                    k = N.nbr.index(pi) if pi in N.nbr else -1
                    if k >= 0 and N.edges[k][1] < 0.0:
                        fall = True                     # FUN_00357148: drop off
                    else:
                        nb = None
            if nb is not None:
                x, y, z = ex, ey, ez
                mx, my, mz = cx - ex, cy - ey, cz - ez
                prev, pi, P = pi, nb, self.polys[nb]
                if fall:
                    return x, y, z, pi, hit, True
                if mx * mx + my * my + mz * mz == 0.0:
                    break
                continue
            # boundary edge
            x, y, z = ex, ey, ez
            mx, my, mz = cx - ex, cy - ey, cz - ez
            if P.flags & F_CHUNK:                       # FUN_00359f38 at the end point
                f = self.locate(cx, cy, cz)
                if f is not None and f[0] != pi:
                    fy = f[1]
                    return cx, fy, cz, f[0], hit, fy - y > DROP_FALL
            a, b = P.v[e], P.v[(e + 1) % P.nv]
            ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
            if air:                                     # state 2: horizontal slide
                uy = 0.0
                keep_y, my = my, 0.0
            lu = math.sqrt(ux * ux + uy * uy + uz * uz)
            if lu > 0.0:
                ux, uy, uz = ux / lu, uy / lu, uz / lu
            s = mx * ux + my * uy + mz * uz             # FUN_0025a778(0, ...): onto the edge
            mx, my, mz = ux * s, uy * s, uz * s
            k = -math.sqrt(mx * mx + my * my + mz * mz) * SLIDE_IN
            en = P.edges[e]
            mx, mz = mx + en[0] * k, mz + en[2] * k
            if air:
                my = keep_y
            else:
                my = my + en[1] * k
            hit = True
            if mx * mx + my * my + mz * mz == 0.0:
                break
        return x, y, z, pi, hit, False

    # ------------------------------------------------------------------ ground
    def ground_height(self, x, z, feet_y=None, step_up=SEARCH_UP):
        """The walk polygon under (x,z): first hit going down from
        feet_y - step_up to feet_y + SEARCH_DOWN (-Y is up). feet_y None: the
        topmost polygon. Returns (y, normal, flags, poly) or None."""
        f = self.find_poly(x, z, None if feet_y is None else feet_y - step_up,
                           math.inf if feet_y is None else feet_y + SEARCH_DOWN)
        if f is None:
            return None
        P = self.polys[f[0]]
        return (f[1], P.plane[:3], P.flags, f[0])

    def move(self, x, y, z, dx, dz, radius=3.5, height=HEIGHT, max_drop=40.0, air=False):
        """Walk the feet (x,y,z) by (dx,dz) over the walk mesh (nav_move).
        Returns (x, y, z, on_ground, hit_wall).
        Ground: off the mesh (no polygon from SEARCH_UP above to max_drop below:
        stay, on_ground False, the caller falls), or a drop -> on_ground False
        at the drop point. air=True: walk the ground point only (the jump / fall
        horizontal step); y is unchanged, on_ground False, and the height of
        the polygon under the new point is ground_y(). radius/height: unused
        (the polygons are the area the body's centre may use)."""
        f = (self.find_poly(x, z, y - SEARCH_UP, y + SEARCH_DOWN) if air
             else self.nearest_poly(x, y, z, max_drop))
        if f is None:
            if air:
                return x + dx, y, z + dz, False, False
            return x, y, z, False, False
        pi, gy = f
        nx, ny, nz, _p, hit, fall = self.nav_move(pi, x, gy, z, dx, dz, air)
        if air:
            return nx, y, nz, False, hit
        return nx, ny, nz, not fall, hit

    # -------------------------------------------------------------- walls
    @staticmethod
    def closest_on_tri(px, py, pz, T):
        """Closest point on triangle T to p (Ericson, RTCD 5.1.5)."""
        ax, ay, az, bx, by, bz, cx, cy, cz = T[:9]
        abx, aby, abz = bx - ax, by - ay, bz - az
        acx, acy, acz = cx - ax, cy - ay, cz - az
        apx, apy, apz = px - ax, py - ay, pz - az
        d1 = abx * apx + aby * apy + abz * apz
        d2 = acx * apx + acy * apy + acz * apz
        if d1 <= 0 and d2 <= 0:
            return ax, ay, az
        bpx, bpy, bpz = px - bx, py - by, pz - bz
        d3 = abx * bpx + aby * bpy + abz * bpz
        d4 = acx * bpx + acy * bpy + acz * bpz
        if d3 >= 0 and d4 <= d3:
            return bx, by, bz
        vc = d1 * d4 - d3 * d2
        if vc <= 0 and d1 >= 0 and d3 <= 0:
            t = d1 / (d1 - d3)
            return ax + t * abx, ay + t * aby, az + t * abz
        cpx, cpy, cpz = px - cx, py - cy, pz - cz
        d5 = abx * cpx + aby * cpy + abz * cpz
        d6 = acx * cpx + acy * cpy + acz * cpz
        if d6 >= 0 and d5 <= d6:
            return cx, cy, cz
        vb = d5 * d2 - d1 * d6
        if vb <= 0 and d2 >= 0 and d6 <= 0:
            t = d2 / (d2 - d6)
            return ax + t * acx, ay + t * acy, az + t * acz
        va = d3 * d6 - d5 * d4
        if va <= 0 and (d4 - d3) >= 0 and (d5 - d6) >= 0:
            t = (d4 - d3) / ((d4 - d3) + (d5 - d6))
            return bx + t * (cx - bx), by + t * (cy - by), bz + t * (cz - bz)
        den = 1.0 / (va + vb + vc)
        v = vb * den
        w = vc * den
        return ax + abx * v + acx * w, ay + aby * v + acy * w, az + abz * v + acz * w

    def capsule_push(self, x, feet_y, z, radius=3.5, height=19.0,
                     step_up=STEP_UP, iters=3):
        """Push a vertical capsule out of the triangle set's walls, horizontally
        only. Not the game's walk (that is the walk mesh, move()); kept for the
        airborne helpers. The capsule runs from step_up above the feet to height
        above the feet (above = smaller y), tested as spheres of `radius` spaced
        <= radius apart. Floors (|normal.y| >= FLOOR_NY) are ignored.
        Returns (x, z, hit)."""
        y0 = feet_y - step_up - radius
        y1 = feet_y - height + radius
        n = max(1, int(math.ceil((y0 - y1) / radius)))
        ys = [y0 + (y1 - y0) * k / n for k in range(n + 1)] if y1 < y0 else [y0]
        hit = False
        for _ in range(iters):
            moved = False
            for i in self.cells(x - radius, z - radius, x + radius, z + radius):
                T = self.tris[i]
                if abs(T[10]) >= FLOOR_NY:
                    continue
                for sy in ys:
                    qx, qy, qz = self.closest_on_tri(x, sy, z, T)
                    dx, dz = x - qx, z - qz
                    dy = sy - qy
                    if dx * dx + dy * dy + dz * dz >= radius * radius:
                        continue
                    h = math.sqrt(dx * dx + dz * dz)
                    if h < 1e-6:          # straight above/below: use the face normal
                        dx, dz = T[9], T[11]
                        h = math.sqrt(dx * dx + dz * dz)
                        if h < 1e-6:
                            continue
                        dist = 0.0
                    else:
                        dist = h
                    # horizontal push to clear the sphere at this height
                    need = math.sqrt(max(radius * radius - dy * dy, 0.0)) - dist
                    if need > 1e-4:
                        x += dx / h * need
                        z += dz / h * need
                        hit = moved = True
            if not moved:
                break
        return x, z, hit

    # ------------------------------------------------------------- segment
    def segment_hit(self, p0, p1):
        """First triangle hit by segment p0->p1 (double-sided Moller-Trumbore).
        Returns (t in [0,1], point, normal, surface) or None. Uses the XZ cells
        under the segment's bbox: fine for short probes (steps, camera)."""
        dx, dy, dz = p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]
        best = None
        for i in self.cells(min(p0[0], p1[0]), min(p0[2], p1[2]),
                            max(p0[0], p1[0]), max(p0[2], p1[2])):
            T = self.tris[i]
            e1 = (T[3] - T[0], T[4] - T[1], T[5] - T[2])
            e2 = (T[6] - T[0], T[7] - T[1], T[8] - T[2])
            px = dy * e2[2] - dz * e2[1]
            py = dz * e2[0] - dx * e2[2]
            pz = dx * e2[1] - dy * e2[0]
            det = e1[0] * px + e1[1] * py + e1[2] * pz
            if abs(det) < 1e-12:
                continue
            inv = 1.0 / det
            sx, sy, sz = p0[0] - T[0], p0[1] - T[1], p0[2] - T[2]
            u = (sx * px + sy * py + sz * pz) * inv
            if u < 0 or u > 1:
                continue
            qx = sy * e1[2] - sz * e1[1]
            qy = sz * e1[0] - sx * e1[2]
            qz = sx * e1[1] - sy * e1[0]
            v = (dx * qx + dy * qy + dz * qz) * inv
            if v < 0 or u + v > 1:
                continue
            t = (e2[0] * qx + e2[1] * qy + e2[2] * qz) * inv
            if 0 <= t <= 1 and (best is None or t < best[0]):
                best = (t, (p0[0] + t * dx, p0[1] + t * dy, p0[2] + t * dz),
                        (T[9], T[10], T[11]), T[12])
        return best


def _selftest(col):
    print('triangle set: %d, grid cells: %d; walk polygons: %d, cells %d'
          % (len(col.tris), len(col.grid), len(col.polys), len(col.pgrid)))
    # Known Vincent feet positions (GS-dump fits; see placement.json files).
    spots = [('hero 163235', -1395.64, -1110.46, -98.77),
             ('walk 185138 h_st_94', -1395.70, -1110.27, -98.66),
             ('walk 185126 h_st_160', -1364.20, -1097.06, -98.67)]
    for name, x, z, fy in spots:
        g = col.ground_height(x, z, fy)
        print('%-22s walk polygon %d at %.2f (feet %.2f, err %+.2f)'
              % (name, g[3], g[0], fy, fy - g[0]))
    # Walk in 8 directions from the start; stop where nothing moves.
    x0, z0 = spots[0][1], spots[0][2]
    y0 = col.ground_height(x0, z0, spots[0][3])[0]
    for k in range(8):
        a = k * math.pi / 4
        x, z, y = x0, z0, y0
        travelled = 0.0
        hits = 0
        for _ in range(200):                       # 200 steps of 1 unit
            nx, ny, nz, on_ground, hit = col.move(x, y, z, math.cos(a), math.sin(a))
            if not on_ground:
                break
            step = math.hypot(nx - x, nz - z)
            hits += hit
            if step < 0.05:
                break
            x, z, y = nx, nz, ny
            travelled += step
        print('dir %3d deg: walked %6.1f units -> (%.1f, %.2f, %.1f), %d wall-slide steps'
              % (k * 45, travelled, x, y, z, hits))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    c = Collision(sys.argv[1])
    if len(sys.argv) >= 4:
        fy = float(sys.argv[4]) if len(sys.argv) > 4 else None
        print(c.ground_height(float(sys.argv[2]), float(sys.argv[3]), fy))
    else:
        _selftest(c)
