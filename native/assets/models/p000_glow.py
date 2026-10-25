#!/usr/bin/env python3
"""Vincent (p000) gauntlet/boots glow pass (texture 0x2ee8), recovered from the GS dump.

What the game draws (hero dump 163235, last frame; every value read from the dump):
- 149 tris/frame with TEX0 0x2005cc8510102ee8: 16x16 CT24, TCC=1, MODULATE, FGE=1
  (fogged), ABE=1. Each one has exactly the screen XYZ of a body triangle drawn with
  TEX0 0x2005ba8621312cd4 (arm/boots texture, CLUT 0x2dd4) earlier in the same frame.
- ALPHA = (A=Cs, B=0, C=Ad, D=Cd), FIX 0:  Cd' = Cd + Cs*Ad/128   (additive).
- TEST 0x53001: ATE=1 ATST=NEVER AFAIL=RGB_ONLY (rgb written, no alpha, no Z),
  ZTE=1 ZTST=GEQUAL (same depth as the body triangle passes).
- FRAME is PSMCT32, so Ad = the alpha the body pass wrote there = As of the body draw
  = arm texel alpha (CLUT 0x2dd4) * vertex alpha 128 / 128 = the texel alpha (0..127).
  The arm texture's alpha channel is therefore the glow MASK.

Export: one extra character draw. Each glow triangle reuses OUR body triangle with the
same UVs (so it is depth-equal to what the body pass drew) and gets its own patch in a
small atlas, baked in barycentric space (both UV sets are linear in it):
  patch rgb = glow texel at the glow UV,  patch alpha = arm texel alpha at the body UV,
  stored as A*255/128 (shader As = t.a * va * 2 with va = 128/255, as for the cape).
Vertex colour = the dump's glow vertex colour /255 (prelit, this frame only).
The renderer draws it fogged, then blends (SRC_ALPHA, ONE) with colour-only writes and
depth LEQUAL, no depth write (render_scene_textured -charglow).
149 glow tris: 64 reuse one of our body triangles (same UV triple); 85 have body UVs our
decode lacks and sit at their dump position (world_to_model), drawn only where they are
depth-equal-or-in-front of our body (no bias: a bias let them spill past our boots).
"""
import sys, os, collections
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, "/srv/games/dirge-port/tools/gsdump/src")
from gsparse import read_header
from gsrender2 import build, pick_frame, tex0_fields
from char_dumpbind import tri_sig
from p000_cape import gs_to_world

GLOW_TBP = 0x2ee8
BODY_TBP = 0x2cd4
PATCH = 32          # texels per triangle patch
MARGIN = 2          # texels of extrapolated border (bilinear/mip bleed)
COLS = 16


def _bilinear(img, u, v):
    """Wrapped bilinear sample (GS REPEAT), img HxWxC float, u/v arrays in [0,1)."""
    h, w = img.shape[:2]
    x = u * w - 0.5; y = v * h - 0.5
    x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int); fx = (x - x0)[..., None]; fy = (y - y0)[..., None]
    g = lambda yy, xx: img[yy % h, xx % w]
    return (g(y0, x0) * (1 - fx) * (1 - fy) + g(y0, x0 + 1) * fx * (1 - fy)
            + g(y0 + 1, x0) * (1 - fx) * fy + g(y0 + 1, x0 + 1) * fx * fy)


def glow_pairs(dumppath):
    """Last frame: [(body_tex0, glow_tex0, [(body_uv, glow_uv, glow_rgba)]*3)] and the
    GS state of the glow draws (for the record)."""
    buf = open(dumppath, 'rb').read(); g = build(buf, read_header(buf)); a, b = pick_frame(g, 'last')
    body = {}; glow = []
    for i in range(a, b):
        A, B, C, ctx = g.tris[i]
        if not ctx['tme'] or ctx['fst']:
            continue
        tbp = tex0_fields(ctx['tex0'])['tbp0']
        key = lambda v: (v[0], v[1], v[2])
        if tbp == BODY_TBP:
            for v in (A, B, C):
                body.setdefault(key(v), []).append((ctx['tex0'], (v[3] / v[5], v[4] / v[5])))
        elif tbp == GLOW_TBP:
            glow.append((A, B, C, ctx))
    out = []; states = collections.Counter(); miss = 0
    for A, B, C, ctx in glow:
        states[(hex(ctx['tex0']), ctx['blend'], hex(ctx['test']), ctx['abe'])] += 1
        cands = [dict(body.get((v[0], v[1], v[2]), [])) for v in (A, B, C)]   # tex0 -> uv
        common = set(cands[0]) & set(cands[1]) & set(cands[2])
        if not common:
            miss += 1; continue
        bt = max(common)
        out.append((bt, ctx['tex0'], [(cands[k][bt], (v[3] / v[5], v[4] / v[5]), tuple(int(x) for x in v[8:12]),
                                       gs_to_world(v[0], v[1], v[5], ctx['xyoffset']))
                                      for k, v in enumerate((A, B, C))]))
    return out, dict(glow_tris=len(glow), paired=len(out), unpaired=miss,
                     gs_state={str(k): n for k, n in states.items()})


def build_glow(dumppath, tc, tris, tri_tex0, uvn, Pn, Nn, to_model):
    """Glow draw vertices (12 floats each, model space) + RGBA atlas + stats.
    A glow triangle whose body UVs are one of OUR body triangles reuses that
    triangle's vertices (depth-equal); otherwise (the dump's body UVs aren't in our
    decode) it takes the dump's own positions via to_model (world -> model space)."""
    pairs, st = glow_pairs(dumppath)
    q = lambda uv: (int(round(uv[0] * 512)), int(round(uv[1] * 512)))
    bysig = collections.defaultdict(list)
    for i, t in enumerate(tris):
        bysig[tri_sig(uvn(t))].append(i)
    rows = []  # (tri index, [corner k for dump corner 0..2], glow uvs, glow cols, body uvs, body tex0, glow tex0)
    nomesh = 0
    for btex0, gtex0, cs in pairs:
        cand = bysig.get(tri_sig([c[0] for c in cs]), [])
        cand = [i for i in cand if tri_tex0[i] == btex0] or cand
        hit = None
        for i in cand:
            m = {q(uv): k for k, uv in enumerate(uvn(tris[i]))}
            ks = [m.get(q(c[0])) for c in cs]
            if None not in ks and len(set(ks)) == 3:
                hit = (i, ks); break
        if hit is None:
            nomesh += 1
        rows.append((hit, cs, btex0, gtex0))
    n = len(rows)
    nrow = (n + COLS - 1) // COLS
    AW, AH = COLS * PATCH, max(nrow, 1) * PATCH
    atlas = np.zeros((AH, AW, 4), np.uint8)
    span = PATCH - 2 * MARGIN
    jj, ii = np.mgrid[0:PATCH, 0:PATCH]
    l1 = (ii + 0.5 - MARGIN) / span; l2 = (jj + 0.5 - MARGIN) / span; l0 = 1 - l1 - l2
    texc = {}
    flat = []
    for r, (hit, cs, btex0, gtex0) in enumerate(rows):
        for t0 in (btex0, gtex0):
            if t0 not in texc: texc[t0] = np.asarray(tc.get(t0), np.float64)
        bu = np.array([c[0] for c in cs]); gu = np.array([c[1] for c in cs])
        lam = np.stack([l0, l1, l2], -1)
        buv = lam @ bu; guv = lam @ gu
        rgb = _bilinear(texc[gtex0][:, :, :3], guv[..., 0], guv[..., 1])
        ad = _bilinear(texc[btex0][:, :, 3:4], buv[..., 0], buv[..., 1])[..., 0]
        px, py = (r % COLS) * PATCH, (r // COLS) * PATCH
        atlas[py:py + PATCH, px:px + PATCH, :3] = np.clip(np.round(rgb), 0, 255)
        atlas[py:py + PATCH, px:px + PATCH, 3] = np.clip(np.round(ad * 255 / 128), 0, 255)
        corner_uv = [((px + MARGIN) / AW, (py + MARGIN) / AH), ((px + MARGIN + span) / AW, (py + MARGIN) / AH),
                     ((px + MARGIN) / AW, (py + MARGIN + span) / AH)]
        if hit is not None:
            t = tris[hit[0]]
            P3 = [Pn[t[hit[1][c]]] for c in range(3)]; N3 = [Nn[t[hit[1][c]]] for c in range(3)]
        else:
            P3 = list(to_model(np.array([c[3] for c in cs])))
            fn = np.cross(P3[1] - P3[0], P3[2] - P3[0]); fn = fn / (np.linalg.norm(fn) or 1.0); N3 = [fn] * 3
        for c in range(3):   # keep the dump corner order (= patch corner order)
            col = cs[c][2]
            flat.extend((*P3[c], *N3[c], *corner_uv[c],
                         col[0] / 255.0, col[1] / 255.0, col[2] / 255.0, 128 / 255.0))   # C=Ad: vertex alpha unused
    st.update(tris=n, on_our_body_tri=n - nomesh, at_dump_position=nomesh, atlas=[AW, AH],
              mask_mean=round(float(atlas[..., 3].mean()) * 128 / 255, 1))
    return flat, atlas, st
