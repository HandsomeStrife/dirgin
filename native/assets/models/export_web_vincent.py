#!/usr/bin/env python3
"""Export sea-of-flames Vincent (p000) as a SKINNED, ANIMATED glTF binary (.glb) for the
three.js web viewer, plus a JSON sidecar. Output is game-derived: write it only into a
gitignored directory (default native/web/data/vincent/, checked with git check-ignore).

  <dump.bin> may be --no-dump: the reference run is export_p000_gpu2.py --no-dump (game files
  only; same glb/json bytes, see that script). This is how tools/byo/build_web.sh runs it.
  export:  python3 -I export_web_vincent.py export <dump.bin> [--out DIR] [--mutate-parents]
                                                   [--clips all|NAME,NAME,...] [--sets c,h]
  verify:  python3 -I export_web_vincent.py verify <dump.bin> [--out DIR] [--glb FILE]
                                                   [--clip h_wk --frames 0,13,27]
           A clip that isn't in vincent.json is looked up in the vincent_<pack>.json sidecars;
           it is then skinned with vincent.glb's mesh and the pack glb's tracks.
  catalogue: python3 -I export_web_vincent.py catalogue [--out DIR]   -> DIR/motions.json
  cloth:   python3 -I export_web_vincent.py cloth [--out DIR]       -> DIR/cloth.json
           The cape cloth data for native/web/cloth.js, from the game files only (no dump).
           Also run at the end of every export without --pack.

  --clips: default h_st,h_wk,h_rn (byte-identical to the original 3-clip export). 'all' =
  every clip of the clip sets in --sets (default c,h = 07700000 common + 0770000b handgun;
  m = 0770000c, r = 0770000d, ev = 0770000f + 07700011). Names are unique across sets.
  With --clips the JSON clip entries also carry loop / in_place_axes / container / root_total.
  --pack NAME: write only vincent_NAME.glb (joint nodes + the clips, no mesh) and
  vincent_NAME.json; vincent.glb/json are left alone. Load it next to vincent.glb.
  The pack JSON records bone_count and parent_sha1 (sha1 of the parent list as compact JSON,
  e.g. [-1,0,1,...]) so a loader can refuse a pack built for another skeleton.
  A pack also writes vincent_NAME_local.bin (the clips' local TRS, vincent_local.bin's format;
  index in the pack JSON's local_pose) for player.js's game-order pose build.
  --pack with --mutate-parents is refused (it would write mutant clips into the real pack).

How it is built
- Textures, UVs, vertex colours, per-triangle alpha flag and the game's draw-order runs come
  from one export_p000_gpu2.py run (--clip h_st --frame 0 --in-place) into OUT/_ref/ (its
  vincent_p000.mesh corner order is kept: body triangles in binding.json order, cape last,
  so its -charruns triangle ranges index our triangles directly).
- Skeleton: render_p005.load_skeleton (93 bones). Body skin: each pool slot's influences
  (bone, bone-local position, weight), read with build_group_pool's own batch walk. The
  game's rule (FUN_005b1190 + VU0 0x120): a batch's cntA vertices start the slot at
  w x M.local, its cntB vertices add w x M.local; w = the batch's S-32 stream. 994 of
  2037 p000 vertices have 2-5 bones; top 4 kept (16 had a 5th at 0.05). Bind positions =
  sum w x world_bind[bone] x local (= each bone's own, to 0.002).
- Cape: p000_cloth.Cape's weighted skin records (bones 28-79 incl. its own chains), i.e.
  the exporter's --clip cape mode (skinned target, no cloth sim). Up to 5 influences per
  vertex; three.js uses 4, so the smallest weight is dropped and the rest renormalised.
- Clips (anim_rot: decode_frame_trs + compose, in place = bone 1 x/z zeroed): per integer
  frame, each joint's WORLD (model-space) matrix R*diag(S) + T as glTF TRS on a FLAT joint
  list (every joint a direct child of the root node). The game's scale composition
  (S_w = S_parent * s, non-uniform, h_wk up to 1.41) is not a standard node hierarchy, so
  local TRS would need shear; world TRS is exact. The real parents are in the JSON.
- Root motion (bone 1 x/z) is removed from the glTF clips and given per frame in the JSON.
  Per axis: bone 1 y is removed too when the clip's bone 1 translation track moves in y
  (jumps h_jp1/2, ladder c_ld_*, knockbacks); never for h_st/h_wk/h_rn (their y track is 0).
- Loops: a clip is a loop when it has at least 8 frames and the pose jump from its last frame
  back to frame 0 is at most max(2 x its median frame step, 0.5) units (in place; is_loop).
  Loops get a closing key at nf = frame 0;
  one-shots end on their last key (nf - 1), so they hold their last pose.
"""
import sys, os, json, struct, subprocess, io
_REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))  # repo root
import numpy as np
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, _REPO + "/tools/gsdump/src")
import render_p005 as P
import anim_rot as AR
from render_p000_bound import CONT, TEXC

REPO = os.path.abspath(os.path.join(HERE, "../../.."))
DEFAULT_OUT = os.path.join(REPO, "native/web/data/vincent")
CLIP_CONT = _REPO + "/extract/_KEL/noPath/0770000b.bin"   # h_* (handgun) clips
CLIPS = ["h_st", "h_wk", "h_rn"]
NOPATH = _REPO + "/extract/_KEL/noPath/"
CLIP_SETS = {'c': ['07700000'], 'h': ['0770000b'], 'm': ['0770000c'], 'r': ['0770000d'],
             'ev': ['0770000f', '07700011']}            # p000's motion containers (0a = copy of 00)
DEFAULT_SETS = ['c', 'h']
GUN_CLIP_CONT = NOPATH + "07770580.bin"               # Cerberus reload clips (4-bone gun rig)
GUN_CONT = NOPATH + "07770581.bin"
FPS = 60.0                    # ASSUMED: 1 clip frame per game tick at 60 Hz (unproven)
REF_CLIP = ("h_st", 0)        # the reference run that supplies textures/UVs/colours/runs
GLOW_GUN = {
    "gun": "Cerberus: weapon slot N -> marker N+0xf (gun 15) = joint 56 (socket under hand 55, "
           "offset 0). Parent the gun mesh (export_cerberus.py, Vincent model space) to joint "
           "node 56 with the inverse of joint 56's bind matrix (bind56 in the JSON).",
    "glow": "0x2ee8 glow pass (p000_glow.py): an extra additive draw over body triangles, so it "
            "would be a second skinned mesh on this same skin (same JOINTS_0/WEIGHTS_0 as the "
            "body triangles it copies). Not exported: its UVs/colours are dump-frame only.",
}


def check_ignored(path):
    r = subprocess.run(["git", "-C", REPO, "check-ignore", "-q", os.path.join(path, "probe.glb")])
    if r.returncode != 0:
        sys.exit(f"REFUSING: {path} is not gitignored (game-derived output)")


# ---------------------------------------------------------------- decode
def load():
    P.CONT = CONT; P.TEXC = TEXC
    buf, NB = P.R.load_container(CONT)
    sk = P.load_skeleton(CONT, trans_scale=1.0)
    return buf, NB, sk


def slot_skin(buf, NB, bc, Pb, G, sc):
    """build_group_pool's batch walk (render_skinned.py), keeping each slot's influences
    [(bone, bone-local position, weight)] instead of the posed sum. Same skip rules.
    FUN_005b1190: a batch's first cntA vertices start their slot afresh, the next cntB
    add to it (VU0 0x120: slot += w * M . local); w = the batch's S-32 weight."""
    R = P.R
    piv = Pb + R._u32(buf, G + 0x0c)
    cnt = R._i32(buf, piv); cr = piv + R._i32(buf, piv + 8); raw = piv + R._i32(buf, piv + 0xc)
    infl = [[] for _ in range(256)]
    for b in range(cnt):
        o = cr + b * 8
        if o + 8 > NB: break
        bn = R._s16(buf, o); cA = buf[o + 2]; num = buf[o + 2] + buf[o + 3]
        qwc = R._s16(buf, o + 4); stride = R._s16(buf, o + 6)
        if num == 0: raw += max(stride, 0); continue
        if not (0 <= bn < bc) or qwc <= 0 or raw + 2 > NB: raw += max(stride, 0); continue
        pos, nrm = R._parse_batch(buf, NB, raw, qwc, num); wt = R._batch_weights(buf, NB, raw, qwc)
        raw += stride
        if pos is None or nrm is None or len(pos) < num or len(nrm) < num: continue
        if wt is None or len(wt) < num: wt = None          # build_group_pool's GUESS fallback
        slot = (nrm[:num, 3].astype(int) + 0x80) & 0xff
        for k in range(num):
            if wt is None: infl[slot[k]] = [(bn, pos[k] * sc, 1.0)]
            elif k < cA: infl[slot[k]] = [(bn, pos[k] * sc, float(wt[k]))]
            else: infl[slot[k]].append((bn, pos[k] * sc, float(wt[k])))
    return infl


def body_skin(buf, NB, sk):
    """(bind positions, bind normals, joints[n,4], weights[n,4], tris, stats) over
    decode_groups' vertices. Up to 4 influences per vertex (three.js): a 5th, smallest
    one is dropped and the rest renormalised (p000: 16 vertices, dropped weight 0.05)."""
    bc = sk['bone_count']; Wb = [np.array(m, float) for m in sk['world_bind']]
    V, N, tris, gmeta = P.decode_groups(buf, NB, Wb, bc)
    J4 = np.full((len(V), 4), -1); W4 = np.zeros((len(V), 4)); st = dict(multi=0, trimmed=0, dropped_w=0.0)
    for name, base, psize, _seen, _pool in gmeta:
        Pb = int(name.split('@')[1].split('.')[0], 16); gi = int(name.rsplit('.g', 1)[1])
        G = Pb + P.R._u32(buf, Pb + P.R._u32(buf, Pb + 0x20) + 4 * gi)
        infl = slot_skin(buf, NB, bc, Pb, G, P.R._f32(buf, Pb + 0x2c) / 2048.0)
        for s in range(psize):
            l = infl[s]
            if not l: continue
            # the decoder's bind position must be the weighted sum of bone x local
            p = sum(w * (Wb[b][:3, :3] @ loc + Wb[b][:3, 3]) for b, loc, w in l)
            assert np.abs(p - V[base + s]).max() < 1e-4, (name, s)
            l = sorted(((w, b) for b, _loc, w in l), reverse=True)
            st['multi'] += len(l) > 1
            if len(l) > 4: st['trimmed'] += 1; st['dropped_w'] = max(st['dropped_w'], l[4][0])
            # GUESS: three.js skins with 4 joints; a 5th bone (max weight 0.05, 16 vertices) is
            # dropped and the rest renormalised (notes/fidelity-audit.md)
            l = l[:4]; tot = sum(w for w, _ in l)
            for k, (w, b) in enumerate(l): J4[base + s, k] = b; W4[base + s, k] = w / tot
    J4[J4 < 0] = 0
    Nn = np.asarray(N, float); ln = np.linalg.norm(Nn, axis=1, keepdims=True); ln[ln < 1e-9] = 1
    return np.asarray(V, float), Nn / ln, J4, W4, tris, st


_CLIPS = {}
def find_clip(cont, clip):
    if (cont, clip) not in _CLIPS: _CLIPS[(cont, clip)] = AR.find_clip(cont, clip)
    return _CLIPS[(cont, clip)]


def list_clips(cont):
    """[(name, frames)] of the MOD motion clips in a container, in file order."""
    b = open(cont, 'rb').read(); u16 = lambda o: struct.unpack_from('<H', b, o)[0]
    u32 = lambda o: struct.unpack_from('<I', b, o)[0]; out = []
    for i in range(u16(0x10)):
        o = u32(0x14 + 4 * i)
        if b[o + 8:o + 11] == b'mod' and b[o + 0x10:o + 0x14] == b'MOD\x00':
            out.append((b[o:o + 8].split(b'\0')[0].decode(), u32(o + 0x10 + 0x0c)))
    return out


def resolve_clips(spec, sets):
    """--clips value -> [(name, container path)]."""
    if spec is None: return [(c, CLIP_CONT) for c in CLIPS]
    allc = {}
    for st in list(CLIP_SETS):
        for cid in CLIP_SETS[st]:
            for n, _f in list_clips(NOPATH + cid + '.bin'): allc.setdefault(n, (NOPATH + cid + '.bin', st))
    if spec == 'all':
        return [(n, c) for n, (c, st) in allc.items() if st in sets]
    out = []
    for n in spec.split(','):
        if n not in allc: sys.exit(f"unknown clip {n}")
        out.append((n, allc[n][0]))
    return out


def compose_trs(sk, trs, parent=None, root_motion=False, zero_y=False):
    if zero_y and not root_motion and trs[1][0] is not None:
        trs = list(trs); t, s_, q = trs[1]; trs[1] = ((t[0], 0.0, t[2]), s_, q)
    s = sk if parent is None else dict(sk, parent=parent)
    return AR.compose(s, trs, root_motion=root_motion)


def clip_mats(sk, clip, frame, parent=None, root_motion=False, cont=CLIP_CONT, zero_y=False):
    cbuf, CP = find_clip(cont, clip)
    trs = AR.decode_frame_trs(cbuf, CP, sk['bone_count'], frame)
    return compose_trs(sk, trs, parent, root_motion, zero_y)


def y_moves(sk, cont, clip):
    """True if bone 1's translation track moves in y (vertical root motion)."""
    cbuf, CP = find_clip(cont, clip)
    for f in range(AR.clip_frames(cbuf, CP)):
        t = AR.decode_frame_trs(cbuf, CP, sk['bone_count'], f)[1][0]
        if t is not None and abs(t[1]) > 1e-9: return True
    return False


def pose_jump(Wa, Wb_):
    return max(float(np.abs(a - b).max()) for a, b in zip(Wa, Wb_))


def is_loop(wrap, step_med, nf):
    """Loop if the last->first pose jump is an ordinary frame step (median over the clip).
    Under 8 frames is never a loop (take-offs, hit flinches)."""
    return nf >= 8 and wrap <= max(2 * step_med, 0.5)


def mat_to_trs(M):
    A = M[:3, :3]; S = np.linalg.norm(A, axis=0); Rm = A / S
    if np.linalg.det(Rm) < 0: S[0] = -S[0]; Rm[:, 0] = -Rm[:, 0]
    return M[:3, 3].copy(), quat(Rm), S


def quat(Rm):
    """(x, y, z, w) of a rotation matrix (Shepperd)."""
    t = np.trace(Rm)
    if t > 0:
        s = np.sqrt(t + 1) * 2; w = s / 4
        x = (Rm[2, 1] - Rm[1, 2]) / s; y = (Rm[0, 2] - Rm[2, 0]) / s; z = (Rm[1, 0] - Rm[0, 1]) / s
    else:
        i = int(np.argmax(np.diag(Rm))); j, k = (i + 1) % 3, (i + 2) % 3
        s = np.sqrt(1 + Rm[i, i] - Rm[j, j] - Rm[k, k]) * 2
        q = np.zeros(3); q[i] = s / 4; q[j] = (Rm[j, i] + Rm[i, j]) / s; q[k] = (Rm[k, i] + Rm[i, k]) / s
        w = (Rm[k, j] - Rm[j, k]) / s; x, y, z = q
    q = np.array([x, y, z, w]); return q / np.linalg.norm(q)


def qmat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def run_ref(dump, outdir, clip, frame, cont=None):
    # dump None (--no-dump): export_p000_gpu2 --no-dump; the mesh/atlas/runs this script reads
    # are byte-identical to the dump run (file binding), only binding/placement.json differ
    cmd = [sys.executable, "-I", os.path.join(HERE, "export_p000_gpu2.py")] + \
          ([dump] if dump else ["--no-dump"]) + [outdir,
           "--clip", clip, "--frame", str(frame), "--in-place"] + (["--clip-cont", cont] if cont else [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0: sys.exit(r.stdout[-2000:] + r.stderr[-2000:])


def read_mesh(path):
    b = open(path, 'rb').read(); _m, _v, n = struct.unpack_from('<4sII', b, 0)
    return np.frombuffer(b, '<f4', n * 12, 36).reshape(n, 12)


# ---------------------------------------------------------------- glb writer
class GLB:
    def __init__(self):
        self.bin = bytearray(); self.j = dict(asset=dict(version="2.0", generator="export_web_vincent.py"),
                                              bufferViews=[], accessors=[])

    def view(self, data, target=None):
        while len(self.bin) % 4: self.bin += b'\0'
        v = dict(buffer=0, byteOffset=len(self.bin), byteLength=len(data))
        if target: v['target'] = target
        self.bin += data; self.j['bufferViews'].append(v); return len(self.j['bufferViews']) - 1

    def acc(self, arr, ctype, typ, target=None, minmax=False):
        arr = np.ascontiguousarray(arr)
        a = dict(bufferView=self.view(arr.tobytes(), target), componentType=ctype,
                 count=int(arr.shape[0]), type=typ)
        if minmax:
            a['min'] = [float(x) for x in np.atleast_1d(arr.reshape(arr.shape[0], -1).min(0))]
            a['max'] = [float(x) for x in np.atleast_1d(arr.reshape(arr.shape[0], -1).max(0))]
        self.j['accessors'].append(a); return len(self.j['accessors']) - 1

    def write(self, path):
        while len(self.bin) % 4: self.bin += b'\0'
        self.j['buffers'] = [dict(byteLength=len(self.bin))]
        js = json.dumps(self.j, separators=(',', ':')).encode()
        js += b' ' * (-len(js) % 4)
        with open(path, 'wb') as f:
            f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(self.bin)))
            f.write(struct.pack('<II', len(js), 0x4E4F534A)); f.write(js)
            f.write(struct.pack('<II', len(self.bin), 0x004E4942)); f.write(bytes(self.bin))


def read_glb(path):
    b = open(path, 'rb').read(); jl = struct.unpack_from('<I', b, 12)[0]
    j = json.loads(b[20:20 + jl]); bin_ = b[20 + jl + 8:]
    dt = {5121: np.uint8, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
    nc = dict(SCALAR=1, VEC2=2, VEC3=3, VEC4=4, MAT4=16)
    def acc(i):
        a = j['accessors'][i]; v = j['bufferViews'][a['bufferView']]
        x = np.frombuffer(bin_, dt[a['componentType']], a['count'] * nc[a['type']], v['byteOffset'])
        return x.reshape(a['count'], nc[a['type']]) if nc[a['type']] > 1 else x
    return j, acc


# ---------------------------------------------------------------- export
def add_anims(g, clips, bc, ext):
    anims = []
    for c, d in clips.items():
        tm = (np.arange(len(d['frames'])) / FPS).astype('<f4')
        ti = g.acc(tm, 5126, 'SCALAR', minmax=True)
        ch = []; smp = []
        for b in range(bc):
            for path, k, typ in (('translation', 0, 'VEC3'), ('rotation', 1, 'VEC4'), ('scale', 2, 'VEC3')):
                arr = np.array([fr[b][k] for fr in d['frames']], '<f4')
                smp.append(dict(input=ti, output=g.acc(arr, 5126, typ), interpolation='LINEAR'))
                ch.append(dict(sampler=len(smp) - 1, target=dict(node=b + 1, path=path)))
        anims.append(dict(name=c, channels=ch, samplers=smp,
                          extras=dict(frames=d['nf'], fps_assumed=FPS, in_place=True,
                                      **(dict(loop=d['loop'], in_place_axes='xyz' if d['zero_y'] else 'xz')
                                         if ext else {}))))
    return anims


def local_pose(sk, parent, clips, out, name='vincent_local.bin'):
    """vincent_local.bin (a pack: vincent_<pack>_local.bin, same format): each clip's LOCAL (parent-relative) TRS per bone at frames 0..nf,
    for native/web/player.js's translation of the game's pose build (FUN_005a9db0 ->
    FUN_005afa30 / FUN_005afa58: per-bone local blend, then compose). Frame nf is the
    decoder's own extrapolation past the last key (FUN_005af6c0; loops reset at nf, so the
    game samples [nf-1, nf)). In place as the glb tracks: bone 1 x/z zeroed (y too when the
    clip moves in y). Record: t xyz, s xyz, q xyzw (anim_rot / three.js order), float32."""
    bc = sk['bone_count']; recs = []; index = {}
    for c, d in clips.items():
        cbuf, CP = find_clip(d['cont'], c); nf = d['nf']
        index[c] = dict(offset=len(recs), frames=nf + 1)     # in records (one per bone)
        for f in range(nf + 1):
            for b, (t, s, q) in enumerate(AR.decode_frame_trs(cbuf, CP, bc, f)):
                t = list(t if t is not None else sk['local_T'][b]); s = s if s is not None else (1.0, 1.0, 1.0)
                if b == 1:
                    t[0] = t[2] = 0.0
                    if d['zero_y']: t[1] = 0.0
                recs.append(list(t) + list(s) + list(q))
    np.array(recs, '<f4').tofile(os.path.join(out, name))
    return dict(file=name, record='t3 s3 q4 (xyzw) float32, parent-relative',
                bones=bc, parent=[int(p) for p in parent], clips=index,
                note='FUN_005a9db0: out layer at weight 1 (FUN_005afa30), head layer lerped in at MP+0x408 '
                     '(FUN_005afa58: t/s lerp, q nlerp with hemisphere flip), then compose (anim_rot.compose)')


def parent_sha1(parent):
    """sha1 of the parent list as compact JSON ([-1,0,1,...]): ties a clip pack to its skeleton."""
    import hashlib
    return hashlib.sha1(json.dumps([int(x) for x in parent], separators=(',', ':')).encode()).hexdigest()


def export(dump, out, mutate_parents=False, clip_spec=None, sets=DEFAULT_SETS, pack=None):
    if pack and mutate_parents:
        sys.exit("--pack with --mutate-parents refused: it would write mutant clips into "
                 f"vincent_{pack}.glb/json. Run the mutation test without --pack.")
    check_ignored(out); os.makedirs(out, exist_ok=True)
    ref = os.path.join(out, "_ref"); os.makedirs(ref, exist_ok=True)
    print("reference export (textures/UVs/colours/runs) ->", ref)
    run_ref(dump, ref, *REF_CLIP)
    mesh = read_mesh(os.path.join(ref, "vincent_p000.mesh"))
    bindj = json.load(open(os.path.join(ref, "binding.json")))
    plj = json.load(open(os.path.join(ref, "placement.json")))
    buf, NB, sk = load(); bc = sk['bone_count']
    parent = list(sk['parent'])
    if mutate_parents:          # MUTATION TEST: skip one level (parent -> grandparent)
        parent = [parent[parent[i]] if parent[i] > 0 else parent[i] for i in range(bc)]
    Wb = [np.array(m, float) for m in sk['world_bind']]
    V, N, J4b, W4b, tris, bst = body_skin(buf, NB, sk)
    print("body skin:", bst)
    assert len(tris) == len(bindj) and all([int(k) for k in t[:3]] == r['v'] for t, r in zip(tris, bindj))
    nbody = len(tris) * 3

    # ---- body corners (mesh order): the game's weighted skin (FUN_005b1190), top 4
    vi = np.array([r['v'] for r in bindj]).reshape(-1)
    pos = [V[vi]]; nrm = [N[vi]]
    joints = [J4b[vi]]; weights = [W4b[vi]]
    assert (W4b[vi].sum(1) > 0.999).all()

    # ---- cape corners: weighted skin, top-4 influences
    import p000_cloth as CL
    import export_p000_gpu2 as E
    cp = CL.Cape()
    infl = [[] for _ in range(cp.N)]
    for v, b, w, _p, _n in cp.skin: infl[v].append((w, b))
    J4 = np.zeros((cp.N, 4), int); W4 = np.zeros((cp.N, 4)); trimmed = 0
    for v, l in enumerate(infl):
        l = sorted(l, reverse=True); trimmed += len(l) > 4; l = l[:4]
        s = sum(w for w, _ in l)
        for k, (w, b) in enumerate(l): J4[v, k] = b; W4[v, k] = w / s
    Tb = cp.targets(Wb)
    ctri = [Tb[list(v)] for v, _u, _c in cp.tris]
    cn = E.cape_normals(ctri)
    cvi = np.array([v for vs, _u, _c in cp.tris for v in vs])
    pos.append(Tb[cvi]); nrm.append(np.array([n for ns in cn for n in ns]))
    joints.append(J4[cvi]); weights.append(W4[cvi])
    pos = np.concatenate(pos); nrm = np.concatenate(nrm)
    joints = np.concatenate(joints); weights = np.concatenate(weights)
    assert len(pos) == len(mesh), (len(pos), len(mesh))
    uv = mesh[:, 6:8]; col = mesh[:, 8:12] * 255.0 / 128.0     # GS 128 = neutral = 1.0

    # ---- clips: per frame world TRS per joint, in place; root motion separately
    clips = {}; ext = clip_spec is not None
    clip_list = resolve_clips(clip_spec, sets)
    print(f"{len(clip_list)} clips")
    for c, cont in clip_list:
        cbuf, CP = find_clip(cont, c); nf = AR.clip_frames(cbuf, CP)
        zy = y_moves(sk, cont, c)
        frames = []; root = []; Ws = []
        # keys sit on frames 0..nf-1 (the last track key is at nf-1; frame nf would be
        # extrapolated). For a loop the wrap nf-1 -> 0 is about one ordinary frame step, so
        # a closing key at nf repeating frame 0 makes the glTF loop seamless.
        for f in range(nf):
            W = clip_mats(sk, c, f, parent, cont=cont, zero_y=zy)
            frames.append([mat_to_trs(M) for M in W]); Ws.append(W)
            Wr = clip_mats(sk, c, f, parent, root_motion=True, cont=cont)
            root.append((Wr[1][:3, 3] - W[1][:3, 3]).tolist())
        wrap, step = (pose_jump(Ws[-1], Ws[0]), pose_jump(Ws[0], Ws[1])) if nf > 1 else (0.0, 0.0)
        step_med = float(np.median([pose_jump(Ws[f], Ws[f + 1]) for f in range(nf - 1)])) if nf > 1 else 0.0
        loop = is_loop(wrap, step_med, nf)
        if loop: frames.append(list(frames[0]))
        for b in range(bc):               # quaternion hemisphere continuity for slerp
            for f in range(1, len(frames)):
                if np.dot(frames[f][b][1], frames[f - 1][b][1]) < 0:
                    t, q, s = frames[f][b]; frames[f][b] = (t, -q, s)
        clips[c] = dict(frames=frames, root=root, nf=nf, loop_err=wrap, step=step, loop=loop,
                        zero_y=zy, cont=cont, step_med=step_med)

    # ---- the glb
    g = GLB(); j = g.j
    A = lambda *a, **k: g.acc(*a, **k)
    attrs = dict(POSITION=A(pos.astype('<f4'), 5126, 'VEC3', 34962, True),
                 NORMAL=A(nrm.astype('<f4'), 5126, 'VEC3', 34962),
                 TEXCOORD_0=A(uv.astype('<f4'), 5126, 'VEC2', 34962),
                 COLOR_0=A(col.astype('<f4'), 5126, 'VEC4', 34962),
                 JOINTS_0=A(joints.astype('<u1'), 5121, 'VEC4', 34962),
                 WEIGHTS_0=A(weights.astype('<f4'), 5126, 'VEC4', 34962))
    runs = plj['alpha_tris']['runs']            # [start tri, count, 'alpha'|'opaque'], draw order
    prims = []
    for order, (s0, cnt, kind) in enumerate(runs):
        idx = np.arange(s0 * 3, (s0 + cnt) * 3, dtype='<u2')
        assert s0 + cnt <= len(tris) or s0 >= len(tris), 'a draw run straddles body and cape'
        # lit: VU1 program D with the game's body normals (play.js, export_web_light.py).
        # The cape is lit by program D in the game too (hero dump: ambient-only corners =
        # 126 x A x 122/128 exactly), but which normal its draw uses is not decoded, so
        # it stays prelit (notes/fidelity-audit.md "Character lighting").
        prims.append(dict(attributes=attrs, indices=A(idx, 5123, 'SCALAR', 34963), mode=4,
                          material=1 if kind == 'alpha' else 0,
                          extras=dict(drawOrder=order, firstTri=s0, triCount=cnt, alpha=kind == 'alpha',
                                      lit=s0 < len(tris))))
    # texture atlas (RGBA, alpha stored as GS A*255/128)
    rb = open(os.path.join(ref, "vincent_p000_tex.rgba"), 'rb').read(); tw, th = struct.unpack_from('<II', rb)
    png = io.BytesIO(); Image.frombytes('RGBA', (tw, th), rb[8:]).save(png, 'PNG')
    j['images'] = [dict(bufferView=g.view(png.getvalue()), mimeType='image/png', name='vincent_atlas')]
    j['samplers'] = [dict(magFilter=9729, minFilter=9729, wrapS=33071, wrapT=33071)]
    j['textures'] = [dict(source=0, sampler=0)]
    aref = plj['char_alpha_ref']
    base = lambda: dict(pbrMetallicRoughness=dict(baseColorTexture=dict(index=0), metallicFactor=0.0, roughnessFactor=1.0),
                        doubleSided=True, extensions=dict(KHR_materials_unlit={}))
    j['materials'] = [dict(name='opaque', alphaMode='OPAQUE', **base()),
                      dict(name='alpha', alphaMode='MASK', alphaCutoff=aref, **base(),
                           extras=dict(gs='ATE GEQUAL 100 AFAIL RGB_ONLY + blend by As: '
                                          'use transparent=true, alphaTest=alphaCutoff, depthWrite=true'))]
    j['extensionsUsed'] = ['KHR_materials_unlit']
    j['meshes'] = [dict(name='vincent', primitives=prims)]
    # nodes: 0 = root (flip model -Y-up to Y-up), 1..bc = joints, bc+1 = mesh. No translation:
    # the game draws the model with MP+0x10 (the character matrix, unit rows), so model
    # origin = the character position (his feet in every in-place clip). The old bind-pose
    # bbox centring (feet at bind y = 12.13, 0.31 back) is not the game's (worker 0083).
    rootT = [0.0, 0.0, 0.0]
    bpos = pos[:nbody]                          # (Vincent's height, for the sidecar)
    j['nodes'] = [dict(name='vincent_root', rotation=[1.0, 0.0, 0.0, 0.0], translation=[float(x) for x in rootT],
                       children=list(range(1, bc + 1)))]
    for b in range(bc):
        t, q, s = mat_to_trs(Wb[b])
        j['nodes'].append(dict(name=f'j{b:02d}', translation=t.tolist(), rotation=q.tolist(), scale=s.tolist(),
                               extras=dict(bone=b, parent=int(sk['parent'][b]))))
    ibm = np.stack([np.linalg.inv(M).T.reshape(16) for M in Wb]).astype('<f4')   # column-major
    j['skins'] = [dict(name='vincent_skin', joints=list(range(1, bc + 1)), skeleton=0,
                       inverseBindMatrices=A(ibm, 5126, 'MAT4'))]
    j['nodes'].append(dict(name='vincent_mesh', mesh=0, skin=0))
    j['scenes'] = [dict(name='vincent', nodes=[0, bc + 1])]; j['scene'] = 0
    toY = lambda v: [v[0], -v[1], -v[2]]
    clip_side = lambda d: dict(
        frames=d['nf'], keys=len(d['frames']), duration_s=(len(d['frames']) - 1) / FPS,
        wrap_jump=round(d['loop_err'], 4), frame_step=round(d['step'], 4),
        keys_note='keys at frames 0..nf-1 + a closing key at nf = frame 0' if d['loop'] else
                  'one-shot: keys at frames 0..nf-1, no closing key (holds the last pose)',
        root_motion_per_frame_glb=[toY(r) for r in d['root']],
        root_motion_per_loop_glb=toY(np.array(d['root'][-1]) * d['nf'] / max(d['nf'] - 1, 1)),
        speed_units_per_frame=round(float(np.linalg.norm(d['root'][-1])) / max(d['nf'] - 1, 1), 4),
        **(dict(loop=d['loop'], in_place_axes='xyz' if d['zero_y'] else 'xz', median_step=round(d['step_med'], 4),
                container=os.path.basename(d['cont']),
                root_total_glb=toY(np.array(d['root'][-1]) - np.array(d['root'][0])))
           if ext else {}))
    if pack:                                    # animation-only glb: root + joint nodes, no mesh
        gp = GLB(); gp.j['nodes'] = j['nodes'][:bc + 1]
        gp.j['scenes'] = [dict(name='vincent_clips', nodes=[0])]; gp.j['scene'] = 0
        gp.j['animations'] = add_anims(gp, clips, bc, ext)
        gp.j['asset']['extras'] = dict(sidecar=f'vincent_{pack}.json', base='vincent.glb')
        gp.write(os.path.join(out, f'vincent_{pack}.glb'))
        json.dump(dict(format=f'vincent_{pack}.glb: animation-only clip pack for vincent.glb (same joint '
                              'node names j00..j92; three.js binds its tracks to the main model by name). '
                              'Fields as in vincent.json clips.', fps=dict(assumed=FPS),
                       bone_count=bc, parent_sha1=parent_sha1(sk['parent']),
                       clips={c: clip_side(d) for c, d in clips.items()},
                       local_pose=local_pose(sk, parent, clips, out, f'vincent_{pack}_local.bin')),
                  open(os.path.join(out, f'vincent_{pack}.json'), 'w'), indent=1)
        print("wrote", os.path.join(out, f'vincent_{pack}.glb'), f"{os.path.getsize(os.path.join(out, f'vincent_{pack}.glb')) / 1e6:.2f} MB,",
              len(clips), "clips:", ' '.join(f"{c}{'(loop)' if d['loop'] else ''}" for c, d in clips.items()))
        return
    j['animations'] = add_anims(g, clips, bc, ext)
    j['asset']['extras'] = dict(sidecar='vincent.json')
    name = 'vincent_MUTANT_parents.glb' if mutate_parents else 'vincent.glb'
    g.write(os.path.join(out, name))
    print("wrote", os.path.join(out, name), f"({len(g.bin) // 1024} KiB bin, {len(pos)} verts, {len(pos) // 3} tris)")
    asz = sum(g.j['bufferViews'][g.j['accessors'][s_['output']]['bufferView']]['byteLength']
              for a in j['animations'] for s_ in a['samplers'])
    print(f"animation outputs {asz / 1e6:.2f} MB over {sum(d['nf'] for d in clips.values())} frames")
    if mutate_parents: return

    # ---- sidecar
    side = dict(
        format="vincent.glb: glTF 2.0 binary, one skinned mesh, flat joints, see 'notes'",
        glb='vincent.glb', units='game world units (unit scale, MP+0x10; Vincent ~%.1f tall)'
            % (bpos[:, 1].max() - bpos[:, 1].min()),
        axes='glb is Y-up via the root node (Rx 180 deg, no translation: model origin = character position, feet at y~0 in the in-place clips). Model space '
             '(joint matrices, IBMs, POSITION) is the game\'s -Y-up space; root.translation=%s' % rootT,
        forward='+Z in glb space (h_wk root motion moves +Z after the flip)',
        vertices=len(pos), triangles=len(pos) // 3, body_triangles=nbody // 3, cape_triangles=len(cp.tris),
        layout='unindexed per-corner vertices in the mesh order of export_p000_gpu2 '
               '(body triangles in binding.json order, cape last); each primitive indexes a run',
        draw_runs=[dict(order=i, first_tri=s0, tri_count=c_, alpha=k == 'alpha') for i, (s0, c_, k) in enumerate(runs)],
        draw_order_note='primitives are already in the game draw order; set mesh.renderOrder = '
                        'primitive.extras.drawOrder (GLTFLoader puts extras in userData)',
        color='COLOR_0 = game vertex colour / 128 (1.0 = neutral; PS2 MODULATE texel*vcol*2), up to ~2.0; '
              'alpha likewise (1.0 = GS 128). Unlit (KHR_materials_unlit): texture x COLOR_0, no lighting.',
        texture='atlas %dx%d RGBA PNG embedded; texture alpha = GS A*255/128' % (tw, th),
        alpha=dict(cutoff=aref, game='ATE GEQUAL 100, AFAIL RGB_ONLY, blended by As', material='alpha'),
        culling='none (doubleSided)',
        skin=dict(body='the game\'s weighted skin (FUN_005b1190 + VU0 0x120: cntA vertices start a slot, '
                       'cntB add w x M.local; w = the batch S-32 stream); top 4 influences renormalised '
                       '(%d of %d body vertices had 5, dropped weight <= %.2f)' % (bst['trimmed'], len(V), bst['dropped_w']),
                  cape='p000_cloth weighted skin records (exporter --clip cape mode, no cloth sim); '
                       'top 4 influences renormalised (%d of %d cape vertices had 5)' % (trimmed, cp.N)),
        joints=dict(count=bc, node_index='joint b = node b+1', names='j00..j%02d' % (bc - 1),
                    parent=[int(p) for p in sk['parent']],
                    hierarchy_note='joints are FLAT in the glb (children of the root); animation '
                                   'channels carry each joint\'s model-space TRS. Use parent[] only for '
                                   'debugging/IK, not for re-parenting nodes (non-uniform scale '
                                   'inheritance is not a standard hierarchy)',
                    bind_world_colmajor=[np.array(M).T.reshape(16).tolist() for M in Wb]),
        clips={c: clip_side(d) for c, d in clips.items()},
        fps=dict(assumed=FPS, note='1 clip frame per game tick at 60 Hz is ASSUMED (HANDOFF: unproven; '
                                   'cloth notes suggest ~2 clip frames per 3 vsyncs = 40 fps). '
                                   'Use AnimationAction.timeScale to change.'),
        attach=GLOW_GUN, bind56_world_colmajor=np.array(Wb[56]).T.reshape(16).tolist(),
        source=dict(ref_run=f"export_p000_gpu2.py --clip {REF_CLIP[0]} --frame {REF_CLIP[1]} --in-place",
                    clip_container=os.path.basename(CLIP_CONT) if not ext else
                    sorted({os.path.basename(d['cont']) for d in clips.values()})))
    side['local_pose'] = local_pose(sk, parent, clips, out)
    json.dump(side, open(os.path.join(out, 'vincent.json'), 'w'), indent=1)
    print("wrote", os.path.join(out, 'vincent.json'))
    for c, d in clips.items():
        print(c, "frames", d['nf'], "wrap jump", round(d['loop_err'], 3), "vs frame step", round(d['step'], 3),
              "speed/frame", side['clips'][c]['speed_units_per_frame'], "root at nf-1 (glb)",
              [round(x, 3) for x in toY(d['root'][-1])])


# ---------------------------------------------------------------- cloth
def cloth(out):
    """OUT/cloth.json: everything native/web/cloth.js reads, from the game files only (no
    dump). Worker 0073's generator (gen_cloth_json.py), moved here as a step. Reads
    p000_cloth.Cape (07700001.bin cape block 0x19f70), the skeleton's world_bind, the body
    decode (for the glb's first cape triangle) and each clip's cloth-window bytes."""
    import p000_cloth as CL
    p = os.path.join(out, 'cloth.json')
    # the output file itself (check_ignored's probe.glb is ignored anywhere by *.glb)
    if subprocess.run(["git", "-C", REPO, "check-ignore", "-q", p]).returncode != 0:
        sys.exit(f"REFUSING: {p} is not gitignored (game-derived output)")
    os.makedirs(out, exist_ok=True)
    cp = CL.Cape()
    buf, NB, sk = load()
    Wb = [np.array(m, float) for m in sk['world_bind']]
    g = cp.groups[0]
    # skin records (FUN_005b06d0): per influence the vertex, bone, weight, bone-local xyz, and
    # the same point in bind model space (= glb POSITION space) for three.js's skinning formula
    # q in plain floats with an exact fused multiply-add, in the order OpenBLAS's gemv used
    # under numpy 2.2.6 when 0073 wrote the first cloth.json (all 552 floats match). numpy's
    # own matmul differs in the last bit between versions (1.21 vs 2.2), and the file must
    # not depend on which numpy runs it.
    from fractions import Fraction as Fr
    fma = lambda a, b, c: float(Fr(a) * Fr(b) + Fr(c))
    skin = []
    for v, b, w, loc, _n in cp.skin:
        M = Wb[b].tolist(); l = [float(x) for x in loc]
        q = [fma(M[r][2], l[2], fma(M[r][0], l[0], M[r][1] * l[1])) + M[r][3] for r in range(3)]
        skin.append([int(v), int(b), float(w), *l, *q])
    # cloth group (FUN_005b0d30 / b38 / 09f8 / 0c50); indices are cloth-buffer vertex numbers
    lst = [int(v) | (0x8000 if pin else 0) for v, pin in zip(g['verts'], g['pinned'])]
    # constraints as the game stores them (group +0x20, 8 bytes): u8 a, u8 b = indices into
    # the vertex list (not vertex numbers), f32 rest length
    Pc = CL.CAPE_BLOCK + 0x10; cb = cp.buf
    G = Pc + CL._u32(cb, Pc + CL._u32(cb, Pc + 0x24))
    cons = [[cb[G + 0x20 + 8 * k], cb[G + 0x21 + 8 * k], CL._f32(cb, G + 0x24 + 8 * k)]
            for k in range(CL._s16(cb, G + 0x12))]
    assert [lst[a] & 0x7fff for a, _x, _r in cons] == [int(x) for x in g['ca']]
    assert [lst[b] & 0x7fff for _x, b, _r in cons] == [int(x) for x in g['cb']]
    # glb cape corners -> cloth vertex (export(): cape corners last, cp.tris order)
    cvi = [int(v) for vs, _u, _c in cp.tris for v in vs]
    # the glb's first cape triangle = the body triangle count (export(): body first)
    first = len(body_skin(buf, NB, sk)[4])
    vj = os.path.join(out, 'vincent.json')
    if os.path.exists(vj):
        assert json.load(open(vj))['body_triangles'] == first, "vincent.json is from another body decode"
    # clip cloth windows (FUN_005a95f0): flag 0 -> window cleared, 2 -> 2 x u16, else unchanged
    windows = {}
    for st, cids in CLIP_SETS.items():
        for cid in cids:
            path = NOPATH + cid + '.bin'
            if not os.path.exists(path):
                continue
            b = open(path, 'rb').read()
            for name, _f in list_clips(path):
                if name in windows:
                    continue
                _cb, CP = find_clip(path, name)
                flag = b[CP + 0x19]; n16 = struct.unpack_from('<H', b, CP + 0x1a)[0]
                windows[name] = [int(flag), int(n16)]
    side = dict(
        note='cloth.js data (worker 0073 generator; game-derived, never commit). See cloth.js header.',
        N=int(cp.N), mesh_scale=float(cp.mesh_scale),
        group=dict(gravity=float(g['gravity']), blend=float(g['blend']), damping=float(g['damping']),
                   list=lst, constraints=cons),
        skin_fields='vertex, bone, weight, local x y z (x meshScale/4096), bind-model x y z',
        skin=skin, corner_vertex=cvi, cape_first_tri=int(first),
        clip_windows=windows, clip_windows_fields='[flag +0x19, u16 +0x1a]; window = 2 x u16 when flag == 2')
    json.dump(side, open(p, 'w'), separators=(',', ':'))
    print("wrote", p, os.path.getsize(p), 'bytes;', len(skin), 'influences;', len(cons), 'constraints;',
          sum(1 for x in lst if x & 0x8000), 'pinned;', len(windows), 'clips')


# ---------------------------------------------------------------- verify
def skin_glb(path, clip, frame, anim_path=None):
    """Skin the glb's own data at an integer clip frame (keys sit on frames), model space.
    anim_path: take the clip's tracks from this glb instead (a clip pack: same joint nodes)."""
    j, acc = read_glb(path)
    ja, acca = read_glb(anim_path) if anim_path else (j, acc)
    an = next(a for a in ja['animations'] if a['name'] == clip)
    sk = j['skins'][0]; ibm = acc(sk['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1)
    node2j = {n: k for k, n in enumerate(sk['joints'])}
    T = {}; Q = {}; S = {}
    for ch in an['channels']:
        s = an['samplers'][ch['sampler']]; out = acca(s['output']).astype(float)
        tm = acca(s['input']); f = int(np.argmin(np.abs(tm - frame / FPS)))
        {'translation': T, 'rotation': Q, 'scale': S}[ch['target']['path']][node2j[ch['target']['node']]] = out[f]
    J = []
    for k in range(len(sk['joints'])):
        M = np.eye(4); M[:3, :3] = qmat(Q[k]) * S[k][None, :]; M[:3, 3] = T[k]; J.append(M @ ibm[k])
    J = np.array(J)
    a = j['meshes'][0]['primitives'][0]['attributes']
    p = acc(a['POSITION']).astype(float); jn = acc(a['JOINTS_0']).astype(int); w = acc(a['WEIGHTS_0']).astype(float)
    ph = np.c_[p, np.ones(len(p))]
    out = np.zeros((len(p), 3))
    for k in range(4):
        out += w[:, k:k + 1] * np.einsum('nij,nj->ni', J[jn[:, k]], ph)[:, :3]
    return out


def verify(dump, out, glb, clip, frames):
    check_ignored(out)
    chk = os.path.join(out, "_ref_check"); os.makedirs(chk, exist_ok=True)
    side = json.load(open(os.path.join(out, 'vincent.json'))); nb = side['body_triangles'] * 3
    anim = None
    if clip not in side['clips']:              # a pack clip: its sidecar + the pack's tracks
        import glob
        for pj in sorted(glob.glob(os.path.join(out, 'vincent_*.json'))):
            pc = json.load(open(pj)).get('clips', {})
            if clip in pc:
                side = dict(side, clips=pc); anim = pj[:-5] + '.glb'; break
        else:
            sys.exit(f"{clip}: not in vincent.json or any vincent_*.json pack")
        print(f"{clip}: pack clip, tracks from {os.path.basename(anim)}")
    ci = side['clips'][clip]; cont = NOPATH + ci['container'] if 'container' in ci else None
    res = {}
    for f in frames:
        run_ref(dump, chk, clip, f, cont)
        ref = read_mesh(os.path.join(chk, "vincent_p000.mesh"))[:, :3].astype(float)
        X = skin_glb(glb, clip, f, anim)
        if ci.get('in_place_axes') == 'xyz':   # gpu2 --in-place keeps bone 1 y: add it back
            X[:, 1] -= ci['root_motion_per_frame_glb'][f][1]   # glb y = -model y
        e = np.linalg.norm(X - ref, axis=1)
        r = dict(body_median=float(np.median(e[:nb])), body_max=float(e[:nb].max()),
                 cape_median=float(np.median(e[nb:])), cape_max=float(e[nb:].max()))
        res[f] = {k: round(v, 6) for k, v in r.items()}
        print(f"{os.path.basename(glb)} {clip} f{f}: ", res[f])
    return res


# ---------------------------------------------------------------- catalogue
# Purpose GUESSES from the clip name, checked against the measured root motion / hip height.
# 'conf': high = root motion or hip height confirms it; mid = name pattern + data agree;
# low = name only. Weapon sets h_/m_/r_ (handgun / machine gun / rifle) share one name scheme.
WEAPON = {
    'wk': ('walk', 'high'), 'rn': ('run', 'high'), 'sq': ('crouch idle', 'high'), 'st': ('stand idle', 'high'),
    'jp0': ('jump: take-off crouch', 'high'), 'jp1': ('jump: rise (12 up)', 'high'),
    'jp2': ('jump: descend (12 down)', 'high'), 'jp3': ('jump: airborne / falling loop', 'mid'),
    'jp4': ('jump: landing', 'high'), 'div': ('dive / roll forward (30)', 'mid'),
    'wk_a': ('aim level, walking', 'mid'), 'rn_a': ('aim level, running', 'mid'),
    'sq_a': ('aim level, crouched', 'mid'), 'st_a': ('aim level, standing', 'mid'),
    'sq_f': ('fire, crouched', 'mid'), 'st_f': ('fire, standing', 'mid'),
    'sq_m': ('crouch walk', 'high'), 'rev_0': ('unknown, hips drop to the floor (collapse/revive?)', 'low'),
    'brn_a': ('aimed strafe run backward', 'high'), 'lrn_a': ('aimed strafe run left', 'mid'),
    'rrn_a': ('aimed strafe run right', 'high'), 'sq_ma': ('aimed crouch walk forward', 'high'),
    'bs_ma': ('aimed crouch walk backward', 'high'), 'ls_ma': ('aimed crouch walk left', 'mid'),
    'rs_ma': ('aimed crouch walk right', 'mid'),
    'wk_rd': ('reload while walking', 'mid'), 'rn_rd': ('reload while running', 'mid'),
    'sq_rd': ('reload, crouched', 'mid'), 'st_rd': ('reload, standing', 'mid'),
    'fwk_rd': ('reload while walking (fast/forward?)', 'low'), 'sqm_rd': ('reload while crouch walking', 'mid'),
    'frn_rd': ('reload while running (fast/forward?)', 'low'), 'swtch': ('weapon switch / draw', 'mid'),
    'ld_del': ('ladder exit at bottom, left (weapon arm variant, no root motion)', 'low'),
    'ld_der': ('ladder exit at bottom, right (weapon arm variant, no root motion)', 'low'),
    'ld_uel': ('ladder exit at top, left (weapon arm variant, no root motion)', 'low'),
    'ld_uer': ('ladder exit at top, right (weapon arm variant, no root motion)', 'low'),
}
for _b in ('wk', 'rn', 'sq', 'st', 'brn', 'lrn', 'rrn', 'sq_m', 'bs_m', 'ls_m', 'rs_m'):
    for _sfx, _w in (('d', 'down'), ('u', 'up')):
        _k = _b + ('a' if _b.endswith('_m') else '_a') + _sfx
        _base = WEAPON.get(_b + ('a' if _b.endswith('_m') else '_a'), ('aim', 'mid'))[0]
        WEAPON[_k] = (_base.replace('aim level', f'aim {_w}').replace('aimed', f'aimed {_w}'), 'mid')
for _p in ('sq', 'st'):
    for _sfx, _w in (('d', 'down'), ('u', 'up')): WEAPON[f'{_p}_f{_sfx}'] = (WEAPON[f'{_p}_f'][0].replace('fire', f'fire {_w}'), 'mid')
COMMON = {
    'sqd': ('hit flinch, crouched (d = damage?)', 'low'), 'std': ('hit flinch, standing', 'low'),
    'jzd': ('hit flinch in the "jz" stance', 'low'),
    'tf_0': ('unknown "tf" (hips lower ~3)', 'low'), 'tf_1': ('unknown "tf" variant', 'low'),
    'tfof_0': ('end of "tf" (tf off)', 'low'), 'tfof_1': ('end of "tf" variant', 'low'),
    'bl_0': ('knocked up/back (bl = blown?)', 'low'), 'bl_1': ('knocked down, 28.7 back, ends lying', 'mid'),
    'fl_0': ('on the floor, kneeling/low (fl = floor?)', 'low'), 'fl_1': ('lying on the floor / getting up', 'low'),
    'bl_ht0': ('blown/hit variant', 'low'),
    'jz_a': ('"jz" stance aim level (1 frame)', 'low'), 'jz_ad': ('"jz" stance aim down (1 frame)', 'low'),
    'jz_au': ('"jz" stance aim up (1 frame)', 'low'), 'jzend': ('leave the "jz" stance', 'low'),
    'jzsetl': ('enter the "jz" stance from the left (6.3 forward)', 'low'),
    'jzsetr': ('enter the "jz" stance from the right', 'low'),
    'jz_fa0': ('death/fall in the "jz" stance', 'low'), 'jz_fa1': ('death/fall in the "jz" stance, variant', 'low'),
    'jz_bl0': ('knocked back out of the "jz" stance', 'low'),
    'ld_d': ('ladder: get on at the TOP, climbing down (18 down)', 'high'),
    'ld_u': ('ladder: get on at the BOTTOM (6 up, starts 5.5 back)', 'high'),
    'ld_jp': ('ladder: jump off backward (10 back)', 'mid'), 'ld_fa0': ('ladder: fall/knocked off', 'mid'),
    'ld_lid': ('ladder: idle, left hand up (loop)', 'high'), 'ld_rid': ('ladder: idle, right hand up (loop)', 'high'),
    'ld_uml': ('ladder: climb up one step (4.5), left', 'high'), 'ld_umr': ('ladder: climb up one step (4.5), right', 'high'),
    'ld_dml': ('ladder: climb down one step (4.5), left', 'high'), 'ld_dmr': ('ladder: climb down one step (4.5), right', 'high'),
    'ld_uel': ('ladder: get off at the TOP, left (18 up, 3.5 forward)', 'high'),
    'ld_uer': ('ladder: get off at the TOP, right', 'high'),
    'ld_del': ('ladder: get off at the BOTTOM, left (6 down, 5.5 back)', 'high'),
    'ld_der': ('ladder: get off at the BOTTOM, right', 'high'),
    'atk_0': ('melee combo hit 1', 'mid'), 'atk_1': ('melee combo hit 2', 'mid'), 'atk_2': ('melee combo hit 3', 'mid'),
    'atk_3': ('melee combo hit 4', 'mid'), 'atk_4': ('melee finisher (hop)', 'mid'),
    'atk_01': ('melee: recover after hit 1', 'low'), 'atk_11': ('melee: recover after hit 2', 'low'),
    'atk_21': ('melee: recover after hit 3', 'low'),
    'jp_fa0': ('death in the air (fa = fatal?)', 'low'), 'sq_fa0': ('death, crouched (ends on the floor)', 'mid'),
    'st_fa0': ('death, standing (ends on the floor)', 'mid'),
    'jp_bl0': ('hit knockback in the air (14-17 back)', 'mid'), 'sq_bl0': ('hit knockback, crouched', 'mid'),
    'st_bl0': ('hit knockback, standing', 'mid'),
}


def guess(name):
    if name.startswith('bv'): return 'event / cutscene motion', 'low'
    pre, _, base = name.partition('_')
    if pre == 'c': return COMMON.get(base, ('unknown', 'low'))
    return WEAPON.get(base, ('unknown', 'low'))


def catalogue(out):
    check_ignored(out)
    sk = load()[2]; bc = sk['bone_count']
    toY = lambda v: [round(float(v[0]), 3), round(float(-v[1]), 3), round(float(-v[2]), 3)]
    cat = []
    for st, cids in CLIP_SETS.items():
        for cid in cids:
            cont = NOPATH + cid + '.bin'
            for name, nf in list_clips(cont):
                cbuf, CP = find_clip(cont, name)
                T = [AR.decode_frame_trs(cbuf, CP, bc, f) for f in range(nf)]
                Wr = [AR.compose(sk, t, root_motion=True) for t in T]
                Wi = [compose_trs(sk, t, zero_y=True) for t in T]        # fully in place
                r = np.array([W[1][:3, 3] for W in Wr]) - np.array([W[1][:3, 3] for W in Wi])
                hip = np.array([W[2][:3, 3] for W in Wr])
                d = lambda a, b: max(float(np.abs(x - y).max()) for x, y in zip(a, b))
                wrap, step = (d(Wi[-1], Wi[0]), d(Wi[0], Wi[1])) if nf > 1 else (0.0, 0.0)
                smed = float(np.median([d(Wi[f], Wi[f + 1]) for f in range(nf - 1)])) if nf > 1 else 0.0
                tot = r[-1] - r[0]; g = toY(tot)
                pur, conf = guess(name)
                cat.append(dict(
                    name=name, set=st, container=cid, frames=nf, duration_s=round(nf / FPS, 3),
                    loops=bool(is_loop(wrap, smed, nf)), wrap_jump=round(wrap, 3), median_step=round(smed, 3),
                    root_start_glb=toY(r[0]), root_total_glb=g,
                    root_horizontal=round(float(np.hypot(tot[0], tot[2])), 3), root_up=g[1],
                    vertical_root_motion=bool(np.abs(r[:, 1]).max() > 1e-6),
                    root_up_range=[round(float(-r[:, 1].max()), 3), round(float(-r[:, 1].min()), 3)],
                    hip_height=[round(float(-hip[0, 1]), 2), round(float(-hip[-1, 1]), 2)],
                    hip_height_range=[round(float(-hip[:, 1].max()), 2), round(float(-hip[:, 1].min()), 2)],
                    purpose=pur, confidence=conf))
    # the gun's own clips (4-bone Cerberus rig)
    from skd_skeleton import load_skeleton
    gsk = load_skeleton(GUN_CONT, trans_scale=1.0); gbc = gsk['bone_count']; gun = []
    for name, nf in list_clips(GUN_CLIP_CONT):
        cbuf, CP = find_clip(GUN_CLIP_CONT, name)
        Ws = [AR.compose(gsk, AR.decode_frame_trs(cbuf, CP, gbc, f)) for f in range(nf)]
        bones = []
        for b in range(gbc):
            R0 = Ws[0][b][:3, :3]; ang = []
            for W in Ws:
                Rm = W[b][:3, :3] @ np.linalg.inv(R0)
                ang.append(np.degrees(np.arccos(np.clip((np.trace(Rm) - 1) / 2, -1, 1))))
            tr = np.array([W[b][:3, 3] for W in Ws])
            bones.append(dict(bone=b, parent=int(gsk['parent'][b]), max_rot_deg=round(float(max(ang)), 1),
                              translation_range=round(float(np.ptp(tr, axis=0).max()), 3)))
        gun.append(dict(name=name, container='07770580', frames=nf, duration_s=round(nf / FPS, 3),
                        pairs_with=name, purpose='Cerberus side of ' + guess(name)[0], bones=bones))
    side = dict(
        note='Vincent (p000) motion catalogue, generated by export_web_vincent.py catalogue. Axes are glb '
             'space (Y up, +Z = his forward). Root = bone 1 translation (the part the glb removes, per '
             'axis); hip = bone 2 height above the model origin (~12 standing, ~5.6 crouched, ~1 lying). '
             'loops = measured (fully in place: pose jump last->first <= max(2 x frame step, 0.5)). '
             'purpose = a GUESS from the name, conf high/mid/low (see the code). 60 fps ASSUMED.',
        sets={k: v for k, v in CLIP_SETS.items()},
        skipped={'0770000a': 'byte-identical copy of 07700000',
                 '07701900/07703200 sets': 'other p000 builds (own rig + motion sets), not catalogued'},
        clips=cat, gun_clips=gun,
        gun_note='07770580 = 6 reload clips on the gun\'s own 4-bone skeleton (07770581), same names and '
                 'lengths as Vincent\'s h_*_rd (h_st_rd: 41 vs 40). No other gun motions exist.')
    json.dump(side, open(os.path.join(out, 'motions.json'), 'w'), indent=1)
    print('wrote', os.path.join(out, 'motions.json'), len(cat), 'clips +', len(gun), 'gun clips')
    return side


def main(argv):
    if argv[:1] == ['catalogue']:
        out = DEFAULT_OUT if '--out' not in argv else os.path.abspath(argv[argv.index('--out') + 1])
        return catalogue(out)
    if argv[:1] == ['cloth']:
        out = DEFAULT_OUT if '--out' not in argv else os.path.abspath(argv[argv.index('--out') + 1])
        return cloth(out)
    if len(argv) < 2 or argv[0] not in ('export', 'verify'): sys.exit(__doc__)
    mode, dump = argv[0], argv[1]; out = DEFAULT_OUT
    if dump == '--no-dump': dump = None          # everything from the game files (tools/byo)
    glb = None; clip = 'h_wk'; frames = [0, 13, 27]
    mutate = False; i = 2; clip_spec = None; sets = DEFAULT_SETS; pack = None
    while i < len(argv):
        k = argv[i]
        if k == '--out': out = os.path.abspath(argv[i + 1]); i += 2
        elif k == '--glb': glb = argv[i + 1]; i += 2
        elif k == '--clip': clip = argv[i + 1]; i += 2
        elif k == '--frames': frames = [int(x) for x in argv[i + 1].split(',')]; i += 2
        elif k == '--mutate-parents': mutate = True; i += 1
        elif k == '--clips': clip_spec = argv[i + 1]; i += 2
        elif k == '--sets': sets = argv[i + 1].split(','); i += 2
        elif k == '--pack': pack = argv[i + 1]; i += 2
        else: sys.exit(f"unknown option {k}")
    if mode == 'export':
        export(dump, out, mutate, clip_spec, sets, pack)
        if pack is None: cloth(out)
    else: verify(dump, out, glb or os.path.join(out, 'vincent.glb'), clip, frames)


if __name__ == '__main__':
    main(sys.argv[1:])
