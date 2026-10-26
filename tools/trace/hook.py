"""Build a trace save state: the z128 .p2s plus a pad-read hook, a scripted input table
and a per-frame field logger, all written into free EE RAM inside the state.

Why in the state: input then starts on the first pad poll after the state loads, with no
host timing involved, so runs are reproducible (see README "Determinism").

Memory map (EE, all zero in sea-of-flames.p2s; below the ELF's 0x100000 load address):
  HOOK   0x00096000  MIPS hook (this file assembles it)
  CTRL   0x00096800  +0 armed, +4 frame index, +8 frame count, +0xc script ptr,
                     +0x10 log ptr, +0x14 record size, +0x18 field table ptr,
                     +0x1c first logged frame (earlier frames get input but no record)
  FIELDS 0x00096900  {u32 src, u32 nwords} ... {0, 0}
  SCRIPT 0x00097000  24 bytes per frame: bytes 0..17 are copied over pad buffer +2..+19
                     (buttons active-low, RX RY LX LY, 12 pressure bytes)
  LOG    0x0009d000 .. 0x00100000

Patch: the `jal 0x443b50` (scePadRead for port 0, buffer = s0 = 0x4e1900) at 0x257ebc in
FUN_00257de0 becomes `jal HOOK`. The hook calls the real 0x443b50 first, so the IOP
side runs as normal; it then overwrites buffer bytes 2..19 and logs.
"""
import struct

HOOK, CTRL, FIELDS, SCRIPT, LOG, LOG_END = 0x96000, 0x96800, 0x96900, 0x97000, 0x9D000, 0x100000
SCRIPT_STRIDE = 24
PAD_COPY = 18
CALL_SITE, PAD_READ = 0x257EBC, 0x443B50
FREE_LO, FREE_HI = 0x96000, 0x100000

R = dict(zero=0, v0=2, a0=4, a1=5, a2=6, t0=8, t1=9, t2=10, t3=11, t4=12, t5=13, t6=14,
         t7=15, s0=16, s1=17, t8=24, t9=25, sp=29, ra=31)


class Asm:
    """Just enough of a MIPS (R5900) assembler for the hook."""

    def __init__(self, base):
        self.base, self.code, self.labels, self.fix = base, [], {}, []

    def pc(self):
        return self.base + 4 * len(self.code)

    def label(self, n):
        self.labels[n] = self.pc()

    def _i(self, op, rs, rt, imm):
        self.code.append((op << 26) | (R[rs] << 21) | (R[rt] << 16) | (imm & 0xFFFF))

    def _r(self, rs, rt, rd, sa, fn):
        self.code.append((R[rs] << 21) | (R[rt] << 16) | (R[rd] << 11) | (sa << 6) | fn)

    def addiu(self, rt, rs, imm): self._i(0x09, rs, rt, imm)
    def lui(self, rt, imm): self._i(0x0F, "zero", rt, imm)
    def ori(self, rt, rs, imm): self._i(0x0D, rs, rt, imm)
    def lw(self, rt, off, rs): self._i(0x23, rs, rt, off)
    def sw(self, rt, off, rs): self._i(0x2B, rs, rt, off)
    def lbu(self, rt, off, rs): self._i(0x24, rs, rt, off)
    def sb(self, rt, off, rs): self._i(0x28, rs, rt, off)
    def ld(self, rt, off, rs): self._i(0x37, rs, rt, off)
    def sd(self, rt, off, rs): self._i(0x3F, rs, rt, off)
    def addu(self, rd, rs, rt): self._r(rs, rt, rd, 0, 0x21)
    def slt(self, rd, rs, rt): self._r(rs, rt, rd, 0, 0x2A)
    def sll(self, rd, rt, sa): self._r("zero", rt, rd, sa, 0x00)
    def mult(self, rs, rt): self._r(rs, rt, "zero", 0, 0x18)
    def mflo(self, rd): self._r("zero", "zero", rd, 0, 0x12)
    def jr(self, rs): self._r(rs, "zero", "zero", 0, 0x08)
    def nop(self): self.code.append(0)
    def jal(self, target): self.code.append((0x03 << 26) | ((target >> 2) & 0x3FFFFFF))

    def _br(self, op, rs, rt, lab):
        self.fix.append((len(self.code), lab))
        self._i(op, rs, rt, 0)

    def beq(self, rs, rt, lab): self._br(0x04, rs, rt, lab)
    def bne(self, rs, rt, lab): self._br(0x05, rs, rt, lab)
    def subu(self, rd, rs, rt): self._r(rs, rt, rd, 0, 0x23)
    def bgtz(self, rs, lab): self._br(0x07, rs, "zero", lab)

    def li32(self, rt, val):
        self.lui(rt, val >> 16)
        self.ori(rt, rt, val & 0xFFFF)

    def bytes(self):
        for idx, lab in self.fix:
            off = (self.labels[lab] - (self.base + 4 * idx + 4)) >> 2
            self.code[idx] = (self.code[idx] & 0xFFFF0000) | (off & 0xFFFF)
        return struct.pack("<%dI" % len(self.code), *self.code)


def assemble_hook():
    a = Asm(HOOK)
    a.addiu("sp", "sp", -48)
    a.sd("ra", 0, "sp")
    a.sd("s0", 8, "sp")
    a.sd("s1", 16, "sp")
    a.addu("s0", "a2", "zero")          # pad buffer
    a.jal(PAD_READ)                     # the real scePadRead (a0, a1, a2 untouched)
    a.nop()
    a.sd("v0", 24, "sp")
    a.li32("s1", CTRL)
    a.lw("t0", 0, "s1")
    a.beq("t0", "zero", "out")          # not armed
    a.nop()
    a.lw("t1", 4, "s1")                 # frame index
    a.lw("t2", 8, "s1")
    a.slt("t3", "t1", "t2")
    a.beq("t3", "zero", "out")          # script finished: real pad, no logging
    a.nop()
    # input: copy SCRIPT[idx][0..17] -> buf[2..19]
    a.lw("t4", 0xC, "s1")
    a.addiu("t5", "zero", SCRIPT_STRIDE)
    a.mult("t5", "t1")
    a.mflo("t5")
    a.addu("t4", "t4", "t5")
    a.addu("t7", "s0", "zero")
    a.addiu("t9", "zero", PAD_COPY)
    a.label("cp")
    a.lbu("t6", 0, "t4")
    a.sb("t6", 2, "t7")
    a.addiu("t4", "t4", 1)
    a.addiu("t7", "t7", 1)
    a.addiu("t9", "t9", -1)
    a.bgtz("t9", "cp")
    a.nop()
    # log record (from frame CTRL+0x1c on): idx, pad words 0 and 1, then the field table
    a.lw("t3", 0x1C, "s1")
    a.slt("t2", "t1", "t3")
    a.bne("t2", "zero", "fdone")
    a.nop()
    a.subu("t2", "t1", "t3")
    a.lw("t4", 0x10, "s1")
    a.lw("t5", 0x14, "s1")
    a.mult("t5", "t2")
    a.mflo("t5")
    a.addu("t4", "t4", "t5")
    a.sw("t1", 0, "t4")
    a.lw("t6", 0, "s0")
    a.sw("t6", 4, "t4")
    a.lw("t6", 4, "s0")
    a.sw("t6", 8, "t4")
    a.addiu("t4", "t4", 12)
    a.lw("t7", 0x18, "s1")
    a.label("fld")
    a.lw("t8", 0, "t7")
    a.beq("t8", "zero", "fdone")
    a.lw("t9", 4, "t7")                 # delay slot
    a.label("wd")
    a.lw("t6", 0, "t8")
    a.sw("t6", 0, "t4")
    a.addiu("t8", "t8", 4)
    a.addiu("t4", "t4", 4)
    a.addiu("t9", "t9", -1)
    a.bgtz("t9", "wd")
    a.nop()
    a.addiu("t7", "t7", 8)
    a.beq("zero", "zero", "fld")
    a.nop()
    a.label("fdone")
    a.addiu("t1", "t1", 1)
    a.sw("t1", 4, "s1")
    a.label("out")
    a.ld("v0", 24, "sp")
    a.ld("ra", 0, "sp")
    a.ld("s0", 8, "sp")
    a.ld("s1", 16, "sp")
    a.jr("ra")
    a.addiu("sp", "sp", 48)
    code = a.bytes()
    assert HOOK + len(code) <= CTRL
    return code


def patch_memory(mem, script_frames, field_table, log_from=0):
    """mem: bytearray of eeMemory.bin. script_frames: list of 18-byte pad payloads.
    field_table: list of (abs_addr, nwords). Returns the log record size in bytes."""
    if any(mem[FREE_LO:FREE_HI]):
        raise RuntimeError("hook area 0x96000-0xfffff is not zero in this state")
    old = struct.unpack_from("<I", mem, CALL_SITE)[0]
    if old != (0x0C000000 | (PAD_READ >> 2)):
        raise RuntimeError("unexpected instruction at the call site: %08x" % old)
    rec = 12 + 4 * sum(n for _, n in field_table)
    nmax = (LOG_END - LOG) // rec
    if len(script_frames) - log_from > nmax:
        raise RuntimeError("script logs %d frames; the log holds %d at %d B/frame"
                           % (len(script_frames) - log_from, nmax, rec))
    if len(script_frames) > (LOG - SCRIPT) // SCRIPT_STRIDE:
        raise RuntimeError("script too long for the script area")
    if FIELDS + 8 * (len(field_table) + 1) > SCRIPT:
        raise RuntimeError("field table too long")
    code = assemble_hook()
    mem[HOOK:HOOK + len(code)] = code
    for i, fr in enumerate(script_frames):
        assert len(fr) == PAD_COPY
        mem[SCRIPT + i * SCRIPT_STRIDE:SCRIPT + i * SCRIPT_STRIDE + PAD_COPY] = fr
    for i, (addr, n) in enumerate(field_table):
        if addr % 4 or n <= 0:
            raise ValueError("bad field %#x x%d" % (addr, n))
        struct.pack_into("<II", mem, FIELDS + 8 * i, addr, n)
    struct.pack_into("<8I", mem, CTRL, 1, 0, len(script_frames), SCRIPT, LOG, rec, FIELDS, log_from)
    struct.pack_into("<I", mem, CALL_SITE, 0x0C000000 | (HOOK >> 2))
    return rec
