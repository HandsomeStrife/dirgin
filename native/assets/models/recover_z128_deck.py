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

  python3 -I recover_z128_deck.py --in out/z128_pktfc_union --out out/z128_pktfc_v3 \
      [--check-uvs <hero dump.bin>]

DECK UVs ARE THE GAME'S (--check-uvs, 2026-10-24, review 37). Checked, not changed: every
triangle bound to the deck texture (pf 0680ff87 or a BIND_OVERRIDE to it) is matched to the
hero dump's TBP 13228 draws (last frame, main framebuffer), unprojected with the hero camera
(w = 1/q, as export_z128_sky.py):
  exact  - a dump triangle with the same 3 corners (any order, max error < 1 unit and
           < 0.25 x our shortest edge); every such candidate must agree on the UVs.
  planar - else, dump triangles in our triangle's plane (corners within 1 unit: dump
           depth puts the floor ~0.5 below ours; normals within ~11 deg) that overlap it
           (either centroid inside the other); each one's affine position->UV map gives
           UVs at our corners; all must agree within 0.02, and the largest one is used.
Result: 507 matched (81 exact, 426 planar; 94% of the visible floor, 99% of the walkway
edge), BMD-vs-dump UV difference median 0.014, max 0.077 (depth noise), i.e. the BMD UVs
already are the game's. Rendering with the dump UVs instead made the floor and edge grade
slightly WORSE (floor gradient corr 0.44 -> 0.43, edge 0.84 -> 0.58), so nothing is
written. The "gradient corr ~0" of earlier grading was misregistration of the PCSX2
screenshot (it shows ~425.5 of the 432 dump columns): register it first (fit: x scale
1.02, y 0.99, shift +9,-3 px at 640x480). The check warns if a matched triangle's UVs
differ by more than 0.1 (would mean wrong BMD UVs).

Then rebuild: python3 -I export_z128_room.py --pf out/z128_pktfc_v3 -o out/z128_room_v2/room
Trusted repo data; bounds-checked raw BMD read; run -I.
"""
import os, sys, json, glob, argparse
import numpy as np
import itertools

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


DECK_TBP = 13228                     # hero dump TBP of 0680ff87 (corr 1.000)


def dump_deck_tris(dump):
    """Hero-dump deck draws as world triangles: (N,3,3) positions, (N,3,2) UVs."""
    from export_z128_sky import last_frame_tris, gs_to_world, MAIN_FBP, MAIN_FBW
    P, U = [], []
    for a, b, c, ctx in last_frame_tris(dump):
        fr = ctx['frame']
        if not ctx['tme'] or ctx['fst'] or (ctx['tex0'] & 0x3fff) != DECK_TBP:
            continue
        if (fr & 0x1ff, (fr >> 16) & 0x3f) != (MAIN_FBP, MAIN_FBW):
            continue
        if any(v[5] <= 0 for v in (a, b, c)):
            continue
        P.append([gs_to_world(v[0], v[1], v[5], ctx['xyoffset']) for v in (a, b, c)])
        U.append([(v[3] / v[5], v[4] / v[5]) for v in (a, b, c)])
    return np.array(P, float).reshape(-1, 3, 3), np.array(U, float).reshape(-1, 3, 2)


def _uv_agree(cands, tol):
    """All candidate UV triples equal up to one common integer shift (texture repeat)?"""
    ref = cands[0]
    for c in cands[1:]:
        d = c - ref; d -= np.round(d[0])
        if np.abs(d).max() > tol:
            return False
    return True


def _bary(T, X):
    """Plane coordinates (a, b) of points X in triangle T: X ~ T0 + a(T1-T0) + b(T2-T0)."""
    A = np.stack([T[1] - T[0], T[2] - T[0]], 1)
    return np.linalg.lstsq(A, (X - T[0]).T, rcond=None)[0].T


def match_uvs(P, DP, DU, DN, DA, DC):
    """UVs for our triangle P from the dump, or (None, None). See the docstring."""
    edge = min(np.linalg.norm(P[i] - P[(i + 1) % 3]) for i in range(3))
    n = np.cross(P[1] - P[0], P[2] - P[0]); area = np.linalg.norm(n) / 2
    if area < 1e-6:
        return None, None
    n /= 2 * area
    c = P.mean(0); r = max(np.linalg.norm(P - c, axis=1))
    near = np.nonzero(np.linalg.norm(DC - c, axis=1) < r + 1.0)[0]
    tol = min(1.0, 0.25 * edge)
    exact = []
    for j in near:
        for pm in itertools.permutations(range(3)):
            if np.linalg.norm(DP[j][list(pm)] - P, axis=1).max() < tol:
                exact.append(DU[j][list(pm)]); break
    if exact:
        return (exact[0], 'exact') if _uv_agree(exact, 0.01) else (None, None)
    cands = []
    for j in near:
        if DA[j] < 0.5 or abs(np.dot(DN[j], n)) < 0.98:
            continue
        if np.abs((DP[j] - c) @ n).max() > 1.0:
            continue
        ab = _bary(P, DP[j].mean(0)[None])[0]; ba = _bary(DP[j], c[None])[0]
        if not ((ab >= 0).all() and ab.sum() <= 1 or (ba >= 0).all() and ba.sum() <= 1):
            continue
        q = _bary(DP[j], P)
        cands.append((DA[j], DU[j][0] + q[:, :1] * (DU[j][1] - DU[j][0])
                      + q[:, 1:] * (DU[j][2] - DU[j][0])))
    if not cands:
        return None, None
    cands.sort(key=lambda t: -t[0])
    uvs = [u for _, u in cands]
    return (uvs[0], 'planar') if _uv_agree(uvs, 0.02) else (None, None)


def check_uvs(dump, outdir, cids):
    """Compare the deck triangles' BMD UVs with the hero dump's (see the docstring)."""
    from export_z128_room import BIND_OVERRIDE
    DP, DU = dump_deck_tris(dump)
    if len(DP) == 0:
        sys.exit('no TBP %d draws in the dump' % DECK_TBP)
    DN = np.cross(DP[:, 1] - DP[:, 0], DP[:, 2] - DP[:, 0]); DA = np.linalg.norm(DN, axis=1) / 2
    DN /= np.maximum(2 * DA, 1e-12)[:, None]
    DC = DP.mean(1)
    cnt = {'exact': 0, 'planar': 0, 'none': 0}; dev = []; bad = []
    for cid in cids:
        buf = open(os.path.join(EXTRACT, cid + '.bin'), 'rb').read()
        pk = decode_packets(buf, find_bmd_payload(buf))
        pf = json.load(open(os.path.join(outdir, 'pf_%s.json' % cid)))
        for i, p in enumerate(pk):
            ov = BIND_OVERRIDE.get((cid, i))
            fc = ov[0] if ov else (pf[i] if i < len(pf) else None)
            if fc != DECK_FC:
                continue
            V = np.array(p['verts'], float); UV = np.array(p['uv'], float)
            for t, tri in enumerate(packet_tris(p)):
                if max(tri) >= len(V):
                    continue
                uv, how = match_uvs(V[list(tri)], DP, DU, DN, DA, DC)
                if uv is None:
                    cnt['none'] += 1; continue
                cnt[how] += 1
                d = uv - UV[list(tri)]; d -= np.round(d[0]); dev.append(np.abs(d).max())
                if dev[-1] > 0.1:
                    bad.append('%s/%d tri %d (%.3f)' % (cid, i, t, dev[-1]))
    dev = np.array(dev) if dev else np.zeros(1)
    print('deck UV check vs dump (%d TBP %d tris): matched exact %d + planar %d, no dump '
          'evidence %d; |BMD - dump UV| median %.3f max %.3f' % (len(DP), DECK_TBP,
          cnt['exact'], cnt['planar'], cnt['none'], np.median(dev), dev.max()))
    if bad:
        print('warning: %d deck triangles have BMD UVs > 0.1 from the dump\'s: %s'
              % (len(bad), ', '.join(bad[:10])), file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--in', dest='inp', required=True, help='input pf dir (e.g. out/z128_pktfc_union)')
    ap.add_argument('--out', required=True, help='output pf dir (e.g. out/z128_pktfc_v3)')
    ap.add_argument('--check-uvs', metavar='DUMP', help='hero GS dump (163235): check the deck '
                    'UVs against it (prints agreement; writes nothing)')
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
    if a.check_uvs:
        check_uvs(a.check_uvs, a.out, cids)

if __name__ == '__main__':
    main()
