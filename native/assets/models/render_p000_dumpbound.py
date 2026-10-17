#!/usr/bin/env python3
"""Render the sea-of-flames Vincent = p000 (07700001 geom / 07700002 tex), bound
per-draw-group from the sea-of-flames GS dump via UV-triple identity.

p000 (NOT p005) is the Vincent used in zone z128 "sea of flames": p000 geometry
matches that dump's Vincent at ~39% (p005 was 1%). This decodes p000's geometry,
recovers each group's runtime texture by matching its UV-triples to the dump draws,
maps that dump TBP0 to the right p000 on-disk atlas (p000_1_a body / p000_2 hair /
p000_3 cape), and renders with p000's own atlases + on-disk UVs (so no dump v-flip /
colour-variant issues).

Run: python3 -I render_p000_dumpbound.py <dump.bin> <outdir>
"""
import sys, os, glob, subprocess, tempfile, collections
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, "/srv/games/dirge-port/tools/gsdump/src")
import render_p005 as P
from char_dumpbind import dump_draws, tri_sig
from gsrender2 import build as gbuild, TexCache, tex0_fields
from gsparse import read_header

CONT = "/srv/games/dirge-port/extract/_KEL/noPath/07700001.bin"   # p000 geometry
TEXC = "/srv/games/dirge-port/extract/_KEL/noPath/07700002.bin"   # p000 textures
DTEX = "/srv/games/dirge-port/native/assets/textures/dtex"

def load_p000_atlases():
    td = tempfile.mkdtemp()
    subprocess.run([DTEX, TEXC, "-o", td], capture_output=True, timeout=60)
    at = {}
    for p in glob.glob(td + "/*.png"):
        nm = os.path.basename(p)
        im = np.asarray(Image.open(p).convert("RGB"), np.float32)
        if im.shape[0] >= 32 and im.shape[1] >= 32:   # skip tiny CLUT images
            at[nm] = im
    return at

def sig16(a):
    im = Image.fromarray(a.astype("uint8")).resize((16, 16))
    v = np.asarray(im, np.float32).reshape(-1); return (v - v.mean()) / (v.std() + 1e-6)

def main():
    dumppath, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)

    # p000 geometry
    P.CONT = CONT; P.TEXC = TEXC
    buf, NB = P.R.load_container(CONT)
    sk = P.load_skeleton(CONT, trans_scale=1.0); bc = sk["bone_count"]
    Wm = [np.array(m, float) for m in sk["world_bind"]]
    V, N, tris, gmeta = P.decode_groups(buf, NB, Wm, bc)
    print(f"p000: {len(gmeta)} groups, {len(V)} verts, {len(tris)} tris")

    # per-group binding to a dump TBP0 via UV-triple identity
    sig2tbp, _ = dump_draws(dumppath)
    gvote = collections.defaultdict(collections.Counter)
    for t in tris:
        uvs = [(t[3][0] / 4096, t[3][1] / 4096), (t[4][0] / 4096, t[4][1] / 4096),
               (t[5][0] / 4096, t[5][1] / 4096)]
        hit = sig2tbp.get(tri_sig(uvs))
        if hit:
            gvote[t[6]][hit.most_common(1)[0][0]] += 1
    gtbp = {}
    for gid in range(len(gmeta)):
        v = gvote.get(gid)
        gtbp[gid] = v.most_common(1)[0][0] if v else None
    print("group->dump TBP0:", {g: hex(t) if t else None for g, t in gtbp.items()})

    # decode each bound dump texture, correlate to the best p000 atlas
    atlases = load_p000_atlases()
    print("p000 atlases:", {k: v.shape for k, v in atlases.items()})
    hbuf = open(dumppath, "rb").read(); hh = read_header(hbuf); gg = gbuild(hbuf, hh); tc = TexCache(dumppath)
    tex0_of = {}
    for a, b, c, ctx in gg.tris:
        if ctx["tme"]:
            tb = tex0_fields(ctx["tex0"])["tbp0"]
            tex0_of.setdefault(tb, ctx["tex0"])
    atl_sigs = {k: sig16(v) for k, v in atlases.items()}
    tbp2atlas = {}
    for tb in set(t for t in gtbp.values() if t is not None):
        im = tc.get(tex0_of.get(tb)) if tb in tex0_of else None
        if im is None:
            continue
        ds = sig16(im[:, :, :3])
        best = max(atlases, key=lambda k: float(atl_sigs[k] @ ds))
        tbp2atlas[tb] = best
        print(f"  dump {hex(tb)} -> {best} (corr {float(atl_sigs[best] @ ds):.2f})")

    # default atlas = p000_1_a (body). NO uv_span filter (it dropped the arm on this
    # low-poly model) and backface=False (both arms present).
    default = next((k for k in atlases if "p000_1_a" in k), list(atlases)[0])
    bound = []
    for t in tris:
        tb = gtbp[t[6]]
        key = tbp2atlas.get(tb, default)
        bound.append((t[0], t[1], t[2], t[3], t[4], t[5], key))
    from collections import Counter
    print("group atlas usage:", Counter(t[6] for t in bound))

    for ang, tag in [(18, "front"), (55, "3q"), (200, "back3q"), (235, "backside"), (90, "side")]:
        P.render_multi(V, N, bound, atlases,
                       os.path.join(outdir, f"vincent_p000_{tag}.png"), ang, 8, backface=False)

if __name__ == "__main__":
    main()
