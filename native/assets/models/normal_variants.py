#!/usr/bin/env python3
"""Does face-normal awareness beat plain centroid 1-NN? And is there front/back
pairing evidence? Reuses loaders from backface_heuristic.py. numpy-only."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from backface_heuristic import load_all

def batched(q_cen, q_nrm, c_cen, c_nrm, c_fc, gate=None):
    """1-NN by centroid. gate='same' restricts candidates to dot(normal)>=0
    (fallback: all); gate=None plain. Returns pred fc, nn dist."""
    pred = np.empty(len(q_cen), dtype=object); nnd = np.empty(len(q_cen))
    B = 400
    for s in range(0, len(q_cen), B):
        q = q_cen[s:s+B]; qn = q_nrm[s:s+B]
        d = np.linalg.norm(c_cen[None, :, :] - q[:, None, :], axis=2)   # (b, C)
        if gate == 'same':
            dot = qn @ c_nrm.T                                          # (b, C)
            bad = dot < 0
            dmask = np.where(bad, np.inf, d)
            # rows where everything gated out -> fall back to plain
            allbad = ~np.isfinite(dmask).any(1)
            dmask[allbad] = d[allbad]
            d = dmask
        j = d.argmin(1)
        pred[s:s+B] = c_fc[j]; nnd[s:s+B] = d[np.arange(len(q)), j]
    return pred, nnd

def main():
    cen, nrm, fc, chunk = load_all()
    bound = np.array([f is not None for f in fc])
    bidx = np.where(bound)[0]; uidx = np.where(~bound)[0]

    # ---- hold-out: plain vs same-hemisphere-gated 1-NN (5 seeds, 20% held) ----
    print("== normal-aware hold-out (20% held, 5 seeds) ==")
    for gate, name in [(None, "plain centroid 1-NN"), ('same', "same-hemisphere 1-NN")]:
        accs = []
        for sd in (1,2,3,4,5):
            rng = np.random.default_rng(sd)
            held = rng.choice(bidx, size=int(len(bidx)*0.2), replace=False)
            hs = set(held.tolist())
            cand = np.array([i for i in bidx if i not in hs])
            pred, _ = batched(cen[held], nrm[held], cen[cand], nrm[cand], fc[cand], gate=gate)
            accs.append((pred == fc[held]).mean())
        print("  %-24s %.1f%% ± %.1f" % (name, 100*np.mean(accs), 100*np.std(accs)))

    # ---- front/back pairing evidence (diagnostic, numpy) ----
    # fraction of UNBOUND packets that have a BOUND packet within r AND normal
    # anti-parallel (dot < -0.5) -> looks like the back side of a textured wall.
    print("\n== front/back pairing (unbound pkt vs nearest bound, numpy) ==")
    cB, nB, fB = cen[bidx], nrm[bidx], fc[bidx]
    for r, athr in [(30, -0.5), (60, -0.5), (60, -0.8)]:
        paired = 0; B = 400
        for s in range(0, len(uidx), B):
            qc = cen[uidx[s:s+B]]; qn = nrm[uidx[s:s+B]]
            d = np.linalg.norm(cB[None,:,:] - qc[:,None,:], axis=2)
            dot = qn @ nB.T
            near_anti = (d <= r) & (dot < athr)
            paired += near_anti.any(1).sum()
        print("  within %2du AND dot<%.1f : %.1f%% of unbound" % (r, athr, 100*paired/len(uidx)))

    # does normal-aware change the unbound assignment vs plain? (agreement rate)
    pp, _ = batched(cen[uidx], nrm[uidx], cB, nB, fB, gate=None)
    ps, _ = batched(cen[uidx], nrm[uidx], cB, nB, fB, gate='same')
    print("\nunbound assignment: plain vs same-hemisphere agree on %.1f%% of %d packets"
          % (100*np.mean(pp == ps), len(uidx)))

if __name__ == '__main__':
    main()
