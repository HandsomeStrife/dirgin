#!/usr/bin/env python3
"""Grade the gun (export_cerberus.py, render_scene_textured -charprop) against the
PCSX2 shot, in the gun's region only.

Region = the dump's gun triangles rasterised at 640x480 (where the game drew the gun)
UNION our gun's pixels (where the -charprop render differs from the same render
without it), dilated 2 px, HUD masked. Screenshot registration: (1.015x+1, 1.00y) at
640x480 (p000_cloth_render.registered_game). Reports mean |d| and SSIM for the render
with and without the gun, plus the silhouette IoU of ours vs the dump's (the dump raster ignores
occlusion: in the hero frame the hair hides most of the gun) and the colour where both
draw the gun ('overlap').

Run: python3 -I cerberus_grade.py <gun640.png> <nogun640.png> <game.png> <dump.bin> <p000_export_dir> [--cam CAM.json]
"""
import sys, os, json
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import p000_cloth_render as CR
import p000_cape as C
import export_cerberus as E


def dump_mask(dumppath, cam):
    _b, _V, _N, gtris, _gm, _part = E.decode_gun()
    sigs = {E.tri_sig(E.uvn(t)): 1 for t in gtris}
    dtris, _tex, _n = E.dump_gun(dumppath, sigs, cam)
    im = Image.new('L', (640, 480), 0); dr = ImageDraw.Draw(im)
    for _t0, vs in dtris:
        d = C.world_to_drawable(np.array([w for w, _uv in vs]), cam)
        dr.polygon([(x * 640 / cam['dw'], y * 480 / cam['dh']) for x, y in d], fill=255)
    return np.asarray(im) > 0


def dilate(m, r=2):
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(np.roll(m, dy, 0), dx, 1)
    return out


def main(a):
    cam = dict(C.HERO_CAM)
    if '--cam' in a:
        j = json.load(open(a[a.index('--cam') + 1])); a = a[:a.index('--cam')]
        cam.update(eye=tuple(j['eye']), at=tuple(j['at']), up=tuple(j['up']), fov=j['fov'])
    gun, nogun, game, dump, _p000 = a
    r1 = np.asarray(Image.open(gun).convert('RGB'), float)
    r0 = np.asarray(Image.open(nogun).convert('RGB'), float)
    g = CR.registered_game(game)
    ours = np.abs(r1 - r0).max(2) > 0
    dm = dump_mask(dump, cam)
    hud = np.ones((480, 640), bool)
    for x0, y0, x1, y1 in CR.HUD: hud[y0:y1, x0:x1] = False
    reg = dilate(ours | dm) & hud
    out = dict(region_px=int(reg.sum()), ours_px=int((ours & hud).sum()), dump_px=int((dm & hud).sum()),
               iou=round(float((ours & dm & hud).sum() / max((( ours | dm) & hud).sum(), 1)), 3))
    for k, r in (('gun', r1), ('nogun', r0)):
        d = np.abs(r - g).mean(2); s = CR.ssim_map(r.mean(2), g.mean(2))
        out[k] = dict(mean_abs=round(float(d[reg].mean()), 2), ssim=round(float(s[reg].mean()), 4))
    # gun on gun: pixels where both we and the dump draw the gun (colour, not placement)
    ov = ours & dm & hud; d = np.abs(r1 - g).mean(2)
    out['overlap'] = dict(px=int(ov.sum()), mean_abs=round(float(d[ov].mean()), 2),
                          ours_rgb=[round(float(x), 1) for x in r1[ov].mean(0)],
                          game_rgb=[round(float(x), 1) for x in g[ov].mean(0)])
    print(json.dumps(out))
    return out


if __name__ == '__main__':
    main(sys.argv[1:])
