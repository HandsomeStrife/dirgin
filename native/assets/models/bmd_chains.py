"""bmd_chains.py -- chain id per decoded BMD packet (data-sources-2026-10-24 items 3/4).

A chain is a run of draw packets (1-47 in z128) ending in a trailer (bbox + words;
176-208 bytes, sometimes more). Raw packet starts are STCYCL 0x01000404 followed by
UNPACK V4-32 num=2 @0 (the per-packet header). Each packet is walked command by
command (VIF command sizes, UNPACK data skipped by its length) to the MSCNT (0x17)
that ends it. If the next packet start follows that MSCNT directly, both are in one
chain; any gap there (the trailer) ends the chain.

The walk replaced a word-by-word search for a 0x17 top byte, which also hit vertex
data and broke 54 z128 chains that were really one. decode_packets drops
geometry-less packets, so decoded packets are mapped to raw starts by their `off`.

Failure direction: a packet the walk can't parse (unknown VIF command, or no MSCNT
before the next packet start) ends its chain, with a warning. A false break only
means less chain fill (packets stay grey), never a wrong texture.
"""
import struct

_VL = {0: 32, 1: 16, 2: 8, 3: 5}
# VIF commands with no or fixed-size payload (bytes after the command word).
_FIXED = {0x00: 0, 0x01: 0, 0x02: 0, 0x03: 0, 0x04: 0, 0x05: 0, 0x06: 0, 0x07: 0,
          0x10: 0, 0x11: 0, 0x13: 0, 0x14: 0, 0x15: 0, 0x17: 0,
          0x20: 4, 0x30: 16, 0x31: 16}


def _u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def vif_packet_end(buf, q, lim):
    """Offset just past the MSCNT that ends the packet starting at q. Raises
    ValueError if the walk meets an unknown command or passes lim (next start)."""
    p = q
    while p + 4 <= lim:
        c = _u32(buf, p); cmd = (c >> 24) & 0x7f; num = (c >> 16) & 0xff
        if cmd == 0x17:
            return p + 4
        if cmd in _FIXED:
            p += 4 + _FIXED[cmd]
        elif cmd >= 0x60:                                   # UNPACK
            nc = ((cmd >> 2) & 3) + 1; n = num or 256
            bits = 16 * n if cmd & 0xf == 0xf else nc * _VL[cmd & 3] * n   # V4-5: 16 bits/elem
            p += 4 + ((bits + 31) // 32) * 4
        elif cmd == 0x4a:                                   # MPG
            p += 4 + (num or 256) * 8
        elif cmd in (0x50, 0x51):                           # DIRECT / DIRECTHL
            p += 4 + ((c & 0xffff) or 65536) * 16
        else:
            raise ValueError('unknown VIF command 0x%02x at 0x%x' % (cmd, p))
    raise ValueError('no MSCNT before 0x%x' % lim)


def raw_chain_ids(buf, pay, warn=None):
    """{raw packet start offset: chain index} for the BMD payload at pay. Problems
    are appended to `warn` (a list) when given."""
    N = len(buf)
    st = [q for q in range(pay, N - 8, 4)
          if _u32(buf, q) == 0x01000404 and (_u32(buf, q + 4) >> 24) & 0x7f == 0x6c
          and ((_u32(buf, q + 4) >> 16) & 0xff) == 2 and (_u32(buf, q + 4) & 0x3ff) == 0]
    out = {}; c = 0
    for j, q in enumerate(st):
        out[q] = c
        if j == len(st) - 1:
            break
        try:
            if st[j + 1] > vif_packet_end(buf, q, st[j + 1]):
                c += 1
        except ValueError as x:
            c += 1
            if warn is not None:
                warn.append('packet at 0x%x: %s; chain ended there' % (q, x))
    if len(st) > 1 and c == 0 and warn is not None:
        warn.append('%d packets and no chain trailer: the whole chunk is one chain' % len(st))
    return out


def packet_chains(buf, pay, packets, warn=None):
    """Chain index per decoded packet (list, same order). Raises ValueError if a
    decoded packet's offset is not a raw packet start (decoder/scan disagree)."""
    ids = raw_chain_ids(buf, pay, warn)
    out = []
    for i, p in enumerate(packets):
        if p.get('off') not in ids:
            raise ValueError('packet %d (off %s) is not a raw chain packet start' % (i, p.get('off')))
        out.append(ids[p['off']])
    return out


def chain_headers(buf, pay, warn=None):
    """{chain index: header} read from the draw object in front of each chain's first packet.

    Decoded from the game code (2026-10-26, worker 0051):
    - FUN_0026ccc8 walks the bone tree's draw objects; each chain's object sits 0xb0 bytes
      before its first packet: 9 V4-32 bbox points, then +0x90 qword count (DMA ref, sent by
      FUN_00265bf0 as 0x30000000|count to obj+0xb0), +0x96 byte, +0x97 sort bucket,
      +0x98 s16 texture slot (-1 = none), +0x9a s16, +0x9c (runtime: BMD header +0x10 table),
      +0xa4 self pointer, +0xa9 second-pass mode, +0xaa visibility group, +0xab 2nd slot.
    - FUN_0026ce60 binds TEX = table[+0x98], the table at BMD header +0x10 holding
      u16 +0x0c u32 entries; an entry is the FileCode's last byte (<level>ffNN): all 448 z128
      entries are 0..0x89. `entry` is returned as stored; export_z128_room.py warns on a value
      above 0xff and leaves that chain unbound rather than guess which bits are the byte.
    - FUN_0026ccc8 skips a chain when FUN_00269450(+0xaa) is true: bit (+0xaa) of the global
      128-bit mask DAT_004e8710 (= DAT_004e86f0 | DAT_004e8700). Ids >= 0x80 always draw.
    - FUN_00265bf0: +0xa9 == 1 or 2 resends the chain with ALPHA 0x58 (Cs*Ad + Cd) and
      TEST 0x51001; texture = global TEX0 (1) or table[+0xab] (2). 0 = single pass.
    The self pointer (+0xa4 == object offset) is checked; a mismatch is a warning and the
    chain gets no header (its packets stay unbound)."""
    ntex = struct.unpack_from('<H', buf, pay + 0x0c)[0]
    tab = pay + _u32(buf, pay + 0x10)
    table = [_u32(buf, tab + 4 * i) for i in range(ntex)]
    ids = raw_chain_ids(buf, pay, warn)
    first = {}
    for q in sorted(ids):
        first.setdefault(ids[q], q)
    out = {}
    for c, q in first.items():
        o = q - 0xb0
        if o < pay or _u32(buf, o + 0xa4) + pay != o:
            if warn is not None:
                warn.append('chain %d at 0x%x: no draw-object header (self pointer mismatch)' % (c, q))
            continue
        slot = struct.unpack_from('<h', buf, o + 0x98)[0]
        out[c] = dict(slot=slot, entry=(table[slot] if 0 <= slot < ntex else None),
                      group=buf[o + 0xaa], pass2=buf[o + 0xa9], slot2=buf[o + 0xab],
                      flags=struct.unpack_from('<H', buf, o + 0x94)[0], bucket=buf[o + 0x97])
    return out, table
