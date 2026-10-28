#!/usr/bin/env python3
"""Export Vincent's gun (Cerberus) for the browser port, as a static mesh in the space
of joint 56 (the weapon socket), so play.js (native/web/gun.js) parents it to node j56.

Attach (export_cerberus.py, decoded): gun vertex in Vincent's model space =
palette56 x T(marker 15 offset) x gun_vertex, where gun_vertex is the gun decoded at its
own bind pose. The glb joint node j56 carries palette56 (its skin IBM is inv(bind56)), so
the mesh local to j56 is just T(marker offset) x gun_vertex: no dump or pose needed.

Parts and textures: the parts the game draws and each part's texture are taken from an
export_cerberus.py run (default out/cerberus_hero/cerberus.json: frame0 +
barrel1; frame -> w022_1, barrel -> w022_3), because picking them needs a GS dump.

Outputs (outdir, default native/web/data/vincent/; git-ignored, game-derived):
  cerberus_web.bin   float32 per corner: x y z (j56 space, game -Y up), u v, r g b a
                     (vertex colour, 1.0 = 0x80, i.e. x2 like the room shader)
  cerberus_web.png   texture atlas (the three 128x128 07770582 textures stacked)
  cerberus_web.json  {corners, stride, atlas, parts, marker_offset, bbox, runs, alpha_ref}
                     (runs = [first tri, count, ate]: triangles in the game's draw order)

No dump (--no-dump, the dump-free build in tools/byo): each triangle's texture comes from
the files instead (smd_material.py: draw-descriptor material slot -> 07770582 texture; on
the hero dump this agrees with all 165 gun triangles the dump draws with our UV triple),
and the parts are NODUMP_PARTS (GUESS: which parts the game draws for the equipped
Cerberus is not decoded; these are the ones the hero dump shows). With a cerberus.json
the dump only chooses the parts; textures come from the files per triangle in both modes.

Run: python3 -I native/assets/models/export_web_gun.py [cerberus.json] [outdir]
     python3 -I native/assets/models/export_web_gun.py --no-dump [outdir]
"""
import os, sys, json
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import export_cerberus as EC

ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))

# GUESS: the Cerberus parts drawn, frame0 + barrel1 (not decoded; see
# notes/actions-research.md "Part records": the weapon's part state picks them).
# Taken from the GS dumps (export_cerberus.py: parts with >= 3 dump corners not tied with
# another part). barrel0 is NOT drawn: hero, walk 185126 and walk 185138 draw none of its
# own triangles; its 4 triangles that are exact copies of barrel1's had won by argmin ties.
# Drawing both put two barrels in the same place (z-fighting stripes). --no-dump only.
NODUMP_PARTS = ('barrel1', 'frame0_0')

# GS TEST AREF 100 of the gun's ATE draws (dumps hero/185126/185138), in the page's alpha
# units (dtex alpha = GS x 255/128): same value as export_p000_gpu2.CAPE_AREF_GL
ALPHA_REF = round((100 - 0.5) / 128, 5)


def file_tex_index(mats, names):
    """Per triangle: index into gun_textures() names, from the file material slot
    (smd_material: draw-descriptor byte 0 -> 07770582 texture)."""
    import smd_material as SM
    tbl = SM.material_table(EC.GUN_TEX, 0, 0x2000)   # VRAM base: only used as a key
    out = []
    for m, _fl in mats:
        if m >= len(tbl):
            raise SystemExit(f"gun triangle material slot {m} not in the table ({len(tbl)})")
        hit = [i for i, n in enumerate(names) if n.endswith('_' + tbl[m]['image_name'] + '.png')]
        if len(hit) != 1:
            raise SystemExit(f"gun material {tbl[m]['image_name']}: {len(hit)} dtex images match")
        out.append(hit[0])
    return out


def main(argv):
    nodump = '--no-dump' in argv
    if nodump:
        argv = [a for a in argv if a != '--no-dump']
        outdir = argv[0] if argv else os.path.join(ROOT, 'native', 'web', 'data', 'vincent')
        sel = set(NODUMP_PARTS)
    else:
        info_path = argv[0] if argv else os.path.join(HERE, 'out', 'cerberus_hero', 'cerberus.json')
        outdir = argv[1] if len(argv) > 1 else os.path.join(ROOT, 'native', 'web', 'data', 'vincent')
        sel = set(json.load(open(info_path))['parts'])

    vm = EC.markers(open(EC.VIN_CONT, 'rb').read())
    moff, mbone = vm[EC.MARKER]
    if mbone != 56:
        raise SystemExit(f"marker {EC.MARKER} is on bone {mbone}, expected 56")
    gbuf, GV, _GN, gtris, gm, part, mats = EC.decode_gun(with_materials=True)
    X = GV + moff                                   # T(marker) x gun vertex: j56 space

    names, files = EC.gun_textures()
    H = sum(f.shape[0] for f in files); W = max(f.shape[1] for f in files)
    atlas = np.zeros((H, W, 4), np.uint8); yo = []; y = 0
    for f in files:
        atlas[y:y + f.shape[0], :f.shape[1]] = f; yo.append(y); y += f.shape[0]
    # texture alpha kept (dtex: GS alpha x 255/128): the ATE draws test and blend by it;
    # the opaque draws (ABE 0, ATE 0) ignore it (gun.js)

    # Texture per triangle from the files, in both modes: the dump's per-group vote gave
    # frame0.g1 one texture, but its 3 descriptors use slots 0/2/1 (the dump draws frame0
    # with 3 TEX0s). Draw order = the game's (smd_material.tri_draw_keys: FUN_005b2ef8 parts
    # last-to-first, FUN_005b2550 deferred buckets): the GS dumps draw the gun's alpha
    # triangles (descriptor flag bit 1) between its opaque ones in this order.
    ftex = file_tex_index(mats, names)
    import smd_material as SM
    keys = SM.tri_draw_keys(gbuf, gm, gtris)
    order = [i for _k, i in sorted(keys) if part[gtris[i][6]] in sel]
    flat = []; runs = []
    for i in order:
        t = gtris[i]; ti = ftex[i]; fh, fw = files[ti].shape[:2]
        ate = 1 if mats[i][1] & 2 else 0           # FUN_005b19b8: +4 bit 1 -> ATE GEQUAL 100, AFAIL RGB_ONLY, ABE
        if runs and runs[-1][2] == ate: runs[-1][1] += 1
        else: runs.append([len(flat) // 3, 1, ate])
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
                runs=runs, alpha_ref=ALPHA_REF,
                note='j56-local (game model space, -Y up); colour 1.0 = 0x80; triangles in game draw '
                     'order; runs = [first tri, count, ate]: ate 1 = GS ATE GEQUAL 100 + AFAIL RGB_ONLY + '
                     'ABE (Cs-Cd)*As+Cd, 0 = opaque; atlas alpha = GS alpha x 255/128',
                texture_binding='file, per triangle (smd_material)',
                **(dict(parts_source='GUESS NODUMP_PARTS') if nodump else {}))
    json.dump(meta, open(os.path.join(outdir, 'cerberus_web.json'), 'w'), indent=1)
    print(f"wrote {outdir}/cerberus_web.bin ({len(a) // 3} tris, runs {runs}), .png {W}x{H}, .json; bbox {meta['bbox']}")


if __name__ == '__main__':
    main(sys.argv[1:])
