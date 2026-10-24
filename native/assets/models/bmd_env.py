#!/usr/bin/env python3
"""bmd_env.py -- read the environment (light + fog) slots from a room BMD header.

Layout from FUN_0026afe8 (analysis/decomp), which turns the slots into VU1 constants.
Offsets are from the 'BMD\\0' magic. The u32 at +4 is the BMD version; the game reads
the env table only when it is > 3 (read_env refuses older files). 32 slots of 0xC4 bytes
starting at +0x24 (the loop runs while +0xb4 + n*0xC4 < +0x1934). Per slot, relative to the slot start:
  +0x00 rgb  colour 0           (not read by FUN_0026afe8)
  +0x04 rgb  light colour A     (/128 -> VU +0xd0)
  +0x08 rgb  light colour B     (/128 -> VU +0xe0)
  +0x0c vec3 light direction 1  (negated -> VU +0xc0)
  +0x1c f32  clip near, +0x20 f32 clip far   (x10, projection; far also x a global)
  +0x24 rgb  fog colour         (GS FOGCOL)
  +0x28 f32  fog near, +0x2c f32 fog far    (x10, world units of view depth w)
  +0x30 f32  fog floor F (0..255; 255 when fog is globally off)
  +0x84 vec3 light direction 2  (negated -> VU +0xf0), +0x90 i32 (-> VU +0xfc; <0 = computed)
Fog per vertex: F = 255*(10*far - w)/(10*far - 10*near), clamped to [floor, 255], i.e.
the renderer's '@fog R G B near far min' with near*10, far*10, floor/255.
Slot 0 is the level default (the only slot with direction 2 set in z128). Which slot is
live at runtime is not decoded; for z128's hero frame slot 0 matches the dump.
"""
import struct

ENV_OFF, ENV_STRIDE, ENV_SLOTS = 0x24, 0xC4, 32
MIN_ENV_VERSION = 4  # the game skips the env table when the BMD version (+4) is <= 3
FOG_SCALE = 10.0   # FUN_0026afe8: fVar16 = 10.0 on fog near/far (and clip near/far)


def _rgb(w):
    return (w & 255, (w >> 8) & 255, (w >> 16) & 255)


def read_env(buf, slot=0):
    """Return the env slot as a dict, or None if the buffer holds no BMD env table.
    Raises ValueError on a BMD older than MIN_ENV_VERSION (the game ignores its table)."""
    m = buf.find(b'BMD\0')
    if m < 0 or not (0 <= slot < ENV_SLOTS) or m + 8 > len(buf):
        return None
    ver = struct.unpack_from('<I', buf, m + 4)[0]
    if ver < MIN_ENV_VERSION:
        raise ValueError('BMD version %d at +4 is <= 3: the game does not read the env '
                         'table of such files, so there is no fog/light to take' % ver)
    o = m + ENV_OFF + slot * ENV_STRIDE
    if o + ENV_STRIDE > len(buf):
        return None
    w = struct.unpack_from('<13I', buf, o); f = struct.unpack_from('<13f', buf, o)
    return {'slot': slot, 'bmd_version': ver, 'colour0': _rgb(w[0]), 'light_col_a': _rgb(w[1]),
            'light_col_b': _rgb(w[2]), 'light_dir1': tuple(f[3:6]),
            'clip_near': f[7], 'clip_far': f[8],
            'fog_col': _rgb(w[9]), 'fog_near': f[10], 'fog_far': f[11], 'fog_floor': f[12],
            'light_dir2': struct.unpack_from('<3f', buf, o + 0x84),
            'i90': struct.unpack_from('<i', buf, o + 0x90)[0]}


def fog_directive(env):
    """Return (line, notes): line is '@fog R G B near far min' for render_scene_textured,
    or None; notes are warnings explaining a None or an adjusted value."""
    near, far, floor = env['fog_near'] * FOG_SCALE, env['fog_far'] * FOG_SCALE, env['fog_floor']
    if not (far > near):
        return None, ['fog far %g <= near %g (x%g): invalid fog range, not "no fog"; '
                      'no @fog written' % (far, near, FOG_SCALE)]
    if floor >= 255:
        return None, ['fog floor %g >= 255: fog is off for this slot' % floor]
    notes = []
    if floor < 0:
        notes.append('fog floor %g < 0, clamped to 0 (outside the 0..255 range)' % floor)
        floor = 0
    r, g, b = env['fog_col']
    return '@fog %d %d %d %g %g %g' % (r, g, b, near, far, floor / 255.0), notes


if __name__ == '__main__':
    import sys
    for p in sys.argv[1:]:
        try:
            e = read_env(open(p, 'rb').read())
        except ValueError as x:
            print(p, 'refused:', x); continue
        print(p, e, fog_directive(e) if e else None)
