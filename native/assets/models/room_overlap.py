"""room_overlap.py -- coplanar cover test between z128 chunks (worker 0051, 2026-10-26).

The 13 z128 chunks overlap: one chunk often stores a coarser copy of a wall another chunk
already has (06800312's 480-unit quad over 06800112's 80-unit quads at z=-510, dark vs lit
vertex colours). The game draws one of them (hero/185126 dumps: 06800112's copy; it draws
16 of 06800312's ~514 in-view triangles), but which chunk it draws where is runtime state
(occluder volumes at EE 0x004e5db0, FUN_00269978) that a free camera can't reproduce.
export_z128_room.py therefore drops an opaque triangle whose four sample points (centroid
and three points 70% toward the corners) all lie inside opaque triangles of EARLIER chunks
on the same plane. Checked against the dumps (UV + vertex colour + position): of 8892
triangles dropped, 68 are drawn by the game (06800312 pkts 813-850, 2585-2644), and each of
those has a same-colour twin at the same place that stays.
Blended (ABE) triangles are never dropped and never cover: they are overlays by design.

overlaps() (worker 0054) is the partial version, used to mark (not drop) the triangles of a
later chunk that share a plane with an earlier chunk's surface over part of their area:
they would tie in depth and z-fight; export_z128_room.py puts them in '@under' groups that
the web viewer pushes back by a polygon offset, so the earlier chunk wins, as in the dumps.
"""
import math
from collections import defaultdict
CELL = 64.0
class PlaneIndex:
    """Opaque triangles of earlier chunks, binned by plane and by 2D cell in that plane."""
    def __init__(self):
        self.bins = defaultdict(list)      # (plane, cx, cy) -> [2D tri]
    @staticmethod
    def plane(a, b, c):
        e1 = [b[i]-a[i] for i in range(3)]; e2 = [c[i]-a[i] for i in range(3)]
        n = [e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]]
        L = math.sqrt(sum(x*x for x in n))
        if L < 1e-9: return None
        n = [x/L for x in n]
        k = max(range(3), key=lambda i: abs(n[i]))
        if n[k] < 0: n = [-x for x in n]
        d = sum(n[i]*a[i] for i in range(3))
        return (round(n[0], 2), round(n[1], 2), round(n[2], 2)), d, k
    @staticmethod
    def proj(p, k):
        return (p[(k+1) % 3], p[(k+2) % 3])
    def add(self, a, b, c):
        pl = self.plane(a, b, c)
        if pl is None: return
        nk, d, k = pl; T = [self.proj(p, k) for p in (a, b, c)]
        xs = [t[0] for t in T]; ys = [t[1] for t in T]
        for cx in range(int(math.floor(min(xs)/CELL)), int(math.floor(max(xs)/CELL))+1):
            for cy in range(int(math.floor(min(ys)/CELL)), int(math.floor(max(ys)/CELL))+1):
                self.bins[(nk, round(d*4), cx, cy)].append(T)
    def covered(self, a, b, c):
        pl = self.plane(a, b, c)
        if pl is None: return False
        nk, d, k = pl; T = [self.proj(p, k) for p in (a, b, c)]
        g = [(T[0][i]+T[1][i]+T[2][i])/3 for i in (0, 1)]
        samples = [g] + [(0.7*T[j][0]+0.3*g[0], 0.7*T[j][1]+0.3*g[1]) for j in range(3)]
        for s in samples:
            hit = False
            for dd in (-1, 0, 1):
                for U in self.bins.get((nk, round(d*4)+dd, int(math.floor(s[0]/CELL)), int(math.floor(s[1]/CELL))), ()):
                    if inside(s, U): hit = True; break
                if hit: break
            if not hit: return False
        return True
    def overlaps(self, a, b, c):
        """True when any of 16 interior sample points (centroid + a barycentric grid) lies
        inside a stored triangle on the same plane. Partial overlap is enough: used to mark
        triangles that would tie in depth with an earlier chunk's surface (worker 0054)."""
        pl = self.plane(a, b, c)
        if pl is None: return False
        nk, d, k = pl; T = [self.proj(p, k) for p in (a, b, c)]
        for w0, w1, w2 in OVERLAP_SAMPLES:
            s = (w0*T[0][0] + w1*T[1][0] + w2*T[2][0], w0*T[0][1] + w1*T[1][1] + w2*T[2][1])
            for dd in (-1, 0, 1):
                for U in self.bins.get((nk, round(d*4)+dd, int(math.floor(s[0]/CELL)), int(math.floor(s[1]/CELL))), ()):
                    if inside(s, U, -1e-3): return True
        return False
# centroid, the 10 interior points of a 1/6 barycentric grid, and 5 near-corner points
OVERLAP_SAMPLES = ([(1/3, 1/3, 1/3)] + [(i/6, j/6, 1 - (i+j)/6) for i in range(1, 6)
                   for j in range(1, 6) if i + j < 6] + [(.8, .1, .1), (.1, .8, .1), (.1, .1, .8),
                   (.45, .45, .1), (.1, .45, .45)])
def inside(p, T, eps=1e-3):
    (x1, y1), (x2, y2), (x3, y3) = T
    den = (y2-y3)*(x1-x3) + (x3-x2)*(y1-y3)
    if abs(den) < 1e-12: return False
    l1 = ((y2-y3)*(p[0]-x3) + (x3-x2)*(p[1]-y3)) / den
    l2 = ((y3-y1)*(p[0]-x3) + (x1-x3)*(p[1]-y3)) / den
    return l1 >= -eps and l2 >= -eps and 1-l1-l2 >= -eps
