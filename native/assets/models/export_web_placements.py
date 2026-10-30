#!/usr/bin/env python3
"""Stage-load enemy placements for play.html (native/web/enemies.js), read from the z128
stage file 06800001.bin at build time. Writes <out>/placements.json (gitignored game data).

Decoded (notes/enemy-placement.md):
  - section table at +0x24 (16 B: offset, count, stride); section 0 = parameter sets (0x20c B,
    +0x20 model name), section 1 = actor placements (0x74 B).
  - FUN_00315050 copies a placement into the actor status: +0xc pos -> status +0x30,
    +0x1c -> status +0x40, +0x2c -> status +0x50 (the record's own [3..5], [7..9], [11..13]).
  - FUN_0055a168 builds the actor matrix (FUN_0054b770 -> actor +0x60) from them:
    row0 = (status +0x50) x (status +0x40), row1 = status +0x50, row2 = status +0x40, row3 = pos.
    So +0x1c is the facing (row 2, the page's heading atan2(x, z)) and +0x2c the up row (0,-1,0).
    The save state's live bodies (C+0x00) carry exactly this matrix.
  - Stage load: FUN_00324e70 -> FUN_00319190: a status is loaded only if its placement flags
    (+0x40) have bit 3 (FUN_00315738(s, 3)) AND FUN_00376938 finds its tag (+0x44) among the
    event actors (DAT_0046f850 + 0x1800 + k*0x200, name at +0xa5); then FUN_003159b8 sets
    status +0x60 bit 0x40 and FUN_005453b0 -> FUN_00558f68 activates it (+0x144).
Not decoded:
  - which event file fills the event-actor table. GUESS stand-in: EVENT_ACTORS below, the four
    tags that table holds in the sea-of-flames save state (EE RAM).
"""
import json, os, struct, sys

SEC0_STRIDE, SEC1_STRIDE = 0x20c, 0x74
# GUESS: event actor tags (DAT_0046f850+0x1800 table, read from the save state; which event
# file fills it is not decoded; see notes/enemy-placement.md, notes/fidelity-audit.md)
EVENT_ACTORS = ("p_Vin", "e_Sol_11", "e_Sol_12", "e_Sol_13")


def sections(b):
    return [struct.unpack_from('<III', b, 0x24 + 16 * n) for n in range(2)]


def cstr(b):
    return b.split(b'\0')[0].decode('latin-1')


def records(b):
    (o0, c0, s0), (o1, c1, s1) = sections(b)
    if s0 != SEC0_STRIDE or s1 != SEC1_STRIDE:
        raise ValueError(f'section strides {s0:#x} / {s1:#x}, expected {SEC0_STRIDE:#x} / {SEC1_STRIDE:#x}')
    out = []
    for r in range(c1):
        o = o1 + r * s1
        ps = struct.unpack_from('<I', b, o)[0]
        if ps >= c0:
            raise ValueError(f'record {r}: parameter set {ps} >= {c0}')
        out.append(dict(
            rec=r, param_set=ps, model=cstr(b[o0 + ps * s0 + 0x20:o0 + ps * s0 + 0x24]),
            group=struct.unpack_from('<H', b, o + 4)[0], ai_mode=struct.unpack_from('<I', b, o + 8)[0],
            pos=list(struct.unpack_from('<3f', b, o + 0xc)), face=list(struct.unpack_from('<3f', b, o + 0x1c)),
            up=list(struct.unpack_from('<3f', b, o + 0x2c)), flags=struct.unpack_from('<I', b, o + 0x40)[0],
            tag=cstr(b[o + 0x44:o + 0x4c])))
    return out


def matrix(r):
    """FUN_0055a168: rows (up x face, up, face, pos)."""
    u, f = r['up'], r['face']
    r0 = [u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]]
    return [r0, list(u), list(f), list(r['pos'])]


def main(argv):
    if len(argv) != 2:
        sys.exit('usage: export_web_placements.py 06800001.bin OUT_DIR')
    b = open(argv[0], 'rb').read()
    R = records(b)
    load = [r for r in R if r['flags'] & 8]                  # FUN_00315738(s, 3)
    drawn = [r for r in load if r['tag'] in EVENT_ACTORS and r['model'] == 'e027']
    out = dict(
        source=os.path.basename(argv[0]) + ' sections 0/1 (export_web_placements.py)',
        rule='FUN_00319190: flags +0x40 bit 3 and tag in the event actors (FUN_00376938); model e027 only (this page)',
        event_actors=dict(tags=list(EVENT_ACTORS), guess='table DAT_0046f850+0x1800 as in the save state; filling event not decoded'),
        matrix='FUN_0055a168: rows (up x face, up, face, pos); heading = atan2(face.x, face.z)',
        load_flag_records=[dict(rec=r['rec'], model=r['model'], tag=r['tag']) for r in load],
        placements=[dict(r, matrix=matrix(r), glb='e027.glb') for r in drawn],
    )
    os.makedirs(argv[1], exist_ok=True)
    p = os.path.join(argv[1], 'placements.json')
    json.dump(out, open(p, 'w'), indent=1)
    print(f'{len(R)} placements, {len(load)} with load bit 3, drawn (event actor, e027): '
          + ', '.join(f"{r['rec']} {r['tag']}" for r in drawn))
    print(f'wrote {p}')


if __name__ == '__main__':
    main(sys.argv[1:])
