#!/usr/bin/env python3
"""Export sea-of-flames Vincent (p000) to the GPU renderer, bound and posed from the
hero-frame GS dump.

- Body: every skinned family (ALL by default). Each triangle gets the dump's full
  TEX0 (texture + CLUT), chosen per TRIANGLE, never by group majority:
    1. exact   - its UV-triple is drawn verbatim in the dump;
    2. uvmap   - its UVs fall in atlas texels the dump draws, and one CLUT owns
                 most of that coverage (per-texture UV-coverage map of the dump);
    3. neighbour - nearest dump-bound triangle sharing a vertex (same group);
    4. group   - group majority (last resort; counted, should be ~0).
- Cape: mesh block 0x19f70 (unskinned, positions posed at runtime). Topology, UVs
  and colours from the file; this frame's positions from the dump (p000_cape.py).
  Appended in model space by inverting the renderer's place_character().
- Vertex colour = vcol/255: the shader already does the PS2 MODULATE doubling
  (texel*vcol*2, renderer_gl.c), so 128 -> 0.5 -> neutral, 255 -> ~2x.
- Placement: scale/yaw/feet solved from matched body vertices (dump unprojected
  through the hero camera); written to placement.json for render_scene_textured.

Run: python3 -I export_p000_gpu2.py <dump.bin> <outdir>
"""
import sys, os, struct, array, collections, json
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, "/srv/games/dirge-port/tools/gsdump/src")
import render_p005 as P
from gsrender2 import TexCache, tex0_fields
from char_dumpbind import tri_sig
from render_p000_bound import CONT, TEXC, dump_sig2tex0
import p000_cape as C

UVRES = 128                     # UV-coverage map resolution (texels of a 256 atlas / 2)
UVMAP_MIN_SHARE = 0.6           # a CLUT must own >=60% of the sampled coverage


def uv_coverage(dtris):
    """Per tbp0: {tex0: coverage grid[UVRES,UVRES]} from the dump's UV triangles."""
    cov = collections.defaultdict(dict)
    ys, xs = np.mgrid[0:UVRES, 0:UVRES]; cx = (xs + 0.5) / UVRES; cy = (ys + 0.5) / UVRES
    for t0, vs in dtris:
        tbp = tex0_fields(t0)['tbp0']
        grid = cov[tbp].setdefault(t0, np.zeros((UVRES, UVRES), np.float32))
        (u0, v0), (u1, v1), (u2, v2) = (v[1] for v in vs)
        x0 = max(int(min(u0, u1, u2) * UVRES) - 1, 0); x1 = min(int(max(u0, u1, u2) * UVRES) + 2, UVRES)
        y0 = max(int(min(v0, v1, v2) * UVRES) - 1, 0); y1 = min(int(max(v0, v1, v2) * UVRES) + 2, UVRES)
        if x0 >= x1 or y0 >= y1:
            continue
        px = cx[y0:y1, x0:x1]; py = cy[y0:y1, x0:x1]
        d = (v1 - v2) * (u0 - u2) + (u2 - u1) * (v0 - v2)
        if abs(d) < 1e-12:
            continue
        l0 = ((v1 - v2) * (px - u2) + (u2 - u1) * (py - v2)) / d
        l1 = ((v2 - v0) * (px - u2) + (u0 - u2) * (py - v2)) / d
        inside = (l0 >= -0.02) & (l1 >= -0.02) & (1 - l0 - l1 >= -0.02)
        grid[y0:y1, x0:x1] += inside
    return cov


def uvmap_lookup(cov_tbp, uvs):
    """CLUT (full tex0) owning the coverage under a triangle's corners + centroid."""
    pts = list(uvs) + [tuple(np.mean(uvs, 0))]
    score = collections.Counter()
    for t0, grid in cov_tbp.items():
        for u, v in pts:
            score[t0] += grid[min(int(v * UVRES), UVRES - 1), min(int(u * UVRES), UVRES - 1)]
    tot = sum(score.values())
    if tot <= 0:
        return None
    t0, n = score.most_common(1)[0]
    return t0 if n / tot >= UVMAP_MIN_SHARE else None


def solve_placement(M, W, piv, iters=3, keep=0.7):
    """W ~= s*Ry(yaw)*(M-piv) + T, rotation about Y only (render_scene_textured
    place_character convention). Trimmed least squares against pose mismatch."""
    idx = np.arange(len(M))
    for _ in range(iters + 1):
        m = M[idx] - piv; w = W[idx]
        mc = m.mean(0); wc = w.mean(0); a = m - mc; b = w - wc
        th = np.arctan2((a[:, 0] * b[:, 2] - a[:, 2] * b[:, 0]).sum(), (a[:, 0] * b[:, 0] + a[:, 2] * b[:, 2]).sum())
        c, s_ = np.cos(th), np.sin(th)
        ar = np.c_[c * a[:, 0] - s_ * a[:, 2], a[:, 1], s_ * a[:, 0] + c * a[:, 2]]
        sc = (ar * b).sum() / (ar * ar).sum()
        T = wc - sc * np.array([c * mc[0] - s_ * mc[2], mc[1], s_ * mc[0] + c * mc[2]])
        def fwd(X):
            x = X - piv
            return sc * np.c_[c * x[:, 0] - s_ * x[:, 2], x[:, 1], s_ * x[:, 0] + c * x[:, 2]] + T
        res = np.linalg.norm(fwd(M) - W, axis=1)
        idx = np.argsort(res)[:max(int(len(M) * keep), 12)]
    # place_character: rx = cos(yaw)*px + sin(yaw)*pz  ==  standard rotation by -yaw
    return dict(s=float(sc), yaw=float(-np.degrees(th)), x=float(T[0]), y=float(T[1]), z=float(T[2]),
                rms_all=float(np.sqrt((res ** 2).mean())), rms_kept=float(np.sqrt((res[idx] ** 2).mean())),
                n=int(len(M)))


def world_to_model(Wp, pl, piv):
    """Inverse of place_character for one placement."""
    y = np.radians(pl['yaw']); cy, sy = np.cos(y), np.sin(y)
    r = np.asarray(Wp, float) - np.array([pl['x'], pl['y'], pl['z']])
    px = cy * r[:, 0] - sy * r[:, 2]; pz = sy * r[:, 0] + cy * r[:, 2]
    return np.c_[px, r[:, 1], pz] / pl['s'] + piv


def main():
    dumppath, outdir = sys.argv[1], sys.argv[2]; os.makedirs(outdir, exist_ok=True)
    P.CONT = CONT; P.TEXC = TEXC
    buf, NB = P.R.load_container(CONT); sk = P.load_skeleton(CONT, trans_scale=1.0)
    bc = sk['bone_count']; Wm = [np.array(m, float) for m in sk['world_bind']]
    V, N, tris, gmeta = P.decode_groups(buf, NB, Wm, bc)

    want = os.environ.get('FAM', 'ALL')
    def fam(g): return gmeta[g][0].split('@')[0]
    keep = set(range(len(gmeta))) if want == 'ALL' else {g for g in range(len(gmeta)) if want in fam(g)}
    tris = [t for t in tris if t[6] in keep]
    uvn = lambda t: [(t[3 + k][0] / 4096, t[3 + k][1] / 4096) for k in range(3)]

    # ---- 1. exact UV-triple binding
    sig2tex0 = dump_sig2tex0(dumppath); tc = TexCache(dumppath)
    tri_tex0 = [sig2tex0.get(tri_sig(uvn(t))) for t in tris]
    how = ['exact' if x is not None else None for x in tri_tex0]
    gvote = collections.defaultdict(collections.Counter)
    for t, x in zip(tris, tri_tex0):
        if x is not None: gvote[t[6]][x] += 1
    gmaj = {g: v.most_common(1)[0][0] for g, v in gvote.items()}
    gtbp = {g: collections.Counter(tex0_fields(x)['tbp0'] for x in v.elements()).most_common(1)[0][0]
            for g, v in gvote.items()}

    # ---- 2. UV-coverage lookup (texture = group's dump texture, CLUT per triangle)
    dtris = C.dump_tris(dumppath, set(gtbp.values()))
    cov = uv_coverage(dtris)
    for i, t in enumerate(tris):
        if tri_tex0[i] is None and t[6] in gtbp:
            x = uvmap_lookup(cov.get(gtbp[t[6]], {}), uvn(t))
            if x is not None: tri_tex0[i] = x; how[i] = 'uvmap'
    # ---- 3. nearest dump-bound neighbour (shared vertex, same group), BFS rounds
    for _ in range(4):
        byv = collections.defaultdict(list)
        for i, t in enumerate(tris):
            if how[i] in ('exact', 'uvmap'):
                for k in range(3): byv[t[k]].append(tri_tex0[i])
        changed = 0
        for i, t in enumerate(tris):
            if tri_tex0[i] is None:
                c = collections.Counter(x for k in range(3) for x in byv.get(t[k], []))
                if c: tri_tex0[i] = c.most_common(1)[0][0]; how[i] = 'neighbour'; changed += 1
        if not changed: break
    # ---- 4. group majority (last resort)
    for i, t in enumerate(tris):
        if tri_tex0[i] is None and t[6] in gmaj: tri_tex0[i] = gmaj[t[6]]; how[i] = 'group'
    hc = collections.Counter(h or 'unbound' for h in how)
    print("body binding per triangle:", dict(hc), "of", len(tris))

    # ---- placement from exact-matched corners (model bind pose -> dump world)
    used_idx = sorted({i for t in tris for i in t[:3]})
    Pn = V.astype(np.float64); bbmin = Pn[used_idx].min(0); bbmax = Pn[used_idx].max(0)
    piv = np.array([(bbmin[0] + bbmax[0]) / 2, bbmax[1], (bbmin[2] + bbmax[2]) / 2])  # -Y-up: feet = max Y
    q = lambda uv: (int(round(uv[0] * 512)), int(round(uv[1] * 512)))
    dsig = {}
    for t0, vs in dtris:
        dsig.setdefault(tri_sig([v[1] for v in vs]), {q(v[1]): v[0] for v in vs})
    Ms, Ws = [], []
    for i, t in enumerate(tris):
        if how[i] != 'exact': continue
        d = dsig.get(tri_sig(uvn(t)))
        if not d: continue
        for k, uv in enumerate(uvn(t)):
            w = d.get(q(uv))
            if w is not None: Ms.append(Pn[t[k]]); Ws.append(w)
    pl = solve_placement(np.array(Ms), np.array(Ws), piv)
    print("placement:", {k: round(v, 3) if isinstance(v, float) else v for k, v in pl.items()})

    # ---- cape (block 0x19f70) posed from the dump
    cape, cape_tex0, cst = C.posed_cape(buf, NB, dumppath)
    print("cape:", cst)

    # ---- atlas: every distinct runtime texture, stacked vertically
    used = []
    for x in tri_tex0 + [cape_tex0]:
        if x is not None and x not in used and tc.get(x) is not None: used.append(x)
    imgs = {x: np.asarray(tc.get(x)[:, :, :3], np.uint8) for x in used}
    Wc = max(im.shape[1] for im in imgs.values()); region = {}; rows = []; yoff = 0
    for x in used:
        im = imgs[x]; h, w = im.shape[:2]
        pad = np.zeros((h, Wc, 3), np.uint8); pad[:, :w] = im
        region[x] = (yoff, h, w); rows.append(pad); yoff += h
    combined = np.vstack(rows); Hc = combined.shape[0]

    Nn = (N.astype(np.float64) if N is not None and len(N) == len(V) else np.zeros_like(Pn))
    ln = np.linalg.norm(Nn, axis=1, keepdims=True); ln[ln < 1e-9] = 1.0; Nn = Nn / ln

    # ---- pose the body to this frame from the dump (p000_pose.py); POSE=0 = bind pose
    pose_stats = None
    if os.environ.get('POSE', '1') != '0':
        import p000_pose as PZ
        yr = np.radians(pl['yaw']); cy, sy = np.cos(yr), np.sin(yr)
        Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])      # place_character's yaw
        fwd = lambda X: pl['s'] * (np.asarray(X) - piv) @ Ry.T + np.array([pl['x'], pl['y'], pl['z']])
        placed = {v: fwd(Pn[v]) for v in used_idx}
        dall = collections.defaultdict(list)
        for t0, vs in dtris:
            dall[tri_sig([v[1] for v in vs])].append({q(v[1]): v[0] for v in vs})
        byuv = collections.defaultdict(list)
        for t0, vs in dtris:
            for w, uv, _c in vs: byuv[(t0, q(uv))].append(w)
        cands = collections.defaultdict(list); src = collections.Counter()
        for i, t in enumerate(tris):
            uvs = uvn(t)
            if how[i] == 'exact':
                for dmap in dall.get(tri_sig(uvs), []):
                    for k in range(3):
                        w = dmap.get(q(uvs[k]))
                        if w is not None: cands[t[k]].append(w)
            elif tri_tex0[i] is not None:
                for k in range(3):
                    cands[t[k]].extend(byuv.get((tri_tex0[i], q(uvs[k])), []))
        vb = PZ.vertex_bones(buf, NB, bc, gmeta)
        posed, rot, pose_stats = PZ.pose(placed, dict(cands), vb, sk['parent'])
        pose_stats['vertices'] = len(used_idx)
        pose_stats['with_dump_candidates'] = sum(1 for v in used_idx if cands.get(v))
        print("pose:", pose_stats)
        Rinv = Ry.T
        for v in used_idx:
            Pn[v] = world_to_model(posed[v][None], pl, piv)[0]
            Nn[v] = Rinv @ (rot[v] @ (Ry @ Nn[v]))
    flat = []; dropped = 0
    def emit(p, n, uv, col, x):
        yo, rh, rw = region[x]
        flat.extend((p[0], p[1], p[2], n[0], n[1], n[2], (uv[0] * rw) / Wc, (yo + uv[1] * rh) / Hc,
                     col[0] / 255.0, col[1] / 255.0, col[2] / 255.0, 1.0))
    for i, t in enumerate(tris):
        x = tri_tex0[i]
        if x is None or x not in region: dropped += 1; continue
        cols = (t[7], t[8], t[9]) if len(t) >= 10 else ((122, 122, 122, 128),) * 3
        for k in range(3): emit(Pn[t[k]], Nn[t[k]], uvn(t)[k], cols[k], x)
    nb = len(flat) // 12
    for ws, uvs, cols in cape:
        m = world_to_model(np.array(ws), pl, piv)
        n = np.cross(m[1] - m[0], m[2] - m[0]); n = n / (np.linalg.norm(n) or 1.0)
        for k in range(3): emit(m[k], n, uvs[k], cols[k], cape_tex0)
    vcount = len(flat) // 12
    print(f"body verts {nb} (dropped tris {dropped}), cape verts {vcount - nb}; atlas {Wc}x{Hc}, {len(used)} textures")
    # header bbox = BODY bbox: render_scene_textured pivots on it, and the cape was
    # mapped into model space against that same pivot.
    hdr = struct.pack('<4sII', b'DMSH', 1, vcount) + struct.pack('<6f', *bbmin, *bbmax)
    mp = os.path.join(outdir, "vincent_p000.mesh")
    with open(mp, 'wb') as f: f.write(hdr); array.array('f', flat).tofile(f)
    Image.fromarray(combined, 'RGB').save(os.path.join(outdir, "vincent_p000_tex.png"))
    rgba = np.dstack([combined, np.full(combined.shape[:2], 255, np.uint8)])
    with open(os.path.join(outdir, "vincent_p000_tex.rgba"), 'wb') as f:
        f.write(struct.pack('<II', Wc, Hc)); f.write(rgba.astype(np.uint8).tobytes())
    json.dump([dict(g=int(t[6]), name=gmeta[t[6]][0], tex0=(None if tri_tex0[i] is None else int(tri_tex0[i])),
                    how=how[i], uv=uvn(t), v=[int(k) for k in t[:3]]) for i, t in enumerate(tris)],
              open(os.path.join(outdir, "binding.json"), 'w'))
    json.dump(dict(placement=pl, binding=dict(hc), cape=cst, pose=pose_stats,
                   render_args=f"-s {pl['s']:.4f} -yaw {pl['yaw']:.2f} -x {pl['x']:.2f} -y {pl['y']:.2f} -z {pl['z']:.2f}"),
              open(os.path.join(outdir, "placement.json"), 'w'), indent=2)
    print(f"wrote {mp} ({vcount} verts) + placement.json")


if __name__ == '__main__':
    main()
