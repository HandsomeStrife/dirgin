"""bmd_chains.py -- chain id per decoded BMD packet (data-sources-2026-10-24 items 3/4).

A chain is a run of 1-10 draw packets ending in a 176-byte trailer (bbox + words).
Raw packet starts are STCYCL 0x01000404 followed by UNPACK V4-32 num=2 @0 (the
per-packet header). Between two packets of one chain the next STCYCL directly
follows the MSCNT (0x17) that ends the previous one; a chain boundary is any gap
there (the trailer). decode_packets drops geometry-less packets, so decoded packets
are mapped to raw starts by their `off`.
"""
import struct


def _u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def raw_chain_ids(buf, pay):
    """{raw packet start offset: chain index} for the BMD payload at pay."""
    N = len(buf)
    st = [q for q in range(pay, N - 8, 4)
          if _u32(buf, q) == 0x01000404 and (_u32(buf, q + 4) >> 24) & 0x7f == 0x6c
          and ((_u32(buf, q + 4) >> 16) & 0xff) == 2 and (_u32(buf, q + 4) & 0x3ff) == 0]
    out = {}; c = 0
    for j, q in enumerate(st):
        out[q] = c
        if j < len(st) - 1:
            e = q
            while e < st[j + 1] and (_u32(buf, e) >> 24) & 0x7f != 0x17:
                e += 4
            if st[j + 1] - e - 4 > 0:
                c += 1
    return out


def packet_chains(buf, pay, packets):
    """Chain index per decoded packet (list, same order). Raises ValueError if a
    decoded packet's offset is not a raw packet start (decoder/scan disagree)."""
    ids = raw_chain_ids(buf, pay)
    out = []
    for i, p in enumerate(packets):
        if p.get('off') not in ids:
            raise ValueError('packet %d (off %s) is not a raw chain packet start' % (i, p.get('off')))
        out.append(ids[p['off']])
    return out
