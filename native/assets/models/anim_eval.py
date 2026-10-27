#!/usr/bin/env python3
"""anim_eval.py -- evaluate a Dirge of Cerberus (PS2) skeletal ANIMATION clip
into per-bone local transforms, compose world matrices, and render an animated
pose via the existing skinned-mesh pipeline.

This closes the "static bind pose only" gap: the character now MOVES, driven by
REAL keyframe data decoded from the game's own motion clips.

=== Format, reversed from the game's own EE + VU code (analysis/decomp/) ===

MOTION CLIP = an RFD entry with ext "mod" (0x646f6d) whose payload is MOD\0 with
VERSION FIELD *(u32)(payload+0x04) == 0x10 (the 14090 version-0x13 "mod" chunks
are MESHES; the 519 version-0x10 ones are motion CLIPS -- verified: for every
v0x10 clip, segmentCount == ceil(frameCount/255), i.e. 255 frames per segment).

Clip payload header (offsets rel. RFD-entry payload = entry+0x10):
  +0x00 u32  magic "MOD\0"
  +0x04 u32  version (0x10 = motion clip)
  +0x08 u32  ANIMATED BONE COUNT (== the target skd's bone count; verified: the
             per-bone track walk consumes each 255-frame segment with ZERO gap
             only when bones == this field)                [FUN_005afa58 @+0x14]
  +0x0c u32  total FRAME count                              [FUN_005af8f8 @+0xc]
  +0x10 u32  SEGMENT count (= ceil(frames/255))            [FUN_005af8f8 @+0x10]
  +0x14 u32  -> segment-pointer table (segCount u32, file-rel) [FUN_005af8f8 @+0x14]
  each segment ptr -> a packed per-bone key stream for <=255 frames.

Per BONE, in stream order (exactly, from FUN_005af8a0's skip = 2 "rot-style"
tracks + 1 "trans-style" track; this is what makes the segment walk exact):
  TRACK P1 (position/aux) : s16 count C; if (C & 0x8000): C&=0x7fff, keys 7 s16
                            (14 B); else keys 4 s16 (8 B) = [time, x, y, z].
                            SPARSE -- only ~6 bones (root + a few) carry it; it is
                            the per-bone TRANSLATION / root motion channel.
  TRACK P2 (position aux) : same encoding; almost always count 0.
  TRACK Q (ROTATION)      : s16 count C; keys 3 s16 (6 B) each, packed
                            sign+magnitude (FUN_005af618) -> the per-bone
                            QUATERNION.  Present on ALL bones (this is the pose).

SUPERSEDED (2026-10-24): the quaternion formula below is WRONG. The real maths is
the VU0 micro program at vcallms(0x218); see anim_rot.py (anim_rot.decode_frame).
Per-bone rotation error vs the hero-frame dump: 72 deg here, 6.8 deg there.
The key bit layout and the track walk below are still right.

QUATERNION decode (TRACK Q key, FUN_005af618): each 6-byte key is
  byte0 = time ; byte1 = y_hi ; u16@2 = (z_mag<<3)|signbits ; u16@4 = (x_mag<<4)|y_lo
  x_mag = u16@4 >> 4           (12 bits)   sign = bit0 of u16@2
  y_mag = (byte1<<4)|(u16@4&0xf)(12 bits)  sign = bit1 of u16@2
  z_mag = u16@2 >> 3           (13 bits)   sign = bit2 of u16@2
The quaternion is q = normalize( sx*x_mag, sy*y_mag, sz*z_mag, 4096 ): the 4th
(w) component is the IMPLICIT fixed-point unit 4096, and the whole is normalized.
Verified: bone0 (4095,0,0,4096)->(.707,0,0,.707) = a clean 90 deg root-axis
rotation (PS2 up-axis convention); every bone yields a UNIT quaternion by
construction; the composed pose is a coherent, clearly-non-bind humanoid.
Frame sampling = LINEAR interp of the signed components between bracketing keys
(FUN_005af6c0 factor (frame-t0)/(t1-t0)) then re-normalize (nlerp).

Composition is IDENTICAL to the bind path (skd_skeleton.py / FUN_005b0200):
  local[i]  = Translate(T_i) . Rotate(Q_i)            (column-major, child->parent)
  world[i]  = world[parent[i]] . local[i]
Q is the decoded animated rotation for every bone; bone offsets T stay at bind
(bone lengths are invariant; the sparse root-translation channel is left at bind
so the character animates IN PLACE -- an honest, conservative choice).

The CLIP and the SKELETON+MESH live in DIFFERENT containers: the clip is bound to
the character at runtime via motion handles (FUN_005a7390, char+0x3bc). We pair
by identical bone count + character tag: the 93-bone protagonist "Vincent" has a
skeleton+mesh container (p012_* meshes) AND 91 "vn" motion clips. e000's exact
103-bone clip is NOT present in the static extract (documented blocker); the
93-bone Vincent pairing is a complete, self-validating real-data substitute.
A correct decode yields a COHERENT humanoid pose that differs from bind; garbage
would mean a wrong decode (the task's own validation criterion).

Usage: python3 -I anim_eval.py
Deps: numpy + PIL (host tools), reuses render_skinned.py + skd_skeleton.py.
"""
import struct, math, os, sys
_REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))  # repo root
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from skd_skeleton import load_skeleton, _quat_to_m3, _mul4
import render_skinned as RS

NOPATH = _REPO + "/extract/_KEL/noPath"
OUT    = _REPO + "/native/renderer/out"

def _u16(b, o): return struct.unpack_from("<H", b, o)[0]
def _u32(b, o): return struct.unpack_from("<I", b, o)[0]
def _s16(b, o): return struct.unpack_from("<h", b, o)[0]


def find_clip(container_path, clip_name):
    """Locate a version-0x10 motion-clip 'mod' entry; return (buf, payload_off)."""
    buf = open(container_path, "rb").read()
    cnt = _u16(buf, 0x10)
    offs = [_u32(buf, 0x14 + 4 * i) for i in range(cnt)]
    nm = clip_name.encode() if isinstance(clip_name, str) else clip_name
    for o in offs:
        if buf[o + 8:o + 11] == b'mod' and buf[o:o + len(nm)] == nm:
            P = o + 0x10
            if buf[P:P + 4] == b'MOD\x00' and _u32(buf, P + 4) == 0x10:
                return buf, P
    raise ValueError("clip %r not found in %s" % (clip_name, container_path))


def _seg_ptr(buf, P, seg):
    segtbl = P + _u32(buf, P + 0x14)
    return P + _u32(buf, segtbl + 4 * seg)


def _skip_rot(buf, p):
    C = _s16(buf, p); n = C & 0x7fff
    stride = 14 if (C & 0x8000) else 8
    return p + 2 + n * stride


def _read_rot_keys(buf, p):
    """Return (end, list[(time, qx,qy,qz)]) for a rotation track (type A assumed;
    type B's first 3 components are taken as the quaternion xyz)."""
    C = _s16(buf, p); n = C & 0x7fff
    typeB = bool(C & 0x8000); stride = 14 if typeB else 8
    keys = []
    for k in range(n):
        kp = p + 2 + k * stride
        t = _s16(buf, kp)
        qx = _s16(buf, kp + 2); qy = _s16(buf, kp + 4); qz = _s16(buf, kp + 6)
        keys.append((t, qx, qy, qz))
    return p + 2 + n * stride, keys


def _read_quat_keys(buf, p):
    """Read a ROTATION (TRACK Q) = packed sign+magnitude quaternion keyframes.
    Return (end, list[(time, sx*xmag, sy*ymag, sz*zmag)]). FUN_005af6c0/af618."""
    C = _s16(buf, p)
    keys = []
    for k in range(C):
        kp = p + 2 + k * 6
        t = buf[kp]
        b1 = buf[kp + 1]
        u2 = _u16(buf, kp + 2)
        u3 = _u16(buf, kp + 4)
        sx = -1.0 if (u2 & 1) else 1.0
        sy = -1.0 if (u2 & 2) else 1.0
        sz = -1.0 if (u2 & 4) else 1.0
        xmag = u3 >> 4
        ymag = (b1 << 4) | (u3 & 0xf)
        zmag = u2 >> 3
        keys.append((t, sx * xmag, sy * ymag, sz * zmag))
    return p + 2 + C * 6, keys


def decode_frame(buf, P, bone_count, frame):
    """Decode one frame of the clip -> list of per-bone quaternions (x,y,z,w).
    Uses the segment covering `frame` (255 frames/segment)."""
    seg = min(int(frame + 0.99) // 255, max(0, _u32(buf, P + 0x10) - 1))
    local_f = float(frame) - seg * 255.0
    p = _seg_ptr(buf, P, seg)
    quats = []
    for _b in range(bone_count):
        p = _skip_rot(buf, p)                    # TRACK P1 (translation, sparse)
        p = _skip_rot(buf, p)                    # TRACK P2 (translation aux)
        p, keysQ = _read_quat_keys(buf, p)       # TRACK Q (rotation = the pose)
        quats.append(_sample_quat(keysQ, local_f))
    return quats


def _sample_quat(keys, frame):
    """nlerp the signed quaternion components between bracketing keys, with the
    implicit w=4096, then normalize. Return identity if the track is empty."""
    if not keys:
        return (0.0, 0.0, 0.0, 1.0)
    if len(keys) == 1:
        _, qx, qy, qz = keys[0]
    else:
        k0 = keys[0]; k1 = keys[-1]
        for i in range(len(keys) - 1):
            if keys[i][0] <= frame <= keys[i + 1][0]:
                k0, k1 = keys[i], keys[i + 1]; break
            if keys[i + 1][0] < frame:
                k0 = keys[i + 1]; k1 = keys[min(i + 2, len(keys) - 1)]
        t0, t1 = k0[0], k1[0]
        a = 0.0 if t1 == t0 else (frame - t0) / (t1 - t0)
        a = max(0.0, min(1.0, a))
        qx = k0[1] * (1 - a) + k1[1] * a
        qy = k0[2] * (1 - a) + k1[2] * a
        qz = k0[3] * (1 - a) + k1[3] * a
    x, y, z, w = qx, qy, qz, 4096.0           # implicit w = fixed-point unit
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    return (x / n, y / n, z / n, w / n)


def compose_world(sk, anim_quats, trans_scale=1.0, neutralize_root=True):
    """Compose per-bone world matrices using animated Q where present, bind Q
    otherwise. Mirrors skd_skeleton.load_skeleton's composition exactly.
    neutralize_root keeps bind Q on bone 0: the clip's root channel is a whole-
    body orientation that the game applies via the character's external world
    matrix (not the skeletal pose), so we leave the character upright in place."""
    bc = sk["bone_count"]; parent = sk["parent"]
    T = [[c * trans_scale for c in t] for t in sk["local_T"]]
    Q = [list(q) for q in sk["local_Q"]]
    nanim = 0
    for i in range(bc):
        if neutralize_root and i == 0:
            continue
        if anim_quats[i] is not None:
            Q[i] = list(anim_quats[i]); nanim += 1
    world = [None] * bc
    def compose(i):
        if world[i] is not None:
            return world[i]
        R = _quat_to_m3(Q[i])
        L = [[R[0][0], R[0][1], R[0][2], T[i][0]],
             [R[1][0], R[1][1], R[1][2], T[i][1]],
             [R[2][0], R[2][1], R[2][2], T[i][2]],
             [0, 0, 0, 1]]
        p = parent[i]
        world[i] = L if (p < 0 or p >= bc or p == i) else _mul4(compose(p), L)
        return world[i]
    for i in range(bc):
        compose(i)
    return world, nanim


def render_pose(mesh_buf, NB, Wm, bc, tex, tag):
    V, _N, tris = RS.assemble(mesh_buf, NB, [np.array(m, float) for m in Wm], bc)
    os.makedirs(OUT, exist_ok=True)
    RS.render(V, _N, tris, f"{OUT}/{tag}.png",      None, 18, 6, False)
    RS.render(V, _N, tris, f"{OUT}/{tag}_3q.png",   None, 55, 8, False)
    if tex is not None:
        RS.render(V, _N, tris, f"{OUT}/{tag}_tex.png", tex, 18, 6, True)
    return len(V), len(tris)


def main():
    SKEL_CONT = f"{NOPATH}/07700301.bin"   # Vincent: skd(93) + p012 smd meshes
    CLIP_CONT = f"{NOPATH}/0c7ee400.bin"   # Vincent motion clips (km01 set)
    CLIP_NAME = "km0102vn"                  # 504 frames, 2 segments, 93 bones

    sk = load_skeleton(SKEL_CONT, trans_scale=1.0)
    bc = sk["bone_count"]
    mesh_buf = open(SKEL_CONT, "rb").read(); NB = len(mesh_buf)
    tex = RS._find_body_texture(SKEL_CONT)
    print(f"skeleton {SKEL_CONT}: {bc} bones ; texture={'yes' if tex is not None else 'NONE'}")

    cbuf, P = find_clip(CLIP_CONT, CLIP_NAME)
    cbones = _u32(cbuf, P + 8); frames = _u32(cbuf, P + 0xc); segs = _u32(cbuf, P + 0x10)
    print(f"clip {CLIP_NAME}: bones={cbones} frames={frames} segments={segs}")
    assert cbones == bc, f"bone-count mismatch clip={cbones} skel={bc}"

    # Bind baseline (for side-by-side)
    Wm_bind = sk["world_bind"]
    nb_v, nb_t = render_pose(mesh_buf, NB, Wm_bind, bc, tex, "14_vincent_bind")
    print(f"  bind pose: verts={nb_v} tris={nb_t}")

    # Animated frames
    for fr in (0, 120, 250):
        quats = decode_frame(cbuf, P, bc, fr)
        Wm, nanim = compose_world(sk, quats, trans_scale=1.0)
        # report how different this pose is from bind (max joint displacement)
        jb = np.array([[m[0][3], m[1][3], m[2][3]] for m in Wm_bind])
        ja = np.array([[m[0][3], m[1][3], m[2][3]] for m in Wm])
        disp = np.linalg.norm(ja - jb, axis=1)
        v, t = render_pose(mesh_buf, NB, Wm, bc, tex, f"14_vincent_anim_f{fr:03d}")
        print(f"  frame {fr}: animated bones={nanim}/{bc}  max joint move={disp.max():.1f}  "
              f"mean={disp.mean():.1f}  verts={v} tris={t}")


if __name__ == "__main__":
    main()
