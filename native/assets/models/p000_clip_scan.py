#!/usr/bin/env python3
"""Rank Vincent's clip frames against a dump-solved pose (fast, approximate).

Runs export_p000_gpu2's dump pose (p000_pose.pose) and stops there, keeping the
vertices whose position came from the dump ('dump'). Each is moved rigidly with its
bone (vertex_bones): X = W_clip[b] . W_bind[b]^-1 . V_bind, then a similarity
(umeyama) aligns the set to the dump; score = median vertex error (world units).
Rigid-per-bone ignores blend weights, so confirm the top hits with
`export_p000_gpu2.py ... --clip NAME --frame F` (exact decode; its
clip.error_vs_dump.placed/aligned are the numbers to quote).

Run: python3 -I p000_clip_scan.py <dump.bin> [--dump-camera CAM.json] [--clip-cont C] [--top 15] [--step 1]
"""
import sys, os, json, contextlib
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import export_p000_gpu2 as E
import p000_pose as PZ
import anim_rot as AR
import anim_eval as A


class _Got(Exception):
    pass


def dump_pose(dump, cam=None):
    """(posed {v: world}, src {v: how}, vb {v: bone}) from the export's pose step."""
    got = {}
    orig = PZ.pose
    def grab(placed, cands, vb, parent, src=None):
        posed, rot, st = orig(placed, cands, vb, parent, src=src)
        got.update(posed=posed, src=dict(src), vb=vb); raise _Got()
    PZ.pose = grab
    argv = [dump, os.path.join(os.environ.get('TMPDIR', '/tmp'), 'p000_clip_scan_out')]
    if cam: argv += ['--dump-camera', cam]
    old = sys.argv; sys.argv = ['x'] + argv
    try:
        with contextlib.redirect_stdout(sys.stderr):
            E.main()
    except _Got:
        pass
    finally:
        sys.argv = old; PZ.pose = orig
    return got['posed'], got['src'], got['vb']


def clips(cont):
    buf = open(cont, 'rb').read(); out = []
    for i in range(A._u16(buf, 0x10)):
        o = A._u32(buf, 0x14 + 4 * i)
        if buf[o + 8:o + 11] == b'mod' and buf[o + 0x10:o + 0x14] == b'MOD\x00':
            out.append((buf[o:o + 8].split(b'\0')[0].decode(), o + 0x10))
    return buf, out


def main(argv):
    opt = dict(cam=None, cont=E.CLIP_CONT, top=15, step=1.0); pos = []; i = 0
    while i < len(argv):
        k = argv[i]
        if k == '--dump-camera': opt['cam'] = argv[i + 1]; i += 2
        elif k == '--clip-cont': opt['cont'] = argv[i + 1]; i += 2
        elif k == '--top': opt['top'] = int(argv[i + 1]); i += 2
        elif k == '--step': opt['step'] = float(argv[i + 1]); i += 2
        else: pos.append(k); i += 1
    if len(pos) != 1: print(__doc__); return 2
    posed, src, vb = dump_pose(pos[0], opt['cam'])
    P = E.P; P.CONT = E.CONT; P.TEXC = E.TEXC
    buf, NB = P.R.load_container(E.CONT); sk = P.load_skeleton(E.CONT, trans_scale=1.0)
    bc = sk['bone_count']; Wb = [np.array(m, float) for m in sk['world_bind']]
    V, _N, _t, _g = P.decode_groups(buf, NB, Wb, bc)
    dv = [v for v in sorted(posed) if src.get(v) == 'dump' and vb.get(v) is not None]
    D = np.array([posed[v] for v in dv]); bones = np.array([vb[v] for v in dv])
    loc = np.array([np.linalg.solve(Wb[b], np.r_[V[v], 1.0]) for v, b in zip(dv, bones)])
    cbuf, cl = clips(opt['cont']); res = []
    for name, CP in cl:
        n = AR.clip_frames(cbuf, CP)
        for f in np.arange(0.0, n + 1e-6, opt['step']):
            W = AR.compose(sk, AR.decode_frame_trs(cbuf, CP, bc, float(f)))
            X = np.einsum('nij,nj->ni', np.array(W)[bones], loc)[:, :3]
            s, R, t = E.umeyama(X, D)
            e = np.linalg.norm(s * X @ R.T + t - D, axis=1)
            res.append((float(np.median(e)), name, float(f), n))
    res.sort()
    best = {}
    for r in res:
        best.setdefault(r[1], r)
    print(json.dumps(dict(dump=os.path.basename(pos[0]), ref_vertices=len(dv),
                          top=[dict(clip=c, frame=f, frames=n, median=round(m, 3)) for m, c, f, n in res[:opt['top']]],
                          best_per_clip=sorted(((round(m, 3), c, f) for m, c, f, n in best.values()))[:20]), indent=1))


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
