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

TRANSLATION + SCALE (FUN_005aefd0, called just before af6c0 for each bone):
  TRACK P1 = local TRANSLATION, TRACK P2 = local SCALE, same encoding: s16 count C;
  C & 0x8000 -> keys of 7 s16 = (s16 time, i32 x, i32 y, i32 z), else 4 s16 =
  (time, x, y, z). Units 1/256 (P1 bone 40 = 512 <-> bind T 2.0; bone 34 = 1124 <->
  4.391). Empty P1 -> the bone's bind translation (bone record +0x40); empty P2 ->
  (256, 256, 256) = unit scale. One key -> constant; else the same bracket search
  as af6c0 and a plain lerp. Bone 1 carries the root motion (h_wk: z 0 -> -20.5),
  bone 2 the hips offset (bind T is 0, clip y ~ -11.9).
Micro (instr 137..216): t = P1/256, s = P2/256 (I = 1/256), each blended with the
  previous pose (vf27 / vf26) by vf31w like the quaternion. Then, per bone:
    S_w = S_parent * s          (componentwise; kept in the matrix w column as S-1)
    p_w = p_parent + R_parent (S_parent * t)
    R_w = R_parent R_local      (unscaled; the skin micro scales basis axis k by S_w[k])
Interpolation factor: neither af6c0 nor aefd0 clamps it, and the micro has no
  MINI/MAX on the weights, so the game EXTRAPOLATES past the last key, as sample()
  does. In practice it barely matters: every track of h_swtch/h_wk ends on the last
  frame (59 of 60, 39 of 40), so f > 1 only between frames N-1 and N.
"""
import math, struct
import numpy as np
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


def _segment(buf, P, frame):
    """FUN_005af8f8: segment from min(int(f + 0.99), frames - 1) // 255; local frame."""
    seg = min(int(frame + 0.99), A._u32(buf, P + 0x0c) - 1) // 255
    if seg >= A._u32(buf, P + 0x10):
        seg = 0
    return A._seg_ptr(buf, P, seg), float(frame) - seg * 255.0


def decode_frame(buf, P, bone_count, frame):
    """Same track walk as anim_eval.decode_frame, VU0 rotation maths."""
    p, local_f = _segment(buf, P, frame)
    quats = []
    for _b in range(bone_count):
        p = A._skip_rot(buf, p)
        p = A._skip_rot(buf, p)
        p, keys = read_keys(buf, p)
        quats.append(sample(keys, local_f))
    return quats


def read_vec_keys(buf, p):
    """TRACK P1/P2 -> (end, [(time, x, y, z)]) in raw 1/256 units (FUN_005aefd0)."""
    C = A._s16(buf, p); n = C & 0x7fff
    keys = []
    if C & 0x8000:
        for k in range(n):
            kp = p + 2 + k * 14
            keys.append((A._s16(buf, kp),) + struct.unpack_from("<3i", buf, kp + 2))
        return p + 2 + n * 14, keys
    for k in range(n):
        keys.append(struct.unpack_from("<4h", buf, p + 2 + k * 8))
    return p + 2 + n * 8, keys


def sample_vec(keys, frame):
    """FUN_005aefd0 for one track: None if empty (caller supplies the default),
    else the lerp between bracketing keys, divided by 256. f is not clamped."""
    if not keys:
        return None
    if len(keys) == 1:
        return tuple(c / 256.0 for c in keys[0][1:])
    n = len(keys) - 1
    lo = 0
    while lo < n and keys[lo][0] < frame:
        lo += 1
    lo -= 1 if lo > 0 else 0
    k0, k1 = keys[lo], keys[lo + 1]
    f = (frame - k0[0]) / float(k1[0] - k0[0])
    return tuple((a * (1 - f) + b * f) / 256.0 for a, b in zip(k0[1:], k1[1:]))


def decode_frame_trs(buf, P, bone_count, frame):
    """Per bone (t or None, s or None, q): translation, scale, rotation at `frame`."""
    p, local_f = _segment(buf, P, frame)
    out = []
    for _b in range(bone_count):
        p, kt = read_vec_keys(buf, p)
        p, ks = read_vec_keys(buf, p)
        p, kq = read_keys(buf, p)
        out.append((sample_vec(kt, local_f), sample_vec(ks, local_f), sample(kq, local_f)))
    return out


def find_clip(container_path, name):
    """(buf, payload) of motion clip `name`. Unlike anim_eval.find_clip, accepts the
    version-0x13 clips of Vincent's 0770000x containers (same layout)."""
    buf = open(container_path, "rb").read()
    for i in range(A._u16(buf, 0x10)):
        o = A._u32(buf, 0x14 + 4 * i)
        if (buf[o + 8:o + 11] == b'mod' and buf[o:o + 8].split(b'\0')[0] == name.encode()
                and buf[o + 0x10:o + 0x14] == b'MOD\x00'):
            return buf, o + 0x10
    raise ValueError("clip %r not found in %s" % (name, container_path))


def clip_frames(buf, P):
    return A._u32(buf, P + 0x0c)


def _m3(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def compose(sk, trs, use_t=True, use_s=True, root_motion=True):
    """World 4x4 per bone (numpy, column-vector, same convention as
    skd_skeleton.world_bind) from decode_frame_trs output, the way the micro does it.
    use_t / use_s = False keep the bind translation / unit scale (rotation-only pose).
    root_motion = False zeroes bone 1's x/z translation (walk in place)."""
    bc = sk["bone_count"]; par = sk["parent"]
    R = [None] * bc; S = [None] * bc; Pw = [None] * bc
    def go(i):
        if R[i] is not None:
            return
        t, s, q = trs[i]
        t = np.array(t if (use_t and t is not None) else sk["local_T"][i], float)
        if i == 1 and not root_motion:
            t[0] = t[2] = 0.0
        s = np.array(s if (use_s and s is not None) else (1.0, 1.0, 1.0), float)
        p = par[i]
        if p < 0 or p >= bc or p == i:
            Rp, Sp, Pp = np.eye(3), np.ones(3), np.zeros(3)
        else:
            go(p); Rp, Sp, Pp = R[p], S[p], Pw[p]
        R[i] = Rp @ _m3(q); S[i] = Sp * s; Pw[i] = Pp + Rp @ (Sp * t)
    W = []
    for i in range(bc):
        go(i)
        M = np.eye(4); M[:3, :3] = R[i] * S[i][None, :]; M[:3, 3] = Pw[i]
        W.append(M)
    return W
