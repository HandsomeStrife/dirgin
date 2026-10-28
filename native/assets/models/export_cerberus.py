#!/usr/bin/env python3
"""Export Vincent's gun (Cerberus) in Vincent's model space for one capture, attached
the way the game attaches it, and validate it against the capture's GS dump.

Gun: extract/_KEL/noPath/07770581.bin ("models" container: marker.mkd, smd parts
frame0-2 / topmt0-1 / barrel0-5 / sidemt0-1, own 4-bone skeleton.skd), textures
07770582.bin (3 x 128x128). Decoded with the shared smd path (render_p005.decode_groups,
bound to the gun's own bind skeleton). Every part is decoded; the parts the game draws
are picked per capture from the dump (UV-triple matches, then geometry where two parts
share UVs, e.g. barrel1/barrel4).

Attach (decoded, not fitted):
  * FUN_00316270 (equip weapon in slot N): FUN_005a7698(player, weapon, N + 0xf, N)
    stores the weapon at player+1000+4N and its MARKER index N+0xf at player+0x45e+N,
    and resets the weapon's local matrix (FUN_005a6e98(0,0,0, weapon+0x10)).
    Slot 0 (the gun) -> marker 15. (FUN_004096f8 / FUN_003471c8 use 0xf / 0x10 too.)
  * FUN_005ab8f0 (character draw), right after the body (FUN_005b2ef8), for each of the
    4 attach slots: M = FUN_005ad508(player, marker) = FUN_0030bef8 = palette[bone] x
    T(marker xyz); child world (child+0x50) = player world (player+0x10) x M; then the
    child is drawn (FUN_005abec0) with the player's light rig and obj+0x450 colour scale.
  * mk2 (marker.mkd): payload +0x04 u16 count, +0x08 -> records of 16 bytes
    {f32 x, y, z; s32 bone} (FUN_0030bef8 / FUN_005ad260 read rec+0xc as the bone).
    Vincent (07700001.bin) marker 15 = bone 56, offset (0,0,0). Bone 56 is a child of
    the right hand (55) with no skinned vertices: the weapon socket.
  So gun vertex in Vincent's model space = palette56 x gun_vertex (gun bind skinned).
  FUN_0026125c(out, A, B) = A after B (out row r = sum_k B[r][k] * A row k).

Vincent's palette56 for a capture: the p000 export does not write bone matrices, so it
is recovered from the export's own output. vincent_p000.mesh holds the posed body in
model space, in binding.json triangle order (tex0 None = dropped); the rigid transform
bind -> posed of hand bone 55's vertices (p000_pose solves one rigid transform per bone)
poses palette56 = R55 x bind56 (bone 56's bind local transform under 55; the dump pose
has no rotation for a bone without vertices). Hook that would make this exact: have
export_p000_gpu2.py write p000_pose's per-bone rigid transforms (or the clip's composed
AR.compose matrices in --clip mode) to e.g. bones.json; this script would read it.

Outputs (outdir): cerberus.mesh (DMSH, Vincent model space, header bbox = Vincent's body
bbox so the renderer pivots it like the body), cerberus_tex.rgba (u32 w, h + RGBA,
textures stacked vertically), cerberus.json (parts, attach, validation).
Render: render_scene_textured ... -charprop OUT/cerberus.mesh OUT/cerberus_tex.rgba

Run: python3 -I export_cerberus.py <dump.bin> <p000_export_dir> <outdir> [--cam CAM.json]
     (--cam: the capture's _cam.json; default the hero camera)
"""
import sys, os, json, struct, array, collections, glob, subprocess, tempfile
_REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))  # repo root
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, _REPO + "/tools/gsdump/src")
import render_p005 as P
import char_dumpbind
char_dumpbind.ROUND = 4096                      # native UV grid (see export_p000_gpu2.py)
from char_dumpbind import tri_sig
from skd_skeleton import load_skeleton
from render_p000_bound import CONT as VIN_CONT
import p000_pose as PZ
import p000_cape as C
from gsparse import read_header
from gsrender2 import build, tex0_fields, TexCache

KEL = _REPO + "/extract/_KEL/noPath/"
GUN_CONT = KEL + "07770581.bin"
GUN_TEX = KEL + "07770582.bin"
DTEX = _REPO + "/native/assets/textures/dtex"
WEAPON_SLOT = 0
MARKER = WEAPON_SLOT + 0xf                       # FUN_00316270
UVQ = 4096


# ---- containers -------------------------------------------------------------------
def entries(buf):
    n = P.R._u16(buf, 0x10)
    return [P.R._u32(buf, 0x14 + 4 * i) for i in range(n)]


def markers(buf):
    """mk2 records [(xyz, bone)] (FUN_0030bef8)."""
    for o in entries(buf):
        if buf[o + 8:o + 11] == b'mkd':
            p = o + 0x10
            assert buf[p:p + 4] == b'mk2\0'
            n = P.R._u16(buf, p + 4); rec = p + P.R._u32(buf, p + 8)
            return [(np.array(struct.unpack_from('<3f', buf, rec + 16 * i)),
                     struct.unpack_from('<i', buf, rec + 16 * i + 12)[0]) for i in range(n)]
    raise ValueError("no marker.mkd")


def rigid(A, B):
    """R (3x3), t: B ~= R A + t (Kabsch, no scale)."""
    ca, cb = A.mean(0), B.mean(0)
    U, _s, Vt = np.linalg.svd((B - cb).T @ (A - ca))
    D = np.diag([1, 1, np.sign(np.linalg.det(U @ Vt))])
    Rm = U @ D @ Vt
    return Rm, cb - Rm @ ca


def read_mesh(path):
    raw = open(path, 'rb').read()
    _m, _v, n = struct.unpack_from('<4sII', raw, 0); bb = struct.unpack_from('<6f', raw, 12)
    return np.frombuffer(raw, '<f4', offset=36).reshape(n, 12), np.array(bb[:3]), np.array(bb[3:])


# ---- Vincent: posed palette for the attach bone -------------------------------------
def vincent_socket(p000_dir, bone):
    """palette[bone] (4x4, Vincent model space) for the export's pose + stats."""
    buf, NB = P.R.load_container(VIN_CONT); sk = load_skeleton(VIN_CONT, trans_scale=1.0)
    bc = sk['bone_count']; Wm = [np.array(m, float) for m in sk['world_bind']]
    V, _N, tris, gm = P.decode_groups(buf, NB, Wm, bc)
    vb = PZ.vertex_bones(buf, NB, bc, gm)
    bind = json.load(open(os.path.join(p000_dir, 'binding.json')))
    mesh, bbmin, bbmax = read_mesh(os.path.join(p000_dir, 'vincent_p000.mesh'))
    if len(bind) != len(tris) or any(b['v'] != [int(k) for k in t[:3]] for b, t in zip(bind, tris)):
        raise ValueError("binding.json does not match this decode")
    posed = {}; k = 0
    for b in bind:
        if b['tex0'] is None: continue
        for c in range(3): posed[b['v'][c]] = mesh[k + c, :3]
        k += 3
    # the bone that carries the socket: itself if it has vertices, else nearest ancestor
    src = bone
    while sum(1 for v in posed if vb.get(v) == src) < 4:
        src = sk['parent'][src]
    vs = [v for v in posed if vb.get(v) == src]
    A = V[vs].astype(float); B = np.array([posed[v] for v in vs])
    Rm, t = rigid(A, B)
    res = np.linalg.norm(A @ Rm.T + t - B, axis=1)
    R4 = np.eye(4); R4[:3, :3] = Rm; R4[:3, 3] = t
    return R4 @ Wm[bone], dict(bone=bone, solved_from=int(src), vertices=len(vs),
                               fit_rms=round(float(np.sqrt((res ** 2).mean())), 4)), (bbmin, bbmax)


# ---- gun ----------------------------------------------------------------------------
def descriptor_tri_limits(buf, NB, Wm, bc):
    """{('part.gN', descriptor index): triangles inside the descriptor's own VIF run}.
    FUN_005b2240 sends each draw descriptor as a DMA ref tag: qwc = u16 at desc+6,
    address = desc+0x14 (the VIF pointer), so the run is exactly qwc quadwords (on the
    gun, next VIF = VIF + 16 x (qwc + 2): the 2 are the next descriptor's header).
    render_skinned.parse_drawpackets is given the next descriptor's VIF, or EOF for a
    group's last one; on the gun 8 last descriptors then read on into the next part's
    packets (frame0.g1 d2: 18 tris, 2 its own; the other 16 carry topmt0's UV triples on
    frame0's vertices, the same-place twins that z-fought). Same walk and filter as
    smd_material.descriptor_materials. Vincent's p000: no change (2389 tris either way)."""
    R = P.R
    n = R._u16(buf, 0x10); offs = [R._u32(buf, 0x14 + 4 * i) for i in range(n)]
    lim = {}
    for o in offs:
        if buf[o + 8:o + 11] != b'smd': continue
        Pp = o + 0x10
        if R._u32(buf, Pp) != R.SM2_MAGIC: continue
        name = buf[o:o + 8].rstrip(b'\0').decode('latin1')
        sc = R._f32(buf, Pp + 0x2c) / 2048.0
        grparr = R._u32(buf, Pp + 0x20)
        for gi in range(R._u16(buf, Pp + 0x0a)):
            G = Pp + R._u32(buf, Pp + grparr + 4 * gi)
            r = R.build_group_pool(buf, NB, Wm, bc, Pp, G, sc)
            if r is None: continue
            _pool, _n, seen = r; psize = len(seen)
            for di, (doff, vif) in enumerate(R.enum_descs(buf, NB, Pp, G)):
                end = Pp + vif + 16 * R._u16(buf, Pp + doff + 6)        # FUN_005b2240: ref qwc
                k = 0
                for sub in R.parse_drawpackets(buf, NB, Pp + vif, end):
                    idx = sub.get('idx')
                    if not idx: continue
                    for ja, jb, jc in R.sub_tris(idx, sub.get('prim')):
                        a, b_, c = idx[ja], idx[jb], idx[jc]
                        if a == b_ or b_ == c or a == c: continue
                        if max(a, b_, c) >= psize or not (seen[a] and seen[b_] and seen[c]): continue
                        k += 1
                lim[(f"{name}.g{gi}", di)] = k
    return lim


def decode_gun(with_materials=False):
    """The gun's triangles as the game draws them: decode_groups, minus the triangles
    the decoder reads past a descriptor's qwc (descriptor_tri_limits). with_materials:
    also [(material slot, descriptor +4 flags)] per triangle (smd_material; flag bit 1 =
    the GS ATE+ABE draw, FUN_005b19b8)."""
    import smd_material as SM
    buf, NB = P.R.load_container(GUN_CONT); sk = load_skeleton(GUN_CONT, trans_scale=1.0)
    Wm = [np.array(m, float) for m in sk['world_bind']]
    V, N, tris, gm = P.decode_groups(buf, NB, Wm, sk['bone_count'])
    dm = SM.descriptor_materials(buf, NB, Wm, sk['bone_count'])
    if len(dm) != len(tris) or any(tuple(int(k) for k in t[:3]) != d[4] for t, d in zip(tris, dm)):
        raise SystemExit("gun: descriptor walk does not match the decode")
    lim = descriptor_tri_limits(buf, NB, Wm, sk['bone_count'])
    cnt = collections.Counter(); keep = []
    for d in dm:
        cnt[(d[2], d[3])] += 1; keep.append(cnt[(d[2], d[3])] <= lim[(d[2], d[3])])
    tris = [t for t, k in zip(tris, keep) if k]
    mats = [(d[0], d[1]) for d, k in zip(dm, keep) if k]
    part = [g[0].split('@')[0].rstrip('_') for g in gm]
    if with_materials:
        return buf, V, N, tris, gm, part, mats
    return buf, V, N, tris, gm, part


def gun_textures():
    """The three 128x128 textures of 07770582.bin (dtex), in file order."""
    with tempfile.TemporaryDirectory() as td:
        subprocess.run([DTEX, GUN_TEX, "-o", td], capture_output=True, timeout=60, check=True)
        fs = sorted(glob.glob(os.path.join(td, "*.png")))
        return [os.path.basename(f) for f in fs], [np.asarray(Image.open(f).convert('RGBA')) for f in fs]


def uvn(t):
    return [(t[3 + k][0] / 4096, t[3 + k][1] / 4096) for k in range(3)]


def q(uv):
    return (int(round(uv[0] * UVQ)), int(round(uv[1] * UVQ)))


def dump_gun(dumppath, gsigs, cam):
    """Last-frame textured dump tris that belong to the gun: the TEX0s whose draws carry
    gun UV triples, within the index span of those matches. [(tex0, [(world, uv)]*3)]"""
    buf = open(dumppath, 'rb').read(); g = build(buf, read_header(buf))
    last = max(c['vframe'] for *_x, c in g.tris)
    cand = [(i, a, b, c, ctx) for i, (a, b, c, ctx) in enumerate(g.tris)
            if ctx['vframe'] == last and ctx['tme'] and not ctx['fst']]
    hit = [(i, ctx['tex0']) for i, a, b, c, ctx in cand
           if tri_sig([(v[3] / v[5], v[4] / v[5]) for v in (a, b, c)]) in gsigs]
    tex0s = {x for _i, x in hit}; lo = min(i for i, _x in hit); hi = max(i for i, _x in hit)
    out = []; first = {}
    for i, a, b, c, ctx in cand:
        if lo <= i <= hi and ctx['tex0'] in tex0s and all(abs(v[5]) > C.QMIN for v in (a, b, c)):
            first.setdefault(ctx['tex0'], i)
            out.append((ctx['tex0'], [(C.gs_to_world(v[0], v[1], v[5], ctx['xyoffset'], cam),
                                       (v[3] / v[5], v[4] / v[5])) for v in (a, b, c)]))
    tc = TexCache(dumppath, uploads=g.uploads)
    texs = {x: tc.get(x, first[x]) for x in tex0s}
    return out, texs, len(hit)


def world_to_model(Wp, pl, piv):
    y = np.radians(pl['yaw']); cy, sy = np.cos(y), np.sin(y)
    r = np.asarray(Wp, float) - np.array([pl['x'], pl['y'], pl['z']])
    return np.c_[cy * r[:, 0] - sy * r[:, 2], r[:, 1], sy * r[:, 0] + cy * r[:, 2]] / pl['s'] + piv


def main(argv):
    cam = dict(C.HERO_CAM); pos = []; i = 0
    while i < len(argv):
        if argv[i] == '--cam':
            j = json.load(open(argv[i + 1]))
            cam.update(eye=tuple(j['eye']), at=tuple(j['at']), up=tuple(j['up']), fov=j['fov']); i += 2
        else:
            pos.append(argv[i]); i += 1
    if len(pos) != 3: sys.exit(__doc__)
    dumppath, p000_dir, outdir = pos; os.makedirs(outdir, exist_ok=True)

    # Vincent's socket (marker 15) in his model space for this export's pose
    vm = markers(open(VIN_CONT, 'rb').read()); moff, mbone = vm[MARKER]
    pal, sock, (bbmin, bbmax) = vincent_socket(p000_dir, mbone)
    Msock = pal.copy(); Msock[:3, 3] = pal[:3, :3] @ moff + pal[:3, 3]      # palette x T(marker)
    pl = json.load(open(os.path.join(p000_dir, 'placement.json')))['placement']
    piv = np.array([(bbmin[0] + bbmax[0]) / 2, bbmax[1], (bbmin[2] + bbmax[2]) / 2])
    print("socket:", dict(sock, marker=MARKER, marker_offset=moff.tolist()))

    # gun, every part, in Vincent's model space
    _gb, GV, GN, gtris, gm, part = decode_gun()
    X = GV @ Msock[:3, :3].T + Msock[:3, 3]
    NX = GN @ Msock[:3, :3].T
    gsigs = collections.defaultdict(set)
    for t in gtris: gsigs[tri_sig(uvn(t))].add(part[t[6]])

    # dump gun tris -> Vincent model space
    dtris, dtex, nhit = dump_gun(dumppath, gsigs, cam)
    print(f"dump gun tris: {len(dtris)} ({nhit} with a gun UV triple), textures {len(dtex)}")
    # each dump corner -> our corners with the same UV (any part): nearest in 3D
    byuv = collections.defaultdict(list)
    for t in gtris:
        for k, uv in enumerate(uvn(t)): byuv[q(uv)].append((t[k], part[t[6]]))
    corners = []
    for t0, vs in dtris:
        for w, uv in vs:
            m = world_to_model(np.array([w]), pl, piv)[0]
            c = byuv.get(q(uv))
            if not c: continue
            d = [np.linalg.norm(X[v] - m) for v, _p in c]
            j = int(np.argmin(d))
            # a corner shared by two parts at the same place (barrel0's 4 tris are exact
            # copies of barrel1's) is no evidence for either: argmin gave it to the first
            # part, which is how barrel0 got selected though the game never draws it
            tie = any(p != c[j][1] and d[k] - d[j] < 1e-4 for k, (_v, p) in enumerate(c))
            corners.append((c[j][1], d[j] * pl['s'], t0, m, c[j][0], tie))
    won = collections.Counter(p for p, *_x, tie in corners if not tie)
    # parts the game draws: won corners; a part sharing all its UVs with a better-placed
    # part (barrel1/barrel4) only wins where it is nearer, so take the majority per UV set
    parts = sorted(p for p, n in won.items() if n >= 3)
    sel = [p for p in parts]
    err = np.array([e for p, e, *_x in corners if p in sel])
    print("corners won per part:", dict(won))
    print(f"selected parts: {sel}")
    print(f"vertex error (world units, {len(err)} corners): median {np.median(err):.3f} "
          f"p90 {np.percentile(err, 90):.3f} max {err.max():.3f}")

    # texture per gun group: the dump TEX0 its UV triples were drawn with -> file texture
    names, files = gun_textures()
    tex_of = {}
    for x, im in dtex.items():
        a = np.asarray(im[:, :, :3], float)
        dd = [np.abs(a - f[:a.shape[0], :a.shape[1], :3]).mean() if f.shape[:2] == a.shape[:2] else 1e9 for f in files]
        tex_of[x] = int(np.argmin(dd))
        print(f"dump tex0 tbp {tex0_fields(x)['tbp0']:#x} -> {names[tex_of[x]]} (mean |d| {min(dd):.2f})")
    dsig = {}
    for t0, vs in dtris: dsig[tri_sig([uv for _w, uv in vs])] = t0
    gvote = collections.defaultdict(collections.Counter)
    for t in gtris:
        x = dsig.get(tri_sig(uvn(t)))
        if x is not None: gvote[t[6]][tex_of[x]] += 1
    pvote = collections.defaultdict(collections.Counter)
    for g, v in gvote.items(): pvote[part[g]].update(v)

    def group_tex(g):
        v = gvote.get(g) or pvote.get(part[g])
        return v.most_common(1)[0][0] if v else 0

    # atlas: the three textures stacked; vertex alpha / colour from the file
    H = sum(f.shape[0] for f in files); W = max(f.shape[1] for f in files)
    atlas = np.zeros((H, W, 4), np.uint8); yo = []; y = 0
    for f in files:
        atlas[y:y + f.shape[0], :f.shape[1]] = f; yo.append(y); y += f.shape[0]
    atlas[:, :, 3] = 255                 # gun draws are opaque (body-like); see cerberus.json
    flat = []; ntri = 0; gtex = {}
    for t in gtris:
        if part[t[6]] not in sel: continue
        ti = gtex.setdefault(t[6], group_tex(t[6])); fh, fw = files[ti].shape[:2]
        for k in range(3):
            p = X[t[k]]; n = NX[t[k]]; ln = np.linalg.norm(n) or 1.0; uv = uvn(t)[k]; col = t[7 + k]
            flat.extend((p[0], p[1], p[2], n[0] / ln, n[1] / ln, n[2] / ln,
                         uv[0] * fw / W, (yo[ti] + uv[1] * fh) / H,
                         col[0] / 255.0, col[1] / 255.0, col[2] / 255.0, col[3] / 255.0))
        ntri += 1
    hdr = struct.pack('<4sII', b'DMSH', 1, ntri * 3) + struct.pack('<6f', *bbmin, *bbmax)
    with open(os.path.join(outdir, 'cerberus.mesh'), 'wb') as f:
        f.write(hdr); array.array('f', flat).tofile(f)
    with open(os.path.join(outdir, 'cerberus_tex.rgba'), 'wb') as f:
        f.write(struct.pack('<II', W, H)); f.write(atlas.tobytes())
    Image.fromarray(atlas[:, :, :3]).save(os.path.join(outdir, 'cerberus_tex.png'))
    perpart = {p: dict(corners=int(won[p]), median=round(float(np.median([e for pp, e, *_x in corners if pp == p])), 3))
               for p in won}
    info = dict(dump=os.path.basename(dumppath), p000_export=p000_dir, socket=sock, marker=MARKER,
                marker_offset=moff.tolist(), parts=sel, tris=ntri, dump_tris=len(dtris),
                group_texture={gm[g][0]: names[ti] for g, ti in gtex.items()},
                vertex_error=dict(median=round(float(np.median(err)), 3), p90=round(float(np.percentile(err, 90)), 3),
                                  max=round(float(err.max()), 3), corners=int(len(err))),
                per_part=perpart)
    json.dump(info, open(os.path.join(outdir, 'cerberus.json'), 'w'), indent=2)
    print(f"wrote {outdir}/cerberus.mesh ({ntri} tris) + cerberus_tex.rgba ({W}x{H}) + cerberus.json")


if __name__ == '__main__':
    main(sys.argv[1:])
