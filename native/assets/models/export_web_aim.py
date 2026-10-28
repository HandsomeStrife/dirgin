#!/usr/bin/env python3
"""export_web_aim.py -- weapon stats and reticle tables for the web play page (aim.js).

    python3 -I native/assets/models/export_web_aim.py [--elf ELF | --iso ISO] [--bgd bgd.bin]
        [--parts 117,141,,,,0] [-o native/web/data/aim] [--check-state STATE.p2s]

Writes OUT/weapon.json and OUT/reticle.json (gitignored game data). Sources (all the user's
own files; notes/aim-research.md):

weapon.json: the weapon stats record W (0x78 bytes) as FUN_0031c3f8 builds it: the sum of up
  to 6 installed part records (bgd.bin section 5, 0x78 bytes each) plus its fix-ups. --parts
  lists the part indices per part slot (empty = no part). The default is the loadout of the
  sea-of-flames save state's current weapon slot (slot record R+0x34: parts #117, #141, #0;
  found by matching the save's part pointers against bgd.bin), so the page matches the PCSX2
  traces. --check-state composes from the save's own part records and compares the result
  with the save's live W (R+0x4c) byte for byte (test aid).

reticle.json: the reticle draw tables read from the ELF (static data):
  DAT_00472790[kind*3 + variant] -> piece records (0x14 bytes, list ends at sprite id 0;
  FUN_003b1048): +0 sprite id, +2 frame, +3 copies (s8; >0: n copies rotated 2pi/n, <0: a
  mirrored pair), +4/+6 x/y (12-bit signed, HUD px), +8/+0xa w/h (-1 = the sprite's size),
  +0xc RGBA, +0x10 flags. DAT_0047bfc0[sprite id] -> frames (u, v, w, h; FUN_003cb1f0 /
  FUN_003cb268). Phase lengths DAT_00472110[kind] (show / hide,
  FUN_003b0680 / FUN_003b0738), DAT_00472130[kind] (normal) and DAT_00472140[kind] (variant;
  FUN_003b07b0 / FUN_003b08a0). Texture name: PTR_DAT_0047d500[DAT_004727f0[2*kind]]
  (FUN_003dd4a8). The texture itself is not exported: its texel layout is not decoded
  (notes/aim-research.md, "Reticle texture").
"""
import argparse
import json
import os
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
BGD = os.path.join(ROOT, 'extract/_KEL/data/bgd/bgd.bin')
ELF = os.path.join(ROOT, 'elf/SLES_541.85')
OUT = os.path.join(ROOT, 'native/web/data/aim')
SAVE_PARTS = '117,141,,,,0'          # the sea-of-flames save's weapon slot 1 (R+0x34)

def f32(x):
    """x as an f32, rounded toward zero (the PS2 FPU's rounding; player.js chop())."""
    r = struct.unpack('<f', struct.pack('<f', x))[0]
    if abs(r) > abs(x):
        i = struct.unpack('<i', struct.pack('<f', r))[0]
        r = struct.unpack('<f', struct.pack('<i', i - 1))[0]
    return r


# ------------------------------------------------------------------ ELF
class Elf:
    def __init__(self, data):
        if data[:4] != b'\x7fELF':
            raise ValueError('not an ELF')
        phoff, = struct.unpack_from('<I', data, 0x1c)
        n, = struct.unpack_from('<H', data, 0x2c)
        self.d = data
        self.segs = []
        for i in range(n):
            t, off, va, _pa, fs, _ms = struct.unpack_from('<6I', data, phoff + 32 * i)
            if t == 1:
                self.segs.append((va, off, fs))

    def read(self, addr, n):
        for va, off, fs in self.segs:
            if va <= addr and addr + n <= va + fs:
                return self.d[off + addr - va: off + addr - va + n]
        raise ValueError('address %#x not in the ELF file image' % addr)

    def u32(self, a): return struct.unpack('<I', self.read(a, 4))[0]
    def u8(self, a): return self.read(a, 1)[0]


def load_elf(args):
    if args.iso:
        sys.path.insert(0, os.path.join(ROOT, 'tools/byo'))
        import disc  # noqa: E402
        iso = disc.Iso(args.iso)
        ent = iso.find(disc.BOOT_ID)
        return Elf(iso.read(ent))
    return Elf(open(args.elf, 'rb').read())


# ------------------------------------------------------------------ weapon (FUN_0031c3f8)
H = lambda b, o: struct.unpack_from('<h', b, o)[0]
F = lambda b, o: struct.unpack_from('<f', b, o)[0]
U = lambda b, o: struct.unpack_from('<I', b, o)[0]


def compose(parts):
    """FUN_0031c3f8(W, parts[6]): parts = 6 records (bytes) or None. Returns W (bytearray)."""
    w = bytearray(0x78)                                   # FUN_0031cb18: zero, then:
    def sH(o, v): struct.pack_into('<h', w, o, ((int(v) + 0x8000) & 0xffff) - 0x8000)
    def sF(o, v): struct.pack_into('<f', w, o, f32(v))
    def sU(o, v): struct.pack_into('<I', w, o, v & 0xffffffff)
    sU(0x14, U(w, 0x14) & 0xffffff83)
    sU(0x10, U(w, 0x10) | 0xf0000)
    sF(0x34, 180.0)
    sU(0x10, U(w, 0x10) & 0xfffffff)
    for i, p in enumerate(parts):
        if p is None:
            continue
        for o in (4, 8, 0xc):
            sU(o, U(w, o) | U(p, o))
        for o in (0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x38):
            sF(o, F(w, o) + F(p, o))
        if F(p, 0x34) >= 0.5:                             # FOV: the smallest part value
            sF(0x34, float(int(min(F(p, 0x34), F(w, 0x34)))))
        for o in (0x40, 0x42, 0x44, 0x46, 0x48, 0x4c, 0x4e, 0x50, 0x52, 0x54, 0x56, 0x60, 0x62,
                  0x64, 0x66, 0x68, 0x6a, 0x58, 0x5a, 0x5c, 0x5e, 0x6c, 0x6e, 0x70, 0x4a):
            sH(o, H(w, o) + H(p, o))
        u14 = U(w, 0x14)
        u14 = (u14 & 0xf0ffffff) | ((((u14 >> 24) & 0xf) + (p[0x17] & 0xf)) & 0xf) << 24
        sU(0x14, u14)
        w[0x16] = max(struct.unpack('b', w[0x16:0x17])[0], struct.unpack('b', p[0x16:0x17])[0]) & 0xff
        s = min(3, (w[0x14] & 3) + (p[0x14] & 3))
        sU(0x14, (U(w, 0x14) & 0xfffffffc) | s)
        if U(p, 0x10) & 0xf0000000:
            sU(0x10, (U(w, 0x10) & 0xfffffff) | (U(p, 0x10) & 0xf0000000))
        w[0x15] = (w[0x15] + p[0x15]) & 0xff
        if i == 1:
            if U(p, 0x14) & 0x60:
                sU(0x14, (U(w, 0x14) & 0xffffff9f) | (U(p, 0x14) & 0x60))
        elif i == 0:
            if U(p, 0x14) & 0x1c:
                sU(0x14, (U(w, 0x14) & 0xffffffe3) | (U(p, 0x14) & 0x1c))
        elif i == 2:
            sU(0x10, (U(w, 0x10) & 0xff0fffff) | 0x100000)
        elif i == 3:
            sU(0x10, (U(w, 0x10) & 0xf0ffffff) | 0x1000000)
    if H(w, 0x4c) != 0:
        sH(0x40, int(f32(f32(H(w, 0x40)) * f32((H(w, 0x4c) + 100.0) * 0.01))))
    s = H(w, 0x4e)
    if not (0 < s + 100):
        sH(0x44, 3)
    elif s != 0:
        sH(0x44, int(f32(f32(H(w, 0x44) * 0.01) * (100.0 - s))))
    if F(w, 0x18) < F(w, 0x38):
        sF(0x38, F(w, 0x18))
    if H(w, 0x60) != 0:
        sF(0x1c, (100.0 - H(w, 0x60)) * 0.01 * F(w, 0x1c))
    if H(w, 0x50) != 0:
        sF(0x20, (100.0 - H(w, 0x50)) * 0.01 * F(w, 0x20))
    if H(w, 0x62) != 0:
        sF(0x24, (100.0 - F(w, 0x24)) * 0.01 * F(w, 0x24))   # as the game: F(0x24), not the %
    if H(w, 0x54) != 0:
        sF(0x28, (H(w, 0x54) + 100) * 0.01 * F(w, 0x28))
    if H(w, 0x52) != 0:
        sF(0x18, (H(w, 0x52) + 100) * 0.01 * F(w, 0x18))
    if struct.unpack('b', w[0x16:0x17])[0] < 1:
        w[0x16] = 1
    if H(w, 0x48) == 0:
        sH(0x48, 1)
    if F(w, 0x18) > 1000.0:
        sF(0x18, 1000.0)
    if F(w, 0x34) <= 0.0 or F(w, 0x34) >= 120.0:
        sF(0x34, 25.0)
    if struct.unpack('b', w[0x15:0x16])[0] >= 3:
        w[0x15] = 2
    if U(w, 4) & 0x30000:
        sU(0x14, U(w, 0x14) & 0xffffff83)
    return w


def bgd_parts(bgd):
    off, cnt, stride = struct.unpack_from('<III', bgd, 0x24 + 16 * 5)
    if stride != 0x78:
        raise ValueError('bgd.bin section 5 stride %#x, expected 0x78' % stride)
    return [bgd[off + i * stride: off + (i + 1) * stride] for i in range(cnt)]


def weapon_json(w, parts_desc):
    return {
        'source': 'FUN_0031c3f8 over bgd.bin section 5 parts ' + parts_desc,
        'flags': U(w, 4),               # bit 0 auto (FUN_003320c0), 2 scope camera, 0x10 no pitch carry
        'class': (U(w, 0x14) >> 2) & 7,  # 1 handgun, 2 auto, 3 rifle (FUN_00577480 kick table)
        'pellets': struct.unpack('b', w[0x16:0x17])[0],
        'range10': F(w, 0x18),          # CAM+0x1b8 = x10 (FUN_00575510); bullet range x10 + 15
        'recoil_deg': F(w, 0x28),       # CAM+0x19c on a shot (FUN_0032ffb0)
        'speed10': F(w, 0x2c),          # bullet speed field (FUN_00552dd8)
        'scope_fov': F(w, 0x34),        # camera type 0xf FOV (FUN_0032a708)
        'lock10': F(w, 0x38),           # CAM+0x1b0 = x10 (FUN_00575510)
        'damage': H(w, 0x40),
        'first_delay': H(w, 0x42),      # state 1/3 timer (FUN_00574a80 / FUN_00574b00)
        'interval': H(w, 0x44),         # state 2/4 timer (FUN_00579570)
        'reload': H(w, 0x46),           # state 9 timer (FUN_00579570 state 7)
        'magazine': H(w, 0x48),
    }


# ------------------------------------------------------------------ reticle tables
def reticle_json(elf):
    kinds = {}
    sprites = {}
    # The length tables DAT_00472110 / 00472130 / 00472140 hold 4 entries each (kinds 0-3; the
    # next table starts right after). The HUD only ever selects kinds 0-2 (FUN_0033c060,
    # FUN_0033c208, FUN_0033b948 -> FUN_003b0440), and kind 3 has its own draw path
    # (FUN_003b0e08), so kinds 1-3 are exported.
    for k in range(1, 4):
        tex_idx = elf.u8(0x4727f0 + 2 * k)
        name_ptr = elf.u32(0x47d500 + 4 * tex_idx)
        tex = elf.read(name_ptr, 8).split(b'\0')[0].decode('latin-1')
        variants = []
        for v in range(3):
            p = elf.u32(0x472790 + k * 0xc + v * 4)
            pieces = []
            while p:
                rec = elf.read(p + 0x14 * len(pieces), 0x14)
                sid, frame, copies = struct.unpack_from('<HBb', rec, 0)
                if sid == 0:
                    break
                x, y, w, h = struct.unpack_from('<hhhh', rec, 4)
                sx = lambda s: ((s & 0xfff) ^ 0x800) - 0x800     # (u16 << 20) >> 20
                pieces.append({'sprite': sid, 'frame': frame, 'copies': copies,
                               'x': sx(x), 'y': sx(y),
                               'w': -1 if w == -1 else sx(w), 'h': -1 if h == -1 else sx(h),
                               'rgba': list(rec[0xc:0x10]), 'flags': struct.unpack_from('<I', rec, 0x10)[0]})
                if sid not in sprites:
                    sp = elf.u32(0x47bfc0 + 0x10 * sid)
                    sprites[sid] = sp
                if len(pieces) > 64:
                    raise ValueError('kind %d variant %d: no end marker' % (k, v))
            variants.append(pieces)
        kinds[k] = {'texture': tex,
                    'len_show': struct.unpack('<i', elf.read(0x472110 + 4 * k, 4))[0],   # FUN_003b0680 / 0738
                    'len': struct.unpack('<i', elf.read(0x472130 + 4 * k, 4))[0],
                    'len_variant': struct.unpack('<i', elf.read(0x472140 + 4 * k, 4))[0],
                    'variants': variants}
    frames = {}
    for sid, sp in sprites.items():
        fr = []
        if sp:
            used = max(pc['frame'] for kd in kinds.values() for var in kd['variants'] for pc in var
                       if pc['sprite'] == sid)
            for i in range(used + 1):
                fr.append(list(struct.unpack('<4h', elf.read(sp + 8 * i, 8))))
        frames[sid] = fr
    return {'source': 'ELF DAT_00472790 / DAT_0047bfc0 / DAT_00472130 / DAT_004727f0 (FUN_003b1048)',
            'hud': [512, 448], 'kinds': kinds, 'sprite_frames': frames}


# ------------------------------------------------------------------ test aid
def check_state(path, parts_all):
    sys.path.insert(0, os.path.join(ROOT, 'tools/trace'))
    from p2s import read_p2s  # noqa: E402
    import fields  # noqa: E402
    mem = read_p2s(path)['eeMemory.bin']
    u32 = lambda a: struct.unpack_from('<I', mem, a & 0x1ffffff)[0]
    P = fields.resolve_bases(bytearray(mem))['P']
    S = u32(P + 0x17c)                     # FUN_0055a370
    L = u32(S + 0x84)                      # FUN_00317c70 (+0x60 bit 0x200 path)
    idx = ((u32(L) & 0xf) ^ 8) - 8         # FUN_0031a990
    R = u32(L + 0x10) + idx * 0xcc
    ptrs = [u32(R + 0x34 + 4 * i) for i in range(6)]
    recs = [bytes(mem[p:p + 0x78]) if p else None for p in ptrs]
    live = bytes(mem[R + 0x4c:R + 0x4c + 0x78])
    got = bytes(compose(recs))
    ids = [next((i for i, q in enumerate(parts_all) if q == r), None) if r else None for r in recs]
    print('save weapon slot %d: parts %s (bgd section 5 indices)' % (idx, ids))
    diff = [o for o in range(0, 0x78, 2) if got[o:o + 2] != live[o:o + 2]]
    print('composed W vs live W: %s' % ('identical' if not diff else 'differs at ' + ', '.join('%#x' % o for o in diff)))
    return not diff


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--elf', default=ELF)
    ap.add_argument('--iso')
    ap.add_argument('--bgd', default=BGD)
    ap.add_argument('--parts', default=SAVE_PARTS)
    ap.add_argument('-o', '--out', default=OUT)
    ap.add_argument('--check-state')
    a = ap.parse_args()
    bgd = open(a.bgd, 'rb').read()
    allp = bgd_parts(bgd)
    if a.check_state:
        sys.exit(0 if check_state(a.check_state, allp) else 1)
    sel = [int(s) if s.strip() else None for s in a.parts.split(',')]
    sel += [None] * (6 - len(sel))
    w = compose([allp[i] if i is not None else None for i in sel[:6]])
    wj = weapon_json(w, a.parts)
    rj = reticle_json(load_elf(a))
    # write only once both are built, so a failure leaves neither file half-updated
    os.makedirs(a.out, exist_ok=True)
    for name, obj, ind in (('weapon.json', wj, 1), ('reticle.json', rj, None)):
        tmp = os.path.join(a.out, name + '.tmp')
        with open(tmp, 'w') as fh:
            json.dump(obj, fh, indent=ind)
        os.replace(tmp, os.path.join(a.out, name))
    print('weapon.json: interval %d, range %.0f, magazine %d, recoil %.2f deg; reticle.json: %d kinds'
          % (wj['interval'], wj['range10'] * 10, wj['magazine'], wj['recoil_deg'], len(rj['kinds'])))


if __name__ == '__main__':
    main()
