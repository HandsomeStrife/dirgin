#!/usr/bin/env python3
"""
Back-face / never-drawn geometry texturing heuristic for the enhanced free-camera
of the Dirge of Cerberus z143 Mako Reactor room.

PROBLEM. The GS-dump oracle bound 8018/15332 room packets (52.3%) = every
camera-visible wall surface. The other 7314 packets are geometry the retail game
never rasterises from any reachable camera (back-faces, structural, far-LOD), so
there is NO ground-truth texture for them anywhere (proven across files + EE RAM).
For a free camera we must GUESS: inherit each unbound packet's material from the
nearest spatially-adjacent BOUND packet.

METHOD. Every chunk decodes into packets with world-space vertex positions
(probe_space.py confirmed the 13 chunks share ONE world frame). For each packet we
compute an area-weighted centroid and face normal. Heuristic = 1-nearest-neighbour
by centroid distance over the BOUND set; assign that neighbour's FileCode.

VALIDATION (Dan's rule: numerically, never by eye). We can't check the guess on
true back-faces (no oracle), so we HOLD OUT a random fraction of the KNOWN-bound
packets, hide their FileCode, predict it from the remaining bound packets, and
compare to the oracle truth. That measures how well "inherit nearest bound
neighbour" reconstructs a texture we actually know. We sweep a distance cutoff
(accuracy vs coverage), compare global-vs-within-chunk and 1-NN vs k-NN, and then
report how many truly-unbound packets even HAVE a bound neighbour within a sane
distance (i.e. are coverable at all).

RESULT (z143, 5-seed hold-out of 20%): global centroid 1-NN = 75.2% +/- 0.4%
exact-match (vs 26.3% majority baseline), ~98% coverage of the 7314 unbound
packets within 200u. k>1 and normal-gating both make it WORSE (they reach across
material boundaries), so the simplest method wins. Ship as a LABELLED cosmetic
fallback for the free camera only -- source='heuristic' -- never folded into the
oracle-bound 52.3% and never presented as recovered binding.

Outputs: prints a report; writes result.json (metrics) and assignments.json
(per-packet final FileCode incl. heuristic fill) to out/z143_backface/. Reads only
from the project tree.
"""
import sys, os, json, glob
sys.path.insert(0, '/srv/games/dirge-port/native/assets/models')
import numpy as np
from export_bmd_textured import find_bmd_payload, decode_packets, packet_tris

EXT = '/srv/games/dirge-port/extract/_KEL/noPath'
PF  = '/srv/games/dirge-port/native/assets/models/out/z143_pktfc'
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out', 'z143_backface')
os.makedirs(OUT, exist_ok=True)

try:
    from scipy.spatial import cKDTree
    HAVE_KD = True
except Exception:
    HAVE_KD = False


# ---------------------------------------------------------------- data loading
def chunk_ids():
    return [os.path.basename(p)[3:11] for p in sorted(glob.glob(PF + '/pf_*.json'))]

def packet_centroid_normal(pk):
    V = np.asarray(pk['verts'], dtype=float)
    if len(V) == 0:
        return np.zeros(3), np.zeros(3)
    cen = V.mean(0)
    acc = np.zeros(3)
    for (a, b, c) in packet_tris(pk):
        if a < len(V) and b < len(V) and c < len(V):
            acc += np.cross(V[b] - V[a], V[c] - V[a])   # area-weighted (||cross||=2*area)
    n = np.linalg.norm(acc)
    nrm = acc / n if n > 1e-9 else np.zeros(3)
    return cen, nrm

def load_all():
    cen, nrm, fc, chunk = [], [], [], []
    for ci, cid in enumerate(chunk_ids()):
        binp = '%s/%s.bin' % (EXT, cid)
        if not os.path.exists(binp):
            continue
        buf = open(binp, 'rb').read()
        pay = find_bmd_payload(buf)
        pk  = decode_packets(buf, pay) if pay is not None else []
        pf  = json.load(open('%s/pf_%s.json' % (PF, cid)))
        assert len(pk) == len(pf), (cid, len(pk), len(pf))
        for p, f in zip(pk, pf):
            c, nv = packet_centroid_normal(p)
            cen.append(c); nrm.append(nv); fc.append(f); chunk.append(ci)
    return (np.array(cen), np.array(nrm),
            np.array([f if f else None for f in fc], dtype=object),
            np.array(chunk))


# ------------------------------------------------------------ nn predict utils
def nn_predict(q_cen, cand_cen, cand_fc, k=1):
    """Return (pred_fc array, nn_dist array) for queries against candidates."""
    if HAVE_KD:
        tree = cKDTree(cand_cen)
        dist, idx = tree.query(q_cen, k=k)
        if k == 1:
            return cand_fc[idx], dist
        preds = []
        for row in idx:
            vals = cand_fc[row]
            u, cnt = np.unique(vals.astype(str), return_counts=True)
            preds.append(u[cnt.argmax()])
        return np.array(preds, dtype=object), dist[:, 0]
    # numpy fallback, batched
    preds = np.empty(len(q_cen), dtype=object); nnd = np.empty(len(q_cen))
    B = 512
    for s in range(0, len(q_cen), B):
        q = q_cen[s:s+B]
        d = np.linalg.norm(cand_cen[None, :, :] - q[:, None, :], axis=2)
        if k == 1:
            j = d.argmin(1); preds[s:s+B] = cand_fc[j]; nnd[s:s+B] = d[np.arange(len(q)), j]
        else:
            order = np.argsort(d, 1)[:, :k]
            for r in range(len(q)):
                vals = cand_fc[order[r]]
                u, cnt = np.unique(vals.astype(str), return_counts=True)
                preds[s+r] = u[cnt.argmax()]
            nnd[s:s+B] = d[np.arange(len(q)), order[:, 0]]
    return preds, nnd


# ---------------------------------------------------------------- validation
CUTOFFS = [10, 25, 50, 75, 100, 150, 200, 300, 500, 1000, float('inf')]

def holdout_validation(cen, fc, chunk, bound_mask, frac=0.2, seeds=(1,2,3,4,5), k=1,
                       within_chunk=False):
    bidx = np.where(bound_mask)[0]
    alld, allc = [], []            # per-query nn-dist, correct?
    per_seed_acc = []
    for sd in seeds:
        rng = np.random.default_rng(sd)
        held = rng.choice(bidx, size=int(len(bidx) * frac), replace=False)
        held_set = set(held.tolist())
        cand = np.array([i for i in bidx if i not in held_set])
        if within_chunk:
            correct = 0; n = 0
            for ch in np.unique(chunk[held]):
                qh = held[chunk[held] == ch]
                cc = cand[chunk[cand] == ch]
                if len(cc) == 0:            # no bound candidate in this chunk
                    for i in qh: alld.append(np.inf); allc.append(False)
                    n += len(qh); continue
                pred, dist = nn_predict(cen[qh], cen[cc], fc[cc], k=k)
                ok = (pred == fc[qh])
                correct += ok.sum(); n += len(qh)
                alld.extend(dist.tolist()); allc.extend(ok.tolist())
            per_seed_acc.append(correct / n)
        else:
            pred, dist = nn_predict(cen[held], cen[cand], fc[cand], k=k)
            ok = (pred == fc[held])
            per_seed_acc.append(ok.mean())
            alld.extend(dist.tolist()); allc.extend(ok.tolist())
    alld = np.array(alld); allc = np.array(allc, dtype=bool)
    curve = []
    for cut in CUTOFFS:
        m = alld <= cut
        cov = m.mean()
        acc = allc[m].mean() if m.any() else float('nan')
        curve.append((cut, cov, acc))
    return float(np.mean(per_seed_acc)), float(np.std(per_seed_acc)), curve


# -------------------------------------------------------------- true unbound
def unbound_analysis(cen, nrm, fc, chunk, bound_mask):
    bidx = np.where(bound_mask)[0]; uidx = np.where(~bound_mask)[0]
    pred, dist = nn_predict(cen[uidx], cen[bidx], fc[bidx], k=1)
    pct = {p: float(np.percentile(dist, p)) for p in (10, 25, 50, 75, 90, 95)}
    cov = {str(cut): float((dist <= cut).mean()) for cut in CUTOFFS[:-1]}
    # per-chunk coverage within 200u
    per_chunk = {}
    cids = chunk_ids()
    for ch in np.unique(chunk[uidx]):
        d = dist[chunk[uidx] == ch]
        per_chunk[cids[ch]] = (int((chunk[uidx] == ch).sum()), float((d <= 200).mean()), float(np.median(d)))
    # opposite-normal front/back pairing evidence (unvalidatable, diagnostic only):
    # fraction of unbound with a bound packet very close AND near-antiparallel normal
    paired = 0
    if HAVE_KD:
        tree = cKDTree(cen[bidx])
        for j, ui in enumerate(uidx):
            nn = tree.query_ball_point(cen[ui], r=60.0)
            if not nn:
                continue
            if any(np.dot(nrm[ui], nrm[bidx[t]]) < -0.5 for t in nn):
                paired += 1
    frac_paired = paired / len(uidx) if HAVE_KD else None
    return dict(n_unbound=len(uidx), nn_dist_pct=pct, coverage=cov,
                per_chunk=per_chunk, frac_with_antiparallel_within_60=frac_paired,
                pred=pred, dist=dist, uidx=uidx)


# ------------------------------------------------------------------- main
def main():
    print("loading packets (scipy KDTree: %s)..." % HAVE_KD)
    cen, nrm, fc, chunk = load_all()
    bound = np.array([f is not None for f in fc])
    N = len(fc); nb = bound.sum(); nu = N - nb
    print("packets=%d  bound=%d (%.1f%%)  unbound=%d\n" % (N, nb, 100*nb/N, nu))

    # majority-class baseline
    vals, cnts = np.unique(fc[bound].astype(str), return_counts=True)
    maj = vals[cnts.argmax()]; maj_base = cnts.max()/nb
    print("majority-class baseline (assign '%s' to all): %.1f%%\n" % (maj, 100*maj_base))

    print("== HOLD-OUT VALIDATION (hide 20%% of bound, predict from the rest) ==")
    res = {}
    for label, kw in [("global 1-NN", dict(k=1)),
                      ("global 3-NN", dict(k=3)),
                      ("global 5-NN", dict(k=5)),
                      ("within-chunk 1-NN", dict(k=1, within_chunk=True))]:
        acc, sd, curve = holdout_validation(cen, fc, chunk, bound, **kw)
        res[label] = dict(acc=acc, sd=sd, curve=curve)
        print("  %-20s accuracy %.1f%% ± %.1f" % (label, 100*acc, 100*sd))
    print()
    print("  global 1-NN accuracy vs distance cutoff (coverage = frac of held-out within cutoff):")
    print("   %8s %10s %10s" % ("cutoff", "coverage", "accuracy"))
    for cut, cov, acc in res["global 1-NN"]["curve"]:
        cc = "inf" if cut == float('inf') else "%d" % cut
        print("   %8s %9.1f%% %9.1f%%" % (cc, 100*cov, 100*acc))

    print("\n== TRUE-UNBOUND COVERAGE (can the %d never-drawn packets even be filled?) ==" % nu)
    ua = unbound_analysis(cen, nrm, fc, chunk, bound)
    print("  nearest-bound distance percentiles:",
          {k: round(v,1) for k,v in ua['nn_dist_pct'].items()})
    print("  fraction of unbound with a bound neighbour within cutoff:")
    for cut in [25,50,100,200,500]:
        print("     <= %4d u : %.1f%%" % (cut, 100*ua['coverage'][str(cut)]))
    print("  front/back pairing evidence (bound pkt within 60u AND normal anti-parallel): %s" %
          ("%.1f%%" % (100*ua['frac_with_antiparallel_within_60']) if ua['frac_with_antiparallel_within_60'] is not None else "n/a"))
    print("  per-chunk unbound coverage (n_unbound, %%within200u, median_dist):")
    for cid, (n, c, md) in sorted(ua['per_chunk'].items(), key=lambda kv:-kv[1][0]):
        print("     %-10s n=%4d  within200u=%5.1f%%  median=%7.1f" % (cid, n, 100*c, md))

    # -------- write machine-readable results + the actual filled assignment
    out = dict(
        packets=int(N), bound=int(nb), unbound=int(nu),
        majority_baseline=dict(filecode=maj, acc=float(maj_base)),
        validation={k: dict(acc=v['acc'], sd=v['sd'],
                            curve=[[('inf' if c==float('inf') else c), cov, (None if acc!=acc else acc)]
                                   for c,cov,acc in v['curve']])
                    for k,v in res.items()},
        unbound_coverage=ua['coverage'],
        unbound_nn_dist_pct=ua['nn_dist_pct'],
        unbound_per_chunk={k:dict(n=v[0], within200u=v[1], median=v[2]) for k,v in ua['per_chunk'].items()},
        frac_with_antiparallel_within_60=ua['frac_with_antiparallel_within_60'],
    )
    json.dump(out, open(os.path.join(OUT,'result.json'),'w'), indent=1)

    # final per-packet assignment: bound keep oracle fc; unbound get nearest-bound fc,
    # tagged with the nn distance and a confidence flag at the recommended cutoff.
    REC_CUT = 200
    assign = []
    ui = {int(i): j for j, i in enumerate(ua['uidx'])}
    for i in range(N):
        if bound[i]:
            assign.append(dict(chunk=chunk_ids()[chunk[i]], fc=str(fc[i]), source='oracle'))
        else:
            j = ui[i]
            d = float(ua['dist'][j])
            assign.append(dict(chunk=chunk_ids()[chunk[i]], fc=str(ua['pred'][j]),
                               source='heuristic', nn_dist=round(d,1),
                               confident=bool(d <= REC_CUT)))
    json.dump(assign, open(os.path.join(OUT,'assignments.json'),'w'))
    print("\nwrote result.json and assignments.json to %s" % OUT)

if __name__ == '__main__':
    main()
