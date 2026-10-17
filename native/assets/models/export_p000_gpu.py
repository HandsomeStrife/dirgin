#!/usr/bin/env python3
"""Export the sea-of-flames Vincent (p000) to the GPU renderer's format:
a single DMSH mesh + one combined RGBA atlas, with each draw-group's UVs remapped
into its region of the combined atlas (body/hair/cape bound from the GS dump).

This lets render_scene_textured (GPU) draw Vincent in the z128 room with correct
UV sampling — unlike the hand-rolled software rasterizer. The long flowing cape is
procedural cloth (not in p000's mesh) and is intentionally absent.

Run: python3 -I export_p000_gpu.py <dump.bin> <outdir>
"""
import sys, os, glob, subprocess, tempfile, struct, array, collections
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, "/srv/games/dirge-port/tools/gsdump/src")
import render_p005 as P
from char_dumpbind import dump_draws, tri_sig
from gsrender2 import build as gbuild, TexCache, tex0_fields
from gsparse import read_header

CONT = "/srv/games/dirge-port/extract/_KEL/noPath/07700001.bin"
TEXC = "/srv/games/dirge-port/extract/_KEL/noPath/07700002.bin"
DTEX = "/srv/games/dirge-port/native/assets/textures/dtex"

def load_atlases():
    td = tempfile.mkdtemp(); subprocess.run([DTEX, TEXC, "-o", td], capture_output=True, timeout=60)
    at = {}
    for p in glob.glob(td + "/*.png"):
        nm = os.path.basename(p); im = np.asarray(Image.open(p).convert("RGB"), np.uint8)
        if im.shape[0] >= 32 and im.shape[1] >= 32:
            at[nm] = im
    return at

def sig16(a):
    im = Image.fromarray(a.astype("uint8")).resize((16, 16))
    v = np.asarray(im, np.float32).reshape(-1); return (v - v.mean()) / (v.std() + 1e-6)

def main():
    dumppath, outdir = sys.argv[1], sys.argv[2]; os.makedirs(outdir, exist_ok=True)
    P.CONT = CONT; P.TEXC = TEXC
    buf, NB = P.R.load_container(CONT); sk = P.load_skeleton(CONT, trans_scale=1.0); bc = sk["bone_count"]
    Wm = [np.array(m, float) for m in sk["world_bind"]]
    V, N, tris, gmeta = P.decode_groups(buf, NB, Wm, bc)
    print(f"p000: {len(gmeta)} groups, {len(V)} verts, {len(tris)} tris")

    # per-group -> dump TBP0 (UV-triple identity)
    sig2tbp, _ = dump_draws(dumppath)
    gvote = collections.defaultdict(collections.Counter)
    for t in tris:
        uvs = [(t[3][0]/4096, t[3][1]/4096), (t[4][0]/4096, t[4][1]/4096), (t[5][0]/4096, t[5][1]/4096)]
        hit = sig2tbp.get(tri_sig(uvs))
        if hit: gvote[t[6]][hit.most_common(1)[0][0]] += 1

    # map each dump TBP0 -> best on-disk p000 atlas
    at = load_atlases()
    hb = open(dumppath, "rb").read(); hh = read_header(hb); gg = gbuild(hb, hh); tc = TexCache(dumppath)
    tex0_of = {}
    for a, b, c, ctx in gg.tris:
        if ctx["tme"]: tex0_of.setdefault(tex0_fields(ctx["tex0"])["tbp0"], ctx["tex0"])
    asig = {k: sig16(v) for k, v in at.items()}
    body = next(k for k in at if "p000_1_a" in k)
    def atlas_of_tbp(tb):
        im = tc.get(tex0_of.get(tb)) if tb in tex0_of else None
        if im is None: return body
        ds = sig16(im[:, :, :3]); return max(at, key=lambda k: float(asig[k] @ ds))
    gkey = {}
    for gid in range(len(gmeta)):
        v = gvote.get(gid)
        gkey[gid] = atlas_of_tbp(v.most_common(1)[0][0]) if v else body
    print("group->atlas:", {k: v for k, v in collections.Counter(gkey.values()).items()})

    # build combined atlas: vertical stack of the distinct atlases used, width = max width
    used = sorted(set(gkey.values()))
    Wc = max(at[k].shape[1] for k in used)
    region = {}; yoff = 0; rows = []
    for k in used:
        im = at[k]; h, w = im.shape[:2]
        pad = np.zeros((h, Wc, 3), np.uint8); pad[:, :w] = im
        region[k] = (yoff, h, w); rows.append(pad); yoff += h
    combined = np.vstack(rows); Hc = combined.shape[0]
    print("combined atlas:", combined.shape, "regions:", {k: region[k] for k in used})

    # export DMSH: per-vertex pos3,nrm3,uv2(remapped),rgba4
    # p000's native mesh is already -Y-up (head at min-Y, feet at max-Y), matching
    # the z128 room. render_scene_textured.place_character (patched) pivots at the
    # feet = max-Y for -Y-up rooms and extends the body toward -Y, so NO flip here.
    Pn = V.astype(np.float64).copy()
    Nn = (N.astype(np.float64).copy() if N is not None and len(N) == len(V) else np.zeros_like(Pn))
    ln = np.linalg.norm(Nn, axis=1, keepdims=True); ln[ln < 1e-9] = 1.0; Nn = Nn / ln
    used_idx = sorted({i for t in tris for i in t[:3]})
    bbmin = Pn[used_idx].min(0); bbmax = Pn[used_idx].max(0)
    flat = []
    for t in tris:
        yo, rh, rw = region[gkey[t[6]]]
        for k in range(3):
            i = t[k]; u = t[3+k][0]/4096.0; v = t[3+k][1]/4096.0
            cu = (u * rw) / Wc                       # remap into combined atlas
            cv = (yo + v * rh) / Hc
            flat.extend((Pn[i][0], Pn[i][1], Pn[i][2], Nn[i][0], Nn[i][1], Nn[i][2],
                         cu, cv, 1.0, 1.0, 1.0, 1.0))
    vcount = len(tris) * 3
    hdr = struct.pack('<4sII', b'DMSH', 1, vcount) + struct.pack('<6f', *bbmin, *bbmax)
    mp = os.path.join(outdir, "vincent_p000.mesh")
    with open(mp, 'wb') as f: f.write(hdr); array.array('f', flat).tofile(f)
    # combined atlas as PNG + raw RGBA sidecar (render_scene_textured -t .rgba)
    Image.fromarray(combined, 'RGB').save(os.path.join(outdir, "vincent_p000_tex.png"))
    rgba = np.dstack([combined, np.full(combined.shape[:2], 255, np.uint8)])
    with open(os.path.join(outdir, "vincent_p000_tex.rgba"), 'wb') as f:
        f.write(struct.pack('<II', Wc, Hc)); f.write(rgba.astype(np.uint8).tobytes())
    print(f"wrote {mp} ({vcount} verts), vincent_p000_tex.png/.rgba ({Wc}x{Hc})")

if __name__ == '__main__':
    main()
