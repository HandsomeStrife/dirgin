#!/usr/bin/env python3
"""Export Vincent's gun (Cerberus) for the browser port, as a static mesh in the space
of joint 56 (the weapon socket), so play.js / actions/shoot.js parents it to node j56.

Attach (export_cerberus.py, decoded): gun vertex in Vincent's model space =
palette56 x T(marker 15 offset) x gun_vertex, where gun_vertex is the gun decoded at its
own bind pose. The glb joint node j56 carries palette56 (its skin IBM is inv(bind56)), so
the mesh local to j56 is just T(marker offset) x gun_vertex: no dump or pose needed.

Parts and textures: the parts the game draws and each part's texture are taken from an
export_cerberus.py run (default out/cerberus_hero/cerberus.json: frame0 + barrel0 +
barrel1; frame -> w022_1, barrels -> w022_3), because picking them needs a GS dump.

Outputs (outdir, default native/web/data/vincent/; git-ignored, game-derived):
  cerberus_web.bin   float32 per corner: x y z (j56 space, game -Y up), u v, r g b a
                     (vertex colour, 1.0 = 0x80, i.e. x2 like the room shader)
  cerberus_web.png   texture atlas (the three 128x128 07770582 textures stacked)
  cerberus_web.json  {corners, stride, atlas, parts, marker_offset, bbox}

Run: python3 -I native/assets/models/export_web_gun.py [cerberus.json] [outdir]
"""
import os, sys, json
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import export_cerberus as EC

ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))


def main(argv):
    info_path = argv[0] if argv else os.path.join(HERE, 'out', 'cerberus_hero', 'cerberus.json')
    outdir = argv[1] if len(argv) > 1 else os.path.join(ROOT, 'native', 'web', 'data', 'vincent')
    info = json.load(open(info_path))
    sel = set(info['parts'])

    vm = EC.markers(open(EC.VIN_CONT, 'rb').read())
    moff, mbone = vm[EC.MARKER]
    if mbone != 56:
        raise SystemExit(f"marker {EC.MARKER} is on bone {mbone}, expected 56")
    _b, GV, _GN, gtris, gm, part = EC.decode_gun()
    X = GV + moff                                   # T(marker) x gun vertex: j56 space

    names, files = EC.gun_textures()
    # texture of each group from the export_cerberus run (keys = group names)
    gtex = {k: names.index(v) for k, v in info['group_texture'].items() if v in names}
    by_part = {}
    for k, ti in gtex.items(): by_part.setdefault(k.split('@')[0].rstrip('_'), ti)

    H = sum(f.shape[0] for f in files); W = max(f.shape[1] for f in files)
    atlas = np.zeros((H, W, 4), np.uint8); yo = []; y = 0
    for f in files:
        atlas[y:y + f.shape[0], :f.shape[1]] = f; yo.append(y); y += f.shape[0]
    atlas[:, :, 3] = 255

    flat = []
    for t in gtris:
        p = part[t[6]]
        if p not in sel: continue
        ti = gtex.get(gm[t[6]][0], by_part.get(p, 0)); fh, fw = files[ti].shape[:2]
        uvs = EC.uvn(t)
        for k in range(3):
            v = X[t[k]]; col = t[7 + k]
            flat.append((v[0], v[1], v[2], uvs[k][0] * fw / W, (yo[ti] + uvs[k][1] * fh) / H,
                         col[0] / 128.0, col[1] / 128.0, col[2] / 128.0, col[3] / 128.0))
    a = np.array(flat, '<f4')
    os.makedirs(outdir, exist_ok=True)
    a.tofile(os.path.join(outdir, 'cerberus_web.bin'))
    Image.fromarray(atlas).save(os.path.join(outdir, 'cerberus_web.png'))
    meta = dict(source='export_web_gun.py', corners=len(a), stride=9, atlas='cerberus_web.png',
                parts=sorted(sel), bone=56, marker=EC.MARKER, marker_offset=moff.tolist(),
                bbox=[a[:, :3].min(0).round(3).tolist(), a[:, :3].max(0).round(3).tolist()],
                note='j56-local (game model space, -Y up); colour 1.0 = 0x80')
    json.dump(meta, open(os.path.join(outdir, 'cerberus_web.json'), 'w'), indent=1)
    print(f"wrote {outdir}/cerberus_web.bin ({len(a) // 3} tris), .png {W}x{H}, .json; bbox {meta['bbox']}")


if __name__ == '__main__':
    main(sys.argv[1:])
