#!/usr/bin/env python3
"""smd_material.py -- texture + CLUT of every skinned-model triangle, read from the
game files (no GS dump).

Decoded from the game code:

1. Each smd draw DESCRIPTOR (group+0x08 array, render_skinned.enum_descs) starts with
   a material byte. FUN_005b2240 calls FUN_005b19b8(desc) before each descriptor's
   VIF run; FUN_005b19b8 writes TEX0 (GS reg 0x06) = *(u64 *)(table + desc[0]*0x10)
   (desc[0] == 0xff -> no texture; a zero entry falls back to slot 31 when obj+0x440
   bit 58 is set). desc +4 bit 1 turns on the alpha test (TEST 0x5364d = ATE GEQUAL
   AREF 100; else 0x51000); desc[2] (when != 0) picks an env-map slot (DAT_005b3b58
   = obj+0x451, the first texture entry with u16 +0x20 > 0xf).
   The table is the global at 0x5b37c0 (FUN_005ab8f0 / FUN_005abec0 ->
   FUN_005b2ef8 -> FUN_005b0ee0 param_2).

2. FUN_00259058 fills that table when the texture container (…2.bin) is uploaded.
   It walks the container's entries in order; each entry of type 'ipu' or 'rst'
   takes the NEXT slot, except an 'rst' whose u16 at +0x22 (top header +2) has bit
   `variant` set (variant = obj+0x44b, or (obj+0x440 >> 50) & 0xff for mode 2;
   0 for the sea-of-flames Vincent: the dump draws the p000_1_* textures).
   VRAM per entry (FUN_00414730): 4 blocks of CLUT if byte +0x2d != 0, then each
   image's block count (image header +0xc). TEX0 =
        TBP0 = image VRAM (entry start + 4 with a CLUT)
        TBW, PSM, TW, TH = image header +8, +2, +10, +11
        CBP  = the CLUT of the latest entry that had one (normally this entry)
        TCC = 1, CLD = 1, CPSM = CSM = CSA = 0 (32-bit CLUT, CSM1).
   An entry with 0 images (byte +0x2e) and byte +0x2f = k >= 0 is a PALETTE-ONLY
   entry: it reuses entry k's image (TBP/TBW/PSM/TW/TH) with its own CLUT. That is
   how p000_1_a (256x256 T8) gets 4 CLUTs: p000_1_a's own + p000_1_b/_c/_d.

In the file, the in-RAM image header is the RST image descriptor + 0x10 (descriptor
+0x12 PSM, +0x18 TBW, +0x1a TW, +0x1b TH, +0x1c size in blocks); the entry's top
header sits at entry+0x20 (u16 +0x20 kind, +0x22 variant mask, bytes +0x2d has-CLUT,
+0x2e image count, +0x2f parent entry).

The VRAM start (FUN_00414730's allocator) is runtime state: TEX0 values here are
relative to `base`. To compare with a capture, pass base = that capture's first
texture block (Vincent: body TBP - 4).
"""
import os, sys, struct, subprocess, tempfile, glob
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import render_skinned as R

DTEX = "/srv/games/dirge-port/native/assets/textures/dtex"
T_RST, T_IPU = 0x747372, 0x757069
BPP = {0x00: 32, 0x01: 24, 0x02: 16, 0x0a: 16, 0x13: 8, 0x14: 4, 0x1b: 8, 0x24: 4, 0x2c: 4}


def _u16(b, o): return struct.unpack_from('<H', b, o)[0]
def _u32(b, o): return struct.unpack_from('<I', b, o)[0]


def texture_entries(texpath):
    """The texture container's entries, in order, with what FUN_00259058 reads."""
    b = open(texpath, 'rb').read()
    n = _u16(b, 0x10); offs = [_u32(b, 0x14 + 4 * i) for i in range(n)]
    ents = []
    for i, o in enumerate(offs):
        e = dict(index=i, off=o, name=b[o:o + 8].rstrip(b'\0').decode('latin1'),
                 type=_u32(b, o + 8), size=_u32(b, o + 0xc))
        if e['type'] == T_RST:
            e.update(kind=_u16(b, o + 0x20), mask=_u16(b, o + 0x22), has_clut=b[o + 0x2d] != 0,
                     nimg=b[o + 0x2e], parent=struct.unpack_from('<b', b, o + 0x2f)[0])
            # descriptor+data blocks after the 0x30-byte top header
            descs = []; p = o + 0x50
            while len(descs) < 8 and p + 0x40 <= len(b):
                psm, w, h, tbw = struct.unpack_from('<4H', b, p + 0x12)
                if not (0 < w <= 4096 and 0 < h <= 4096) or psm not in BPP:
                    break
                descs.append(dict(off=p, psm=psm, w=w, h=h, tbw=tbw, tw=b[p + 0x1a], th=b[p + 0x1b],
                                  blocks=_u32(b, p + 0x1c), data=p + 0x40, nbytes=w * h * BPP[psm] // 8))
                p += 0x40 + w * h * BPP[psm] // 8
            k = 1 if e['has_clut'] else 0
            e['clut'] = descs[0] if e['has_clut'] and descs else None
            e['images'] = descs[k:k + e['nimg']]
        ents.append(e)
    return ents


def material_table(texpath, variant=0, base=0):
    """FUN_00259058: slot -> material (texture entry, palette entry, TEX0 fields)."""
    ents = texture_entries(texpath)
    mats = []; vram = base; cbp = 0; tbp_of = {}
    for e in ents:
        if e['type'] == T_IPU:
            mats.append(dict(slot=len(mats), entry=e['index'], name=e['name'], ipu=True))
            continue
        if e['type'] != T_RST or (e['mask'] >> (variant & 0x1f)) & 1:
            continue
        start = vram
        vram += (4 if e['has_clut'] else 0) + sum(im['blocks'] for im in e['images'])
        if e['has_clut']:
            cbp = start
        tbp = start + (4 if e['has_clut'] else 0)
        img_ent = e
        if e['nimg'] == 0 and 0 <= e['parent'] < len(ents):
            img_ent = ents[e['parent']]; tbp = tbp_of.get(img_ent['index'], tbp)
        tbp_of[e['index']] = tbp
        im = img_ent['images'][0] if img_ent.get('images') else None
        f = dict(tbp0=tbp, tbw=im['tbw'] if im else 0, psm=im['psm'] if im else 0,
                 tw=im['tw'] if im else 0, th=im['th'] if im else 0, cbp=cbp, cpsm=0, csa=0, tcc=1)
        tex0 = (f['tbp0'] | f['tbw'] << 14 | f['psm'] << 20 | f['tw'] << 26 | f['th'] << 30
                | 1 << 34 | f['cbp'] << 37 | 1 << 61)
        mats.append(dict(slot=len(mats), entry=e['index'], name=e['name'], image_entry=img_ent['index'],
                         image_name=img_ent['name'], clut_entry=e['index'] if e['has_clut'] else None,
                         envmap=e['kind'] > 0xf, fields=f, tex0=tex0))
    return mats


def texture_rgba(texpath, mat):
    """RGBA uint8 [H,W,4] of a material: its image entry decoded by dtex with the
    material's own CLUT swapped in (alpha is dtex's a*255/128)."""
    b = bytearray(open(texpath, 'rb').read()); ents = texture_entries(texpath)
    ie = ents[mat['image_entry']]; ce = ents[mat['clut_entry']] if mat.get('clut_entry') is not None else None
    blocks = ([ie['clut']] if ie['clut'] else []) + ie['images']
    end = max(d['data'] + d['nbytes'] for d in blocks)       # (+0xc is not the payload size)
    pay = bytearray(b[ie['off'] + 0x10:end])
    if ce is not None and ce['index'] != ie['index'] and ie['clut'] is not None:
        src, dst = ce['clut'], ie['clut']
        assert src['nbytes'] == dst['nbytes']
        o = dst['data'] - (ie['off'] + 0x10)
        pay[o:o + dst['nbytes']] = b[src['data']:src['data'] + src['nbytes']]
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, 'm.bin'); open(p, 'wb').write(bytes(pay))
        subprocess.run([DTEX, p, '-o', td], capture_output=True, timeout=60, check=True)
        pngs = glob.glob(os.path.join(td, '*.png'))
        if len(pngs) != 1:
            raise RuntimeError(f"dtex gave {len(pngs)} images for {mat['name']}")
        return np.asarray(Image.open(pngs[0]).convert('RGBA'), np.uint8)


def descriptor_materials(buf, NB, Wm, bc):
    """Per triangle, in render_p005.decode_groups order: (material byte, desc +4 flags,
    'smd.gN', descriptor index). Same walk and same triangle filter as decode_groups;
    callers should check the vertex indices against it (tri_materials does)."""
    n = R._u16(buf, 0x10); offs = [R._u32(buf, 0x14 + 4 * i) for i in range(n)]
    smdblk = [(buf[o:o + 8].rstrip(b'\0').decode('latin1'), o + 0x10)
              for o in offs if buf[o + 8:o + 11] == b'smd']
    out = []; vbase = 0
    for name, P in smdblk:
        if R._u32(buf, P) != R.SM2_MAGIC:
            continue
        sc = R._f32(buf, P + 0x2c) / 2048.0
        grparr = R._u32(buf, P + 0x20); gc = R._u16(buf, P + 0x0a)
        for gi in range(gc):
            G = P + R._u32(buf, P + grparr + 4 * gi)
            r = R.build_group_pool(buf, NB, Wm, bc, P, G, sc)
            if r is None:
                continue
            _pool, _n, seen = r; psize = len(seen)
            descs = R.enum_descs(buf, NB, P, G)
            for di, (doff, vif) in enumerate(descs):
                end = (P + descs[di + 1][1]) if di + 1 < len(descs) else NB
                mat = buf[P + doff]; flags = R._u16(buf, P + doff + 4)
                for sub in R.parse_drawpackets(buf, NB, P + vif, end):
                    idx = sub.get('idx')
                    if not idx:
                        continue
                    for ja, jb, jc in R.sub_tris(idx, sub.get('prim')):
                        a, b_, c = idx[ja], idx[jb], idx[jc]
                        if a == b_ or b_ == c or a == c:
                            continue
                        if max(a, b_, c) >= psize or not (seen[a] and seen[b_] and seen[c]):
                            continue
                        out.append((mat, flags, f"{name}.g{gi}", di, (vbase + a, vbase + b_, vbase + c)))
            vbase += psize
    return out


def tri_materials(buf, NB, Wm, bc, tris):
    """[(material byte, flags)] aligned to decode_groups' `tris`; raises if the walk
    disagrees with the decode (different triangle list)."""
    dm = descriptor_materials(buf, NB, Wm, bc)
    if len(dm) != len(tris) or any(tuple(int(k) for k in t[:3]) != d[4] for t, d in zip(tris, dm)):
        raise ValueError(f"descriptor walk ({len(dm)} tris) does not match the decode ({len(tris)})")
    return [(d[0], d[1]) for d in dm]


def block_material(buf, block_off, gi=0, di=0):
    """Material byte + flags of one descriptor of the smd block at file offset
    block_off (e.g. p000's cape block 0x19f70, which decode_groups skips)."""
    P = block_off + 0x10
    G = P + R._u32(buf, P + R._u32(buf, P + 0x20) + 4 * gi)
    doff = R.enum_descs(buf, len(buf), P, G)[di][0]
    return buf[P + doff], R._u16(buf, P + doff + 4)


# ---- GS-faithful software render (uncaptured models) ---------------------------
#
# What the game does per descriptor (FUN_005b19b8), checked against the p000 hero
# dump (every Vincent draw of the last frame):
#   flags & 2 == 0 -> opaque: ABE 0, no alpha test (TEST 0x51000)   [all p000 flag-0 draws]
#   flags & 2      -> ABE 1, ALPHA (Cs-Cd)*As+Cd (reg 0x42 = 0x44), TEST ATE GEQUAL
#                     AREF 100 AFAIL RGB_ONLY (0x5364d): every texel blends by As, only
#                     As >= 100 writes Z.                             [p000 hair p000_2: 135 of 216 tris]
#   TEX0 TFX MODULATE, TCC 1: As = At*Av/128 (Av = 0x80 for p005).
# Parts: FUN_005b2ef8 walks the part list last-to-first (the hero dump draws the hair
# part first); groups and descriptors in order. No culling: the strip winding is not
# consistent (render_skinned), and the GS has none; the hair is single-layer cards.

GS_AREF = 100


def model_scene(cont, texc, variant=0):
    """Decode + bind a skinned model from the files. Returns dict(V, N, tris, mat, flags,
    order, atl={slot: RGBA float, alpha in GS 0..128}, mats, gm)."""
    import render_p005 as P5
    from skd_skeleton import load_skeleton
    buf, NB = R.load_container(cont)
    sk = load_skeleton(cont, trans_scale=1.0); bc = sk['bone_count']
    Wm = [np.array(m, float) for m in sk['world_bind']]
    V, N, tris, gm = P5.decode_groups(buf, NB, Wm, bc)
    tm = tri_materials(buf, NB, Wm, bc, tris)
    mats = material_table(texc, variant)
    blocks = sorted({gm[t[6]][0].split('.')[0] for t in tris}, key=lambda s: int(s.split('@')[1].split('.')[0], 16))
    bidx = {b: i for i, b in enumerate(blocks)}
    order = sorted(range(len(tris)), key=lambda i: (-bidx[gm[tris[i][6]][0].split('.')[0]], i))
    atl = {}
    for m, _f in tm:
        if m != 0xff and m < len(mats) and not mats[m].get('ipu') and m not in atl:
            a = texture_rgba(texc, mats[m]).astype(np.float32)
            a[:, :, 3] = np.round(a[:, :, 3] * 128 / 255)          # dtex a*255/128 -> GS 0..128
            atl[m] = a
    return dict(V=V, N=N, tris=tris, mat=[m for m, _ in tm], flags=[f for _, f in tm], order=order,
                atl=atl, mats=mats, gm=gm, blocks=blocks)


def render_gs(sc, path, angle, elev, center=None, radius=None, gs=True, W_=900, H_=1200,
              falsecol=None):
    """gs=True: two-sided, part order, per-descriptor alpha test + blend (see above).
    gs=False: render_p005.render_multi's behaviour (winding back-face cull, opaque, file
    order) on the same camera, for before/after. falsecol: {slot: rgb} flat colours."""
    import math
    V, N, tris = sc['V'], sc['N'], sc['tris']
    Vr = V.copy(); Vr[:, 1] *= -1.0
    c = np.median(Vr, 0) if center is None else np.array([center[0], -center[1], center[2]], float)
    rad = (np.percentile(np.linalg.norm(Vr - c, axis=1), 98) * 1.7) if radius is None else radius
    az, el = math.radians(angle), math.radians(elev)
    eye = c + rad * np.array([math.cos(el) * math.sin(az), math.sin(el), math.cos(el) * math.cos(az)])
    fdir = c - eye; fdir /= np.linalg.norm(fdir)
    s = np.cross(fdir, [0, 1.0, 0]); s /= np.linalg.norm(s); u = np.cross(s, fdir)
    basis = np.array([s, u, -fdir]); Vt = (Vr - eye) @ basis.T
    Nr = N.copy(); Nr[:, 1] *= -1.0; Nview = Nr @ basis.T
    fl = 1 / math.tan(math.radians(40) / 2); aspect = W_ / H_
    img = np.full((H_, W_, 3), 18, np.float32); zb = np.full((H_, W_), 1e9)
    L = np.array([0.3, 0.6, 0.7]); L /= np.linalg.norm(L)
    for i in (sc['order'] if gs else range(len(tris))):
        t = tris[i]; m = sc['mat'][i]
        if m not in sc['atl']:
            continue
        p = Vt[[t[0], t[1], t[2]]]
        if np.any(p[:, 2] > -0.02):
            continue
        gnrm = np.cross(p[1] - p[0], p[2] - p[0]); gn = np.linalg.norm(gnrm)
        if gn < 1e-9 or (not gs and gnrm[2] > 0):
            continue
        fn = Nview[[t[0], t[1], t[2]]].mean(0); fnn = np.linalg.norm(fn)
        fn = fn / fnn if fnn > 1e-9 else gnrm / gn
        sh = abs(np.dot(fn, L)) * 0.55 + 0.45
        pr = np.array([((fl / aspect * q[0] / -q[2] * 0.5 + 0.5) * W_,
                        (0.5 - fl * q[1] / -q[2] * 0.5) * H_, -q[2]) for q in p])
        minx, maxx = max(int(pr[:, 0].min()), 0), min(int(pr[:, 0].max()) + 1, W_)
        miny, maxy = max(int(pr[:, 1].min()), 0), min(int(pr[:, 1].max()) + 1, H_)
        if maxx <= minx or maxy <= miny:
            continue
        (x0, y0), (x1, y1), (x2, y2) = pr[0, :2], pr[1, :2], pr[2, :2]
        d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if abs(d) < 1e-9:
            continue
        ys, xs = np.mgrid[miny:maxy, minx:maxx]; xf, yf = xs + 0.5, ys + 0.5
        aa = ((y1 - y2) * (xf - x2) + (x2 - x1) * (yf - y2)) / d
        bb = ((y2 - y0) * (xf - x2) + (x0 - x2) * (yf - y2)) / d; g = 1 - aa - bb
        cov = (aa >= 0) & (bb >= 0) & (g >= 0)
        if not cov.any():
            continue
        z = aa * pr[0, 2] + bb * pr[1, 2] + g * pr[2, 2]
        sub = zb[miny:maxy, minx:maxx]; ztest = cov & (z < sub)
        if not ztest.any():
            continue
        tex = sc['atl'][m]; TH, TW = tex.shape[:2]
        uv = np.array(t[3:6], float) / 4096.0
        uu = aa * uv[0, 0] + bb * uv[1, 0] + g * uv[2, 0]; vv = aa * uv[0, 1] + bb * uv[1, 1] + g * uv[2, 1]
        tx = np.clip((np.mod(uu, 1.0) * TW).astype(int), 0, TW - 1)
        ty = np.clip((np.mod(vv, 1.0) * TH).astype(int), 0, TH - 1)
        texel = tex[ty, tx]
        col = (np.broadcast_to(np.array(falsecol[m], np.float32), texel[..., :3].shape) if falsecol
               else texel[..., :3]) * sh
        block = img[miny:maxy, minx:maxx]
        if gs and sc['flags'][i] & 2:
            As = texel[..., 3] * (t[7][3] if len(t) > 7 else 128) / 128.0
            k = np.clip(As / 128.0, 0, 1)[..., None]
            block[ztest] = (col * k + block * (1 - k))[ztest]
            sub[ztest & (As >= GS_AREF)] = z[ztest & (As >= GS_AREF)]
        else:
            block[ztest] = col[ztest]; sub[ztest] = z[ztest]
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)
    return path


def head_view(sc, top_frac=0.16):
    """Center/radius framing the top `top_frac` of the figure (decode space is -Y up)."""
    V = sc['V'][sorted({k for t in sc['tris'] for k in t[:3]})]
    ymin, ymax = V[:, 1].min(), V[:, 1].max(); H = V[V[:, 1] < ymin + (ymax - ymin) * top_frac]
    return H.mean(0), float(np.ptp(H, 0).max() * 1.6)


# ---- demo: a model with no capture --------------------------------------------

def render_from_file(cont, texc, outdir, tag, variant=0):
    """Bind-pose software render of a skinned model with file-derived textures, drawn
    the way the game does (render_gs: two-sided, part order, per-descriptor alpha
    test + blend). Full figure front/back + head close-up front/side."""
    import collections
    sc = model_scene(cont, texc, variant)
    os.makedirs(outdir, exist_ok=True)
    use = collections.Counter(m for m in sc['mat'])
    mats = sc['mats']
    info = dict(tris=len(sc['tris']), bound=sum(1 for m in sc['mat'] if m in sc['atl']),
                alpha_tested=sum(1 for f in sc['flags'] if f & 2),
                slots={m: f"{mats[m]['image_name']}+clut({mats[m]['name']}) x{n}" for m, n in sorted(use.items())
                       if m < len(mats)})
    c, r = head_view(sc); paths = []
    for nm, ang, el, kw in (('front', 198, 6, {}), ('back', 18, 6, {}),
                            ('head_front', 198, 4, dict(center=c, radius=r, W_=600, H_=600)),
                            ('head_side', 120, 4, dict(center=c, radius=r, W_=600, H_=600))):
        paths.append(render_gs(sc, os.path.join(outdir, f"{tag}_fromfile_{nm}.png"), ang, el, **kw))
    return info, paths


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == '--table':
        for m in material_table(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0):
            print(m['slot'], m['name'], m.get('image_name'), m.get('fields'))
    elif len(sys.argv) >= 5 and sys.argv[1] == '--render':
        print(render_from_file(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5] if len(sys.argv) > 5 else 'model'))
    else:
        sys.exit("usage: smd_material.py --table TEX.bin [variant] | --render CONT.bin TEX.bin OUTDIR [tag]")
