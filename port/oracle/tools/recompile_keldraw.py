#!/usr/bin/env python3
# =====================================================================
# recompile.py -- R5900 -> C static recompiler for the EE ELF (WS-R-scale)
#
# Lifts the FULL integer + load/store + control + MMI + COP1(FPU) + COP2
# (VU0 macro) instruction set of the Dirge-of-Cerberus EE executable to C
# emitted against the frozen contract in ee_runtime.h. VU0 macro ops are
# emitted as ee_cop2_* intrinsic calls (WS-V owns their bodies); COP0 and
# cache/sync/trap ops are lowered to the nearest faithful no-op. Anything
# still outside the set becomes an ee_trap_unimpl() call -- honest failure,
# counted by class so the residual is auditable.
#
# Modes:
#   recompile.py <ELF> <addr_hex> [more...]        # legacy: C to stdout
#   recompile.py emit --elf E --csv C --outdir D [--tu-size N] [--limit N]
#       # whole-program: writes generated/eetu_*.c, function_table.c,
#       #   ee_funcs.h, unimpl_report.txt, manifest.txt
# =====================================================================
import sys, struct, csv, os, argparse
from collections import Counter

TEXT_VA, TEXT_OFF = 0x00100000, 0x100   # .text: VA 0x100000 at file off 0x100
TEXT_END_VA = 0x005c0000                # PATCHED: cover .kel_draw_section

import struct as _st
_SEGS=[]
def load_elf(path):
    with open(path, 'rb') as f: data=f.read()
    _SEGS.clear()
    e_phoff=_st.unpack_from('<I',data,28)[0]
    e_phentsize=_st.unpack_from('<H',data,42)[0]
    e_phnum=_st.unpack_from('<H',data,44)[0]
    for i in range(e_phnum):
        ph=e_phoff+i*e_phentsize
        p_type,p_off,p_va,_pa,p_fsz,p_msz=_st.unpack_from('<IIIIII',data,ph)
        if p_type==1 and p_fsz>0:
            _SEGS.append((p_va,p_va+p_fsz,p_off))
    return data
def va2off(va):
    for vlo,vhi,off in _SEGS:
        if vlo<=va<vhi: return off+(va-vlo)
    return va - TEXT_VA + TEXT_OFF  # fallback (first-seg linear)
def word(data, va): return struct.unpack_from('<I', data, va2off(va))[0]
def sx16(x):
    x &= 0xffff
    return x - 0x10000 if x & 0x8000 else x

GPR = ['zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3',
       't4','t5','t6','t7','s0','s1','s2','s3','s4','s5','s6','s7',
       't8','t9','k0','k1','gp','sp','fp','ra']

# ----- VU0 macro decode tables (from PS2Recomp instructions.h) -----------
VU_S1 = {}  # special1_func (fn & 0x3f) -> intrinsic mnemonic
for f in range(0x00,0x04): VU_S1[f]='vaddbc'
for f in range(0x04,0x08): VU_S1[f]='vsubbc'
for f in range(0x08,0x0c): VU_S1[f]='vmaddbc'
for f in range(0x0c,0x10): VU_S1[f]='vmsubbc'
for f in range(0x10,0x14): VU_S1[f]='vmaxbc'
for f in range(0x14,0x18): VU_S1[f]='vminibc'
for f in range(0x18,0x1c): VU_S1[f]='vmulbc'
VU_S1.update({0x1c:'vmulq',0x1d:'vmaxi',0x1e:'vmuli',0x1f:'vminii',
  0x20:'vaddq',0x21:'vmaddq',0x22:'vaddi',0x23:'vmaddi',0x24:'vsubq',
  0x25:'vmsubq',0x26:'vsubi',0x27:'vmsubi',0x28:'vadd',0x29:'vmadd',
  0x2a:'vmul',0x2b:'vmax',0x2c:'vsub',0x2d:'vmsub',0x2e:'vopmsub',
  0x2f:'vmini',0x30:'viadd',0x31:'visub',0x32:'viaddi',0x34:'viand',
  0x35:'vior',0x38:'vcallms',0x39:'vcallmsr'})
VU_S2 = {}  # vu_func = ((raw>>6)&0x1f)<<2 | (raw&3)  -> mnemonic
for f in range(0x00,0x04): VU_S2[f]='vaddabc'
for f in range(0x04,0x08): VU_S2[f]='vsubabc'
for f in range(0x08,0x0c): VU_S2[f]='vmaddabc'
for f in range(0x0c,0x10): VU_S2[f]='vmsubabc'
for f in range(0x10,0x14): VU_S2[f]='vitof'
for f in range(0x14,0x18): VU_S2[f]='vftoi'
for f in range(0x18,0x1c): VU_S2[f]='vmulabc'
VU_S2.update({0x1c:'vmulaq',0x1d:'vabs',0x1e:'vmulai',0x1f:'vclip',
  0x20:'vaddaq',0x21:'vmaddaq',0x22:'vaddai',0x23:'vmaddai',0x24:'vsubaq',
  0x25:'vmsubaq',0x26:'vsubai',0x27:'vmsubai',0x28:'vadda',0x29:'vmadda',
  0x2a:'vmula',0x2c:'vsuba',0x2d:'vmsuba',0x2e:'vopmula',0x2f:'vnop',
  0x30:'vmove',0x31:'vmr32',0x34:'vlqi',0x35:'vsqi',0x36:'vlqd',0x37:'vsqd',
  0x38:'vdiv',0x39:'vsqrt',0x3a:'vrsqrt',0x3b:'vwaitq',0x3c:'vmtir',
  0x3d:'vmfir',0x3e:'vilwr',0x3f:'viswr',0x40:'vrnext',0x41:'vrget',
  0x42:'vrinit',0x43:'vrxor'})

# SIF/IOP library functions that must NOT be recompiled -- they are routed to
# the sif/ HLE (WS-K). func_<addr> is #defined to the hook in ee_funcs.h so both
# direct JAL calls and the indirect function table reach the HLE entry point.
HLE_ROUTES = {
    0x00453d20: 'sif_call_rpc',        # sceSifCallRpc
    0x004535d8: 'sif_bind_rpc',        # sceSifBindRpc
    0x004517f0: 'sif_reboot_iop',      # RebootIop
    0x00451958: 'sif_sync_iop',        # SyncIop
    0x00453360: 'sif_writeback_dcache',# WriteBackDCache
    0x00453b50: 'sif_bind_server',     # sceSifBindRpc (bind an IOP RPC server)
}

class Insn:
    __slots__=('va','raw','op','rs','rt','rd','sa','fn','imm','simm','tgt')
    def __init__(self, va, raw):
        self.va=va; self.raw=raw
        self.op=raw>>26; self.rs=(raw>>21)&31; self.rt=(raw>>16)&31
        self.rd=(raw>>11)&31; self.sa=(raw>>6)&31; self.fn=raw&63
        self.imm=raw&0xffff; self.simm=sx16(raw)
        self.tgt=(raw&0x3ffffff)

# ---- control-flow classification ------------------------------------
def branch_target(i):
    op=i.op
    if op==0:
        if i.fn==0x08: return ('jr',None)
        if i.fn==0x09: return ('jalr',None)
        # NB: SYSCALL/BREAK are synchronous exceptions, NOT branches -- they
        # have no delay slot, so they are lowered as straight-line statements
        # in translate_alu (the instruction after them executes normally).
        return None
    if op==1:
        m={0:'bltz',1:'bgez',2:'bltzl',3:'bgezl',16:'bltzal',17:'bgezal',
           18:'bltzall',19:'bgezall'}.get(i.rt)
        if m: return (m, i.va+4+i.simm*4)
        return None
    if op==2: return ('j',  (i.va & 0xf0000000) | (i.tgt<<2))
    if op==3: return ('jal',(i.va & 0xf0000000) | (i.tgt<<2))
    if op in (4,5,6,7):
        return ({4:'beq',5:'bne',6:'blez',7:'bgtz'}[op], i.va+4+i.simm*4)
    if op in (0x14,0x15,0x16,0x17):
        return ({0x14:'beql',0x15:'bnel',0x16:'blezl',0x17:'bgtzl'}[op], i.va+4+i.simm*4)
    if op==0x11 and i.rs==8:  # BC1F/BC1T/BC1FL/BC1TL
        k='bc1'+('t' if (i.rt&1) else 'f')+('l' if (i.rt&2) else '')
        return (k, i.va+4+i.simm*4)
    if op==0x12 and i.rs==8:  # BC2* (not present in this ELF)
        k='bc2'+('t' if (i.rt&1) else 'f')+('l' if (i.rt&2) else '')
        return (k, i.va+4+i.simm*4)
    return None

LIKELY={'beql','bnel','blezl','bgtzl','bltzl','bgezl','bltzall','bgezall',
        'bc1tl','bc1fl','bc2tl','bc2fl'}

# ---- register expression helpers ------------------------------------
def r32(n):  return "0u" if n==0 else f"EE_GET32(ee,{n})"
def rs32(n): return "0" if n==0 else f"EE_GETS32(ee,{n})"
def r64(n):  return "0ull" if n==0 else f"EE_GET64(ee,{n})"
def rs64(n): return "0ll" if n==0 else f"EE_GETS64(ee,{n})"
def set32(n,e): return "" if n==0 else f"EE_SET32(ee,{n},(uint32_t)({e}));"
def set64(n,e): return "" if n==0 else f"EE_SET64(ee,{n},(uint64_t)({e}));"

class Ctx:
    """Per-emit accumulators (unimpl tally + generated func-start set)."""
    def __init__(self, func_starts):
        self.func_starts = func_starts
        self.unimpl = Counter()

def trap(ctx, i, cls):
    ctx.unimpl[cls]+=1
    return f'ee_trap_unimpl(ee,0x{i.va:08x},0x{i.raw:08x},"{cls}");'

# ---- MMI (op 0x1c) ---------------------------------------------------
MMI0={0x00:'paddw',0x01:'psubw',0x02:'pcgtw',0x03:'pmaxw',0x04:'paddh',
 0x05:'psubh',0x06:'pcgth',0x07:'pmaxh',0x08:'paddb',0x09:'psubb',0x0a:'pcgtb',
 0x10:'paddsw',0x11:'psubsw',0x12:'pextlw',0x13:'ppacw',0x14:'paddsh',
 0x15:'psubsh',0x16:'pextlh',0x17:'ppach',0x18:'paddsb',0x19:'psubsb',
 0x1a:'pextlb',0x1b:'ppacb'}
MMI1={0x01:'pabsw',0x02:'pceqw',0x03:'pminw',0x04:'padsbh',0x05:'pabsh',
 0x06:'pceqh',0x07:'pminh',0x0a:'pceqb',0x10:'padduw',0x11:'psubuw',
 0x12:'pextuw',0x14:'padduh',0x15:'psubuh',0x16:'pextuh',0x18:'paddub',
 0x19:'psubub',0x1a:'pextub'}  # 0x1b qfsrv handled specially
MMI2_VEC={0x02:'psllvw',0x03:'psrlvw',0x0a:'pinth',0x0e:'pcpyld',0x12:'pand',
 0x13:'pxor',0x1a:'pexeh',0x1b:'prevh',0x1e:'pexew',0x1f:'prot3w'}
MMI2_HILO={0x00:'pmaddw',0x04:'pmsubw',0x0c:'pmultw',0x0d:'pdivw',0x10:'pmaddh',
 0x11:'phmadh',0x14:'pmsubh',0x15:'phmsbh',0x1c:'pmulth',0x1d:'pdivbw'}
MMI3_VEC={0x0a:'pinteh',0x0e:'pcpyud',0x12:'por',0x13:'pnor',0x1a:'pexch',0x1b:'pcpyh',0x1e:'pexcw'}
MMI3_HILO={0x00:'pmadduw',0x03:'psravw',0x0c:'pmultuw',0x0d:'pdivuw'}
# unary vector ops (one source = rt)
MMI_UNARY={'pabsw','pabsh','pcpyh','pexeh','prevh','pexew','prot3w','pinteh','pexch','pexcw'}

def translate_mmi(ctx,i):
    fn=i.fn; rd,rs,rt,sa=i.rd,i.rs,i.rt,i.sa
    def vec(mn):
        if mn in MMI_UNARY and mn not in ('pinteh','pexcw'):  # true 1-src
            return "" if rd==0 else f"ee_mmi_{mn}(&ee->r[{rd}],&ee->r[{rt}]);"
        return "" if rd==0 else f"ee_mmi_{mn}(&ee->r[{rd}],&ee->r[{rs}],&ee->r[{rt}]);"
    if fn==0x00: return (f"{{ int64_t _a=EE__HILO(ee)+(int64_t){rs32(rs)}*(int64_t){rs32(rt)};"
                         f" ee__setlo32(ee,_a); {set32(rd,'(uint32_t)_a') if rd else ''} }}")  # madd
    if fn==0x01: return (f"{{ int64_t _a=EE__HILO(ee)+(int64_t)(uint64_t)((uint64_t){r32(rs)}*(uint64_t){r32(rt)});"
                         f" ee__setlo32(ee,_a); {set32(rd,'(uint32_t)_a') if rd else ''} }}")  # maddu
    if fn==0x02: return (f"{{ int64_t _a=EE__HILO(ee)-(int64_t){rs32(rs)}*(int64_t){rs32(rt)};"
                         f" ee__setlo32(ee,_a); {set32(rd,'(uint32_t)_a') if rd else ''} }}")  # msub
    if fn==0x03: return (f"{{ int64_t _a=EE__HILO(ee)-(int64_t)(uint64_t)((uint64_t){r32(rs)}*(uint64_t){r32(rt)});"
                         f" ee__setlo32(ee,_a); {set32(rd,'(uint32_t)_a') if rd else ''} }}")  # msubu
    if fn==0x04:  # plzcw
        if rd==0: return "/* plzcw rd0 */"
        return (f"EE_SET64(ee,{rd},((uint64_t)ee_plzcw32({r32(rs)})"
                f"|((uint64_t)ee_plzcw32((uint32_t)(EE_GET64(ee,{rs})>>32))<<32)));")
    if fn==0x08:  # MMI0
        mn=MMI0.get(sa)
        return vec(mn) if mn else trap(ctx,i,f"mmi0.sa{sa:#04x}")
    if fn==0x28:  # MMI1
        if sa==0x1b: return "" if rd==0 else f"ee_mmi_qfsrv(&ee->r[{rd}],&ee->r[{rs}],&ee->r[{rt}],ee->sa);"
        mn=MMI1.get(sa)
        if mn in ('pabsw','pabsh'): return "" if rd==0 else f"ee_mmi_{mn}(&ee->r[{rd}],&ee->r[{rt}]);"
        return vec(mn) if mn else trap(ctx,i,f"mmi1.sa{sa:#04x}")
    if fn==0x09:  # MMI2
        if sa in MMI2_VEC: return vec(MMI2_VEC[sa])
        if sa==0x08: return "" if rd==0 else f"ee->r[{rd}]=ee->hi;"       # pmfhi
        if sa==0x09: return "" if rd==0 else f"ee->r[{rd}]=ee->lo;"       # pmflo
        if sa in MMI2_HILO: return f"ee_mmi_{MMI2_HILO[sa]}(ee,{rd},{rs},{rt});"
        return trap(ctx,i,f"mmi2.sa{sa:#04x}")
    if fn==0x29:  # MMI3
        if sa in MMI3_VEC:
            mn=MMI3_VEC[sa]
            if mn in ('pcpyh','pexch','pexcw'): return "" if rd==0 else f"ee_mmi_{mn}(&ee->r[{rd}],&ee->r[{rt}]);"
            return vec(mn)
        if sa==0x08: return f"ee->hi=ee->r[{rs}];"                        # pmthi
        if sa==0x09: return f"ee->lo=ee->r[{rs}];"                        # pmtlo
        if sa in MMI3_HILO:
            if sa==0x03: return vec('psravw')
            return f"ee_mmi_{MMI3_HILO[sa]}(ee,{rd},{rs},{rt});"
        return trap(ctx,i,f"mmi3.sa{sa:#04x}")
    if fn==0x10: return set64(rd,"ee->hi.ud[1]")                          # mfhi1
    if fn==0x11: return f"ee->hi.ud[1]={r64(rs)};"                        # mthi1
    if fn==0x12: return set64(rd,"ee->lo.ud[1]")                          # mflo1
    if fn==0x13: return f"ee->lo.ud[1]={r64(rs)};"                        # mtlo1
    if fn==0x18: return (f"{{ int64_t _p=(int64_t){rs32(rs)}*(int64_t){rs32(rt)};"
                         f" ee->lo.sd[1]=(int32_t)_p; ee->hi.sd[1]=(int32_t)(_p>>32);"
                         f" {set32(rd,'(uint32_t)_p') if rd else ''} }}")  # mult1
    if fn==0x19: return (f"{{ uint64_t _p=(uint64_t){r32(rs)}*(uint64_t){r32(rt)};"
                         f" ee->lo.sd[1]=(int32_t)_p; ee->hi.sd[1]=(int32_t)(_p>>32);"
                         f" {set32(rd,'(uint32_t)_p') if rd else ''} }}")  # multu1
    if fn==0x1a: return (f"{{ int32_t _a={rs32(rs)},_b={rs32(rt)}; if(_b){{ if(_b==-1&&_a==INT32_MIN){{ee->lo.sd[1]=INT32_MIN;ee->hi.sd[1]=0;}}"
                         f" else{{ee->lo.sd[1]=_a/_b;ee->hi.sd[1]=_a%_b;}} }} }}")  # div1
    if fn==0x1b: return (f"{{ uint32_t _a={r32(rs)},_b={r32(rt)}; if(_b){{ ee->lo.sd[1]=(int32_t)(_a/_b); ee->hi.sd[1]=(int32_t)(_a%_b); }} }}")  # divu1
    if fn==0x20: return (f"{{ int64_t _a=(int64_t)(((uint64_t)ee->hi.uw[2]<<32)|ee->lo.uw[2])+(int64_t){rs32(rs)}*(int64_t){rs32(rt)};"
                         f" ee->lo.sd[1]=(int32_t)(uint32_t)_a; ee->hi.sd[1]=(int32_t)(uint32_t)((uint64_t)_a>>32); {set32(rd,'(uint32_t)_a') if rd else ''} }}")  # madd1
    if fn==0x21: return (f"{{ int64_t _a=(int64_t)(((uint64_t)ee->hi.uw[2]<<32)|ee->lo.uw[2])+(int64_t)(uint64_t)((uint64_t){r32(rs)}*(uint64_t){r32(rt)});"
                         f" ee->lo.sd[1]=(int32_t)(uint32_t)_a; ee->hi.sd[1]=(int32_t)(uint32_t)((uint64_t)_a>>32); {set32(rd,'(uint32_t)_a') if rd else ''} }}")  # maddu1
    if fn==0x30: return f"ee_mmi_pmfhl(ee,{rd},{sa}u);"                   # pmfhl
    if fn==0x31:
        if sa==0x00: return f"ee_mmi_pmthl_lw(ee,{rs});"                  # pmthl.lw
        return trap(ctx,i,f"pmthl.sa{sa:#04x}")
    if fn==0x34: return "" if rd==0 else f"ee_mmi_psllh(&ee->r[{rd}],&ee->r[{rt}],{sa}u);"
    if fn==0x36: return "" if rd==0 else f"ee_mmi_psrlh(&ee->r[{rd}],&ee->r[{rt}],{sa}u);"
    if fn==0x37: return "" if rd==0 else f"ee_mmi_psrah(&ee->r[{rd}],&ee->r[{rt}],{sa}u);"
    if fn==0x3c: return "" if rd==0 else f"ee_mmi_psllw(&ee->r[{rd}],&ee->r[{rt}],{sa}u);"
    if fn==0x3e: return "" if rd==0 else f"ee_mmi_psrlw(&ee->r[{rd}],&ee->r[{rt}],{sa}u);"
    if fn==0x3f: return "" if rd==0 else f"ee_mmi_psraw(&ee->r[{rd}],&ee->r[{rt}],{sa}u);"
    return trap(ctx,i,f"mmi.fn{fn:#04x}")

# ---- COP0 (op 0x10): kernel regs / TLB / EI/DI -- lowered to no-ops ----
def translate_cop0(ctx,i):
    rs=i.rs; rt=i.rt; rd=i.rd; fn=i.fn
    if rs==0x00: return set32(rt,f"ee_cop0_mfc0(ee,{rd})")   # mfc0
    if rs==0x04: return f"ee_cop0_mtc0(ee,{rd},{r32(rt)});"  # mtc0
    if rs==0x10:  # CO: EI/DI/ERET/TLB
        if fn==0x38: return "ee_ei(ee);"
        if fn==0x39: return "ee_di(ee);"
        if fn==0x18: return "ee_eret(ee);"
        return "/* cop0 tlb op (nop; WS-K) */"
    if rs==0x08: return "/* bc0 (nop) */"
    return trap(ctx,i,f"cop0.rs{rs:#04x}")

# ---- COP1 (op 0x11): single-precision FPU ------------------------------
COP1S_BIN={0x00:'ee_fadd',0x01:'ee_fsub',0x02:'ee_fmul',0x28:'ee_fmax',0x29:'ee_fmin'}
COP1S_UN={0x04:'ee_fsqrt',0x05:'ee_fabs',0x07:'ee_fneg',
 0x0c:'ee_round_w_s',0x0d:'ee_trunc_w_s',0x0e:'ee_ceil_w_s',0x0f:'ee_floor_w_s',0x24:'ee_cvt_w_s'}
def translate_cop1(ctx,i):
    rs=i.rs; ft=i.rt; fs=i.rd; fd=i.sa; fn=i.fn
    if rs==0x00: return set32(ft,f"ee->f[{fs}]")              # mfc1
    if rs==0x02:                                              # cfc1
        src = "ee->fcr31" if fs==31 else ("ee->fcr0" if fs==0 else "0")
        return set32(ft,src)
    if rs==0x04: return f"ee->f[{fs}]={r32(ft)};"             # mtc1
    if rs==0x06:                                              # ctc1
        if fs==31: return f"ee->fcr31={r32(ft)};"
        return f"/* ctc1 fcr{fs} (ignored) */"
    if rs==0x10:  # single-precision ops
        if fn in COP1S_BIN: return f"ee->f[{fd}]={COP1S_BIN[fn]}(ee->f[{fs}],ee->f[{ft}]);"
        if fn in COP1S_UN:  return f"ee->f[{fd}]={COP1S_UN[fn]}(ee->f[{fs}]);"
        if fn==0x06: return f"ee->f[{fd}]=ee->f[{fs}];"                   # mov.s
        if fn==0x03: return f"ee->f[{fd}]=ee_fdiv(ee->f[{fs}],ee->f[{ft}]);"
        if fn==0x16: return f"ee->f[{fd}]=ee_frsqrt(ee->f[{fs}],ee->f[{ft}]);"
        if fn==0x18: return f"ee->facc=ee_fadd(ee->f[{fs}],ee->f[{ft}]);"  # adda
        if fn==0x19: return f"ee->facc=ee_fsub(ee->f[{fs}],ee->f[{ft}]);"  # suba
        if fn==0x1a: return f"ee->facc=ee_fmul(ee->f[{fs}],ee->f[{ft}]);"  # mula
        if fn==0x1c: return f"ee->f[{fd}]=ee_fadd(ee->facc,ee_fmul(ee->f[{fs}],ee->f[{ft}]));"  # madd
        if fn==0x1d: return f"ee->f[{fd}]=ee_fsub(ee->facc,ee_fmul(ee->f[{fs}],ee->f[{ft}]));"  # msub
        if fn==0x1e: return f"ee->facc=ee_fadd(ee->facc,ee_fmul(ee->f[{fs}],ee->f[{ft}]));"      # madda
        if fn==0x1f: return f"ee->facc=ee_fsub(ee->facc,ee_fmul(ee->f[{fs}],ee->f[{ft}]));"      # msuba
        if fn in (0x30,0x38): return "ee_fset_c(ee,0);"                   # c.f / c.sf
        if fn in (0x31,0x39): return "ee_fset_c(ee,0);"                   # c.un/c.ngle (no NaN)
        if fn in (0x32,0x33,0x3a,0x3b): return f"ee_fset_c(ee,ee_fc_eq(ee->f[{fs}],ee->f[{ft}]));"
        if fn in (0x34,0x35,0x3c,0x3d): return f"ee_fset_c(ee,ee_fc_lt(ee->f[{fs}],ee->f[{ft}]));"
        if fn in (0x36,0x37,0x3e,0x3f): return f"ee_fset_c(ee,ee_fc_le(ee->f[{fs}],ee->f[{ft}]));"
        return trap(ctx,i,f"cop1.s.fn{fn:#04x}")
    if rs==0x14:  # word -> single
        if fn==0x20: return f"ee->f[{fd}]=ee_cvt_s_w(ee->f[{fs}]);"       # cvt.s.w
        return trap(ctx,i,f"cop1.w.fn{fn:#04x}")
    return trap(ctx,i,f"cop1.rs{rs:#04x}")

# ---- COP2 (op 0x12): VU0 macro mode ------------------------------------
def translate_cop2(ctx,i):
    rs=i.rs; rt=i.rt; rd=i.rd
    if rs==0x01:  # qmfc2  vf=rd -> gpr=rt
        if rt==0: return "/* qmfc2 rt0 */"
        return ("{ "+" ".join(f"ee->r[{rt}].uw[{l}]=ee_cop2_qmfc2(ee,{rd},{l});" for l in range(4))+" }")
    if rs==0x05:  # qmtc2  gpr=rt -> vf=rd
        src=(lambda l: "0u") if rt==0 else (lambda l: f"ee->r[{rt}].uw[{l}]")
        return "{ "+" ".join(f"ee_cop2_qmtc2(ee,{rd},{l},{src(l)});" for l in range(4))+" }"
    if rs==0x02: return set32(rt,f"ee_cop2_cfc2(ee,{rd})")    # cfc2
    if rs==0x06: return f"ee_cop2_ctc2(ee,{rd},{r32(rt)});"   # ctc2
    if rs>=0x10:  # CO macro op
        s1=i.fn&0x3f
        if s1>=0x3c:
            vf=(((i.raw>>6)&0x1f)<<2)|(i.raw&3)
            mn=VU_S2.get(vf)
            if mn: return f"ee_cop2_{mn}(ee,0x{i.raw:08x}u);"
            return trap(ctx,i,f"cop2.s2.{vf:#04x}")
        mn=VU_S1.get(s1)
        if mn: return f"ee_cop2_{mn}(ee,0x{i.raw:08x}u);"
        return trap(ctx,i,f"cop2.s1.{s1:#04x}")
    return trap(ctx,i,f"cop2.rs{rs:#04x}")

# ---- translate a NON-control instruction -----------------------------
def translate_alu(ctx,i):
    op=i.op
    if op==0x1c: return translate_mmi(ctx,i)
    if op==0x10: return translate_cop0(ctx,i)
    if op==0x11: return translate_cop1(ctx,i)
    if op==0x12: return translate_cop2(ctx,i)
    if op==0:  # SPECIAL
        fn=i.fn; rd,rs,rt,sa=i.rd,i.rs,i.rt,i.sa
        if i.raw==0: return "/* nop */"
        T={
            0x00:set32(rd,f"{r32(rt)} << {sa}"),
            0x02:set32(rd,f"{r32(rt)} >> {sa}"),
            0x03:set32(rd,f"{rs32(rt)} >> {sa}"),
            0x04:set32(rd,f"{r32(rt)} << ({r32(rs)} & 31)"),
            0x06:set32(rd,f"{r32(rt)} >> ({r32(rs)} & 31)"),
            0x07:set32(rd,f"{rs32(rt)} >> ({r32(rs)} & 31)"),
            0x0a:f"if ({r64(rt)}==0) {{ {set64(rd,r64(rs))} }}",
            0x0b:f"if ({r64(rt)}!=0) {{ {set64(rd,r64(rs))} }}",
            0x0f:f"ee_sync(ee,{sa}u);",
            0x10:set64(rd,"ee->hi.ud[0]"),
            0x11:f"ee->hi.ud[0]={r64(rs)};",
            0x12:set64(rd,"ee->lo.ud[0]"),
            0x13:f"ee->lo.ud[0]={r64(rs)};",
            0x14:set64(rd,f"{r64(rt)} << ({r32(rs)} & 63)"),
            0x16:set64(rd,f"{r64(rt)} >> ({r32(rs)} & 63)"),
            0x17:set64(rd,f"{rs64(rt)} >> ({r32(rs)} & 63)"),
            0x18:(f"{{ int64_t _p=(int64_t){rs32(rs)}*(int64_t){rs32(rt)};"
                  f" ee->lo.sd[0]=(int32_t)_p; ee->hi.sd[0]=(int32_t)(_p>>32);"
                  f" {set32(rd,'(uint32_t)_p') if rd else ''} }}"),
            0x19:(f"{{ uint64_t _p=(uint64_t){r32(rs)}*(uint64_t){r32(rt)};"
                  f" ee->lo.sd[0]=(int32_t)_p; ee->hi.sd[0]=(int32_t)(_p>>32);"
                  f" {set32(rd,'(uint32_t)_p') if rd else ''} }}"),
            0x1a:(f"{{ int32_t _a={rs32(rs)},_b={rs32(rt)}; if(_b){{ if(_b==-1&&_a==INT32_MIN){{ee->lo.sd[0]=INT32_MIN;ee->hi.sd[0]=0;}}"
                  f" else{{ ee->lo.sd[0]=_a/_b; ee->hi.sd[0]=_a%_b; }} }} }}"),
            0x1b:(f"{{ uint32_t _a={r32(rs)},_b={r32(rt)}; if(_b){{ ee->lo.sd[0]=(int32_t)(_a/_b); ee->hi.sd[0]=(int32_t)(_a%_b); }} }}"),
            0x20:set32(rd,f"{rs32(rs)} + {rs32(rt)}"),
            0x21:set32(rd,f"{r32(rs)} + {r32(rt)}"),
            0x22:set32(rd,f"{rs32(rs)} - {rs32(rt)}"),
            0x23:set32(rd,f"{r32(rs)} - {r32(rt)}"),
            0x24:set64(rd,f"{r64(rs)} & {r64(rt)}"),
            0x25:set64(rd,f"{r64(rs)} | {r64(rt)}"),
            0x26:set64(rd,f"{r64(rs)} ^ {r64(rt)}"),
            0x27:set64(rd,f"~({r64(rs)} | {r64(rt)})"),
            0x28:set32(rd,"ee->sa"),                 # mfsa
            0x29:f"ee->sa={r32(rs)};",               # mtsa
            0x2a:set32(rd,f"({rs64(rs)} < {rs64(rt)}) ? 1 : 0"),
            0x2b:set32(rd,f"({r64(rs)} < {r64(rt)}) ? 1 : 0"),
            0x2c:set64(rd,f"{rs64(rs)} + {rs64(rt)}"),
            0x2d:set64(rd,f"{r64(rs)} + {r64(rt)}"),
            0x2e:set64(rd,f"{rs64(rs)} - {rs64(rt)}"),
            0x2f:set64(rd,f"{r64(rs)} - {r64(rt)}"),
            0x38:set64(rd,f"{r64(rt)} << {sa}"),
            0x3a:set64(rd,f"{r64(rt)} >> {sa}"),
            0x3b:set64(rd,f"{rs64(rt)} >> {sa}"),
            0x3c:set64(rd,f"{r64(rt)} << ({sa}+32)"),
            0x3e:set64(rd,f"{r64(rt)} >> ({sa}+32)"),
            0x3f:set64(rd,f"{rs64(rt)} >> ({sa}+32)"),
        }
        if fn==0x0c: return f"ee_syscall(ee,0x{(i.raw>>6)&0xfffff:x}u);"
        if fn==0x0d: return f"ee_break(ee,0x{(i.raw>>6)&0xfffff:x}u);"
        if fn in (0x30,0x31,0x32,0x33,0x34,0x36):   # traps tge/tgeu/tlt/tltu/teq/tne
            return "/* trap (ignored; no EE exception model) */"
        if fn in T:
            s=T[fn]
            return s if s!="" else f"/* op0.fn{fn:#x} rd0 */"
        return trap(ctx,i,f"special.fn{fn:#04x}")
    if op==1:  # REGIMM non-branch (traps / mtsab / mtsah)
        rt=i.rt; rs=i.rs
        if rt==0x18: return f"ee->sa=(({r32(rs)} ^ (uint32_t){i.simm}) & 0xF) << 3;"   # mtsab
        if rt==0x19: return f"ee->sa=(({r32(rs)} ^ (uint32_t){i.simm}) & 0x7) << 4;"   # mtsah
        if rt in (8,9,10,11,12,14): return "/* trap imm (ignored) */"
        return trap(ctx,i,f"regimm.rt{rt:#04x}")
    # immediate / load / store
    rs,rt,simm,imm=i.rs,i.rt,i.simm,i.imm
    ea=f"(uint32_t)({r32(rs)} + {simm})"
    if op==0x1e:  # LQ
        if rt==0: return "/* lq rt0 */"
        return f"ee_lq(ee,{ea},&ee->r[{rt}]);"
    if op==0x1f:  # SQ
        return f"ee_sq(ee,{ea},&ee->r[{rt}]);"
    if op==0x31: return f"ee->f[{rt}]=ee_lw(ee,{ea});"          # lwc1
    if op==0x39: return f"ee_sw(ee,{ea},ee->f[{rt}]);"          # swc1
    if op==0x36:  # lqc2  (vf=rt)
        return (f"{{ ee_reg _q; ee_lq(ee,{ea},&_q); "+
                " ".join(f"ee_cop2_qmtc2(ee,{rt},{l},_q.uw[{l}]);" for l in range(4))+" }")
    if op==0x3e:  # sqc2
        return (f"{{ ee_reg _q; "+
                " ".join(f"_q.uw[{l}]=ee_cop2_qmfc2(ee,{rt},{l});" for l in range(4))+
                f" ee_sq(ee,{ea},&_q); }}")
    if op==0x2f: return "/* cache (nop) */"
    if op==0x33: return "/* pref (nop) */"
    M={
        0x08:set32(rt,f"{rs32(rs)} + ({simm})"),
        0x09:set32(rt,f"{r32(rs)} + ({simm})"),
        0x0a:set32(rt,f"({rs64(rs)} < ({simm}ll)) ? 1 : 0"),
        0x0b:set32(rt,f"({r64(rs)} < (uint64_t)(int64_t)({simm}ll)) ? 1 : 0"),
        0x0c:set64(rt,f"{r64(rs)} & 0x{imm:x}ull"),
        0x0d:set64(rt,f"{r64(rs)} | 0x{imm:x}ull"),
        0x0e:set64(rt,f"{r64(rs)} ^ 0x{imm:x}ull"),
        0x0f:set32(rt,f"0x{(imm<<16)&0xffffffff:x}u"),
        0x18:set64(rt,f"{rs64(rs)} + ({simm}ll)"),
        0x19:set64(rt,f"{rs64(rs)} + ({simm}ll)"),
        0x1a:set64(rt,f"ee_ldl(ee,{ea},{r64(rt)})"),
        0x1b:set64(rt,f"ee_ldr(ee,{ea},{r64(rt)})"),
        0x20:set32(rt,f"(int32_t)ee_lb(ee,{ea})"),
        0x21:set32(rt,f"(int32_t)ee_lh(ee,{ea})"),
        0x22:set32(rt,f"ee_lwl(ee,{ea},{r32(rt)})"),
        0x23:set32(rt,f"(int32_t)ee_lw(ee,{ea})"),
        0x24:set32(rt,f"ee_lbu(ee,{ea})"),
        0x25:set32(rt,f"ee_lhu(ee,{ea})"),
        0x26:set32(rt,f"ee_lwr(ee,{ea},{r32(rt)})"),
        0x27:set64(rt,f"(uint64_t)ee_lw(ee,{ea})"),
        0x37:set64(rt,f"ee_ld(ee,{ea})"),
        0x28:f"ee_sb(ee,{ea},(uint8_t){r32(rt)});",
        0x29:f"ee_sh(ee,{ea},(uint16_t){r32(rt)});",
        0x2a:f"ee_swl(ee,{ea},{r32(rt)});",
        0x2b:f"ee_sw(ee,{ea},{r32(rt)});",
        0x2c:f"ee_sdl(ee,{ea},{r64(rt)});",
        0x2d:f"ee_sdr(ee,{ea},{r64(rt)});",
        0x2e:f"ee_swr(ee,{ea},{r32(rt)});",
        0x3f:f"ee_sd(ee,{ea},{r64(rt)});",
    }
    if op in M:
        s=M[op]
        return s if s!="" else f"/* op{op:#x} rt0 */"
    return trap(ctx,i,f"op{op:#04x}")

def cond_expr(kind,i):
    rs,rt=i.rs,i.rt
    base={
        'beq':f"{r64(rs)} == {r64(rt)}", 'bne':f"{r64(rs)} != {r64(rt)}",
        'beql':f"{r64(rs)} == {r64(rt)}",'bnel':f"{r64(rs)} != {r64(rt)}",
        'blez':f"{rs64(rs)} <= 0",'bgtz':f"{rs64(rs)} > 0",
        'blezl':f"{rs64(rs)} <= 0",'bgtzl':f"{rs64(rs)} > 0",
        'bltz':f"{rs64(rs)} < 0",'bgez':f"{rs64(rs)} >= 0",
        'bltzl':f"{rs64(rs)} < 0",'bgezl':f"{rs64(rs)} >= 0",
        'bltzal':f"{rs64(rs)} < 0",'bgezal':f"{rs64(rs)} >= 0",
        'bltzall':f"{rs64(rs)} < 0",'bgezall':f"{rs64(rs)} >= 0",
        'bc1t':"(ee->fcr31 & 0x00800000u)",'bc1f':"!(ee->fcr31 & 0x00800000u)",
        'bc1tl':"(ee->fcr31 & 0x00800000u)",'bc1fl':"!(ee->fcr31 & 0x00800000u)",
        'bc2t':"0",'bc2f':"0",'bc2tl':"0",'bc2fl':"0",
    }
    return base.get(kind)

def decode_function(data, start, end):
    insns={}
    va=start
    while va<end and va<TEXT_END_VA:
        insns[va]=Insn(va, word(data,va)); va+=4
    return insns

# ---------------------------------------------------------------------------
# Jump-table resolution (ADDED for M1 integration).
#
# The original lift lowered every computed `jr $rX` (rX != $ra) to
# ee_call_indirect(), which can only resolve FUNCTION ENTRIES.  C `switch`
# statements compile to a jump table (`lw $t, table($idx); jr $t`) whose
# targets are LABELS INSIDE the current function -- not function entries -- so
# those jumps trapped ("indirect call to unknown"), blocking the boot path
# (main's boot-mode switch, the per-frame state dispatch, etc.).
#
# This pass recognises the standard MIPS jump-table idiom, reads the table out
# of the ELF, and -- when every entry lands inside the current function -- lets
# emit_control() emit a real `switch(_t){ case 0xADDR: goto L_ADDR; ... }`.
# It is CONSERVATIVE: any deviation returns None and the old ee_call_indirect
# fallback is used unchanged, so no previously-working jump can regress.
#
# word()/va2off map the file-backed first LOAD segment (VA 0x100000.. the boot
# and main-loop code live here); tables outside it are left to the fallback.
DATA_SEG0_END = 0x004b949c   # TEXT_VA + seg0 p_filesz (file-backed data end)

def _elf_ok(va):
    return TEXT_VA <= va and (va - TEXT_VA + TEXT_OFF + 4) <= DATA_SEG0_END - TEXT_VA + TEXT_OFF

def _const_prop(insns, upto_va):
    """Forward constant propagation over the function prefix [.., upto_va).
    Tracks only LUI/ADDIU/ORI and ADDU(const,const); anything else invalidates
    its (best-effort) destination.  Over-/under-approximation is safe: a wrong
    base yields out-of-range targets -> None -> fallback."""
    cj = {}
    for v in sorted(insns):
        if v >= upto_va: break
        i = insns[v]; op=i.op; d=None; val=None
        if   op==0x0f: d=i.rt; val=(i.imm<<16)&0xffffffff          # LUI
        elif op==0x09: d=i.rt; val=((cj[i.rs]+i.simm)&0xffffffff) if i.rs in cj else None  # ADDIU
        elif op==0x0d: d=i.rt; val=((cj[i.rs]|i.imm)&0xffffffff)   if i.rs in cj else None  # ORI
        elif op==0 and i.fn==0x21:                                 # ADDU
            d=i.rd
            if i.rs in cj and i.rt in cj: val=(cj[i.rs]+cj[i.rt])&0xffffffff
        elif op==0:                                                # other SPECIAL -> writes rd
            d=i.rd
        elif op in (0x08,0x0a,0x0b,0x0c,0x0e,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x30,0x1a,0x1b):
            d=i.rt                                                 # imm-ops/loads write rt
        if d:
            if val is None: cj.pop(d, None)
            else: cj[d]=val
    return cj

def resolve_jumptable(insns, jr_va, start, end):
    """Return a de-duplicated list of in-function targets for the jr at jr_va,
    or None if this is not a recognisable in-range jump table."""
    jr = insns[jr_va]; tgt_reg = jr.rs
    pre = [v for v in sorted(insns) if v < jr_va]
    # (1) the LW that last defines the jr register
    lw = None
    for v in reversed(pre):
        i = insns[v]
        dreg = i.rt if (i.op==0x23 or (0x08<=i.op<=0x0f) or (0x20<=i.op<=0x27)) else (i.rd if i.op==0 else None)
        if dreg == tgt_reg:
            lw = i if i.op==0x23 else None     # must be LW to be a table load
            break
    if lw is None: return None
    base_reg, off = lw.rs, lw.simm
    # (2) resolve the table base = the constant part of (idx + base)
    addu = None
    for v in reversed(pre):
        if v >= lw.va: continue
        i = insns[v]
        dreg = i.rd if i.op==0 else (i.rt if ((0x08<=i.op<=0x0f) or (0x20<=i.op<=0x27)) else None)
        if dreg == base_reg: addu = i; break
    if addu is not None and addu.op==0 and addu.fn==0x21:
        cj = _const_prop(insns, addu.va)
        a, b = cj.get(addu.rs), cj.get(addu.rt)
        base = a if (a is not None and b is None) else (b if (b is not None and a is None) else None)
    else:
        cj = _const_prop(insns, lw.va)         # base reg directly constant (rare)
        base = cj.get(base_reg)
    if base is None: return None
    table = (base + off) & 0xffffffff
    if not _elf_ok(table): return None
    # (3) read entries until one leaves the function; cap for safety
    targets = []
    for k in range(256):
        va = table + 4*k
        if not _elf_ok(va): break
        w = struct.unpack_from('<I', resolve_jumptable.data, va2off(va))[0]
        if not (start <= w < end): break
        targets.append(w)
    if not targets: return None
    # de-dup preserving order
    seen=set(); out=[]
    for t in targets:
        if t not in seen: seen.add(t); out.append(t)
    return out
resolve_jumptable.data = None   # set by emit_all/legacy before emission

def call_or_indirect(ctx, tgt, tail):
    if tgt in ctx.func_starts:
        return f"func_{tgt:08x}(ee);" + (" return; /* tail */" if tail else "")
    return f"ee_call_indirect(ee,0x{tgt:08x}u);" + (" return;" if tail else "")

def emit_control(ctx,i,kind,tgt,dstmt,insns,start,end,slot_is_target=False,jt_targets=None):
    # slot_is_target: the delay slot (va+4) is itself an in-range branch target,
    # so it is NOT consumed here -- it is emitted again at its own label L_<dva>
    # (with an L_<dva>_a "after" label). Paths that FALL THROUGH this branch must
    # skip that label copy so the slot is not executed twice; we emit an explicit
    # `goto L_<dva>_a;`. Jumps that always transfer need no skip.
    L=[]; pad="  "
    dva=i.va+4
    skip=(f"{pad}goto L_{dva:08x}_a;") if slot_is_target else None
    if kind=='jr':
        if i.rs==31:
            L.append(f"{pad}{dstmt}"); L.append(f"{pad}return; /* jr $ra */")
        elif jt_targets:
            # resolved jump table -> real in-function switch dispatch
            L.append(f"{pad}{{ uint32_t _t={r32(i.rs)}; {dstmt}")
            L.append(f"{pad}  switch(_t) {{")
            for t in jt_targets:
                L.append(f"{pad}    case 0x{t:08x}u: goto L_{t:08x};")
            L.append(f"{pad}    default: ee_call_indirect(ee,_t); return;")
            L.append(f"{pad}  }} }}")
        else:
            L.append(f"{pad}{{ uint32_t _t={r32(i.rs)}; {dstmt} ee_call_indirect(ee,_t); return; }}")
        return L
    if kind=='jalr':
        L.append(f"{pad}{{ uint32_t _t={r32(i.rs)}; "
                 f"{set32(i.rd if i.rd else 31, f'0x{i.va+8:08x}u')} {dstmt} ee_call_indirect(ee,_t); }}")
        if skip: L.append(skip)
        return L
    if kind in ('j','jal'):
        if kind=='jal':
            L.append(f"{pad}{set32(31, f'0x{i.va+8:08x}u')}")
        if tgt in insns:
            L.append(f"{pad}{dstmt}"); L.append(f"{pad}goto L_{tgt:08x};")
        else:
            L.append(f"{pad}{dstmt}")
            L.append(f"{pad}{call_or_indirect(ctx,tgt, kind=='j')}")
        if kind=='jal' and skip: L.append(skip)  # jal returns here -> falls through
        return L
    # conditional branch
    c=cond_expr(kind,i)
    pre=f"{set32(31, f'0x{i.va+8:08x}u')} " if kind in ('bltzal','bgezal','bltzall','bgezall') else ""
    inrange = tgt in insns
    taken = f"goto L_{tgt:08x};" if inrange else f"{{ {call_or_indirect(ctx,tgt,True)} }}"
    if kind in LIKELY:
        L.append(f"{pad}{pre}if ({c}) {{ {dstmt} {taken} }}")
    else:
        L.append(f"{pad}{{ {pre}int _c=({c}); {dstmt} if (_c) {taken} }}")
    if skip: L.append(skip)
    return L

def emit_function(ctx, data, name, start, end, stats):
    insns=decode_function(data,start,end)
    labels=set()
    for va in insns:
        bt=branch_target(insns[va])
        if bt and bt[1] is not None and bt[1] in insns:
            labels.add(bt[1])
    # Resolve jump tables: a computed `jr` (not $ra) whose target comes from an
    # in-function table.  Add every target as a label so the switch dispatch can
    # goto it.  (Conservative: resolve_jumptable returns None -> old fallback.)
    resolve_jumptable.data = data
    jumptables={}
    for va in insns:
        ins=insns[va]
        bt=branch_target(ins)
        if bt is not None and bt[0]=='jr' and ins.rs!=31:
            tgts=resolve_jumptable(insns, va, start, end)
            if tgts:
                jumptables[va]=tgts
                for t in tgts:
                    if t in insns: labels.add(t)
                stats['jumptable_resolved']+=1
                stats['jumptable_cases']+=len(tgts)
    # A delay slot that is itself an in-range branch target is NOT consumed:
    # it is emitted at its own label (plus an L_<dva>_a "after" label) so the
    # goto-into-slot path executes it, while fall-through paths skip the copy.
    slot_targets=set()
    for va in insns:
        bt=branch_target(insns[va])
        if bt is not None:
            dva=va+4
            if dva in labels and dva in insns:
                slot_targets.add(dva)
                stats['label_on_delayslot']+=1
    out=[f"void {name}(ee_state *ee) {{", "  (void)ee;"]
    delay_consumed=set()
    for va in sorted(insns):
        ins=insns[va]
        if va in labels:
            out.append(f"L_{va:08x}:;")
        if va in delay_consumed:
            continue
        bt=branch_target(ins)
        if bt is None:
            out.append("  "+translate_alu(ctx,ins))
        else:
            dva=va+4; dslot=insns.get(dva)
            dstmt=translate_alu(ctx,dslot) if dslot is not None else "/* (no slot: fn end) */"
            sit = dva in slot_targets
            if not sit:
                delay_consumed.add(dva)
            kind,tgt=bt
            out.extend(emit_control(ctx,ins,kind,tgt,dstmt,insns,start,end,sit,
                                    jumptables.get(va)))
        if va in slot_targets:
            out.append(f"L_{va:08x}_a:;")
    out.append("}")
    return "\n".join(out)

def load_boundaries(csvpath):
    rows=[]
    with open(csvpath) as f:
        for row in csv.DictReader(f):
            s=int(row['start'],16); e=int(row['end'],16)
            rows.append((row['name'], s, e))
    rows.sort(key=lambda r:r[1])
    return rows

# ---- legacy stdout mode (keeps old tests working) --------------------
def legacy(elf_path, addrs, csvpath=None):
    data=load_elf(elf_path)
    starts=set(addrs)
    if csvpath and os.path.exists(csvpath):
        starts={int(r['start'],16) for r in csv.DictReader(open(csvpath))}
    ctx=Ctx(starts|set(addrs))
    stats=Counter()
    out=['/* GENERATED by tools/recompile.py -- do not edit by hand. */',
         '#include "ee_runtime.h"\n']
    # need end for each addr: look up in csv if available, else sweep heuristically
    ends={}
    if csvpath and os.path.exists(csvpath):
        for r in csv.DictReader(open(csvpath)):
            ends[int(r['start'],16)]=int(r['end'],16)
    for a in addrs:
        end=ends.get(a)
        if end is None:  # heuristic: sweep to next jr $ra+slot
            end=a
            va=a
            while va<TEXT_END_VA:
                w=word(data,va); fn=w&63
                va+=4
                if (w>>26)==0 and fn==0x08 and ((w>>21)&31)==31:
                    end=va+4; break
                if va-a>0x8000: end=va; break
        out.append(emit_function(ctx,data,f"func_{a:08x}",a,end,stats))
        out.append("")
    sys.stdout.write("\n".join(out)+"\n")

# ---- whole-program emit mode -----------------------------------------
def emit_all(elf_path, csvpath, outdir, tu_size, limit):
    data=load_elf(elf_path)
    rows=load_boundaries(csvpath)
    if limit: rows=rows[:limit]
    func_starts={s for (_n,s,_e) in rows}
    ctx=Ctx(func_starts)
    stats=Counter()
    os.makedirs(outdir, exist_ok=True)
    # ee_funcs.h : prototypes for every function
    with open(os.path.join(outdir,'ee_funcs.h'),'w') as h:
        h.write('/* GENERATED: prototypes for every recompiled EE function.\n'
                ' * HLE-routed guest functions are #defined to their sif_* hook\n'
                ' * (declared in ee_runtime.h) instead of being prototyped here. */\n')
        h.write('#ifndef EE_FUNCS_H\n#define EE_FUNCS_H\n#include "ee_runtime.h"\n')
        # Prototypes for every HLE hook (harmless redeclaration of the ones that
        # ee_runtime.h already declares; required for any additional routes so
        # the generated TUs + function table see a declaration).
        _seen_hooks=set()
        for _hk in HLE_ROUTES.values():
            if _hk not in _seen_hooks:
                _seen_hooks.add(_hk); h.write(f"void {_hk}(ee_state *ee);\n")
        for (_n,s,_e) in rows:
            if s in HLE_ROUTES:
                h.write(f"#define func_{s:08x} {HLE_ROUTES[s]}\n")
            else:
                h.write(f"void func_{s:08x}(ee_state *ee);\n")
        h.write('#endif\n')
    # partition into TUs
    tus=[]
    cur=[];
    for r in rows:
        cur.append(r)
        if len(cur)>=tu_size:
            tus.append(cur); cur=[]
    if cur: tus.append(cur)
    manifest=[]
    nfunc=0
    for idx,tu in enumerate(tus):
        path=os.path.join(outdir,f"eetu_{idx:04d}.c")
        with open(path,'w') as f:
            f.write('/* GENERATED by tools/recompile.py -- do not edit by hand. */\n')
            f.write('#include "ee_funcs.h"\n\n')
            for (_n,s,e) in tu:
                if s in HLE_ROUTES:
                    f.write(f"/* func_{s:08x} routed to HLE hook {HLE_ROUTES[s]} (not recompiled) */\n\n")
                    continue
                f.write(emit_function(ctx,data,f"func_{s:08x}",s,e,stats))
                f.write("\n\n")
                nfunc+=1
        manifest.append(f"eetu_{idx:04d}.c {len(tu)} funcs")
    # function_table.c : address -> fn pointer, sorted for binary search
    with open(os.path.join(outdir,'function_table.c'),'w') as f:
        f.write('/* GENERATED function table: every discovered guest function.\n'
                ' * ee_lookup()/ee_call_indirect() resolve computed calls,\n'
                ' * vtables and HLE callbacks through it (binary search). */\n')
        f.write('#include "ee_funcs.h"\n#include <stddef.h>\n\n')
        f.write('typedef struct { uint32_t addr; ee_func_t fn; } ee_row;\n')
        f.write('static const ee_row g_table[] = {\n')
        for (_n,s,_e) in rows:
            f.write(f"    {{ 0x{s:08x}u, func_{s:08x} }},\n")
        f.write('};\n')
        f.write('static const size_t g_table_n = sizeof g_table / sizeof g_table[0];\n\n')
        f.write('ee_func_t ee_lookup(uint32_t a){\n')
        f.write('    size_t lo=0, hi=g_table_n;\n')
        f.write('    while(lo<hi){\n')
        f.write('        size_t m=(lo+hi)>>1; uint32_t k=g_table[m].addr;\n')
        f.write('        if(k==a) return g_table[m].fn;\n')
        f.write('        if(k<a) lo=m+1; else hi=m;\n')
        f.write('    }\n')
        f.write('    return NULL;\n}\n')
    # reports
    with open(os.path.join(outdir,'manifest.txt'),'w') as f:
        f.write(f"functions emitted: {nfunc}\n")
        f.write(f"translation units: {len(tus)} (<= {tu_size} funcs each)\n")
        f.write(f"delay-slot-is-branch-target occurrences: {stats['label_on_delayslot']}\n")
        f.write("\n".join(manifest)+"\n")
    total_unimpl=sum(ctx.unimpl.values())
    with open(os.path.join(outdir,'unimpl_report.txt'),'w') as f:
        f.write(f"total ee_trap_unimpl sites: {total_unimpl}\n")
        f.write(f"distinct classes: {len(ctx.unimpl)}\n\n")
        for k,v in ctx.unimpl.most_common():
            f.write(f"{v:8d}  {k}\n")
    print(f"emitted {nfunc} functions into {len(tus)} TUs")
    print(f"unimpl sites: {total_unimpl} across {len(ctx.unimpl)} classes "
          f"(see {outdir}/unimpl_report.txt)")
    print(f"delay-slot/label overlaps: {stats['label_on_delayslot']}")
    return total_unimpl

def main():
    if len(sys.argv)>=3 and sys.argv[1]!='emit' and os.path.exists(sys.argv[1]) \
       and all(a.startswith('0x') or all(c in '0123456789abcdefABCDEF' for c in a) for a in sys.argv[2:]):
        legacy(sys.argv[1], [int(a,16) for a in sys.argv[2:]],
               csvpath='/srv/games/dirge-port/analysis/ee_boundaries.csv')
        return
    ap=argparse.ArgumentParser()
    sub=ap.add_subparsers(dest='cmd')
    e=sub.add_parser('emit')
    e.add_argument('--elf',required=True); e.add_argument('--csv',required=True)
    e.add_argument('--outdir',required=True); e.add_argument('--tu-size',type=int,default=300)
    e.add_argument('--limit',type=int,default=0)
    a=ap.parse_args()
    if a.cmd=='emit':
        emit_all(a.elf,a.csv,a.outdir,a.tu_size,a.limit)
    else:
        ap.print_help(); sys.exit(1)

if __name__=='__main__':
    main()
