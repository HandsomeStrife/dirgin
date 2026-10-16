#!/usr/bin/env python3
"""export_bmd_textured.py -- decode a BMD *environment* (static world geometry)
WITH its real per-vertex UVs, baked PRELIT vertex colours, and material grouping,
then bind the room's TXD textures and emit loadable DMSH meshes (one per material
group) + a manifest, so the native GPU renderer draws a TEXTURED room.

This supersedes export_bmd_mesh.py, which recovered only the V4-32 float POSITION
runs and baked a grey directional stand-in because "the UVs and baked colours live
in a separate VIF stream the native loader does not yet resolve". That stream is
now RESOLVED. Reversed from the game's own draw path (FORMAT.md Section 3 + the
new Section 3.4 below), traced against the real room 06960212:

  THE ATTRIBUTED (textured) BMD mesh is a VIF DMA chain of per-submesh draw
  packets. Each packet is:
      STCYCL cl=4 wl=4
      UNPACK V4-32 num=2 @addr0     -- GIFtag + PRIM/TEX0 template (2 qwords).
                                       Its PRIM type = 5 (tri), TME=1 (textured);
                                       giftag qword1 word0 = 0x8000 | matSel  --
                                       the per-submesh MATERIAL/texture selector
                                       (16 distinct values in 06960212).
      STCYCL cl=4 wl=1 ; STROW b0 b1 b2 0 ; STMOD 2 (difference/accumulate)
      UNPACK V3-16 num=N @addr2      -- POSITION, int16, DELTA-encoded:
                                       out[k] = out[k-1] + raw[k], out[-1]=STROW;
                                       world = out / 16  (ITOF4, same /16 as the
                                       V4-32 float runs -- verified: this stream
                                       reconstructs the IDENTICAL room bbox).
      STMOD 0 ; UNPACK V4-8  num=N @addr3  -- RGBA baked PRELIT vertex colour
                                             (alpha 0x80 = GS-neutral).
      UNPACK V2-16 num=N @addr4     -- UV, int16; texcoord = raw / 4096 (ITOF12).
      MSCNT
  Position/colour/uv are ALL num=N and 1:1 per vertex (363/363 packets lock in
  06960212, 0 mismatches, 7491 verts). Connectivity = triangle STRIP per packet
  (same winding model the OBJ/scanner path already uses), which renders coherent.

  TEXTURE BINDING — CORRECTED 2026-10-16 (was wrong; kept here as a warning).
  EARLIER BELIEF: "the per-submesh material selector is real and on-disk (giftag)",
  so grouping by `sel` gave material groups and we only had to pick the atlas.
  PROVEN FALSE: the giftag field read as `sel` is the per-submesh VERTEX COUNT
  (`qw1.x & 0x7fff == vertex num` for all 1273 packets of z128 06800212; see the
  inline note in decode_packets and FORMAT.md 3.4). So the `sel`-keyed groups this
  script emits are VERTEX-COUNT BUCKETS, not materials. There is NO material/texture
  selector in the VIF draw chain at all — the real binding is the runtime group
  table (model+0x60, material selector at group+8) resolved through the TXD handle
  table (context+0x24), FORMAT.md Section 6.
  GROUND TRUTH: the GS dump of z128/Kalm (tools/gsdump/) shows this script's
  dominant-order binding gets 0 of 9 drawn room atlases right across all 13 Kalm
  chunks (out/z128_dump03_binding_oracle.json). UVs, baked colours and the per-packet
  geometry ARE exact decoded data; the grouping and the atlas assignment are both
  still open (recovery path: parse the runtime group table, or use the GS-dump
  oracle per room). This script's `.list`/manifest texture column is therefore a
  PLACEHOLDER, not a correct binding — do not trust it for final rendering.

Usage:
    python3 -I export_bmd_textured.py [container.bin] [out_prefix]
    (defaults to 06960212 -> out/bmd_06960212_tex.*)

Trusted inputs only: reads the raw BMD bytes directly (bounds-checked, python3 -I)
and shells out to the trusted `dtex` decoder for textures.
"""
import os, sys, struct, glob, subprocess, math, array, json

HERE = os.path.dirname(os.path.abspath(__file__))
OUT  = os.path.join(HERE, "out")
DTEX = "/srv/games/dirge-port/native/assets/textures/dtex"
EXTRACT = "/srv/games/dirge-port/extract/_KEL/noPath"

MESH_MAGIC = b"DMSH"
MESH_VERSION = 1
POS_SCALE = 1.0 / 16.0     # ITOF4, same as the V4-32 float runs
UV_SCALE  = 1.0 / 4096.0   # ITOF12

VL = {0: 32, 1: 16, 2: 8, 3: 5}


def _rd(buf):
    N = len(buf)
    def u16(o): return struct.unpack_from('<H', buf, o)[0]
    def u32(o): return struct.unpack_from('<I', buf, o)[0]
    def i32(o): return struct.unpack_from('<i', buf, o)[0]
    def s16(o): return struct.unpack_from('<h', buf, o)[0]
    def u64(o): return struct.unpack_from('<Q', buf, o)[0]
    return N, u16, u32, i32, s16, u64


def find_bmd_payload(buf):
    """RFD unwrap (FORMAT.md Section 1) -> BMD payload offset, or None."""
    N, u16, u32, i32, s16, u64 = _rd(buf)
    if N < 0x18:
        return None
    cnt = u16(0x10)
    if cnt == 0 or cnt > 1024:
        return None
    for i in range(cnt):
        if 0x14 + 4 * i + 4 > N:
            break
        eoff = u32(0x14 + 4 * i)
        if eoff + 0x10 > N:
            continue
        if buf[eoff + 8:eoff + 11] == b'bmd' and buf[eoff + 0x10:eoff + 0x13] == b'BMD':
            return eoff + 0x10
    return None


def decode_packets(buf, pay):
    """Walk the VIF draw chain from the BMD payload; return a list of packets:
    {verts:[(x,y,z)], uv:[(u,v)], col:[(r,g,b,a)], sel:int, prim:int, abe:bool}."""
    N, u16, u32, i32, s16, u64 = _rd(buf)

    def rd_unpack(p):
        c = u32(p); num = (c >> 16) & 0xff; cmd = (c >> 24) & 0x7f
        if not (0x60 <= cmd <= 0x7f):
            return None
        vn = (cmd >> 2) & 3; vl = cmd & 3; bits = VL[vl]; nc = vn + 1
        addr = (c & 0xffff) & 0x3ff
        nbytes = ((nc * bits + 7) // 8) * num
        return (nc, bits, num, addr, p + 4, nbytes)

    # start at the first STCYCL cl4/wl4 (0x01000404)
    start = pay + 8
    o = pay + 8
    while o + 4 <= N:
        if u32(o) == 0x01000404:
            start = o; break
        o += 4

    packets = []
    p = start
    while p + 4 <= N:
        c = u32(p); cmd = (c >> 24) & 0x7f; imm = c & 0xffff
        if not (cmd == 0x01 and imm == 0x0404):
            p += 4; continue
        pp = p + 4
        sel = 0; prim = 0; abe = False; is_list = False
        pos = col = uv = None
        row = [0, 0, 0, 0]; mode = 0
        # Per-submesh header: UNPACK V4-32 num=2 @0 -> two control qwords.
        # qword0 is the GIF A+D / material-setup template (its PRIM field is the
        # TEX0/register-setup packet, NOT the geometry primitive). qword1 carries
        # the per-submesh draw parameters the VU1 reads (program_A.txt idx 33-43):
        #   qw1.x & 0x7fff  = VERTEX COUNT     (ILW.x vi08 ; IAND vi08,vi04=0x7fff)
        #   qw1.y & 0x8000  = CONNECTIVITY FLAG(ILW.y vi09 ; IAND vi09,vi05=0x8000;
        #                     IBEQ vi09 -> instr174): SET => the vertices are a
        #                     TRIANGLE LIST (3-vertex loop); CLEAR => a TRIANGLE
        #                     STRIP (4-vertex loop).  Verified: every flagged
        #                     submesh in 06960212 has (count % 3 == 0), the
        #                     necessary condition for a list.
        # NOTE: the old `sel = qw1.x & 0xff` is the low byte of that vertex COUNT,
        # not a material id (num == qw1.x & 0x7fff for all packets) -- the on-disk
        # material/texture handle is runtime-only (FORMAT.md Section 6).  It is kept
        # below only as a grouping key; the real decoded data are pos/uv/colour.
        u = rd_unpack(pp)
        if u and u[0] == 4 and u[1] == 32 and u[3] == 0:
            gt = u[4]
            if gt + 24 <= N:
                lo = u64(gt)
                prim = (lo >> 47) & 0x7ff
                abe = bool(prim & 0x40)
                sel = u32(gt + 16) & 0xff          # (low byte of the vertex count)
                is_list = bool(u32(gt + 20) & 0x8000)
            pp = u[4] + u[5]
        guard = 0
        while pp + 4 <= N and guard < 48:
            guard += 1
            cc = u32(pp); cm = (cc >> 24) & 0x7f; im = cc & 0xffff
            if cm == 0x01: pp += 4; continue            # STCYCL
            if cm == 0x05: mode = im; pp += 4; continue  # STMOD
            if cm == 0x20: pp += 8; continue             # STMASK
            if cm == 0x30:                               # STROW (+4 data words)
                row = [i32(pp + 4), i32(pp + 8), i32(pp + 12), i32(pp + 16)]
                pp += 20; continue
            if cm == 0x17: pp += 4; break                # MSCNT -> packet end
            u = rd_unpack(pp)
            if not u:
                pp += 4; continue
            nc, bits, num, addr, doff, nb = u
            if doff + nb > N:
                break
            if addr == 2 and nc == 3 and bits == 16:     # POSITION (delta int16)
                P = []; acc = [row[0], row[1], row[2]]
                for k in range(num):
                    r0 = s16(doff + 6 * k); r1 = s16(doff + 6 * k + 2); r2 = s16(doff + 6 * k + 4)
                    if mode == 2:                        # difference/accumulate
                        acc = [acc[0] + r0, acc[1] + r1, acc[2] + r2]
                        P.append((acc[0] * POS_SCALE, acc[1] * POS_SCALE, acc[2] * POS_SCALE))
                    elif mode == 1:                      # offset (add ROW each)
                        P.append(((r0 + row[0]) * POS_SCALE, (r1 + row[1]) * POS_SCALE, (r2 + row[2]) * POS_SCALE))
                    else:
                        P.append((r0 * POS_SCALE, r1 * POS_SCALE, r2 * POS_SCALE))
                pos = P
            elif addr == 3 and nc == 4 and bits == 8:    # RGBA baked colour
                col = [(buf[doff + 4 * k], buf[doff + 4 * k + 1],
                        buf[doff + 4 * k + 2], buf[doff + 4 * k + 3]) for k in range(num)]
            elif addr == 4 and nc == 2 and bits == 16:   # UV
                uv = [(s16(doff + 4 * k) * UV_SCALE, s16(doff + 4 * k + 2) * UV_SCALE)
                      for k in range(num)]
            pp += 4 + nb
            if pp & 3:
                pp = (pp + 3) & ~3
        if pos and col and uv and len(pos) == len(col) == len(uv) and len(pos) >= 3:
            packets.append(dict(verts=pos, uv=uv, col=col, sel=sel, prim=prim,
                                abe=abe, is_list=is_list))
        p = pp
    return packets


def _norm(v):
    l = math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2])
    return (0.0, 0.0, 0.0) if l < 1e-20 else (v[0]/l, v[1]/l, v[2]/l)


def strip_tris(n):
    """Continuous triangle-strip index triples with alternating winding. KEPT for
    reference only -- the real STRIP primitive is a QUAD GROUP (see quad_tris /
    packet_tris); a continuous strip over quad-group vertices bridges the separate
    quads together, which is the room 'black gash' artefact."""
    out = []
    for k in range(n - 2):
        if k & 1:
            out.append((k + 1, k, k + 2))
        else:
            out.append((k, k + 1, k + 2))
    return out


def list_tris(n):
    """Triangle-LIST index triples: each consecutive group of three vertices is
    one independent triangle (no shared/bridging edges)."""
    return [(3 * t, 3 * t + 1, 3 * t + 2) for t in range(n // 3)]


def quad_tris(n):
    """QUAD-GROUP index triples: every 4 consecutive vertices form one planar quad
    (two triangles in strip winding), the primitive RESTARTING every 4 vertices --
    NO bridging triangle across a quad boundary. This is what the VU1 does for a
    STRIP submesh: its inner loop is a 4-vertex loop ('STRIP 4-loop', program_A.txt
    idx 43), not a continuous strip. Evidence: across rooms 06960212/068f0612/
    06960112 EVERY strip-packet vertex count is a multiple of 4, 97-99% of each
    group-of-4 is coplanar (a real wall/floor panel), and the spurious long
    'black gash' spanning triangles are exactly the ones that straddle a group-of-4
    boundary -- which this restart removes (room 06960212: 31 gash tris -> 8, the
    remaining 8 being legitimately large single quads)."""
    out = []
    for g in range(n // 4):
        b = 4 * g
        out.append((b, b + 1, b + 2))
        out.append((b + 2, b + 1, b + 3))
    # tolerate a non-multiple-of-4 tail (not seen in practice) as a short strip
    rem = n - 4 * (n // 4)
    if rem == 3:
        b = 4 * (n // 4)
        out.append((b, b + 1, b + 2))
    return out


def packet_tris(pk):
    """Connectivity for one submesh, selected by the on-disk header flag the VU1
    branches on (qw1.y & 0x8000, program_A.txt idx 43):
      * flag SET  -> TRIANGLE LIST  (count % 3 == 0; groups of 3).
      * flag CLEAR-> QUAD GROUP      (count % 4 == 0; groups of 4, VU 'strip 4-loop').
    Decoding a STRIP submesh as a *continuous* strip (the previous behaviour)
    bridged its independent quads together with long spanning triangles -- the room
    'black gash' / 'slab/needle' artefact. The quad-group restart removes exactly
    those bridges while keeping every real panel."""
    return list_tris(len(pk["verts"])) if pk.get("is_list") else quad_tris(len(pk["verts"]))


def decode_room_textures(container):
    """Decode every sibling <levelcode>ffNN.bin TXD with libdtex. Returns a list
    of dicts {png, name, w, h, px} sorted by pixel area (largest first)."""
    base = os.path.basename(container)
    stem = base[:-4] if base.endswith(".bin") else base
    level = stem[:4]                                   # e.g. "0696"
    tdir = os.path.join(OUT, "tex_" + stem)
    os.makedirs(tdir, exist_ok=True)
    # clear old
    for f in glob.glob(os.path.join(tdir, "*.png")):
        try: os.remove(f)
        except OSError: pass
    cands = sorted(glob.glob(os.path.join(EXTRACT, level + "ff*.bin")))
    for c in cands:
        try:
            subprocess.run([DTEX, c, "-o", tdir], capture_output=True, timeout=60)
        except Exception:
            pass
    try:
        from PIL import Image
    except ImportError:
        Image = None
    out = []
    for png in sorted(glob.glob(os.path.join(tdir, "*.png"))):
        w = h = 0
        if Image is not None:
            try:
                im = Image.open(png); w, h = im.size
            except Exception:
                continue
        name = os.path.basename(png)
        fc = name.split("_", 1)[0]                     # 8-hex FileCode prefix
        container_bin = os.path.join(EXTRACT, fc + ".bin")
        out.append(dict(png=png, name=name, w=w, h=h, px=w * h,
                        container=container_bin))
    out.sort(key=lambda d: -d["px"])
    return out, tdir


def export(container, out_prefix):
    buf = open(container, "rb").read()
    pay = find_bmd_payload(buf)
    if pay is None:
        raise SystemExit("no BMD payload in %s" % container)
    packets = decode_packets(buf, pay)
    if not packets:
        raise SystemExit("no attributed draw packets decoded from %s" % container)

    # group by material selector
    sels = sorted({pk["sel"] for pk in packets})
    print("decoded %d attributed packets, %d distinct material selectors: %s"
          % (len(packets), len(sels), [hex(s) for s in sels]))

    # resolve textures (best-effort, documented)
    textures, tdir = decode_room_textures(container)
    print("decoded %d room textures into %s" % (len(textures), tdir))

    # Assign room atlases to material groups deterministically (largest first,
    # most-used selector first). This is the documented 'dominant textures' bind;
    # the exact selector->FileCode slot is the runtime handle table (Section 6).
    from collections import Counter
    use = Counter(pk["sel"] for pk in packets)
    sel_order = [s for s, _ in use.most_common()]      # most-used first
    sel_tex = {}
    for i, s in enumerate(sel_order):
        sel_tex[s] = textures[i % len(textures)] if textures else None

    # emit one DMSH mesh per material group + a manifest
    os.makedirs(OUT, exist_ok=True)
    manifest = {"container": os.path.basename(container), "groups": [],
                "note": ("UVs + per-vertex baked colours + the material grouping "
                         "are exact decoded data; texture<->group binding is the "
                         "documented dominant-order approximation (Section 6 handle "
                         "table is runtime-only).")}
    gbb = [1e30]*3 + [-1e30]*3
    total_tris = 0
    for gi, sel in enumerate(sel_order):
        flat = []
        bbmin = [1e30, 1e30, 1e30]; bbmax = [-1e30, -1e30, -1e30]
        ntri = 0
        for pk in packets:
            if pk["sel"] != sel:
                continue
            V = pk["verts"]; UV = pk["uv"]; C = pk["col"]
            for (i0, i1, i2) in packet_tris(pk):
                a, b, c = V[i0], V[i1], V[i2]
                e1 = (b[0]-a[0], b[1]-a[1], b[2]-a[2])
                e2 = (c[0]-a[0], c[1]-a[1], c[2]-a[2])
                n = _norm((e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2],
                           e1[0]*e2[1]-e1[1]*e2[0]))
                if n == (0.0, 0.0, 0.0):
                    continue                        # degenerate
                for idx in (i0, i1, i2):
                    p3 = V[idx]; uv = UV[idx]; rgba = C[idx]
                    flat.extend((p3[0], p3[1], p3[2],
                                 n[0], n[1], n[2],
                                 uv[0], uv[1],
                                 rgba[0]/255.0, rgba[1]/255.0,
                                 rgba[2]/255.0, rgba[3]/255.0))
                    for k in range(3):
                        if p3[k] < bbmin[k]: bbmin[k] = p3[k]
                        if p3[k] > bbmax[k]: bbmax[k] = p3[k]
                        if p3[k] < gbb[k]:   gbb[k] = p3[k]
                        if p3[k] > gbb[3+k]: gbb[3+k] = p3[k]
                ntri += 1
        if ntri == 0:
            continue
        mesh_path = "%s_g%02d.mesh" % (out_prefix, gi)
        vcount = ntri * 3
        header = struct.pack("<4sII", MESH_MAGIC, MESH_VERSION, vcount) + \
            struct.pack("<6f", bbmin[0], bbmin[1], bbmin[2], bbmax[0], bbmax[1], bbmax[2])
        with open(mesh_path, "wb") as f:
            f.write(header); array.array("f", flat).tofile(f)
        tex = sel_tex.get(sel)
        manifest["groups"].append({
            "mesh": os.path.basename(mesh_path),
            "sel": sel, "tris": ntri,
            "texture": (tex["png"] if tex else None),
            "texture_name": (tex["name"] if tex else None),
            "texture_container": (tex["container"] if tex else None),
            "abe": any(pk["abe"] for pk in packets if pk["sel"] == sel),
        })
        total_tris += ntri
    man_path = out_prefix + "_manifest.json"
    with open(man_path, "w") as f:
        json.dump(manifest, f, indent=2)
    # simple tab-separated sidecar the C render driver reads:
    #   <mesh.mesh>\t<texture.png-or-NONE>\t<abe 0/1>
    list_path = out_prefix + ".list"
    with open(list_path, "w") as f:
        for g in manifest["groups"]:
            f.write("%s\t%s\t%d\n" % (g["mesh"], g["texture_container"] or "NONE",
                                      1 if g["abe"] else 0))
    print("wrote %d group meshes (%d tris total) + %s"
          % (len(manifest["groups"]), total_tris, os.path.basename(man_path)))
    print("room bbox X[%.1f,%.1f] Y[%.1f,%.1f] Z[%.1f,%.1f]"
          % (gbb[0], gbb[3], gbb[1], gbb[4], gbb[2], gbb[5]))
    return man_path


def main():
    container = sys.argv[1] if len(sys.argv) > 1 else os.path.join(EXTRACT, "06960212.bin")
    base = os.path.splitext(os.path.basename(container))[0]
    out_prefix = sys.argv[2] if len(sys.argv) > 2 else os.path.join(OUT, "bmd_" + base + "_tex")
    os.makedirs(OUT, exist_ok=True)
    export(container, out_prefix)


if __name__ == "__main__":
    main()
