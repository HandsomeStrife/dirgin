#!/usr/bin/env python3
"""cid_query.py -- reference collision queries over a DCOL web collision file.

Pure Python (struct/array/math only) so it ports line-for-line to JavaScript.
Reads the file written by `cidweb` (see FORMAT.md section 8):

    python3 -I cid_query.py out/web/z128_collision.bin            # self-test
    python3 -I cid_query.py out/web/z128_collision.bin X Z [Y]    # ground under x,z

World space = the game's: float32 units, **-Y is up** (gravity is +Y; the sky
dome spans y -1101..-356, the deck Vincent stands on is at y=-98.44, y=0 is below
it). Same frame as the room meshes (out/z128_room_v2) and Vincent's placement
(feet y). A web viewer that wants +Y up negates y (and z, to keep handedness).

Rules (each one checked on z128, see the worker-0045 report):
  * Player set = sub-tree 1 (payload +0x58, the tree the game's floor probe
    FUN_002714d0 walks) minus packets whose surface mask has bit 0x80 (every
    character probe flag set, 0x18e / 0x18d, excludes that bit: e.g. a
    collision-only plane at y=-86 over the start point). Sub-tree 0 (+0x38) is
    a separate small tree the segment query uses; not needed for walking.
  * Floors are triangles whose normal (b-a)x(c-a) has y >= FLOOR_NY, i.e. the
    stored winding makes that normal point DOWN into the floor (+Y). Checked
    under the start point: deck top -98.44 has ny=+1, the deck's bottom face
    -94.5 has ny=-1. Ceilings/undersides have ny <= -FLOOR_NY.
  * Ground = the first floor at or below the feet (smallest y >= feet_y -
    STEP_UP: the feet may snap UP by at most STEP_UP; stairs rise ~3.4).
"""
import math, struct, sys
from array import array

FLOOR_NY = 0.6      # cos(53 deg): the 45-degree ramps are walkable
STEP_UP = 5.0       # max rise the feet snap up onto (stairs are ~3.4)
CELL = 32.0         # XZ grid cell size for the broad phase


class Collision:
    def __init__(self, path, include_all=False):
        b = open(path, 'rb').read()
        magic, ver, nv, nt = struct.unpack_from('<4sIII', b, 0)
        if magic != b'DCOL' or ver != 1:
            raise ValueError('not a DCOL v1 file')
        o = 0x30
        self.v = array('f'); self.v.frombytes(b[o:o + nv * 12]); o += nv * 12
        tri = array('I'); tri.frombytes(b[o:o + nt * 12]); o += nt * 12
        surf = array('I'); surf.frombytes(b[o:o + nt * 4]); o += nt * 4
        info = array('I'); info.frombytes(b[o:o + nt * 4]); o += nt * 4
        if o != len(b):
            raise ValueError('size mismatch')
        if sys.byteorder != 'little':
            for a in (self.v, tri, surf, info):
                a.byteswap()
        # Keep the player set; precompute per-triangle corners and unit normal.
        self.tris = []          # (ax,ay,az, bx,by,bz, cx,cy,cz, nx,ny,nz, surface)
        v = self.v
        for t in range(nt):
            inf = info[t]
            if not include_all and ((inf & 3) != 1 or (inf >> 16) & 0x80):
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
        # Uniform XZ grid: each triangle goes in every cell its XZ bbox touches.
        self.grid = {}
        for idx, T in enumerate(self.tris):
            x0 = int(math.floor(min(T[0], T[3], T[6]) / CELL))
            x1 = int(math.floor(max(T[0], T[3], T[6]) / CELL))
            z0 = int(math.floor(min(T[2], T[5], T[8]) / CELL))
            z1 = int(math.floor(max(T[2], T[5], T[8]) / CELL))
            for gx in range(x0, x1 + 1):
                for gz in range(z0, z1 + 1):
                    self.grid.setdefault((gx, gz), []).append(idx)

    def cells(self, xmin, zmin, xmax, zmax):
        """Triangle indices whose cells overlap the XZ box (deduplicated)."""
        seen = set()
        for gx in range(int(math.floor(xmin / CELL)), int(math.floor(xmax / CELL)) + 1):
            for gz in range(int(math.floor(zmin / CELL)), int(math.floor(zmax / CELL)) + 1):
                for i in self.grid.get((gx, gz), ()):
                    if i not in seen:
                        seen.add(i)
                        yield i

    # ------------------------------------------------------------------ ground
    def surface_y(self, T, x, z):
        """Height of triangle T's plane at (x,z) if (x,z) is inside its XZ
        projection, else None. Winding-independent barycentrics."""
        ax, az = T[0], T[2]
        ux, uz = T[3] - ax, T[5] - az
        wx, wz = T[6] - ax, T[8] - az
        den = ux * wz - wx * uz
        if abs(den) < 1e-9:
            return None                       # vertical triangle
        px, pz = x - ax, z - az
        u = (px * wz - wx * pz) / den
        v = (ux * pz - px * uz) / den
        e = 1e-6
        if u < -e or v < -e or u + v > 1 + e:
            return None
        return T[1] + u * (T[4] - T[1]) + v * (T[7] - T[1])

    def ground_height(self, x, z, feet_y=None, step_up=STEP_UP):
        """First floor (normal.y >= FLOOR_NY) under (x,z), counting from
        feet_y - step_up downwards (-Y is up, so 'below' = larger y): the
        smallest y >= feet_y - step_up. feet_y=None: the topmost floor.
        Returns (y, normal, surface) or None if there is no floor below."""
        best = None
        lim = None if feet_y is None else feet_y - step_up
        for i in self.cells(x, z, x, z):
            T = self.tris[i]
            if T[10] < FLOOR_NY:
                continue
            y = self.surface_y(T, x, z)
            if y is None or (lim is not None and y < lim):
                continue
            if best is None or y < best[0]:
                best = (y, (T[9], T[10], T[11]), T[12])
        return best

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
        """Push a vertical capsule out of walls, horizontally only.
        The capsule runs from step_up above the feet (so kerbs/stairs lower than
        the step do not block; the ground query handles them) to height above
        the feet. Above = smaller y.
        It is tested as spheres of `radius` spaced <= radius apart along that
        axis. Floors (normal.y >= FLOOR_NY) are ignored. Returns (x, z, hit)."""
        y0 = feet_y - step_up - radius
        y1 = feet_y - height + radius
        n = max(1, int(math.ceil((y0 - y1) / radius)))
        ys = [y0 + (y1 - y0) * k / n for k in range(n + 1)] if y1 < y0 else [y0]
        hit = False
        for _ in range(iters):
            moved = False
            for i in self.cells(x - radius, z - radius, x + radius, z + radius):
                T = self.tris[i]
                if T[10] >= FLOOR_NY:
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
    print('player-set triangles: %d, grid cells: %d' % (len(col.tris), len(col.grid)))
    # Known Vincent feet positions (GS-dump fits; see placement.json files).
    spots = [('hero 163235', -1395.64, -1110.46, -98.77),
             ('walk 185138 h_st_94', -1395.70, -1110.27, -98.66),
             ('walk 185126 h_st_160', -1364.20, -1097.06, -98.67)]
    for name, x, z, fy in spots:
        g = col.ground_height(x, z, fy)
        top = col.ground_height(x, z)
        px, pz, hit = col.capsule_push(x, fy, z)
        print('%-22s ground %.2f (feet %.2f, err %+.2f)  topmost floor %.2f  '
              'capsule moved %.2f hit=%s' % (name, g[0], fy, fy - g[0], top[0],
                                             math.hypot(px - x, pz - z), hit))
    # Walk in 8 directions from the start; stop where a wall pushes back.
    x0, z0, fy = spots[0][1], spots[0][2], spots[0][3]
    for k in range(8):
        a = k * math.pi / 4
        x, z, y = x0, z0, fy
        travelled = 0.0
        for _ in range(200):                       # 200 steps of 1 unit
            nx, nz = x + math.cos(a), z + math.sin(a)
            nx, nz, _hit = col.capsule_push(nx, y, nz)
            g = col.ground_height(nx, nz, y)
            if g is None or g[0] - y > 40:         # drop of > 40 units: stop
                break
            step = math.hypot(nx - x, nz - z)
            if step < 0.05:
                break
            x, z, y = nx, nz, g[0]
            travelled += step
        print('dir %3d deg: walked %6.1f units -> (%.1f, %.2f, %.1f)'
              % (k * 45, travelled, x, y, z))
    s = col.segment_hit((x0, fy - 10, z0), (x0, fy + 10, z0))
    print('segment down (+y) through start: t=%.3f y=%.2f n=%s' % (s[0], s[1][1],
          tuple(round(c, 2) for c in s[2])))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    c = Collision(sys.argv[1])
    if len(sys.argv) >= 4:
        fy = float(sys.argv[4]) if len(sys.argv) > 4 else None
        print(c.ground_height(float(sys.argv[2]), float(sys.argv[3]), fy))
    else:
        _selftest(c)
