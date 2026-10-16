#!/usr/bin/env python3
"""export_bmd_bound.py -- emit textured room meshes using the REAL per-packet
texture binding recovered from the GS-dump UV-triple ground truth (2026-10-16),
NOT the old vertex-count-bucket placeholder of export_bmd_textured.py.

Input binding: out/z143_pktfc/pf_<chunk>.json -- a list (one entry per decoded
packet, in decode_packets() order) of the matched FileCode string or null. These
come from the stable UV-triple identity match (tools: scratchpad/uvbatch.py) and
are validated against out/z143_mako_dump_binding_oracle.json (TBP0->FileCode at
corr~1.0, US dump == EU assets).

Output: one DMSH mesh per bound FileCode (packets of the same texture merged) +
a manifest + a .list sidecar the C render driver reads. Packets with a null
binding (dump culling left them unmatched this frame) are emitted as a single
'unbound' group with no texture -- honest, not guessed.

    python3 -I export_bmd_bound.py <container.bin> <pf_binding.json> [out_prefix]

Trusted repo data + bounds-checked raw BMD read (via export_bmd_textured), run -I.
"""
import os, sys, json, struct, array, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_bmd_textured import (find_bmd_payload, decode_packets, packet_tris,
                                 _norm, OUT, EXTRACT, DTEX, MESH_MAGIC, MESH_VERSION)


def emit_group(flat, ntri, out_prefix, gi):
    bbmin = [1e30]*3; bbmax = [-1e30]*3
    # recompute bbox from flat (stride 12: pos3,nrm3,uv2,rgba4)
    for v in range(0, len(flat), 12):
        for k in range(3):
            p = flat[v+k]
            if p < bbmin[k]: bbmin[k] = p
            if p > bbmax[k]: bbmax[k] = p
    mesh_path = "%s_g%02d.mesh" % (out_prefix, gi)
    vcount = ntri * 3
    header = struct.pack("<4sII", MESH_MAGIC, MESH_VERSION, vcount) + \
        struct.pack("<6f", bbmin[0], bbmin[1], bbmin[2], bbmax[0], bbmax[1], bbmax[2])
    with open(mesh_path, "wb") as f:
        f.write(header); array.array("f", flat).tofile(f)
    return os.path.basename(mesh_path), bbmin, bbmax


def build_flat(packets, idxs):
    """Flatten the given packet indices into interleaved vertex floats + tri count."""
    flat = []; ntri = 0
    for pi in idxs:
        pk = packets[pi]
        V = pk["verts"]; UV = pk["uv"]; C = pk["col"]
        for (i0, i1, i2) in packet_tris(pk):
            a, b, c = V[i0], V[i1], V[i2]
            e1 = (b[0]-a[0], b[1]-a[1], b[2]-a[2]); e2 = (c[0]-a[0], c[1]-a[1], c[2]-a[2])
            n = _norm((e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2],
                       e1[0]*e2[1]-e1[1]*e2[0]))
            if n == (0.0, 0.0, 0.0):
                continue
            for idx in (i0, i1, i2):
                p3 = V[idx]; uv = UV[idx]; rgba = C[idx]
                flat.extend((p3[0], p3[1], p3[2], n[0], n[1], n[2], uv[0], uv[1],
                             rgba[0]/255.0, rgba[1]/255.0, rgba[2]/255.0, rgba[3]/255.0))
            ntri += 1
    return flat, ntri


def tex_png_for(fc):
    """Best (largest) decoded PNG for a FileCode, decoding its TXD if needed."""
    cont = os.path.join(EXTRACT, fc + ".bin")
    tdir = os.path.join(OUT, "tex_bound")
    os.makedirs(tdir, exist_ok=True)
    pngs = sorted(glob.glob(os.path.join(tdir, fc + "_*.png")))
    if not pngs and os.path.exists(cont):
        try:
            import subprocess
            subprocess.run([DTEX, cont, "-o", tdir], capture_output=True, timeout=60)
            pngs = sorted(glob.glob(os.path.join(tdir, fc + "_*.png")))
        except Exception:
            pass
    if not pngs:
        return None, cont
    try:
        from PIL import Image
        pngs.sort(key=lambda p: -(lambda im: im.size[0]*im.size[1])(Image.open(p)))
    except Exception:
        pass
    return pngs[0], cont


def export(container, pf_path, out_prefix):
    buf = open(container, "rb").read()
    pay = find_bmd_payload(buf)
    if pay is None:
        raise SystemExit("no BMD payload in %s" % container)
    packets = decode_packets(buf, pay)
    pf = json.load(open(pf_path))
    if len(pf) != len(packets):
        print("WARNING: binding length %d != packet count %d (order must match "
              "decode_packets)" % (len(pf), len(packets)), file=sys.stderr)
    n = min(len(pf), len(packets))

    # group packet indices by bound FileCode (None -> 'unbound')
    from collections import defaultdict, Counter
    groups = defaultdict(list)
    for i in range(n):
        fc = pf[i]
        groups[fc if fc else "unbound"].append(i)
    # bound FileCodes first, by tri weight; 'unbound' last
    order = sorted([k for k in groups if k != "unbound"],
                   key=lambda k: -len(groups[k]))
    if "unbound" in groups:
        order.append("unbound")

    os.makedirs(OUT, exist_ok=True)
    manifest = {"container": os.path.basename(container),
                "binding": os.path.basename(pf_path),
                "method": "UV-triple GS-dump ground truth (validated, 2026-10-16)",
                "groups": []}
    total_tris = 0; bound_tris = 0
    for gi, fc in enumerate(order):
        flat, ntri = build_flat(packets, groups[fc])
        if ntri == 0:
            continue
        mesh_name, bbmin, bbmax = emit_group(flat, ntri, out_prefix, gi)
        if fc == "unbound":
            png, cont = None, None
        else:
            png, cont = tex_png_for(fc)
            bound_tris += ntri
        manifest["groups"].append({
            "mesh": mesh_name, "filecode": (None if fc == "unbound" else fc),
            "packets": len(groups[fc]), "tris": ntri,
            "texture": png, "texture_container": cont,
            "abe": any(packets[i]["abe"] for i in groups[fc]),
        })
        total_tris += ntri
    man_path = out_prefix + "_manifest.json"
    json.dump(manifest, open(man_path, "w"), indent=2)
    with open(out_prefix + ".list", "w") as f:
        for g in manifest["groups"]:
            f.write("%s\t%s\t%d\n" % (g["mesh"], g["texture_container"] or "NONE",
                                      1 if g["abe"] else 0))
    nb = sum(1 for g in manifest["groups"] if g["filecode"])
    print("%s: %d bound groups (+%s unbound), %d/%d tris textured, -> %s"
          % (os.path.basename(container), nb,
             "1" if "unbound" in groups else "0", bound_tris, total_tris,
             os.path.basename(man_path)))
    return man_path


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: export_bmd_bound.py <container.bin> <pf_binding.json> [out_prefix]")
    container, pf_path = sys.argv[1], sys.argv[2]
    base = os.path.splitext(os.path.basename(container))[0]
    out_prefix = sys.argv[3] if len(sys.argv) > 3 else os.path.join(OUT, "bound_" + base)
    os.makedirs(OUT, exist_ok=True)
    export(container, pf_path, out_prefix)


if __name__ == "__main__":
    main()
