#!/usr/bin/env python3
"""Export an enemy character (default: the z128 soldier e027) as a skinned, animated glTF
binary for the web viewer, plus a JSON sidecar and a local-pose file in vincent_local.bin's
format. Game-derived output: write only into a gitignored directory (checked with
git check-ignore). Game files only, no GS dump.

  export:  python3 -I export_web_enemy.py export [--model e027] [--out DIR] [--clips r_gst,...]
  verify:  python3 -I export_web_enemy.py verify DUMP.bin [--model e027] [--png OUT.png]
           UV-triple match of the export against the dump's draws on the model's texture.

Files (FUN_00249cb8: id = 0x07000000 | letter<<16 | number<<6 | k): k=1 models.rfd
(skeleton + smd), k=2 texture.txd, k=0xb the rifle clip set m001 (r_*, idle r_gst).

Why a new parser (decoded 2026-10-29, worker 0115)
- e027's draw sub-packets are  STCYCL 0x0404, V4-32 x2 GIFtag, STCYCL 0x3000, V4-8 masked
  x48 @2, STCYCL 0x0102, V2-16 UV xN @2, V1-8 index xN @2, MSCNT.  Vincent's are the same
  except the colour: STCYCL 0x0102, V4-8 xN (data).
- STCYCL 0x3000 is CL=0, WL=48: VIF *filling* write. An UNPACK in filling mode (CL < WL)
  reads only CL elements per WL written, so this V4-8 x48 reads 0 bytes from the stream:
  its 48 quadwords come from the row/column registers through the mask (M bit set, cmd
  0x7e). render_skinned.parse_drawpackets ignores STCYCL and consumed 192 bytes there, ran
  into the UV data and stopped, so e027 decoded 1 triangle.
- parse_drawpackets_cycl tracks CL/WL and sizes every UNPACK by the VIF rule
  (CL >= WL: num elements; else (num // WL) * CL + min(num % WL, CL)). On Vincent's
  07703201 it returns the same sub-packets as parse_drawpackets (checked by `selftest`).
- Colour of a fill-mode V4-8 (decoded): FUN_005b0ee0, called by FUN_005b2ef8 before a
  model's smd groups, sends VIF1  STCOL 0x80,0x80,0x80,0x80  and  STMASK 0xaaaaaa0f. The
  masked V4-8 (cmd 0x7e) writes its quadwords from mask row min(cycle,3): row 0 = 0x0f
  (x,y protected, z,w data), rows 1-3 = 0xaa (all lanes = column register). The colour
  quadwords are cycles >= 1, so every vertex colour = (128,128,128,128) = 1.0 after /128.
  e027's streams carry no STCOL/STMASK of their own.
- Variants (LODs): FUN_005b2ef8 draws only the smd blocks whose s16 at smd +0x12 equals the
  object's byte +0x44b, walking the model's smd list from the LAST to the first
  (draw order). FUN_00259058 picks the textures with the same byte (smd_material variant):
  e027 var 0 = em027_l0 x2 + e027_1 (256x256), var 1/2 = e027_L01/S01 + e027lw_1
  (128x128). What sets +0x44b (distance?) is not decoded (notes/enemy-model.md).
- Everything else is the existing decode, imported unchanged: build_group_pool (skin pool),
  enum_descs (group+0x08 descriptors), sub_tris (GIFtag LIST/STRIP), the degenerate/
  seen-slot filter of render_skinned.assemble, slot_skin (export_web_vincent), material
  table/texture (smd_material, FUN_00259058 / FUN_005b19b8), clips (anim_rot).
"""
import sys, os, json, io, collections, struct
import numpy as np
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "../../.."))
sys.path.insert(0, HERE); sys.path.insert(0, REPO + "/tools/gsdump/src")
import render_skinned as R
import smd_material as M
import anim_rot as AR
import export_web_vincent as V
from skd_skeleton import load_skeleton

NOPATH = REPO + "/extract/_KEL/noPath/"
DEFAULT_OUT = os.path.join(REPO, "native/web/data/enemies")
FPS = V.FPS                     # ASSUMED, as for Vincent (1 clip frame per 60 Hz tick)
DEFAULT_CLIPS = ["r_gst"]       # z128 save state: every active e027 idles in r_gst / r_gst_lk


def file_id(model, k):
    """FUN_00249cb8: 'e027' -> 0x07000000 | ('e'-'a'... letter code)<<16 | number<<6 | k.
    The letter code is taken from the known ids (e -> 0x65, p -> 0x70: the ASCII byte)."""
    return 0x07000000 | (ord(model[0]) << 16) | (int(model[1:]) << 6) | k


def paths(model):
    f = lambda k: NOPATH + "%08x.bin" % file_id(model, k)
    return dict(cont=f(1), tex=f(2), clips=f(0xb))


# ---------------------------------------------------------------- decode
def parse_drawpackets_cycl(buf, NB, start, end):
    """render_skinned.parse_drawpackets with the VIF cycle register: STCYCL (cmd 0x01,
    imm = WL<<8 | CL) sets how many elements an UNPACK reads. Filling mode (CL < WL) reads
    (num // WL) * CL + min(num % WL, CL) elements; an UNPACK that reads none is recorded
    as sub['fill'][cmd] = num (written from the row/column registers, no file data)."""
    p = start; subs = []; cur = None; pending_prim = None; cl, wl = 1, 1
    while p < end and p + 4 <= NB:
        code = R._u32(buf, p); num = (code >> 16) & 0xff
        cmd = (code >> 24) & 0x7f
        if cmd == 0x01:                          # STCYCL
            cl = code & 0xff; wl = (code >> 8) & 0xff; p += 4
        elif cmd in (0x00, 0x04, 0x05, 0x10, 0x11, 0x13, 0x14, 0x15):
            p += 4
        elif cmd == 0x17:                        # MSCNT = end of a sub-packet
            if cur:
                subs.append(cur); cur = None
            pending_prim = None
            p += 4
        elif 0x60 <= cmd <= 0x7f:
            vn = (cmd >> 2) & 3; vl = cmd & 3; bits = {0: 32, 1: 16, 2: 8, 3: 16}[vl]
            nc = vn + 1
            n = num if (cl >= wl or wl == 0) else (num // wl) * cl + min(num % wl, cl)
            nb = nc * bits // 8 * n
            if p + 4 + nb > NB:
                break
            d = buf[p + 4:p + 4 + nb]
            if nc == 4 and bits == 32:
                pending_prim = R._giftag_prim(np.frombuffer(d, dtype='<u4'))
            else:
                if cur is None:
                    cur = {'prim': pending_prim}
                if nb == 0:
                    cur.setdefault('fill', {})[cmd] = num
                elif nc == 2 and bits == 16:
                    cur['uv'] = np.frombuffer(d, dtype='<i2').reshape(-1, 2).astype(float)
                elif nc == 4 and bits == 8:
                    cur['col'] = np.frombuffer(d, dtype=np.uint8).reshape(-1, 4)
                elif nc == 1 and bits == 8:
                    cur['idx'] = list(d)
            p += 4 + nb
            if p & 3:
                p = (p + 3) & ~3
        elif (p & 15) == 0 and ((code >> 28) & 7) in (1, 2) and (code & 0xffff) < 64:
            p += 16 + 16 * (code & 0xffff)       # inline DMA tag, as parse_drawpackets
        else:
            break
    if cur and cur.get('idx'):
        subs.append(cur)
    return subs


def smd_blocks(buf):
    n = R._u16(buf, 0x10); offs = [R._u32(buf, 0x14 + 4 * i) for i in range(n)]
    return [(buf[o:o + 8].rstrip(b'\0').decode('latin1'), o, o + 0x10)
            for o in offs if buf[o + 8:o + 11] == b'smd' and R._u32(buf, o + 0x10) == R.SM2_MAGIC]


def group_scale(buf, P, G):
    """Position scale of one smd group: the VU gets the float at the group's skin header
    (group +0x0c -> +4) with y = 32.0 (FUN_005b1260 for smd version > 3, FUN_005b16c0 for
    version 3; DAT_005b3bf8 = version > 3, FUN_005b0ee0), applied to the V3-16 positions as
    ITOF12 (/4096). Equals the header's +0x2c / 2048 on version-4 models (e027 3.33525,
    Cerberus 3.152425, p000 3.465913); version-3 smd (the soldiers' rifle w003) has no
    header float (+0x2c = 0x10), only this one (1.960384)."""
    return R._f32(buf, P + R._u32(buf, G + 0x0c) + 4) / 4096.0


def decode(buf, NB, sk, variant=None):
    """render_skinned.assemble's walk with parse_drawpackets_cycl. Returns
    (verts, normals, tris, groups, tri_desc). tris as assemble's (a, b, c, uv0..2 raw
    V2-16, col0..2); tri_desc[i] = (desc material byte, desc +4 flags, fill colour?).
    variant: only blocks with smd +0x12 == variant, in FUN_005b2ef8's order (last block
    first); None = every block in file order (for checks)."""
    bc = sk['bone_count']; Wm = [np.array(m, float) for m in sk['world_bind']]
    verts = []; norms = []; tris = []; groups = []; tdesc = []
    blocks = smd_blocks(buf)
    if variant is not None:
        blocks = [b for b in blocks[::-1] if struct.unpack_from('<h', buf, b[2] + 0x12)[0] == variant]
    for name, blk, P in blocks:
        grparr = R._u32(buf, P + 0x20); gc = R._u16(buf, P + 0x0a)
        for gi in range(gc):
            G = P + R._u32(buf, P + grparr + 4 * gi)
            sc = group_scale(buf, P, G)
            r = R.build_group_pool(buf, NB, Wm, bc, P, G, sc)
            if r is None:
                print(f"WARN {name}@{blk:#x}.g{gi}: no pool"); continue
            pool, nrmp, seen = r; base = len(verts); psize = len(pool)
            verts.extend(pool); norms.extend(nrmp); t0 = len(tris)
            descs = R.enum_descs(buf, NB, P, G)
            for di, (doff, vifptr) in enumerate(descs):
                end = (P + descs[di + 1][1]) if di + 1 < len(descs) else NB
                mat = buf[P + doff]; flags = buf[P + doff + 4]
                for sub in parse_drawpackets_cycl(buf, NB, P + vifptr, end):
                    idx = sub.get('idx'); uv = sub.get('uv'); col = sub.get('col')
                    fill = 0x7e in sub.get('fill', {}) or 0x6e in sub.get('fill', {})
                    for ja, jb, jc in R.sub_tris(idx, sub.get('prim')):
                        a, b, c = idx[ja], idx[jb], idx[jc]
                        if a == b or b == c or a == c:
                            continue
                        if max(a, b, c) >= psize or not (seen[a] and seen[b] and seen[c]):
                            continue
                        if uv is not None and len(uv) > max(ja, jb, jc):
                            uvs = (tuple(uv[ja]), tuple(uv[jb]), tuple(uv[jc]))
                        else:
                            uvs = ((0.0, 0.0),) * 3
                        tris.append((base + a, base + b, base + c) + uvs +
                                    tuple(R._corner_col(col, j) for j in (ja, jb, jc)))
                        tdesc.append((mat, flags, fill))
            groups.append(dict(name=f"{name}@{blk:#x}.g{gi}", P=P, G=G, sc=sc, base=base,
                               psize=psize, tris=len(tris) - t0))
            if len(tris) == t0:
                print(f"WARN {name}@{blk:#x}.g{gi}: 0 triangles")
    return np.array(verts), np.array(norms), tris, groups, tdesc


def skin(buf, NB, sk, groups, V0):
    """Per pool vertex: top-4 (joint, weight) from export_web_vincent.slot_skin (the
    FUN_005b1190 batch walk), asserting the weighted sum reproduces the decoded position."""
    bc = sk['bone_count']; Wb = [np.array(m, float) for m in sk['world_bind']]
    J4 = np.zeros((len(V0), 4), int); W4 = np.zeros((len(V0), 4)); st = dict(multi=0, trimmed=0, dropped_w=0.0)
    for g in groups:
        infl = V.slot_skin(buf, NB, bc, g['P'], g['G'], g['sc'])
        for s in range(g['psize']):
            l = infl[s]
            if not l: continue
            p = sum(w * (Wb[b][:3, :3] @ loc + Wb[b][:3, 3]) for b, loc, w in l)
            assert np.abs(p - V0[g['base'] + s]).max() < 1e-4, (g['name'], s)
            l = sorted(((w, b) for b, _loc, w in l), reverse=True)
            st['multi'] += len(l) > 1
            if len(l) > 4: st['trimmed'] += 1; st['dropped_w'] = max(st['dropped_w'], l[4][0])
            # GUESS: three.js skins with 4 joints; a 5th bone is dropped and the rest
            # renormalised (same stand-in as Vincent, notes/fidelity-audit.md)
            l = l[:4]; tot = sum(w for w, _ in l)
            for k, (w, b) in enumerate(l): J4[g['base'] + s, k] = b; W4[g['base'] + s, k] = w / tot
    return J4, W4, st


def load(model):
    pth = paths(model)
    buf, NB = R.load_container(pth['cont'])
    sk = load_skeleton(pth['cont'], trans_scale=1.0)      # as export_web_vincent.load
    return pth, buf, NB, sk


# ---------------------------------------------------------------- export
def variants(buf):
    return sorted({struct.unpack_from('<h', buf, P + 0x12)[0] for _n, _b, P in smd_blocks(buf)})


def export(model, out, clip_names, variant=0):
    """variant 0 -> MODEL.glb/json, else MODEL_vN.glb/json; plus MODEL_local.bin (skeleton +
    clips only: the same bytes for every variant)."""
    V.check_ignored(out); os.makedirs(out, exist_ok=True)
    pth, buf, NB, sk = load(model); bc = sk['bone_count']
    stem = model if variant == 0 else f"{model}_v{variant}"
    Wb = [np.array(m, float) for m in sk['world_bind']]
    V0, N0, tris, groups, tdesc = decode(buf, NB, sk, variant)
    J4, W4, sst = skin(buf, NB, sk, groups, V0)
    mats = M.material_table(pth['tex'], variant=variant)
    print(f"{stem}: {len(V0)} pool vertices, {len(tris)} triangles, {len(groups)} groups; skin {sst}")

    # unindexed per-corner, in decode (= file draw) order
    vi = np.array([t[:3] for t in tris]).reshape(-1)
    pos = V0[vi]; nrm = N0[vi]; joints = J4[vi]; weights = W4[vi]
    uv = np.array([u for t in tris for u in t[3:6]], float) / 4096.0
    fill = np.repeat([d[2] for d in tdesc], 3)
    raw = np.array([c for t in tris for c in t[6:9]], float)
    # fill-mode V4-8: FUN_005b0ee0's STCOL 0x80 x4 through STMASK 0xaaaaaa0f -> 128 = 1.0
    col = np.where(fill[:, None], 128.0 / 128.0, raw / 128.0)
    mat_bytes = sorted({d[0] for d in tdesc}); alpha_flags = sorted({d[1] & 2 for d in tdesc})
    if mat_bytes != [0] or len(mats) != 1:
        sys.exit(f"material bytes {mat_bytes} / {len(mats)} materials: only one material is handled")

    # clips (Vincent's per-frame world TRS on flat joints, in place = bone 1 x/z zeroed)
    clips = {}
    for c in clip_names:
        cbuf, CP = V.find_clip(pth['clips'], c); nf = AR.clip_frames(cbuf, CP)
        zy = V.y_moves(sk, pth['clips'], c); frames = []; root = []; Ws = []
        for f in range(nf):
            W = V.clip_mats(sk, c, f, cont=pth['clips'], zero_y=zy)
            frames.append([V.mat_to_trs(Mx) for Mx in W]); Ws.append(W)
            Wr = V.clip_mats(sk, c, f, root_motion=True, cont=pth['clips'])
            root.append((Wr[1][:3, 3] - W[1][:3, 3]).tolist())
        wrap = V.pose_jump(Ws[-1], Ws[0]) if nf > 1 else 0.0
        step_med = float(np.median([V.pose_jump(Ws[f], Ws[f + 1]) for f in range(nf - 1)])) if nf > 1 else 0.0
        loop = V.is_loop(wrap, step_med, nf)
        if loop: frames.append(list(frames[0]))
        for b in range(bc):
            for f in range(1, len(frames)):
                if np.dot(frames[f][b][1], frames[f - 1][b][1]) < 0:
                    t, q, s = frames[f][b]; frames[f][b] = (t, -q, s)
        clips[c] = dict(frames=frames, root=root, nf=nf, loop=loop, zero_y=zy, cont=pth['clips'],
                        loop_err=wrap, step_med=step_med)

    g = V.GLB(); j = g.j; j['asset']['generator'] = 'export_web_enemy.py'
    A = lambda *a, **k: g.acc(*a, **k)
    attrs = dict(POSITION=A(pos.astype('<f4'), 5126, 'VEC3', 34962, True),
                 NORMAL=A(nrm.astype('<f4'), 5126, 'VEC3', 34962),
                 TEXCOORD_0=A(uv.astype('<f4'), 5126, 'VEC2', 34962),
                 COLOR_0=A(col.astype('<f4'), 5126, 'VEC4', 34962),
                 JOINTS_0=A(joints.astype('<u1'), 5121, 'VEC4', 34962),
                 WEIGHTS_0=A(weights.astype('<f4'), 5126, 'VEC4', 34962))
    # draw runs: consecutive triangles with the same descriptor alpha flag (desc +4 bit 1 ->
    # TEST ATE GEQUAL AREF 100, FUN_005b19b8 via smd_material), in file draw order
    runs = []
    for i, d in enumerate(tdesc):
        a = bool(d[1] & 2)
        if runs and runs[-1][2] == a: runs[-1][1] += 1
        else: runs.append([i, 1, a])
    prims = [dict(attributes=attrs, indices=A(np.arange(s0 * 3, (s0 + n) * 3, dtype='<u2'), 5123, 'SCALAR', 34963),
                  mode=4, material=1 if a else 0,
                  extras=dict(drawOrder=o, firstTri=s0, triCount=n, alpha=a))
             for o, (s0, n, a) in enumerate(runs)]
    rgba = M.texture_rgba(pth['tex'], mats[0])
    png = io.BytesIO(); Image.fromarray(rgba, 'RGBA').save(png, 'PNG')
    j['images'] = [dict(bufferView=g.view(png.getvalue()), mimeType='image/png', name=mats[0]['name'])]
    j['samplers'] = [dict(magFilter=9729, minFilter=9729, wrapS=33071, wrapT=33071)]
    j['textures'] = [dict(source=0, sampler=0)]
    aref = 100 / 255.0
    base = lambda: dict(pbrMetallicRoughness=dict(baseColorTexture=dict(index=0), metallicFactor=0.0, roughnessFactor=1.0),
                        doubleSided=True, extensions=dict(KHR_materials_unlit={}))
    j['materials'] = [dict(name='opaque', alphaMode='OPAQUE', **base()),
                      dict(name='alpha', alphaMode='MASK', alphaCutoff=aref, **base())]
    j['extensionsUsed'] = ['KHR_materials_unlit']
    j['meshes'] = [dict(name=stem, primitives=prims)]
    j['nodes'] = [dict(name=f'{model}_root', rotation=[1.0, 0.0, 0.0, 0.0], translation=[0.0, 0.0, 0.0],
                       children=list(range(1, bc + 1)))]
    for b in range(bc):
        t, q, s = V.mat_to_trs(Wb[b])
        j['nodes'].append(dict(name=f'j{b:02d}', translation=t.tolist(), rotation=q.tolist(), scale=s.tolist(),
                               extras=dict(bone=b, parent=int(sk['parent'][b]))))
    ibm = np.stack([np.linalg.inv(Mx).T.reshape(16) for Mx in Wb]).astype('<f4')
    j['skins'] = [dict(name=f'{model}_skin', joints=list(range(1, bc + 1)), skeleton=0,
                       inverseBindMatrices=A(ibm, 5126, 'MAT4'))]
    j['nodes'].append(dict(name=f'{model}_mesh', mesh=0, skin=0))
    j['scenes'] = [dict(name=model, nodes=[0, bc + 1])]; j['scene'] = 0
    j['animations'] = V.add_anims(g, clips, bc, True)
    j['asset']['extras'] = dict(sidecar=f'{stem}.json')
    g.write(os.path.join(out, f'{stem}.glb'))
    toY = lambda v: [v[0], -v[1], -v[2]]
    side = dict(
        format=f"{stem}.glb: glTF 2.0 binary, one skinned mesh, flat joints (as vincent.glb)",
        glb=f'{stem}.glb', model=model, files={k: os.path.basename(v) for k, v in pth.items()},
        variant=dict(value=variant, all=variants(buf),
                     rule='FUN_005b2ef8: smd blocks with s16 +0x12 == object byte +0x44b, last block '
                          'first; textures by the same byte (FUN_00259058). Who sets +0x44b: not decoded'),
        units='game world units, unit scale', axes='as vincent.json: root node Rx 180 deg, model space -Y-up',
        vertices=len(pos), triangles=len(tris), pool_vertices=len(V0),
        groups=[dict(name=gr['name'], pool=gr['psize'], triangles=gr['tris']) for gr in groups],
        layout='unindexed per-corner vertices in file draw order; each primitive indexes a run',
        draw_runs=[dict(order=o, first_tri=s0, tri_count=n, alpha=a) for o, (s0, n, a) in enumerate(runs)],
        color='COLOR_0 = vertex colour / 128 (1.0 = neutral). Fill-mode V4-8 (no data): '
              'FUN_005b0ee0 STCOL 0x80 x4 + STMASK 0xaaaaaa0f -> 128 = 1.0',
        fill_mode_triangles=int(sum(d[2] for d in tdesc)),
        texture=dict(name=mats[0]['name'], size=[int(rgba.shape[1]), int(rgba.shape[0])],
                     tex0_rel={k: v for k, v in mats[0]['fields'].items()}),
        alpha=dict(cutoff=aref, game='desc +4 bit 1: ATE GEQUAL 100 (FUN_005b19b8)'),
        skin=dict(rule='FUN_005b1190 + VU0 0x120 (slot_skin), top 4 renormalised', **sst),
        joints=dict(count=bc, node_index='joint b = node b+1', parent=[int(p) for p in sk['parent']],
                    bind_world_colmajor=[np.array(Mx).T.reshape(16).tolist() for Mx in Wb]),
        clips={c: dict(frames=d['nf'], keys=len(d['frames']), loop=d['loop'], wrap_jump=round(d['loop_err'], 4),
                       median_step=round(d['step_med'], 4), in_place_axes='xyz' if d['zero_y'] else 'xz',
                       container=os.path.basename(d['cont']),
                       root_motion_per_frame_glb=[toY(r) for r in d['root']]) for c, d in clips.items()},
        fps=dict(assumed=FPS, note='as vincent.json (unproven)'),
        bone_count=bc, parent_sha1=V.parent_sha1(sk['parent']))
    side['local_pose'] = V.local_pose(sk, list(sk['parent']), clips, out, f'{model}_local.bin')
    json.dump(side, open(os.path.join(out, f'{stem}.json'), 'w'), indent=1)
    print("wrote", out, f"{stem}.glb/json: {len(pos)} corners, {len(tris)} tris, runs {len(runs)}, "
          f"clips " + ' '.join(f"{c}({d['nf']}f{' loop' if d['loop'] else ''})" for c, d in clips.items()))


# ---------------------------------------------------------------- weapon (attached model)
def socket(model, marker):
    """(bone, offset) of a character's marker (marker.mkd, export_cerberus.markers:
    FUN_0030bef8 reads rec+0xc as the bone)."""
    import export_cerberus as EC
    buf = open(paths(model)['cont'], 'rb').read()
    xyz, bone = EC.markers(buf)[marker]
    return int(bone), [float(x) for x in xyz]


def weapon_names(owner, slot):
    """{section-1 record: weapon name} for the owner's placements (FUN_00316058 slot name from
    the record's parameter set; none when the set's +0x92 bit 4 is set, FUN_00316270)."""
    import export_web_placements as PL
    stage = open(NOPATH + '06800001.bin', 'rb').read()
    (o0, c0, s0), (o1, c1, s1) = PL.sections(stage)[:2]
    out = {}
    for k in range(c1):
        ps = struct.unpack_from('<I', stage, o1 + k * s1)[0]
        p = o0 + ps * s0
        if stage[p + 0x20:p + 0x24].decode('latin1') != owner: continue
        if struct.unpack_from('<H', stage, p + 0x92)[0] & 0x10: continue
        nm = stage[p + 0x44 + 8 * slot:p + 0x4c + 8 * slot].split(b'\0')[0].decode('latin1')
        if nm: out[k] = nm
    return out


def export_weapons(owner, slot, out, variant=0):
    """Every weapon the owner's placements carry in this slot -> WEAPON.glb/json, plus
    OWNER_weapons.json {record: weapon}."""
    V.check_ignored(out); os.makedirs(out, exist_ok=True)
    recs = weapon_names(owner, slot)
    for nm in sorted(set(recs.values())):
        export_weapon(owner, slot, nm, out, variant)
    json.dump(dict(owner=owner, slot=slot, marker=slot + 0xf,
                   rule='FUN_00315e90 -> FUN_00316058: parameter set +0x44 (+8 x slot) -> FUN_00316270',
                   records={str(k): v for k, v in sorted(recs.items())}),
              open(os.path.join(out, f'{owner}_weapons.json'), 'w'), indent=1)
    print(f"wrote {owner}_weapons.json: {len(recs)} records, " +
          ' '.join(f"{n} x{list(recs.values()).count(n)}" for n in sorted(set(recs.values()))))


def export_weapon(owner, slot, weapon, out, variant=0):
    """An enemy's weapon model as a static glb in the owner's marker-(slot + 0xf) socket space.

    Which weapon (decoded): FUN_00315e90 (status, non-player: status +0x60 bit 0x200 clear)
    -> for slots 0-3, FUN_00316058 copies the name at parameter set (section 0) +0x44 / +0x4c /
    +0x54 / +0x5c; FUN_00316270 loads it (FUN_00408b30), resets its local matrix
    (FUN_005a6e98(0,0,0, w+0x10)) and attaches it with FUN_005a7698(owner MP, w, slot + 0xf,
    slot); nothing when parameter set +0x92 bit 4 is set. z128's soldiers (sets 27-32): slot 0
    'w003', slots 1-3 empty, +0x92 = 0.
    Where (decoded, as Vincent's gun, export_cerberus.py): FUN_005ab8f0 draws each attached
    child after the body with child world = owner world x FUN_005ad508 = palette[bone] x
    T(marker xyz), with the owner's light rig and colour scale. The weapon's own skeleton is
    not animated here (reset matrix), so a vertex in socket space = the weapon's bind-pose
    vertex (its own world bind; w003 is rigid, one bone per vertex).
    Variant: FUN_005b2ef8's smd +0x12 == the weapon object's +0x44b (GUESS 0, as e027's)."""
    bone, off = socket(owner, slot + 0xf)
    V.check_ignored(out); os.makedirs(out, exist_ok=True)
    pth, buf, NB, sk = load(weapon)
    V0, N0, tris, groups, tdesc = decode(buf, NB, sk, variant)
    J4, W4, sst = skin(buf, NB, sk, groups, V0)
    if sst['multi']:
        sys.exit(f"{weapon}: {sst['multi']} multi-bone vertices; only a rigid weapon is handled")
    mats = M.material_table(pth['tex'], variant=variant)
    mat_bytes = sorted({d[0] for d in tdesc})
    if mat_bytes != [0] or len(mats) != 1:
        sys.exit(f"{weapon}: material bytes {mat_bytes} / {len(mats)} materials: only one material is handled")
    vi = np.array([t[:3] for t in tris]).reshape(-1)
    pos = V0[vi] + np.array(off)                        # T(marker xyz) x bind vertex
    nrm = N0[vi]
    uv = np.array([u for t in tris for u in t[3:6]], float) / 4096.0
    fill = np.repeat([d[2] for d in tdesc], 3)
    raw = np.array([c for t in tris for c in t[6:9]], float)
    col = np.where(fill[:, None], 128.0 / 128.0, raw / 128.0)   # FUN_005b0ee0 STCOL 0x80 (as e027)
    runs = []
    for i, d in enumerate(tdesc):
        a = bool(d[1] & 2)
        if runs and runs[-1][2] == a: runs[-1][1] += 1
        else: runs.append([i, 1, a])
    g = V.GLB(); j = g.j; j['asset']['generator'] = 'export_web_enemy.py weapon'
    A = lambda *x, **k: g.acc(*x, **k)
    attrs = dict(POSITION=A(pos.astype('<f4'), 5126, 'VEC3', 34962, True),
                 NORMAL=A(nrm.astype('<f4'), 5126, 'VEC3', 34962),
                 TEXCOORD_0=A(uv.astype('<f4'), 5126, 'VEC2', 34962),
                 COLOR_0=A(col.astype('<f4'), 5126, 'VEC4', 34962))
    prims = [dict(attributes=attrs, indices=A(np.arange(s0_ * 3, (s0_ + n) * 3, dtype='<u2'), 5123, 'SCALAR', 34963),
                  mode=4, material=1 if a else 0, extras=dict(drawOrder=o, firstTri=s0_, triCount=n, alpha=a))
             for o, (s0_, n, a) in enumerate(runs)]
    rgba = M.texture_rgba(pth['tex'], mats[0])
    png = io.BytesIO(); Image.fromarray(rgba, 'RGBA').save(png, 'PNG')
    j['images'] = [dict(bufferView=g.view(png.getvalue()), mimeType='image/png', name=mats[0]['name'])]
    j['samplers'] = [dict(magFilter=9729, minFilter=9729, wrapS=33071, wrapT=33071)]
    j['textures'] = [dict(source=0, sampler=0)]
    base = lambda: dict(pbrMetallicRoughness=dict(baseColorTexture=dict(index=0), metallicFactor=0.0, roughnessFactor=1.0),
                        doubleSided=True, extensions=dict(KHR_materials_unlit={}))
    j['materials'] = [dict(name='opaque', alphaMode='OPAQUE', **base()),
                      dict(name='alpha', alphaMode='MASK', alphaCutoff=100 / 255.0, **base())]
    j['extensionsUsed'] = ['KHR_materials_unlit']
    j['meshes'] = [dict(name=weapon, primitives=prims)]
    j['nodes'] = [dict(name=f'{weapon}_mesh', mesh=0)]   # no root flip: socket space = game -Y-up
    j['scenes'] = [dict(name=weapon, nodes=[0])]; j['scene'] = 0
    stem = weapon
    g.write(os.path.join(out, f'{stem}.glb'))
    side = dict(format=f'{stem}.glb: one static mesh in the owner socket space (game -Y-up, no root flip)',
                owner=owner, slot=slot, weapon=weapon, marker=slot + 0xf, bone=bone, marker_offset=off,
                files={k: os.path.basename(v) for k, v in pth.items() if k != 'clips'},
                variant=dict(value=variant, all=variants(buf)),
                scale=[round(group_scale(buf, gr['P'], gr['G']), 9) for gr in groups],
                vertices=len(pos), triangles=len(tris),
                bbox=[pos.min(0).round(4).tolist(), pos.max(0).round(4).tolist()],
                texture=dict(name=mats[0]['name'], size=[int(rgba.shape[1]), int(rgba.shape[0])]),
                draw_runs=[dict(order=o, first_tri=s0_, tri_count=n, alpha=a) for o, (s0_, n, a) in enumerate(runs)])
    json.dump(side, open(os.path.join(out, f'{stem}.json'), 'w'), indent=1)
    print(f"wrote {out} {stem}.glb/json: {weapon} var {variant}, {len(tris)} tris, bone {bone} offset {off}, "
          f"bbox {side['bbox']}")


# ---------------------------------------------------------------- self-test
def selftest():
    """parse_drawpackets_cycl == parse_drawpackets on Vincent's 07703201 (every sub-packet)."""
    buf, NB = R.load_container(NOPATH + "07703201.bin"); ns = 0
    for name, blk, P in smd_blocks(buf):
        for gi in range(R._u16(buf, P + 0x0a)):
            G = P + R._u32(buf, P + R._u32(buf, P + 0x20) + 4 * gi)
            d = R.enum_descs(buf, NB, P, G)
            for di, (_o, vp) in enumerate(d):
                end = (P + d[di + 1][1]) if di + 1 < len(d) else NB
                a = R.parse_drawpackets(buf, NB, P + vp, end); b = parse_drawpackets_cycl(buf, NB, P + vp, end)
                assert len(a) == len(b), (name, gi, di)
                for x, y in zip(a, b):
                    assert x.get('prim') == y.get('prim') and x.get('idx') == y.get('idx') and 'fill' not in y
                    for k in ('uv', 'col'):
                        assert (x.get(k) is None) == (y.get(k) is None) and (x.get(k) is None or (x[k] == y[k]).all())
                ns += len(a)
    print(f"selftest: {ns} Vincent sub-packets identical")


if __name__ == '__main__':
    a = sys.argv[1:]
    opt = lambda k, d=None: a[a.index(k) + 1] if k in a else d
    if not a or a[0] == 'export':
        model = opt('--model', 'e027'); out = opt('--out', DEFAULT_OUT)
        cl = opt('--clips', ','.join(DEFAULT_CLIPS)).split(',')
        pth, buf, _NB, _sk = load(model)
        vs = [int(opt('--variant'))] if '--variant' in a else variants(buf)
        for v in vs:
            export(model, out, cl, v)
    elif a[0] == 'weapon':
        export_weapons(opt('--owner', 'e027'), int(opt('--slot', '0')), opt('--out', DEFAULT_OUT),
                       int(opt('--variant', '0')))
    elif a[0] == 'selftest':
        selftest()
    elif a[0] == 'verify':
        import enemy_dump_check
        enemy_dump_check.main(a[1:])
    else:
        sys.exit(__doc__)
