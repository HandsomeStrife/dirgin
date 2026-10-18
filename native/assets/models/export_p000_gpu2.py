#!/usr/bin/env python3
"""Export sea-of-flames Vincent (p000) to the GPU renderer with CORRECT per-draw
runtime palette binding (red mantle / grey body, from the GS dump), L01 LOD only,
and the game's per-vertex colours.

Supersedes export_p000_gpu.py, which used the wrong single on-disk palette (brown)
and combined only 2 atlases. Here each triangle is bound to the exact dump TEX0
(tbp0 + cbp) it matches; the distinct runtime textures are decoded (correct
palettes) and packed into one combined atlas with remapped UVs. Mesh stays -Y-up
(z128 room) and carries the game vertex RGBA so the GPU MODULATE shades it.

Run: python3 -I export_p000_gpu2.py <dump.bin> <outdir>
"""
import sys, os, struct, array, collections
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, "/srv/games/dirge-port/tools/gsdump/src")
import render_p005 as P
from gsrender2 import TexCache, tex0_fields
from char_dumpbind import tri_sig
from render_p000_bound import CONT, TEXC, dump_sig2tex0

def main():
    dumppath, outdir = sys.argv[1], sys.argv[2]; os.makedirs(outdir, exist_ok=True)
    P.CONT = CONT; P.TEXC = TEXC
    buf, NB = P.R.load_container(CONT); sk = P.load_skeleton(CONT, trans_scale=1.0)
    bc = sk['bone_count']; Wm = [np.array(m, float) for m in sk['world_bind']]
    V, N, tris, gmeta = P.decode_groups(buf, NB, Wm, bc)

    # L01 LOD family only
    def fam(g): return gmeta[g][0].split('@')[0]
    keep = {g for g in range(len(gmeta)) if 'L01' in fam(g)}
    tris = [t for t in tris if t[6] in keep]

    # per-triangle runtime TEX0 (dump UV-triple match), per-group majority fallback
    sig2tex0 = dump_sig2tex0(dumppath); tc = TexCache(dumppath)
    gvote = collections.defaultdict(collections.Counter); tri_tex0 = [None]*len(tris)
    for i, t in enumerate(tris):
        uvs = [(t[3][0]/4096, t[3][1]/4096), (t[4][0]/4096, t[4][1]/4096), (t[5][0]/4096, t[5][1]/4096)]
        x = sig2tex0.get(tri_sig(uvs)); tri_tex0[i] = x
        if x is not None: gvote[t[6]][x] += 1
    gmaj = {g: v.most_common(1)[0][0] for g, v in gvote.items()}
    for i, t in enumerate(tris):
        if tri_tex0[i] is None: tri_tex0[i] = gmaj.get(t[6])

    # decode distinct runtime textures, pack into a vertical-stack combined atlas
    used = []
    for x in tri_tex0:
        if x is not None and x not in used and tc.get(x) is not None: used.append(x)
    imgs = {x: np.asarray(tc.get(x)[:, :, :3], np.uint8) for x in used}
    Wc = max(im.shape[1] for im in imgs.values())
    region = {}; rows = []; yoff = 0
    for x in used:
        im = imgs[x]; h, w = im.shape[:2]
        pad = np.zeros((h, Wc, 3), np.uint8); pad[:, :w] = im
        region[x] = (yoff, h, w); rows.append(pad); yoff += h
    combined = np.vstack(rows); Hc = combined.shape[0]
    print(f"L01 tris {len(tris)}; distinct runtime textures {len(used)}; atlas {combined.shape}")

    # mesh: pos(-Y-up, no flip), normal, remapped UV, vertex RGBA (game colour/255)
    Pn = V.astype(np.float64)
    Nn = (N.astype(np.float64) if N is not None and len(N) == len(V) else np.zeros_like(Pn))
    ln = np.linalg.norm(Nn, axis=1, keepdims=True); ln[ln < 1e-9] = 1.0; Nn = Nn/ln
    used_idx = sorted({i for t in tris for i in t[:3]})
    bbmin = Pn[used_idx].min(0); bbmax = Pn[used_idx].max(0)
    flat = []
    for i, t in enumerate(tris):
        x = tri_tex0[i]
        if x is None or x not in region: continue
        yo, rh, rw = region[x]
        cols = (t[7], t[8], t[9]) if len(t) >= 10 else ((122,122,122,128),)*3
        for k in range(3):
            vi = t[k]; u = t[3+k][0]/4096.0; v = t[3+k][1]/4096.0
            cu = (u*rw)/Wc; cv = (yo + v*rh)/Hc
            r, g, b, _a = cols[k]
            # GS modulate is texel*(col/128), 128 = neutral. Scale by /128 (clamp 1)
            # so the game's bright vertex colours don't darken the model.
            flat.extend((Pn[vi][0], Pn[vi][1], Pn[vi][2], Nn[vi][0], Nn[vi][1], Nn[vi][2],
                         cu, cv, min(r/128.0, 1.0), min(g/128.0, 1.0), min(b/128.0, 1.0), 1.0))
    vcount = len(flat)//12
    hdr = struct.pack('<4sII', b'DMSH', 1, vcount) + struct.pack('<6f', *bbmin, *bbmax)
    mp = os.path.join(outdir, "vincent_p000.mesh")
    with open(mp, 'wb') as f: f.write(hdr); array.array('f', flat).tofile(f)
    Image.fromarray(combined, 'RGB').save(os.path.join(outdir, "vincent_p000_tex.png"))
    rgba = np.dstack([combined, np.full(combined.shape[:2], 255, np.uint8)])
    with open(os.path.join(outdir, "vincent_p000_tex.rgba"), 'wb') as f:
        f.write(struct.pack('<II', Wc, Hc)); f.write(rgba.astype(np.uint8).tobytes())
    print(f"wrote {mp} ({vcount} verts), vincent_p000_tex.png/.rgba ({Wc}x{Hc})")

if __name__ == '__main__':
    main()
