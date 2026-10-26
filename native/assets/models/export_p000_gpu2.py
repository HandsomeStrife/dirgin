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

- Clip pose (optional): --clip NAME --frame F poses the body from a real animation
  clip frame (anim_rot.py: rotation, translation and scale tracks, composed like the
  game's VU0 micro) instead of the dump. Placement is re-solved for that pose. The
  dump pose is still computed as the reference and placement.json "clip" gets the
  vertex error vs it. --in-place drops the root motion (bone 1 x/z); --clip-cont picks
  the clip container; --no-cape leaves the cape out.
  Cape in --clip mode (p000_cloth.py): by default the SKINNED cape, i.e. the game's
  per-vertex cloth target (28 weighted skin records, bones 28-79 incl. the cape's own
  chains), posed with the same bone matrices (and root-motion setting) as the body.
  --cape-cloth uses the game's cloth sim instead (run from clip frame 0 to --frame,
  last frame). Without --clip the cape stays dump-posed (unchanged).

- Other dumps (optional): --dump-camera CAM.json for a dump taken from another camera
  (same room). JSON: eye, at, up, fov (fitted to the dump's room vertices, see
  fit_dump_camera.py) and Vincent's TBPs in that dump: body_tbp, cape_tbp, glow_tbp
  (VRAM layout differs per capture). Without it: the hero camera/TBPs (unchanged).

Run: python3 -I export_p000_gpu2.py <dump.bin> <outdir> [--clip h_swtch --frame 58]
"""
import sys, os, struct, array, collections, json
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, "/srv/games/dirge-port/tools/gsdump/src")
import render_p005 as P
from gsrender2 import TexCache, tex0_fields
import char_dumpbind
from char_dumpbind import tri_sig
# UVs are 1/4096 fixed point in the file (V2-16) and in the dump (s/q, float noise ~1e-4).
# Matching on char_dumpbind's default 1/512 grid put ~24% of our UVs exactly on a rounding
# half, where round() and the dump's noise disagree: 40% of the dump's body tris (85 of the
# 149 glow tris) found no UV-triple match. Match on the native grid instead.
UVQ = char_dumpbind.ROUND = 4096
from render_p000_bound import CONT, TEXC, dump_sig2tex0
import p000_cape as C
import anim_rot as AR

CLIP_CONT = "/srv/games/dirge-port/extract/_KEL/noPath/0770000b.bin"   # h_* (handgun) clips

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


# Cape GS alpha test (dump: TEST ATE=1 ATST=GEQUAL AREF=100 AFAIL=RGB_ONLY), in the
# renderer's 0..1 As scale (As = 1.0 at GS 128); same rounding as export_z128_room.py.
CAPE_AREF_GL = round((100 - 0.5) / 128, 5)


def world_to_model(Wp, pl, piv):
    """Inverse of place_character for one placement."""
    y = np.radians(pl['yaw']); cy, sy = np.cos(y), np.sin(y)
    r = np.asarray(Wp, float) - np.array([pl['x'], pl['y'], pl['z']])
    px = cy * r[:, 0] - sy * r[:, 2]; pz = sy * r[:, 0] + cy * r[:, 2]
    return np.c_[px, r[:, 1], pz] / pl['s'] + piv


def umeyama(A, B):
    """s, R, t minimising |s R a + t - b| (similarity)."""
    ca, cb = A.mean(0), B.mean(0); a = A - ca; b = B - cb
    U, D, Vt = np.linalg.svd(b.T @ a / len(A))
    S = np.diag([1, 1, np.sign(np.linalg.det(U @ Vt))])
    Rm = U @ S @ Vt; sc = np.trace(np.diag(D) @ S) / ((a ** 2).sum() / len(A))
    return sc, Rm, cb - sc * Rm @ ca


def err_stats(X, D):
    e = np.linalg.norm(X - D, axis=1)
    return dict(median=round(float(np.median(e)), 3), rms=round(float(np.sqrt((e ** 2).mean())), 3),
                p90=round(float(np.percentile(e, 90)), 3), max=round(float(e.max()), 3), n=int(len(e)))


def cape_normals(tris_m, flat=False, quant=1e-3):
    """Per-corner cape normals from the posed positions. Smooth (default): the
    area-weighted face normals of every cape triangle sharing the corner's position,
    each flipped to agree with this triangle's own face normal first, so the front and
    back copies (same positions, opposite winding) don't cancel and each keeps its own
    side. flat=True: the old per-triangle face normal (lit cape shows flat facets)."""
    fn = [np.cross(m[1] - m[0], m[2] - m[0]) for m in tris_m]          # area-weighted
    unit = lambda v: v / (np.linalg.norm(v) or 1.0)
    if flat:
        return [[unit(n)] * 3 for n in fn]
    key = lambda p: tuple(np.round(np.asarray(p) / quant).astype(np.int64))
    share = collections.defaultdict(list)
    for i, m in enumerate(tris_m):
        for k in range(3): share[key(m[k])].append(i)
    out = []
    for i, m in enumerate(tris_m):
        own = fn[i]; ns = []
        for k in range(3):
            acc = np.zeros(3)
            for j in share[key(m[k])]:
                acc += fn[j] if np.dot(fn[j], own) >= 0 else -fn[j]
            ns.append(unit(acc if np.linalg.norm(acc) > 1e-12 else own))
        out.append(ns)
    return out


def clip_pose_cape(args, sk, cbuf, CP, trs, cape, pl, piv):
    """Cape triangles in the clip-posed model space (same order/UVs as `cape`, the
    dump-posed cape) + stats. Skinned target by default, cloth sim with --cape-cloth."""
    import p000_cloth as CL
    cp = CL.Cape()
    if [u for _v, u, _c in cp.tris] != [tuple(tuple(x) for x in u) for _w, u, _c in cape]:
        raise ValueError("p000_cloth cape triangles do not match p000_cape's order/UVs")
    root = not args['in_place']
    if args['cape_cloth']:
        f = args['frame']; frames = [float(x) for x in np.arange(f % 1.0, f + 1e-6, 1.0)]
        pos, tg, ks, info = CL.run_clip(cp, sk, args['clip'], frames, cont=args['cont'], in_place=args['in_place'])
        P = pos[-1]; dev = np.linalg.norm(P - tg[-1], axis=1)
        st = dict(mode='cloth', ticks=len(frames), k=round(float(ks[-1]), 3), window=info['window'],
                  offset_from_skinned=err_stats(P, tg[-1]))
    else:
        P = cp.targets(AR.compose(sk, trs, root_motion=root))
        st = dict(mode='skinned')
    tm = [P[list(v)] for v, _u, _c in cp.tris]
    # vs the dump-posed cape (world units; dump corners are UV-matched, some interpolated)
    D = np.concatenate([world_to_model(np.array(ws), pl, piv) for ws, _u, _c in cape])
    st['vs_dump_cape'] = err_stats(np.concatenate(tm) * pl['s'], D * pl['s'])
    return tm, st


def parse_args(argv):
    a = dict(dump=None, out=None, clip=None, frame=0.0, cont=CLIP_CONT, in_place=False, no_cape=False,
             cape_flat=False, cape_cloth=False, dump_camera=None, bind_from_file=False)
    pos = []; i = 0
    while i < len(argv):
        k = argv[i]
        if k == '--clip': a['clip'] = argv[i + 1]; i += 2
        elif k == '--frame': a['frame'] = float(argv[i + 1]); i += 2
        elif k == '--clip-cont': a['cont'] = argv[i + 1]; i += 2
        elif k == '--in-place': a['in_place'] = True; i += 1
        elif k == '--no-cape': a['no_cape'] = True; i += 1
        elif k == '--cape-flat-normals': a['cape_flat'] = True; i += 1
        elif k == '--cape-cloth': a['cape_cloth'] = True; i += 1
        elif k == '--dump-camera': a['dump_camera'] = argv[i + 1]; i += 2
        elif k == '--bind-from-file': a['bind_from_file'] = True; i += 1
        elif k.startswith('--'): sys.exit(f"unknown option {k}")
        else: pos.append(k); i += 1
    if len(pos) != 2: sys.exit(__doc__)
    a['dump'], a['out'] = pos
    return a


class DrawTimeTex:
    """TexCache.get(tex0) as the texture was when the last frame drew with it: the live
    upload at that draw, not TexCache's legacy 'largest upload anywhere'. In walk dump
    185126 the arm/boots CLUT (0x2f60) and head CLUT (0x2e5c) are overwritten later in
    the frame by 64x128 / 128x128 textures, which the legacy pick took (garbage palettes).
    TEX0s never drawn fall back to the legacy pick."""
    def __init__(self, dumppath):
        from gsparse import read_header
        from gsrender2 import build
        buf = open(dumppath, 'rb').read(); g = build(buf, read_header(buf))
        last = max(ctx['vframe'] for *_x, ctx in g.tris)
        self.seq = {}
        for i, (_a, _b, _c, ctx) in enumerate(g.tris):
            if ctx['vframe'] == last and ctx['tme']:
                self.seq.setdefault(ctx['tex0'], i)
        self.tc = TexCache(dumppath, uploads=g.uploads)

    def get(self, tex0):
        return self.tc.get(tex0, self.seq.get(tex0))


def use_dump_camera(path):
    """Point every dump reader at another capture: its camera (HERO_CAM is updated in
    place, so gs_to_world/world_to_drawable defaults follow) and its Vincent TBPs."""
    import p000_glow as GL
    j = json.load(open(path))
    C.HERO_CAM.update(eye=tuple(j['eye']), at=tuple(j['at']), up=tuple(j['up']), fov=j['fov'])
    C.CAPE_TBP = j['cape_tbp']; GL.GLOW_TBP = j['glow_tbp']; GL.BODY_TBP = j['body_tbp']
    global BIND_REF
    BIND_REF = j.get('bind_ref')


# Optional (--dump-camera JSON "bind_ref"): binding.json of the hero export. A triangle's
# CLUT is a property of the model, not of the capture, but a capture from another angle
# has less dump evidence (walk 185138: 63 group-majority tris vs 15, 184 tris on another
# CLUT than the hero, 37 of them group-bound where the hero has an exact UV match).
BIND_REF = None
BIND_RANK = {'exact': 0, 'uvmap': 1, 'neighbour': 2, 'group': 3, None: 4}


def apply_bind_ref(tris, tri_tex0, how, path):
    """Where the reference has stronger evidence (exact/uvmap over this capture's
    uvmap/none), take its CLUT, translated to this capture's TEX0 through the TEX0
    pairs of triangles bound 'exact' in both. Returns counts."""
    ref = json.load(open(path))
    if len(ref) != len(tris) or any(r['v'] != [int(k) for k in t[:3]] for r, t in zip(ref, tris)):
        raise ValueError("bind_ref triangles do not match this decode")
    m = collections.defaultdict(collections.Counter)
    for r, x, h in zip(ref, tri_tex0, how):
        if r['how'] == 'exact' and h == 'exact': m[r['tex0']][x] += 1
    xl = {k: v.most_common(1)[0][0] for k, v in m.items()}
    n = collections.Counter()
    for i, r in enumerate(ref):
        if r['how'] in ('exact', 'uvmap') and BIND_RANK[r['how']] < BIND_RANK[how[i]] and r['tex0'] in xl:
            n[f"{how[i]}->ref_{r['how']}"] += 1
            tri_tex0[i] = xl[r['tex0']]; how[i] = 'ref_' + r['how']
    return dict(n)


def bind_from_file(buf, NB, Wm, bc, tris, tri_tex0, how):
    """--bind-from-file: each triangle's TEX0 (texture + CLUT) from the game files
    (smd_material.py: draw-descriptor material byte -> FUN_00259058's material table
    over 07700002.bin). Replaces tri_tex0 in place; `how` (the dump evidence) is kept
    for placement/pose. VRAM addresses are runtime: the table is laid out from this
    capture's body TBP - 4 (p000_glow.BODY_TBP, set by --dump-camera). Keys stay the
    dump's TEX0 ints where the dump drew that material (so the glow/pose lookups by
    TEX0 work); textures come from the file. Returns stats + cape/texture hooks."""
    import smd_material as SM
    import p000_glow as GL
    mats = SM.material_table(TEXC, 0, GL.BODY_TBP - 4)
    tm = SM.tri_materials(buf, NB, Wm, bc, tris)
    K = ('tbp0', 'tbw', 'psm', 'tw', 'th', 'cbp', 'cpsm')
    fkey = lambda f: tuple(f[k] for k in K)
    dump_key = {}
    for x in tri_tex0:
        if x is not None: dump_key.setdefault(fkey(tex0_fields(x)), x)
    key_mat = {}
    def key(m):
        k = dump_key.get(fkey(mats[m]['fields']), mats[m]['tex0']); key_mat[k] = m
        return k
    st = collections.Counter(); per_how = collections.Counter(); old = list(tri_tex0)
    for i, (m, _fl) in enumerate(tm):
        if m == 0xff or m >= len(mats) or mats[m].get('ipu'):
            st['no_material'] += 1; continue
        tri_tex0[i] = key(m)
        same = old[i] is not None and fkey(tex0_fields(old[i])) == fkey(mats[m]['fields'])
        per_how[(how[i] or 'unbound', 'agree' if same else ('was_none' if old[i] is None else 'differs'))] += 1
    stats = dict(materials=dict(sorted(collections.Counter(m for m, _ in tm).items())),
                 vs_dump_binding={f"{h}:{a}": n for (h, a), n in sorted(per_how.items())}, **st)

    def cape(cape_tex0):
        m, _fl = SM.block_material(buf, 0x19f70)          # p000_L02 cape block
        k = key(m)
        stats['cape'] = dict(material=m, agrees_with_dump=fkey(tex0_fields(cape_tex0)) == fkey(mats[m]['fields']))
        return k

    def textures(tc):
        cache = {}
        class FileTex:
            def get(self, x):
                if x not in key_mat: return tc.get(x)
                if x not in cache:
                    rgba = SM.texture_rgba(TEXC, mats[key_mat[x]]).astype(np.float64)
                    # dtex alpha is a*255/128; the dump (and the cape alpha code) use GS 0..128
                    rgba[:, :, 3] = np.round(rgba[:, :, 3] * 128 / 255)
                    cache[x] = rgba
                return cache[x]
        ft = FileTex(); tst = {}
        for x, m in sorted(key_mat.items(), key=lambda kv: kv[1]):
            a, d = ft.get(x), tc.get(x)
            tst[m] = (f"{mats[m]['image_name']}+{mats[m]['name']}: "
                      + ("no dump texture" if d is None or d.shape[:2] != a.shape[:2] else
                         "max|file-dump| rgb %d a %d" % (np.abs(a[:, :, :3] - np.asarray(d[:, :, :3], float)).max(),
                                                          np.abs(a[:, :, 3] - np.asarray(d[:, :, 3], float)).max())))
        stats['textures'] = tst
        return ft
    return dict(stats=stats, cape=cape, textures=textures, mat=[m for m, _ in tm])


def main():
    args = parse_args(sys.argv[1:])
    dumppath, outdir = args['dump'], args['out']; os.makedirs(outdir, exist_ok=True)
    if args['dump_camera']: use_dump_camera(args['dump_camera'])
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
    sig2tex0 = dump_sig2tex0(dumppath); tc = DrawTimeTex(dumppath)
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
    if BIND_REF:
        print("bind_ref:", apply_bind_ref(tris, tri_tex0, how, BIND_REF))
    # ---- 3. nearest dump-bound neighbour (shared vertex, same group), BFS rounds
    for _ in range(4):
        byv = collections.defaultdict(list)
        for i, t in enumerate(tris):
            if how[i] in ('exact', 'uvmap', 'ref_exact', 'ref_uvmap'):
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
    fb = None
    if args['bind_from_file']:
        fb = bind_from_file(buf, NB, Wm, bc, tris, tri_tex0, how)
        print("bind-from-file:", fb['stats'])

    # ---- placement from exact-matched corners (model bind pose -> dump world)
    used_idx = sorted({i for t in tris for i in t[:3]})
    Pn = V.astype(np.float64); bbmin = Pn[used_idx].min(0); bbmax = Pn[used_idx].max(0)
    piv = np.array([(bbmin[0] + bbmax[0]) / 2, bbmax[1], (bbmin[2] + bbmax[2]) / 2])  # -Y-up: feet = max Y
    q = lambda uv: (int(round(uv[0] * UVQ)), int(round(uv[1] * UVQ)))
    dsig = {}
    for t0, vs in dtris:
        dsig.setdefault(tri_sig([v[1] for v in vs]), {q(v[1]): v[0] for v in vs})
    def exact_pairs(Pm):
        Ms, Ws = [], []
        for i, t in enumerate(tris):
            if how[i] != 'exact': continue
            d = dsig.get(tri_sig(uvn(t)))
            if not d: continue
            for k, uv in enumerate(uvn(t)):
                w = d.get(q(uv))
                if w is not None: Ms.append(Pm[t[k]]); Ws.append(w)
        return np.array(Ms), np.array(Ws)
    pl = solve_placement(*exact_pairs(Pn), piv)
    print("placement:", {k: round(v, 3) if isinstance(v, float) else v for k, v in pl.items()})

    import p000_pose as PZ
    vb0 = PZ.vertex_bones(buf, NB, bc, gmeta)

    # ---- cape (block 0x19f70) posed from the dump
    cape, cape_tex0, cst = C.posed_cape(buf, NB, dumppath)
    print("cape:", cst)
    texsrc = tc
    if fb is not None:
        cape_tex0 = fb['cape'](cape_tex0)
        texsrc = fb['textures'](tc)
        print("bind-from-file textures:", fb['stats']['textures'])

    # ---- atlas: every distinct runtime texture, stacked vertically
    used = []
    for x in tri_tex0 + [cape_tex0]:
        if x is not None and x not in used and texsrc.get(x) is not None: used.append(x)
    imgs = {x: np.asarray(texsrc.get(x)[:, :, :3], np.uint8) for x in used}
    Wc = max(im.shape[1] for im in imgs.values()); region = {}; rows = []; yoff = 0
    for x in used:
        im = imgs[x]; h, w = im.shape[:2]
        pad = np.zeros((h, Wc, 3), np.uint8); pad[:, :w] = im
        region[x] = (yoff, h, w); rows.append(pad); yoff += h
    combined = np.vstack(rows); Hc = combined.shape[0]

    Pn_bind = Pn.copy()
    Nn = (N.astype(np.float64) if N is not None and len(N) == len(V) else np.zeros_like(Pn))
    ln = np.linalg.norm(Nn, axis=1, keepdims=True); ln[ln < 1e-9] = 1.0; Nn = Nn / ln

    # ---- pose the body to this frame from the dump (p000_pose.py); POSE=0 = bind pose
    pose_stats = None; clip_stats = None; src = {}; posed = None; clip_cape = None
    if os.environ.get('POSE', '1') != '0' or args['clip']:
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
        posed, rot, pose_stats = PZ.pose(placed, dict(cands), vb0, sk['parent'], src=src)
        pose_stats['vertices'] = len(used_idx)
        pose_stats['with_dump_candidates'] = sum(1 for v in used_idx if cands.get(v))
        print("pose:", pose_stats)
        if not args['clip']:
            Rinv = Ry.T
            for v in used_idx:
                Pn[v] = world_to_model(posed[v][None], pl, piv)[0]
                Nn[v] = Rinv @ (rot[v] @ (Ry @ Nn[v]))
    if args['clip']:
        # ---- pose from a clip frame instead; the dump pose above is the reference.
        cbuf, CP = AR.find_clip(args['cont'], args['clip'])
        trs = AR.decode_frame_trs(cbuf, CP, bc, args['frame'])
        dv = [v for v in used_idx if src.get(v) == 'dump']        # dump's own positions
        Dw = np.array([posed[v] for v in dv])
        # bones where P1 / P2 can be judged: translation off bind by > 0.05 units, or a
        # scale component off 1 by > 5%
        moved = [b for b in range(bc) if b > 2 and (
                 (trs[b][0] is not None and np.linalg.norm(np.subtract(trs[b][0], sk['local_T'][b])) > 0.05)
                 or (trs[b][1] is not None and max(abs(c - 1) for c in trs[b][1]) > 0.05))]
        def clip_pose(**kw):
            Wc = AR.compose(sk, trs, root_motion=not args['in_place'], **kw)
            Vc, Nc, tris_c, _g = P.decode_groups(buf, NB, Wc, bc)
            tris_c = [t for t in tris_c if t[6] in keep]
            assert len(Vc) == len(V) and [t[:3] for t in tris_c] == [t[:3] for t in tris]
            Pc = Vc.astype(np.float64); pv = Pc[used_idx]
            pc = np.array([(pv[:, 0].min() + pv[:, 0].max()) / 2, pv[:, 1].max(), (pv[:, 2].min() + pv[:, 2].max()) / 2])
            plc = solve_placement(*exact_pairs(Pc), pc)
            yr = np.radians(plc['yaw']); cy, sy = np.cos(yr), np.sin(yr)
            Ryc = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
            X = plc['s'] * (Pc[dv] - pc) @ Ryc.T + np.array([plc['x'], plc['y'], plc['z']])
            sc, Rm, t = umeyama(Pc[dv], Dw)
            e = np.linalg.norm(X - Dw, axis=1)
            per = {b: round(float(np.median(e[m])), 3) for b in moved
                   for m in [np.array([vb0.get(v) == b for v in dv])] if m.sum() >= 4}
            return Pc, Nc, pv, pc, plc, dict(placed=err_stats(X, Dw), aligned=err_stats(sc * Pc[dv] @ Rm.T + t, Dw),
                                             moved_bones_median=per)
        Pn, Nc, pv, piv, pl, err = clip_pose()
        Nn = Nc.astype(np.float64); ln = np.linalg.norm(Nn, axis=1, keepdims=True); ln[ln < 1e-9] = 1.0; Nn = Nn / ln
        bbmin, bbmax = pv.min(0), pv.max(0)
        rot_only = clip_pose(use_t=False, use_s=False)[5]
        no_scale = clip_pose(use_s=False)[5]
        sb, Rb, tb = umeyama(Pn_bind[dv], Dw)
        clip_stats = dict(clip=args['clip'], frame=args['frame'], container=os.path.basename(args['cont']),
                          frames=AR.clip_frames(cbuf, CP), in_place=args['in_place'],
                          ref_vertices=len(dv), error_vs_dump=err,
                          error_rotation_only=rot_only, error_no_scale=no_scale,
                          error_bind_aligned=err_stats(sb * Pn_bind[dv] @ Rb.T + tb, Dw))
        print("clip pose:", clip_stats)
        if not args['no_cape']:
            clip_cape, cst['clip_cape'] = clip_pose_cape(args, sk, cbuf, CP, trs, cape, pl, piv)
            print("cape (clip):", cst['clip_cape'])
        print("placement (clip):", {k: round(v, 3) if isinstance(v, float) else v for k, v in pl.items()})
    flat = []; dropped = 0
    def emit(p, n, uv, col, x):
        yo, rh, rw = region[x]
        flat.extend((p[0], p[1], p[2], n[0], n[1], n[2], (uv[0] * rw) / Wc, (yo + uv[1] * rh) / Hc,
                     col[0] / 255.0, col[1] / 255.0, col[2] / 255.0,
                     (col[3] if len(col) > 3 else 128) / 255.0))   # GS vertex alpha: shader As = texel*va*2
    for i, t in enumerate(tris):
        x = tri_tex0[i]
        if x is None or x not in region: dropped += 1; continue
        cols = (t[7], t[8], t[9]) if len(t) >= 10 else ((122, 122, 122, 128),) * 3
        for k in range(3): emit(Pn[t[k]], Nn[t[k]], uvn(t)[k], cols[k], x)
    nb = len(flat) // 12
    capes = [] if args['no_cape'] else cape
    cm = clip_cape if clip_cape is not None else [world_to_model(np.array(ws), pl, piv) for ws, _u, _c in capes]
    cn = cape_normals(cm, flat=args['cape_flat'])
    for (ws, uvs, cols), m, ns in zip(capes, cm, cn):
        for k in range(3): emit(m[k], ns[k], uvs[k], cols[k], cape_tex0)
    vcount = len(flat) // 12
    print(f"body verts {nb} (dropped tris {dropped}), cape verts {vcount - nb}; atlas {Wc}x{Hc}, {len(used)} textures")
    # header bbox = BODY bbox: render_scene_textured pivots on it, and the cape was
    # mapped into model space against that same pivot.
    hdr = struct.pack('<4sII', b'DMSH', 1, vcount) + struct.pack('<6f', *bbmin, *bbmax)
    mp = os.path.join(outdir, "vincent_p000.mesh")
    with open(mp, 'wb') as f: f.write(hdr); array.array('f', flat).tofile(f)
    Image.fromarray(combined, 'RGB').save(os.path.join(outdir, "vincent_p000_tex.png"))
    # Alpha: only the cape keeps its texture alpha. Its dump draws are ATE GEQUAL AREF 100,
    # AFAIL RGB_ONLY, blended by As (20% of its texels are 0 = the torn hem/fold gaps);
    # the body draws are not alpha-tested, so the body stays 255 (= opaque under any ref
    # <= 1.0 with vertex alpha 128). Stored as A*255/128 like the room decoder, so the
    # renderer's GS alpha test uses ref CAPE_AREF_GL (placement.json char_alpha_ref).
    alpha = np.full(combined.shape[:2], 255, np.uint8)
    yo, rh, rw = region[cape_tex0]
    ca = np.asarray(texsrc.get(cape_tex0)[:, :, 3], np.float64)
    alpha[yo:yo + rh, :rw] = np.clip(np.round(ca * 255 / 128), 0, 255).astype(np.uint8)
    rgba = np.dstack([combined, alpha])
    with open(os.path.join(outdir, "vincent_p000_tex.rgba"), 'wb') as f:
        f.write(struct.pack('<II', Wc, Hc)); f.write(rgba.astype(np.uint8).tobytes())
    # ---- 0x2ee8 glow pass (p000_glow.py): extra draw, only used with -charglow.
    # Dump-frame UVs and colours, so only for the dump pose.
    glow_stats = None
    if not args['clip']:
        import p000_glow as GL
        gflat, gatlas, glow_stats = GL.build_glow(dumppath, tc, tris, tri_tex0, uvn, Pn, Nn,
                                                    lambda W: world_to_model(W, pl, piv))
        print("glow:", glow_stats)
        with open(os.path.join(outdir, "vincent_p000_glow.mesh"), 'wb') as f:
            f.write(struct.pack('<4sII', b'DMSH', 1, len(gflat) // 12) + struct.pack('<6f', *bbmin, *bbmax))
            array.array('f', gflat).tofile(f)
        with open(os.path.join(outdir, "vincent_p000_glow_tex.rgba"), 'wb') as f:
            f.write(struct.pack('<II', gatlas.shape[1], gatlas.shape[0])); f.write(gatlas.tobytes())
    json.dump([dict(g=int(t[6]), name=gmeta[t[6]][0], tex0=(None if tri_tex0[i] is None else int(tri_tex0[i])),
                    how=how[i], uv=uvn(t), v=[int(k) for k in t[:3]],
                    **({} if fb is None else dict(src='file', mat=fb['mat'][i]))) for i, t in enumerate(tris)],
              open(os.path.join(outdir, "binding.json"), 'w'))
    json.dump(dict(placement=pl, binding=dict(hc), cape=cst, pose=pose_stats, clip=clip_stats,
                   **({} if glow_stats is None else dict(glow=glow_stats)),
                   **({} if fb is None else dict(bind_from_file=fb['stats'])),
                   char_alpha_ref=CAPE_AREF_GL,
                   render_args=f"-s {pl['s']:.4f} -yaw {pl['yaw']:.2f} -x {pl['x']:.2f} -y {pl['y']:.2f} -z {pl['z']:.2f}",
                   # Vincent is placed in this camera's world: render with exactly it.
                   camera_args="-fov {fov} -eye {e} -at {a} -up {u}".format(
                       fov=C.HERO_CAM['fov'], e=','.join(map(str, C.HERO_CAM['eye'])),
                       a=','.join(map(str, C.HERO_CAM['at'])), u=','.join(map(str, C.HERO_CAM['up'])))),
              open(os.path.join(outdir, "placement.json"), 'w'), indent=2)
    print(f"wrote {mp} ({vcount} verts) + placement.json")


if __name__ == '__main__':
    main()
