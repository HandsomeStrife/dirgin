#!/usr/bin/env python3
"""export_bmd_mesh.py -- dump a BMD *environment* (static world geometry) to the
native GPU renderer's `DMSH` triangle-soup format, the same binary the character
pipeline (render_skinned.py -> char_e000.mesh) emits, so the scene renderer in
native/gpu can load an environment and a character into one GL world.

Pipeline (all from OUR OWN trusted decoders -- no external untrusted parsing in
Python):
  1. Run the native `doc_model` loader (C, bounds-checked RFD/BMD parser) on the
     chosen RFD container to emit a Wavefront OBJ of the world geometry.
  2. Parse that OBJ (positions + triangle faces) -- a trusted intermediate we
     just produced, not the raw game file.
  3. Reconstruct per-FACE flat normals and write an unindexed `DMSH` triangle
     soup: pos3, normal3, uv2, rgba4 per vertex.

HONEST NOTES on the data:
  * BMD world geometry is PRELIT on PS2 -- its look comes from BAKED per-vertex
    colours, NOT runtime lighting. Those baked colours (and the UVs) live in a
    separate VIF stream the native loader does not yet resolve (uv range reads
    0..0; doc_model.h / FORMAT.md). So we have NO authentic vertex colours.
  * To keep the PRELIT path meaningful AND make the room read as 3D geometry
    rather than a flat silhouette, we BAKE a neutral directional grey shade into
    the PRELIT vertex colour from the reconstructed face normal:
        shade = ambient + (1-ambient) * |N . L|      (two-sided, |.| so winding
                                                      sign is irrelevant)
    This is a DOCUMENTED STAND-IN for the unrecovered baked vertex colours, not
    captured data. The face normal itself is geometric (cross product), also
    reconstructed. Everything here is derived from the real decoded positions;
    only the grey baked shade is a provisional placeholder.

Usage:
    python3 -I export_bmd_mesh.py <container.bin> [out_prefix]
    (defaults to the 06960212 gallery room -> out/bmd_06960212.mesh)
"""
import os
import sys
import math
import struct
import array
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
DOC_MODEL = os.path.join(HERE, "doc_model")

MESH_MAGIC = b"DMSH"
MESH_VERSION = 1

# Baked-shade stand-in parameters (see module docstring).
BAKE_LIGHT = (0.55, 0.50, 0.68)   # key with a strong horizontal component so
                                  # walls read distinctly from floors/ceilings
BAKE_AMBIENT = 0.30               # floor so back/unlit faces stay visible


def _norm(v):
    l = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    if l < 1e-20:
        return (0.0, 0.0, 0.0)
    return (v[0] / l, v[1] / l, v[2] / l)


def parse_obj(path):
    """Parse positions + triangle faces from a (trusted) OBJ. Returns
    (positions[list of (x,y,z)], faces[list of (i0,i1,i2)] 0-based)."""
    pos = []
    faces = []
    with open(path, "r") as f:
        for line in f:
            if line.startswith("v "):
                p = line.split()
                pos.append((float(p[1]), float(p[2]), float(p[3])))
            elif line.startswith("f "):
                idx = []
                for tok in line.split()[1:]:
                    # "a/b" or "a/b/c" or "a" -> position index (1-based)
                    idx.append(int(tok.split("/")[0]))
                # fan-triangulate just in case (doc_model emits triangles)
                for k in range(1, len(idx) - 1):
                    faces.append((idx[0] - 1, idx[k] - 1, idx[k + 1] - 1))
    return pos, faces


def export(container, out_prefix):
    obj_path = out_prefix + ".obj"
    # (1) decode with the native loader (always re-run so the OBJ is fresh)
    rc = subprocess.run([DOC_MODEL, container, out_prefix],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if rc.returncode != 0 or not os.path.exists(obj_path):
        sys.stderr.write(rc.stdout.decode("utf-8", "replace"))
        raise SystemExit("doc_model failed for %s" % container)

    # (2) parse the trusted intermediate
    pos, faces = parse_obj(obj_path)
    if not faces:
        raise SystemExit("no faces parsed from %s" % obj_path)
    npos = len(pos)

    L = _norm(BAKE_LIGHT)
    amb = BAKE_AMBIENT

    # (3) build unindexed triangle soup with per-face flat normal + baked shade
    flat = []
    bbmin = [1e30, 1e30, 1e30]
    bbmax = [-1e30, -1e30, -1e30]
    ntri = 0
    for (i0, i1, i2) in faces:
        if not (0 <= i0 < npos and 0 <= i1 < npos and 0 <= i2 < npos):
            continue  # bounds-check the (trusted but still validated) indices
        a, b, c = pos[i0], pos[i1], pos[i2]
        e1 = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        e2 = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        n = (e1[1] * e2[2] - e1[2] * e2[1],
             e1[2] * e2[0] - e1[0] * e2[2],
             e1[0] * e2[1] - e1[1] * e2[0])
        n = _norm(n)
        if n == (0.0, 0.0, 0.0):
            continue  # degenerate triangle
        ndl = abs(n[0] * L[0] + n[1] * L[1] + n[2] * L[2])   # two-sided
        shade = amb + (1.0 - amb) * ndl
        if shade > 1.0:
            shade = 1.0
        for p in (a, b, c):
            flat.extend((p[0], p[1], p[2],
                         n[0], n[1], n[2],
                         0.0, 0.0,                      # no resolved UVs
                         shade, shade, shade, 1.0))     # baked PRELIT grey
            for k in range(3):
                if p[k] < bbmin[k]:
                    bbmin[k] = p[k]
                if p[k] > bbmax[k]:
                    bbmax[k] = p[k]
        ntri += 1

    vcount = ntri * 3
    mesh_path = out_prefix + ".mesh"
    header = struct.pack("<4sII", MESH_MAGIC, MESH_VERSION, vcount) + \
        struct.pack("<6f", bbmin[0], bbmin[1], bbmin[2],
                    bbmax[0], bbmax[1], bbmax[2])
    with open(mesh_path, "wb") as f:
        f.write(header)
        array.array("f", flat).tofile(f)
    print("wrote %s (%d verts / %d tris)" % (mesh_path, vcount, ntri))
    print("bbox X[%.2f,%.2f] Y[%.2f,%.2f] Z[%.2f,%.2f]" %
          (bbmin[0], bbmax[0], bbmin[1], bbmax[1], bbmin[2], bbmax[2]))
    return mesh_path


def main():
    container = (sys.argv[1] if len(sys.argv) > 1
                 else "/srv/games/dirge-port/extract/_KEL/noPath/06960212.bin")
    base = os.path.splitext(os.path.basename(container))[0]
    out_prefix = (sys.argv[2] if len(sys.argv) > 2
                  else os.path.join(OUT, "bmd_" + base))
    os.makedirs(OUT, exist_ok=True)
    export(container, out_prefix)


if __name__ == "__main__":
    main()
