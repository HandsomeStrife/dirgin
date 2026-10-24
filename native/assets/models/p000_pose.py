#!/usr/bin/env python3
"""Pose Vincent's skinned body to the hero frame from the GS dump (no animation decode).

Same idea as the cape (p000_cape.py): the dump's body draws (0x2cd4 / 0x2de4) unproject
to this frame's posed world positions. Each body vertex is matched to dump corners by
UV (triangle-signature matches first, then same-texture vertex UV). A rigid transform
per BONE is solved from the matched vertices (bind pose, already placed) -> dump; it
poses every vertex of that bone, matched or not. A matched vertex takes its dump
position directly when it agrees with its bone's prediction (within TOL).
"""
import collections
import numpy as np
import render_skinned as R

TOL = 1.0          # world units (Vincent is ~19 tall): max dump-vs-bone disagreement
MIN_PTS = 4        # matched vertices needed to solve a bone on its own


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


def pose(placed, cands, vbone, parent, iters=4):
    """placed: {v: bind world pos (placed)}; cands: {v: [dump world pos]}.
    Returns (posed {v: pos}, rot {v: R}, stats)."""
    bones = sorted(set(vbone.values()))
    T = {b: (np.eye(3), np.zeros(3)) for b in bones}
    pred = lambda v: T[vbone[v]][0] @ placed[v] + T[vbone[v]][1]
    pick = {}
    for it in range(iters):
        # choose each vertex's candidate nearest its current prediction
        pick = {}
        for v, cs in cands.items():
            if v not in vbone or not cs:
                continue
            p = pred(v); d = [np.linalg.norm(c - p) for c in cs]; k = int(np.argmin(d))
            if it == 0 or d[k] < TOL * (3 if it == 1 else 1.5):
                pick[v] = cs[k]
        byb = collections.defaultdict(list)
        for v, c in pick.items():
            byb[vbone[v]].append(v)
        solved = {}
        for b, vs in byb.items():
            if len(vs) < MIN_PTS:
                continue
            A = np.array([placed[v] for v in vs]); B = np.array([pick[v] for v in vs])
            Rm, t = _rigid(A, B)
            res = np.linalg.norm(A @ Rm.T + t - B, axis=1)
            keep = res <= max(np.percentile(res, 75), 1e-6) if len(vs) >= 8 else res >= 0
            if keep.sum() >= MIN_PTS:
                Rm, t = _rigid(A[keep], B[keep])
            solved[b] = (Rm, t)
        # unsolved bones inherit the nearest solved ancestor (else keep identity)
        for b in bones:
            if b in solved:
                T[b] = solved[b]; continue
            a = b
            while a >= 0 and a not in solved:
                a = parent[a] if a < len(parent) else -1
            if a >= 0:
                T[b] = solved[a]
    posed, rot = {}, {}
    how = collections.Counter()
    for v in placed:
        if v not in vbone:
            posed[v] = placed[v]; rot[v] = np.eye(3); how['no-bone'] += 1; continue
        p = pred(v); rot[v] = T[vbone[v]][0]
        c = pick.get(v)
        if c is not None and np.linalg.norm(c - p) < TOL:
            posed[v] = c; how['dump'] += 1
        else:
            posed[v] = p; how['bone' if not cands.get(v) else 'bone(dump rejected)'] += 1
    nsolved = len({b for b in bones if any(vbone.get(v) == b for v in pick)})
    return posed, rot, dict(how=dict(how), bones=len(bones), bones_with_matches=nsolved)
