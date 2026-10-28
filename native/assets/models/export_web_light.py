#!/usr/bin/env python3
"""export_web_light.py -- Vincent's light rig inputs for play.html (data/vincent/light.json).

    python3 -I export_web_light.py [OUT_DIR]      (default native/web/data/vincent)

What the game does (decoded; notes/fidelity-audit.md "Character lighting"):
  FUN_005ab0b0 builds an actor's VU1 program-D rig every drawn frame:
    light 0  dir = env slot obj[+0x44e] light direction (stored negated in the BMD, bmd_env.py),
             colour = light_col_a/128                                  -> x obj[+0x450]/128
    light 1  nearest active point light of the 4-slot table DAT_004eea50 (FUN_0030e208), or the
             actor's own obj+0x160: effect lights (FUN_00271fb8). Not exported: the page has no
             effects, and the table is empty in the z128 save state.
    light 2  room light: FUN_0026a710 -> FUN_00270d08 over every loaded BMD's light grid
             (BMD version >= 0x12, struct at BMD+0x98): the light with the least d^2/k in the
             actor point's grid cell (type 2 = spot: skipped when outside its cone).
             dir = (light - p)/d, colour = rgb * k*100/d^2 (NOT scaled by +0x450); smoothed by
             FUN_005ab608 only while the chosen light changes.
    ambient  light_col_b/128                                           -> x obj[+0x450]/128
  Actor point p = obj+0x10 matrix x (obj+0x90 + obj+0xa0)/2 (FUN_005a72a8).
  Program D per vertex: rgb = min(vcol * (sum_i C_i*max(L_i.n, 0) + A), 255).

Layout of the BMD light grid (file offsets are relative to the BMD base, file offset 0x30;
the game relocates them by adding the load address):
  BMD+0x98  i32 count (0 = no grid), ..., +0xa8 u32 grid
  grid      f32 cell size, f32 x0, f32 z0, i16 nx, i16 nz, then nx*nz u32 cell lists (0 = empty)
  cell list i32 n, 12 bytes, then n u32 light records
  record    +0x40 vec3 position, +0x4c i32 type, +0x50 vec3 spot dir + f32 cos limit,
            +0x60 rgb (floats) + f32 k
z128 loads three BMD parts (DAT_004e4c60 table, save state): 06800111 and 06800211 have
grids; the third has count 0.
"""
import os, sys, json, struct

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
NOPATH = os.path.join(REPO, "extract", "_KEL", "noPath")
sys.path.insert(0, HERE)
import bmd_env as BE

ENV_FILE = "06800012.bin"              # z128 env slots (bmd_env.py)
ENV_SLOT = 0                           # Vincent's obj+0x44e in z128 (save state)
GRID_FILES = ["06800111.bin", "06800211.bin"]   # z128 BMD parts with a light grid
BMD_BASE = 0x30                        # BMD header offset inside these files
# Vincent's motion player (MP = C+0x7c8), read from the z128 save state:
#   +0x450 = 122: light 0 / ambient scale byte, set by script (FUN_00342240 from actor+0x779)
#   +0x90 / +0xa0 = model-space bbox min/max, set once at model setup (FUN_005a8850 ->
#   FUN_005b0380 / FUN_005affb0, not decoded); FUN_005a72a8 takes their mean.
SCALE_BYTE = 122
BBOX_MIN = (-8.8122, -18.0863, -6.9951)
BBOX_MAX = (8.5174, -0.3689, 10.3344)


def read_grid(path, base=BMD_BASE):
    d = open(path, "rb").read()
    u32 = lambda o: struct.unpack_from("<I", d, base + o)[0]
    if u32(0x98) == 0:
        return None
    g = u32(0x98 + 0x10)
    size, x0, z0 = struct.unpack_from("<3f", d, base + g)
    nx, nz = struct.unpack_from("<2h", d, base + g + 0xc)
    recs, index, cells = [], {}, []
    for c in range(nx * nz):
        cl = u32(g + 0x10 + 4 * c); lst = []
        if cl:
            for k in range(u32(cl)):
                r = u32(cl + 16 + 4 * k)
                if r not in index:
                    o = base + r
                    index[r] = len(recs)
                    recs.append(dict(
                        pos=list(struct.unpack_from("<3f", d, o + 0x40)),
                        type=struct.unpack_from("<i", d, o + 0x4c)[0],
                        spot=list(struct.unpack_from("<4f", d, o + 0x50)),
                        rgb=list(struct.unpack_from("<3f", d, o + 0x60)),
                        k=struct.unpack_from("<f", d, o + 0x6c)[0]))
                lst.append(index[r])
        cells.append(lst)
    return dict(file=os.path.basename(path), size=size, x0=x0, z0=z0, nx=nx, nz=nz,
                cells=cells, lights=recs)


def main(argv):
    out = os.path.abspath(argv[0]) if argv else os.path.join(REPO, "native", "web", "data", "vincent")
    r = os.system(f"git -C '{REPO}' check-ignore -q '{os.path.join(out, 'light.json')}'")
    if r != 0:
        sys.exit(f"REFUSING: {out} is not gitignored (game-derived output)")
    env = BE.read_env(open(os.path.join(NOPATH, ENV_FILE), "rb").read(), ENV_SLOT)
    grids = [g for g in (read_grid(os.path.join(NOPATH, f)) for f in GRID_FILES) if g]
    j = dict(
        format="dirge light rig v1 (export_web_light.py)",
        env=dict(file=ENV_FILE, slot=ENV_SLOT,
                 dir0=[-x for x in env["light_dir1"]],          # VU +0xc0: toward the light
                 col0=[c / 128.0 for c in env["light_col_a"]],
                 ambient=[c / 128.0 for c in env["light_col_b"]]),
        scale_byte=SCALE_BYTE,
        actor_center=[(a + b) / 2 for a, b in zip(BBOX_MIN, BBOX_MAX)],
        pick_start=1e6,                                           # FUN_00270ec0: DAT_004e88a4
        grids=grids,
        note="program D: rgb = min(vcol*(C0*max(L0.n,0) + C2*max(L2.n,0) + A), 255); "
             "C0, A x scale_byte/128; light 2 from the grids (FUN_00270d08)")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "light.json"), "w") as f:
        json.dump(j, f, separators=(",", ":"))
    print("light.json: %d grids, %d lights -> %s" % (len(grids), sum(len(g["lights"]) for g in grids),
                                                      os.path.join(out, "light.json")))


if __name__ == "__main__":
    main(sys.argv[1:])
