#!/usr/bin/env python3
"""render_skeleton.py -- validate recovered bind-pose bone matrices by rendering
the skeleton as a shaded 3D figure. Uses ONLY real recovered data
(skd_skeleton.load_skeleton -> world bind matrices). Writes PNGs to
../renderer/out/. Dependency: numpy + PIL (host tools), software rasteriser.

    python3 render_skeleton.py [container.bin]
"""
import sys, math, os
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from skd_skeleton import load_skeleton

CONT = sys.argv[1] if len(sys.argv) > 1 else \
    "/srv/games/dirge-port/extract/_KEL/noPath/07650001.bin"
OUT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "renderer", "out"))

sk = load_skeleton(CONT)
bc, parent = sk["bone_count"], sk["parent"]
joints = np.array([[w[0][3], w[1][3], w[2][3]] for w in sk["world_bind"]], float)
joints[:, 1] *= -1.0   # model up-axis is -Y; stand the figure upright

verts, tris = [], []
def add_octa(a, b, r):
    d = b - a; L = np.linalg.norm(d)
    if L < 1e-5: return
    d /= L
    up = np.array([0, 0, 1.0]) if abs(d[2]) < 0.9 else np.array([1.0, 0, 0])
    u = np.cross(d, up); u /= np.linalg.norm(u); v = np.cross(d, u)
    mid = a + d * (L * 0.22); base = len(verts)
    verts.append(a); verts.append(b)
    for s in [(1, 0), (0, 1), (-1, 0), (0, -1)]:
        verts.append(mid + (u*s[0] + v*s[1]) * r)
    ring = [base+2, base+3, base+4, base+5]
    for k in range(4):
        n = ring[(k+1) % 4]
        tris.append((base, ring[k], n)); tris.append((base+1, n, ring[k]))
for i in range(bc):
    p = parent[i]
    if 0 <= p < bc and p != i:
        add_octa(joints[p], joints[i], max(np.linalg.norm(joints[i]-joints[p])*0.14, 0.05*256))

V = np.array(verts); Tr = np.array(tris)

def render(angle, elev, path, W=900, H=1100):
    c = joints.mean(0); rad = np.linalg.norm(joints - c, axis=1).max() * 1.6
    az, el = math.radians(angle), math.radians(elev)
    eye = c + rad*np.array([math.cos(el)*math.sin(az), math.sin(el), math.cos(el)*math.cos(az)])
    f = c - eye; f /= np.linalg.norm(f)
    s = np.cross(f, [0, 1.0, 0]); s /= np.linalg.norm(s); u = np.cross(s, f)
    Vt = (V - eye) @ np.array([s, u, -f]).T
    fl = 1/math.tan(math.radians(42)/2); aspect = W/H
    img = np.zeros((H, W, 3), np.float32); zb = np.full((H, W), 1e9)
    L = np.array([0.4, 0.7, 0.6]); L /= np.linalg.norm(L)
    for t in Tr:
        p = Vt[list(t)]
        if np.any(p[:, 2] > -0.05): continue
        pr = []
        for q in p:
            pr.append((((fl/aspect*q[0]/-q[2])*0.5+0.5)*W, (0.5-(fl*q[1]/-q[2])*0.5)*H, -q[2]))
        pr = np.array(pr)
        n = np.cross(p[1]-p[0], p[2]-p[0]); nn = np.linalg.norm(n)
        if nn < 1e-9: continue
        col = np.array([0.55, 0.68, 0.95]) * (abs(np.dot(n/nn, L))*0.85+0.15) * 255
        minx, maxx = max(int(pr[:,0].min()),0), min(int(pr[:,0].max())+1,W)
        miny, maxy = max(int(pr[:,1].min()),0), min(int(pr[:,1].max())+1,H)
        if maxx<=minx or maxy<=miny: continue
        (x0,y0),(x1,y1),(x2,y2) = pr[0,:2],pr[1,:2],pr[2,:2]
        d = (y1-y2)*(x0-x2)+(x2-x1)*(y0-y2)
        if abs(d) < 1e-9: continue
        ys, xs = np.mgrid[miny:maxy, minx:maxx]; xf, yf = xs+0.5, ys+0.5
        a = ((y1-y2)*(xf-x2)+(x2-x1)*(yf-y2))/d
        b = ((y2-y0)*(xf-x2)+(x0-x2)*(yf-y2))/d
        g = 1-a-b; m = (a>=0)&(b>=0)&(g>=0)
        if not m.any(): continue
        z = a*pr[0,2]+b*pr[1,2]+g*pr[2,2]
        sub = zb[miny:maxy, minx:maxx]; upd = m & (z < sub)
        sub[upd] = z[upd]; img[miny:maxy, minx:maxx][upd] = col
    Image.fromarray(np.clip(img,0,255).astype(np.uint8)).save(path); print("wrote", path)

os.makedirs(OUT, exist_ok=True)
tag = os.path.splitext(os.path.basename(CONT))[0]
print(f"{CONT}: {bc} bones")
render(20, 8,  f"{OUT}/skel_{tag}_front.png")
render(70, 12, f"{OUT}/skel_{tag}_3q.png")
