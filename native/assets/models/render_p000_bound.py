#!/usr/bin/env python3
"""Render sea-of-flames Vincent (p000) with CORRECT per-triangle runtime texture
binding recovered from the GS dump.

Key facts (established 2026-10-18, dump-oracle verified):
  * The p000 decoder's UVs are CORRECT (coherent, mutation-validated) -- the old
    "red legs" was NOT a UV bug.
  * The body index texture TBP0 0x2cd4 is drawn with FOUR different CLUTs in one
    frame: CBP 0x2dd8 = RED (mantle/cloak), 0x2cd0/0x2ddc/0x2dd4 = GREY
    (body/armour/legs). "red vs grey" is a per-draw PALETTE choice, not a UV
    region. So binding must carry the full TEX0 (tbp0 + cbp), not just tbp0.
  * decode_groups emits 4 overlapping LOD families (L01/L02/S01/S02) that z-fight;
    the game draws one. We keep L01 (the dense high-detail family).

This binds each model triangle to the dump draw whose UV-triple it matches, and
decodes that draw's exact TEX0 (correct palette) via the gsdump TexCache. Unmatched
triangles fall back to their group's majority TEX0.

Run: python3 -I render_p000_bound.py <dump.bin> <outdir>
"""
import sys, os, collections
_REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))  # repo root
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _REPO + "/tools/gsdump/src")
import render_p005 as P
from gsparse import read_header
from gsrender2 import build, TexCache, tex0_fields
from char_dumpbind import tri_sig

# z128 loads Vincent's model from 07703201 (mesh) / 07703202 (textures), not 07700001/02:
# the sea-of-flames save state's RAM holds 07703201's image at 0x18dfe80 (37718 of 39492 words
# equal, header size field 0x26910) and the hero GS dump draws the envmaps at TBP 0x2ee8/0x2eec,
# CBP 0x2e64 = 07703202's VRAM layout (07700002 adds a 64x64 'p000_ta' eye texture before them).
# Same body/cape/skeleton bytes as 07700001 (every block 0xa0 earlier: shorter texanm header).
CONT = _REPO + "/extract/_KEL/noPath/07703201.bin"
TEXC = _REPO + "/extract/_KEL/noPath/07703202.bin"
QMIN = 1e-6

def dump_sig2tex0(dumppath):
    """UV-triple -> most-common full TEX0 (tbp0+cbp+psm) among dump draws."""
    buf = open(dumppath, 'rb').read(); h = read_header(buf); g = build(buf, h)
    sig2 = collections.defaultdict(collections.Counter)
    for a, b, c, ctx in g.tris:
        if not ctx['tme']:
            continue
        f = tex0_fields(ctx['tex0'])
        uvs = []
        if ctx['fst']:
            tw = 1 << f['tw']; th = 1 << f['th']
            for v in (a, b, c):
                uvs.append((v[6] / 16.0 / tw, v[7] / 16.0 / th))
        else:
            ok = True
            for v in (a, b, c):
                q = v[5]
                if abs(q) < QMIN:
                    ok = False; break
                uvs.append((v[3] / q, v[4] / q))
            if not ok:
                continue
        sig2[tri_sig(uvs)][ctx['tex0']] += 1
    return {k: v.most_common(1)[0][0] for k, v in sig2.items()}

def main():
    dumppath, outdir = sys.argv[1], sys.argv[2]; os.makedirs(outdir, exist_ok=True)
    P.CONT = CONT; P.TEXC = TEXC
    buf, NB = P.R.load_container(CONT); sk = P.load_skeleton(CONT, trans_scale=1.0)
    bc = sk['bone_count']; Wm = [np.array(m, float) for m in sk['world_bind']]
    V, N, tris, gmeta = P.decode_groups(buf, NB, Wm, bc)

    # keep the L01 LOD family (labels like "..p000_L01..") -- the game draws one LOD
    def fam(gid): return gmeta[gid][0].split('@')[0]
    fams = sorted({fam(g) for g in range(len(gmeta))})
    l01 = [f for f in fams if 'L01' in f] or fams[:1]
    keep = {g for g in range(len(gmeta)) if fam(g) in l01}
    tris = [t for t in tris if t[6] in keep]
    print(f"families: {fams}  keeping: {l01}  tris: {len(tris)}")

    sig2tex0 = dump_sig2tex0(dumppath)
    tc = TexCache(dumppath)

    # per-triangle TEX0 (match), with per-group majority fallback
    gvote = collections.defaultdict(collections.Counter)
    tri_tex0 = [None] * len(tris)
    for i, t in enumerate(tris):
        uvs = [(t[3][0]/4096, t[3][1]/4096), (t[4][0]/4096, t[4][1]/4096), (t[5][0]/4096, t[5][1]/4096)]
        tex0 = sig2tex0.get(tri_sig(uvs))
        tri_tex0[i] = tex0
        if tex0 is not None:
            gvote[t[6]][tex0] += 1
    gmaj = {g: v.most_common(1)[0][0] for g, v in gvote.items()}
    matched = sum(1 for x in tri_tex0 if x is not None)
    print(f"matched {matched}/{len(tris)} tris; group majority tex0: "
          + ", ".join(f"g{g}:{tex0_fields(t)['tbp0']:#x}/cbp{tex0_fields(t)['cbp']:#x}" for g, t in gmaj.items()))

    # build atlases keyed by tex0; bind each tri
    atlases = {}; bound = []
    for i, t in enumerate(tris):
        tex0 = tri_tex0[i]
        if tex0 is None:
            tex0 = gmaj.get(t[6])
        if tex0 is None:
            continue
        if tex0 not in atlases:
            im = tc.get(tex0)
            if im is None:
                continue
            atlases[tex0] = np.asarray(im[:, :, :3], np.float32)
        bound.append((t[0], t[1], t[2], t[3], t[4], t[5], tex0))
    print(f"distinct runtime textures used: {len(atlases)}; bound tris: {len(bound)}")

    for ang, tag in [(18, "front"), (55, "3q"), (90, "side"), (200, "back3q")]:
        P.render_multi(V, N, bound, atlases, os.path.join(outdir, f"vincent_bound_{tag}.png"),
                       ang, 6, backface=True)
    print("rendered ->", outdir)

if __name__ == '__main__':
    main()
