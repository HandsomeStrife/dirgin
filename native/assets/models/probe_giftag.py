#!/usr/bin/env python3
"""probe_giftag.py -- settle what the BMD draw-packet giftag qword1.x field IS.

FORMAT.md 3.4 once called it a "material/texture selector"; export_bmd_textured.py's
inline comment called it the vertex count. This probe decides it numerically: for
every draw packet it reads qword1.x and the actually-unpacked V3-16 vertex count and
checks whether `qword1.x & 0x7fff == num`.

RESULT (z128 chunk 06800212, 2026-10-16): equal for 1273/1273 packets. The field is
the VERTEX COUNT (with VU flag bit 0x8000 set). There is NO per-submesh material
selector in the VIF draw chain -- the real binding is the runtime group table
(FUN_002e62b0: M+0x60[group], material selector u16 @group+8). See FORMAT.md 3.4
(corrected) and notes/DECODER-STATUS.md (5th pass).

Run: python3 -I probe_giftag.py <container.bin>   (trusted-input only; bounds-checked)
"""
import sys, struct

VL = {0: 32, 1: 16, 2: 8, 3: 5}


def _rd(buf):
    def u16(o): return struct.unpack_from('<H', buf, o)[0]
    def u32(o): return struct.unpack_from('<I', buf, o)[0]
    def u64(o): return struct.unpack_from('<Q', buf, o)[0]
    return u16, u32, u64


def find_bmd_payload(buf):
    u16, u32, u64 = _rd(buf); N = len(buf)
    if N < 0x18:
        return None
    cnt = u16(0x10)
    if cnt == 0 or cnt > 1024:
        return None
    for i in range(cnt):
        if 0x14 + 4 * i + 4 > N:
            break
        eoff = u32(0x14 + 4 * i)
        if eoff + 0x10 > N:
            continue
        if buf[eoff + 8:eoff + 11] == b'bmd' and buf[eoff + 0x10:eoff + 0x13] == b'BMD':
            return eoff + 0x10
    return None


def rd_unpack(buf, p):
    N = len(buf)
    if p + 4 > N:
        return None
    c = struct.unpack_from('<I', buf, p)[0]
    num = (c >> 16) & 0xff; cmd = (c >> 24) & 0x7f
    if not (0x60 <= cmd <= 0x7f):
        return None
    vn = (cmd >> 2) & 3; vl = cmd & 3; bits = VL[vl]; nc = vn + 1
    addr = (c & 0xffff) & 0x3ff
    nbytes = ((nc * bits + 7) // 8) * num
    return (nc, bits, num, addr, p + 4, nbytes)


def main():
    buf = open(sys.argv[1], 'rb').read(); N = len(buf)
    u16, u32, u64 = _rd(buf)
    pay = find_bmd_payload(buf)
    if pay is None:
        raise SystemExit("no BMD payload in %s" % sys.argv[1])
    print("payload @ 0x%x" % pay)
    o = pay + 8
    while o + 4 <= N and u32(o) != 0x01000404:
        o += 4
    p = o
    rows = []
    while p + 4 <= N:
        c = u32(p); cmd = (c >> 24) & 0x7f; imm = c & 0xffff
        if not (cmd == 0x01 and imm == 0x0404):
            p += 4; continue
        pp = p + 4
        u = rd_unpack(buf, p + 4)
        gw1 = prim = None
        if u and u[0] == 4 and u[1] == 32 and u[3] == 0:
            gt = u[4]
            if gt + 32 <= N:
                lo = u64(gt); gw1 = u64(gt + 16); prim = (lo >> 47) & 0x7ff
            pp = u[4] + u[5]
        num_pos = None; guard = 0; q = pp
        while q + 4 <= N and guard < 48:
            guard += 1
            cm = (u32(q) >> 24) & 0x7f
            if cm == 0x01: q += 4; continue
            if cm == 0x05: q += 4; continue
            if cm == 0x20: q += 8; continue
            if cm == 0x30: q += 20; continue
            if cm == 0x17: q += 4; break
            uu = rd_unpack(buf, q)
            if not uu: q += 4; continue
            nc, bits, num, addr, doff, nb = uu
            if addr == 2 and nc == 3 and bits == 16:
                num_pos = num
            q += 4 + nb
            if q & 3: q = (q + 3) & ~3
        if gw1 is not None:
            rows.append((gw1 & 0xffffffff, num_pos, prim))
        p = q if q > p else p + 4

    eqnum = 0; vals = {}
    for qw1x, num_pos, prim in rows:
        lo15 = qw1x & 0x7fff
        vals[lo15] = vals.get(lo15, 0) + 1
        if lo15 == num_pos:
            eqnum += 1
    print("packets=%d ; (qw1.x & 0x7fff)==unpacked_num for %d/%d"
          % (len(rows), eqnum, len(rows)))
    print("distinct (qw1.x & 0x7fff): %d -> %s"
          % (len(vals), [hex(v) for v in sorted(vals)]))
    print("VERDICT: field is the VERTEX COUNT"
          if eqnum == len(rows) else "VERDICT: field is NOT simply the vertex count")


if __name__ == '__main__':
    main()
