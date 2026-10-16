#!/usr/bin/env python3
"""
Standalone VU (PS2 Vector Unit) micro-mode disassembler.
Opcode tables transcribed from the PCSX2 interpreter dispatch tables
(VU_UPPER_OPCODE / VU_UPPER_FD_xx / VU_LOWER_OPCODE / LowerOP_T3_xx).
Each VU instruction = 64 bits: LOWER word at +0, UPPER word at +4 (LE).
Usage: vudis.py <file.bin> [base_instr_index]
Written for the Dirge of Cerberus port (WS-V). No external deps.
"""
import sys, struct

BC = ['x', 'y', 'z', 'w']

def dest_str(code):
    m = (code >> 21) & 0xF  # bits 24..21 = x,y,z,w
    s = ''
    if m & 0x8: s += 'x'
    if m & 0x4: s += 'y'
    if m & 0x2: s += 'z'
    if m & 0x1: s += 'w'
    return s if s else ''

def ft(c): return (c >> 16) & 0x1F
def fs(c): return (c >> 11) & 0x1F
def fd(c): return (c >> 6) & 0x1F
def it(c): return (c >> 16) & 0xF
def is_(c): return (c >> 11) & 0xF
def id_(c): return (c >> 6) & 0xF
def fsf(c): return (c >> 21) & 0x3
def ftf(c): return (c >> 23) & 0x3
def imm11(c):
    v = c & 0x7FF
    return v - 0x800 if v & 0x400 else v
def uimm11(c): return c & 0x7FF
def imm15(c):  # IADDIU/ISUBIU: bits[10:6]<<11 | bits[15:0]&0x7ff... layout: (c>>10)&0x7800 | c&0x7ff
    return (((c >> 10) & 0x7800) | (c & 0x7FF))
def imm5(c):   # IADDI signed 5-bit in bits[10:6]
    v = (c >> 6) & 0x1F
    return v - 0x20 if v & 0x10 else v

def D(c): return dest_str(c)

# ---------- UPPER ----------
def up_bc(mn, c, bc):
    return f"{mn}{BC[bc]}.{D(c)} vf{fd(c):02d}, vf{fs(c):02d}, vf{ft(c):02d}{BC[bc]}"
def up_std(mn, c):
    return f"{mn}.{D(c)} vf{fd(c):02d}, vf{fs(c):02d}, vf{ft(c):02d}"
def up_q(mn, c):
    return f"{mn}.{D(c)} vf{fd(c):02d}, vf{fs(c):02d}, Q"
def up_i(mn, c):
    return f"{mn}.{D(c)} vf{fd(c):02d}, vf{fs(c):02d}, I"

def disasm_upper(c):
    op = c & 0x3F
    flags = ''
    if c & 0x80000000: flags += ' [I]'
    if c & 0x40000000: flags += ' [E]'
    if c & 0x20000000: flags += ' [M]'
    if c & 0x10000000: flags += ' [D]'
    if c & 0x08000000: flags += ' [T]'
    def fin(s): return s + flags
    if op <= 0x1B:
        grp = ['ADD','SUB','MADD','MSUB','MAX','MINI','MUL'][op >> 2]
        return fin(up_bc(grp, c, op & 3))
    if op == 0x1C: return fin(up_q('MULq', c))
    if op == 0x1D: return fin(up_i('MAXi', c))
    if op == 0x1E: return fin(up_i('MULi', c))
    if op == 0x1F: return fin(up_i('MINIi', c))
    if op == 0x20: return fin(up_q('ADDq', c))
    if op == 0x21: return fin(up_q('MADDq', c))
    if op == 0x22: return fin(up_i('ADDi', c))
    if op == 0x23: return fin(up_i('MADDi', c))
    if op == 0x24: return fin(up_q('SUBq', c))
    if op == 0x25: return fin(up_q('MSUBq', c))
    if op == 0x26: return fin(up_i('SUBi', c))
    if op == 0x27: return fin(up_i('MSUBi', c))
    if op == 0x28: return fin(up_std('ADD', c))
    if op == 0x29: return fin(up_std('MADD', c))
    if op == 0x2A: return fin(up_std('MUL', c))
    if op == 0x2B: return fin(up_std('MAX', c))
    if op == 0x2C: return fin(up_std('SUB', c))
    if op == 0x2D: return fin(up_std('MSUB', c))
    if op == 0x2E: return fin(f"OPMSUB.{D(c)} vf{fd(c):02d}, vf{fs(c):02d}, vf{ft(c):02d}")
    if op == 0x2F: return fin(up_std('MINI', c))
    if op in (0x3C, 0x3D, 0x3E, 0x3F):
        bc = op & 3
        sub = (c >> 6) & 0x1F
        return fin(disasm_upper_ext(c, bc, sub))
    return fin(f".word 0x{c:08x}  ; unknown upper op=0x{op:02x}")

def disasm_upper_ext(c, bc, sub):
    acc = "ACC"
    # acc broadcast ops
    if sub == 0x00: return f"ADDA{BC[bc]}.{D(c)} {acc}, vf{fs(c):02d}, vf{ft(c):02d}{BC[bc]}"
    if sub == 0x01: return f"SUBA{BC[bc]}.{D(c)} {acc}, vf{fs(c):02d}, vf{ft(c):02d}{BC[bc]}"
    if sub == 0x02: return f"MADDA{BC[bc]}.{D(c)} {acc}, vf{fs(c):02d}, vf{ft(c):02d}{BC[bc]}"
    if sub == 0x03: return f"MSUBA{BC[bc]}.{D(c)} {acc}, vf{fs(c):02d}, vf{ft(c):02d}{BC[bc]}"
    if sub == 0x04:
        n = ['0','4','12','15'][bc]
        return f"ITOF{n}.{D(c)} vf{ft(c):02d}, vf{fs(c):02d}"
    if sub == 0x05:
        n = ['0','4','12','15'][bc]
        return f"FTOI{n}.{D(c)} vf{ft(c):02d}, vf{fs(c):02d}"
    if sub == 0x06: return f"MULA{BC[bc]}.{D(c)} {acc}, vf{fs(c):02d}, vf{ft(c):02d}{BC[bc]}"
    # sub 0x07+ : per-bc specials
    table = {
        (0,0x07): f"MULAq.{D(c)} {acc}, vf{fs(c):02d}, Q",
        (0,0x08): f"ADDAq.{D(c)} {acc}, vf{fs(c):02d}, Q",
        (0,0x09): f"SUBAq.{D(c)} {acc}, vf{fs(c):02d}, Q",
        (0,0x0A): f"ADDA.{D(c)} {acc}, vf{fs(c):02d}, vf{ft(c):02d}",
        (0,0x0B): f"SUBA.{D(c)} {acc}, vf{fs(c):02d}, vf{ft(c):02d}",
        (1,0x07): f"ABS.{D(c)} vf{ft(c):02d}, vf{fs(c):02d}",
        (1,0x08): f"MADDAq.{D(c)} {acc}, vf{fs(c):02d}, Q",
        (1,0x09): f"MSUBAq.{D(c)} {acc}, vf{fs(c):02d}, Q",
        (1,0x0A): f"MADDA.{D(c)} {acc}, vf{fs(c):02d}, vf{ft(c):02d}",
        (1,0x0B): f"MSUBA.{D(c)} {acc}, vf{fs(c):02d}, vf{ft(c):02d}",
        (2,0x07): f"MULAi.{D(c)} {acc}, vf{fs(c):02d}, I",
        (2,0x08): f"ADDAi.{D(c)} {acc}, vf{fs(c):02d}, I",
        (2,0x09): f"SUBAi.{D(c)} {acc}, vf{fs(c):02d}, I",
        (2,0x0A): f"MULA.{D(c)} {acc}, vf{fs(c):02d}, vf{ft(c):02d}",
        (2,0x0B): f"OPMULA.{D(c)} {acc}, vf{fs(c):02d}, vf{ft(c):02d}",
        (3,0x07): f"CLIP vf{fs(c):02d}xyz, vf{ft(c):02d}w",
        (3,0x08): f"MADDAi.{D(c)} {acc}, vf{fs(c):02d}, I",
        (3,0x09): f"MSUBAi.{D(c)} {acc}, vf{fs(c):02d}, I",
        (3,0x0B): "NOP",
    }
    return table.get((bc, sub), f".word 0x{c:08x}  ; unknown upper-ext bc={bc} sub=0x{sub:02x}")

# ---------- LOWER ----------
def disasm_lower(c, pc_idx):
    op = (c >> 25) & 0x7F
    if op == 0x00: return f"LQ.{D(c)} vf{ft(c):02d}, 0x{uimm11(c)*16:x}(vi{is_(c):02d})"
    if op == 0x01: return f"SQ.{D(c)} vf{fs(c):02d}, 0x{uimm11(c)*16:x}(vi{it(c):02d})"
    if op == 0x04: return f"ILW.{D(c)} vi{it(c):02d}, 0x{imm11(c):x}(vi{is_(c):02d})"
    if op == 0x05: return f"ISW.{D(c)} vi{it(c):02d}, 0x{imm11(c):x}(vi{is_(c):02d})"
    if op == 0x08: return f"IADDIU vi{it(c):02d}, vi{is_(c):02d}, 0x{imm15(c):x}"
    if op == 0x09: return f"ISUBIU vi{it(c):02d}, vi{is_(c):02d}, 0x{imm15(c):x}"
    if op == 0x10: return f"FCEQ 0x{c & 0xFFFFFF:06x}"
    if op == 0x11: return f"FCSET 0x{c & 0xFFFFFF:06x}"
    if op == 0x12: return f"FCAND 0x{c & 0xFFFFFF:06x}"
    if op == 0x13: return f"FCOR 0x{c & 0xFFFFFF:06x}"
    if op == 0x14: return f"FSEQ vi{it(c):02d}, 0x{c & 0xFFF:03x}"
    if op == 0x15: return f"FSSET 0x{c & 0xFFF:03x}"
    if op == 0x16: return f"FSAND vi{it(c):02d}, 0x{c & 0xFFF:03x}"
    if op == 0x17: return f"FSOR vi{it(c):02d}, 0x{c & 0xFFF:03x}"
    if op == 0x18: return f"FMEQ vi{it(c):02d}, vi{is_(c):02d}"
    if op == 0x1A: return f"FMAND vi{it(c):02d}, vi{is_(c):02d}"
    if op == 0x1B: return f"FMOR vi{it(c):02d}, vi{is_(c):02d}"
    if op == 0x1C: return f"FCGET vi{it(c):02d}"
    if op == 0x20: return f"B 0x{ (pc_idx+1+imm11(c)) :x}  ; -> instr {pc_idx+1+imm11(c)}"
    if op == 0x21: return f"BAL vi{it(c):02d}, 0x{(pc_idx+1+imm11(c)):x}  ; -> instr {pc_idx+1+imm11(c)}"
    if op == 0x24: return f"JR vi{is_(c):02d}"
    if op == 0x25: return f"JALR vi{it(c):02d}, vi{is_(c):02d}"
    if op == 0x28: return f"IBEQ vi{it(c):02d}, vi{is_(c):02d}, -> instr {pc_idx+1+imm11(c)}"
    if op == 0x29: return f"IBNE vi{it(c):02d}, vi{is_(c):02d}, -> instr {pc_idx+1+imm11(c)}"
    if op == 0x2C: return f"IBLTZ vi{is_(c):02d}, -> instr {pc_idx+1+imm11(c)}"
    if op == 0x2D: return f"IBGTZ vi{is_(c):02d}, -> instr {pc_idx+1+imm11(c)}"
    if op == 0x2E: return f"IBLEZ vi{is_(c):02d}, -> instr {pc_idx+1+imm11(c)}"
    if op == 0x2F: return f"IBGEZ vi{is_(c):02d}, -> instr {pc_idx+1+imm11(c)}"
    if op == 0x40: return disasm_lower_op(c)
    return f".word 0x{c:08x}  ; unknown lower op=0x{op:02x}"

def disasm_lower_op(c):
    op = c & 0x3F
    if op == 0x30: return f"IADD vi{id_(c):02d}, vi{is_(c):02d}, vi{it(c):02d}"
    if op == 0x31: return f"ISUB vi{id_(c):02d}, vi{is_(c):02d}, vi{it(c):02d}"
    if op == 0x32: return f"IADDI vi{it(c):02d}, vi{is_(c):02d}, {imm5(c)}"
    if op == 0x34: return f"IAND vi{id_(c):02d}, vi{is_(c):02d}, vi{it(c):02d}"
    if op == 0x35: return f"IOR vi{id_(c):02d}, vi{is_(c):02d}, vi{it(c):02d}"
    if op in (0x3C, 0x3D, 0x3E, 0x3F):
        return disasm_lower_t3(c, op & 3)
    return f".word 0x{c:08x}  ; unknown lowerOP op=0x{op:02x}"

def disasm_lower_t3(c, t):
    sub = (c >> 6) & 0x1F
    FS=fs(c); FT=ft(c); IS=is_(c); IT=it(c); ID=id_(c)
    def sf(i): return ['x','y','z','w'][i]
    T = {
     (0,0x0C): f"MOVE.{D(c)} vf{FT:02d}, vf{FS:02d}",
     (0,0x0D): f"LQI.{D(c)} vf{FT:02d}, (vi{IS:02d}++)",
     (0,0x0E): f"DIV Q, vf{FS:02d}{sf(fsf(c))}, vf{FT:02d}{sf(ftf(c))}",
     (0,0x0F): f"MTIR vi{IT:02d}, vf{FS:02d}{sf(fsf(c))}",
     (0,0x10): f"RNEXT.{D(c)} vf{FT:02d}, R",
     (0,0x19): f"MFP.{D(c)} vf{FT:02d}, P",
     (0,0x1A): f"XTOP vi{IT:02d}",
     (0,0x1B): f"XGKICK vi{IS:02d}",
     (0,0x1C): f"ESADD P, vf{FS:02d}",
     (0,0x1D): f"EATANxy P, vf{FS:02d}",
     (0,0x1E): f"ESQRT P, vf{FS:02d}{sf(fsf(c))}",
     (0,0x1F): f"ESIN P, vf{FS:02d}{sf(fsf(c))}",
     (1,0x0C): f"MR32.{D(c)} vf{FT:02d}, vf{FS:02d}",
     (1,0x0D): f"SQI.{D(c)} vf{FS:02d}, (vi{IT:02d}++)",
     (1,0x0E): f"SQRT Q, vf{FT:02d}{sf(ftf(c))}",
     (1,0x0F): f"MFIR.{D(c)} vf{FT:02d}, vi{IS:02d}",
     (1,0x10): f"RGET.{D(c)} vf{FT:02d}, R",
     (1,0x1A): f"XITOP vi{IT:02d}",
     (1,0x1C): f"ERSADD P, vf{FS:02d}",
     (1,0x1D): f"EATANxz P, vf{FS:02d}",
     (1,0x1E): f"ERSQRT P, vf{FS:02d}{sf(fsf(c))}",
     (1,0x1F): f"EATAN P, vf{FS:02d}",
     (2,0x0D): f"LQD.{D(c)} vf{FT:02d}, (--vi{IS:02d})",
     (2,0x0E): f"RSQRT Q, vf{FS:02d}{sf(fsf(c))}, vf{FT:02d}{sf(ftf(c))}",
     (2,0x0F): f"ILWR.{D(c)} vi{IT:02d}, (vi{IS:02d})",
     (2,0x10): f"RINIT R, vf{FS:02d}{sf(fsf(c))}",
     (2,0x1C): f"ELENG P, vf{FS:02d}",
     (2,0x1D): f"ESUM P, vf{FS:02d}",
     (2,0x1E): f"ERCPR P, vf{FS:02d}{sf(fsf(c))}",
     (2,0x1F): f"EEXP P, vf{FS:02d}{sf(fsf(c))}",
     (3,0x0D): f"SQD.{D(c)} vf{FS:02d}, (--vi{IT:02d})",
     (3,0x0E): f"WAITQ",
     (3,0x0F): f"ISWR.{D(c)} vi{IT:02d}, (vi{IS:02d})",
     (3,0x10): f"RXOR R, vf{FS:02d}{sf(fsf(c))}",
     (3,0x1C): f"ERLENG P, vf{FS:02d}",
     (3,0x1E): f"WAITP",
    }
    return T.get((t, sub), f".word 0x{c:08x}  ; unknown T3 t={t} sub=0x{sub:02x}")

def main():
    path = sys.argv[1]
    base = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    data = open(path, 'rb').read()
    n = len(data) // 8
    print(f"; {path}  ({len(data)} bytes, {n} VU instruction pairs)")
    print(f"; idx  addr    UPPER (FMAC/FDIV pipe)                 | LOWER (load/store/int/branch/EFU)")
    for i in range(n):
        lo, hi = struct.unpack_from('<II', data, i*8)
        up = disasm_upper(hi)
        lw = disasm_lower(lo, i)
        print(f"{i:4d} {i*8:#06x}  {up:40s} | {lw}")

if __name__ == '__main__':
    main()
