#!/usr/bin/env python3
"""Vincent p005 textured by transferring the rip's UVs AND its per-material atlas
onto the p005 geometry, sampling the rip's own atlas images (natural colour).

render_p005_riptransfer.py sampled the dump atlas 0x2e50, which is the RED variant
(== p005_1_c); the natural body atlas is p005_1_a, same UV layout. The dump proved
the layout; the rip's own atlases give the colour. This samples the rip atlas the
material actually names (p005_1_a..f / p005_2 / p005_3).

Run: python3 -I render_p005_riptransfer2.py <outdir>
"""
import sys, os
import numpy as np
import xml.etree.ElementTree as ET
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import render_p005 as P
from render_p005_riptransfer import solve_align

RIPDIR = os.path.dirname(P.RIP)

def rip_corners(dae=P.RIP, flipv=False):
    """Per-corner (pos, uv, atlas-filename) from the rip DAE."""
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
    def atlas_file(name):
        if not name: return 'p005_1_a.png'
        base = name.replace('_png', '.png')
        if not base.endswith('.png'): base += '.png'
        return base if os.path.exists(os.path.join(RIPDIR, base)) else 'p005_1_a.png'
    pos = []; uv = []; af = []
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
            vsrc = usrc = None; voff = uoff = 0; stride = 0
            for i in tri.findall(f('input')):
                off = int(i.get('offset')); stride = max(stride, off + 1)
                if i.get('semantic') == 'VERTEX': vsrc = vmap[i.get('source').lstrip('#')]; voff = off
                elif i.get('semantic') == 'TEXCOORD': usrc = i.get('source').lstrip('#'); uoff = off
            if vsrc is None or usrc is None: continue
            POS = srcs[vsrc].reshape(-1, 3); UV = srcs[usrc].reshape(-1, 2)
            afile = atlas_file(eff.get(mat.get(tri.get('material'))))
            p = [int(x) for x in tri.find(f('p')).text.split()]
            for k in range(0, len(p), stride):
                pos.append(POS[p[k + voff]]); u, vv = UV[p[k + uoff]]
                uv.append((u, (1.0 - vv) if flipv else vv)); af.append(afile)
    return np.array(pos), np.array(uv), np.array(af)

def main():
    outdir = sys.argv[1]
    flipv = '--flipv' in sys.argv
    os.makedirs(outdir, exist_ok=True)
    # load rip atlases
    files = sorted(set(os.listdir(RIPDIR)))
    atlases = {}
    for fn in files:
        if fn.lower().endswith('.png'):
            atlases[fn] = np.asarray(Image.open(os.path.join(RIPDIR, fn)).convert('RGB'), np.float32)
    print("rip atlases:", {k: v.shape for k, v in atlases.items()})

    buf, NB = P.R.load_container(P.CONT)
    sk = P.load_skeleton(P.CONT, trans_scale=1.0); bc = sk['bone_count']
    Wm = [np.array(m, float) for m in sk['world_bind']]
    V, N, tris, gmeta = P.decode_groups(buf, NB, Wm, bc)

    rip_pos, rip_uv, rip_af = rip_corners(flipv=flipv)
    my_rf = V.copy(); my_rf[:, 1] *= -1.0
    to_rip, best, rip_n = solve_align(my_rf, rip_pos)
    print(f"align chamfer={best[0]:.4f} swapxz={best[1]} sx={best[2]} sz={best[3]}  flipv={flipv}")

    q = to_rip(my_rf)
    vuv = np.zeros((len(V), 2)); vaf = np.empty(len(V), dtype=object)
    CH = 2000
    for i in range(0, len(q), CH):
        d = ((q[i:i+CH][:, None, :] - rip_n[None, :, :]) ** 2).sum(2)
        nn = d.argmin(1)
        vuv[i:i+CH] = rip_uv[nn]; vaf[i:i+CH] = rip_af[nn]
    bound = []
    for t in tris:
        a, b, c = t[0], t[1], t[2]
        ids = [vaf[a], vaf[b], vaf[c]]
        atl = max(set(ids), key=ids.count)
        bound.append((a, b, c, (vuv[a]*4096).tolist(), (vuv[b]*4096).tolist(),
                      (vuv[c]*4096).tolist(), atl))
    from collections import Counter
    print("tri atlas:", Counter(t[6] for t in bound))
    for ang, tag in [(18, 'front'), (55, '3q'), (200, 'back3q'), (235, 'backside')]:
        P.render_multi(V, N, bound, atlases,
                       os.path.join(outdir, f"vincent_rip2_{tag}.png"), ang, 8)

if __name__ == '__main__':
    main()
