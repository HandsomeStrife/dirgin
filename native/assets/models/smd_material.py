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


# ---- demo: a model with no capture --------------------------------------------

def render_from_file(cont, texc, outdir, tag, variant=0):
    """Bind-pose software render of a skinned model with file-derived textures
    (render_p005.render_multi; one atlas per material slot)."""
    import render_p005 as P5
    from skd_skeleton import load_skeleton
    import collections
    buf, NB = R.load_container(cont)
    sk = load_skeleton(cont, trans_scale=1.0); bc = sk['bone_count']
    Wm = [np.array(m, float) for m in sk['world_bind']]
    V, N, tris, gmeta = P5.decode_groups(buf, NB, Wm, bc)
    tm = tri_materials(buf, NB, Wm, bc, tris)
    mats = material_table(texc, variant)
    atl = {}; bound = []; miss = collections.Counter()
    for t, (m, _f) in zip(tris, tm):
        if m == 0xff or m >= len(mats) or mats[m].get('ipu'):
            miss[m] += 1; continue
        if m not in atl:
            atl[m] = texture_rgba(texc, mats[m])[:, :, :3].astype(np.float32)
        bound.append((t[0], t[1], t[2], t[3], t[4], t[5], m))
    os.makedirs(outdir, exist_ok=True)
    use = collections.Counter(m for *_x, m in bound)
    info = dict(tris=len(tris), bound=len(bound), unbound=dict(miss),
                slots={m: f"{mats[m]['image_name']}+clut({mats[m]['name']}) x{n}" for m, n in sorted(use.items())})
    paths = []
    for ang, el, nm in ((18, 6, 'front'), (200, 6, 'back'), (55, 8, '3q')):
        p = os.path.join(outdir, f"{tag}_fromfile_{nm}.png")
        P5.render_multi(V, N, bound, atl, p, ang, el); paths.append(p)
    return info, paths


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == '--table':
        for m in material_table(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0):
            print(m['slot'], m['name'], m.get('image_name'), m.get('fields'))
    elif len(sys.argv) >= 5 and sys.argv[1] == '--render':
        print(render_from_file(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5] if len(sys.argv) > 5 else 'model'))
    else:
        sys.exit("usage: smd_material.py --table TEX.bin [variant] | --render CONT.bin TEX.bin OUTDIR [tag]")
