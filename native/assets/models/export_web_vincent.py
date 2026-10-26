#!/usr/bin/env python3
"""Export sea-of-flames Vincent (p000) as a SKINNED, ANIMATED glTF binary (.glb) for the
three.js web viewer, plus a JSON sidecar. Output is game-derived: write it only into a
gitignored directory (default native/web/data/vincent/, checked with git check-ignore).

  export:  python3 -I export_web_vincent.py export <dump.bin> [--out DIR] [--mutate-parents]
  verify:  python3 -I export_web_vincent.py verify <dump.bin> [--out DIR] [--glb FILE]
                                                   [--clip h_wk --frames 0,13,27]

How it is built
- Textures, UVs, vertex colours, per-triangle alpha flag and the game's draw-order runs come
  from one export_p000_gpu2.py run (--clip h_st --frame 0 --in-place) into OUT/_ref/ (its
  vincent_p000.mesh corner order is kept: body triangles in binding.json order, cape last,
  so its -charruns triangle ranges index our triangles directly).
- Skeleton: render_p005.load_skeleton (93 bones). Body skin: each pool slot's bone and
  bone-local position, read with build_group_pool's own batch walk (single bone per vertex,
  last batch writing a slot wins, exactly what the decoder does; the group header's blend
  weights are still ignored). Bind positions = world_bind[bone] x local.
- Cape: p000_cloth.Cape's weighted skin records (bones 28-79 incl. its own chains), i.e.
  the exporter's --clip cape mode (skinned target, no cloth sim). Up to 5 influences per
  vertex; three.js uses 4, so the smallest weight is dropped and the rest renormalised.
- Clips (anim_rot: decode_frame_trs + compose, in place = bone 1 x/z zeroed): per integer
  frame, each joint's WORLD (model-space) matrix R*diag(S) + T as glTF TRS on a FLAT joint
  list (every joint a direct child of the root node). The game's scale composition
  (S_w = S_parent * s, non-uniform, h_wk up to 1.41) is not a standard node hierarchy, so
  local TRS would need shear; world TRS is exact. The real parents are in the JSON.
- Root motion (bone 1 x/z) is removed from the glTF clips and given per frame in the JSON.
"""
import sys, os, json, struct, subprocess, io
import numpy as np
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, "/srv/games/dirge-port/tools/gsdump/src")
import render_p005 as P
import anim_rot as AR
from render_p000_bound import CONT, TEXC

REPO = os.path.abspath(os.path.join(HERE, "../../.."))
DEFAULT_OUT = os.path.join(REPO, "native/web/data/vincent")
CLIP_CONT = "/srv/games/dirge-port/extract/_KEL/noPath/0770000b.bin"   # h_* (handgun) clips
CLIPS = ["h_st", "h_wk", "h_rn"]
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
    """build_group_pool's batch walk (render_skinned.py), keeping each slot's bone and
    bone-local position instead of the posed one. Same skip rules, last write wins."""
    R = P.R
    piv = Pb + R._u32(buf, G + 0x0c)
    cnt = R._i32(buf, piv); cr = piv + R._i32(buf, piv + 8); raw = piv + R._i32(buf, piv + 0xc)
    loc = np.zeros((256, 3)); bone = np.full(256, -1)
    for b in range(cnt):
        o = cr + b * 8
        if o + 8 > NB: break
        bn = R._s16(buf, o); num = buf[o + 2] + buf[o + 3]
        qwc = R._s16(buf, o + 4); stride = R._s16(buf, o + 6)
        if num == 0: raw += max(stride, 0); continue
        if not (0 <= bn < bc) or qwc <= 0 or raw + 2 > NB: raw += max(stride, 0); continue
        pos, nrm = R._parse_batch(buf, NB, raw, qwc, num); raw += stride
        if pos is None or nrm is None or len(pos) < num or len(nrm) < num: continue
        slot = (nrm[:num, 3].astype(int) + 0x80) & 0xff
        for k in range(num):
            loc[slot[k]] = pos[k] * sc; bone[slot[k]] = bn
    return loc, bone


def body_skin(buf, NB, sk):
    """(bind positions, bind normals, per-vertex bone, tris) over decode_groups' vertices."""
    bc = sk['bone_count']; Wb = [np.array(m, float) for m in sk['world_bind']]
    V, N, tris, gmeta = P.decode_groups(buf, NB, Wb, bc)
    bone = np.full(len(V), -1)
    for name, base, psize, _seen, _pool in gmeta:
        Pb = int(name.split('@')[1].split('.')[0], 16); gi = int(name.rsplit('.g', 1)[1])
        G = Pb + P.R._u32(buf, Pb + P.R._u32(buf, Pb + 0x20) + 4 * gi)
        loc, bn = slot_skin(buf, NB, bc, Pb, G, P.R._f32(buf, Pb + 0x2c) / 2048.0)
        bone[base:base + psize] = bn[:psize]
        for s in range(psize):           # the decoder's bind position must be bone x local
            if bn[s] >= 0:
                p = Wb[bn[s]][:3, :3] @ loc[s] + Wb[bn[s]][:3, 3]
                assert np.abs(p - V[base + s]).max() < 1e-4, (name, s)
    Nn = np.asarray(N, float); ln = np.linalg.norm(Nn, axis=1, keepdims=True); ln[ln < 1e-9] = 1
    return np.asarray(V, float), Nn / ln, bone, tris


def clip_mats(sk, clip, frame, parent=None, root_motion=False):
    cbuf, CP = AR.find_clip(CLIP_CONT, clip)
    trs = AR.decode_frame_trs(cbuf, CP, sk['bone_count'], frame)
    s = sk if parent is None else dict(sk, parent=parent)
    return AR.compose(s, trs, root_motion=root_motion)


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


def run_ref(dump, outdir, clip, frame):
    cmd = [sys.executable, "-I", os.path.join(HERE, "export_p000_gpu2.py"), dump, outdir,
           "--clip", clip, "--frame", str(frame), "--in-place"]
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
def export(dump, out, mutate_parents=False):
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
    V, N, bone, tris = body_skin(buf, NB, sk)
    assert len(tris) == len(bindj) and all([int(k) for k in t[:3]] == r['v'] for t, r in zip(tris, bindj))
    nbody = len(tris) * 3

    # ---- body corners (mesh order), single bone, weight 1
    vi = np.array([r['v'] for r in bindj]).reshape(-1)
    pos = [V[vi]]; nrm = [N[vi]]
    joints = [np.c_[bone[vi], np.zeros((nbody, 3), int)]]
    weights = [np.c_[np.ones(nbody), np.zeros((nbody, 3))]]
    assert (bone[vi] >= 0).all()

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
    clips = {}
    for c in CLIPS:
        cbuf, CP = AR.find_clip(CLIP_CONT, c); nf = AR.clip_frames(cbuf, CP)
        frames = []; root = []
        # keys sit on frames 0..nf-1 (the last track key is at nf-1; frame nf would be
        # extrapolated). The wrap nf-1 -> 0 is about one ordinary frame step, so a closing
        # key at nf repeating frame 0 makes the glTF loop seamless.
        for f in range(nf):
            W = clip_mats(sk, c, f, parent)
            frames.append([mat_to_trs(M) for M in W])
            Wr = clip_mats(sk, c, f, parent, root_motion=True)
            root.append((Wr[1][:3, 3] - W[1][:3, 3]).tolist())
        frames.append(list(frames[0]))
        for b in range(bc):               # quaternion hemisphere continuity for slerp
            for f in range(1, len(frames)):
                if np.dot(frames[f][b][1], frames[f - 1][b][1]) < 0:
                    t, q, s = frames[f][b]; frames[f][b] = (t, -q, s)
        d_ = lambda f0, f1: max(float(np.abs(a - b).max()) for a, b in
                                zip(clip_mats(sk, c, f0, parent), clip_mats(sk, c, f1, parent)))
        clips[c] = dict(frames=frames, root=root, nf=nf, loop_err=d_(nf - 1, 0), step=d_(0, 1))

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
        prims.append(dict(attributes=attrs, indices=A(idx, 5123, 'SCALAR', 34963), mode=4,
                          material=1 if kind == 'alpha' else 0,
                          extras=dict(drawOrder=order, firstTri=s0, triCount=cnt, alpha=kind == 'alpha')))
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
    # nodes: 0 = root (flip model -Y-up to Y-up, feet at y=0, centred), 1..bc = joints, bc+1 = mesh
    bpos = pos[:nbody]; piv = np.array([(bpos[:, 0].min() + bpos[:, 0].max()) / 2, bpos[:, 1].max(),
                                        (bpos[:, 2].min() + bpos[:, 2].max()) / 2])
    rootT = [-piv[0], piv[1], piv[2]]           # Rx(180): (x,y,z) -> (x,-y,-z)
    st0 = clips['h_st']['frames'][0]
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
    j['animations'] = []
    for c, d in clips.items():
        tm = (np.arange(d['nf'] + 1) / FPS).astype('<f4')
        ti = A(tm, 5126, 'SCALAR', minmax=True)
        ch = []; smp = []
        for b in range(bc):
            for path, k, typ in (('translation', 0, 'VEC3'), ('rotation', 1, 'VEC4'), ('scale', 2, 'VEC3')):
                arr = np.array([fr[b][k] for fr in d['frames']], '<f4')
                smp.append(dict(input=ti, output=A(arr, 5126, typ), interpolation='LINEAR'))
                ch.append(dict(sampler=len(smp) - 1, target=dict(node=b + 1, path=path)))
        j['animations'].append(dict(name=c, channels=ch, samplers=smp,
                                    extras=dict(frames=d['nf'], fps_assumed=FPS, in_place=True)))
    j['asset']['extras'] = dict(sidecar='vincent.json')
    name = 'vincent_MUTANT_parents.glb' if mutate_parents else 'vincent.glb'
    g.write(os.path.join(out, name))
    print("wrote", os.path.join(out, name), f"({len(g.bin) // 1024} KiB bin, {len(pos)} verts, {len(pos) // 3} tris)")
    if mutate_parents: return

    # ---- sidecar
    toY = lambda v: [v[0], -v[1], -v[2]]
    side = dict(
        format="vincent.glb: glTF 2.0 binary, one skinned mesh, flat joints, see 'notes'",
        glb='vincent.glb', units='game world units (placement scale ~1.0; Vincent ~%.1f tall)'
            % (bpos[:, 1].max() - bpos[:, 1].min()),
        axes='glb is Y-up via the root node (Rx 180 deg + centring, feet at y=0). Model space '
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
        skin=dict(body='single bone per vertex (JOINTS_0.x, weight 1); the decoder\'s last-write-wins '
                       'slot rule; group-header blend weights NOT used',
                  cape='p000_cloth weighted skin records (exporter --clip cape mode, no cloth sim); '
                       'top 4 influences renormalised (%d of %d cape vertices had 5)' % (trimmed, cp.N)),
        joints=dict(count=bc, node_index='joint b = node b+1', names='j00..j%02d' % (bc - 1),
                    parent=[int(p) for p in sk['parent']],
                    hierarchy_note='joints are FLAT in the glb (children of the root); animation '
                                   'channels carry each joint\'s model-space TRS. Use parent[] only for '
                                   'debugging/IK, not for re-parenting nodes (non-uniform scale '
                                   'inheritance is not a standard hierarchy)',
                    bind_world_colmajor=[np.array(M).T.reshape(16).tolist() for M in Wb]),
        clips={c: dict(frames=d['nf'], keys=d['nf'] + 1, duration_s=d['nf'] / FPS,
                       wrap_jump=round(d['loop_err'], 4), frame_step=round(d['step'], 4),
                       keys_note='keys at frames 0..nf-1 + a closing key at nf = frame 0',
                       root_motion_per_frame_glb=[toY(r) for r in d['root']],
                       root_motion_per_loop_glb=toY(np.array(d['root'][-1]) * d['nf'] / max(d['nf'] - 1, 1)),
                       speed_units_per_frame=round(float(np.linalg.norm(d['root'][-1])) / max(d['nf'] - 1, 1), 4))
               for c, d in clips.items()},
        fps=dict(assumed=FPS, note='1 clip frame per game tick at 60 Hz is ASSUMED (HANDOFF: unproven; '
                                   'cloth notes suggest ~2 clip frames per 3 vsyncs = 40 fps). '
                                   'Use AnimationAction.timeScale to change.'),
        attach=GLOW_GUN, bind56_world_colmajor=np.array(Wb[56]).T.reshape(16).tolist(),
        source=dict(ref_run=f"export_p000_gpu2.py --clip {REF_CLIP[0]} --frame {REF_CLIP[1]} --in-place",
                    clip_container=os.path.basename(CLIP_CONT)))
    json.dump(side, open(os.path.join(out, 'vincent.json'), 'w'), indent=1)
    print("wrote", os.path.join(out, 'vincent.json'))
    for c, d in clips.items():
        print(c, "frames", d['nf'], "wrap jump", round(d['loop_err'], 3), "vs frame step", round(d['step'], 3),
              "speed/frame", side['clips'][c]['speed_units_per_frame'], "root at nf-1 (glb)",
              [round(x, 3) for x in toY(d['root'][-1])])


# ---------------------------------------------------------------- verify
def skin_glb(path, clip, frame):
    """Skin the glb's own data at an integer clip frame (keys sit on frames), model space."""
    j, acc = read_glb(path)
    an = next(a for a in j['animations'] if a['name'] == clip)
    sk = j['skins'][0]; ibm = acc(sk['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1)
    node2j = {n: k for k, n in enumerate(sk['joints'])}
    T = {}; Q = {}; S = {}
    for ch in an['channels']:
        s = an['samplers'][ch['sampler']]; out = acc(s['output']).astype(float)
        tm = acc(s['input']); f = int(np.argmin(np.abs(tm - frame / FPS)))
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
    res = {}
    for f in frames:
        run_ref(dump, chk, clip, f)
        ref = read_mesh(os.path.join(chk, "vincent_p000.mesh"))[:, :3].astype(float)
        X = skin_glb(glb, clip, f)
        e = np.linalg.norm(X - ref, axis=1)
        r = dict(body_median=float(np.median(e[:nb])), body_max=float(e[:nb].max()),
                 cape_median=float(np.median(e[nb:])), cape_max=float(e[nb:].max()))
        res[f] = {k: round(v, 6) for k, v in r.items()}
        print(f"{os.path.basename(glb)} {clip} f{f}: ", res[f])
    return res


def main(argv):
    if len(argv) < 2 or argv[0] not in ('export', 'verify'): sys.exit(__doc__)
    mode, dump = argv[0], argv[1]; out = DEFAULT_OUT; glb = None; clip = 'h_wk'; frames = [0, 13, 27]
    mutate = False; i = 2
    while i < len(argv):
        k = argv[i]
        if k == '--out': out = os.path.abspath(argv[i + 1]); i += 2
        elif k == '--glb': glb = argv[i + 1]; i += 2
        elif k == '--clip': clip = argv[i + 1]; i += 2
        elif k == '--frames': frames = [int(x) for x in argv[i + 1].split(',')]; i += 2
        elif k == '--mutate-parents': mutate = True; i += 1
        else: sys.exit(f"unknown option {k}")
    if mode == 'export': export(dump, out, mutate)
    else: verify(dump, out, glb or os.path.join(out, 'vincent.glb'), clip, frames)


if __name__ == '__main__':
    main(sys.argv[1:])
