#!/usr/bin/env python3
"""
Bake a room-binding MANIFEST that the native C loader (engine/scene/material_loader.c)
consumes to build a DocMaterialModel + DocTextureContext for a z143 room chunk.

WHY A BAKE STEP. The recovered per-packet texture binding is indexed by the Python
decode_packets() order (export_bmd_textured.py owns the fragile VIF packet
enumeration). Re-deriving that order in C risks packet-for-packet drift. So the one
Python decoder that owns packet order emits a stable, decode-order manifest; the C
loader just reads it. (See scratchpad material_loader_plan.md and FORMAT.md §6: the
runtime material model is NOT stored in the BMD, so the binding is reconstructed from
the empirically recovered per-packet FileCode, not parsed from the file.)

INPUTS
  <chunk.bin>        the BMD chunk, e.g. extract/_KEL/noPath/068f0112.bin
  <pf_binding.json>  out/z143_pktfc/pf_<chunk>.json : list aligned 1:1 to
                     decode_packets() order; FileCode string or null (AUTHORITATIVE).
  --assignments F    out/z143_backface/assignments.json (optional)
  --heuristic        with --assignments: fill oracle-null packets with the heuristic
                     FileCode, but ONLY where source=='heuristic' and confident.
                     Without this flag (default) nulls stay unbound (slot -1).

OUTPUT (line-based text, trivially parseable in C; '#' comments; see MANIFEST FORMAT)

  # dirgin-room-binding v1
  chunk 068f0112
  slots 12
  slot 0 068fff0f 99 oracle
  slot 1 068fff23 18 oracle
  ...
  packets 1803
  0
  0
  -1
  ...                     (one slot index per packet, decode order; -1 = unbound)

The per-slot count on each `slot` line is the EXPECTED number of packets that must
resolve to that slot, so the C test is self-checking against this baked ground truth.

  python3 -I emit_loader_manifest.py <chunk.bin> <pf.json> -o <manifest>
"""
import sys, os, json, argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_bmd_textured import find_bmd_payload, decode_packets


def chunk_id_from_path(p):
    b = os.path.basename(p)
    return b[:8] if len(b) >= 8 else os.path.splitext(b)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("chunk_bin")
    ap.add_argument("pf_json")
    ap.add_argument("--assignments")
    ap.add_argument("--heuristic", action="store_true")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()

    chunk = chunk_id_from_path(a.chunk_bin)

    buf = open(a.chunk_bin, "rb").read()
    pay = find_bmd_payload(buf)
    if pay is None:
        sys.exit("no BMD payload in %s" % a.chunk_bin)
    pkts = decode_packets(buf, pay)

    pf = json.load(open(a.pf_json))
    if len(pkts) != len(pf):
        sys.exit("packet/pf length mismatch: %d decoded vs %d in %s"
                 % (len(pkts), len(pf), a.pf_json))

    # per-packet FileCode: oracle first; optional heuristic fill of the nulls.
    fc = [f if f else None for f in pf]
    src = ["oracle" if f else None for f in fc]
    if a.assignments and a.heuristic:
        asg = json.load(open(a.assignments))
        # assignments.json is all 13 chunks concatenated in per-chunk decode order;
        # take the slice for THIS chunk, in order.
        mine = [e for e in asg if e.get("chunk") == chunk]
        if len(mine) != len(pf):
            sys.exit("assignments slice for %s is %d, expected %d"
                     % (chunk, len(mine), len(pf)))
        for i, e in enumerate(mine):
            if fc[i] is None and e.get("source") == "heuristic" and e.get("confident"):
                fc[i] = e["fc"]
                src[i] = "heuristic"

    # dense slots per distinct bound FileCode, in order of first appearance.
    slot_of = {}
    slot_fc = []
    slot_src = []
    for i, f in enumerate(fc):
        if f is None:
            continue
        if f not in slot_of:
            slot_of[f] = len(slot_fc)
            slot_fc.append(f)
            slot_src.append(src[i])

    pkt_slot = [(slot_of[f] if f is not None else -1) for f in fc]
    counts = [0] * len(slot_fc)
    for s in pkt_slot:
        if s >= 0:
            counts[s] += 1

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w") as w:
        w.write("# dirgin-room-binding v1\n")
        w.write("chunk %s\n" % chunk)
        w.write("slots %d\n" % len(slot_fc))
        for s in range(len(slot_fc)):
            w.write("slot %d %s %d %s\n" % (s, slot_fc[s], counts[s], slot_src[s]))
        w.write("packets %d\n" % len(pkt_slot))
        for s in pkt_slot:
            w.write("%d\n" % s)

    matched = sum(counts)
    print("wrote %s: chunk=%s slots=%d packets=%d matched=%d unbound=%d"
          % (a.out, chunk, len(slot_fc), len(pkt_slot), matched, len(pkt_slot) - matched))


if __name__ == "__main__":
    main()
