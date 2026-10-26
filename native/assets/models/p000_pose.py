#!/usr/bin/env python3
"""Pose Vincent's skinned body to the hero frame from the GS dump (no animation decode).

Same idea as the cape (p000_cape.py): the dump's body draws (0x2cd4 / 0x2de4) unproject
to this frame's posed world positions. Each body vertex is matched to dump corners by
UV (triangle-signature matches first, then same-texture vertex UV). A rigid transform
per BONE is solved from the matched vertices (bind pose, already placed) -> dump; it
poses every vertex of that bone, matched or not. A matched vertex takes its dump
position directly when it agrees with its bone's prediction (within TOL).
A bone is solved only with >= MIN_PTS matches whose 2nd principal spread is >= MIN_SPREAD;
otherwise it takes its twist from the nearest solved ancestor, keeping its own direction/position (colinear bones), or copies it outright (too few points) (stats list which bones and why).

Mirrored parts (legs, arms, boots) share UVs, so a vertex gets candidates from BOTH
sides of the body; nearest-to-prediction alone can put a whole bone on the wrong limb
(185138: the right thigh, bone 87, took the left thigh's points). So candidates are first
filtered by SIDE: the body midline plane is found from mirrored vertex pairs (two vertices
with the same candidate set), each vertex keeps its bind-pose side of it, and the plane
is carried into the frame by the vertex's nearest central ancestor bone (pelvis 81, chest 3).
Candidates on the other side are dropped when the vertex is clearly off the midline and
some candidate is on its own side; for limb bones, also when all of them are well across.
"""
import collections
import numpy as np
import render_skinned as R

TOL = 1.0          # world units (Vincent is ~19 tall): max dump-vs-bone disagreement
MIN_PTS = 4        # matched vertices needed to solve a bone on its own
MIN_SPREAD = 0.05  # min 2nd principal spread (RMS, world units) to solve a bone; below it
                   # the twist about the points' line is unconstrained. Data (frame 163235):
                   # 6 bones are exactly 0 (24,32,34,38,67,75); the next is 0.058 (bone 26) and
                   # the hand bones 53-61 are >= 0.087, so 0.05 sits in the gap.
SIDE_MARGIN = 0.1  # |bind offset| from the midline (world units) below which a vertex's
                   # side isn't trusted (no side filter). Legs sit at +-0.7..0.9.
STRICT = True      # also drop a limb vertex's candidates when ALL are across the midline
STRICT_MARGIN = 0.5  # ... by more than this. The other copy of a mirrored UV is often missing
                   # (hero/185126: 8 right-thigh vertices had only left-thigh candidates).
                   # Walking feet land near the midline, so near-midline candidates are kept:
                   # 0.1/0.3 dropped real left-shin points (fringe on bone 84); 0.5 still does
                   # a little in 185126. Central bones (torso) are never strict-filtered.
CENTRAL = 0.3      # a bone is "central" (carries the midline plane) if its vertices' mean
                   # bind offset is below this: 3,4,5,9,10,16,32,33,47,81; next is 91 at 0.5.


def slot_bones(buf, NB, bc, P, G):
    """Pool slot -> skinning bone, mirroring build_group_pool's batch walk."""
    piv = P + R._u32(buf, G + 0x0c)
    cnt = R._i32(buf, piv); cr = piv + R._i32(buf, piv + 8); raw = piv + R._i32(buf, piv + 0xc)
    out = {}
    for b in range(max(cnt, 0)):
        o = cr + b * 8
        if o + 8 > NB:
            break
        bone = R._s16(buf, o); num = buf[o + 2] + buf[o + 3]
        qwc = R._s16(buf, o + 4); stride = R._s16(buf, o + 6)
        if num == 0 or not (0 <= bone < bc) or qwc <= 0:
            raw += max(stride, 0); continue
        pos, nrm = R._parse_batch(buf, NB, raw, qwc, num); raw += stride
        if pos is None or nrm is None or len(nrm) < num:
            continue
        for s in ((nrm[:num, 3].astype(int) + 0x80) & 0xff):
            out[int(s)] = bone
    return out


def vertex_bones(buf, NB, bc, gmeta):
    """Global vertex index -> bone, for decode_groups' gmeta."""
    vb = {}
    for name, base, psize, _seen, _pool in gmeta:
        P = int(name.split('@')[1].split('.')[0], 16); gi = int(name.rsplit('.g', 1)[1])
        G = P + R._u32(buf, P + R._u32(buf, P + 0x20) + 4 * gi)
        for s, b in slot_bones(buf, NB, bc, P, G).items():
            if s < psize:
                vb[base + s] = b
    return vb


def _rigid(A, B):
    """R, t minimising |R a + t - b| (Kabsch)."""
    ca, cb = A.mean(0), B.mean(0)
    U, _, Vt = np.linalg.svd((A - ca).T @ (B - cb))
    D = np.diag([1, 1, np.sign(np.linalg.det(Vt.T @ U.T))])
    Rm = Vt.T @ D @ U.T
    return Rm, cb - Rm @ ca


def _spread(A):
    """RMS extent of A along its principal axes, largest first."""
    return np.linalg.svd(A - A.mean(0), compute_uv=False) / np.sqrt(len(A))


def _fit_line(Rp, A, B):
    """R, t for points on (or near) a line: the twist about the line comes from Rp,
    the line's direction and the centroid come from A -> B."""
    ca, cb = A.mean(0), B.mean(0); Rm = Rp
    if _spread(A)[0] >= MIN_SPREAD:
        da = np.linalg.svd(A - ca)[2][0]; db = np.linalg.svd(B - cb)[2][0]
        if ((A - ca) @ da) @ ((B - cb) @ db) < 0:
            db = -db
        u = Rp @ da; ax = np.cross(u, db); sn = np.linalg.norm(ax); cs = float(u @ db)
        if sn > 1e-9:
            K = np.array([[0, -ax[2], ax[1]], [ax[2], 0, -ax[0]], [-ax[1], ax[0], 0]]) / sn
            Rm = (np.eye(3) + sn * K + (1 - cs) * K @ K) @ Rp     # swing u -> db
    return Rm, cb - Rm @ ca


def _midline(placed, cands, vbone):
    """Body midline plane (n, h: n.x = h) in the bind (placed) frame, from mirrored
    vertex pairs: exactly two vertices with the same candidate set. None if too few."""
    g = collections.defaultdict(list)
    for v, cs in cands.items():
        if cs and v in vbone and v in placed:
            g[tuple(sorted({tuple(np.round(c, 2)) for c in cs}))].append(v)
    D, M = [], []
    for vs in g.values():
        if len(vs) == 2:
            a, b = placed[vs[0]], placed[vs[1]]
            if np.linalg.norm(a - b) > 0.2:
                D.append(a - b); M.append((a + b) / 2)
    if len(D) < 20:
        return None
    D = np.array(D); M = np.array(M); n = np.linalg.svd(D)[2][0]
    mir = abs(D @ n) / np.linalg.norm(D, axis=1) > 0.95     # true mirror pairs only
    if mir.sum() < 20:
        return None
    return n, float(np.median(M[mir] @ n))


def pose(placed, cands, vbone, parent, iters=4, src=None):
    """placed: {v: bind world pos (placed)}; cands: {v: [dump world pos]}.
    Returns (posed {v: pos}, rot {v: R}, stats). If `src` is a dict, it is filled
    with {v: 'dump' | 'bone' | ...}: which posed positions are the dump's own."""
    bones = sorted(set(vbone.values()))
    T = {b: (np.eye(3), np.zeros(3)) for b in bones}
    pred = lambda v: T[vbone[v]][0] @ placed[v] + T[vbone[v]][1]
    # side of the midline per vertex, and the central bone that carries the plane
    mid = _midline(placed, cands, vbone); vside, ref, central = {}, {}, set()
    if mid is not None:
        n, h = mid
        off = collections.defaultdict(list)
        for v in placed:
            if v in vbone:
                vside[v] = float(placed[v] @ n - h); off[vbone[v]].append(vside[v])
        central = {b for b, o in off.items() if abs(np.mean(o)) < CENTRAL}
        for b in bones:
            a = b
            while a >= 0 and a not in central:
                a = parent[a] if a < len(parent) else -1
            ref[b] = a if a >= 0 else None
    side_dropped = 0
    pick = {}
    for it in range(iters):
        # choose each vertex's candidate nearest its current prediction, among those on
        # the vertex's own side of the (posed) midline when the side is clear
        pick = {}; side_dropped = 0
        for v, cs in cands.items():
            if v not in vbone or not cs:
                continue
            if mid is not None and abs(vside[v]) > SIDE_MARGIN and ref[vbone[v]] is not None and len(cs) > 1:
                Rr, tr = T[ref[vbone[v]]]
                nr = Rr @ n; hr = h + nr @ tr                     # plane moved by the ref bone
                same = [c for c in cs if (c @ nr - hr) * vside[v] > 0]
                if same and len(same) < len(cs):
                    side_dropped += 1; cs = same
                elif STRICT and not same and vbone[v] not in central and \
                        all(abs(c @ nr - hr) > STRICT_MARGIN for c in cs):
                    side_dropped += 1; continue
            p = pred(v); d = [np.linalg.norm(c - p) for c in cs]; k = int(np.argmin(d))
            if it == 0 or d[k] < TOL * (3 if it == 1 else 1.5):
                pick[v] = cs[k]
        byb = collections.defaultdict(list)
        for v, c in pick.items():
            byb[vbone[v]].append(v)
        solved, flat = {}, {}
        for b, vs in byb.items():
            if len(vs) < MIN_PTS:
                continue
            A = np.array([placed[v] for v in vs]); B = np.array([pick[v] for v in vs])
            if _spread(A)[1] < MIN_SPREAD:          # (near-)colinear: twist unconstrained
                flat[b] = (A, B); continue
            Rm, t = _rigid(A, B)
            res = np.linalg.norm(A @ Rm.T + t - B, axis=1)
            keep = res <= max(np.percentile(res, 75), 1e-6) if len(vs) >= 8 else res >= 0
            if keep.sum() >= MIN_PTS and _spread(A[keep])[1] >= MIN_SPREAD:
                Rm, t = _rigid(A[keep], B[keep])
            solved[b] = (Rm, t)
        # unsolved bones copy the nearest solved ancestor; with none, bind pose (identity).
        # A colinear bone takes only its twist from there (_fit_line keeps the rest).
        copied, unposed = [], []
        for b in bones:
            if b in solved:
                T[b] = solved[b]; continue
            a = b
            while a >= 0 and a not in solved:
                a = parent[a] if a < len(parent) else -1
            base = solved[a] if a >= 0 else (np.eye(3), np.zeros(3))
            T[b] = _fit_line(base[0], *flat[b]) if b in flat else base
            (copied if a >= 0 else unposed).append(b)
    posed, rot = {}, {}
    how = collections.Counter()
    for v in placed:
        if v not in vbone:
            posed[v] = placed[v]; rot[v] = np.eye(3); how['no-bone'] += 1; continue
        p = pred(v); rot[v] = T[vbone[v]][0]
        c = pick.get(v)
        if c is not None and np.linalg.norm(c - p) < TOL:
            posed[v] = c; h = 'dump'
        else:
            posed[v] = p; h = 'bone' if not cands.get(v) else 'bone(dump rejected)'
        how[h] += 1
        if src is not None:
            src[v] = h
    # stats describe the final pass
    return posed, rot, dict(how=dict(how), bones=len(bones), bones_solved=len(solved),
                            bones_copied_parent=sorted(copied),
                            bones_colinear=sorted(flat), bones_unposed=sorted(unposed),
                            side_filtered=side_dropped)

