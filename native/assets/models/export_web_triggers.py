#!/usr/bin/env python3
"""Trigger volumes and event scripts for the enemy AI on play.html (native/web/enemy_ai.js),
read from the z128 stage file 06800001.bin at build time. Writes <out>/ai.json (gitignored
game data). Decode notes: notes/enemy-research.md §8 / §9, notes/enemy-ai-hook.md.

Decoded (D) layout, read here, nothing copied into the repo:
  - section table at +0x24 (16 B: offset, count, stride); tools/stage_events.py reads the same.
  - section 8 = trigger volumes (0xc0 B; object class vtable 0x58cdf8, record at object +0xd4):
    +0x00 matrix (row 3 = position), +0x40 inverse, +0x80 / +0x90 local box min / max,
    +0xa0 s16 script index, +0xa4 flags, +0xaa shape (FUN_005517a8, FUN_005507a0).
  - section 2 / 3 = event scripts (FUN_00310d20, FUN_0036fd58, FUN_0036fdc8): per script a table
    of (event id, fn index); per fn +8 local count, code = 8-byte (op, imm) instructions.
  - section 1 = actor placements (0x74 B): +0 parameter set, +8 AI mode, +4 s16 script index (object +0x34;
    record 44 -> 108, 45 -> 109, 66 -> 92 as notes §8), +0xc position, +0x1c facing,
    +0x40 override bits (status +0x0c, FUN_00315050), +0x54 weapon index.
  - weapon index (FUN_00318718): override bit 0x16 set -> record +0x54, else the parameter set
    (section 0) +0x88; -1 = none. FUN_00318610 then points status +0xac at that section-24 record.
  - section 24 = enemy weapons (0x104 B; FUN_00311600): the first 0x78 B are W (layout as
    export_web_aim.py): +4 flags, +0x16 s8 pellets, +0x18 f range, +0x2c f speed, +0x40 s16
    damage, +0x42 first delay, +0x44 interval, +0x46 reload, +0x48 magazine, +0x4a u8 hit value.
    z128 records 44 / 45 (#66 / #67) -> weapon 2 = the values in the save's RAM (notes §9.1).
  - section 1 / 0 max HP and level per record (FUN_00314ea8, see records()).
  - e027's part boxes (marker.mkd box array, FUN_0030baa0; see hitboxes()).
  - hit-zone records per section-19 index (parameter set +0x84; FUN_003153d0 / FUN_0030fc68; see zone_set()).
  - e027 clip frame counts (r_gst, r_gst_lk, r_st_a, r_st_f) from its rifle clip set, through
    export_web_enemy.py's loaders (anim_rot.clip_frames).

Usage: export_web_triggers.py STAGE.bin OUTDIR
"""
import json, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '../../../tools'))
import stage_events as SE          # noqa: E402  (section / script readers, decoded there)

TRIG_STRIDE = 0xc0
SEC1_STRIDE = 0x74
SEC24_STRIDE = 0x104
AI_CLIPS = ('r_gst', 'r_gst_lk', 'r_st_a', 'r_st_f', 'r_stlld0', 'r_stlaf0', 'r_stbf0')


def f32s(d, o, n):
    return [round(x, 6) for x in struct.unpack_from('<%df' % n, d, o)]


def triggers(d):
    so, n, stride = SE.sections(d)[8]
    if stride != TRIG_STRIDE:
        raise ValueError('section 8 stride %#x, expected %#x' % (stride, TRIG_STRIDE))
    out = []
    for k in range(n):
        r = so + k * stride
        out.append(dict(
            idx=k,
            m=[f32s(d, r + 16 * i, 4) for i in range(4)],
            inv=[f32s(d, r + 0x40 + 16 * i, 4) for i in range(4)],
            lo=f32s(d, r + 0x80, 3), hi=f32s(d, r + 0x90, 3),
            script=struct.unpack_from('<h', d, r + 0xa0)[0],
            flags=SE.u32(d, r + 0xa4), shape=d[r + 0xaa]))
    # resolved activations / wakes for the save (stage_events.activations: difficulty 1, flag0 = 0;
    # informational: the page runs the scripts themselves in enemy_ai.js's VM)
    nscr = len(SE.scripts(d))
    for t in out:
        t['acts'] = SE.activations(d, t['script']) if 0 <= t['script'] < nscr else []
    return out


def script(d, si):
    """{event id: {locals, code: [[op, imm], ...]}} for script si (op 5 imm as float)."""
    b = SE.scripts(d)[si]
    ev = {}
    for e, f, code, end in SE.functions(d, b):
        ins = SE.disasm(d, code, end)
        ev[str(e)] = dict(locals=SE.u32(d, f + 8), code=[[op, imm] for _i, op, imm in ins])
    return ev


def records(d):
    sec = SE.sections(d)
    o0, c0, s0 = sec[0]
    o1, c1, s1 = sec[1]
    if s1 != SEC1_STRIDE:
        raise ValueError('section 1 stride %#x' % s1)
    out = []
    for k in range(c1):
        o = o1 + k * s1
        ps = SE.u32(d, o)
        model = d[o0 + ps * s0 + 0x20:o0 + ps * s0 + 0x24].split(b'\0')[0].decode('latin-1')
        over = SE.u32(d, o + 0x40)                                   # status +0x0c (FUN_00315050)
        if over >> 0x16 & 1:                                         # FUN_00318718 (FUN_00315738(s, 0x16))
            weapon = struct.unpack_from('<i', d, o + 0x54)[0]
        else:
            weapon = struct.unpack_from('<i', d, o0 + ps * s0 + 0x88)[0]
        # +8 = AI mode the activation creates the AI with (FUN_00559748 -> FUN_00365c50: status
        # +0xa0 (this record) +8; mode 8 -> 0 then FUN_0035f2e0)
        # max HP status +0x04 and level +0x11 (FUN_00314ea8): override bit 0x14 -> record +0x50 (u32),
        # else parameter set +0x30 (u16); bit 0x15 -> record +0x59 (u8), else parameter set +0x79.
        # (Status +0x60 bit 1 takes both from FUN_00320d20 instead: a status flag, not in the stage file.)
        hp = SE.u32(d, o + 0x50) if over >> 0x14 & 1 else struct.unpack_from('<H', d, o0 + ps * s0 + 0x30)[0]
        lvl = d[o + 0x59] if over >> 0x15 & 1 else d[o0 + ps * s0 + 0x79]
        # hit-zone set: parameter set +0x84 = section-19 index (FUN_003153d0 -> FUN_00310e40; zone_set)
        zi = struct.unpack_from('<i', d, o0 + ps * s0 + 0x84)[0]
        out.append(dict(rec=k, model=model, script=struct.unpack_from('<h', d, o + 4)[0],
                        mode=struct.unpack_from('<i', d, o + 8)[0],
                        pos=f32s(d, o + 0xc, 3), face=f32s(d, o + 0x1c, 3), weapon=weapon, hp=hp, lvl=lvl, zones=zi))
    return out


def weapons(d):
    """Section 24 records: the W fields enemy_ai.js reads."""
    so, n, stride = SE.sections(d)[24]
    if stride != SEC24_STRIDE:
        raise ValueError('section 24 stride %#x, expected %#x' % (stride, SEC24_STRIDE))
    out = []
    for k in range(n):
        o = so + k * stride
        h = lambda x: struct.unpack_from('<h', d, o + x)[0]
        f = lambda x: round(struct.unpack_from('<f', d, o + x)[0], 6)
        out.append(dict(idx=k, flags=SE.u32(d, o + 4), pellets=struct.unpack_from('b', d, o + 0x16)[0],
                        range=f(0x18), speed=f(0x2c), damage=h(0x40), first=h(0x42), interval=h(0x44),
                        reload=h(0x46), mag=h(0x48), hit=d[o + 0x4a]))
    return out


def hitboxes(model='e027'):
    """The model's part boxes for the bullet part test FUN_005ad118 -> FUN_0030baa0: the 'mkd'
    chunk (header 'mk2', +6 u16 box count, +0xc box array; FUN_0030b9e0 relocates +8 / +0xc), 32 B
    per box: +0 offset xyz (f32), +0xc bone (s32), +0x10 scale xyz (f32). Box k = part k."""
    import export_web_enemy as E, export_cerberus as EC
    buf = open(E.paths(model)['cont'], 'rb').read()
    for o in EC.entries(buf):
        if buf[o + 8:o + 11] == b'mkd':
            p = o + 0x10
            if buf[p:p + 4] != b'mk2\0':
                raise ValueError('%s marker.mkd: no mk2 header' % model)
            n = struct.unpack_from('<H', buf, p + 6)[0]
            b0 = p + struct.unpack_from('<I', buf, p + 0xc)[0]
            out = []
            for i in range(n):
                r = struct.unpack_from('<3fi3f', buf, b0 + 32 * i)
                out.append(dict(offset=[round(x, 6) for x in r[:3]], bone=r[3], scale=[round(x, 6) for x in r[4:7]]))
            return out
    raise ValueError('%s: no marker.mkd' % model)


def zone_set(d, i, n):
    """Hit-zone records of section-19 entry i (status +0xa8 -> +0x30, 16 B per part; FUN_0055bda8 /
    FUN_0055d2c0 / FUN_0055db50), from this stage file:
      status +0xa8 = FUN_00310e40(parameter set +0x84) = section-20 base + section-19 [i] (u32),
        set by FUN_003153d0; 0 when +0x84 < 0 or the file version FUN_00310c40 (FUN_00312248:
        'bzd' + 3 digits at +0) is < 0xe.
      +0x30 = the entry + its u32 +8 + u32 +4 x 4 + u32 +0 (FUN_0030fc68, run on every section-19
        entry at stage load by FUN_0030f968).
    n = the part count (the model's part boxes: part k reads record k). Returns [{scale: byte 0,
    flags: ushort +2}], or None when status +0xa8 would be 0."""
    sec = SE.sections(d)
    ver = (d[3] - 0x30) * 100 + (d[4] - 0x30) * 10 + d[5] - 0x30 if d[:3] == b'bzd' else -1
    o19, n19, _ = sec[0x13]
    if i < 0 or ver < 0xe or i >= n19:
        return None
    r = sec[0x14][0] + SE.u32(d, o19 + 4 * i)
    z = r + SE.u32(d, r + 8) + SE.u32(d, r + 4) * 4 + SE.u32(d, r)
    return [dict(scale=d[z + 16 * k], flags=struct.unpack_from('<H', d, z + 16 * k + 2)[0]) for k in range(n)]


def lock_bone(d, i):
    """The lock-on bone of section-19 entry i: header byte +0x14 (status +0xa8, as in zone_set), copied
    to C+0x765 at activation by FUN_00340760(C, status +0xa8, 0) (FUN_00559748); vt+0x5c = FUN_00563720
    takes that bone's world position (-1 / no header: FUN_005634e0(actor, 3), 2 below the top)."""
    sec = SE.sections(d)
    ver = (d[3] - 0x30) * 100 + (d[4] - 0x30) * 10 + d[5] - 0x30 if d[:3] == b'bzd' else -1
    o19, n19, _ = sec[0x13]
    if i < 0 or ver < 0xe or i >= n19:
        return -1
    b = d[sec[0x14][0] + SE.u32(d, o19 + 4 * i) + 0x14]
    return b - 256 if b >= 128 else b


def zone_sets(d, recs, boxes):
    """{section-19 index: zone_set} for every index a record of a model with part boxes uses."""
    out = {}
    for r in recs:
        if r['model'] in boxes and r['zones'] >= 0 and str(r['zones']) not in out:
            out[str(r['zones'])] = zone_set(d, r['zones'], len(boxes[r['model']]))
    return out


def clip_frames():
    import export_web_enemy as E
    pth = E.paths('e027')
    res = {}
    for c in AI_CLIPS:
        cbuf, CP = E.V.find_clip(pth['clips'], c)
        res[c] = int(E.AR.clip_frames(cbuf, CP))
    return res


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    stage, out = sys.argv[1], sys.argv[2]
    d = open(stage, 'rb').read()
    trig = triggers(d)
    recs = records(d)
    nscr = len(SE.scripts(d))
    # + script 0 = the stage controller (events 0x22 / 0x23 stage start / restart, L; it arms the
    # bit-0x20000 triggers by difficulty and event flag: notes/enemy-activation.md §1)
    want = {t['script'] for t in trig} | {r['script'] for r in recs} | {0}
    scr = {str(s): script(d, s) for s in sorted(want) if 0 <= s < nscr}
    boxes = dict(e027=hitboxes('e027'))
    os.makedirs(out, exist_ok=True)
    res = dict(format='enemy-ai-2', stage=os.path.basename(stage), triggers=trig, records=recs,
               weapons=weapons(d), scripts=scr, clips=clip_frames(), hitboxes=boxes,
               zones=zone_sets(d, recs, boxes),
               lock_bones={str(r['zones']): lock_bone(d, r['zones']) for r in recs if r['zones'] >= 0})
    path = os.path.join(out, 'ai.json')
    with open(path, 'w') as fh:
        json.dump(res, fh, separators=(',', ':'))
    print('wrote %s: %d triggers, %d records, %d weapons, %d scripts, zone sets %s, clips %s' % (
        path, len(trig), len(recs), len(res['weapons']), len(scr), sorted(res['zones']), res['clips']))


if __name__ == '__main__':
    main()
