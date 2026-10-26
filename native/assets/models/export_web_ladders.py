#!/usr/bin/env python3
"""export_web_ladders.py -- z128 ladders for the web play page (actions/ladder.js).

    python3 -I native/assets/models/export_web_ladders.py [stage.bin] [collision.bin] [out.json]

Defaults: extract/_KEL/noPath/06800001.bin (z128 stage file, "bzd048"),
native/web/data/z128_collision.bin (DCOL v1), native/web/data/z128_ladders.json.

Source (notes/actions-research.md section 3, worker 0057): section 8 of the stage
file = gimmick records, 0xC0 bytes each. +0x00 4x4 float matrix (row vectors;
row 0..2 = local x/y/z axes, row 3 = world origin = trigger floor point), +0x80 local
box min, +0x90 local box max, +0xaa u8 type (1 = ladder bottom trigger, 2 = ladder top
trigger). No pair id was found in the record, so each bottom pairs with the nearest top
(horizontal distance; the pairs are also adjacent records).

Local +z (row 2) of the bottom record points at the ladder, so it is the facing while
climbing. The face plane is taken from the collision triangles with class bit 0x01
(sub-tree 1, mask & 0x01, e.g. 0x10d / 0x109 / 0x105: the probe class FUN_00551e80 casts with 0x18e)
in front of the bottom trigger. When there is none, the game's own fallback is used:
the point local (0, 0, max.z) of the bottom box, normal = -row 2.

World is the game's: -Y up. Output y values are feet heights (bigger = lower).
"""
import json
import math
import os
import struct
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
STAGE = os.path.join(ROOT, 'extract/_KEL/noPath/06800001.bin')
COLL = os.path.join(ROOT, 'native/web/data/z128_collision.bin')
OUT = os.path.join(ROOT, 'native/web/data/z128_ladders.json')


def records(b):
    if not b.startswith(b'bzd048'):
        raise ValueError('not a bzd048 stage file')
    off, count, stride = struct.unpack_from('<III', b, 0x24 + 16 * 8)
    if stride != 0xC0:
        raise ValueError(f'section 8 stride {stride:#x}, expected 0xc0')
    for r in range(count):
        o = off + r * stride
        m = struct.unpack_from('<16f', b, o)
        rows = [m[0:3], m[4:7], m[8:11], m[12:15]]
        yield {
            'rec': r, 'type': b[o + 0xaa], 'pair': struct.unpack_from('<H', b, o + 0xa6)[0],
            'rows': rows,
            'min': struct.unpack_from('<3f', b, o + 0x80),
            'max': struct.unpack_from('<3f', b, o + 0x90),
        }


def to_world(rec, l):
    r = rec['rows']
    return [r[3][k] + l[0] * r[0][k] + l[1] * r[1][k] + l[2] * r[2][k] for k in range(3)]


def box(rec):
    """trigger box: origin + local axes + half sizes, and the world AABB of its corners"""
    mn, mx = rec['min'], rec['max']
    corners = [to_world(rec, (x, y, z)) for x in (mn[0], mx[0]) for y in (mn[1], mx[1]) for z in (mn[2], mx[2])]
    rnd = lambda v: [round(c, 4) for c in v]
    return {
        'rec': rec['rec'], 'origin': rnd(rec['rows'][3]),
        'axes': [rnd(rec['rows'][k]) for k in range(3)],
        'min': rnd(mn), 'max': rnd(mx),
        'aabb': {'min': rnd([min(c[k] for c in corners) for k in range(3)]),
                 'max': rnd([max(c[k] for c in corners) for k in range(3)])},
    }


def class01_tris(path):
    b = open(path, 'rb').read()
    magic, ver, nv, nt = struct.unpack_from('<4sIII', b, 0)
    if magic != b'DCOL' or ver != 1:
        raise ValueError('not a DCOL v1 file')
    o = 0x30
    v = struct.unpack_from(f'<{nv * 3}f', b, o); o += nv * 12
    tri = struct.unpack_from(f'<{nt * 3}I', b, o); o += nt * 12
    o += nt * 4                                    # surface words
    info = struct.unpack_from(f'<{nt}I', b, o)
    out = []
    for t in range(nt):
        inf = info[t]
        if inf & 3 != 1 or not (inf >> 16) & 0x01:
            continue
        out.append((t, [v[3 * tri[3 * t + i]:3 * tri[3 * t + i] + 3] for i in range(3)]))
    return out


def face_for(bottom, top, tris):
    """class-0x01 triangles near the line between the triggers, as one plane"""
    o, f = bottom['rows'][3], bottom['rows'][2]
    y_lo, y_hi = min(o[1], top['rows'][3][1]) - 5, max(o[1], top['rows'][3][1]) + 5
    hits = []
    for t, P in tris:
        cx = sum(p[0] for p in P) / 3; cy = sum(p[1] for p in P) / 3; cz = sum(p[2] for p in P) / 3
        along = (cx - o[0]) * f[0] + (cz - o[2]) * f[2]
        side = abs(-(cx - o[0]) * f[2] + (cz - o[2]) * f[0])
        if -2 <= along <= 12 and side <= 8 and y_lo <= cy <= y_hi:
            hits.append((t, P, along))
    if not hits:
        p = to_world(bottom, (0, 0, bottom['max'][2]))
        n = [-c for c in f]
        return {'source': 'fallback (bottom box local 0,0,max.z)', 'tris': [],
                'point': [round(c, 4) for c in p], 'normal': [round(c, 4) for c in n],
                'dist': round(sum(p[k] * n[k] for k in range(3)), 4), 'y': None}
    pts = [p for _, P, _ in hits for p in P]
    n = [-c for c in f]                            # toward the climber (trigger side)
    d = sum(sum(p[k] * n[k] for k in range(3)) for p in pts) / len(pts)
    ctr = [sum(p[k] for p in pts) / len(pts) for k in range(3)]
    return {'source': 'collision class 0x01', 'tris': sorted(t for t, _, _ in hits),
            'point': [round(c, 4) for c in ctr], 'normal': [round(c, 4) for c in n], 'dist': round(d, 4),
            'y': [round(min(p[1] for p in pts), 4), round(max(p[1] for p in pts), 4)],
            'spread': round(max(abs(sum(p[k] * n[k] for k in range(3)) - d) for p in pts), 4)}


def check_ignored(path):
    rel = os.path.relpath(path, ROOT)
    r = subprocess.run(['git', '-C', ROOT, 'check-ignore', '-q', rel])
    if r.returncode != 0:
        sys.exit(f'refusing to write {rel}: not git-ignored (generated data must stay out of the repo)')


def main(argv):
    stage = argv[1] if len(argv) > 1 else STAGE
    coll = argv[2] if len(argv) > 2 else COLL
    out = argv[3] if len(argv) > 3 else OUT
    b = open(stage, 'rb').read()
    recs = [r for r in records(b) if r['type'] in (1, 2)]
    tris = class01_tris(coll)
    tops = [r for r in recs if r['type'] == 2]
    hd = lambda a, c: math.hypot(a['rows'][3][0] - c['rows'][3][0], a['rows'][3][2] - c['rows'][3][2])
    ladders = []
    for pid, bot in enumerate(r for r in recs if r['type'] == 1):
        top = min(tops, key=lambda t: hd(bot, t))
        if hd(bot, top) > 30:
            print(f"bottom rec {bot['rec']}: nearest top {top['rec']} is {hd(bot, top):.1f} away, skipped", file=sys.stderr)
            continue
        f = bot['rows'][2]
        face = face_for(bot, top, tris)
        ladders.append({
            'id': pid, 'records': {'bottom': bot['rec'], 'top': top['rec']},
            'bottom': box(bot), 'top': box(top),
            'facing': [round(f[0], 4), round(f[1], 4), round(f[2], 4)],
            'heading': round(math.atan2(f[0], f[2]), 6),   # main.js yaw: dir = (sin h, cos h)
            'bottomY': round(bot['rows'][3][1], 4), 'topY': round(top['rows'][3][1], 4),
            'rise': round(bot['rows'][3][1] - top['rows'][3][1], 4),
            'face': face,
        })
    used = {t for l in ladders for t in l['face']['tris']}
    doc = {'source': os.path.relpath(stage, ROOT), 'collision': os.path.relpath(coll, ROOT),
           'note': 'z128 ladders from stage section 8 (types 1/2), -Y up; export_web_ladders.py',
           'ladders': ladders,
           'unmatched_class01_tris': [t for t, _ in tris if t not in used]}
    check_ignored(out)
    with open(out, 'w') as fh:
        json.dump(doc, fh, indent=1)
    for l in ladders:
        print(f"ladder {l['id']}: recs {l['records']['bottom']}/{l['records']['top']}  "
              f"bottom {l['bottom']['origin']}  top {l['top']['origin']}  rise {l['rise']}  "
              f"facing {l['facing']}  face {l['face']['source']} d={l['face']['dist']} tris {l['face']['tris']}")
    print(f"unmatched class-0x01 tris: {doc['unmatched_class01_tris']}")
    print('wrote', os.path.relpath(out, ROOT))


if __name__ == '__main__':
    main(sys.argv)
