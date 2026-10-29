// Cape lighting vs the hero GS dump (frame 163235): cloth.js skinNormals (FUN_005b06d0's summed
// int8 normals, unnormalised) lit by VU1 program D (port/vu/RENDER-MATH.md §1; the same math as
// play.js LIGHT_VS) must reproduce the GIF vertex colours of the cape's draws (texture 0x2e68).
//
// Run: node --import ./native/web/test_hooks.mjs native/web/test_cape_light.mjs [cloth.js]
// The optional argument loads another cloth.js (mutation checks).
//
// The game data is read at run time and never stored here (the repo is public): the dump,
// data/vincent/cloth.json and light.json, and the game files the Python tools read. If any is
// missing the test skips (exit 0).
//
// Method (worker 0111's capecheck.py, notes/HANDOFF.md "Cape + hair lighting"): the dump has
// no pose, so the Python step poses the skeleton at h_st frames 80..100, rigid-fits each
// frame's cape targets to the dump's cape corners on the 20 pinned vertices (corners matched
// by UV), keeps the best frame (h_st f89), and picks light 2 at the actor point as play.js
// does. Per dump triangle the drawn layer is the clockwise one (the cape's two coincident
// layers have opposite normals). It hands the JS side the skin matrices (bind model -> dump
// world), the lights, and per corner the cloth vertex, its vertex colour and the GIF colour.
// The normals themselves come only from cloth.js. Measured (worker 0111): max-channel error
// median 3, max 4 on 168 corners; the residual is light 2's colour (~3.4% strong, audit row
// "Light 2 colour"). The check: median <= 3, max <= 4.
import { existsSync, readFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const modPath = path.resolve(process.argv[2] ?? path.join(here, 'cloth.js'));
const dump = path.join(root, 'tools/gsdump/dumps/z128_hero_163235.bin');
const clothJson = path.join(here, 'data/vincent/cloth.json');
const lightJson = path.join(here, 'data/vincent/light.json');
for (const f of [dump, clothJson, lightJson]) {
  if (!existsSync(f)) { console.log(`SKIP test_cape_light: ${path.relative(root, f)} missing (gitignored game data)`); process.exit(0); }
}

const PY = String.raw`
import sys, json, collections, numpy as np
R = sys.argv[1]
sys.path.insert(0, R + '/native/assets/models'); sys.path.insert(0, R + '/tools/gsdump/src')
import p000_cloth as PCL, p000_cape as PC, skd_skeleton as SK, anim_rot as AR
from gsparse import read_header
from gsrender2 import build, tex0_fields
dump, LJ = sys.argv[2], json.load(open(sys.argv[3]))
cam = PC.HERO_CAM
cape = PCL.Cape(); sk = SK.load_skeleton(PCL.CONT, trans_scale=1.0)
cbuf, CP = AR.find_clip(PCL.CLIP_CONT, 'h_st')
Q = 4096
uv2v = collections.defaultdict(set)
for vs, uvs, _c in cape.tris:
    for v, uv in zip(vs, uvs): uv2v[(int(round(uv[0] * Q)), int(round(uv[1] * Q)))].add(v)
buf = open(dump, 'rb').read(); g = build(buf, read_header(buf))
cand = [(a, b, c, ctx) for a, b, c, ctx in g.tris if ctx['tme'] and not ctx['fst'] and tex0_fields(ctx['tex0'])['tbp0'] == PC.CAPE_TBP]
last = max(ctx['vframe'] for *_x, ctx in cand)
DT = []
for a, b, c, ctx in cand:
    if ctx['vframe'] != last: continue
    keys = [(int(round(v[3] / v[5] * Q)), int(round(v[4] / v[5] * Q))) for v in (a, b, c)]
    W = np.array([PC.gs_to_world(v[0], v[1], v[5], ctx['xyoffset'], cam) for v in (a, b, c)])
    DT.append((keys, W, [[int(v[8]), int(v[9]), int(v[10])] for v in (a, b, c)]))
g0 = cape.groups[0]; PINS = set(int(x) for x in g0['verts'][g0['pinned']])
def rigid(src, dst):
    ms, md = src.mean(0), dst.mean(0); U, S, Vt = np.linalg.svd((src - ms).T @ (dst - md))
    D = np.diag([1, 1, np.sign(np.linalg.det(Vt.T @ U.T))]); Rr = Vt.T @ D @ U.T
    return Rr, md - Rr @ ms
def fit(T):
    Rr = np.eye(3); t = np.concatenate([W for _k, W, _o in DT]).mean(0) - T.mean(0)
    for _ in range(4):
        src, dst = [], []
        for keys, W, _o in DT:
            for k, w in zip(keys, W):
                c = [x for x in uv2v.get(k, []) if x in PINS]
                if not c: continue
                j = np.argmin(np.linalg.norm(T[c] @ Rr.T + t - w, axis=1)); src.append(T[c[j]]); dst.append(w)
        src, dst = np.array(src), np.array(dst); Rr, t = rigid(src, dst)
    return Rr, t, np.linalg.norm(src @ Rr.T + t - dst, axis=1)
best = None
for fr in np.arange(80.0, 100.0 + 1e-9, 1.0):
    Wp = PCL.bone_mats(sk, cbuf, CP, float(fr)); T = cape.targets(Wp)
    Rr, t, res = fit(T)
    if best is None or np.median(res) < best[0]: best = (np.median(res), fr, Wp, T, Rr, t)
med, fr, Wp, T, Rr, t = best
Tw = T @ Rr.T + t
# light 2 at the actor point, the global least d^2/k (play.js pick, FUN_0026a710)
p = Rr @ np.array(LJ['actor_center']) + t
bq, hit = 1e6, None
for gd in LJ['grids']:
    ix = min(max(int(np.trunc((p[0] - gd['x0']) / gd['size'])), 0), gd['nx'] - 1)
    iz = min(max(int(np.trunc((p[2] - gd['z0']) / gd['size'])), 0), gd['nz'] - 1)
    for li in gd['cells'][iz * gd['nx'] + ix]:
        r = gd['lights'][li]; d = p - np.array(r['pos']); q = d @ d / r['k']
        if q < bq: bq, hit = q, r
d = np.array(hit['pos']) - p; dl = np.linalg.norm(d)
# skin matrices: bind model -> dump world = [Rr t] x posed bone x inverse bind (column-major)
X = np.eye(4); X[:3, :3] = Rr; X[:3, 3] = t
mats = {}
for b in sorted(set(int(r[1]) for r in cape.skin)):
    mats[b] = (X @ np.asarray(Wp[b], float) @ np.linalg.inv(np.asarray(sk['world_bind'][b], float))).T.ravel().tolist()
corners = []
for keys, W, obs in DT:
    opts = []
    for tv, tuv, tc in cape.tris:
        tk = [(int(round(u[0] * Q)), int(round(u[1] * Q))) for u in tuv]
        if sorted(tk) != sorted(keys): continue
        S = PC.world_to_drawable(Tw[list(tv)], cam)
        cw = (S[1, 0] - S[0, 0]) * (S[2, 1] - S[0, 1]) - (S[1, 1] - S[0, 1]) * (S[2, 0] - S[0, 0])
        vs = [tv[tk.index(k)] for k in keys]
        opts.append((cw > 0, np.linalg.norm(Tw[vs] - W, axis=1).mean(), vs, [tc[tk.index(k)][0] for k in keys]))
    if not opts: continue
    o = min([x for x in opts if x[0]] or opts, key=lambda x: x[1])
    for v, ob, vc in zip(o[2], obs, o[3]): corners.append([int(v), float(vc), ob])
json.dump(dict(frame=float(fr), fit_median=float(med), mats=mats, targets=Tw.ravel().tolist(),
               L2=(d / dl).tolist(), C2=(np.array(hit['rgb']) * hit['k'] * 100 / dl ** 2).tolist(),
               corners=corners), sys.stdout)
`;
const pr = spawnSync('python3', ['-I', '-c', PY, root, dump, lightJson], { maxBuffer: 1 << 26, encoding: 'utf8' });
if (pr.status !== 0) {
  const err = pr.stderr || String(pr.error);
  if (/FileNotFoundError|No such file/.test(err)) { console.log('SKIP test_cape_light: game files missing\n' + err.trim().split('\n').pop()); process.exit(0); }
  console.error(err); process.exit(2);
}
const py = JSON.parse(pr.stdout);

const { CapeCloth } = await import(pathToFileURL(modPath).href);
const data = JSON.parse(readFileSync(clothJson, 'utf8'));
const L = JSON.parse(readFileSync(lightJson, 'utf8'));
const cloth = new CapeCloth(data);
const skinMat = b => py.mats[b];
const checks = [];

// the skin matrices are right: cloth.js's targets equal the Python targets the fit used
cloth.skin(skinMat);
let dT = 0;
for (let i = 0; i < cloth.N * 3; i++) dT = Math.max(dT, Math.abs(cloth.target[i] - py.targets[i]));
checks.push(['cloth.js skin() targets = the fitted targets', dT < 1e-3, `max |d| ${dT.toExponential(2)}`]);

// program D (RENDER-MATH.md §1, play.js LIGHT_VS): vcol x (C0 max(L0.n,0) + C1 max(L1.n,0)
// + C2 max(L2.n,0) + A), MINIi 255, FTOI0. Light 1 is zero on the page and in the game here.
const n = cloth.skinNormals(skinMat, new Float32Array(cloth.N * 3));
const s = L.scale_byte / 128;
const L0 = L.env.dir0, C0 = L.env.col0.map(x => x * s), A = L.env.ambient.map(x => x * s);
const L2 = py.L2, C2 = py.C2;
const dot = (a, v) => a[0] * n[3 * v] + a[1] * n[3 * v + 1] + a[2] * n[3 * v + 2];
const err = [];
for (const [v, vc, obs] of py.corners) {
  const d0 = Math.max(dot(L0, v), 0), d2 = Math.max(dot(L2, v), 0);
  let e = 0;
  for (let c = 0; c < 3; c++) {
    const rgb = Math.floor(Math.min(vc * (C0[c] * d0 + C2[c] * d2 + A[c]), 255));
    e = Math.max(e, Math.abs(rgb - obs[c]));
  }
  err.push(e);
}
err.sort((a, b) => a - b);
const med = err.length % 2 ? err[err.length >> 1] : (err[err.length / 2 - 1] + err[err.length / 2]) / 2;
const p90 = err[Math.floor(0.9 * (err.length - 1))], max = err[err.length - 1];
const mean = err.reduce((a, b) => a + b, 0) / err.length;
const stats = `n ${err.length}, median ${med}, mean ${mean.toFixed(2)}, p90 ${p90}, max ${max}`;
checks.push(['168 cape corners, one per drawn-triangle vertex', err.length === 168, `n ${err.length}`]);
checks.push(['GIF colours: max-channel error median <= 3, max <= 4', med <= 3 && max <= 4, stats]);

console.log(`hero 163235: pose h_st f${py.frame} (pinned fit median ${py.fit_median.toFixed(3)})`);
let bad = 0;
for (const [name, ok, info] of checks) { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}  (${info})`); if (!ok) bad++; }
process.exit(bad ? 1 : 0);
