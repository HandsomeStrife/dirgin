#!/usr/bin/env python3
"""Textured, z-buffered rasterizer for a GS dump -> reproduce the embedded frame.

Unlike gsrender.py (painter-order, flat vertex colour, no texture) this samples
the dump's own decoded textures and uses the per-vertex GS Z for a real z-buffer,
so the output can be graded against the dump's embedded screenshot (the closed
loop that validates the decode + binding stack end to end).

  python3 -I src/gsrender2.py <dump.bin> <out.png> [--notex] [--only-tex] [--flat]

GS XYZ is screen-space (post-VU, 12.4 fixed); no camera recovery is needed.
Z-test is GEQUAL (larger Z = nearer), the Dirge default.
"""
import sys, struct, os, ctypes, numpy as np
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from gsparse import read_header
from gstex import capture, PSM
from gsrender import TriInterp
from gstex_decode import (decode_ct32, decode_ct24, decode_ct16,
                          clut_from, unswizzle8, apply_clut)

# PSMT4 deswizzle via the project's canonical C (native/assets/textures/swizzle.c),
# compiled on demand. Returns a callable(src_bytes,w,h)->(h,w) uint8 index array,
# or None if it can't be built.
def _load_unswizzle4():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, '../../..'))
    tdir = os.path.join(root, 'native/assets/textures')
    so = os.path.join(here, '_libswizzle.so')
    src = os.path.join(tdir, 'swizzle.c')
    try:
        if (not os.path.exists(so)) or os.path.getmtime(so) < os.path.getmtime(src):
            os.system(f'cc -O2 -shared -fPIC -I "{tdir}" "{src}" -o "{so}" 2>/dev/null')
        lib = ctypes.CDLL(so)
        lib.dtex_unswizzle4.restype = ctypes.c_int
        lib.dtex_unswizzle4.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    except Exception:
        return None
    def f(src_bytes, w, h):
        dst = ctypes.create_string_buffer(w * h)
        if lib.dtex_unswizzle4(dst, bytes(src_bytes), w, h) != 1:
            return None
        return np.frombuffer(dst.raw, dtype=np.uint8)[:w * h].reshape(h, w)
    return f

_UNSW4 = _load_unswizzle4()

# GS register addrs for depth-test + blend state (ignored by the base interp)
R_TEST_1 = 0x47; R_TEST_2 = 0x48; R_ZBUF_1 = 0x4e; R_ZBUF_2 = 0x4f
R_ALPHA_1 = 0x42; R_ALPHA_2 = 0x43

class DepthInterp(TriInterp):
    """TriInterp that also tracks TEST (ZTE/ZTST) and ZBUF (ZMSK) per context,
    so the rasterizer can honour the GS depth pipeline (skybox = ALWAYS+ZMSK)."""
    def __init__(self):
        super().__init__()
        self.test = [0, 0]   # TEST_1, TEST_2 raw
        self.zbuf = [0, 0]   # ZBUF_1, ZBUF_2 raw
        self.alpha = [0, 0]  # ALPHA_1, ALPHA_2 raw

    def write_reg(self, addr, data):
        if addr == R_TEST_1:   self.test[0] = data
        elif addr == R_TEST_2: self.test[1] = data
        elif addr == R_ZBUF_1: self.zbuf[0] = data
        elif addr == R_ZBUF_2: self.zbuf[1] = data
        elif addr == R_ALPHA_1: self.alpha[0] = data
        elif addr == R_ALPHA_2: self.alpha[1] = data
        super().write_reg(addr, data)

    def _ctx(self):
        d = super()._ctx()
        c = (self.prim >> 9) & 1
        t = self.test[c]; z = self.zbuf[c]; al = self.alpha[c]
        d['zte'] = t & 1
        d['ztst'] = (t >> 17) & 3          # 0 NEVER,1 ALWAYS,2 GEQUAL,3 GREATER
        d['zmsk'] = (z >> 32) & 1           # 1 = z-write disabled
        # ALPHA: Cv = (A-B)*C>>7 + D ; A/B/D in {Cs,Cd,0}, C in {As,Ad,FIX}
        d['blend'] = (al & 3, (al >> 2) & 3, (al >> 4) & 3, (al >> 6) & 3, (al >> 32) & 0xff)
        return d

def _seed_xyoffset(gif, buf, h):
    """The frame inherits GS context registers (carried from the prior frame) from
    the frozen state; packets only rewrite XYOFFSET partway through. Without seeding,
    the frame's opening draws use OFX/OFY=0 and fly offscreen. Read the two
    XYOFFSET regs out of the freeze block (PCSX2 GSDrawingContext)."""
    fz = h['freeze_at']
    def looks_like_off(v):
        ofx = v & 0xffffffff; ofy = (v >> 32) & 0xffffffff
        return 0 < ofx < 0x10000 and 0 < ofy < 0x10000 and (ofx % 16 == 0) and (ofy % 16 == 0)
    # fixed offsets for this PCSX2 version (state_version 9); verify, else scan.
    cand = []
    for rel in (0x7c, 0xdc):
        v = struct.unpack_from('<Q', buf, fz + rel)[0]
        if looks_like_off(v):
            cand.append(v)
    if len(cand) != 2:
        cand = []
        for rel in range(0, 0x400):
            v = struct.unpack_from('<Q', buf, fz + rel)[0]
            if looks_like_off(v):
                cand.append(v)
                if len(cand) == 2:
                    break
    if len(cand) >= 1:
        gif.xyoffset[0] = cand[0]
        gif.xyoffset[1] = cand[1] if len(cand) > 1 else cand[0]

def build(buf, h):
    gif = DepthInterp()
    _seed_xyoffset(gif, buf, h)
    off = h['packets_at']; n = len(buf)
    while off < n:
        t = buf[off]; off += 1
        if t == 0:
            off += 1
            size, = struct.unpack_from('<I', buf, off); off += 4
            gif.feed(buf[off:off + size]); off += size
        elif t == 1: off += 1
        elif t == 2: off += 4
        elif t == 3: off += 0x2000
        else: break
    return gif

def tex0_fields(t):
    return dict(tbp0=t & 0x3fff, tbw=(t >> 14) & 0x3f, psm=(t >> 20) & 0x3f,
                tw=(t >> 26) & 0xf, th=(t >> 30) & 0xf, cbp=(t >> 37) & 0x3fff,
                cpsm=(t >> 51) & 0xf, csa=(t >> 56) & 0x1f)

class TexCache:
    """Decode each distinct TEX0 once, following gsbind's decode logic."""
    def __init__(self, dump):
        ups = capture(dump)
        by_dbp = {}
        for u in ups:
            by_dbp.setdefault(u['dbp'], []).append(u)
        self.by_dbp = by_dbp
        self.cache = {}
        self.stats = {'t8': 0, 'ct': 0, 't4': 0, 'miss': 0}

    def best_at(self, dbp):
        lst = self.by_dbp.get(dbp)
        if not lst:
            return None
        return max(lst, key=lambda u: u['rrw'] * u['rrh'])

    def get(self, tex0):
        if tex0 in self.cache:
            return self.cache[tex0]
        img = self._decode(tex0)
        self.cache[tex0] = img
        return img

    def _decode(self, tex0):
        f = tex0_fields(tex0)
        w = 1 << f['tw']; hgt = 1 << f['th']
        up = self.best_at(f['tbp0'])
        if up is None:
            self.stats['miss'] += 1
            return None
        data = up['data']; uw, uh = up['rrw'], up['rrh']
        spsm = f['psm']
        try:
            if spsm == 0x13:                       # PSMT8 (CT32 fast-path upload)
                idx = unswizzle8(data, w, hgt)
                cu = self.best_at(f['cbp'])
                if cu is None:
                    self.stats['miss'] += 1; return None
                img = apply_clut(idx, clut_from(cu['data']))
                self.stats['t8'] += 1
                return np.asarray(img, dtype=np.float32)
            elif spsm == 0x00:
                self.stats['ct'] += 1
                return np.asarray(decode_ct32(data, uw, uh), dtype=np.float32)
            elif spsm == 0x01:
                self.stats['ct'] += 1
                return np.asarray(decode_ct24(data, uw, uh), dtype=np.float32)
            elif spsm == 0x02:
                self.stats['ct'] += 1
                return np.asarray(decode_ct16(data, uw, uh), dtype=np.float32)
            elif spsm == 0x14 and _UNSW4 is not None:   # PSMT4 (CT32 fast-path upload)
                idx = _UNSW4(data, w, hgt)
                if idx is None:
                    self.stats['miss'] += 1; return None
                cu = self.best_at(f['cbp'])
                if cu is None:
                    self.stats['miss'] += 1; return None
                pal = clut_from(cu['data'])
                csa = f['csa']
                pal = pal[csa * 16: csa * 16 + 16]
                if len(pal) < 16:
                    pal = np.vstack([pal, np.zeros((16 - len(pal), 4), np.uint8)])
                img = pal[idx]
                self.stats['t4'] += 1
                return np.asarray(img, dtype=np.float32)
            else:                                   # unhandled
                self.stats['miss'] += 1
                return None
        except Exception:
            self.stats['miss'] += 1
            return None

_PAINTER = False
_NO_SENTINEL_Z = False
_LAYER = False

def rasterize(gif, tc, W=640, Hh=480, notex=False, only_tex=False, flat=False):
    img = np.zeros((Hh, W, 3), dtype=np.float32)
    zbuf = np.full((Hh, W), -1e30, dtype=np.float64)
    drawn = 0; offscreen = 0; textured = 0
    tris = gif.tris
    if _LAYER:
        # Painter order with coarse depth layering: draw large background quads
        # first, small foreground detail last. Reproduces the frame without needing
        # the full GS depth state (opaque z=max sentinels, per-draw TEST/ZBUF).
        def area(t):
            a, b, c, ctx = t
            ofx = ctx['xyoffset'] & 0xffff; ofy = (ctx['xyoffset'] >> 32) & 0xffff
            X = [(v[0] - ofx) / 16.0 for v in (a, b, c)]
            Y = [(v[1] - ofy) / 16.0 for v in (a, b, c)]
            return (max(X) - min(X)) * (max(Y) - min(Y))
        tris = sorted(enumerate(gif.tris), key=lambda it: (-area(it[1]), it[0]))
        tris = [t for _, t in tris]
    for a, b, c, ctx in tris:
        ofx = (ctx['xyoffset'] & 0xffff); ofy = (ctx['xyoffset'] >> 32) & 0xffff
        x0 = (a[0] - ofx) / 16.0; y0 = (a[1] - ofy) / 16.0
        x1 = (b[0] - ofx) / 16.0; y1 = (b[1] - ofy) / 16.0
        x2 = (c[0] - ofx) / 16.0; y2 = (c[1] - ofy) / 16.0
        z0 = float(a[2]); z1 = float(b[2]); z2 = float(c[2])
        minx = int(max(0, np.floor(min(x0, x1, x2)))); maxx = int(min(W - 1, np.ceil(max(x0, x1, x2))))
        miny = int(max(0, np.floor(min(y0, y1, y2)))); maxy = int(min(Hh - 1, np.ceil(max(y0, y1, y2))))
        if maxx < minx or maxy < miny:
            offscreen += 1; continue
        if (maxx - minx) * (maxy - miny) > 300000:
            continue
        denom = ((y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2))
        if abs(denom) < 1e-9:
            continue
        xs = np.arange(minx, maxx + 1); ys = np.arange(miny, maxy + 1)
        px, py = np.meshgrid(xs + 0.5, ys + 0.5)
        w0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / denom
        w1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / denom
        w2 = 1 - w0 - w1
        mask = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not mask.any():
            continue
        # interpolated depth (affine screen-space is fine for ordering)
        z = w0 * z0 + w1 * z1 + w2 * z2
        sub_z = zbuf[miny:maxy + 1, minx:maxx + 1]
        zte = ctx.get('zte', 1); ztst = ctx.get('ztst', 2)
        if _PAINTER:
            zmask = mask
        elif zte == 0 or ztst == 1:        # ALWAYS / test disabled
            zmask = mask
        elif ztst == 2:                  # GEQUAL
            zmask = mask & (z >= sub_z)
        elif ztst == 3:                  # GREATER
            zmask = mask & (z > sub_z)
        else:                            # NEVER
            continue
        if not zmask.any():
            continue
        # vertex colour (gouraud or flat)
        if flat:
            cr = (a[8] + b[8] + c[8]) / 3.0; cg = (a[9] + b[9] + c[9]) / 3.0; cb = (a[10] + b[10] + c[10]) / 3.0
            col = np.empty(px.shape + (3,), np.float32)
            col[..., 0] = cr; col[..., 1] = cg; col[..., 2] = cb
        else:
            col = np.stack([w0 * a[8] + w1 * b[8] + w2 * c[8],
                            w0 * a[9] + w1 * b[9] + w2 * c[9],
                            w0 * a[10] + w1 * b[10] + w2 * c[10]], axis=-1).astype(np.float32)
        tex = None
        if ctx['tme'] and not notex:
            tex = tc.get(ctx['tex0'])
        if tex is not None:
            th, tw = tex.shape[0], tex.shape[1]
            f = tex0_fields(ctx['tex0'])
            texW = 1 << f['tw']; texH = 1 << f['th']
            if ctx['fst']:
                # UV register: 12.4 fixed -> texel
                u = (w0 * a[6] + w1 * b[6] + w2 * c[6]) / 16.0
                v = (w0 * a[7] + w1 * b[7] + w2 * c[7]) / 16.0
            else:
                # STQ, perspective correct
                sq = w0 * (a[3] * a[5]) + w1 * (b[3] * b[5]) + w2 * (c[3] * c[5])
                tq = w0 * (a[4] * a[5]) + w1 * (b[4] * b[5]) + w2 * (c[4] * c[5])
                qq = w0 * a[5] + w1 * b[5] + w2 * c[5]
                qq = np.where(np.abs(qq) < 1e-9, 1e-9, qq)
                u = (sq / qq) * texW
                v = (tq / qq) * texH
            # map texel space (texW x texH from TEX0) onto decoded array (tw x th)
            ui = np.mod(np.round(u * (tw / max(texW, 1))).astype(np.int64), tw)
            vi = np.mod(np.round(v * (th / max(texH, 1))).astype(np.int64), th)
            texel = tex[vi, ui]                       # (...,4) RGBA
            out = texel[..., :3] * (col / 128.0)
            ta = texel[..., 3]
            textured += 1
        else:
            if only_tex:
                continue
            out = col * 2.0
            ta = None
        sub_img = img[miny:maxy + 1, minx:maxx + 1]
        if ctx.get('abe', 0) and not notex:
            # GS alpha blend: Cv = (A-B)*C + D, A/B/D in {Cs,Cd,0}, C in {As,Ad,FIX}
            bA, bB, bC, bD, fix = ctx.get('blend', (0, 1, 0, 1, 0x80))
            va = w0 * a[11] + w1 * b[11] + w2 * c[11]      # vertex alpha (128=1.0)
            As = np.clip(va / 128.0, 0, 1)
            if ta is not None:                             # fold in texture alpha
                As = As * np.clip(ta / 128.0, 0, 1)
            As = As[..., None]
            Cs = out; Cd = sub_img
            def term(sel):
                return Cs if sel == 0 else (Cd if sel == 1 else 0.0)
            C = As if bC == 0 else (1.0 if bC == 1 else (fix / 128.0))
            out = (term(bA) - term(bB)) * C + term(bD)
        np.clip(out, 0, 255, out=out)
        sub_img[zmask] = out[zmask]
        # z-write enabled, but don't let z=max sentinel/background geometry (drawn
        # at the far-z rail as a non-occluding backdrop) write depth and block the
        # real foreground behind it.
        if ctx.get('zmsk', 0) == 0 and not _NO_SENTINEL_Z:
            sub_z[zmask] = z[zmask]
        elif ctx.get('zmsk', 0) == 0:
            wm = zmask & (z < 16000000)
            sub_z[wm] = z[wm]
        drawn += 1
    return img, drawn, offscreen, textured

def drawable_size(tris, default=(432, 512)):
    """The GS draws the 3D scene into a native drawable that the video hardware then
    stretches to the 4:3 display. In this game that drawable is 432x512 (verified:
    rasterizing the raw screen-space XY straight into a 640x480 buffer puts Vincent
    ~70px left of and far thinner than the game; at 432x512 resized to 640x480 his
    silhouette lands on the game's). SCISSOR would give it exactly, but like XYOFFSET
    it's inherited from the frozen state and not re-written by the packets, so it
    reads 0 here -- seeding it from the freeze block is future work. FRAME FBW is 448
    (64-aligned) for the main pass, consistent with a 432-wide drawable. Returns
    (W, H); for now the verified 432x512 default, overridable by callers."""
    return default

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    notex = '--notex' in sys.argv
    only_tex = '--only-tex' in sys.argv
    flat = '--flat' in sys.argv
    native = '--native' in sys.argv     # render at the GS drawable size, resize to 4:3
    global _PAINTER, _NO_SENTINEL_Z
    _PAINTER = '--painter' in sys.argv
    _NO_SENTINEL_Z = '--no-sentinel-z' in sys.argv
    global _LAYER
    _LAYER = '--layer' in sys.argv
    buf = open(args[0], 'rb').read()
    h = read_header(buf)
    gif = build(buf, h)
    tc = TexCache(args[0])
    print(f"tris={len(gif.tris)} verts={gif.nverts}")
    from PIL import Image
    if native:
        dw, dh = drawable_size(gif.tris)
        print(f"native drawable {dw}x{dh} -> resize to 640x480 (4:3 display)")
        img, drawn, off, textured = rasterize(gif, tc, W=dw, Hh=dh, notex=notex, only_tex=only_tex, flat=flat)
        out = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB').resize((640, 480))
    else:
        img, drawn, off, textured = rasterize(gif, tc, notex=notex, only_tex=only_tex, flat=flat)
        out = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB')
    out.save(args[1])
    print(f"rasterized {drawn} tris ({textured} textured, {off} offscreen) -> {args[1]}")
    print(f"tex decode: {tc.stats}  distinct tex0 decoded={len(tc.cache)}")

if __name__ == '__main__':
    main()
