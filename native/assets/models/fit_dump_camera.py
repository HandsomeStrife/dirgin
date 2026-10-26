#!/usr/bin/env python3
"""Fit the camera of a z128 GS dump taken from another view, against the hero dump.

Room triangles are matched by UV-triple signature (TBPs differ between captures);
the hero dump's vertices are unprojected through HERO_CAM to world. Repeated objects
share signatures, so: RANSAC (4 triangles) then refit on inliers (< 0.5 px). The
model is linear: 1/q = f.(P-eye), and nx*z, ny*z are linear in P too, so fov, eye
and basis fall out of three least-squares fits. |f| == 1 and fov == the hero's
confirm the same room and projection. Hero dump self-check: 0.0 px.

Run: python3 -I fit_dump_camera.py <dump.bin> <out.json> --body 0x2e60 --cape 0x2ff4 --glow 0x3074
     (Vincent's TBPs in that dump; written into the JSON for export_p000_gpu2 --dump-camera)
"""
import sys, os, collections, json
import numpy as np
sys.path.insert(0, "/srv/games/dirge-port/native/assets/models"); sys.path.insert(0, "/srv/games/dirge-port/tools/gsdump/src")
import p000_cape as C
from gsparse import read_header
from gsrender2 import build, tex0_fields
from char_dumpbind import tri_sig
import char_dumpbind; char_dumpbind.ROUND = 4096
HERO = '/srv/games/dirge-port/tools/gsdump/dumps/z128_hero_163235.bin'
EXCL = {0x2cd4, 0x2e68, 0x2ee8, 0x2eec}   # hero Vincent; the target's are added from argv
cam = C.HERO_CAM
def tris(path, vframe):
    buf = open(path,'rb').read(); g = build(buf, read_header(buf))
    sig = collections.defaultdict(list)
    for a,b,c,ctx in g.tris:
        if ctx['vframe'] != vframe or not ctx['tme'] or ctx['fst']: continue
        if tex0_fields(ctx['tex0'])['tbp0'] in EXCL: continue
        if any(abs(v[5]) < 1e-6 for v in (a,b,c)): continue
        uv = [(v[3]/v[5], v[4]/v[5]) for v in (a,b,c)]
        off = ctx['xyoffset']
        sig[tri_sig(uv)].append({(round(v[3]/v[5]*4096), round(v[4]/v[5]*4096)): v for v in (a,b,c)} | {'off': off})
    return sig
def n_of(dx, dy):
    return (dx/cam['dw']*cam['crop_w'] + cam['crop_x'])/cam['W']*2 - 1, 1 - dy/cam['dh']*2
def solve(P, z, nx, ny):
    A = np.c_[P, np.ones(len(P))]
    return [np.linalg.lstsq(A, y, rcond=None)[0] for y in (z, nx*z, ny*z)]
def err(co, P, nx, ny):
    A = np.c_[P, np.ones(len(P))]; zp = A@co[0]
    ex = (A@co[1]/zp - nx)*cam['W']/2*cam['dw']/cam['crop_w']; ey = (A@co[2]/zp - ny)*cam['dh']/2
    return np.hypot(ex, ey)
av = sys.argv[1:]; opt = {av[i]: int(av[i + 1], 16) for i in range(2, len(av), 2)}
EXCL |= set(opt.values()) | {opt['--glow'] + 4}
hero = tris(HERO, 3)
heroW = {}
for k, lst in hero.items():
    heroW[k] = [{uv: C.gs_to_world(v[0], v[1], v[5], d['off']) for uv, v in d.items() if uv != 'off'} for d in lst]
rng = np.random.default_rng(0); out = {}
for name in [av[0]]:
    new = tris(name, 3)
    # candidate correspondences per new triangle: (3 world pts, 3 obs)
    T = []
    for k, lst in new.items():
        if k not in heroW or len(lst) != 1: continue
        d = lst[0]; obs = [(uv, v) for uv, v in d.items() if uv != 'off']
        for hw in heroW[k]:
            if all(uv in hw for uv, _ in obs):
                T.append((np.array([hw[uv] for uv, _ in obs]), np.array([[ (v[0]-(d['off']&0xffff))/16., (v[1]-((d['off']>>32)&0xffff))/16., v[5]] for _, v in obs])))
    P = np.concatenate([t[0] for t in T]); Q = np.concatenate([t[1] for t in T])
    z = 1/Q[:,2]; nx, ny = n_of(Q[:,0], Q[:,1]); tid = np.repeat(np.arange(len(T)), [len(t[0]) for t in T])
    best = (0, None)
    for it in range(4000):
        s = rng.choice(len(T), 4, replace=False); m = np.isin(tid, s)
        co = solve(P[m], z[m], nx[m], ny[m])
        e = err(co, P, nx, ny); inl = (e < 1.0).sum()
        if inl > best[0]: best = (inl, co)
    co = best[1]
    for _ in range(3):
        e = err(co, P, nx, ny); keep = e < 0.5; co = solve(P[keep], z[keep], nx[keep], ny[keep])
    e = err(co, P, nx, ny); keep = e < 0.5
    f, sx, uy = co[0][:3], co[1][:3], co[2][:3]
    th = 1/np.linalg.norm(uy); thx = 1/(np.linalg.norm(sx)*cam['W']/cam['H'])
    eye = np.linalg.solve(np.array([f, sx, uy]), -np.array([co[0][3], co[1][3], co[2][3]]))
    fn = f/np.linalg.norm(f); un = uy*th
    print(name, 'tri cands', len(T), 'inlier verts', keep.sum(), '/', len(P), 'median px', round(float(np.median(e[keep])),4),
          'p90', round(float(np.percentile(e[keep],90)),4), '|f|', round(float(np.linalg.norm(f)),5), 'fov', round(float(np.degrees(2*np.arctan(th))),4),
          'fovx', round(float(np.degrees(2*np.arctan(thx))),4), 'f.u', round(float(fn@un),5))
    print('   eye', np.round(eye,3), 'fwd', np.round(fn,4), 'up', np.round(un,5))
    out[name] = dict(eye=[round(float(x),3) for x in eye], at=[round(float(x),3) for x in eye+fn*1000], up=[round(float(x),5) for x in un/np.linalg.norm(un)],
                     fov=round(float(np.degrees(2*np.arctan(th))),4), inliers=int(keep.sum()), candidates=int(len(P)), median_px=round(float(np.median(e[keep])), 4),
                     depth_scale=round(float(np.linalg.norm(f)), 5), body_tbp=opt['--body'], cape_tbp=opt['--cape'], glow_tbp=opt['--glow'])
json.dump(out[av[0]], open(av[1], 'w'), indent=1)
