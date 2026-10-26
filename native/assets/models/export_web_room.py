#!/usr/bin/env python3
"""export_web_room.py -- room z128 + a static Vincent for the browser viewer (native/web).

Reads the native renderer's inputs and writes what render_scene_textured would
upload to the GPU, already resolved, so the web client needs no decoder:

  ROOM   out/z128_room_v2/room.list  (@fog, @nofog, mesh, TXD container, ABE, aref)
         each mesh's DMSH vertices copied verbatim; each container decoded with the
         trusted `dtex` CLI, LARGEST image kept (first one on a tie, like
         tex_load_largest), RGBA as dtex gives it (alpha = A*255/128).
  CHAR   out/p000_gpu3/vincent_p000.mesh + vincent_p000_tex.rgba, placed in world
         space with placement.json render_args (place_character math, feet = max-Y
         because the room is -Y up), split into the -charruns draws, every run drawn
         PRELIT with the GS alpha test at char_alpha_ref (-charprelit -charatest).

Output (default native/web/data/, gitignored -- it is game-derived and must never be
committed; the script refuses to write anywhere git would not ignore):

  scene.json   see SCHEMA below
  *.bin        float32 LE, 12 floats/vertex: px py pz nx ny nz u v r g b a
               (the native RVertex / DMSH body), triangle list
  *.rgba       raw RGBA8, rows top-first, width*height*4 bytes, no header

SCHEMA (scene.json, version 1)
  up       [0,-1,0]       world up (z128 is -Y up)
  fog      {color:[r,g,b] 0..1, near, far, min} or null.  F = clamp((far-w)/(far-near),
           min, 1) per vertex, w = view depth; rgb = mix(color, rgb, F)
  camera   {eye, at, up, fov}   the hero camera (fov vertical, degrees)
  bbox     {min, max}     union of every draw (room + char), for near/far
  clear    [r,g,b] 0..1   native clear colour
  textures {id: {file, w, h}}
  meshes   {id: {file, vcount}}
  draws    [ {name, kind 'room'|'char', mesh, first, count (vertices), tex|null,
              aref (-1 = opaque, else GS ATE two-pass ref 0..1), fog bool} ]  in draw order
  char     {mesh, s, yaw, x, y, z, pivot, runs:[[start_tri,count],...], aref, bbox}
"""
import argparse, json, os, re, shutil, struct, subprocess, sys, tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(HERE, "out")
DTEX = os.path.join(ROOT, "native", "assets", "textures", "dtex")


def load_dmsh(path):
    b = open(path, "rb").read()
    if b[:4] != b"DMSH":
        raise SystemExit(f"{path}: bad magic")
    ver, n = struct.unpack_from("<II", b, 4)
    bb = struct.unpack_from("<6f", b, 12)
    body = b[36:]
    if ver != 1 or n == 0 or n % 3 or len(body) != n * 48:
        raise SystemExit(f"{path}: size mismatch (v={n} ver={ver})")
    return np.frombuffer(body, "<f4").reshape(n, 12).copy(), bb


def largest_image(container, cache):
    """dtex-decode a container, return (rgba uint8 [h,w,4]) of the largest image."""
    if container in cache:
        return cache[container]
    from PIL import Image
    info = subprocess.run([DTEX, "--info", container], capture_output=True, text=True, timeout=60).stdout
    imgs = []   # (index order) name, w, h
    for ln in info.splitlines():
        p = ln.split()
        if len(p) >= 6 and p[0] == "img":
            imgs.append((p[1], p[3], int(p[4]), int(p[5])))
    if not imgs:
        raise SystemExit(f"dtex: no images in {container}")
    best = max(range(len(imgs)), key=lambda i: (imgs[i][2] * imgs[i][3], -i))
    fc, name, w, h = imgs[best]
    with tempfile.TemporaryDirectory() as td:
        subprocess.run([DTEX, container, "-o", td], capture_output=True, timeout=60, check=True)
        png = os.path.join(td, f"{fc}_{name}.png")
        if not os.path.exists(png):
            raise SystemExit(f"dtex: expected {png}, got {os.listdir(td)}")
        im = np.asarray(Image.open(png).convert("RGBA"))
    if im.shape[:2] != (h, w):
        raise SystemExit(f"{container}: dtex image {im.shape} != info {w}x{h}")
    cache[container] = im
    return im


def parse_list(path):
    fog, nofog, groups = None, set(), []
    for ln in open(path):
        s = ln.strip()
        if not s or s.startswith("#"):
            continue
        if s.startswith("@fog"):
            r, g, b, n, f, mn = map(float, s.split()[1:7])
            fog = dict(color=[r / 255, g / 255, b / 255], near=n, far=f, min=mn)
        elif s.startswith("@nofog"):
            nofog.add(s.split()[1])
        elif s.startswith("@"):
            raise SystemExit(f"unknown directive {s}")
        else:
            c = ln.rstrip("\n").split("\t")
            aref = -1.0
            if len(c) > 3 and c[3]:
                x = float(c[3])
                aref = x if 0.0 <= x <= 1.0 else -1.0
            groups.append(dict(mesh=c[0], tex=c[1] if len(c) > 1 else "NONE", aref=aref))
    return fog, nofog, groups


def place_character(v, bbmin, bbmax, s, yaw, tx, fy, tz, up_y):
    """render_scene_textured.place_character, in numpy."""
    pivx = (bbmin[0] + bbmax[0]) * 0.5
    pivz = (bbmin[2] + bbmax[2]) * 0.5
    pivy = bbmax[1] if up_y < 0 else bbmin[1]
    cy, sy = np.cos(np.radians(yaw)), np.sin(np.radians(yaw))
    d = v.copy()
    px, py, pz = (v[:, 0] - pivx) * s, (v[:, 1] - pivy) * s, (v[:, 2] - pivz) * s
    d[:, 0] = cy * px + sy * pz + tx
    d[:, 1] = py + fy
    d[:, 2] = -sy * px + cy * pz + tz
    d[:, 3] = cy * v[:, 3] + sy * v[:, 5]
    d[:, 5] = -sy * v[:, 3] + cy * v[:, 5]
    return d.astype("<f4"), [pivx, pivy, pivz]


def args_dict(s):
    t = s.split()
    return {t[i]: t[i + 1] for i in range(0, len(t) - 1, 2) if t[i].startswith("-")}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--room", default=os.path.join(OUT, "z128_room_v2", "room.list"))
    ap.add_argument("--char", default=os.path.join(OUT, "p000_gpu3"))
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "native", "web", "data"))
    a = ap.parse_args()

    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    probe = os.path.join(out, "probe.bin")
    if subprocess.run(["git", "-C", ROOT, "check-ignore", "-q", probe]).returncode != 0:
        raise SystemExit(f"refusing: {out} is not gitignored (game-derived data)")

    for f in os.listdir(out):                         # stale files from an older export
        if f.endswith((".bin", ".rgba")) or f == "scene.json":
            os.remove(os.path.join(out, f))

    fog, nofog, groups = parse_list(a.room)
    ldir = os.path.dirname(os.path.abspath(a.room))
    cache, textures, meshes, draws = {}, {}, {}, []
    lo, hi = np.full(3, 1e30), np.full(3, -1e30)

    def add_tex(tid, rgba):
        open(os.path.join(out, tid + ".rgba"), "wb").write(np.ascontiguousarray(rgba, np.uint8).tobytes())
        textures[tid] = dict(file=tid + ".rgba", w=int(rgba.shape[1]), h=int(rgba.shape[0]))

    for g in groups:
        v, _ = load_dmsh(os.path.join(ldir, g["mesh"]))
        mid = os.path.splitext(g["mesh"])[0]
        v.astype("<f4").tofile(os.path.join(out, mid + ".bin"))
        meshes[mid] = dict(file=mid + ".bin", vcount=len(v))
        lo, hi = np.minimum(lo, v[:, :3].min(0)), np.maximum(hi, v[:, :3].max(0))
        tid = None
        if g["tex"] != "NONE":
            tid = "t_" + os.path.basename(g["tex"]).split(".")[0]
            if tid not in textures:
                add_tex(tid, largest_image(g["tex"], cache))
        aref = g["aref"] if tid else -1.0
        draws.append(dict(name=g["mesh"], kind="room", mesh=mid, first=0, count=len(v),
                          tex=tid, aref=aref, fog=fog is not None and g["mesh"] not in nofog))

    pl = json.load(open(os.path.join(a.char, "placement.json")))
    ra, ca = args_dict(pl["render_args"]), args_dict(pl["camera_args"])
    cam = dict(eye=[float(x) for x in ca["-eye"].split(",")], at=[float(x) for x in ca["-at"].split(",")],
               up=[float(x) for x in ca["-up"].split(",")], fov=float(ca["-fov"]))
    cv, cbb = load_dmsh(os.path.join(a.char, "vincent_p000.mesh"))
    s, yaw = float(ra["-s"]), float(ra["-yaw"])
    tx, fy, tz = float(ra["-x"]), float(ra["-y"]), float(ra["-z"])
    placed, piv = place_character(cv, cbb[:3], cbb[3:], s, yaw, tx, fy, tz, cam["up"][1])
    placed.tofile(os.path.join(out, "vincent.bin"))
    meshes["vincent"] = dict(file="vincent.bin", vcount=len(placed))
    lo, hi = np.minimum(lo, placed[:, :3].min(0)), np.maximum(hi, placed[:, :3].max(0))

    raw = open(os.path.join(a.char, "vincent_p000_tex.rgba"), "rb").read()
    w, h = struct.unpack_from("<II", raw, 0)
    add_tex("t_vincent", np.frombuffer(raw, np.uint8, w * h * 4, 8).reshape(h, w, 4))

    ntri = len(placed) // 3
    runs = [[int(x) for x in r.split(":")] for r in ra["-charruns"].split(",")] if "-charruns" in ra else [[0, ntri]]
    cov = np.zeros(ntri, int)
    for st, n in runs:
        cov[st:st + n] += 1
    if not (cov == 1).all():
        raise SystemExit("-charruns does not cover every character triangle exactly once")
    aref = float(pl.get("char_alpha_ref", -1))
    for i, (st, n) in enumerate(runs):
        draws.append(dict(name=f"vincent_run{i:02d}", kind="char", mesh="vincent", first=3 * st, count=3 * n,
                          tex="t_vincent", aref=aref, fog=fog is not None))

    scene = dict(version=1, up=[0, -1, 0], fog=fog, camera=cam,
                 bbox=dict(min=lo.tolist(), max=hi.tolist()), clear=[24 / 255, 26 / 255, 32 / 255],
                 textures=textures, meshes=meshes, draws=draws,
                 char=dict(mesh="vincent", s=s, yaw=yaw, x=tx, y=fy, z=tz, pivot=piv, runs=runs, aref=aref,
                           bbox=dict(min=placed[:, :3].min(0).tolist(), max=placed[:, :3].max(0).tolist())))
    json.dump(scene, open(os.path.join(out, "scene.json"), "w"), indent=1)
    mb = sum(os.path.getsize(os.path.join(out, f)) for f in os.listdir(out)) / 1e6
    print(f"wrote {out}: {len(meshes)} meshes, {len(textures)} textures, {len(draws)} draws, "
          f"{sum(m['vcount'] for m in meshes.values()) // 3} tris, {mb:.1f} MB")


if __name__ == "__main__":
    main()
