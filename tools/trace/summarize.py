#!/usr/bin/env python3
"""Print a compact per-frame table for a trace, or compare two traces.

    python3 -I tools/trace/summarize.py out/b_walk.jsonl [--from N --to M]
    python3 -I tools/trace/summarize.py --diff out/b_walk.jsonl out/b_walk2.jsonl
"""
import argparse
import json
import math


def load(path):
    lines = [json.loads(l) for l in open(path)]
    return lines[0]["meta"], lines[1:]


def head_layer(d, key="mp"):
    h = bytes.fromhex(d[key + "_head"])
    return d[key + "_layers"][h[8] & 7], h[9]


def table(recs, lo, hi):
    print("frm step  pos(x,y,z)                    d_xz   gspd  mode st gnd motion   head:clip frame  dfrm wt n | cam yaw")
    prev = None
    for d in recs[lo:hi]:
        pos = d["matrix"][3]
        dxz = math.hypot(pos[0] - prev["matrix"][3][0], pos[2] - prev["matrix"][3][2]) if prev else 0.0
        lay, n = head_layer(d)
        dfr = ""
        if prev:
            pl, _ = head_layer(prev)
            if pl["clip"] == lay["clip"]:
                dfr = "%.3f" % (lay["frame"] - pl["frame"])
        st = bytes.fromhex(d["p_state"])[4]
        gnd = 1 if d["mflags"] & 0x200 else 0
        print("%3d %.2f (%8.2f,%7.2f,%8.2f) %6.3f %6.1f %4x %2d %d  %-8s %-8s %6.2f %6s %3d %d | %.3f" % (
            d["frame"], d["step"], pos[0], pos[1], pos[2], dxz, d["ground_speed"], d["move_mode"],
            st, gnd, d["motion"], lay["clip"], lay["frame"], dfr, lay["weight"], n,
            d["cam_fov_pitch_yaw"][2]))
        prev = d


def diff(a, b):
    n = min(len(a), len(b))
    first = None
    for i in range(n):
        x, y = dict(a[i]), dict(b[i])
        if x != y:
            keys = [k for k in x if x[k] != y.get(k)]
            first = (i, keys)
            break
    if first is None:
        print("identical over %d frames (every logged field, bit for bit)" % n)
    else:
        print("first difference at frame %d in %s" % first)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--from", dest="lo", type=int, default=0)
    ap.add_argument("--to", dest="hi", type=int, default=10 ** 9)
    ap.add_argument("--diff", action="store_true")
    a = ap.parse_args()
    if a.diff:
        diff(load(a.files[0])[1], load(a.files[1])[1])
        return
    for f in a.files:
        meta, recs = load(f)
        print(meta)
        table(recs, a.lo, a.hi)


if __name__ == "__main__":
    main()
