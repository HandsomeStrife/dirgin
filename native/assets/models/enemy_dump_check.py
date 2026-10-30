#!/usr/bin/env python3
"""Check export_web_enemy's glb files against a GS dump's draws of the model.

  python3 -I enemy_dump_check.py DUMP.bin [--model e027] [--dir OUT] [--png OUT.png]

For each exported variant (MODEL.glb, MODEL_vN.glb in --dir):
 1. texture: every dump TEX0 whose decoded texels equal the variant's texture (mean |d| = 0)
    is that variant's page;
 2. UVs: a dump triangle on that page matches when its unordered corner UV set (S/Q on the
    1/4096 grid) is an exported triangle's UV set -> dump triangles matched, export
    triangles covered;
 Only the dump's last full frame is used (gsrender2.pick_frame, as the oracle render).
 3. geometry: dump triangles are lifted to world space (hero camera, or DUMP_cam.json; w=1/q,
    p000_cape.gs_to_world) and split into instances (connected within 8 units). For the
    largest instance the export, posed by every r_gst frame, is fitted (similarity) to the
    dump corners matched through the UV-matched triangles; the best frame's RMS residual is
    reported, and --png draws the dump instance and the fitted export side by side, both
    projected by the dump camera.
 4. winding: the dump's drawn triangles and its ADC=1 ones (XYZ3, not drawn) on the page, each
    taken in the matched export triangle's index order; mirror side resolved by position per
    instance (fit + pose_from_dump). cw on screen (y down) = WebGL back face -> the page's side.
 5. draw order: seq range of the variant's triangles vs Vincent's (TBP 0x2cd4) in the frame.
"""
import sys, os, json, collections
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.abspath(os.path.join(HERE, "../../../tools/gsdump/src")))
from gsparse import read_header
from gsrender2 import build, tex0_fields, TexCache, pick_frame, DepthInterp
import export_web_enemy as E
import export_web_vincent as V
import smd_material as M
import p000_cape as PC


def key3(uvs):
    return frozenset((int(round(u * 4096)), int(round(v * 4096))) for u, v in uvs)


def load_dump(path):
    """build() keeps only drawn triangles. Triangles closed by an XYZ3 (ADC=1, not drawn) go to
    g.adc_tris instead, with the same ctx (vframe, seq = drawn-triangle index at that point)."""
    buf = open(path, 'rb').read()
    orig = DepthInterp._emit_vertex
    def emit(self, x, y, z, f=None, drawkick=True, adc=False):
        if not adc: return orig(self, x, y, z, f, drawkick, adc)
        n = len(self.tris); orig(self, x, y, z, f, True, adc)          # same queue handling as a kick
        if not hasattr(self, 'adc_tris'): self.adc_tris = []
        self.adc_tris.extend(self.tris[n:]); del self.tris[n:]
    DepthInterp._emit_vertex = emit
    try: g = build(buf, read_header(buf))
    finally: DepthInterp._emit_vertex = orig
    if not hasattr(g, 'adc_tris'): g.adc_tris = []
    return buf, g, TexCache(buf, uploads=g.uploads)


def winding(tri, mesh, ti):
    """Screen winding of a dump triangle's corners taken in the export triangle ti's index
    order (corners paired by UV). GS screen is y down: > 0 = clockwise on screen, which
    is a WebGL back face (three.js front = counter-clockwise on screen; the page's chain
    glb root Rx(pi) -> flip Rx(pi) -> yaw is a proper rotation). None if UVs don't pair 1:1."""
    q = lambda u, v: (int(round(u * 4096)), int(round(v * 4096)))
    by = {q(x[3] / x[5], x[4] / x[5]): x for x in tri[:3]}
    if len(by) != 3: return None
    try: P = [by[q(*mesh['uv'][3 * ti + k])] for k in range(3)]
    except KeyError: return None
    c = (P[1][0] - P[0][0]) * (P[2][1] - P[0][1]) - (P[1][1] - P[0][1]) * (P[2][0] - P[0][0])
    return 0 if c == 0 else (1 if c > 0 else -1)


def winding_stats(tris, fk, mesh):
    """Per dump triangle: the winding over every export candidate with its UV triple.
    unique = one candidate; agree = several candidates, all the same sign; split = mirrored
    candidates disagree (side unknown without a fit)."""
    st = collections.Counter()
    for t in tris:
        ws = {winding(t, mesh, ti) for ti in fk[t[-1]]} - {None}
        if len(ws) != 1: st['split' if len(ws) > 1 else 'unpaired'] += 1; continue
        w = ws.pop(); kind = 'unique' if len(fk[t[-1]]) == 1 else 'agree'
        st[(kind, {1: 'cw', -1: 'ccw', 0: 'zero'}[w])] += 1
    return st


def fmt_w(st):
    out = []
    for kind in ('unique', 'agree'):
        cw, ccw = st[(kind, 'cw')], st[(kind, 'ccw')]
        out.append(f"{kind} cw {cw} / ccw {ccw}" + (f" / zero {st[(kind, 'zero')]}" if st[(kind, 'zero')] else ""))
    return "; ".join(out) + f"; mirror-split {st['split']}, unpaired {st['unpaired']}"


def camera(path):
    cj = os.path.splitext(path)[0] + '_cam.json'
    if os.path.exists(cj):
        c = json.load(open(cj)); return dict(PC.HERO_CAM, eye=c['eye'], at=c['at'], up=c['up'], fov=c['fov'])
    return PC.HERO_CAM


def glb_mesh(path):
    j, acc = V.read_glb(path)
    a = j['meshes'][0]['primitives'][0]['attributes']
    return dict(pos=acc(a['POSITION']).astype(float), uv=acc(a['TEXCOORD_0']).astype(float),
                J=acc(a['JOINTS_0']).astype(int), W=acc(a['WEIGHTS_0']).astype(float),
                ibm=acc(j['skins'][0]['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1))


def pose(mesh, Wj):
    """Linear blend skin of the glb corners with joint world matrices Wj (model space)."""
    S = np.stack([Wj[b] @ mesh['ibm'][b] for b in range(len(Wj))])
    P4 = np.c_[mesh['pos'], np.ones(len(mesh['pos']))]
    out = np.zeros((len(P4), 3))
    for k in range(4):
        out += mesh['W'][:, k:k + 1] * np.einsum('nij,nj->ni', S[mesh['J'][:, k]], P4)[:, :3]
    return out


def sim_fit(A, B):
    """Similarity (s, R, t) minimising |s R A + t - B|; returns (s, R, t, rms)."""
    ca, cb = A.mean(0), B.mean(0); A0, B0 = A - ca, B - cb
    U, S_, Vt = np.linalg.svd(A0.T @ B0); d = np.sign(np.linalg.det(Vt.T @ U.T))
    D = np.diag([1, 1, d]); R = Vt.T @ D @ U.T; s = (S_ * np.diag(D)).sum() / (A0 ** 2).sum()
    t = cb - s * R @ ca; r = np.sqrt(((s * (R @ A.T).T + t - B) ** 2).sum(1).mean())
    return s, R, t, r


def instances(tris_w, link=8.0):
    """Split dump triangles into connected groups (corners within `link` units)."""
    n = len(tris_w); par = list(range(n))
    def f(i):
        while par[i] != i: par[i] = par[par[i]]; i = par[i]
        return i
    cent = np.array([t.mean(0) for t in tris_w]); order = np.argsort(cent[:, 0])
    for ii, i in enumerate(order):
        for k in order[ii + 1:]:
            if cent[k, 0] - cent[i, 0] > 2 * link + 30: break
            if np.linalg.norm(cent[k] - cent[i]) < link: par[f(k)] = f(i)
    groups = collections.defaultdict(list)
    for i in range(n): groups[f(i)].append(i)
    return sorted(groups.values(), key=len, reverse=True)


def main(argv):
    dump = argv[0]; opt = lambda k, d=None: argv[argv.index(k) + 1] if k in argv else d
    model = opt('--model', 'e027'); d = opt('--dir', E.DEFAULT_OUT); png = opt('--png')
    pth, buf, NB, sk = E.load(model)
    dbuf, g, tc = load_dump(dump); cam = camera(dump)
    fa, fb = pick_frame(g, 'last'); frame_tris = g.tris[fa:fb]      # the oracle's frame
    vf = frame_tris[0][3]['vframe'] if frame_tris else None
    frame_adc = [t for t in g.adc_tris if t[3]['vframe'] == vf and fa <= t[3]['seq'] <= fb]
    vin_seq = [t[3]['seq'] for t in frame_tris if t[3]['tme'] and tex0_fields(t[3]['tex0'])['tbp0'] == 0x2cd4]
    print(f"dump {os.path.basename(dump)}: {len(g.tris)} triangles, last frame {fb - fa} drawn + {len(frame_adc)} ADC")
    clip = E.DEFAULT_CLIPS[0]
    cbuf, CP = V.find_clip(pth['clips'], clip); nf = E.AR.clip_frames(cbuf, CP)
    Wf = [V.clip_mats(sk, clip, f, cont=pth['clips']) for f in range(nf)]
    texcache = {}
    report = {}
    for v in E.variants(buf):
        stem = model if v == 0 else f"{model}_v{v}"
        mesh = glb_mesh(os.path.join(d, stem + '.glb')); nt = len(mesh['pos']) // 3
        mat = M.material_table(pth['tex'], variant=v)[0]
        ftex = M.texture_rgba(pth['tex'], mat).astype(float)[..., :3]
        fk = collections.defaultdict(list)
        for i in range(nt): fk[key3(mesh['uv'][3 * i:3 * i + 3])].append(i)
        def on_page(a, b, c, ctx):
            if not ctx['tme'] or ctx['fst']: return False
            f = tex0_fields(ctx['tex0'])
            if (f['tw'], f['th']) != (mat['fields']['tw'], mat['fields']['th']): return False
            kk = (ctx['tex0'], id(tc.best_at(f['tbp0'], ctx['seq'])), id(tc.best_at(f['cbp'], ctx['seq'])))
            if kk not in texcache:
                im = tc.get(ctx['tex0'], ctx['seq'])
                texcache[kk] = im is not None and np.asarray(im).shape[:2] == ftex.shape[:2] and \
                    float(np.abs(np.asarray(im, float)[..., :3] - ftex).mean()) == 0.0
            return texcache[kk] and all(abs(x[5]) > 1e-9 for x in (a, b, c))
        sel = [t for t in frame_tris if on_page(*t)]
        hit = []; cov = set()
        for a, b, c, ctx in sel:
            k = key3([(x[3] / x[5], x[4] / x[5]) for x in (a, b, c)])
            if k in fk: hit.append((a, b, c, ctx, k)); cov.update(fk[k])
        line = (f"  {stem}: export {nt} tris; texture {mat['name']} {1 << mat['fields']['tw']}x{1 << mat['fields']['th']}: "
                f"dump tris on it {len(sel)}, UV-matched {len(hit)}" +
                (f" ({100.0 * len(hit) / len(sel):.2f}%)" if sel else "") + f", export tris covered {len(cov)}/{nt}")
        print(line); report[stem] = dict(dump_tris=len(sel), matched=len(hit), covered=len(cov), export_tris=nt)
        if not hit: continue
        # winding: drawn triangles vs the ones the game sent with ADC=1 (XYZ3, not drawn)
        adc = []
        for t in frame_adc:
            if on_page(*t):
                k = key3([(x[3] / x[5], x[4] / x[5]) for x in t[:3]])
                if k in fk: adc.append((*t, k))
        wd, wa = winding_stats(hit, fk, mesh), winding_stats(adc, fk, mesh)
        print(f"     winding in glb index order (screen, y down; cw = WebGL back face):")
        print(f"       drawn {len(hit)}: {fmt_w(wd)}")
        print(f"       ADC   {len(adc)}: {fmt_w(wa)}")
        report[stem].update(wind_drawn=dict((('_'.join(k) if isinstance(k, tuple) else k), v) for k, v in wd.items()),
                            wind_adc=dict((('_'.join(k) if isinstance(k, tuple) else k), v) for k, v in wa.items()))
        seqs = [t[3]['seq'] for t in hit]
        print(f"     draw order: e027 seq {min(seqs)}..{max(seqs)}; Vincent (TBP 0x2cd4) seq "
              + (f"{min(vin_seq)}..{max(vin_seq)}" if vin_seq else "none")
              + f"; e027 after Vincent's last: {sum(q > max(vin_seq) for q in seqs) if vin_seq else '-'}"
              + f", before Vincent's first: {sum(q < min(vin_seq) for q in seqs) if vin_seq else '-'}")
        W_ = [np.array([PC.gs_to_world(x[0], x[1], x[5], ctx['xyoffset'], cam) for x in (a, b, c)])
              for a, b, c, ctx, _k in hit]
        # pose-independent shape: edge lengths of each dump triangle vs its export triangle in
        # the bind pose, corners paired by UV; mirrored candidates -> the closest one
        def edges(T): return np.array([np.linalg.norm(T[1] - T[0]), np.linalg.norm(T[2] - T[1]), np.linalg.norm(T[0] - T[2])])
        def corner_order(h, ti):
            a, b, c, ctx, k = hit[h]
            return [3 * ti + min(range(3), key=lambda q: np.abs(mesh['uv'][3 * ti + q] - (x[3] / x[5], x[4] / x[5])).sum())
                    for x in (a, b, c)]
        ratios = []; rigid = []
        rig = lambda cs: all(mesh['W'][i, 0] > 0.999 for i in cs) and len({mesh['J'][i, 0] for i in cs}) == 1
        for h in range(len(hit)):
            de = edges(W_[h]); best_r = None
            for ti in fk[hit[h][-1]]:
                cs = corner_order(h, ti); ee = edges(mesh['pos'][cs])
                if (ee < 1e-6).any(): continue
                rr = de / ee
                if best_r is None or np.ptp(np.log(rr)) < np.ptp(np.log(best_r)): best_r = rr; best_rig = rig(cs)
            if best_r is not None: ratios.append(best_r); rigid.append(best_rig)
        ratios = np.array(ratios); rigid = np.array(rigid); sc_ = np.median(ratios); rel = np.abs(ratios / sc_ - 1).max(1)
        for lab, m_ in (('all', np.ones(len(rel), bool)), ('rigid (1 joint, w=1)', rigid), ('skinned across joints', ~rigid)):
            r_ = rel[m_]
            if not len(r_): continue
            print(f"     shape {lab}: {len(r_)} tris; max edge error per tri median {100 * np.median(r_):.2f}%, "
                  f"within 2% {100 * (r_ < 0.02).mean():.1f}%, 5% {100 * (r_ < 0.05).mean():.1f}%, 10% {100 * (r_ < 0.10).mean():.1f}%")
        print(f"     dump/export edge scale {sc_:.4f}")
        report[stem].update(edge_scale=float(sc_), edge_err_median=float(np.median(rel)),
                            within5=float((rel < 0.05).mean()))
        inst = instances(W_)
        print(f"     instances (connected, 8 units): {len(inst)} sizes {[len(x) for x in inst[:8]]}")
        # corners: dump world point <-> export corner with the same UV in the matched triangle
        # Mirrored halves share UV triples, so a key can name several export triangles.
        # Fit on UV-unique triangles; then give each ambiguous one its nearest candidate.
        def corners(tri, ti):
            out = []
            for x in tri[:3]:
                uv = (x[3] / x[5], x[4] / x[5])
                out.append(3 * ti + min(range(3), key=lambda q: np.abs(mesh['uv'][3 * ti + q] - uv).sum()))
            return out

        def fit(members):
            uniq = [h for h in members if len(fk[hit[h][-1]]) == 1]
            A_idx = [i for h in uniq for i in corners(hit[h], fk[hit[h][-1]][0])]
            B = np.concatenate([W_[h] for h in uniq]); Ball = np.concatenate([W_[h] for h in members])
            cand = sorted((sim_fit(pose(mesh, Wf[f])[A_idx], B)[3], f) for f in range(nf))[:10]
            best = None
            for _r0, f in cand:                  # refine: nearest candidate per tri, refit, x4
                P = pose(mesh, Wf[f]); s, R, t, r = sim_fit(P[A_idx], B)
                for _it in range(4):
                    Pf = s * (R @ P.T).T + t
                    idx = [i for h in members for i in min((corners(hit[h], ti) for ti in fk[hit[h][-1]]),
                                                           key=lambda cs: ((Pf[cs] - W_[h]) ** 2).sum())]
                    s, R, t, r = sim_fit(P[idx], Ball)
                if best is None or r < best[0]: best = (r, f, s, R, t, P, idx)
            return best, uniq, Ball

        # winding with the mirror side resolved by position: per instance (>= 20 tris), the
        # whole-body fit picks a side per triangle, the export is posed per joint from the dump
        # (pose_from_dump), and each drawn / ADC triangle takes its nearest export candidate,
        # kept only if that one is within 1 unit RMS and the next candidate >= 3x farther.
        adcW = [np.array([PC.gs_to_world(x[0], x[1], x[5], t[3]['xyoffset'], cam) for x in t[:3]]) for t in adc]
        res_d, res_a = collections.Counter(), collections.Counter()
        def classify(tri, Wt, Pj, acc):
            ds = sorted((np.sqrt(((Pj[corners(tri, ti)] - Wt) ** 2).sum(1).mean()), ti) for ti in fk[tri[-1]])
            if ds[0][0] >= 1.0: return False
            if len(ds) > 1 and ds[1][0] < 3 * ds[0][0]: acc['ambiguous'] += 1; return True
            acc[{1: 'cw', -1: 'ccw', 0: 'zero', None: 'unpaired'}[winding(tri, mesh, ds[0][1])]] += 1; return True
        taken = set()
        for members in inst:
            if len(members) < 20: continue
            (r_, f_, s_, R_, t_, P_, idx_), _u, _B = fit(members)
            chosen = {h: idx_[3 * k:3 * k + 3] for k, h in enumerate(members)}
            Pj, _st = pose_from_dump(mesh, sk, members, W_, chosen)
            for h in members: classify(hit[h], W_[h], Pj, res_d)
            for j in range(len(adc)):
                if j not in taken and classify(adc[j], adcW[j], Pj, res_a): taken.add(j)
        fr = lambda c: f"cw {c['cw']} / ccw {c['ccw']} / zero {c['zero']}, ambiguous {c['ambiguous']}"
        print(f"     winding, mirror side by position: drawn {fr(res_d)} (of {len(hit)}); ADC {fr(res_a)} (of {len(adc)})")
        report[stem].update(wind_pos_drawn=dict(res_d), wind_pos_adc=dict(res_a))
        big = inst[0]
        best, uniq, Ball = fit(big)
        r, f, s, R, t, P, idx = best
        Pf = s * (R @ P.T).T + t
        allerr = np.sqrt(((Pf[idx] - Ball) ** 2).sum(1))
        ext = np.ptp(Ball, 0).max()
        print(f"     largest instance: {len(big)} tris ({len(uniq)} UV-unique), {len(Ball)} corners, extent "
              f"{ext:.1f} units; best {clip} frame {f}: similarity fit over all corners RMS {r:.3f} units, "
              f"median {np.median(allerr):.3f}, 95% {np.percentile(allerr, 95):.3f}, max {allerr.max():.3f} (scale {s:.4f})")
        report[stem].update(instance_tris=len(big), fit_rms=r, fit_frame=f, fit_scale=s, extent=ext)
        if png and v == 0:
            chosen = {h: idx[3 * k:3 * k + 3] for k, h in enumerate(big)}   # side from the whole-body fit
            Pj, st = pose_from_dump(mesh, sk, big, W_, chosen)
            print(f"     dump-posed export (rigid fit per joint, trimmed): {st}")
            draw(png, [W_[h] for h in big], Pj, cam, ftex, hit, big, mesh)
    return report


def kabsch(A, B):
    ca, cb = A.mean(0), B.mean(0); U, _S, Vt = np.linalg.svd((A - ca).T @ (B - cb))
    d = np.sign(np.linalg.det(Vt.T @ U.T)); R = Vt.T @ np.diag([1, 1, d]) @ U.T
    return R, cb - R @ ca


def pose_from_dump(mesh, sk, big, W_, chosen):
    """Per joint: rigid transform bind -> dump world from the corners whose top weight
    (>0.5) is that joint, using each dump triangle's chosen export triangle (mirror side
    picked by the whole-body fit), trimmed to
    residual <= max(0.5, 2 x median) x5. Joints with < 4 kept points take their parent's.
    Then skin every export corner with those transforms (glb weights). Check image only:
    this is NOT the game's animation."""
    pairs = collections.defaultdict(list)
    for h in big:
        for i, w in zip(chosen[h], W_[h]):
            if mesh['W'][i, 0] > 0.5: pairs[int(mesh['J'][i, 0])].append((mesh['pos'][i], w))
    T = {}; kept = 0
    for j, pr in pairs.items():
        A = np.array([a for a, _ in pr]); B = np.array([b for _, b in pr]); m = np.ones(len(A), bool)
        for _ in range(5):
            if m.sum() < 4: break
            R, t = kabsch(A[m], B[m]); r = np.linalg.norm((R @ A.T).T + t - B, axis=1)
            m = r <= max(0.5, 2 * np.median(r[m]))
        if m.sum() >= 4 and np.ptp(A[m], 0).max() > 0.3:
            T[j] = kabsch(A[m], B[m]); kept += int(m.sum())
    par = sk['parent']; res = []
    def get(j):
        while j not in T and j >= 0: j = par[j]
        return T.get(j)
    out = np.zeros_like(mesh['pos'])
    for i, p in enumerate(mesh['pos']):
        acc = np.zeros(3); ws = 0.0
        for k in range(4):
            w = mesh['W'][i, k]; tr = get(int(mesh['J'][i, k])) if w > 0 else None
            if tr is not None: acc += w * (tr[0] @ p + tr[1]); ws += w
        out[i] = acc / ws if ws > 0 else p
    for h in big:                                 # residual of the matched corners
        res.append(np.sqrt(((out[chosen[h]] - W_[h]) ** 2).sum(1).mean()))
    res = np.array(res)
    return out, (f"{len(T)} joints solved, {kept} points kept; matched tri corner RMS median {np.median(res):.3f}, "
                 f"90% {np.percentile(res, 90):.3f}, max {res.max():.3f} units")


def draw(png, dump_w, fitted, cam, ftex, hit, big, mesh):
    """Left: the dump instance, right: the fitted export (all its triangles), both projected
    by the dump camera and textured with nearest texels of the variant's texture."""
    from PIL import Image, ImageDraw
    dp = [PC.world_to_drawable(t) for t in dump_w]
    ep = PC.world_to_drawable(fitted)
    allp = np.concatenate(dp + [ep]); x0, y0 = allp.min(0); x1, y1 = allp.max(0)
    S = 560.0 / max(x1 - x0, y1 - y0); Wd = int((x1 - x0) * S) + 40; Hd = int((y1 - y0) * S) + 40
    im = Image.new('RGB', (2 * Wd, Hd), (24, 24, 28)); dr = ImageDraw.Draw(im)
    tw = ftex.shape[1]; th = ftex.shape[0]
    def col(uvs):
        u, v = np.mean(uvs, 0); return tuple(int(c) for c in ftex[min(int(v * th), th - 1), min(int(u * tw), tw - 1)])
    def tr(p, ox): return [((x - x0) * S + 20 + ox, (y - y0) * S + 20) for x, y in p]
    # painter's order by depth (far first) for both
    eye = np.array(cam['eye'])
    for h, p, w in sorted(zip(big, dp, dump_w), key=lambda z: -np.linalg.norm(z[2].mean(0) - eye)):
        a, b, c, ctx, k = hit[h]
        dr.polygon(tr(p, 0), fill=col([(x[3] / x[5], x[4] / x[5]) for x in (a, b, c)]))
    nt = len(fitted) // 3
    for i in sorted(range(nt), key=lambda i: -np.linalg.norm(fitted[3 * i:3 * i + 3].mean(0) - eye)):
        dr.polygon(tr(ep[3 * i:3 * i + 3], Wd), fill=col(mesh['uv'][3 * i:3 * i + 3]))
    dr.text((8, 4), "GS dump (instance)", fill=(255, 255, 255)); dr.text((Wd + 8, 4), "export (e027.glb), posed per joint from the dump", fill=(255, 255, 255))
    im.save(png); print("     wrote", png)


if __name__ == '__main__':
    main(sys.argv[1:])
