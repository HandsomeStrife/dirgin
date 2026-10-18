#!/usr/bin/env python3
"""recover_z128_deck.py -- spatial recovery of the z128 "sea-of-flames" walkway
deck, the single most prominent surface the UV-triple oracle leaves grey.

WHY A SEPARATE PASS: the room's per-packet texture binding (out/z128_pktfc_union)
is recovered by UV-triple identity -- a model packet's (u,v) triangle must coincide
with a GS-dump draw's (s/q,t/q) triangle. That validates to 99.6% but reaches only
~48% of surfaces: a surface whose model UVs do not coincide with any of the 8 dumps'
draws stays NULL and renders flat grey. The walkway deck Vincent stands on is such a
surface -- only 2 of its ~32 packets UV-match a dump draw -- yet the deck IS drawn
in the hero frame (163235): the largest near-horizontal floor draw in that dump,
0x033ac, carries TEX0->TBP0 whose decoded texture correlates 1.00 to on-disk
FileCode 0680ff87 (brown plank/board walkway). So we have an independent GROUND
TRUTH for the deck's texture even though UV identity fails, and we bind the deck's
near-horizontal packets spatially (coplanar walkway in the hero-camera corridor)
to 0680ff87.

This is a SPATIAL bind, not a UV-validated one; it is applied only to the deck,
whose texture is independently ground-truthed. Other dump-unseen grey surfaces
(pitched roof facets, etc.) are deliberately LEFT grey: 3/381 of the central roof
triangle's packets UV-match any dump and its material is ambiguous (slate vs wood),
so guessing would risk replacing an honest grey with a wrong texture.

  python3 -I recover_z128_deck.py --in out/z128_pktfc_union --out out/z128_pktfc_v3

Then rebuild: python3 -I export_z128_room.py --pf out/z128_pktfc_v3 -o out/z128_room_v2/room
Trusted repo data; bounds-checked raw BMD read; run -I.
"""
import os, sys, json, glob, argparse
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_bmd_textured import find_bmd_payload, decode_packets, EXTRACT, packet_tris

DECK_FC = '0680ff87'                 # ground truth: hero-dump draw 0x033ac, corr 1.00
# walkway-deck packet selection (hero-camera foreground corridor, near-horizontal):
X0, X1 = -1500, -560
Z0, Z1 = -1250, -420
Y0, Y1 = -135, -78                   # the deck plane band (eye Y=-116; deck below it)
NY_MIN = 0.80                        # near-horizontal face normal


def packet_normal(V, tris):
    ns = []
    for (i, j, k) in tris:
        if max(i, j, k) < len(V):
            n = np.cross(V[j] - V[i], V[k] - V[i]); L = np.linalg.norm(n)
            if L > 1e-6:
                ns.append(n / L)
    if not ns:
        return None
    m = np.mean(ns, 0); L = np.linalg.norm(m)
    return m / L if L > 1e-6 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--in', dest='inp', required=True, help='input pf dir (e.g. out/z128_pktfc_union)')
    ap.add_argument('--out', required=True, help='output pf dir (e.g. out/z128_pktfc_v3)')
    a = ap.parse_args()
    cids = [os.path.basename(p)[3:11]
            for p in sorted(glob.glob(os.path.join(a.inp, 'pf_*.json')))]
    os.makedirs(a.out, exist_ok=True)

    nbound = 0
    for cid in cids:
        buf = open(os.path.join(EXTRACT, cid + '.bin'), 'rb').read()
        pay = find_bmd_payload(buf); pk = decode_packets(buf, pay)
        pf = json.load(open(os.path.join(a.inp, 'pf_%s.json' % cid)))
        out = list(pf)
        for i, p in enumerate(pk):
            if i < len(pf) and pf[i]:       # never overwrite a UV-validated bind
                continue
            V = np.array(p['verts'], float); c = V.mean(0)
            n = packet_normal(V, list(packet_tris(p)))
            if n is None or abs(n[1]) < NY_MIN:
                continue
            if X0 <= c[0] <= X1 and Z0 <= c[2] <= Z1 and Y0 <= c[1] <= Y1:
                out[i] = DECK_FC; nbound += 1
        json.dump(out, open(os.path.join(a.out, 'pf_%s.json' % cid), 'w'))
    print('deck packets spatially bound to %s: %d -> %s' % (DECK_FC, nbound, a.out))


if __name__ == '__main__':
    main()
