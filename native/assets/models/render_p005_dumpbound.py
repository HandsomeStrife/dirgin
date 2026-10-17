#!/usr/bin/env python3
"""Render Vincent p005 using the GS-dump-recovered per-group texture binding.

char_dumpbind.py recovers, per draw-group, the real runtime TBP0 (0x2e50 body,
0x3068 hair, 0x316c cloak) by UV-triple matching against the dump. This renders
each group against its bound atlas (decoded from the dump = the real runtime
texture) with the on-disk UVs. Contrast with render_p005.py, which guessed the
atlas from the rip and reconstructed its own atlases.

Run: python3 -I render_p005_dumpbound.py <dump.bin> <binding.json> <outdir>
"""
import sys, os, json
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, "/srv/games/dirge-port/tools/gsdump/src")
import render_p005 as P
from gsparse import read_header
from gsrender2 import build, TexCache, tex0_fields

BODY, HAIR, CLOAK = 0x2e50, 0x3068, 0x316c   # TBP0s used as atlas keys
DEFAULT = BODY

def load_dump_atlases(dumppath):
    buf = open(dumppath, 'rb').read(); h = read_header(buf)
    g = build(buf, h); tc = TexCache(dumppath)
    first = {}
    for a, b, c, ctx in g.tris:
        if not ctx['tme']:
            continue
        t = tex0_fields(ctx['tex0'])['tbp0']
        if t in (BODY, HAIR, CLOAK) and t not in first:
            first[t] = ctx['tex0']
    atlases = {}
    for tbp, tex0 in first.items():
        im = tc.get(tex0)
        atlases[tbp] = np.asarray(im[:, :, :3], dtype=np.float32)
    return atlases

def main():
    dumppath, bindpath, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(outdir, exist_ok=True)
    binding = {int(k): v for k, v in json.load(open(bindpath)).items()}
    atlases = load_dump_atlases(dumppath)
    for tbp in (BODY, HAIR, CLOAK):
        if tbp not in atlases:
            print(f"WARN missing atlas {tbp:#06x}"); atlases[tbp] = np.zeros((16, 16, 3), np.float32)
    print("atlases:", {hex(k): v.shape for k, v in atlases.items()})

    buf, NB = P.R.load_container(P.CONT)
    sk = P.load_skeleton(P.CONT, trans_scale=1.0); bc = sk['bone_count']
    Wm = [np.array(m, float) for m in sk['world_bind']]
    V, N, tris, gmeta = P.decode_groups(buf, NB, Wm, bc)

    # assign each group an atlas key: dump top-vote, but only switch off BODY
    # when the alt atlas wins a clear majority (UV collisions give body groups
    # a few spurious hair votes).
    gkey = {}
    for gid in range(len(gmeta)):
        b = binding.get(gid, {})
        tbp = b.get('tbp0'); frac = b.get('frac_top', 0.0)
        votes = b.get('votes', {})
        v_body = votes.get(hex(BODY), 0)
        v_alt = max((votes.get(hex(k), 0) for k in (HAIR, CLOAK)), default=0)
        if tbp in (HAIR, CLOAK) and v_alt > v_body:
            gkey[gid] = tbp
        else:
            gkey[gid] = DEFAULT
    from collections import Counter
    print("group->atlas:", {hex(k): v for k, v in Counter(gkey.values()).items()})

    SPAN = 0.35
    bound = []
    for t in tris:
        if P.uv_span(t) > SPAN:
            continue
        bound.append((t[0], t[1], t[2], t[3], t[4], t[5], gkey[t[6]]))

    for ang, tag in [(18, 'front'), (55, '3q'), (200, 'back3q'), (235, 'backside')]:
        P.render_multi(V, N, bound, atlases,
                       os.path.join(outdir, f"vincent_dumpbound_{tag}.png"), ang, 8)

if __name__ == '__main__':
    main()
