"""Read and rewrite PCSX2 save states (.p2s).

A .p2s is a zip archive. PCSX2 2.x compresses its members with zstd (zip method 93),
which Python's zipfile can't read, so this module parses the archive by hand and calls
the `zstd` command-line tool.

    members = read_p2s(path)              # {name: bytes}, decompressed
    write_p2s(path, members, raw_from)    # rewrite; unchanged members are copied raw
"""
import struct
import subprocess
import zlib

ZSTD = 93


def _entries(blob):
    """Yield (name, method, comp_bytes, crc, usize) from the central directory."""
    eocd = blob.rfind(b"PK\x05\x06")
    if eocd < 0:
        raise ValueError("not a zip (no end-of-central-directory record)")
    count, cd_size, cd_off = struct.unpack_from("<HII", blob, eocd + 10)
    p = cd_off
    for _ in range(count):
        if blob[p:p + 4] != b"PK\x01\x02":
            raise ValueError("bad central directory entry at %#x" % p)
        method, = struct.unpack_from("<H", blob, p + 10)
        crc, csize, usize = struct.unpack_from("<III", blob, p + 16)
        nlen, xlen, clen = struct.unpack_from("<HHH", blob, p + 28)
        loff, = struct.unpack_from("<I", blob, p + 42)
        name = blob[p + 46:p + 46 + nlen].decode()
        p += 46 + nlen + xlen + clen
        lnlen, lxlen = struct.unpack_from("<HH", blob, loff + 26)
        data = blob[loff + 30 + lnlen + lxlen:loff + 30 + lnlen + lxlen + csize]
        yield name, method, data, crc, usize


def _decompress(method, data, usize):
    if method == 0:
        return data
    if method == 8:
        return zlib.decompress(data, -15)
    if method == ZSTD:
        out = subprocess.run(["zstd", "-dqc"], input=data, capture_output=True, check=True).stdout
        if len(out) != usize:
            raise ValueError("zstd size mismatch")
        return out
    raise ValueError("unsupported zip method %d" % method)


def read_p2s(path):
    blob = open(path, "rb").read()
    return {name: _decompress(m, d, u) for name, m, d, _c, u in _entries(blob)}


def write_p2s(path, members, raw_from):
    """Write a .p2s. `members` maps names to new uncompressed bytes; every other member of
    `raw_from` (original .p2s path) is copied with its original compressed bytes."""
    blob = open(raw_from, "rb").read()
    out = bytearray()
    cd = bytearray()
    n = 0
    for name, method, data, crc, usize in _entries(blob):
        if name in members:
            raw = members[name]
            usize = len(raw)
            crc = zlib.crc32(raw) & 0xFFFFFFFF
            data = subprocess.run(["zstd", "-q", "-3", "-c"], input=raw,
                                  capture_output=True, check=True).stdout
            method = ZSTD
        nb = name.encode()
        off = len(out)
        out += struct.pack("<IHHHHHIIIHH", 0x04034B50, 63, 0, method, 0, 0x21,
                           crc, len(data), usize, len(nb), 0) + nb + data
        cd += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 63, 63, 0, method, 0, 0x21,
                          crc, len(data), usize, len(nb), 0, 0, 0, 0, 0, off) + nb
        n += 1
    cd_off = len(out)
    out += cd
    out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, n, n, len(cd), cd_off, 0)
    open(path, "wb").write(out)
