"""Call logger for the trace state (worker 0127): wrap chosen game calls, log args and results.

Opt-in with TRACE_CALLLOG=<set> (see SETS). Each hook is either a `jal` call site inside a
function (rewritten to `jal STUB`) or a vtable slot (pointer replaced by STUB). The stub saves
a0-a3/t0/t1 and ra, calls the real function, then a shared logger appends one 128-byte entry:
  w0 hook id, w1 frame index (CTRL+4), w2 a0, w3 a1, w4 v0, w5 f0 (float result), w6 f12 at
  entry (first float argument), then up to 25 words copied from
  (saved register + offset) AFTER the call, per the hook's copy list.
Entries are only written while frame_lo <= frame < frame_hi. Results go to <name>.calls.jsonl.

Memory: stubs + logger at 0x96200..0x967ff (the pad hook ends below 0x96200), descriptors at
CLOG_CTRL, entries in the top CLOG_BYTES of the frame-log area (the frame log is shortened).
"""
import struct

import hook

STUBS, CLOG_CTRL, CLOG_DESC = 0x96200, 0x9CC00, 0x9CC20   # CTRL2: lo, hi, wp, end
# CLOG_CTRL/DESC sit at the end of the script area: scripts must be <= 1000 frames.
CLOG_BYTES = 0x30000
ENTRY = 128
SLOT = dict(a0=0x10, a1=0x20, a2=0x30, a3=0x40, t0=0x50, t1=0x60, v0=0x70)


class Asm(hook.Asm):
    def lq(self, rt, off, rs): self._i(0x1E, rs, rt, off)
    def sq(self, rt, off, rs): self._i(0x1F, rs, rt, off)
    def sra(self, rd, rt, sa): self._r("zero", rt, rd, sa, 0x03)
    def srl(self, rd, rt, sa): self._r("zero", rt, rd, sa, 0x02)
    def andi(self, rt, rs, imm): self._i(0x0C, rs, rt, imm)
    def swc1(self, ft, off, rs): self.code.append((0x39 << 26) | (hook.R[rs] << 21) | (ft << 16) | (off & 0xFFFF))


hook.R.update(a3=7, v1=3, s2=18, t9=25)


def _logger(a):
    """t8 = stub frame (saved regs), t9 = descriptor {id, copy words..., 0}. Leaf, t-regs only."""
    a.label("logger")
    a.li32("t0", CLOG_CTRL)
    a.li32("t1", hook.CTRL)
    a.lw("t1", 4, "t1")                  # frame index
    a.lw("t2", 0, "t0")
    a.slt("t3", "t1", "t2")
    a.bne("t3", "zero", "lret")
    a.lw("t2", 4, "t0")                  # delay
    a.slt("t3", "t1", "t2")
    a.beq("t3", "zero", "lret")
    a.lw("t4", 8, "t0")                  # delay: wp
    a.lw("t5", 12, "t0")
    a.addiu("t6", "t4", ENTRY)
    a.slt("t3", "t5", "t6")
    a.bne("t3", "zero", "lret")          # full
    a.nop()
    a.sw("t6", 8, "t0")                  # wp += ENTRY
    a.lw("t2", 0, "t9")
    a.sw("t2", 0, "t4")                  # id
    a.sw("t1", 4, "t4")                  # frame
    a.lw("t2", SLOT["a0"], "t8"); a.sw("t2", 8, "t4")
    a.lw("t2", SLOT["a1"], "t8"); a.sw("t2", 12, "t4")
    a.lw("t2", SLOT["v0"], "t8"); a.sw("t2", 16, "t4")
    a.lw("t2", 0x80, "t8"); a.sw("t2", 20, "t4")      # f0 after the call
    a.lw("t2", 0x84, "t8"); a.sw("t2", 24, "t4")      # f12 at entry
    a.addiu("t4", "t4", 28)
    a.addiu("t9", "t9", 4)
    a.label("cpy")
    a.lw("t2", 0, "t9")                  # (slot << 24) | (n << 16) | off16
    a.beq("t2", "zero", "lret")
    a.srl("t3", "t2", 24)                # delay: slot
    a.addu("t3", "t3", "t8")
    a.lw("t3", 0, "t3")                  # saved register (low word)
    a.sll("t5", "t2", 16)
    a.sra("t5", "t5", 16)
    a.addu("t3", "t3", "t5")             # src
    a.srl("t6", "t2", 16)
    a.andi("t6", "t6", 0xFF)             # n words
    a.label("cw")
    a.lw("t5", 0, "t3")
    a.sw("t5", 0, "t4")
    a.addiu("t3", "t3", 4)
    a.addiu("t4", "t4", 4)
    a.addiu("t6", "t6", -1)
    a.bgtz("t6", "cw")
    a.nop()
    a.addiu("t9", "t9", 4)
    a.beq("zero", "zero", "cpy")
    a.nop()
    a.label("lret")
    a.jr("ra")
    a.nop()


def _stub(a, target, desc):
    a.addiu("sp", "sp", -0x90)
    a.sq("ra", 0, "sp")
    for r in ("a0", "a1", "a2", "a3", "t0", "t1"):
        a.sq(r, SLOT[r], "sp")
    a.swc1(12, 0x84, "sp")
    a.jal(target)
    a.nop()
    a.sq("v0", SLOT["v0"], "sp")
    a.swc1(0, 0x80, "sp")               # f0 (float result); f12 was saved at 0x84
    a.li32("t9", desc)
    a.addu("t8", "sp", "zero")
    a.jal(a.labels["logger"])
    a.nop()
    a.lq("v0", SLOT["v0"], "sp")
    a.lq("ra", 0, "sp")
    a.jr("ra")
    a.addiu("sp", "sp", 0x90)


def cp(reg, off, n):
    return (SLOT[reg] << 24) | (n << 16) | (off & 0xFFFF)


JAL = lambda t: 0x0C000000 | (t >> 2)

# Hook sets. kind "site": (function start, end, callee) -> every `jal callee` in [start, end).
# kind "vt": (vtable, slot offset).
SETS = {
    # worker 0127, notes/enemy-research.md §11: the bullet hit test.
    "bullet": [
        dict(id=1, kind="site", where=(0x556870, 0x557400, 0x5442D8), name="hit_test",
             # a3 = bullet: +0xe0 seg start, +0x90 seg end, +0x120 flags, +0x128 owner id,
             # +0xc0 origin, +0x124 time, +0x12c speed; a0 = mgr +0x10c0 hit point/normal
             copy=[cp("a3", 0xE0, 4), cp("a3", 0x90, 4), cp("a3", 0x120, 1), cp("a3", 0x128, 1),
                   cp("a3", 0xC0, 4), cp("a3", 0x124, 3), cp("a0", 0x10C0, 8)]),
        dict(id=2, kind="vt", where=(0x58F128, 0x1AC), name="filter_player", copy=[]),
        dict(id=3, kind="vt", where=(0x58E1F8, 0x1AC), name="filter_e027", copy=[]),
        dict(id=4, kind="vt", where=(0x58F128, 0x17C), name="part_player",
             copy=[cp("t0", 0, 3), cp("t1", 0, 3), cp("a2", 0, 3), cp("a0", 0x140, 1),
                   cp("a0", 0x19C, 1), cp("a0", 0x2C, 1), cp("a0", 0x118, 1), cp("a0", 0x28, 1)]),
        dict(id=5, kind="vt", where=(0x58E1F8, 0x17C), name="part_e027",
             copy=[cp("t0", 0, 3), cp("t1", 0, 3), cp("a2", 0, 3), cp("a0", 0x140, 1),
                   cp("a0", 0x19C, 1), cp("a0", 0x2C, 1), cp("a0", 0x118, 1), cp("a0", 0x28, 1)]),
        dict(id=6, kind="site", where=(0x55BDA8, 0x55C470, 0x55BC90), name="capsule",
             copy=[cp("a1", 0, 4), cp("a2", 0, 4), cp("a0", 0x2C, 1)]),
        dict(id=7, kind="site", where=(0x55BDA8, 0x55C470, 0x5AD118), name="part_ray",
             copy=[cp("a1", 0, 3), cp("a2", 0, 3)]),
        dict(id=8, kind="site", where=(0x55BDA8, 0x55C470, 0x25C0F8), name="seg_capsule",
             copy=[]),
        dict(id=9, kind="site", where=(0x55BDA8, 0x55C470, 0x25C770), name="pt_capsule",
             copy=[]),
    ],
}
# worker 0127: the fire-time spread (FUN_005405b0 -> FUN_0025a058, rand FUN_0040b5c0).
SETS["spread"] = [SETS["bullet"][0],
    dict(id=20, kind="site", where=(0x5694F8, 0x569870, 0x5409A8), name="spawn",
         copy=[cp("a1", 0, 4), cp("a2", 0, 4)]),          # muzzle, aim as passed (unchanged)
    dict(id=21, kind="site", where=(0x5409A8, 0x540AB0, 0x5405B0), name="aim_spread",
         copy=[cp("a2", 0, 4), cp("a3", 0, 4)]),          # muzzle, aim after spread (= end point)
    dict(id=22, kind="site", where=(0x5405B0, 0x5408E8, 0x25A058), name="cone",
         copy=[cp("a1", 0, 4)]),                          # f12 = spread (deg); dir after
    dict(id=23, kind="site", where=(0x25A058, 0x25A240, 0x40B5C0), name="rand", copy=[]),  # f0
]

# worker 0158: the physics-body step of mode 0xc (FUN_0034c388 -> FUN_00358fb8 / FUN_003590c8 ->
# FUN_00357d48). a0 = body (C+0x3b0): +0 pos, +0x30 step, +0x40 velocity, +0x80 (= C+0x430), +0xa8.
SETS["pushstep"] = [
    dict(id=30, kind="site", where=(0x34C388, 0x34D400, 0x358FB8), name="set_step",
         copy=[cp("a1", 0, 4), cp("a0", 0x30, 4), cp("a0", 0x80, 4), cp("a0", 0xA8, 2)]),
    dict(id=31, kind="site", where=(0x34C388, 0x34D400, 0x3590C8), name="body_step",
         copy=[cp("a0", 0, 4), cp("a0", 0x30, 4), cp("a0", 0x40, 4), cp("a0", 0x80, 4)]),
    dict(id=32, kind="site", where=(0x3590C8, 0x359D68, 0x357D48), name="walk_move",
         copy=[cp("a1", 0, 4), cp("a0", 0, 4), cp("a0", 0x110, 1)]),
    # FUN_0035a440(body, out, in): out = in + correction (wall / object contacts, then + body+0x80)
    dict(id=33, kind="site", where=(0x357D48, 0x358FB8, 0x35A440), name="push_out",
         copy=[cp("a1", 0, 4), cp("a2", 0, 4), cp("a0", 0x80, 4), cp("a0", 0xB0, 4)]),
]


def patch(mem, setname, frame_lo, frame_hi, log_end):
    """Install the hooks of SETS[setname]. Returns (clog_start, clog_end, hooks)."""
    hooks = SETS[setname]
    a = Asm(STUBS)
    a.beq("zero", "zero", "past")        # logger first so stubs can jal it
    a.nop()
    _logger(a)
    a.label("past")
    desc_at, desc_words = CLOG_DESC, []
    sites = []
    for h in hooks:
        d = desc_at + 4 * len(desc_words)
        desc_words += [h["id"]] + h["copy"] + [0]
        assert len(h["copy"]) == 0 or sum((c >> 16) & 0xFF for c in h["copy"]) <= 25
        stub_at = a.pc()
        if h["kind"] == "site":
            lo, hi, callee = h["where"]
            found = [x for x in range(lo, hi, 4) if struct.unpack_from("<I", mem, x)[0] == JAL(callee)]
            if not found:
                raise RuntimeError("no jal %#x in %#x..%#x" % (callee, lo, hi))
            _stub(a, callee, d)
            sites += [(x, JAL(stub_at)) for x in found]
            h["sites"] = [hex(x) for x in found]
        else:
            vt, off = h["where"]
            target = struct.unpack_from("<I", mem, vt + off)[0]
            _stub(a, target, d)
            sites.append((vt + off, stub_at))
            h["sites"] = [hex(vt + off) + "->" + hex(target)]
    code = a.bytes()
    assert STUBS + len(code) <= hook.CTRL, "stubs overflow"
    assert CLOG_DESC + 4 * len(desc_words) <= hook.LOG, "descriptors overflow"
    assert not any(mem[CLOG_CTRL:hook.LOG]), "script area overlaps the call-log control block"
    mem[STUBS:STUBS + len(code)] = code
    struct.pack_into("<%dI" % len(desc_words), mem, CLOG_DESC, *desc_words)
    start = log_end - CLOG_BYTES
    struct.pack_into("<4I", mem, CLOG_CTRL, frame_lo, frame_hi, start, log_end)
    for addr, word in sites:
        struct.pack_into("<I", mem, addr, word)
    return start, log_end, hooks


def decode(raw, hooks):
    names = {h["id"]: h for h in hooks}
    out = []
    for i in range(0, len(raw) - ENTRY + 1, ENTRY):
        w = struct.unpack_from("<32I", raw, i)
        if w[0] == 0:
            break
        h = names[w[0]]
        tof = lambda x: round(struct.unpack("<f", struct.pack("<I", x))[0], 6)
        e = dict(hook=h["name"], frame=w[1], a0=hex(w[2]), a1=w[3], v0=w[4] if w[4] < 0x80000000 else w[4] - (1 << 32),
                 f0=tof(w[5]), f12=tof(w[6]))
        k = 7
        for c in h["copy"]:
            n, off, reg = (c >> 16) & 0xFF, c & 0xFFFF, [r for r, s in SLOT.items() if s == c >> 24][0]
            ws = w[k:k + n]
            fl = [round(struct.unpack("<f", struct.pack("<I", x))[0], 4) for x in ws]
            e["%s+%#x" % (reg, off)] = {"u": [hex(x) for x in ws], "f": fl}
            k += n
        out.append(e)
    return out
