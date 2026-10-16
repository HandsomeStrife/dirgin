#!/usr/bin/env python3
"""skd_skeleton.py -- native recovery of Dirge of Cerberus (PS2) character
bind-pose skeletons from the `skd` ("SK2") chunk, and composition of the
per-bone world BIND MATRICES that the VU0 skin program consumes at
*(ctx+0x20) + boneIndex*0x40.

This closes the "bind pose is runtime-only" gap recorded in FORMAT.md §7.5:
km01vnw's container ships NO skeleton, but the EVENT containers
(`extract/_KEL/noPath/0765*.bin`, named e000..) DO -- they carry a
`skeleton.skd` chunk (+ `smd` meshes, `mkd`, `tad`). The skd is a clean,
authored rest skeleton; the bind matrices are composed statically here with
NO runtime/oracle needed (Route (a) of the task).

Format reversed from the game's own EE parser (analysis/decomp/):
  - RFD entry ext int 0x646b73 = "skd"  -> FUN_005b27c8 (relocation; translation x256)
  - FUN_005b2c78: palette bytes = boneCount<<6  => each bone = one 0x40 (4x4 f32) matrix
  - FUN_005a8850: instantiation reads skd (0x646b73) + tad (0x646174) chunks

skd ("SK2") payload layout (offsets relative to RFD-entry payload = entry+0x10):
  +0x00  u32   magic "SK2\0" (0x00324b53)
  +0x04  u8    version/flag (seen 3)
  +0x08  s16   group count (named smd sub-meshes, e.g. e000_L01..e000_S02b)
  +0x0c  u32   ptr -> group-name table (ASCII)                 [file-relative]
  +0x10  u32   ptr -> group-pointer array (runtime-filled)     [file-relative]
  +0x14  s32   BONE COUNT
  +0x18  u32   ptr -> bone-node pointer array (boneCount u32)   [file-relative]

bone node (208 = 0xd0 bytes), the fields this tool reads:
  +0x20  f32[3]  LOCAL translation (xyz)  -- bone offset in PARENT frame
                  (limb length along local X; FUN_005b27c8 scales a copy x256)
  +0x30  f32[4]  LOCAL rotation QUATERNION (x,y,z,w), unit
  +0xc0  s16     PARENT bone index (-1 = root)   [recovered by tree-validity scan]

world_bind[i] = world_bind[parent[i]] . ( Translate(T_i) . Rotate(Q_i) )
(column-major; child-space -> parent-space). Translation is scaled by 256 to
match the skin's world units (per FUN_005b27c8); the scale is uniform so it does
not change shape.  Verified: composing the 103-bone e000 skeleton yields a
symmetric upright humanoid (arms/legs/spine/head) -- see
native/renderer/out/skel_e000_*.png.
"""
import struct, math, json, sys

SKD_MAGIC = 0x00324b53  # "SK2\0"
BONE_STRIDE = 0xd0
TRANS_SCALE = 256.0     # FUN_005b27c8: translation copy *= 256.0

def _find_skd_entry(buf):
    """Return (payload_off, bone_count) of the first skd entry in an RFD container."""
    if len(buf) < 0x14:
        return None
    count = struct.unpack_from("<H", buf, 0x10)[0]
    if count == 0 or count > 256:
        return None
    for i in range(count):
        o = struct.unpack_from("<I", buf, 0x14 + i*4)[0]
        if o + 0x10 > len(buf):
            continue
        ext = buf[o+8:o+0x0c]
        if ext[:3] == b"skd":
            P = o + 0x10
            if struct.unpack_from("<I", buf, P)[0] == SKD_MAGIC:
                return P, struct.unpack_from("<i", buf, P+0x14)[0]
    return None

def _quat_to_m3(q):
    x, y, z, w = q
    n = math.sqrt(x*x+y*y+z*z+w*w) or 1.0
    x, y, z, w = x/n, y/n, z/n, w/n
    return [[1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w)],
            [2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w)],
            [2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y)]]

def _mul4(A, B):
    return [[sum(A[r][k]*B[k][c] for k in range(4)) for c in range(4)] for r in range(4)]

def load_skeleton(path, trans_scale=TRANS_SCALE):
    """Parse an RFD container's skd and return a dict with bone count, per-bone
    local (T,Q), parent indices, and composed 4x4 world bind matrices (row-major
    lists). All values are REAL file data -- nothing invented."""
    buf = open(path, "rb").read()
    r = _find_skd_entry(buf)
    if not r:
        raise ValueError("no skd/SK2 chunk in container")
    P, bc = r
    arr = struct.unpack_from("<I", buf, P+0x18)[0]
    offs = [struct.unpack_from("<I", buf, P+arr+i*4)[0] for i in range(bc)]
    T, Q, parent = [], [], []
    for i in range(bc):
        b = P + offs[i]
        T.append([struct.unpack_from("<f", buf, b+0x20+4*k)[0]*trans_scale for k in range(3)])
        Q.append([struct.unpack_from("<f", buf, b+0x30+4*k)[0] for k in range(4)])
        parent.append(struct.unpack_from("<h", buf, b+0xc0)[0])
    world = [None]*bc
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
    return {"path": path, "bone_count": bc, "parent": parent,
            "local_T": T, "local_Q": Q, "world_bind": world}

if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else \
        "/srv/games/dirge-port/extract/_KEL/noPath/07650001.bin"
    sk = load_skeleton(path)
    js = sys.argv[2] if len(sys.argv) > 2 else "out/skeleton_bind.json"
    import os
    os.makedirs(os.path.dirname(js), exist_ok=True)
    json.dump({"source": path, "bone_count": sk["bone_count"],
               "parent": sk["parent"], "world_bind": sk["world_bind"]},
              open(js, "w"), indent=1)
    j = [(w[0][3], w[1][3], w[2][3]) for w in sk["world_bind"]]
    xs = [p[0] for p in j]; ys = [p[1] for p in j]; zs = [p[2] for p in j]
    print(f"{path}: {sk['bone_count']} bones, root(s)="
          f"{[i for i,p in enumerate(sk['parent']) if p<0 or p==i]}")
    print(f"  joint bbox X[{min(xs):+.0f},{max(xs):+.0f}] "
          f"Y[{min(ys):+.0f},{max(ys):+.0f}] Z[{min(zs):+.0f},{max(zs):+.0f}]")
    print(f"  wrote {js}")
