#!/usr/bin/env python3
"""Vincent's cape (p000 smd 'p000_L02' @0x19ed0 of 07703201): the game's cloth, reimplemented.

What the game does (decoded from the EE code, raw MIPS + COP2 checked by hand):

  The cape block is NOT unskinned. Its smd header has a second, weighted skin path
  that the batch decoder never read:
      +0x0c s16  skin record count (28)      +0x0e s16  vertex count N (82)
      +0x10 s16  cloth group count (1)       +0x18 u32  skin records (payload-relative)
      +0x24 u32  cloth group pointer table   +0x2c f32  meshScale (1.73296)
  Skin record (FUN_005b06d0): s16 bone, u8 n, pad to 8 bytes, then n x 16 bytes
      { s16 x,y,z, s16 vertex, s8 nx,ny,nz, pad, f32 weight }.
      target[v] = M . sum_w  W[bone] . (xyz * meshScale/4096, 1)      (bone-LOCAL xyz)
  Every one of the 82 vertices is weighted (sum 1.0) to bones 28-79: the hips/thighs
  and the cape's own bone chains (38-42, 43-46, 47-50, 62-65, 71-74, 75-79), which
  the clips animate like any other bone. M = the character matrix (param 3).

  Cloth group (FUN_005b0d30 drives it once per frame):
      +0x00 f32 gravity  0.2   (added to velocity.y; world +y is down)
      +0x04 f32 blend    0.1   (minimum pull toward the skinned target)
      +0x08 f32 damping  0.1   (velocity multiplier)
      +0x12 s16 constraint count (336)   +0x14 s16 vertex count (82)
      +0x20 constraints, 8 bytes: u8 a, u8 b (indices into the vertex list), pad,
            f32 rest length.  Then the vertex list: u16, bit 15 = pinned.
  Per frame, if (DAT_0047e1e0 < 2 || k == 1.0):
      1. integrate (FUN_005b0b38), per listed vertex:
           pinned:  pos = target
           free:    v.y += gravity (only when not paused and timescale != 0)
                    old = pos;  pos += v;  v = (pos - old) * damping
      2. 4 passes of FUN_005b09f8, constraints in file order:
           d = p[a] - p[b]; L = |d|;  if L > rest:
               c = d * (L - rest) / L * 0.8
               free a: p[a] -= c;   free b: p[b] += c       (stretch-only)
      3. blend (FUN_005b0c50), f = max(k, blend):
           free and f != 1:  pos += (target - pos) * f
           else:             pos = target
  Velocity is taken BEFORE the constraints, so they never feed back into it.

  k = DAT_005b3b50, set per character by FUN_005b1128:
      k = (1 - c[0x438] / c[0x434])^2  if cloth is globally on and c[0x434] != 0
      k = 1.0                          otherwise (cape drawn exactly at its target)
  FUN_005a95f0 (clip start): clip payload byte +0x19 == 2 ->
      c[0x434] = c[0x438] = 2 * u16 clip+0x1a;   == 0 -> both 0;  else unchanged.
  FUN_005aa5b8 decrements c[0x438] once per tick (when not paused). FUN_005a6f40
  re-arms it (c[0x438] = c[0x434]) when the facing turns (dot < 0.992, > ~7 deg).
  So the cape only simulates in a settle window after a clip starts or the character
  turns; the pull toward the target rises from 0.1 to 1 over that window, and after
  it the cape IS the skinned cape.
  Vincent (0770000b, = 0770320b for these): h_wk 2x30, h_sq_m 2x30, h_rn 2x14, h_jp0 2x3, h_jp4 2x50;
  h_swtch (and 54 others) 0 -> no cloth; h_st (and 2 others) flag 1 -> unchanged.

Space: the game runs it in M-space (character matrix). Only a y-axis rotation,
a translation and a ~0.96-1.0 scale separate that from model space, and the sim is
invariant to the first two, so this module simulates in model space (root motion
included, which is what gives the cape inertia).

CLI:
  p000_cloth.py validate <dump.bin> <export_out_dir>   (export made with --clip h_swtch --frame 58)
  p000_cloth.py run <clip> <first> <last> <out.npz> [--prev-clip NAME --prev-frame F]
"""
import sys, os, struct, json, collections
_REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))  # repo root
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import render_skinned as R
import skd_skeleton as SK
import anim_rot as AR

CONT = _REPO + "/extract/_KEL/noPath/07703201.bin"     # the model z128 loads (render_p000_bound.py)
CLIP_CONT = _REPO + "/extract/_KEL/noPath/0770320b.bin"  # handgun clips z128 loads (export_web_vincent CLIP_SETS)
CAPE_BLOCK = 0x19ed0
CONSTRAINT_ITERS = 4          # FUN_005b0d30: iVar4 = 3 .. 0
CONSTRAINT_GAIN = 0.8         # FUN_005b09f8: 0x3f4ccccd


def _s16(b, o): return struct.unpack_from('<h', b, o)[0]
def _u16(b, o): return struct.unpack_from('<H', b, o)[0]
def _u32(b, o): return struct.unpack_from('<I', b, o)[0]
def _f32(b, o): return struct.unpack_from('<f', b, o)[0]


class Cape:
    """Everything the game reads for the cape, straight from 07703201.bin."""

    def __init__(self, path=CONT, block=CAPE_BLOCK):
        b = open(path, 'rb').read(); self.buf = b
        P = block + 0x10
        if _u32(b, P) != R.SM2_MAGIC:
            raise ValueError("cape block 0x%x: bad smd magic" % block)
        self.N = _s16(b, P + 0x0e)
        self.mesh_scale = _f32(b, P + 0x2c)
        # ---- weighted skin records (FUN_005b06d0)
        sk = []; o = P + _u32(b, P + 0x18)
        for _r in range(_s16(b, P + 0x0c)):
            bone = _s16(b, o); n = b[o + 2]
            for k in range(n):
                e = o + 8 + 16 * k
                x, y, z, v = struct.unpack_from('<4h', b, e)
                nx, ny, nz = struct.unpack_from('<3b', b, e + 8)
                sk.append((v, bone, _f32(b, e + 12), np.array([x, y, z], float) * self.mesh_scale / 4096.0,
                           np.array([nx, ny, nz], float)))
            o += 8 + 16 * n
        self.skin = sk
        ws = np.zeros(self.N)
        for v, _b, w, _p, _n in sk:
            ws[v] += w
        self.weight_sum = ws
        # ---- cloth groups
        self.groups = []
        tab = P + _u32(b, P + 0x24)
        for gi in range(_s16(b, P + 0x10)):
            G = P + _u32(b, tab + 4 * gi)
            nc, nv = _s16(b, G + 0x12), _s16(b, G + 0x14)
            lst = struct.unpack_from('<%dH' % nv, b, G + 0x20 + nc * 8)
            cons = [(b[G + 0x20 + 8 * k], b[G + 0x21 + 8 * k], _f32(b, G + 0x24 + 8 * k)) for k in range(nc)]
            self.groups.append(dict(
                gravity=_f32(b, G), blend=_f32(b, G + 4), damping=_f32(b, G + 8),
                verts=np.array([x & 0x7fff for x in lst]), pinned=np.array([bool(x & 0x8000) for x in lst]),
                ca=np.array([lst[a] & 0x7fff for a, _b, _r in cons]),
                cb=np.array([lst[c] & 0x7fff for _a, c, _r in cons]),
                pa=np.array([bool(lst[a] & 0x8000) for a, _b, _r in cons]),
                pb=np.array([bool(lst[c] & 0x8000) for _a, c, _r in cons]),
                rest=np.array([r for _a, _b, r in cons])))
        # ---- draw topology. The S-8 index stream addresses VU slots; FUN_005b1260
        # fills them from the cloth buffer through the draw group's slot list
        # (+0x10 count, +0x14 list of {u8 slot, u8 0, u16 cloth vertex}).
        NB = len(b); G = P + R._u32(b, P + R._u32(b, P + 0x20))
        L = P + _u32(b, G + 0x14)
        self.slot = {b[L + 4 * i]: _u16(b, L + 4 * i + 2) for i in range(_u32(b, G + 0x10))}
        descs = R.enum_descs(b, NB, P, G); tris = []
        for di, (_d, vp) in enumerate(descs):
            st = P + vp; en = (P + descs[di + 1][1]) if di + 1 < len(descs) else NB
            for sub in R.parse_drawpackets(b, NB, st, en):
                idx = sub.get('idx'); uv = sub.get('uv'); col = sub.get('col')
                if not idx or uv is None:
                    continue
                for j in R.sub_tris(idx, sub.get('prim')):
                    if max(j) >= len(uv):
                        continue
                    tris.append((tuple(self.slot[idx[k]] for k in j), tuple((uv[k][0] / 4096.0, uv[k][1] / 4096.0) for k in j),
                                 tuple(R._corner_col(col, k) for k in j)))
        self.tris = tris

    def targets(self, W, normals=False):
        """Skinned target positions (N,3) in the bone matrices' space."""
        T = np.zeros((self.N, 3)); Nn = np.zeros((self.N, 3))
        for v, bone, w, p, n in self.skin:
            M = W[bone]
            T[v] += w * (M[:3, :3] @ p + M[:3, 3])
            Nn[v] += w * (M[:3, :3] @ n)
        return (T, Nn) if normals else T


class Cloth:
    """The game's per-frame cloth step (FUN_005b0d30 and callees), one group."""

    def __init__(self, cape, gravity_on=True):
        self.cape = cape; self.g = cape.groups[0]; self.gravity_on = gravity_on
        self.pos = None; self.vel = np.zeros((cape.N, 3))

    def reset(self, T):
        self.pos = T.copy(); self.vel[:] = 0.0

    def step(self, T, k):
        """T: this frame's skinned targets. k: DAT_005b3b50 (1.0 = snap to target)."""
        g = self.g; pos = self.pos; vel = self.vel
        if pos is None:
            self.reset(T); return self.pos.copy()
        # 1. integrate
        for v, pin in zip(g['verts'], g['pinned']):
            if pin:
                pos[v] = T[v]
            else:
                dv = vel[v].copy()
                if self.gravity_on:
                    dv[1] += g['gravity']
                old = pos[v].copy(); pos[v] = old + dv
                vel[v] = (pos[v] - old) * g['damping']
        # 2. stretch-only distance constraints, sequential (Gauss-Seidel) as in the game
        for _it in range(CONSTRAINT_ITERS):
            for a, b_, pa, pb, rest in zip(g['ca'], g['cb'], g['pa'], g['pb'], g['rest']):
                d = pos[a] - pos[b_]; L = np.sqrt(d @ d)
                if rest < L:
                    c = d * ((1.0 / L) * ((L - rest) * CONSTRAINT_GAIN))
                    if not pa: pos[a] = pos[a] - c
                    if not pb: pos[b_] = pos[b_] + c
        # 3. pull toward the target
        f = max(k, g['blend'])
        for v, pin in zip(g['verts'], g['pinned']):
            if not pin and f != 1.0:
                pos[v] = pos[v] + (T[v] - pos[v]) * f
            else:
                pos[v] = T[v]
        return pos.copy()


def clip_window(cbuf, CP):
    """FUN_005a95f0: (flag, ticks). flag 2 -> settle window of ticks; 0 -> no cloth."""
    flag = cbuf[CP + 0x19]
    return flag, (2 * _u16(cbuf, CP + 0x1a) if flag == 2 else 0)


def k_of(total, remaining, cloth_on=True):
    """FUN_005b1128."""
    if cloth_on and total:
        return (1.0 - remaining / total) ** 2
    return 1.0


def bone_mats(sk, cbuf, CP, frame, in_place=False):
    trs = AR.decode_frame_trs(cbuf, CP, sk['bone_count'], frame)
    return AR.compose(sk, trs, root_motion=not in_place)


def run_clip(cape, sk, clip, frames, cont=CLIP_CONT, in_place=False, prev=None, gravity_on=True,
             force_window=None, loop=True, rearm_on_loop=True):
    """Simulate `clip` over `frames` (absolute clip-frame times, one tick each).
    The clip starts at frames[0]: its window is armed then (FUN_005a95f0).
    loop: times past the clip's length wrap (a looping walk). Root motion keeps
    accumulating across wraps (the character keeps moving), and each restart runs
    FUN_005a95f0 again, re-arming the window (rearm_on_loop).
    prev = (clip, frame): settle into that pose first (k=1 -> cape at target).
    force_window = ticks: pretend the clip has a window (diagnostic only).
    Returns positions (F,N,3) and targets (F,N,3), both in the model space of each
    frame's LOCAL clip time (what the exporter poses the body with), k per frame."""
    cbuf, CP = AR.find_clip(cont, clip)
    flag, total = clip_window(cbuf, CP)
    if force_window is not None:
        total = force_window
    nfr = AR.clip_frames(cbuf, CP)
    # root displacement per cycle (bone 1 translation, end minus start)
    stride = np.zeros(3)
    if loop and not in_place:
        stride = bone_mats(sk, cbuf, CP, float(nfr), in_place)[1][:3, 3] - \
                 bone_mats(sk, cbuf, CP, 0.0, in_place)[1][:3, 3]
    cl = Cloth(cape, gravity_on=gravity_on)
    if prev is not None:
        pb, PP = AR.find_clip(cont, prev[0])
        cl.reset(cape.targets(bone_mats(sk, pb, PP, prev[1], in_place)))
    remaining = total; out = []; tg = []; ks = []; cyc = None
    for fr in frames:
        c, local = (int(fr // nfr), fr % nfr) if loop else (0, fr)
        if cyc is not None and c != cyc and rearm_on_loop:
            remaining = total
        cyc = c
        off = c * stride
        T = cape.targets(bone_mats(sk, cbuf, CP, local, in_place)) + off
        k = k_of(total, remaining)
        if cl.pos is None:
            cl.reset(T)
        out.append(cl.step(T, k) - off); tg.append(T - off); ks.append(k)
        if remaining > 0:
            remaining -= 1
    return np.array(out), np.array(tg), np.array(ks), dict(flag=flag, window=total, clip_frames=nfr,
                                                           stride=stride.round(3).tolist())


# ---------------------------------------------------------------- validation

def dump_cape_by_uv(dumppath, quant=256):
    """Last dump frame's cape (tbp 0x2e68) corners: {uv key: world position}."""
    sys.path.insert(0, _REPO + "/tools/gsdump/src")
    import p000_cape as PC
    from gsparse import read_header
    from gsrender2 import build, tex0_fields
    buf = open(dumppath, 'rb').read(); g = build(buf, read_header(buf))
    cand = [(a, b, c, ctx) for a, b, c, ctx in g.tris
            if ctx['tme'] and not ctx['fst'] and tex0_fields(ctx['tex0'])['tbp0'] == PC.CAPE_TBP]
    last = max(ctx['vframe'] for *_x, ctx in cand)
    pos = collections.defaultdict(list)
    for a, b, c, ctx in cand:
        if ctx['vframe'] != last:
            continue
        for v in (a, b, c):
            if abs(v[5]) < PC.QMIN:
                continue
            uv = (v[3] / v[5], v[4] / v[5])
            pos[(int(round(uv[0] * quant)), int(round(uv[1] * quant)))].append(
                PC.gs_to_world(v[0], v[1], v[5], ctx['xyoffset']))
    return {k: np.mean(v, 0) for k, v in pos.items()}, last


def placement_fwd(outdir):
    """model -> world for an export (render args + the body-bbox pivot the renderer uses)."""
    pl = json.load(open(os.path.join(outdir, 'placement.json')))['placement']
    hdr = open(os.path.join(outdir, 'vincent_p000.mesh'), 'rb').read(36)
    mn = np.array(struct.unpack_from('<3f', hdr, 12)); mx = np.array(struct.unpack_from('<3f', hdr, 24))
    piv = np.array([(mn[0] + mx[0]) / 2, mx[1], (mn[2] + mx[2]) / 2])
    y = np.radians(pl['yaw']); cy, sy = np.cos(y), np.sin(y)
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    return lambda X: pl['s'] * (np.asarray(X) - piv) @ Ry.T + np.array([pl['x'], pl['y'], pl['z']]), pl


def vertex_dump_positions(cape, dpos, quant=256):
    """Cloth vertex -> dump world position, via the draw packets' UVs (exact key)."""
    acc = collections.defaultdict(list)
    for vs, uvs, _c in cape.tris:
        for v, uv in zip(vs, uvs):
            k = (int(round(uv[0] * quant)), int(round(uv[1] * quant)))
            if k in dpos:
                acc[v].append(dpos[k])
    return {v: np.mean(p, 0) for v, p in acc.items()}


def stats(e):
    return dict(median=round(float(np.median(e)), 3), rms=round(float(np.sqrt((e ** 2).mean())), 3),
                p90=round(float(np.percentile(e, 90)), 3), max=round(float(e.max()), 3), n=int(len(e)))


def validate(dumppath, outdir, clip='h_swtch', frame=58.0):
    cape = Cape(); sk = SK.load_skeleton(CONT, trans_scale=1.0)
    fwd, pl = placement_fwd(outdir)
    dpos, vf = dump_cape_by_uv(dumppath)
    D = vertex_dump_positions(cape, dpos)
    vs = sorted(D); Dw = np.array([D[v] for v in vs])
    cbuf, CP = AR.find_clip(CLIP_CONT, clip)
    res = dict(dump_vframe=vf, vertices_matched=len(vs), clip=clip, frame=frame,
               clip_window=clip_window(cbuf, CP))
    def err(Tm):
        return np.linalg.norm(fwd(Tm[vs]) - Dw, axis=1)
    # (a) what the game does for h_swtch: k = 1 -> cape at its skinned target
    T = cape.targets(bone_mats(sk, cbuf, CP, frame))
    res['skinned_target'] = stats(err(T))
    pins = set(cape.groups[0]['verts'][cape.groups[0]['pinned']])
    res['skinned_target_pinned'] = stats(err(T)[[v in pins for v in vs]])
    res['skinned_target_free'] = stats(err(T)[[v not in pins for v in vs]])
    # (b) bind pose skin (no clip) for scale
    Wb = [np.array(m, float) for m in sk['world_bind']]
    res['bind_target'] = stats(err(cape.targets(Wb)))
    # (c) counterfactual: same frames WITH a full settle window (sim from f0, window 60)
    frames = [float(f) for f in range(0, int(frame) + 1)]
    pos, _t, ks, _i = run_clip(cape, sk, clip, frames, force_window=60)
    res['if_simulated_window60_from_f0'] = stats(err(pos[-1]))
    pos2, _t, _k, _i = run_clip(cape, sk, clip, frames, force_window=10 ** 6)
    res['if_simulated_forever'] = stats(err(pos2[-1]))
    for f in (59.0,):
        res['skinned_target_f%d' % f] = stats(err(cape.targets(bone_mats(sk, cbuf, CP, f))))
    return res


def main(argv):
    if len(argv) >= 3 and argv[0] == 'validate':
        r = validate(argv[1], argv[2], *(argv[3:4] or ['h_swtch']), *(map(float, argv[4:5]) or [58.0]))
        print(json.dumps(r, indent=1, default=str))
    elif len(argv) >= 5 and argv[0] == 'run':
        cape = Cape(); sk = SK.load_skeleton(CONT, trans_scale=1.0)
        frames = [float(f) for f in range(int(argv[2]), int(argv[3]) + 1)]
        pos, tg, ks, info = run_clip(cape, sk, argv[1], frames)
        np.savez(argv[4], pos=pos, targets=tg, k=ks, frames=frames)
        dev = np.linalg.norm(pos - tg, axis=2)
        print(info, 'max |pos-target| per frame:', np.round(dev.max(1), 2).tolist())
    else:
        print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
