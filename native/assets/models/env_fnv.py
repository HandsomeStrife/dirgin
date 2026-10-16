#!/usr/bin/env python3
"""env_fnv.py -- the Python ORACLE side of the native env-decoder parity check.
Computes the exact same integer FNV-1a 64 signature as doc_env.c:doc_env_fnv,
from export_bmd_textured.decode_packets, so C vs Python can be diffed line-for-line.

    python3 -I env_fnv.py <chunk.bin ...>
      -> "<chunk8> npk <n> nv <total> fnv <hex>"  per input
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_bmd_textured import find_bmd_payload, decode_packets

FNV_OFFSET = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3
MASK = 0xFFFFFFFFFFFFFFFF


def fnv_update(h, data):
    for byte in data:
        h ^= byte
        h = (h * FNV_PRIME) & MASK
    return h


def chunk_fnv(packets):
    h = FNV_OFFSET
    for pk in packets:
        nv = len(pk["verts"])
        # counts: int32 LE  nv,is_list,sel,prim,abe   (matches doc_env.c order)
        h = fnv_update(h, struct.pack("<5i", nv, 1 if pk.get("is_list") else 0,
                                      pk["sel"], pk["prim"], 1 if pk["abe"] else 0))
        # pos_i: 3*nv int32  (pos = pos_i / 16 exactly)
        pos_i = []
        for (x, y, z) in pk["verts"]:
            pos_i += [int(round(x * 16.0)), int(round(y * 16.0)), int(round(z * 16.0))]
        h = fnv_update(h, struct.pack("<%di" % (3 * nv), *pos_i))
        # uv_i: 2*nv int16  (uv = uv_i / 4096 exactly)
        uv_i = []
        for (u, v) in pk["uv"]:
            uv_i += [int(round(u * 4096.0)), int(round(v * 4096.0))]
        h = fnv_update(h, struct.pack("<%dh" % (2 * nv), *uv_i))
        # col: 4*nv bytes
        col = bytearray()
        for (r, g, b, a) in pk["col"]:
            col += bytes((r & 0xff, g & 0xff, b & 0xff, a & 0xff))
        h = fnv_update(h, bytes(col))
    return h


def main():
    for path in sys.argv[1:]:
        buf = open(path, "rb").read()
        pay = find_bmd_payload(buf)
        if pay is None:
            sys.stderr.write("%s: no BMD payload\n" % path); continue
        pk = decode_packets(buf, pay)
        nv = sum(len(p["verts"]) for p in pk)
        name = os.path.basename(path)[:8]
        print("%s npk %d nv %d fnv %016x" % (name, len(pk), nv, chunk_fnv(pk)))


if __name__ == "__main__":
    main()
