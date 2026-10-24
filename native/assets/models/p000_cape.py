#!/usr/bin/env python3
"""Vincent (p000) cape + hero-frame placement, recovered from the GS dump.

The cape is mesh block 0x19f70 of 07700001.bin (an smd named 'p000_L02', 112 tris).
It has NO skinning batches (its pivot count is 0), so its vertex positions are not
in the file: the game poses it at runtime (cloth). Its topology, UVs and vertex
colours ARE in the file, and its UVs are identical to the dump's cape draws
(texture 0x2e68). So: topology/UV/colour come from the file, and each vertex's
posed position for this frame comes from the dump vertex with the same UV.

Dump -> world uses the hero camera that render_scene_textured is run with
(HERO_CAM). Depth is w = 1/q: unprojecting the dump's ROOM vertices with
w = k/q lands them on our z128 mesh with a sharp peak at k = 1.00.
"""
import sys, os, collections
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, "/srv/games/dirge-port/tools/gsdump/src")
import render_skinned as R
from gsparse import read_header
from gsrender2 import build, tex0_fields

CAPE_BLOCK = 0x19f70          # smd header; payload at +0x10
CAPE_TBP = 0x2e68
QMIN = 1e-6

# The hero camera, exactly as passed to render_scene_textured (see notes/HANDOFF.md):
# GS drawable 432x512 -> crop 2592x2048+36+0 of a 2628x2048 render.
# Fitted (2026-10-24) to 5164 dump room vertices matched to our z128 mesh: median
# reprojection error 0.025 px (0.025 px at depth < 60). The old hand-rounded camera
# (eye -1419,-116,-1130 / at -631,-77,-515 / up 0.030,-0.999,0.023 / fov 59.28) was
# fine far away but 4.7 px off at depth ~25, so the dump-posed cape landed ~3 px
# off the railing posts and poked through them.
HERO_CAM = dict(eye=(-1419.227, -115.571, -1129.797), at=(-631.348, -77.227, -515.163),
                up=(0.03023, -0.99926, 0.02358),
                fov=59.2749, W=2628, H=2048, crop_w=2592, crop_x=36, dw=432, dh=512)


def _basis(cam=HERO_CAM):
    eye = np.array(cam['eye'], float); f = np.array(cam['at'], float) - eye; f /= np.linalg.norm(f)
    s = np.cross(f, np.array(cam['up'], float)); s /= np.linalg.norm(s); u = np.cross(s, f)
    return eye, f, s, u


def gs_to_world(X, Y, q, xyoffset, cam=HERO_CAM):
    """GS window coords (12.4) + q -> world point under the hero camera."""
    eye, f, s, u = _basis(cam)
    th = np.tan(np.radians(cam['fov']) / 2); asp = cam['W'] / cam['H']
    dx = (X - (xyoffset & 0xffff)) / 16.0; dy = (Y - ((xyoffset >> 32) & 0xffff)) / 16.0
    nx = (dx / cam['dw'] * cam['crop_w'] + cam['crop_x']) / cam['W'] * 2 - 1
    ny = 1 - (dy / cam['dh'] * cam['H']) / cam['H'] * 2
    return eye + (f + s * nx * th * asp + u * ny * th) / q


def world_to_drawable(P, cam=HERO_CAM):
    """Inverse of gs_to_world: world points -> GS drawable pixels (dx, dy)."""
    eye, f, s, u = _basis(cam)
    th = np.tan(np.radians(cam['fov']) / 2); asp = cam['W'] / cam['H']
    d = np.asarray(P, float) - eye; z = d @ f
    nx = (d @ s) / z / (th * asp); ny = (d @ u) / z / th
    rx = (nx + 1) / 2 * cam['W']; ry = (1 - ny) / 2 * cam['H']
    return np.c_[(rx - cam['crop_x']) / cam['crop_w'] * cam['dw'], ry / cam['H'] * cam['dh']]


def dump_tris(dumppath, tbps=None):
    """Textured, non-FST dump triangles: list of (tex0, [(world, uv, rgba)]*3)."""
    buf = open(dumppath, 'rb').read(); g = build(buf, read_header(buf)); out = []
    for a, b, c, ctx in g.tris:
        if not ctx['tme'] or ctx['fst']:
            continue
        if tbps is not None and tex0_fields(ctx['tex0'])['tbp0'] not in tbps:
            continue
        if any(abs(v[5]) < QMIN for v in (a, b, c)):
            continue
        out.append((ctx['tex0'], [(gs_to_world(v[0], v[1], v[5], ctx['xyoffset']),
                                   (v[3] / v[5], v[4] / v[5]), tuple(int(x) for x in v[8:12]))
                                  for v in (a, b, c)]))
    return out


def decode_cape_block(buf, NB, block=CAPE_BLOCK):
    """Topology/UV/colour of the unskinned cape block: [(uv(3), rgba(3))]."""
    P0 = block + 0x10
    if R._u32(buf, P0) != R.SM2_MAGIC:
        raise ValueError("cape block 0x%x: bad smd magic" % block)
    G = P0 + R._u32(buf, P0 + R._u32(buf, P0 + 0x20))
    descs = R.enum_descs(buf, NB, P0, G); tris = []
    for di, (_doff, vp) in enumerate(descs):
        st = P0 + vp; en = (P0 + descs[di + 1][1]) if di + 1 < len(descs) else NB
        for sub in R.parse_drawpackets(buf, NB, st, en):
            idx = sub.get('idx'); uv = sub.get('uv'); col = sub.get('col')
            if not idx or uv is None:
                continue
            for ja, jb, jc in R.sub_tris(idx, sub.get('prim')):
                if len(uv) <= max(ja, jb, jc):
                    continue
                tris.append((tuple((uv[j][0] / 4096.0, uv[j][1] / 4096.0) for j in (ja, jb, jc)),
                             tuple(R._corner_col(col, j) for j in (ja, jb, jc))))
    return tris


def posed_cape(buf, NB, dumppath, quant=256):
    """File cape topology posed with this frame's dump positions (matched by UV).
    Returns (tris [(world(3), uv(3), rgba(3))], cape_tex0, stats)."""
    ft = decode_cape_block(buf, NB)
    dt = dump_tris(dumppath, {CAPE_TBP})
    if not dt:
        raise ValueError("no 0x%x cape draws in dump" % CAPE_TBP)
    key = lambda uv: (int(round(uv[0] * quant)), int(round(uv[1] * quant)))
    pos = collections.defaultdict(list)
    for _t0, vs in dt:
        for w, uv, _c in vs:
            pos[key(uv)].append(w)
    pos = {k: np.mean(v, 0) for k, v in pos.items()}
    tex0 = collections.Counter(t0 for t0, _ in dt).most_common(1)[0][0]
    def interp(uv):
        """UV not drawn verbatim this frame: place it on the dump's cape surface by
        barycentric interpolation in UV space (nearest dump vertex if outside)."""
        p = np.array(uv)
        for _t0, vs in dt:
            a, b, c = (np.array(v[1]) for v in vs)
            M = np.c_[b - a, c - a]
            if abs(np.linalg.det(M)) < 1e-12:
                continue
            l1, l2 = np.linalg.solve(M, p - a)
            if l1 >= -1e-6 and l2 >= -1e-6 and l1 + l2 <= 1 + 1e-6:
                return (1 - l1 - l2) * vs[0][0] + l1 * vs[1][0] + l2 * vs[2][0], 'bary'
        k = min(pos, key=lambda k: (k[0] / quant - uv[0]) ** 2 + (k[1] / quant - uv[1]) ** 2)
        return pos[k], 'nearest'
    out = []; how = collections.Counter()
    for uvs, cols in ft:
        ws = []
        for uv in uvs:
            k = key(uv)
            if k in pos:
                ws.append(pos[k]); how['exact'] += 1
            else:
                w, h = interp(uv); ws.append(w); how[h] += 1
        out.append((tuple(ws), uvs, cols))
    return out, tex0, dict(file_tris=len(ft), posed=len(out), dump_tris=len(dt),
                           corners=dict(how))
