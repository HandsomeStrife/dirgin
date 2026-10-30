#!/usr/bin/env python3
"""Offline reader for a stage file's event scripts (sections 2 and 3).

Reads the stage file given on the command line at run time; no game data is
stored in the repo. Format decoded from the game's code (PAL SLES_541.85):

  FUN_00310d20(n)   script n = section3_base + section2[n] (u32 offset table)
  FUN_0036fd58      script header: +0 size, +4 offset of the globals area,
                    +8 globals size, +0xc function-table size; the function
                    table follows the globals: pairs (event id, index); a
                    function header sits at table_end + index*8
  FUN_0036fdc8      function header: +4 data offset, +8 local count,
                    +0xc code offset (code = fn + [+4] + [+0xc])
  FUN_003715e0      run event N = find (N, index) in the table
  FUN_00371318      interpreter: 8-byte instructions (opcode u32, imm s32),
                    handler table at 0x4a2f60 (53 entries); values on the stack
                    are 8 bytes (value, type: 0 int, 1 float, 2 string/obj)
  FUN_00370b40 (op 38) send command imm: pops the target id (-1 = the script's
                    host, -2 = own object id at VM+0x1c); args = registers
                    r0..r3 (VM+0x3c.., set by op 26). Commands < 0x100 go to the
                    host's default object = the object manager (FUN_00545b20);
                    commands >= 0x100 go to the object with that id (host +0xc)
                    and land in its +0xc handler (e027: FUN_005661e8).
  Object vtable +0x2f4 (FUN_0054e4a0 / actors FUN_00568f98) fires event N of
  the object's script (object +0x34 = script index).

Usage: stage_events.py STAGE.bin [--dis N] [--cmds 0x143,0x108] [--triggers] [--activations]
"""
import struct
import sys

OPS = {
    0: 'nop', 1: 'halt', 2: 'drop', 3: 'dup', 4: 'push_i', 5: 'push_f',
    6: 'add', 7: 'fadd', 8: 'sub', 9: 'fsub', 10: 'mul', 11: 'fmul',
    12: 'div', 13: 'fdiv', 14: 'jmp', 15: 'call_script', 16: 'call_host',
    17: 'ret', 18: 'gt', 19: 'lt', 20: 'ge', 21: 'le', 22: 'eq', 23: 'ne',
    24: 'push_reg', 25: 'push_local', 26: 'pop_reg', 27: 'pop_local',
    28: 'not', 29: 'jz', 30: 'jnz', 31: 'nop31', 32: 'enter', 33: 'clr_local_i',
    34: 'clr_local_f', 35: 'clr_local_s', 36: 'push_data', 37: 'push_data_ref',
    38: 'send', 39: 'op39', 40: 'op40', 41: 'rand_i', 42: 'rand_range',
    43: 'rand_f', 44: 'lerp_rand', 45: 'switch', 46: 'shl', 47: 'shr',
    48: 'band', 49: 'bor', 50: 'land', 51: 'lor', 52: 'clr_regs',
}


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def s32(d, o):
    return struct.unpack_from('<i', d, o)[0]


def sections(d):
    return [struct.unpack_from('<3I', d, 0x24 + i * 16) for i in range(0x22)]


def scripts(d):
    sec = sections(d)
    s2, n, _ = sec[2]
    s3 = sec[3][0]
    out = []
    for i in range(n):
        out.append(s3 + u32(d, s2 + 4 * i))
    return out


def functions(d, b):
    """[(event id, fn offset, code offset, code end)] for script at b."""
    t1 = b + u32(d, b + 4) + u32(d, b + 8)
    te = t1 + u32(d, b + 0xc)
    fns = []
    for o in range(t1, te, 8):
        ev, idx = s32(d, o), s32(d, o + 4)
        f = te + idx * 8
        fns.append([ev, f, f + u32(d, f + 4) + u32(d, f + 0xc)])
    end = b + u32(d, b)
    starts = sorted(f for _, f, _ in fns) + [end]
    for fn in fns:
        fn.append(min(s for s in starts if s > fn[1]))
    return fns


def disasm(d, code, end):
    out = []
    o = code
    while o + 8 <= end:
        op, imm = s32(d, o), s32(d, o + 4)
        if op < 0 or op > 52:
            break
        if op == 5:
            imm = struct.unpack_from('<f', d, o + 4)[0]
        out.append(((o - code) // 8, op, imm))
        o += 8
        if op == 17 and o + 8 <= end and s32(d, o) not in OPS:
            break
    return out


def fmt(ins):
    i, op, imm = ins
    name = OPS.get(op, 'op%d' % op)
    if op == 38 or op == 16:
        return '%4d %s 0x%x' % (i, name, imm)
    return '%4d %s %s' % (i, name, imm)


def sends(d, cmds):
    """Find op 38 with imm in cmds; report target and r0/r1 pushed before."""
    res = []
    for si, b in enumerate(scripts(d)):
        for ev, f, code, end in functions(d, b):
            ins = disasm(d, code, end)
            for k, (i, op, imm) in enumerate(ins):
                if op == 38 and imm in cmds:
                    ctx = ins[max(0, k - 10):k]
                    res.append((si, ev, i, imm, ctx))
    return res


def actions(d, si, ev_want=None):
    """Light symbolic pass over one script: tracks the stack as expressions,
    the registers, local 0..n, and the guard of each jz (innermost only).
    Manager commands used for naming (FUN_00545b20): 0xc(N) = event flag N
    (FUN_003ff738), 0x3a(k) = object id of section-1 record k (manager
    +0x3b2 table), 0xa6/0xa7 = FUN_00242c28() == 2 / 3."""
    b = scripts(d)[si]
    out = []
    for ev, f, code, end in functions(d, b):
        if ev_want is not None and ev != ev_want:
            continue
        ins = disasm(d, code, end)
        st, regs, loc = [], {}, {}
        guards = []          # (end index, text)
        for i, op, imm in ins:
            guards = [g for g in guards if g[0] > i]
            pop = lambda: st.pop() if st else '?'
            if op in (4, 5):
                st.append(imm)
            elif op == 52:
                regs = {}
            elif op == 26:
                regs[imm] = pop()
            elif op == 24:
                st.append(regs.get(imm, 'r%d' % imm))
            elif op == 27:
                loc[imm] = pop()
            elif op == 25:
                st.append(loc.get(imm, 'l%d' % imm))
            elif op in (18, 19, 20, 21, 22, 23, 6, 8, 48, 49, 50, 51):
                bb, aa = pop(), pop()
                sym = {18: '>', 19: '<', 20: '>=', 21: '<=', 22: '==', 23: '!=',
                       6: '+', 8: '-', 48: '&', 49: '|', 50: '&&', 51: '||'}[op]
                st.append('(%s %s %s)' % (aa, sym, bb))
            elif op == 28:
                st.append('!%s' % pop())
            elif op == 2:
                pop()
            elif op in (29, 30):
                c = pop()
                guards.append((imm, ('%s' if op == 29 else '!%s') % c))
            elif op == 38:
                tgt = pop()
                a = [regs.get(k, 0) for k in range(4)]
                if imm == 0xc:
                    res = 'flag%s' % a[0]
                elif imm in (0x3a, 0x3f):                      # manager +0x3b2 (section 1)
                    res = 'obj[%s]' % a[0]
                elif imm == 0x3d:                              # manager +0x7b0 (section 8)
                    res = 'trig[%s]' % a[0]
                elif imm == 0x3e:                              # manager +0x5b2 (section 15)
                    res = 'prop[%s]' % a[0]
                elif imm == 0x44:                              # manager +0xfac (section 22)
                    res = 'misc[%s]' % a[0]
                elif imm == 0xa6:
                    res = 'diff==2'
                elif imm == 0xa7:
                    res = 'diff==3'
                else:
                    res = 'c%x(%s)' % (imm, ','.join(str(x) for x in a[:2]))
                    out.append((ev, i, tgt, imm, a, [g[1] for g in guards]))
                st.append(res)
            elif op in (17, 1):
                pass
    return out


# Save state the page starts from (notes/enemy-activation.md §1): FUN_00242c28() = 1, so the
# manager 0xa6 / 0xa7 tests (== 2 / == 3) are false; event flags (FUN_003ff738) = 0 except 2 and 46.
SAVE_FLAGS = {2: 7, 46: 1}
SAVE_DIFF = 1
ACT_CMDS = {0x101: 'wake', 0x108: 'activate', 0x109: 'deactivate', 0x130: 'deactivate', 0x143: 'ai mode'}


def eval_guard(g, flags=SAVE_FLAGS, diff=SAVE_DIFF):
    """True / False for a guard the export can know (difficulty, event flags), None = runtime
    (own vars 0x105, manager shorts 0x30, message args ...)."""
    import re
    e = g.replace('diff==2', str(int(diff == 2))).replace('diff==3', str(int(diff == 3)))
    e = re.sub(r'flag(\d+)', lambda m: str(flags.get(int(m.group(1)), 0)), e)
    e = e.replace('!', ' not ').replace('&&', ' and ').replace('||', ' or ').replace(' not =', '!=')
    if re.search(r'[A-Za-z_\[]', e.replace('not', '').replace('and', '').replace('or', '')):
        return None
    try:
        return bool(eval(e, {'__builtins__': {}}))
    except Exception:
        return None


def activations(d, si):
    """Resolved 0x101 / 0x108 / 0x109 / 0x130 / 0x143 sends of script si: target named by its
    manager lookup (obj[k] = section-1 record k, trig[k] = section-8 trigger k), guards evaluated
    for the save (difficulty 1, flag0 = 0). Branches whose known guard is false are dropped;
    'runtime' lists the guards left to the VM (e.g. the trigger's own var0 'once')."""
    out = []
    for ev, i, tgt, cmd, a, g in actions(d, si):
        if cmd not in ACT_CMDS:
            continue
        vals = [eval_guard(x) for x in g]
        if False in vals:
            continue
        who = a[0] if cmd in (0x101,) else tgt
        out.append(dict(ev=ev, at=i, cmd=cmd, what=ACT_CMDS[cmd], target=str(who),
                        n=a[1] if cmd == 0x101 else a[0] if cmd == 0x143 else None,
                        runtime=[x for x, v in zip(g, vals) if v is None]))
    return out


def main():
    path = sys.argv[1]
    d = open(path, 'rb').read()
    args = sys.argv[2:]
    if '--triggers' in args:
        # Section 8 = trigger volumes (object class vtable 0x58cdf8; record at
        # object +0xd4). FUN_005517a8 (vtable +0x160): position moves outside ->
        # inside (FUN_005507a0) fires event 0xd if +0xa4 bit 0, inside -> outside
        # fires 0xe if bit 1. +0x00 matrix (+0x30 position), +0x40 inverse,
        # +0x80/+0x90 local box min/max, +0xa0 s16 script index, +0xaa shape.
        so, n, stride = sections(d)[8]
        cmds = {0x101, 0x102, 0x108, 0x143, 0x193, 0x104}
        for k in range(n):
            r = so + k * stride
            pos = struct.unpack_from('<3f', d, r + 0x30)
            lo = struct.unpack_from('<3f', d, r + 0x80)
            hi = struct.unpack_from('<3f', d, r + 0x90)
            scr = struct.unpack_from('<h', d, r + 0xa0)[0]
            fl, shape = u32(d, r + 0xa4), d[r + 0xaa]
            acts = []
            if 0 <= scr < len(scripts(d)):
                for ev, i, tgt, cmd, a, g in actions(d, scr):
                    if cmd in cmds:
                        who = tgt if tgt != -2 else 'self'
                        if cmd in (0x101, 0x102):
                            who = a[0]
                        acts.append('ev%x:%x>%s' % (ev, cmd, who))
            print('trig %3d pos (%7.1f %6.1f %7.1f) box %s..%s shape %d fl %x script %d  %s' % (
                k, *pos, tuple(round(x, 1) for x in lo), tuple(round(x, 1) for x in hi),
                shape, fl, scr, ' '.join(acts[:8])))
        return
    if '--activations' in args:
        # every trigger's resolved activations / wakes for the save (notes/enemy-activation.md §2)
        so, n, stride = sections(d)[8]
        for k in range(n):
            r = so + k * stride
            scr = struct.unpack_from('<h', d, r + 0xa0)[0]
            fl = u32(d, r + 0xa4)
            if not 0 <= scr < len(scripts(d)):
                continue
            acts = activations(d, scr)
            if not acts:
                continue
            pos = struct.unpack_from('<3f', d, r + 0x30)
            print('trig %3d (%7.1f %6.1f %7.1f) fl %6x%s script %d' % (
                k, *pos, fl, ' UNARMED' if fl >> 17 & 1 else '', scr))
            for x in acts:
                print('    ev 0x%-3x %-10s %-9s n=%-4s if %s' % (
                    x['ev'], x['what'], x['target'], x['n'], ' && '.join(x['runtime']) or '-'))
        return
    if '--actions' in args:
        si = int(args[args.index('--actions') + 1], 0)
        want = None
        if '--obj' in args:
            want = {'obj[%s]' % x for x in args[args.index('--obj') + 1].split(',')}
        for ev, i, tgt, cmd, a, g in actions(d, si):
            if want and str(tgt) not in want:
                continue
            print('ev 0x%-3x @%-5d %-10s send 0x%-3x r=%s  if %s' % (
                ev, i, tgt, cmd, a[:2], ' && '.join(g) or '-'))
        return
    if '--dis' in args:
        n = int(args[args.index('--dis') + 1], 0)
        b = scripts(d)[n]
        for ev, f, code, end in functions(d, b):
            print('== script %d event 0x%x (fn +0x%x)' % (n, ev, f - b))
            for ins in disasm(d, code, end):
                print('   ' + fmt(ins))
        return
    cmds = {0x143, 0x108}
    if '--cmds' in args:
        cmds = {int(x, 0) for x in args[args.index('--cmds') + 1].split(',')}
    for si, ev, i, imm, ctx in sends(d, cmds):
        print('script %3d event 0x%-3x @%-4d send 0x%x  <- %s' % (
            si, ev, i, imm, ' ; '.join(fmt(c).split(None, 1)[1] for c in ctx[-8:])))


if __name__ == '__main__':
    main()
