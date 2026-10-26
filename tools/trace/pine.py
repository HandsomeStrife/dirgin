"""Minimal PINE client (PCSX2's IPC socket) for reading EE memory.

Protocol (PCSX2 pcsx2/PINE.cpp): request = u32 total size, then commands (u8 opcode +
args); reply = u32 total size, u8 result (0 = OK), then each command's data.
Only reads and status are used here; the trace hook does its own input injection.
"""
import os
import socket
import struct

READ32, READ64, VERSION, STATUS = 2, 3, 8, 0xF
MAX_BATCH = 4000          # 4000 x 5-byte commands, replies 8 bytes each: well under 650000


class Pine:
    def __init__(self, slot=28011):
        run = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
        name = "pcsx2.sock" if slot == 28011 else "pcsx2.sock.%d" % slot
        self.path = os.path.join(run, name)
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(10)
        self.s.connect(self.path)

    def _call(self, body):
        self.s.sendall(struct.pack("<I", len(body) + 4) + body)
        hdr = self._recv(4)
        size, = struct.unpack("<I", hdr)
        rest = self._recv(size - 4)
        if rest[0] != 0:
            raise IOError("PINE command failed")
        return rest[1:]

    def _recv(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.s.recv(n - len(buf))
            if not chunk:
                raise IOError("PINE socket closed")
            buf += chunk
        return buf

    def status(self):
        """0 = running, 1 = paused, 2 = shutdown."""
        return struct.unpack("<I", self._call(bytes([STATUS])))[0]

    def read32(self, addr):
        return struct.unpack("<I", self._call(struct.pack("<BI", READ32, addr)))[0]

    def read_block(self, addr, size):
        """Read `size` bytes (multiple of 8) starting at an 8-aligned EE address."""
        assert addr % 8 == 0 and size % 8 == 0
        out = bytearray()
        n = size // 8
        for start in range(0, n, MAX_BATCH):
            cnt = min(MAX_BATCH, n - start)
            body = b"".join(struct.pack("<BI", READ64, addr + 8 * (start + i)) for i in range(cnt))
            out += self._call(body)
        return bytes(out)
