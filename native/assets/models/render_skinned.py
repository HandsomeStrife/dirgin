#!/usr/bin/env python3
"""render_skinned.py -- decode & render the Dirge of Cerberus (PS2) skinned
character MESH (smd / "SM2") from an event container, with CORRECT triangle
connectivity (the S-8 draw-index stream), per-vertex UVs, and the real texture.

This supersedes the earlier "naive file-order strip" decode (which left ~1/3
shards + dropped ~40% of batches). What changed, all traced to the game's own
parsers (analysis/decomp/ + port/vu/disasm/) -- see FORMAT.md Section 8.3/8.4:

  1. CONTROL TABLE is located EXACTLY, not guessed. Each group's control
     records live at  piVar2 = smd_payload + *(u32)(group+0x0c);  the records
     start at  piVar2 + piVar2[2]  and the raw VIF batch bytes at
     piVar2 + piVar2[3] (= group+0x30). Record = 8B
     { s16 bone, u8 cntA, u8 cntB, s16 qwc, s16 stride }. This locks 100% of
     batches (was ~60%). (FUN_005b1260 reads param_1[3]=group+0x0c as piVar2.)

  2. The POSED VERTEX POOL is scatter-indexed, NOT batch-sequential. The VU0
     skin program (port/vu/disasm/vu0_skin_micro.txt, entry 0x120) writes each
     transformed vertex to pool slot  (normalW + 0x80) & 0xff, where normalW is
     the 4th byte of that vertex's V4-8 "normal" plane (`ILW.w vi03,0x3(vi01)`).
     Weighted/boundary verts from adjacent bone batches collapse onto the same
     slot. For char e000 this yields a 245-slot pool per group (not 404 records).

  3. CONNECTIVITY + UV come from the +0x08 DRAW packets (STCYCL cl4/wl4). The
     group+0x08 field points at a self-terminating array of draw DESCRIPTORS
     ({u16 n, 0xffff, u32, 0,0, u32 vifPtr}); each descriptor's VIF packets are a
     run of sub-packets, each: V4-32 GIFtag template @0, V4-8 colour @3,
     V2-16 UV @2, V1-8 (S-8) POOL INDEX @2, MSCNT. The 24 S-8 indices per
     sub-packet form a GS triangle STRIP into the posed pool (degenerate =
     repeated index = restart). UV = V2-16 / 4096  (-> [0,1], verified).

  4. TEXTURE: the event container ships only texanm.tad (the "_eye" blink anim);
     the body atlas lives in the sibling texture container 0765xxx2.bin (RST,
     512x256 PSMT8), decoded by native/assets/textures/libdtex. The smd UVs
     index straight into it.

All geometry/UVs/texture are REAL file data. Deps: numpy + PIL (host tools).
Usage: python3 -I render_skinned.py [container.bin] [texture.png]
"""
import struct, math, sys, os, subprocess, glob, tempfile
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from skd_skeleton import load_skeleton

CONT = sys.argv[1] if len(sys.argv) > 1 else \
    "/srv/games/dirge-port/extract/_KEL/noPath/07650001.bin"
OUT  = "/srv/games/dirge-port/native/renderer/out"
DTEX = "/srv/games/dirge-port/native/assets/textures/dtex"

SM2_MAGIC = 0x00324d53


def load_container(path):
    buf = open(path, "rb").read()
    return buf, len(buf)


def _find_body_texture(cont_path, explicit=None):
    """Decode the sibling texture container (…2.bin) with libdtex and return the
    largest decoded atlas as an RGB numpy array, or None. The character's body
    atlas is NOT in the event container; it is the RST 512x256 in …2.bin."""
    if explicit and os.path.exists(explicit):
        return np.asarray(Image.open(explicit).convert("RGB"), dtype=np.float32)
    base = os.path.basename(cont_path)                 # e.g. 07650001.bin
    stem = base[:-4] if base.endswith(".bin") else base   # -> 07650001
    cands = []
    if len(stem) >= 1:
        cands.append(os.path.join(os.path.dirname(cont_path), stem[:-1] + "2.bin"))
    best = None; best_px = 0
    with tempfile.TemporaryDirectory() as td:
        for c in cands:
            if not os.path.exists(c):
                continue
            try:
                subprocess.run([DTEX, c, "-o", td], capture_output=True, timeout=60)
            except Exception:
                continue
            for png in glob.glob(os.path.join(td, "*.png")):
                try:
                    im = Image.open(png).convert("RGB")
                except Exception:
                    continue
                if im.width * im.height > best_px:
                    best_px = im.width * im.height
                    best = np.asarray(im, dtype=np.float32)
    return best


# ---- smd geometry ----------------------------------------------------------

def _u16(b, o): return struct.unpack_from('<H', b, o)[0]
def _s16(b, o): return struct.unpack_from('<h', b, o)[0]
def _u32(b, o): return struct.unpack_from('<I', b, o)[0]
def _i32(b, o): return struct.unpack_from('<i', b, o)[0]
def _f32(b, o): return struct.unpack_from('<f', b, o)[0]


def _parse_batch(buf, NB, rawstart, qwc, num):
    """Parse one batch's VIF geometry stream; return (pos int16x3, nrm u8x4)."""
    p = rawstart; end = min(rawstart + qwc * 16, NB); pos = nrm = None
    while p < end and p + 4 <= NB:
        code = _u32(buf, p); vn = (code >> 16) & 0xff; cmd = (code >> 24) & 0xff
        if cmd in (0x20, 0x30, 0x31):          # STMASK/STROW/STCOL (+1 data word)
            p += 8
        elif cmd in (0x01, 0x00, 0x04, 0x05, 0x10, 0x11, 0x13, 0x14, 0x15, 0x17):
            p += 4
        elif 0x60 <= cmd <= 0x7f:              # UNPACK
            vl = cmd & 3; bits = {0: 32, 1: 16, 2: 8, 3: 16}[vl]
            nc = ((code >> 26) & 3) + 1; nb = nc * bits // 8 * vn
            if p + 4 + nb > NB:
                break
            if nc == 3 and bits == 16:
                pos = np.frombuffer(buf[p + 4:p + 4 + nb], dtype='<i2').reshape(-1, 3).astype(float)
            elif nc == 4 and bits == 8:
                nrm = np.frombuffer(buf[p + 4:p + 4 + nb], dtype=np.uint8).reshape(-1, 4)
            p += 4 + nb
            if p & 3:
                p = (p + 3) & ~3
        else:
            break
    return pos, nrm


def build_group_pool(buf, NB, Wm, bc, P, G, sc):
    """Skin a group's batches into the scatter-indexed posed vertex pool.
    Returns (pool[N,3], normals[N,3], seen[N]) or None.
    The V4-8 attribute stream carries, per vertex: xyz = the PACKED NORMAL
    (signed int8 /128, then rotated by the bone), w = the scatter-pool slot
    (VU0 0x120 output index). We keep the normal so the renderer can light the
    mesh from the game's own normals instead of a winding heuristic (the S-8/
    strip winding is not globally consistent)."""
    pc_off = _u32(buf, G + 0x0c)
    if not (0 < pc_off < NB) or P + pc_off + 0x10 > NB:
        return None
    piv = P + pc_off
    cnt = _i32(buf, piv); streamoff = _i32(buf, piv + 8); rawoff = _i32(buf, piv + 0xc)
    if not (0 < cnt < 8192) or not (0 < streamoff < NB) or not (0 <= rawoff < NB):
        return None
    cr = piv + streamoff; raw = piv + rawoff
    pool = np.zeros((256, 3)); nrmp = np.zeros((256, 3))
    seen = np.zeros(256, bool); maxslot = -1
    for b in range(cnt):
        o = cr + b * 8
        if o + 8 > NB:
            break
        bone = _s16(buf, o); cA = buf[o + 2]; cB = buf[o + 3]
        qwc = _s16(buf, o + 4); stride = _s16(buf, o + 6); num = cA + cB
        if num == 0:
            raw += max(stride, 0); continue
        if not (0 <= bone < bc) or qwc <= 0 or raw + 2 > NB:
            raw += max(stride, 0); continue
        pos, nrm = _parse_batch(buf, NB, raw, qwc, num); raw += stride
        if pos is None or nrm is None or len(pos) < num or len(nrm) < num:
            continue
        P4 = np.c_[pos[:num] * sc, np.ones(num)]
        world = (Wm[bone] @ P4.T).T[:, :3]
        nloc = nrm[:num, :3].astype(np.int8).astype(float) / 128.0   # packed normal
        wn = (Wm[bone][:3, :3] @ nloc.T).T                           # bone-rotated
        slot = (nrm[:num, 3].astype(int) + 0x80) & 0xff   # VU0 0x120 output index
        for k in range(num):
            s = slot[k]; pool[s] = world[k]; nrmp[s] = wn[k]; seen[s] = True
            if s > maxslot:
                maxslot = s
    if maxslot < 0:
        return None
    return pool[:maxslot + 1], nrmp[:maxslot + 1], seen[:maxslot + 1]


def _valid_desc(buf, NB, P, doff):
    a = P + doff
    if a + 0x18 > NB or a < 0 or doff <= 0:
        return False
    return _u16(buf, a + 2) == 0xffff and 0 < _u32(buf, a + 0x14) < NB


def enum_descs(buf, NB, P, G):
    """group+0x08 -> self-terminating array of draw-descriptor offsets.
    Return list of (desc_off, vifPtr) file-relative."""
    arr = P + _u32(buf, G + 0x08); out = []
    for k in range(64):
        if arr + 4 * k + 4 > NB:
            break
        doff = _u32(buf, arr + 4 * k)
        if not _valid_desc(buf, NB, P, doff):
            break
        out.append((doff, _u32(buf, P + doff + 0x14)))
    return out


def _giftag_prim(qwords):
    """Extract the GS primitive type from a draw sub-packet's V4-32 num=2
    GIFtag template. Each sub-packet opens with a V4-32 num=2 unpack (cmd 0x6c)
    whose first quadword is a GIFtag; when its PRE bit is set, bits 47..57 are
    the PRIM register and bits 0..2 of that are the primitive type:
      3 = GS_PRIM_TRIANGLE (triangle LIST, 3 verts per tri, no shared edge)
      4 = GS_PRIM_TRIANGLESTRIP (sliding 3-window, parity flip)
    Returns the 3-bit prim type, or None if PRE is clear / tag malformed.
    This is authoritative: ~1/3 of Vincent's draw sub-packets are LISTs, not
    strips, and treating them as strips fabricates UV-bridging triangles that
    smear atlas content across UV islands (the torso "scramble")."""
    if qwords is None or len(qwords) < 2:
        return None
    low64 = int(qwords[0]) | (int(qwords[1]) << 32)
    if not ((low64 >> 46) & 1):                # PRE (PRIM-enable) bit
        return None
    return (low64 >> 47) & 0x7                  # PRIM.PRIM (primitive type)


def parse_drawpackets(buf, NB, start, end):
    """Collect the draw sub-packets (each = GIFtag/colour/UV/S-8 index + MSCNT).
    Each sub carries 'prim' = the GS primitive type from its GIFtag (see
    _giftag_prim): 3=triangle LIST, 4=triangle STRIP (None -> assume strip)."""
    p = start; subs = []; cur = None; pending_prim = None
    while p < end and p + 4 <= NB:
        code = _u32(buf, p); num = (code >> 16) & 0xff; cmd = (code >> 24) & 0xff
        if cmd in (0x01, 0x00, 0x04, 0x05, 0x10, 0x11, 0x13, 0x14, 0x15):
            p += 4
        elif cmd == 0x17:                      # MSCNT = end of a sub-packet
            if cur:
                subs.append(cur); cur = None
            pending_prim = None
            p += 4
        elif 0x60 <= cmd <= 0x7f:
            vn = (cmd >> 2) & 3; vl = cmd & 3; bits = {0: 32, 1: 16, 2: 8, 3: 16}[vl]
            nc = vn + 1; nb = nc * bits // 8 * num
            if p + 4 + nb > NB:
                break
            d = buf[p + 4:p + 4 + nb]
            if nc == 4 and bits == 32:                 # V4-32 GIFtag template
                pending_prim = _giftag_prim(np.frombuffer(d, dtype='<u4'))
            else:
                if cur is None:
                    cur = {'prim': pending_prim}
                if nc == 2 and bits == 16:
                    cur['uv'] = np.frombuffer(d, dtype='<i2').reshape(-1, 2).astype(float)
                elif nc == 4 and bits == 8:            # V4-8 per-vertex RGBA colour
                    cur['col'] = np.frombuffer(d, dtype=np.uint8).reshape(-1, 4)
                elif nc == 1 and bits == 8:
                    cur['idx'] = list(d)
            p += 4 + nb
            if p & 3:
                p = (p + 3) & ~3
        else:
            break
    if cur and cur.get('idx'):
        subs.append(cur)
    return subs


def sub_tris(idx, prim):
    """Yield (ja, jb, jc) index-POSITION triples for one draw sub-packet's S-8
    index run, honouring the GIFtag primitive type:
      prim == 3 (triangle LIST):  non-overlapping triples (0,1,2)(3,4,5)...
      otherwise  (triangle STRIP): sliding 3-window with odd-step parity flip.
    Positions index into the sub-packet's parallel idx/uv/col streams; the
    caller maps them to pool slots and UVs."""
    n = len(idx)
    if prim == 3:
        for b in range(0, n - 2, 3):
            yield (b, b + 1, b + 2)
    else:
        for j in range(n - 2):
            if j & 1:
                yield (j + 1, j, j + 2)
            else:
                yield (j, j + 1, j + 2)


def assemble(buf, NB, Wm, bc):
    """Return (verts[N,3], normals[N,3], tris). Decoder validated 2026-10-15/-17
    against the community model rips and the game's own smd draw structure:

      * POSITIONS: each vertex is skinned into a 256-slot scatter pool per group
        (build_group_pool). The position-offset dequant is **/2048** -- i.e.
        ITOF12 (/4096) x2. The x2 was confirmed by matching the rips' body
        thickness exactly (without it the mesh collapses onto the skeleton).
      * ENUMERATION + CONNECTIVITY: taken from the game's OWN draw structure, not
        a heuristic scan. Each group's  group+0x08  points at a self-terminating
        array of draw-DESCRIPTOR offsets (enum_descs); each descriptor's VIF run
        is a sequence of sub-packets (parse_drawpackets), every sub-packet being
          V4-32 num=2 @0 (GIFtag)  V4-8 @3 (RGBA)  V2-16 @2 (UV)
          V1-8 @2 (S-8 POOL INDEX)  MSCNT
        and its S-8 indices form a GS triangle STRIP into THIS group's pool
        (FORMAT.md 8.4). The descriptor array lives on the group object, so every
        sub-packet is associated to its pool DIRECTLY -- no file-position bisect,
        no count-matching, no dropped streams.

      * HISTORY / why this changed: the previous build enumerated draws by a FLAT
        word-scan for  V4-32 num=2  "headers" and  V1-8  index streams, then
        paired them by  header.vtxCount == idx.length  and located the group by
        file position. On char e000 that scan found 475 candidate index streams
        but only 411 had a count-matching header, so **64 real strips (8649
        indices) were silently dropped** -- the cape/arms/legs came out missing,
        3127 tris instead of ~5.5k. The descriptor walk recovers all 405 real
        sub-packets -> 5536 triangles. The UV read is UNCHANGED in value: the V2-16
        parse_drawpackets collects (forward, within the sub-packet) is byte-for-
        byte identical to the earlier backward-read UV fix on all 405 streams
        (verified), so UV degeneracy stays ~0% (0 all-zero-UV tris)."""
    n = _u16(buf, 0x10); offs = [_u32(buf, 0x14 + 4 * i) for i in range(n)]
    smdblk = [(buf[o:o + 8].rstrip(b'\0').decode('latin1'), o + 0x10)
              for o in offs if buf[o + 8:o + 11] == b'smd']
    verts = []; norms = []; tris = []
    for name, P in smdblk:
        if _u32(buf, P) != SM2_MAGIC:
            continue
        sc = _f32(buf, P + 0x2c) / 2048.0           # ITOF12 (/4096) x2 -- see docstring
        grparr = _u32(buf, P + 0x20); gc = _u16(buf, P + 0x0a)
        for gi in range(gc):
            G = P + _u32(buf, P + grparr + 4 * gi)
            r = build_group_pool(buf, NB, Wm, bc, P, G, sc)
            if r is None:
                continue
            pool, nrmp, seen = r
            base = len(verts); psize = len(pool)
            verts.extend([tuple(v) for v in pool])
            norms.extend([tuple(v) for v in nrmp])
            # This group's draw descriptors (group+0x08) -> sub-packet strips.
            descs = enum_descs(buf, NB, P, G)
            for di, (doff, vifptr) in enumerate(descs):
                start = P + vifptr
                # The VIF run ends at the next descriptor's run (self-terminating
                # on an unknown opcode for the last one).
                end = (P + descs[di + 1][1]) if di + 1 < len(descs) else NB
                for sub in parse_drawpackets(buf, NB, start, end):
                    idx = sub.get('idx')
                    if not idx:
                        continue
                    uv = sub.get('uv'); col = sub.get('col'); prim = sub.get('prim')
                    for ja, jb, jc in sub_tris(idx, prim):  # LIST or STRIP per GIFtag
                        a, b, c = idx[ja], idx[jb], idx[jc]
                        if a == b or b == c or a == c:      # restart / degenerate
                            continue
                        if max(a, b, c) >= psize or not (seen[a] and seen[b] and seen[c]):
                            continue
                        if uv is not None and len(uv) > max(ja, jb, jc):
                            uv0, uv1, uv2 = tuple(uv[ja]), tuple(uv[jb]), tuple(uv[jc])
                        else:
                            uv0 = uv1 = uv2 = (0.0, 0.0)
                        c0 = _corner_col(col, ja); c1 = _corner_col(col, jb)
                        c2 = _corner_col(col, jc)
                        tris.append((base + a, base + b, base + c,
                                     uv0, uv1, uv2, c0, c1, c2))
    if not verts:
        return np.zeros((0, 3)), np.zeros((0, 3)), []
    return np.array(verts), np.array(norms), tris


def _corner_col(col, jj):
    """Per-corner RGBA (0..255) from the V4-8 draw colour; PS2-neutral default
    (0x7a7a7a80 = 122,122,122,128) when the sub-packet carried no colour."""
    if col is not None and jj < len(col):
        return (int(col[jj][0]), int(col[jj][1]), int(col[jj][2]), int(col[jj][3]))
    return (122, 122, 122, 128)


# ---- software rasteriser ---------------------------------------------------

def render(V, N, tris, path, tex, angle, elev, textured, backface=False,
           W_=900, H_=1200):
    Vr = V.copy(); Vr[:, 1] *= -1.0
    c = np.median(Vr, 0); rad = np.percentile(np.linalg.norm(Vr - c, axis=1), 98) * 1.7
    az, el = math.radians(angle), math.radians(elev)
    eye = c + rad * np.array([math.cos(el) * math.sin(az), math.sin(el),
                              math.cos(el) * math.cos(az)])
    f = c - eye; f /= np.linalg.norm(f)
    s = np.cross(f, [0, 1.0, 0]); s /= np.linalg.norm(s); u = np.cross(s, f)
    basis = np.array([s, u, -f])
    Vt = (Vr - eye) @ basis.T
    # Game per-vertex normals -> view space (lit shading, winding-independent).
    Nview = None
    if N is not None and len(N) == len(V):
        Nr = N.copy(); Nr[:, 1] *= -1.0
        Nview = Nr @ basis.T
    fl = 1 / math.tan(math.radians(40) / 2); aspect = W_ / H_
    img = np.full((H_, W_, 3), 18, np.float32); zb = np.full((H_, W_), 1e9)
    L = np.array([0.3, 0.6, 0.7]); L /= np.linalg.norm(L)
    TH, TW = (tex.shape[0], tex.shape[1]) if tex is not None else (0, 0)
    for t in tris:
        p = Vt[[t[0], t[1], t[2]]]
        if np.any(p[:, 2] > -0.02):
            continue
        gnrm = np.cross(p[1] - p[0], p[2] - p[0]); gn = np.linalg.norm(gnrm)
        if gn < 1e-9:
            continue
        if backface and gnrm[2] > 0:            # cull back faces (view space +Z)
            continue
        if Nview is not None:                   # game normal (averaged over the 3)
            fn = Nview[[t[0], t[1], t[2]]].mean(0); fnn = np.linalg.norm(fn)
            fn = fn / fnn if fnn > 1e-9 else gnrm / gn
        else:
            fn = gnrm / gn
        pr = np.array([((fl / aspect * q[0] / -q[2] * 0.5 + 0.5) * W_,
                        (0.5 - fl * q[1] / -q[2] * 0.5) * H_, -q[2]) for q in p])
        sh = abs(np.dot(fn, L)) * (0.55 if textured else 0.78) + (0.45 if textured else 0.22)
        minx, maxx = max(int(pr[:, 0].min()), 0), min(int(pr[:, 0].max()) + 1, W_)
        miny, maxy = max(int(pr[:, 1].min()), 0), min(int(pr[:, 1].max()) + 1, H_)
        if maxx <= minx or maxy <= miny:
            continue
        (x0, y0), (x1, y1), (x2, y2) = pr[0, :2], pr[1, :2], pr[2, :2]
        d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if abs(d) < 1e-9:
            continue
        ys, xs = np.mgrid[miny:maxy, minx:maxx]; xf, yf = xs + 0.5, ys + 0.5
        aa = ((y1 - y2) * (xf - x2) + (x2 - x1) * (yf - y2)) / d
        bb = ((y2 - y0) * (xf - x2) + (x0 - x2) * (yf - y2)) / d; g = 1 - aa - bb
        m = (aa >= 0) & (bb >= 0) & (g >= 0)
        if not m.any():
            continue
        z = aa * pr[0, 2] + bb * pr[1, 2] + g * pr[2, 2]
        sub = zb[miny:maxy, minx:maxx]; upd = m & (z < sub)
        if not upd.any():
            continue
        sub[upd] = z[upd]
        if textured and tex is not None:
            uv0, uv1, uv2 = np.array(t[3]), np.array(t[4]), np.array(t[5])
            uu = (aa * uv0[0] + bb * uv1[0] + g * uv2[0]) / 4096.0
            vv = (aa * uv0[1] + bb * uv1[1] + g * uv2[1]) / 4096.0
            tx = np.clip((uu * TW).astype(int), 0, TW - 1)
            tyv = np.clip((vv * TH).astype(int), 0, TH - 1)
            col = tex[tyv, tx] * sh
        else:
            col = np.empty((upd.shape[0], upd.shape[1], 3), np.float32)
            col[...] = np.array([0.74, 0.70, 0.63]) * sh * 255
        block = img[miny:maxy, minx:maxx]; block[upd] = col[upd]
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)
    print("wrote", path)


# ---- mesh export (loadable asset for the native GPU renderer) --------------

MESH_MAGIC = b'DMSH'          # Dirge MeSH: a flat RVertex triangle soup
MESH_VERSION = 1


def _vertex_normals(P, tris):
    """Smoothed per-vertex normals, each forced to point OUTWARD from the model
    centroid. The S-8 strip winding is not globally consistent across groups
    (FORMAT.md §8.4), so a raw face normal has an arbitrary sign; the GPU LIT
    shader samples the per-vertex normal directly (not two-sided), so we must
    hand it an outward-consistent field or lit faces flip dark. Centroid
    orientation is the robust choice for a roughly radial humanoid body."""
    used = sorted({i for t in tris for i in t[:3]})
    centroid = P[used].mean(0) if used else np.zeros(3)
    N = np.zeros_like(P)
    for t in tris:
        i0, i1, i2 = t[0], t[1], t[2]
        fn = np.cross(P[i1] - P[i0], P[i2] - P[i0])
        nn = np.linalg.norm(fn)
        if nn < 1e-12:
            continue
        fn /= nn
        fc = (P[i0] + P[i1] + P[i2]) / 3.0
        if np.dot(fn, fc - centroid) < 0.0:      # force outward
            fn = -fn
        N[i0] += fn; N[i1] += fn; N[i2] += fn
    ln = np.linalg.norm(N, axis=1, keepdims=True); ln[ln < 1e-12] = 1.0
    N = N / ln
    # verts touched by no non-degenerate triangle -> radial fallback
    radial = P - centroid
    rl = np.linalg.norm(radial, axis=1, keepdims=True); rl[rl < 1e-12] = 1.0
    radial = radial / rl
    zero = np.linalg.norm(N, axis=1) < 1e-6
    N[zero] = radial[zero]
    return N, used


def export_mesh(V, N_game, tris, tex, outdir):
    """Dump the posed, textured character to loadable assets in `outdir`:
      char_e000.mesh      -- custom binary RVertex triangle soup (GPU loader)
      char_e000_tex.png   -- the resolved body atlas (RGB)
      char_e000.obj/.mtl  -- the same geometry in Wavefront OBJ (interchange)
    Positions carry the same Y-flip the software rasteriser applies at render
    time, so the GPU's +Y-up auto-frame shows the character upright.
    Normals are the GAME's own per-vertex normals (bone-rotated), Y-flipped to
    match, so lit shading is correct regardless of strip winding."""
    import array
    os.makedirs(outdir, exist_ok=True)
    P = V.astype(np.float64).copy(); P[:, 1] *= -1.0      # upright for +Y-up cam
    used = sorted({i for t in tris for i in t[:3]})
    if N_game is not None and len(N_game) == len(V):
        N = N_game.astype(np.float64).copy(); N[:, 1] *= -1.0
        ln = np.linalg.norm(N, axis=1, keepdims=True); ln[ln < 1e-9] = 1.0; N = N / ln
    else:
        N, used = _vertex_normals(P, tris)
    bbmin = P[used].min(0); bbmax = P[used].max(0)

    # --- custom binary .mesh: header + vcount * (pos3,nrm3,uv2,rgba4) f32 ---
    flat = []
    for t in tris:
        idx = (t[0], t[1], t[2]); uvs = (t[3], t[4], t[5]); cols = (t[6], t[7], t[8])
        for k in range(3):
            i = idx[k]
            flat.extend((P[i][0], P[i][1], P[i][2],
                         N[i][0], N[i][1], N[i][2],
                         uvs[k][0] / 4096.0, uvs[k][1] / 4096.0,
                         cols[k][0] / 255.0, cols[k][1] / 255.0,
                         cols[k][2] / 255.0, cols[k][3] / 255.0))
    vcount = len(tris) * 3
    header = struct.pack('<4sII', MESH_MAGIC, MESH_VERSION, vcount) + \
        struct.pack('<6f', bbmin[0], bbmin[1], bbmin[2], bbmax[0], bbmax[1], bbmax[2])
    meshpath = os.path.join(outdir, "char_e000.mesh")
    with open(meshpath, 'wb') as f:
        f.write(header); array.array('f', flat).tofile(f)
    print("wrote", meshpath, "(%d verts / %d tris)" % (vcount, len(tris)))

    # --- resolved texture as PNG ---
    texpath = os.path.join(outdir, "char_e000_tex.png")
    if tex is not None:
        Image.fromarray(np.clip(tex, 0, 255).astype(np.uint8), 'RGB').save(texpath)
        print("wrote", texpath, "(%dx%d)" % (tex.shape[1], tex.shape[0]))

    # --- Wavefront OBJ + MTL (triangle soup; same Y-flipped positions) ---
    objpath = os.path.join(outdir, "char_e000.obj")
    mtlpath = os.path.join(outdir, "char_e000.mtl")
    with open(mtlpath, 'w') as f:
        f.write("newmtl char_e000\nKa 0 0 0\nKd 1 1 1\nd 1\nillum 1\n"
                "map_Kd char_e000_tex.png\n")
    with open(objpath, 'w') as f:
        f.write("# Dirge of Cerberus char e000 -- posed, textured (render_skinned.py)\n")
        f.write("mtllib char_e000.mtl\nusemtl char_e000\n")
        for t in tris:
            for k in range(3):
                i = t[k]; f.write("v %.5f %.5f %.5f\n" % (P[i][0], P[i][1], P[i][2]))
        for t in tris:
            for k in (3, 4, 5):
                f.write("vt %.6f %.6f\n" % (t[k][0] / 4096.0, 1.0 - t[k][1] / 4096.0))
        for t in tris:
            for k in range(3):
                i = t[k]; f.write("vn %.5f %.5f %.5f\n" % (N[i][0], N[i][1], N[i][2]))
        for ti in range(len(tris)):
            a = ti * 3 + 1; b = a + 1; c = a + 2
            f.write("f %d/%d/%d %d/%d/%d %d/%d/%d\n" % (a, a, a, b, b, b, c, c, c))
    print("wrote", objpath, "+", os.path.basename(mtlpath))


def main():
    buf, NB = load_container(CONT)
    sk = load_skeleton(CONT, trans_scale=1.0); bc = sk['bone_count']
    Wm = [np.array(m, float) for m in sk['world_bind']]
    V, N, tris = assemble(buf, NB, Wm, bc)
    tex = _find_body_texture(CONT, sys.argv[2] if len(sys.argv) > 2 else None)
    print("vertices=%d triangles=%d texture=%s" %
          (len(V), len(tris), "yes" if tex is not None else "NONE"))
    print("bbox X[%.2f,%.2f] Y[%.2f,%.2f] Z[%.2f,%.2f]" %
          (V[:, 0].min(), V[:, 0].max(), V[:, 1].min(), V[:, 1].max(),
           V[:, 2].min(), V[:, 2].max()))
    os.makedirs(OUT, exist_ok=True)
    # Export the posed+textured mesh as a loadable asset for the native GPU
    # renderer (native/gpu). Keeps the software render below working unchanged.
    export_mesh(V, N, tris, tex, os.path.join(HERE, "out"))
    # Clean shaded (connectivity proof: shards gone on the body surface)
    render(V, N, tris, f"{OUT}/char_e000_clean.png",      None, 18, 6, False)
    render(V, N, tris, f"{OUT}/char_e000_clean_3q.png",   None, 55, 8, False)
    render(V, N, tris, f"{OUT}/char_e000_clean_side.png", None, 90, 6, False)
    if tex is not None:
        render(V, N, tris, f"{OUT}/char_e000_textured.png",    tex, 18, 6, True)
        render(V, N, tris, f"{OUT}/char_e000_textured_3q.png", tex, 55, 8, True)
        render(V, N, tris, f"{OUT}/char_e000_textured_side.png", tex, 90, 6, True)


if __name__ == "__main__":
    main()
