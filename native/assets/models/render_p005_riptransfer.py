#!/usr/bin/env python3
"""Texture Vincent p005 by transferring UVs from the community rip (full model
coverage) onto the p005 geometry, then sampling the REAL dump atlases.

The GS dump (d03) only drew ~1/3 of Vincent's UVs, so it can't validate the torso.
The rip (Vincent.dae) is a full, correctly-UV'd model. We align p005 into rip space
(same similarity solve the classifier uses), transfer each p005 vertex the nearest
rip corner's UV + material-class, and sample the dump atlas for that class
(body=0x2e50, hair=0x3068, cloak=0x316c).

Run: python3 -I render_p005_riptransfer.py <dump.bin> <outdir> [--flipv]
"""
import sys, os
import numpy as np
import xml.etree.ElementTree as ET
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, "/srv/games/dirge-port/tools/gsdump/src")
import render_p005 as P
from render_p005_dumpbound import load_dump_atlases, BODY, HAIR, CLOAK

def rip_cloud_uv(dae=P.RIP, flipv=False):
    """Per-corner (pos, uv, class) from the rip DAE triangles."""
    tree = ET.parse(dae); root = tree.getroot()
    ns = root.tag.split('}')[0].strip('{')
    def f(t): return f"{{{ns}}}{t}"
    img = {im.get('id'): (im.find(f('init_from')).text if im.find(f('init_from')) is not None else None)
           for im in root.iter(f('image'))}
    eff = {}
    for e in root.iter(f('effect')):
        ch = None
        for t in [x.text for x in e.iter(f('init_from'))]:
            ch = img.get(t, t) if (t in img or t) else ch
        eff[e.get('id')] = ch
    mat = {m.get('id'): m.find(f('instance_effect')).get('url').lstrip('#')
           for m in root.iter(f('material'))}
    def cls(a):
        if a and a.startswith('p005_2'): return HAIR
        if a and a.startswith('p005_3'): return CLOAK
        return BODY
    pos = []; uv = []; cl = []
    for g in root.iter(f('geometry')):
        mesh = g.find(f('mesh'))
        if mesh is None: continue
        srcs = {s.get('id'): np.array([float(x) for x in s.find(f('float_array')).text.split()])
                for s in mesh.iter(f('source'))}
        vmap = {}
        for v in mesh.iter(f('vertices')):
            posrc = [i.get('source').lstrip('#') for i in v.findall(f('input'))
                     if i.get('semantic') == 'POSITION'][0]
            vmap[v.get('id')] = posrc
        for tri in mesh.iter(f('triangles')):
            inputs = tri.findall(f('input'))
            vsrc = usrc = None; voff = uoff = 0; stride = 0
            for i in inputs:
                off = int(i.get('offset')); stride = max(stride, off + 1)
                if i.get('semantic') == 'VERTEX':
                    vsrc = vmap[i.get('source').lstrip('#')]; voff = off
                elif i.get('semantic') == 'TEXCOORD':
                    usrc = i.get('source').lstrip('#'); uoff = off
            if vsrc is None or usrc is None: continue
            POS = srcs[vsrc].reshape(-1, 3); UV = srcs[usrc].reshape(-1, 2)
            c = cls(eff.get(mat.get(tri.get('material'))))
            p = [int(x) for x in tri.find(f('p')).text.split()]
            for k in range(0, len(p), stride):
                pi = p[k + voff]; ui = p[k + uoff]
                pos.append(POS[pi]); u, vv = UV[ui]
                uv.append((u, (1.0 - vv) if flipv else vv)); cl.append(c)
    return np.array(pos), np.array(uv), np.array(cl)

def solve_align(my_rf, rip_pos):
    """Similarity (swapxz + axis-sign) align p005 render-frame cloud to rip."""
    def norm(pts):
        c = pts.mean(0); q = pts - c; s = np.sqrt((q ** 2).sum(1).mean()); return q / s, c, s
    rip_n, _, _ = norm(rip_pos)
    _, myc, mys = norm(my_rf)
    rng = np.random.RandomState(0)
    ri = rip_n[rng.choice(len(rip_n), min(1500, len(rip_n)), replace=False)]
    mi = ((my_rf - myc) / mys)[rng.choice(len(my_rf), min(1500, len(my_rf)), replace=False)]
    best = None
    for swapxz in (False, True):
        for sx in (1, -1):
            for sz in (1, -1):
                T = mi.copy()
                if swapxz: T = T[:, [2, 1, 0]]
                T = T * np.array([sx, 1, sz])
                d = ((T[:, None, :] - ri[None, :, :]) ** 2).sum(2)
                ch = d.min(1).mean() + d.min(0).mean()
                if best is None or ch < best[0]: best = (ch, swapxz, sx, sz)
    ch, swapxz, sx, sz = best
    def to_rip(pts_rf):
        q = (pts_rf - myc) / mys
        if swapxz: q = q[:, [2, 1, 0]]
        return q * np.array([sx, 1, sz])
    return to_rip, best, rip_n

def main():
    dumppath, outdir = sys.argv[1], sys.argv[2]
    flipv = '--flipv' in sys.argv
    os.makedirs(outdir, exist_ok=True)
    atlases = load_dump_atlases(dumppath)
    for k in (BODY, HAIR, CLOAK): atlases.setdefault(k, np.zeros((16, 16, 3), np.float32))

    buf, NB = P.R.load_container(P.CONT)
    sk = P.load_skeleton(P.CONT, trans_scale=1.0); bc = sk['bone_count']
    Wm = [np.array(m, float) for m in sk['world_bind']]
    V, N, tris, gmeta = P.decode_groups(buf, NB, Wm, bc)

    rip_pos, rip_uv, rip_cls = rip_cloud_uv(flipv=flipv)
    print(f"rip corners: {len(rip_pos)}  flipv={flipv}")
    my_rf = V.copy(); my_rf[:, 1] *= -1.0
    to_rip, best, rip_n = solve_align(my_rf, rip_pos)
    print(f"align chamfer={best[0]:.4f} swapxz={best[1]} sx={best[2]} sz={best[3]}")

    # per p005 vertex: nearest rip corner -> UV (0..1) + class
    q = to_rip(my_rf)                      # p005 verts in normalised rip space
    vuv = np.zeros((len(V), 2)); vcls = np.zeros(len(V), int)
    CHUNK = 2000
    for i in range(0, len(q), CHUNK):
        sub = q[i:i + CHUNK]
        d = ((sub[:, None, :] - rip_n[None, :, :]) ** 2).sum(2)
        nn = d.argmin(1)
        vuv[i:i + CHUNK] = rip_uv[nn]; vcls[i:i + CHUNK] = rip_cls[nn]
    # rebuild tris with transferred UV (scaled to 4096 so render_multi's /4096 works)
    # and per-triangle atlas = majority class of its 3 verts.
    bound = []
    for t in tris:
        a, b, c = t[0], t[1], t[2]
        cl = np.bincount([vcls[a], vcls[b], vcls[c]], minlength=max(BODY, HAIR, CLOAK) + 1)
        # bincount index is the class VALUE (BODY/HAIR/CLOAK are the tbp0 ints) -> too large.
        # classes stored as tbp0 ints; map to small ids for vote.
        ids = [vcls[a], vcls[b], vcls[c]]
        atl = max(set(ids), key=ids.count)
        uv0 = (vuv[a] * 4096).tolist(); uv1 = (vuv[b] * 4096).tolist(); uv2 = (vuv[c] * 4096).tolist()
        bound.append((a, b, c, uv0, uv1, uv2, atl))
    from collections import Counter
    print("tri atlas:", {hex(k): v for k, v in Counter(t[6] for t in bound).items()})

    for ang, tag in [(18, 'front'), (55, '3q'), (200, 'back3q')]:
        P.render_multi(V, N, bound, atlases,
                       os.path.join(outdir, f"vincent_riptransfer_{tag}.png"), ang, 8)

if __name__ == '__main__':
    main()
