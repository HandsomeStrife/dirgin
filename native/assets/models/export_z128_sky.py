#!/usr/bin/env python3
"""export_z128_sky.py -- the z128 sky dome, taken from the hero GS dump (163235).

The sky is not in any 0680* room BMD (its UV triangles match 1/165 across all 208
files), so we take its geometry straight from the dump: the main-framebuffer draws
with TEX0.TBP0 = 11504 (166 tris per frame), unprojected to world with the hero
camera (depth w = 1/q, same mapping as p000_cape.py). Texture = FileCode 0680ff35
(its decoded image correlates 1.000 with the dump's TBP 11504).

Colour. In the game the dome is drawn ADDITIVELY (ALPHA = Cs*As + Cd) with vertex
RGB 5/128 over the frame clear colour (38,38,47). Our renderer has no additive
blend and clears to its own colour, so the dome is drawn opaque and its vertex
colour is set so the textured result keeps the game's mean:
    out = tex * vcol * 2   ~=   clear + tex_mean * (5/128) * As
=> vcol = (clear + tex_mean*5/128*As) / (2*tex_mean), per channel and per vertex.
This keeps the mean, but the cloud contrast becomes ~0.3x the texture instead of the
game's ~0.04x. The game's visible clouds mostly come from a separate additive layer
(TBP 14272) drawn after the scene, which is not reproduced here.

Only valid for this camera: these are the 166 triangles the game drew this frame.

Scale. The dump puts the dome 5,400-13,700 units from the eye. render_scene_textured
derives its near plane from the bbox of ALL groups (znear = 0.0005*zfar), so a sky that
big pushes znear from ~11.9 to ~14.8 and clips the deck at the bottom-left. The dome is
therefore scaled about the eye by SKY_SCALE, which leaves the image unchanged (similar
triangles, same UVs) as long as it stays behind every room surface: the farthest
visible room pixel is 0.098 of the sky depth on the same ray (hero cam), and the room
bbox allows up to 0.217. 0.2 gives 2x margin; the script checks the bbox fit.

  python3 -I export_z128_room.py --pf out/z128_pktfc_v3 -o out/z128_room_v2/room   (room bbox)
  python3 -I export_z128_sky.py <hero dump.bin> out/z128_room_v2/room_sky.mesh
  python3 -I export_z128_room.py --pf out/z128_pktfc_v3 -o out/z128_room_v2/room --sky room_sky.mesh
"""
import os, sys, struct, array
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '../../../tools/gsdump/src'))
from gsparse import read_header
from gsrender import TriInterp

SKY_TBP = 11504
MAIN_FBP, MAIN_FBW = 224, 7          # main 448-wide framebuffer
CLEAR = np.array([38, 38, 47], float)  # frame clear sprite colour (dump, fb 224)
TEX_MEAN = np.array([132, 135, 146], float)  # mean of decoded TBP 11504 / 0680ff35
VCOL_RGB = 5.0                        # dump vertex RGB of every sky vertex
SKY_SCALE = 0.2                       # about the eye; see docstring
HERO_CAM = dict(eye=(-1419, -116, -1130), at=(-631, -77, -515), up=(0.030, -0.999, 0.023),
                fov=59.28, W=2628, H=2048, crop_w=2592, crop_x=36, dw=432, dh=512)
MESH_MAGIC, MESH_VERSION = b'DMSH', 1


def basis(cam=HERO_CAM):
    eye = np.array(cam['eye'], float); f = np.array(cam['at'], float) - eye; f /= np.linalg.norm(f)
    s = np.cross(f, np.array(cam['up'], float)); s /= np.linalg.norm(s); u = np.cross(s, f)
    return eye, f, s, u


def gs_to_world(X, Y, q, xyoffset, cam=HERO_CAM):
    eye, f, s, u = basis(cam)
    th = np.tan(np.radians(cam['fov']) / 2); asp = cam['W'] / cam['H']
    dx = (X - (xyoffset & 0xffff)) / 16.0; dy = (Y - ((xyoffset >> 32) & 0xffff)) / 16.0
    nx = (dx / cam['dw'] * cam['crop_w'] + cam['crop_x']) / cam['W'] * 2 - 1
    ny = 1 - (dy / cam['dh'] * cam['H']) / cam['H'] * 2
    return eye + (f + s * nx * th * asp + u * ny * th) / q


def last_frame_tris(path):
    """Triangles of the dump's last complete frame (between the last two vsyncs)."""
    buf = open(path, 'rb').read(); h = read_header(buf)
    gif = TriInterp()
    fz = h['freeze_at']          # XYOFFSET_1/2 inherited from the freeze block
    gif.xyoffset[0] = struct.unpack_from('<Q', buf, fz + 0x7c)[0]
    gif.xyoffset[1] = struct.unpack_from('<Q', buf, fz + 0xdc)[0]
    off = h['packets_at']; n = len(buf); vs = []
    while off < n:
        t = buf[off]; off += 1
        if t == 0:
            off += 1; size, = struct.unpack_from('<I', buf, off); off += 4
            gif.feed(buf[off:off + size]); off += size
        elif t == 1: vs.append(len(gif.tris)); off += 1
        elif t == 2: off += 4
        elif t == 3: off += 0x2000
        else: break
    if len(vs) < 2:
        sys.exit('need >= 2 vsyncs in the dump, found %d' % len(vs))
    return gif.tris[vs[-2]:vs[-1]]


def room_bbox(outdir):
    """Union bbox of the room_gNN.mesh headers already exported next to the sky."""
    import glob
    fs = sorted(glob.glob(os.path.join(outdir, 'room_g[0-9]*.mesh')))
    if not fs:
        sys.exit('no room_gNN.mesh in %s: export the room first (without --sky)' % outdir)
    bb = np.array([struct.unpack_from('<6f', open(f, 'rb').read(36), 12) for f in fs])
    return bb[:, :3].min(0), bb[:, 3:].max(0)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    tris = last_frame_tris(sys.argv[1])
    eye = np.array(HERO_CAM['eye'], float)
    flat = []; nt = 0
    for a, b, c, ctx in tris:
        fr = ctx['frame']
        if not ctx['tme'] or (ctx['tex0'] & 0x3fff) != SKY_TBP or ctx['fst']:
            continue
        if (fr & 0x1ff, (fr >> 16) & 0x3f) != (MAIN_FBP, MAIN_FBW):
            continue
        V = [v for v in (a, b, c)]
        if any(v[5] <= 0 for v in V) or any(tuple(v[8:11]) != (VCOL_RGB,) * 3 for v in V):
            sys.exit('unexpected sky vertex (q<=0 or rgb != 5): %r' % (V,))
        P = [eye + SKY_SCALE * (gs_to_world(v[0], v[1], v[5], ctx['xyoffset']) - eye) for v in V]
        nrm = eye - np.mean(P, axis=0); nrm /= np.linalg.norm(nrm)   # faces the camera
        for v, p in zip(V, P):
            As = min(v[11], 128) / 128.0
            col = (CLEAR + TEX_MEAN * VCOL_RGB / 128.0 * As) / (2 * TEX_MEAN)
            flat.extend((p[0], p[1], p[2], nrm[0], nrm[1], nrm[2],
                         v[3] / v[5], v[4] / v[5], col[0], col[1], col[2], 1.0))
        nt += 1
    if nt == 0:
        sys.exit('no TBP %d draws in the last frame' % SKY_TBP)
    P = np.array(flat, np.float32).reshape(-1, 12)[:, :3]
    bmin, bmax = P.min(0), P.max(0)
    rlo, rhi = room_bbox(os.path.dirname(os.path.abspath(sys.argv[2])))
    if (bmin < rlo).any() or (bmax > rhi).any():
        sys.exit('scaled sky bbox %s..%s leaves the room bbox %s..%s; it would move the '
                 'renderer near plane' % (bmin.round(0), bmax.round(0), rlo.round(0), rhi.round(0)))
    with open(sys.argv[2], 'wb') as f:
        f.write(struct.pack('<4sII', MESH_MAGIC, MESH_VERSION, nt * 3))
        f.write(struct.pack('<6f', *bmin, *bmax))
        array.array('f', flat).tofile(f)
    d = np.linalg.norm(P - eye, axis=1)
    print('sky: %d tris -> %s  (distance from eye %.0f..%.0f)' % (nt, sys.argv[2], d.min(), d.max()))


if __name__ == '__main__':
    main()
