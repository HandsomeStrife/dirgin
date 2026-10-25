#!/usr/bin/env python3
"""Render helper for p000_cloth.py (does not touch the exporter).

Takes an export made by export_p000_gpu2.py --clip CLIP --frame F (body posed from
the clip, cape still the dump's), swaps the cape vertices for p000_cloth's output,
renders with render_scene_textured and grades against the PCSX2 shot.

The exporter appends the cape after the body, 3 vertices per file triangle, in the
same traversal order as p000_cloth.Cape.tris (checked here through the UVs).

  p000_cloth_render.py frame <export_dir> <out_dir> <clip> <frame> [--sim-from F0] [--prev CLIP:F]
      --sim-from F0: simulate from F0 (clip start, window armed) up to <frame>;
      default: k from the clip header with no history (= skinned target for h_swtch).
  p000_cloth_render.py grade <render640.png> [game.png]
"""
import sys, os, json, shutil, struct, subprocess
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import p000_cloth as C
import skd_skeleton as SK
import anim_rot as AR

ROOT = "/srv/games/dirge-port"
GAME = ROOT + "/../../shared/dirge-port/save-states/sea-of-flames-gs-dumps/" \
       "Dirge of Cerberus - Final Fantasy VII_SLES-54185_20261017163235.png"
GAME = os.path.normpath(GAME)
ROOM = ROOT + "/native/assets/models/out/z128_room_v2/room.list"
RENDERER = ROOT + "/native/gpu/render_scene_textured"
HUD = [(0, 0, 312, 64), (0, 398, 264, 472)]          # x0, y0, x1, y1 at 640x480
VINCENT = (262, 180, 440, 480)


def swap_cape(export_dir, out_dir, pos, nrm):
    os.makedirs(out_dir, exist_ok=True)
    for f in ('vincent_p000_tex.rgba', 'vincent_p000_tex.png', 'placement.json'):
        shutil.copy(os.path.join(export_dir, f), os.path.join(out_dir, f))
    raw = open(os.path.join(export_dir, 'vincent_p000.mesh'), 'rb').read()
    n = struct.unpack_from('<I', raw, 8)[0]; hdr = raw[:36]
    V = np.frombuffer(raw[36:], dtype='<f4').reshape(n, 12).copy()
    cape = C.Cape(); nc = 3 * len(cape.tris); cv = V[n - nc:]
    # order check: stored atlas UV is an affine map of the file UV
    fu = np.array([uv for _v, uvs, _c in cape.tris for uv in uvs])
    A = np.c_[fu, np.ones(len(fu))]
    res = np.abs(A @ np.linalg.lstsq(A, cv[:, 6:8], rcond=None)[0] - cv[:, 6:8]).max()
    if res > 1e-4:
        raise ValueError("cape vertex order mismatch (uv residual %g)" % res)
    vid = [v for vs, _u, _c in cape.tris for v in vs]
    cv[:, 0:3] = pos[vid]
    nn = nrm[vid] / np.maximum(np.linalg.norm(nrm[vid], axis=1, keepdims=True), 1e-9)
    cv[:, 3:6] = nn
    V[n - nc:] = cv
    with open(os.path.join(out_dir, 'vincent_p000.mesh'), 'wb') as f:
        f.write(hdr); f.write(V.astype('<f4').tobytes())
    return out_dir


def render(mesh_dir, out_png, tag='T'):
    pl = json.load(open(os.path.join(mesh_dir, 'placement.json')))
    od = os.path.join(mesh_dir, 'render'); os.makedirs(od, exist_ok=True)
    cmd = [RENDERER, '-l', ROOM, '-m', os.path.join(mesh_dir, 'vincent_p000.mesh'),
           '-t', os.path.join(mesh_dir, 'vincent_p000_tex.rgba')] + pl['render_args'].split() + \
          ['-colscale', '1.0', '-charprelit', '-charatest', str(pl['char_alpha_ref']),
           '-W', '2628', '-H', '2048'] + pl['camera_args'].split() + ['-o', od, '-tag', tag]
    env = dict(os.environ, EGL_PLATFORM='surfaceless', GALLIUM_DRIVER='llvmpipe')
    subprocess.run(cmd, check=True, env=env, stdout=subprocess.DEVNULL)
    subprocess.run(['convert', os.path.join(od, tag + '_freecam.png'), '-crop', '2592x2048+36+0', '+repage',
                    '-resize', '640x480!', out_png], check=True)
    return out_png


def registered_game(game=GAME):
    """The PCSX2 shot sampled at (1.015x+1, 1.00y) at 640x480 (bilinear)."""
    g = np.asarray(Image.open(game).convert('RGB').resize((640, 480), Image.BILINEAR), float)
    xs = np.clip(1.015 * np.arange(640) + 1, 0, 639); x0 = np.floor(xs).astype(int); x1 = np.minimum(x0 + 1, 639)
    fx = (xs - x0)[None, :, None]
    return g[:, x0] * (1 - fx) + g[:, x1] * fx


def _box7(a):
    p = np.pad(a, 3, mode='reflect'); c = np.cumsum(np.cumsum(p, 0), 1)
    c = np.pad(c, ((1, 0), (1, 0)))
    return (c[7:, 7:] - c[:-7, 7:] - c[7:, :-7] + c[:-7, :-7]) / 49.0


def ssim_map(x, y, L=255.0):
    """SSIM map, 7x7 uniform window, sample covariance (= skimage defaults)."""
    C1, C2 = (0.01 * L) ** 2, (0.03 * L) ** 2; k = 49 / 48.0
    mx, my = _box7(x), _box7(y)
    vx = k * (_box7(x * x) - mx * mx); vy = k * (_box7(y * y) - my * my); cxy = k * (_box7(x * y) - mx * my)
    return ((2 * mx * my + C1) * (2 * cxy + C2)) / ((mx * mx + my * my + C1) * (vx + vy + C2))


def grade(png, game=GAME):
    r = np.asarray(Image.open(png).convert('RGB'), float); g = registered_game(game)
    mask = np.ones((480, 640), bool)
    for x0, y0, x1, y1 in HUD:
        mask[y0:y1, x0:x1] = False
    smap = ssim_map(r.mean(2), g.mean(2))
    d = np.abs(r - g).mean(2)
    x0, y0, x1, y1 = VINCENT; vm = np.zeros_like(mask); vm[y0:y1, x0:x1] = True; vm &= mask
    return dict(full=dict(mean_abs=round(float(d[mask].mean()), 2), ssim=round(float(smap[mask].mean()), 4)),
                vincent=dict(mean_abs=round(float(d[vm].mean()), 2), ssim=round(float(smap[vm].mean()), 4)))


def cape_frame(clip, frame, sim_from=None, prev=None):
    cape = C.Cape(); sk = SK.load_skeleton(C.CONT, trans_scale=1.0)
    cb, CP = AR.find_clip(C.CLIP_CONT, clip)
    W = C.bone_mats(sk, cb, CP, frame % AR.clip_frames(cb, CP))      # absolute time -> local
    T, Nn = cape.targets(W, normals=True)
    if sim_from is None:
        _fl, win = C.clip_window(cb, CP)
        return T, Nn, dict(cape="skinned target, no sim", clip_window=win)
    frames = [float(f) for f in np.arange(sim_from, frame + 1e-6, 1.0)]
    pos, tg, ks, info = C.run_clip(cape, sk, clip, frames, prev=prev)
    dev = np.linalg.norm(pos[-1] - tg[-1], axis=1)
    return pos[-1], Nn, dict(info, k=round(float(ks[-1]), 3), ticks=len(frames),
                             median_dev=round(float(np.median(dev)), 3), max_dev=round(float(dev.max()), 3))


def main(a):
    if a and a[0] == 'frame':
        exp, out, clip, fr = a[1], a[2], a[3], float(a[4])
        sim_from = float(a[a.index('--sim-from') + 1]) if '--sim-from' in a else None
        prev = None
        if '--prev' in a:
            pc, pf = a[a.index('--prev') + 1].split(':'); prev = (pc, float(pf))
        pos, nrm, info = cape_frame(clip, fr, sim_from, prev)
        swap_cape(exp, out, pos, nrm)
        png = render(out, os.path.join(out, 'final.png'))
        print(json.dumps(dict(info, png=png, grade=grade(png))))
    elif a and a[0] == 'grade':
        print(json.dumps(grade(a[1], *(a[2:3] or [GAME]))))
    else:
        print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
