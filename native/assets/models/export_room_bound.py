#!/usr/bin/env python3
"""export_room_bound.py -- assemble the WHOLE z143 Mako Reactor room (all 13
geometry chunks, which share one world frame) into textured meshes using the
recovered per-packet binding, merged GLOBALLY by FileCode so the result fits the
renderer's group limit and draws as one coherent room.

Unlike export_bmd_bound.py (one chunk -> one .list), this unions every chunk and
emits ONE DMSH per distinct room texture (all its packets from all chunks) plus a
single 'unbound' group. ~18 textures + unbound << render_env_textured's MAXG.

Binding sources (same as the validated loader, see notes/session-2026-10-15-gsdump.md):
  - out/z143_pktfc/pf_<chunk>.json : authoritative per-packet FileCode (oracle).
  - --heuristic out/z143_backface/assignments.json : fill oracle-null packets with
    the back-face heuristic FileCode where confident (free-camera cosmetic fill).

    python3 -I export_room_bound.py [--heuristic] [--assignments F] [-o OUTPREFIX]

Output: <OUTPREFIX>_gNN.mesh, <OUTPREFIX>.list, <OUTPREFIX>_manifest.json.
Trusted repo data + bounds-checked raw BMD read (via export_bmd_textured); run -I.
"""
import os, sys, json, glob, argparse
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_bmd_textured import find_bmd_payload, decode_packets, OUT, EXTRACT
from export_bmd_bound import build_flat, emit_group, tex_png_for


def chunk_ids():
    return [os.path.basename(p)[3:11]
            for p in sorted(glob.glob(os.path.join(OUT, "z143_pktfc", "pf_*.json")))]


def per_packet_filecodes(cid, use_heuristic, assignments):
    pf = json.load(open(os.path.join(OUT, "z143_pktfc", "pf_%s.json" % cid)))
    fc = [f if f else None for f in pf]
    if use_heuristic and assignments is not None:
        mine = [e for e in assignments if e.get("chunk") == cid]
        if len(mine) == len(fc):
            for i, e in enumerate(mine):
                if fc[i] is None and e.get("source") == "heuristic" and e.get("confident"):
                    fc[i] = e["fc"]
    return fc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--heuristic", action="store_true")
    ap.add_argument("--assignments", default=os.path.join(OUT, "z143_backface", "assignments.json"))
    ap.add_argument("-o", "--out", default=os.path.join(OUT, "z143_room", "room"))
    a = ap.parse_args()

    assignments = None
    if a.heuristic and os.path.exists(a.assignments):
        assignments = json.load(open(a.assignments))

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)

    # accumulate per global FileCode across all chunks
    flat_by_fc = defaultdict(list)     # fc -> interleaved float list
    ntri_by_fc = defaultdict(int)
    abe_by_fc  = defaultdict(bool)
    npk_by_fc  = defaultdict(int)

    cids = chunk_ids()
    for cid in cids:
        binp = os.path.join(EXTRACT, "%s.bin" % cid)
        if not os.path.exists(binp):
            continue
        buf = open(binp, "rb").read()
        pay = find_bmd_payload(buf)
        if pay is None:
            continue
        packets = decode_packets(buf, pay)
        fc = per_packet_filecodes(cid, a.heuristic, assignments)
        n = min(len(fc), len(packets))
        groups = defaultdict(list)
        for i in range(n):
            groups[fc[i] if fc[i] else "unbound"].append(i)
        for key, idxs in groups.items():
            flat, ntri = build_flat(packets, idxs)
            if ntri == 0:
                continue
            flat_by_fc[key].extend(flat)
            ntri_by_fc[key] += ntri
            npk_by_fc[key]  += len(idxs)
            if any(packets[i]["abe"] for i in idxs):
                abe_by_fc[key] = True

    # bound FileCodes first (by tri weight), 'unbound' last
    order = sorted([k for k in flat_by_fc if k != "unbound"], key=lambda k: -ntri_by_fc[k])
    if "unbound" in flat_by_fc:
        order.append("unbound")

    manifest = {"method": "whole-room union of 13 chunks, merged by FileCode",
                "heuristic_fill": bool(a.heuristic), "chunks": cids, "groups": []}
    total_tris = bound_tris = 0
    for gi, fc in enumerate(order):
        mesh_name, bbmin, bbmax = emit_group(flat_by_fc[fc], ntri_by_fc[fc], a.out, gi)
        cont = None
        if fc != "unbound":
            _, cont = tex_png_for(fc)
            bound_tris += ntri_by_fc[fc]
        manifest["groups"].append({
            "mesh": mesh_name, "filecode": (None if fc == "unbound" else fc),
            "packets": npk_by_fc[fc], "tris": ntri_by_fc[fc],
            "texture_container": cont, "abe": abe_by_fc[fc]})
        total_tris += ntri_by_fc[fc]

    json.dump(manifest, open(a.out + "_manifest.json", "w"), indent=2)
    with open(a.out + ".list", "w") as f:
        for g in manifest["groups"]:
            f.write("%s\t%s\t%d\n" % (g["mesh"], g["texture_container"] or "NONE",
                                      1 if g["abe"] else 0))
    nb = sum(1 for g in manifest["groups"] if g["filecode"])
    print("whole room: %d chunks, %d textures (+%s unbound), %d/%d tris textured "
          "(heuristic=%s) -> %s.list"
          % (len(cids), nb, "1" if "unbound" in flat_by_fc else "0",
             bound_tris, total_tris, a.heuristic, a.out))


if __name__ == "__main__":
    main()
