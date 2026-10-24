#!/usr/bin/env python3
"""render_p005.py -- Vincent (p005) MULTI-ATLAS character render (software).

Why this file exists
--------------------
`render_skinned.py` decodes the skinned mesh correctly (positions, per-vertex
UVs, connectivity via the game draw-descriptors) but samples EVERY draw-group
into a SINGLE texture atlas (`_find_body_texture` picks the largest decoded
image = p005_1_a, the body atlas). Vincent's model actually spans THREE distinct
native atlases in its texture container 07700142.bin:

    p005_1_a  256x512  -- BODY/face/armour/skin/hands (diffuse)
    p005_2    256x256  -- HAIR (long black strands)
    p005_3    256x128  -- the red CLOAK

The per-group -> atlas binding is NOT stored in the model file: every draw
sub-packet carries a byte-identical GIFtag (no TEX0/TBP0), and every group
header points at the SAME runtime texture-handle pointer (0x0022f868). It is
resolved in RAM by the loader at bind time -- the same runtime-binding shape
that makes the room texture binding unrecoverable from disk alone. So forcing
all groups into one atlas lands hair/cloak UV islands on the body atlas and
scrambles the torso.

This path reconstructs a multi-atlas render:
  1. decode the 3 real native atlases from 07700142.bin via dtex,
  2. assign each draw-GROUP an atlas (BODY/HAIR/CLOAK) using the community
     Vincent rip (Vincent.dae) as frame-independent ground truth -- the rip
     tags every part with its real atlas; we align the two vertex clouds and
     label each of our groups by nearest rip vertex (majority vote),
  3. drop the worst strip-bridging triangles (render_skinned still uses a pure
     triangle STRIP = 8441 tris vs the rip's 7327; the extra ~hundreds are
     restart/needle/bridge tris that smear atlas content across UV islands),
  4. rasterise each triangle against ITS group's atlas.

HONEST CAVEATS (do not over-claim):
  * The atlas assignment is a HEURISTIC derived by cloud alignment to the rip.
    The alignment is coarse (centroid/RMS + axis-sign search), so the BODY/HAIR
    boundary around the head/neck is uncertain. The authoritative binding needs
    the GS-dump oracle (tools/gsdump, UV-triple matching), not done here.
  * Connectivity is still render_skinned's STRIP; the UV-span drop is a
    mitigation, not the correct list/strip rule.
  * This is NOT claimed pixel-correct. Grade the images.

Usage:  python3 -I render_p005.py [out_dir]
"""
import sys, os, glob, tempfile, subprocess
import numpy as np
import math
from PIL import Image
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import render_skinned as R
from skd_skeleton import load_skeleton

CONT = "/srv/games/dirge-port/extract/_KEL/noPath/07700141.bin"
TEXC = "/srv/games/dirge-port/extract/_KEL/noPath/07700142.bin"
DTEX = "/srv/games/dirge-port/native/assets/textures/dtex"
RIP  = ("/tmp/claude-1000/-srv-games-dirge-port/"
        "9bdd650d-972a-4ab8-9c5a-73a606c34b4a/scratchpad/viprip/"
        "Vincent Valentine/Vincent.dae")

BODY, HAIR, CLOAK = 0, 1, 2
ATLAS_NAME = {BODY: "p005_1_a(BODY)", HAIR: "p005_2(HAIR)", CLOAK: "p005_3(CLOAK)"}


# --------------------------------------------------------------------------
def load_atlases(texc=TEXC):
    """Decode 07700142.bin and return [body(256x512), hair(256x256),
    cloak(256x128)] as float32 RGB arrays (shape H,W,3)."""
    out = {}
    with tempfile.TemporaryDirectory() as td:
        subprocess.run([DTEX, texc, "-o", td], capture_output=True, timeout=60)
        for png in glob.glob(os.path.join(td, "*.png")):
            name = os.path.basename(png)
            im = Image.open(png).convert("RGB")
            arr = np.asarray(im, dtype=np.float32)
            if "p005_1_a" in name:
                out[BODY] = arr
            elif "p005_2" in name:
                out[HAIR] = arr
            elif "p005_3" in name:
                out[CLOAK] = arr
    for k in (BODY, HAIR, CLOAK):
        if k not in out:
            raise SystemExit("missing atlas %s from %s" % (ATLAS_NAME[k], texc))
    return out


# --------------------------------------------------------------------------
def decode_groups(buf, NB, Wm, bc):
    """Group-aware decode. Returns (verts[N,3], norms[N,3], tris) where each
    tri = (a, b, c, uv0, uv1, uv2, gid). Mirrors render_skinned.assemble but
    keeps a per-triangle GROUP id so we can bind an atlas per group."""
    n = R._u16(buf, 0x10); offs = [R._u32(buf, 0x14 + 4 * i) for i in range(n)]
    smdblk = [(buf[o:o + 8].rstrip(b'\0').decode('latin1'), o + 0x10)
              for o in offs if buf[o + 8:o + 11] == b'smd']
    verts = []; norms = []; tris = []; gid = 0; gmeta = []
    for name, P in smdblk:
        if R._u32(buf, P) != R.SM2_MAGIC:
            print(f"WARN decode_groups: skipping block {name}@{P - 0x10:#x}: bad smd magic",
                  file=sys.stderr)
            continue
        sc = R._f32(buf, P + 0x2c) / 2048.0
        grparr = R._u32(buf, P + 0x20); gc = R._u16(buf, P + 0x0a)
        for gi in range(gc):
            G = P + R._u32(buf, P + grparr + 4 * gi)
            r = R.build_group_pool(buf, NB, Wm, bc, P, G, sc)
            if r is None:
                # e.g. p000's cape block (0x19f70): no skinning batches, positions
                # are posed at runtime -> not decodable here (see p000_cape.py).
                print(f"WARN decode_groups: skipping {name}@{P - 0x10:#x}.g{gi}: "
                      f"no skinned vertex pool (unskinned/runtime-posed group)", file=sys.stderr)
                continue
            pool, nrmp, seen = r
            base = len(verts); psize = len(pool)
            verts.extend([tuple(v) for v in pool])
            norms.extend([tuple(v) for v in nrmp])
            descs = R.enum_descs(buf, NB, P, G)
            ntri0 = len(tris)
            for di, (doff, vifptr) in enumerate(descs):
                start = P + vifptr
                end = (P + descs[di + 1][1]) if di + 1 < len(descs) else NB
                for sub in R.parse_drawpackets(buf, NB, start, end):
                    idx = sub.get('idx')
                    if not idx:
                        continue
                    uv = sub.get('uv'); prim = sub.get('prim'); col = sub.get('col')
                    for ja, jb, jc in R.sub_tris(idx, prim):  # LIST or STRIP per GIFtag
                        a, b, c = idx[ja], idx[jb], idx[jc]
                        if a == b or b == c or a == c:
                            continue
                        if max(a, b, c) >= psize or not (seen[a] and seen[b] and seen[c]):
                            continue
                        if uv is not None and len(uv) > max(ja, jb, jc):
                            uv0, uv1, uv2 = tuple(uv[ja]), tuple(uv[jb]), tuple(uv[jc])
                        else:
                            uv0 = uv1 = uv2 = (0.0, 0.0)
                        # per-corner RGBA (game vertex colour) at idx 7,8,9 (gid stays 6)
                        c0 = R._corner_col(col, ja); c1 = R._corner_col(col, jb); c2 = R._corner_col(col, jc)
                        tris.append([base + a, base + b, base + c, uv0, uv1, uv2, gid, c0, c1, c2])
            if len(tris) == ntri0:
                print(f"WARN decode_groups: {name}@{P - 0x10:#x}.g{gi} decoded 0 triangles "
                      f"({len(descs)} descriptors)", file=sys.stderr)
            gmeta.append((f"{name}@{P:x}.g{gi}", base, psize, seen, pool))
            gid += 1
    return (np.array(verts) if verts else np.zeros((0, 3)),
            np.array(norms) if norms else np.zeros((0, 3)), tris, gmeta)


# --------------------------------------------------------------------------
def rip_cloud(dae=RIP):
    """Per-vertex (pos, class) from the rip DAE. class in {BODY,HAIR,CLOAK}."""
    tree = ET.parse(dae); root = tree.getroot()
    nsU = root.tag.split('}')[0].strip('{')

    def f(t):
        return f"{{{nsU}}}{t}"
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
        if a and a.startswith('p005_2'):
            return HAIR
        if a and a.startswith('p005_3'):
            return CLOAK
        return BODY
    pts = []; cl = []
    for g in root.iter(f('geometry')):
        mesh = g.find(f('mesh'))
        if mesh is None:
            continue
        srcs = {s.get('id'): np.array([float(x) for x in s.find(f('float_array')).text.split()])
                for s in mesh.iter(f('source')) if s.find(f('float_array')) is not None}
        tri = mesh.find(f('triangles')) or mesh.find(f('polylist'))
        if tri is None:
            continue
        c = cls(eff.get(mat.get(tri.get('material'))))
        vtx = mesh.find(f('vertices'))
        possrc = [i.get('source').lstrip('#') for i in vtx.findall(f('input'))
                  if i.get('semantic') == 'POSITION'][0]
        POS = srcs[possrc].reshape(-1, 3)
        for p in POS:
            pts.append(p); cl.append(c)
    return np.array(pts), np.array(cl)


def classify_groups(gmeta, rip_pts, rip_cls, hair_thresh=0.35):
    """Assign each group an atlas by aligning our cloud to the rip and taking a
    majority vote of nearest-rip-vertex class. Returns {gid: atlas}."""
    def norm(pts):
        c = pts.mean(0); q = pts - c; s = np.sqrt((q ** 2).sum(1).mean()); return q / s, c, s
    my_all = np.vstack([pool[seen] for (_, _, _, seen, pool) in gmeta]).copy()
    my_rf = my_all.copy(); my_rf[:, 1] *= -1.0          # render-frame (Y up)
    rip_n, _, _ = norm(rip_pts)
    _, myc, mys = norm(my_rf)
    rng = np.random.RandomState(0)
    ri = rip_n[rng.choice(len(rip_n), min(1500, len(rip_n)), replace=False)]
    mi_src = (my_rf - myc) / mys
    mi = mi_src[rng.choice(len(mi_src), min(1500, len(mi_src)), replace=False)]
    best = None
    for swapxz in (False, True):
        for sx in (1, -1):
            for sz in (1, -1):
                T = mi.copy()
                if swapxz:
                    T = T[:, [2, 1, 0]]
                T = T * np.array([sx, 1, sz])
                d = ((T[:, None, :] - ri[None, :, :]) ** 2).sum(2)
                ch = d.min(1).mean() + d.min(0).mean()
                if best is None or ch < best[0]:
                    best = (ch, swapxz, sx, sz)
    ch, swapxz, sx, sz = best

    def to_rip(pts_rf):
        q = (pts_rf - myc) / mys
        if swapxz:
            q = q[:, [2, 1, 0]]
        return q * np.array([sx, 1, sz])
    assign = {}; table = []
    for gid, (lbl, base, psize, seen, pool) in enumerate(gmeta):
        v = pool[seen].copy(); v[:, 1] *= -1.0
        q = to_rip(v)
        k = rng.choice(len(q), min(80, len(q)), replace=False)
        votes = np.zeros(3, int)
        for p in q[k]:
            d = ((rip_n - p) ** 2).sum(1); votes[rip_cls[d.argmin()]] += 1
        tot = votes.sum()
        fr = votes / tot
        if fr[CLOAK] >= 0.50:
            a = CLOAK
        elif fr[HAIR] >= hair_thresh:
            a = HAIR
        else:
            a = BODY
        assign[gid] = a
        table.append((lbl, a, fr, tot))
    return assign, table, best


# --------------------------------------------------------------------------
def render_multi(V, N, tris, atlases, path, angle, elev, backface=True,
                 W_=900, H_=1200):
    """Software raster; each tri = (...,uv0,uv1,uv2,atlas_id). Samples its own
    atlas. Adapted from render_skinned.render."""
    Vr = V.copy(); Vr[:, 1] *= -1.0
    c = np.median(Vr, 0); rad = np.percentile(np.linalg.norm(Vr - c, axis=1), 98) * 1.7
    az, el = math.radians(angle), math.radians(elev)
    eye = c + rad * np.array([math.cos(el) * math.sin(az), math.sin(el),
                              math.cos(el) * math.cos(az)])
    fdir = c - eye; fdir /= np.linalg.norm(fdir)
    s = np.cross(fdir, [0, 1.0, 0]); s /= np.linalg.norm(s); u = np.cross(s, fdir)
    basis = np.array([s, u, -fdir])
    Vt = (Vr - eye) @ basis.T
    Nview = None
    if N is not None and len(N) == len(V):
        Nr = N.copy(); Nr[:, 1] *= -1.0; Nview = Nr @ basis.T
    fl = 1 / math.tan(math.radians(40) / 2); aspect = W_ / H_
    img = np.full((H_, W_, 3), 18, np.float32); zb = np.full((H_, W_), 1e9)
    L = np.array([0.3, 0.6, 0.7]); L /= np.linalg.norm(L)
    for t in tris:
        p = Vt[[t[0], t[1], t[2]]]
        if np.any(p[:, 2] > -0.02):
            continue
        gnrm = np.cross(p[1] - p[0], p[2] - p[0]); gn = np.linalg.norm(gnrm)
        if gn < 1e-9:
            continue
        if backface and gnrm[2] > 0:
            continue
        if Nview is not None:
            fn = Nview[[t[0], t[1], t[2]]].mean(0); fnn = np.linalg.norm(fn)
            fn = fn / fnn if fnn > 1e-9 else gnrm / gn
        else:
            fn = gnrm / gn
        pr = np.array([((fl / aspect * q[0] / -q[2] * 0.5 + 0.5) * W_,
                        (0.5 - fl * q[1] / -q[2] * 0.5) * H_, -q[2]) for q in p])
        sh = abs(np.dot(fn, L)) * 0.55 + 0.45
        minx, maxx = max(int(pr[:, 0].min()), 0), min(int(pr[:, 0].max()) + 1, W_)
        miny, maxy = max(int(pr[:, 1].min()), 0), min(int(pr[:, 1].max()) + 1, H_)
        if maxx <= minx or maxy <= miny:
            continue
        (x0, y0), (x1, y1), (x2, y2) = pr[0, :2], pr[1, :2], pr[2, :2]
        d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if abs(d) < 1e-9:
            continue
        ys, xs = np.mgrid[miny:maxy, minx:maxx]; xf, yf = xs + 0.5, ys + 0.5
        aa = ((y1 - y2) * (xf - x2) + (x2 - x1) * (yf - y2)) / d
        bb = ((y2 - y0) * (xf - x2) + (x0 - x2) * (yf - y2)) / d; g = 1 - aa - bb
        m = (aa >= 0) & (bb >= 0) & (g >= 0)
        if not m.any():
            continue
        z = aa * pr[0, 2] + bb * pr[1, 2] + g * pr[2, 2]
        sub = zb[miny:maxy, minx:maxx]; upd = m & (z < sub)
        if not upd.any():
            continue
        sub[upd] = z[upd]
        tex = atlases[t[6]]; TH, TW = tex.shape[0], tex.shape[1]
        uv0, uv1, uv2 = np.array(t[3]), np.array(t[4]), np.array(t[5])
        uu = (aa * uv0[0] + bb * uv1[0] + g * uv2[0]) / 4096.0
        vv = (aa * uv0[1] + bb * uv1[1] + g * uv2[1]) / 4096.0
        tx = np.clip((np.mod(uu, 1.0) * TW).astype(int), 0, TW - 1)
        tyv = np.clip((np.mod(vv, 1.0) * TH).astype(int), 0, TH - 1)
        col = tex[tyv, tx] * sh
        block = img[miny:maxy, minx:maxx]; block[upd] = col[upd]
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)
    print("wrote", path)


def render_classdebug(V, N, tris, assign, path, angle, elev):
    """Flat per-class colour so the atlas assignment is visually auditable:
    BODY=grey, HAIR=green, CLOAK=red."""
    COL = {BODY: np.array([190., 180., 170.]), HAIR: np.array([40., 200., 60.]),
           CLOAK: np.array([210., 40., 40.])}
    flat = [(t[0], t[1], t[2], COL[assign[t[6]]]) for t in tris]
    Vr = V.copy(); Vr[:, 1] *= -1.0
    c = np.median(Vr, 0); rad = np.percentile(np.linalg.norm(Vr - c, axis=1), 98) * 1.7
    az, el = math.radians(angle), math.radians(elev)
    eye = c + rad * np.array([math.cos(el) * math.sin(az), math.sin(el), math.cos(el) * math.cos(az)])
    fdir = c - eye; fdir /= np.linalg.norm(fdir)
    s = np.cross(fdir, [0, 1., 0]); s /= np.linalg.norm(s); u = np.cross(s, fdir)
    basis = np.array([s, u, -fdir]); Vt = (Vr - eye) @ basis.T
    Nr = N.copy(); Nr[:, 1] *= -1.0; Nview = Nr @ basis.T
    fl = 1 / math.tan(math.radians(40) / 2); W_, H_ = 900, 1200; aspect = W_ / H_
    img = np.full((H_, W_, 3), 18, np.float32); zb = np.full((H_, W_), 1e9)
    L = np.array([0.3, 0.6, 0.7]); L /= np.linalg.norm(L)
    for a3, b3, c3, basecol in flat:
        p = Vt[[a3, b3, c3]]
        if np.any(p[:, 2] > -0.02):
            continue
        gnrm = np.cross(p[1] - p[0], p[2] - p[0]); gn = np.linalg.norm(gnrm)
        if gn < 1e-9 or gnrm[2] > 0:
            continue
        fn = Nview[[a3, b3, c3]].mean(0); fnn = np.linalg.norm(fn)
        fn = fn / fnn if fnn > 1e-9 else gnrm / gn
        pr = np.array([((fl / aspect * q[0] / -q[2] * 0.5 + 0.5) * W_,
                        (0.5 - fl * q[1] / -q[2] * 0.5) * H_, -q[2]) for q in p])
        sh = abs(np.dot(fn, L)) * 0.6 + 0.4
        minx, maxx = max(int(pr[:, 0].min()), 0), min(int(pr[:, 0].max()) + 1, W_)
        miny, maxy = max(int(pr[:, 1].min()), 0), min(int(pr[:, 1].max()) + 1, H_)
        if maxx <= minx or maxy <= miny:
            continue
        (x0, y0), (x1, y1), (x2, y2) = pr[0, :2], pr[1, :2], pr[2, :2]
        d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if abs(d) < 1e-9:
            continue
        ys, xs = np.mgrid[miny:maxy, minx:maxx]; xf, yf = xs + 0.5, ys + 0.5
        aa = ((y1 - y2) * (xf - x2) + (x2 - x1) * (yf - y2)) / d
        bb = ((y2 - y0) * (xf - x2) + (x0 - x2) * (yf - y2)) / d; g = 1 - aa - bb
        m = (aa >= 0) & (bb >= 0) & (g >= 0)
        if not m.any():
            continue
        z = aa * pr[0, 2] + bb * pr[1, 2] + g * pr[2, 2]
        sub = zb[miny:maxy, minx:maxx]; upd = m & (z < sub)
        if not upd.any():
            continue
        sub[upd] = z[upd]
        block = img[miny:maxy, minx:maxx]; block[upd] = basecol * sh
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)
    print("wrote", path)


def uv_span(t):
    uv = np.array([t[3], t[4], t[5]]) / 4096.0
    return max(np.hypot(*(uv[i] - uv[j])) for i in range(3) for j in range(i + 1, 3))


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(outdir, exist_ok=True)
    buf, NB = R.load_container(CONT)
    sk = load_skeleton(CONT, trans_scale=1.0); bc = sk['bone_count']
    Wm = [np.array(m, float) for m in sk['world_bind']]
    V, N, tris, gmeta = decode_groups(buf, NB, Wm, bc)
    print("groups=%d verts=%d tris=%d" % (len(gmeta), len(V), len(tris)))
    atlases = load_atlases()
    for k in (BODY, HAIR, CLOAK):
        print("  atlas %-14s %dx%d" % (ATLAS_NAME[k], atlases[k].shape[1], atlases[k].shape[0]))
    rip_pts, rip_cls = rip_cloud()
    assign, table, best = classify_groups(gmeta, rip_pts, rip_cls)
    print("align chamfer=%.4f swapxz=%s sx=%d sz=%d" % (best[0], best[1], best[2], best[3]))
    print("\n== per-group atlas assignment (BODY/HAIR/CLOAK votes) ==")
    for (lbl, a, fr, tot) in table:
        print("  %-22s -> %-14s  B=%.2f H=%.2f C=%.2f (n=%d)"
              % (lbl, ATLAS_NAME[a], fr[BODY], fr[HAIR], fr[CLOAK], tot))
    from collections import Counter
    print("totals:", {ATLAS_NAME[k]: v for k, v in Counter(assign.values()).items()})

    # bind atlas per triangle + drop worst strip-bridge tris
    SPAN = 0.35
    bound = []
    dropped = 0
    for t in tris:
        if uv_span(t) > SPAN:
            dropped += 1; continue
        bound.append((t[0], t[1], t[2], t[3], t[4], t[5], assign[t[6]]))
    print("bridging tris dropped (uv-span>%.2f): %d/%d" % (SPAN, dropped, len(tris)))

    render_classdebug(V, N, tris, assign, os.path.join(outdir, "vincent_p005_classdebug_3q.png"), 55, 8)
    render_classdebug(V, N, tris, assign, os.path.join(outdir, "vincent_p005_classdebug_front.png"), 18, 6)
    render_multi(V, N, bound, atlases, os.path.join(outdir, "vincent_p005_multiatlas_front.png"), 18, 6)
    render_multi(V, N, bound, atlases, os.path.join(outdir, "vincent_p005_multiatlas_3q.png"), 55, 8)
    render_multi(V, N, bound, atlases, os.path.join(outdir, "vincent_p005_multiatlas_side.png"), 90, 6)


if __name__ == "__main__":
    main()
