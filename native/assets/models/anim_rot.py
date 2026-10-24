#!/usr/bin/env python3
"""anim_rot.py -- clip ROTATION key decode, as done by the game's VU0 microcode.

anim_eval.py read a TRACK Q key as q = normalize(sx*xmag, sy*ymag, sz*zmag, 4096).
That is not what the game does. FUN_005af618 only unpacks integer magnitudes and
+/-1 signs; FUN_005afa58 then loads them into VU0 and runs the resident micro
program at vcallms(0x218) (port/vu/disasm/vu0_skin_micro.txt, instr 67..180).

EE side (FUN_005afa58, raw lqc2 order at 0x5afb74 / 0x5afe0c):
  vf29 = DAT_005b3af0 = sin Taylor coeffs (-1/6, 1/120, -1/5040, 1/362880)
  vf30 = DAT_005b3ae0 = cos Taylor coeffs (-1/2, 1/24, -1/720, 1/40320)
  vf31 = (pi/8192, pi/8192, 1/scale, blend weight)   [DAT_005b3b00 = 3.83495e-4]
  vf09/vf10 = int magnitudes (x,y,z) of key0 / key1     (FUN_005af6c0 -> af618)
  vf11/vf12 = signs (x,y,z) of key0 / key1; w = (1-f) / f   (interp factor)

Micro (per key):
  ax = xmag*pi/8192, ay = ymag*pi/8192, az = zmag*pi/8192   (ITOF0, MULx/MULy vf31)
  c = cos(a), s = sin(a) per component (8th/9th-order Taylor in vf30/vf29)
  axis = (cx*cy, cy*sx, sy) * (signx, signy, signz)          (unit by construction)
  q    = (axis * sin(az), cos(az))
So x,y are spherical angles of the rotation axis (12 bits each over [0, pi/2],
octant from the sign bits) and z is the HALF-angle (13 bits over [0, pi]).
Then: if dot(q0,q1) < 0, q0 = -q0; q = normalize(q0*(1-f) + q1*f)  (nlerp),
then a blend with the previous pose (vf28) by vf31w, then quat -> matrix.
"""
import math
import anim_eval as A

K = math.pi / 8192.0          # vf31.x == vf31.y == 0.0003834952


def key_quat(xm, ym, zm, sx, sy, sz):
    """One TRACK Q key -> unit quaternion (x, y, z, w). Magnitudes unsigned."""
    ax, ay, az = xm * K, ym * K, zm * K
    cx, cy = math.cos(ax), math.cos(ay)
    snx, sny = math.sin(ax), math.sin(ay)
    s = math.sin(az)
    return (sx * cx * cy * s, sy * cy * snx * s, sz * sny * s, math.cos(az))


def read_keys(buf, p):
    """TRACK Q -> (end, [(time, quat)]). Raw unpack of FUN_005af618 (signs kept
    even when a magnitude is 0: cos(0) = 1, so the sign still matters)."""
    C = A._s16(buf, p)
    keys = []
    for k in range(C):
        kp = p + 2 + k * 6
        u2 = A._u16(buf, kp + 2); u3 = A._u16(buf, kp + 4)
        xm = u3 >> 4; ym = (buf[kp + 1] << 4) | (u3 & 0xf); zm = u2 >> 3
        sg = [-1.0 if (u2 >> i) & 1 else 1.0 for i in range(3)]
        keys.append((buf[kp], key_quat(xm, ym, zm, *sg)))
    return p + 2 + C * 6, keys


def sample(keys, frame):
    """FUN_005af6c0 bracket search + VU0 nlerp with hemisphere flip."""
    if not keys:                       # af6c0: zero mags, +1 signs -> identity
        return (0.0, 0.0, 0.0, 1.0)
    if len(keys) == 1:
        return keys[0][1]
    # af6c0: lo = first key whose time >= frame, then step back one (if > 0)
    n = len(keys) - 1
    lo = 0
    while lo < n and keys[lo][0] < frame:
        lo += 1
    lo -= 1 if lo > 0 else 0
    k0, k1 = keys[lo], keys[min(lo + 1, n)]
    dt = k1[0] - k0[0]
    f = 0.0 if dt == 0 else (frame - k0[0]) / dt
    q0, q1 = k0[1], k1[1]
    if sum(a * b for a, b in zip(q0, q1)) < 0:
        q0 = tuple(-c for c in q0)
    q = [a * (1 - f) + b * f for a, b in zip(q0, q1)]
    m = math.sqrt(sum(c * c for c in q)) or 1.0
    return tuple(c / m for c in q)


def decode_frame(buf, P, bone_count, frame):
    """Same track walk as anim_eval.decode_frame, VU0 rotation maths."""
    seg = min(int(frame + 0.99) // 255, max(0, A._u32(buf, P + 0x10) - 1))
    local_f = float(frame) - seg * 255.0
    p = A._seg_ptr(buf, P, seg)
    quats = []
    for _b in range(bone_count):
        p = A._skip_rot(buf, p)
        p = A._skip_rot(buf, p)
        p, keys = read_keys(buf, p)
        quats.append(sample(keys, local_f))
    return quats
