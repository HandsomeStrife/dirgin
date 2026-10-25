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


# --- character lighting (VU1 program D) ------------------------------------------------
# FUN_005ab0b0 builds an actor's light rig every frame from env slot obj[+0x44e]:
#   light 0  dir = -light_dir1 (VU +0xc0, toward the light), colour = light_col_a/128
#   light 1  a point light near the actor (FUN_0026a710 / obj+0x160 / FUN_0030e208):
#            dir = normalize(lightpos - actorpos), colour = col * k*100/dist^2.
#            Runtime only; NOT in the env block.
#   light 2  smoothed toward colour 0 (FUN_005ab608): 0 at steady state
#   ambient  light_col_b/128 (VU +0xe0)
# Lights 0, 1 and the ambient are then scaled by obj[+0x450]/128. FUN_005ab738 only blends
# when the slot changes. light_dir2/i90 go to FUN_005acc00 (obj+0x453), not to colour.
# Program D per vertex: rgb = min(vcol * (sum C_i*max(L_i.n, 0) + A), 255), then GS
# MODULATE texel*rgb/128.
# Vincent, z128 hero frame 163235: the dump's 10930 body vertex colours are reproduced to a
# median 0.9 levels with slot 0, scale 123/128 and the point light HERO_POINT_LIGHT (fitted
# from those colours: direction + colour are runtime state, not decoded from game data).
HERO_SCALE_BYTE = 123
HERO_POINT_LIGHT = ((-0.1514, -0.3678, -0.9175), (83.7, 43.6, 28.1))   # dir, colour x128


def char_rig(env, scale_byte=128, point=None):
    """Return {'lights': [(dir3, rgb3) x up to 2], 'ambient': rgb3} in program-D units
    (colours already x scale_byte/128). point = (dir3, rgb3 x128) or None."""
    s = scale_byte / 128.0
    d = tuple(-x for x in env['light_dir1'])
    lights = [(d, tuple(c / 128.0 * s for c in env['light_col_a']))]
    if point is not None:
        lights.append((tuple(point[0]), tuple(c / 128.0 * s for c in point[1])))
    return {'lights': lights, 'ambient': tuple(c / 128.0 * s for c in env['light_col_b'])}


def rig_args(rig):
    """render_scene_textured flags for a char_rig(): -light0/-light1 dx,dy,dz,r,g,b -ambient r,g,b."""
    a = []
    for k, (d, c) in enumerate(rig['lights']):
        a += ['-light%d' % k, ','.join('%.6g' % x for x in tuple(d) + tuple(c))]
    return a + ['-ambient', ','.join('%.6g' % x for x in rig['ambient'])]


if __name__ == '__main__' and len(__import__('sys').argv) > 1 and __import__('sys').argv[1] == '--rig':
    # bmd_env.py --rig BMD [slot] [scale_byte] [hero]   -> print renderer light flags
    import sys
    a = sys.argv[2:]
    e = read_env(open(a[0], 'rb').read(), int(a[1]) if len(a) > 1 else 0)
    if e is None:
        sys.exit('no BMD env table in %s' % a[0])
    hero = len(a) > 3 and a[3] == 'hero'
    print(' '.join(rig_args(char_rig(e, int(a[2]) if len(a) > 2 else 128,
                                     HERO_POINT_LIGHT if hero else None))))
    sys.exit(0)

if __name__ == '__main__':
    import sys
    for p in sys.argv[1:]:
        try:
            e = read_env(open(p, 'rb').read())
        except ValueError as x:
            print(p, 'refused:', x); continue
        print(p, e, fog_directive(e) if e else None)
