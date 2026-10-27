#!/usr/bin/env python3
"""Recover p005 (Vincent) per-draw-group -> texture (TBP0) binding from a GS dump.

The per-group texture binding is RUNTIME EE-RAM state, absent from the on-disk
container (every draw packet's GIFtag carries no TEX0). The GS dump IS the game
submitting those exact packets with the real runtime TEX0, so we recover the
binding by matching triangles' UV-triples:

  dump per-vertex UV (normalised) = (s/q, t/q)   [STQ draws]
  p005 per-vertex UV (normalised) = raw / 4096    [12.4 texel in a <=256 atlas]

Both land in [0,1]; the same model triangle has the same UV-triple on both sides,
so a p005 group's triangles vote for the dump TBP0 they match -> the group's
runtime texture.

Run: python3 -I char_dumpbind.py <dump.bin> [outdir]
"""
import sys, os, json, math, collections
_REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))  # repo root
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _REPO + "/tools/gsdump/src")
import render_p005 as P
from gsparse import read_header
from gsrender2 import build, tex0_fields

QMIN = 1e-6
ROUND = 512          # UV quantisation for signature (1/512 of the atlas)

def tri_sig(uvs):
    """Order-independent signature of a triangle's 3 normalised UVs."""
    pts = tuple(sorted((int(round(u * ROUND)), int(round(v * ROUND))) for u, v in uvs))
    return pts

def dump_draws(dumppath):
    """All textured dump triangles -> (tbp0, normalised-UV-triple)."""
    buf = open(dumppath, 'rb').read(); h = read_header(buf)
    g = build(buf, h)
    sig2tbp = collections.defaultdict(collections.Counter)
    n = 0
    for a, b, c, ctx in g.tris:
        if not ctx['tme']:
            continue
        f = tex0_fields(ctx['tex0']); tbp = f['tbp0']
        uvs = []
        if ctx['fst']:
            tw = 1 << f['tw']; th = 1 << f['th']
            for v in (a, b, c):
                uvs.append((v[6] / 16.0 / tw, v[7] / 16.0 / th))
        else:
            ok = True
            for v in (a, b, c):
                q = v[5]
                if abs(q) < QMIN:
                    ok = False; break
                uvs.append((v[3] / q, v[4] / q))
            if not ok:
                continue
        sig2tbp[tri_sig(uvs)][tbp] += 1
        n += 1
    return sig2tbp, n

def p005_groups():
    buf, NB = P.R.load_container(P.CONT)
    sk = P.load_skeleton(P.CONT, trans_scale=1.0); bc = sk['bone_count']
    Wm = [np.array(m, float) for m in sk['world_bind']]
    V, N, tris, gmeta = P.decode_groups(buf, NB, Wm, bc)
    return tris, gmeta

def main():
    dumppath = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else "."
    os.makedirs(outdir, exist_ok=True)
    sig2tbp, ndump = dump_draws(dumppath)
    print(f"dump textured tris indexed: {ndump}  distinct UV-tri sigs: {len(sig2tbp)}")
    tris, gmeta = p005_groups()
    print(f"p005 groups: {len(gmeta)}  tris: {len(tris)}")

    # per group, vote each triangle to a dump TBP0 via its UV-triple
    group_votes = collections.defaultdict(collections.Counter)
    group_total = collections.Counter()
    matched = 0
    for t in tris:
        gid = t[6]
        group_total[gid] += 1
        uvs = [(t[3][0] / 4096.0, t[3][1] / 4096.0),
               (t[4][0] / 4096.0, t[4][1] / 4096.0),
               (t[5][0] / 4096.0, t[5][1] / 4096.0)]
        sig = tri_sig(uvs)
        hit = sig2tbp.get(sig)
        if hit:
            tbp = hit.most_common(1)[0][0]
            group_votes[gid][tbp] += 1
            matched += 1
    print(f"p005 tris matched to a dump draw: {matched}/{len(tris)} "
          f"({100*matched/max(len(tris),1):.0f}%)")

    binding = {}
    print(f"\n{'grp':>3} {'tris':>5} {'matched':>7} {'TBP0':>7} {'frac':>5}  votes")
    for gid in sorted(group_total):
        tot = group_total[gid]; votes = group_votes[gid]
        if votes:
            tbp, nv = votes.most_common(1)[0]
            frac = nv / tot
            binding[gid] = {"tbp0": tbp, "matched": sum(votes.values()),
                            "tris": tot, "frac_top": frac,
                            "votes": {hex(k): v for k, v in votes.most_common(4)}}
            vs = " ".join(f"{hex(k)}:{v}" for k, v in votes.most_common(4))
            print(f"{gid:>3} {tot:>5} {sum(votes.values()):>7} {tbp:#07x} {frac:>5.2f}  {vs}")
        else:
            binding[gid] = {"tbp0": None, "matched": 0, "tris": tot}
            print(f"{gid:>3} {tot:>5} {0:>7} {'-':>7} {'-':>5}  (no match)")

    with open(os.path.join(outdir, "p005_dumpbinding.json"), "w") as f:
        json.dump(binding, f, indent=1)
    print(f"\nwrote {outdir}/p005_dumpbinding.json")
    # distinct TBP0s bound
    tbps = collections.Counter(b["tbp0"] for b in binding.values() if b.get("tbp0") is not None)
    print("distinct TBP0s bound:", {hex(k): v for k, v in tbps.most_common()})

if __name__ == '__main__':
    main()
