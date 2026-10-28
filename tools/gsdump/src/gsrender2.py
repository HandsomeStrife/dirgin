#!/usr/bin/env python3
"""Textured, z-buffered rasterizer for a GS dump -> reproduce the embedded frame.

Unlike gsrender.py (painter-order, flat vertex colour, no texture) this samples
the dump's own decoded textures and uses the per-vertex GS Z for a real z-buffer,
so the output can be graded against the game's screenshot (the closed loop that
validates the decode + binding stack end to end).

  python3 -I src/gsrender2.py <dump.bin> <out.png> [--native] [--crop=display]
        [--frame=last|all|N] [--fb=main|N] [--tex=live|largest]
        [--notex] [--only-tex] [--flat]
  python3 -I src/gsrender2.py <dump.bin> <out.png> --legacy [--native] [--layer --painter ...]

Default (faithful) mode renders ONE frame (--frame, default: the last frame that
ends in a VSync; a dump usually holds several), only into the scene framebuffer
(--fb, default: found from the displayed DISPFB and the blit chain into it, plus
the offscreen buffers it samples, e.g. the flame/bloom layers), with each
texture as it was when the draw happened (--tex=largest: old pick). Output is
the 432x512 drawable (SCISSOR); --native stretches it to 640x480, and
--crop=display first crops to the width the CRTC actually reads (DISPLAY
DW/MAGH = 425.5 here), which matches the PCSX2 screenshot best.

--legacy keeps the original behaviour: every frame and every framebuffer into one
640x480 (or --native 432x512) buffer, largest upload per texture address.

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
R_ALPHA_1 = 0x42; R_ALPHA_2 = 0x43; R_TRXDIR = 0x53; R_TEXA = 0x3b

class DepthInterp(TriInterp):
    """TriInterp that also tracks TEST (ZTE/ZTST) and ZBUF (ZMSK) per context,
    so the rasterizer can honour the GS depth pipeline (skybox = ALWAYS+ZMSK).

    It also records *when* things happen, so a renderer can pick one frame and
    the texture state live at each draw:
      ctx['vframe'] = VSync count before the draw (frame index, set by build())
      ctx['seq']    = index of the triangle in self.tris
      self.uploads  = host->local image transfers in stream order, each with
                      'seq' = len(self.tris) when it started (so it is visible
                      to triangles with seq >= that)."""
    def __init__(self, sprite_uv=False):
        super().__init__()
        self.sprite_uv = sprite_uv
        self.test = [0, 0]   # TEST_1, TEST_2 raw
        self.zbuf = [0, 0]   # ZBUF_1, ZBUF_2 raw
        self.alpha = [0, 0]  # ALPHA_1, ALPHA_2 raw
        self.vframe = 0
        self.uploads = []
        self._up = None      # upload currently receiving IMAGE data
        self.texa = 0x80     # TA0=0x80 until written

    def _sprite(self, a, b):
        # TriInterp._sprite zeroes the UV register (FST=1 sprites, i.e. every
        # screen blit and HUD glyph, sampled texel 0,0). Same rect, UV kept.
        # Opt-in (build(..., sprite_uv=True)) so existing importers are unchanged.
        if not self.sprite_uv:
            return super()._sprite(a, b)
        ax, ay = a[0], a[1]; bx, by, bz = b[0], b[1], b[2]
        def mk(x, y, s, t, u, v):
            return (x, y, bz, s, t, b[5], u, v, b[8], b[9], b[10], b[11])
        c0 = mk(ax, ay, a[3], a[4], a[6], a[7]); c1 = mk(bx, ay, b[3], a[4], b[6], a[7])
        c2 = mk(bx, by, b[3], b[4], b[6], b[7]); c3 = mk(ax, by, a[3], b[4], a[6], b[7])
        ctx = self._ctx()
        self.tris.append((c0, c1, c2, ctx)); self.tris.append((c0, c2, c3, ctx))

    def write_reg(self, addr, data):
        if addr == R_TEST_1:   self.test[0] = data
        elif addr == R_TEST_2: self.test[1] = data
        elif addr == R_ZBUF_1: self.zbuf[0] = data
        elif addr == R_ZBUF_2: self.zbuf[1] = data
        elif addr == R_ALPHA_1: self.alpha[0] = data
        elif addr == R_ALPHA_2: self.alpha[1] = data
        elif addr == R_TEXA: self.texa = data
        elif addr == R_TRXDIR and (data & 3) == 0:      # host -> local starts
            bb = getattr(self, '_bitbltbuf', 0); tr = getattr(self, '_trxreg', 0)
            tp = getattr(self, '_trxpos', 0)
            self._up = dict(dbp=(bb >> 32) & 0x3fff, dbw=((bb >> 48) & 0x3f) * 64,
                            dpsm=(bb >> 56) & 0x3f, rrw=tr & 0xfff, rrh=(tr >> 32) & 0xfff,
                            dsax=(tp >> 32) & 0x7ff, dsay=(tp >> 48) & 0x7ff,
                            seq=len(self.tris), vframe=self.vframe, chunks=[])
            self.uploads.append(self._up)
        super().write_reg(addr, data)

    def feed(self, data):
        """GIFInterp.feed, but keeps IMAGE payloads (attached to the upload that
        the last TRXDIR write started) instead of skipping them."""
        off = 0; n = len(data)
        while off + 16 <= n:
            if self._img_remaining > 0:
                take = min(self._img_remaining, n - off)
                if self._up is not None:
                    self._up['chunks'].append(bytes(data[off:off + take]))
                off += take; self._img_remaining -= take
                continue
            tag = int.from_bytes(data[off:off + 16], 'little'); off += 16
            nloop = tag & 0x7fff; eop = (tag >> 15) & 1; pre = (tag >> 46) & 1
            prim = (tag >> 47) & 0x7ff; flg = (tag >> 58) & 3
            nreg = (tag >> 60) & 0xf or 16
            regs = (tag >> 64) & 0xffffffffffffffff
            reglist = [(regs >> (4 * i)) & 0xf for i in range(nreg)]
            if pre:
                self.set_prim(prim)
            if flg == 0:
                for _ in range(nloop):
                    for ri in reglist:
                        if off + 16 > n: return off
                        self.packed(ri, int.from_bytes(data[off:off + 16], 'little')); off += 16
            elif flg == 1:
                for i in range(nloop * nreg):
                    if off + 8 > n: return off
                    self._reglist_write(reglist[i % nreg], int.from_bytes(data[off:off + 8], 'little')); off += 8
            elif flg == 2:
                self._img_remaining = nloop * 16
            if eop and off >= n:
                break
        return off

    def finish_uploads(self):
        for u in self.uploads:
            if 'chunks' in u:
                u['data'] = b''.join(u.pop('chunks'))

    def _ctx(self):
        d = super()._ctx()
        c = (self.prim >> 9) & 1
        t = self.test[c]; z = self.zbuf[c]; al = self.alpha[c]
        d['zte'] = t & 1                   # NB: legacy - this is really ATE (bit 0)
        d['zte16'] = (t >> 16) & 1         # the actual ZTE bit
        d['ztst'] = (t >> 17) & 3          # 0 NEVER,1 ALWAYS,2 GEQUAL,3 GREATER
        d['zmsk'] = (z >> 32) & 1           # 1 = z-write disabled
        # ALPHA: Cv = (A-B)*C>>7 + D ; A/B/D in {Cs,Cd,0}, C in {As,Ad,FIX}
        d['blend'] = (al & 3, (al >> 2) & 3, (al >> 4) & 3, (al >> 6) & 3, (al >> 32) & 0xff)
        d['test'] = t
        d['texa'] = self.texa
        d['zbufr'] = z
        d['scissor'] = self.scissor[c]
        d['vframe'] = self.vframe
        d['seq'] = len(self.tris)
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

def build(buf, h, sprite_uv=False):
    gif = DepthInterp(sprite_uv=sprite_uv)
    _seed_xyoffset(gif, buf, h)
    off = h['packets_at']; n = len(buf)
    gif.vsyncs = []          # len(gif.tris) at each VSync packet
    gif.priv_at = [h['regs_at']]   # privileged-register snapshots (DISPFB/DISPLAY)
    while off < n:
        t = buf[off]; off += 1
        if t == 0:
            off += 1
            size, = struct.unpack_from('<I', buf, off); off += 4
            gif.feed(buf[off:off + size]); off += size
        elif t == 1:
            off += 1
            gif.vsyncs.append(len(gif.tris)); gif.vframe += 1
        elif t == 2: off += 4
        elif t == 3:
            gif.priv_at.append(off); off += 0x2000
        else: break
    gif.finish_uploads()
    return gif

def tex0_fields(t):
    return dict(tbp0=t & 0x3fff, tbw=(t >> 14) & 0x3f, psm=(t >> 20) & 0x3f,
                tw=(t >> 26) & 0xf, th=(t >> 30) & 0xf, cbp=(t >> 37) & 0x3fff,
                cpsm=(t >> 51) & 0xf, csa=(t >> 56) & 0x1f)

def tex_coords(fst, a, b, c, w0, w1, w2, texW, texH):
    """Texel-space (u, v) per pixel, GS rules (GS User's Manual, 3.4 texture mapping).
    FST=1: UV register, 12.4 fixed point -> texel units.
    FST=0: the packet's S, T are already s*Q, t*Q (the VU divides by w before kick);
    the GS interpolates S, T, Q linearly in screen space and divides per pixel:
    u = (S/Q) * 2^TW, v = (T/Q) * 2^TH. S and T must NOT be multiplied by Q here."""
    if fst:
        u = (w0 * a[6] + w1 * b[6] + w2 * c[6]) / 16.0
        v = (w0 * a[7] + w1 * b[7] + w2 * c[7]) / 16.0
        return u, v
    sq = w0 * a[3] + w1 * b[3] + w2 * c[3]
    tq = w0 * a[4] + w1 * b[4] + w2 * c[4]
    qq = w0 * a[5] + w1 * b[5] + w2 * c[5]
    qq = np.where(np.abs(qq) < 1e-9, 1e-9, qq)
    return (sq / qq) * texW, (tq / qq) * texH

def texel_index(u, v, tw, th, texW, texH):
    """Nearest (point) sampling: texel i covers [i, i+1), so the GS takes floor(u),
    not round(u) (UV = 16*i + 8 is texel i's centre). Wraps (REPEAT); CLAMP_1/2
    and bilinear TEX1 are not emulated."""
    ui = np.mod(np.floor(u * (tw / max(texW, 1))).astype(np.int64), tw)
    vi = np.mod(np.floor(v * (th / max(texH, 1))).astype(np.int64), th)
    return ui, vi

class TexCache:
    """Decode each distinct TEX0 once, following gsbind's decode logic.

    TexCache(dump) keeps the legacy lookup: for each address, the largest upload
    anywhere in the dump. Pass uploads=gif.uploads (from build()) and call
    get(tex0, seq) to get the texture as it was when triangle `seq` was drawn:
    the latest upload to that address that started before it (falling back to
    the legacy pick when nothing precedes it; counted in stats['stale'])."""
    def __init__(self, dump, uploads=None):
        ups = capture(dump) if uploads is None else uploads
        by_dbp = {}
        for i, u in enumerate(ups):
            u.setdefault('seq', -1)
            by_dbp.setdefault(u['dbp'], []).append(u)
        self.by_dbp = by_dbp
        self.cache = {}
        self.stats = {'t8': 0, 'ct': 0, 't4': 0, 'miss': 0, 'stale': 0}

    def best_at(self, dbp, seq=None):
        lst = self.by_dbp.get(dbp)
        if not lst:
            return None
        if seq is not None:
            live = None
            for u in lst:              # stream order
                if u['seq'] <= seq:
                    live = u
                else:
                    break
            if live is not None:
                return live
            self.stats['stale'] += 1
        return max(lst, key=lambda u: u['rrw'] * u['rrh'])

    def get(self, tex0, seq=None):
        if seq is None:
            key = tex0
        else:
            f = tex0_fields(tex0)
            st = self.stats['stale']
            key = (tex0, id(self.best_at(f['tbp0'], seq)), id(self.best_at(f['cbp'], seq)))
            self.stats['stale'] = st
        if key in self.cache:
            return self.cache[key]
        self._seq = seq
        img = self._decode(tex0)
        self.cache[key] = img
        return img

    def _decode(self, tex0):
        f = tex0_fields(tex0)
        w = 1 << f['tw']; hgt = 1 << f['th']
        up = self.best_at(f['tbp0'], getattr(self, '_seq', None))
        if up is None:
            self.stats['miss'] += 1
            return None
        data = up['data']; uw, uh = up['rrw'], up['rrh']
        spsm = f['psm']
        try:
            if spsm == 0x13:                       # PSMT8 (CT32 fast-path upload)
                idx = unswizzle8(data, w, hgt)
                cu = self.best_at(f['cbp'], getattr(self, '_seq', None))
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
                cu = self.best_at(f['cbp'], getattr(self, '_seq', None))
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
_TEX_LARGEST = False   # --tex=largest: legacy pick (largest upload per address)

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
            u, v = tex_coords(ctx['fst'], a, b, c, w0, w1, w2, texW, texH)
            # map texel space (texW x texH from TEX0) onto decoded array (tw x th)
            ui, vi = texel_index(u, v, tw, th, texW, texH)
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
    stretches to the 4:3 display. In this game it is 432x512: the main pass's
    SCISSOR_1 is written in the packet stream as X 0..431, Y 0..511 (FRAME FBW is
    448 only because buffer widths are 64-aligned). Returns the most common
    non-zero scissor among `tris` as (W, H), else `default`."""
    from collections import Counter
    c = Counter(t[3].get('scissor', 0) for t in tris)
    c.pop(0, None)
    if not c:
        return default
    sc = c.most_common(1)[0][0]
    x0, x1 = sc & 0x7ff, (sc >> 16) & 0x7ff
    y0, y1 = (sc >> 32) & 0x7ff, (sc >> 48) & 0x7ff
    return (x1 + 1, y1 + 1) if x0 == 0 and y0 == 0 else default

# ---------------------------------------------------------------------------
# Frame / framebuffer selection

def frame_ranges(gif):
    """[(start, end)] triangle-index ranges per frame, split at VSync packets.
    A trailing range with no VSync after it is marked partial (third element)."""
    out = []; prev = 0
    for v in getattr(gif, 'vsyncs', []):
        out.append((prev, v, True)); prev = v
    if prev < len(gif.tris):
        out.append((prev, len(gif.tris), False))
    return out

def pick_frame(gif, which='last'):
    """which: 'last' (last complete non-empty frame), 'all', or an int index."""
    if which == 'all':
        return 0, len(gif.tris)
    fr = frame_ranges(gif)
    if which == 'last':
        full = [(a, b) for a, b, ok in fr if ok and b > a]
        if full:
            return full[-1]
        return fr[-1][:2] if fr else (0, len(gif.tris))
    a, b, _ = fr[int(which)]
    return a, b

def display_info(buf, gif):
    """Displayed framebuffer from the last privileged-register snapshot:
    PMODE picks the enabled read circuit, DISPFB gives FBP/FBW/PSM and DISPLAY the
    width the CRTC reads ((DW+1)/(MAGH+1)) and height (DH+1)."""
    at = gif.priv_at[-1]
    g = lambda o: struct.unpack_from('<Q', buf, at + o)[0]
    pmode = g(0x00)
    circ = 2 if (pmode & 2) and not (pmode & 1) else 1
    dfb = g(0x70 if circ == 1 else 0x90); dsp = g(0x80 if circ == 1 else 0xa0)
    magh = (dsp >> 23) & 0xf; dw = (dsp >> 32) & 0xfff; dh = (dsp >> 44) & 0x7ff
    return dict(circuit=circ, fbp=dfb & 0x1ff, fbw=((dfb >> 9) & 0x3f) * 64,
                psm=(dfb >> 15) & 0x1f, dbx=(dfb >> 32) & 0x7ff, dby=(dfb >> 43) & 0x7ff,
                read_w=(dw + 1) / (magh + 1), read_h=dh + 1)

def _fbp(ctx):
    return ctx['frame'] & 0x1ff

def main_framebuffer(tris, disp_fbp):
    """The buffer the 3D scene is drawn into. Starts at the displayed FBP and, while
    the draws into it are (>=90%) textured copies of one other render target
    (TBP0 == that FBP*32, e.g. Dirge blits FBP 224 into the displayed 112 in 32px
    strips), follows the copy back to its source. Returns (fbp, chain)."""
    from collections import Counter
    drawn = set(_fbp(t[3]) for t in tris)
    cur = disp_fbp; chain = [cur]
    for _ in range(4):
        into = [t[3] for t in tris if _fbp(t[3]) == cur]
        if not into:
            break
        src = Counter()
        for c in into:
            tb = c['tex0'] & 0x3fff
            if c['tme'] and tb % 32 == 0 and tb // 32 in drawn and tb // 32 != cur:
                src[tb // 32] += 1
        if not src:
            break
        s_fbp, k = src.most_common(1)[0]
        if k < 0.9 * len(into):
            break
        cur = s_fbp; chain.append(cur)
    return cur, chain

def needed_targets(tris, main_fbp):
    """main + every render target (FBP drawn this frame) that main's draws sample,
    transitively -- e.g. the flame layer composited onto the scene."""
    drawn = set(_fbp(t[3]) for t in tris)
    need = {main_fbp}; todo = [main_fbp]
    while todo:
        f = todo.pop()
        for t in tris:
            c = t[3]
            if _fbp(c) != f or not c['tme']:
                continue
            tb = c['tex0'] & 0x3fff
            if tb % 32 == 0 and tb // 32 in drawn and tb // 32 not in need:
                need.add(tb // 32); todo.append(tb // 32)
    return need

# ---------------------------------------------------------------------------
# Per-framebuffer rasterizer (faithful mode)

_PSM_BITS = {0: 32, 1: 24, 2: 16, 0x0a: 16, 0x30: 32, 0x31: 24, 0x32: 16, 0x3a: 16}

def _atest(atst, a, aref):
    return {0: np.zeros_like(a, bool), 1: np.ones_like(a, bool), 2: a < aref, 3: a <= aref,
            4: a == aref, 5: a >= aref, 6: a > aref, 7: a != aref}[atst]

def _page_h(psm):
    """GS page height in pixels (pages are 64 wide): 32 for 32/24-bit, 64 for 16-bit."""
    return 64 if _PSM_BITS.get(psm, 32) == 16 else 32

class _Surf:
    """A rectangle of GS memory: `fbw` pages wide starting at page `base`."""
    def __init__(self, kind, base, fbw, psm, H):
        self.kind, self.base, self.fbw, self.psm = kind, base, fbw, psm
        self.ph = _page_h(psm); self.W = fbw * 64; self.H = H
        self.npages = -(-H // self.ph) * fbw
        if kind == 'c':
            self.rgb = np.zeros((H, self.W, 3), np.float32); self.a = np.zeros((H, self.W), np.float32)
        else:
            self.z = np.full((H, self.W), -1e30, np.float64)
        self.used = False; self.last_seq = -1

    def remap(self, base, fbw, ph, xs, ys):
        """Pixels (xs, ys) of a buffer at (base, fbw) with page height ph -> this
        surface's pixels, by page. Returns (ok mask, tx, ty)."""
        if ph != self.ph:
            return np.zeros(xs.shape, bool), xs, ys
        lp = base + (ys // ph) * fbw + xs // 64 - self.base
        ok = (lp >= 0) & (lp < self.npages)
        tx = (lp % self.fbw) * 64 + xs % 64; ty = (lp // self.fbw) * ph + ys % ph
        ok &= (tx < self.W) & (ty < self.H) & (tx >= 0) & (ty >= 0)
        return ok, tx, ty

def _z_to_rgb(z, zpsm):
    z = int(z)
    if _PSM_BITS.get(zpsm, 32) == 16:
        return ((z & 31) << 3, ((z >> 5) & 31) << 3, ((z >> 10) & 31) << 3), (128 if z >> 15 & 1 else 0)
    return (z & 0xff, (z >> 8) & 0xff, (z >> 16) & 0xff), ((z >> 24) & 0xff)

def rasterize_targets(tris, tc, targets, notex=False, only_tex=False, flat=False):
    """Rasterize `tris` into the colour buffers `targets` ({fbp: (W, H, fbw, psm)}).

    Modelled at GS page granularity, because Dirge relies on memory aliasing:
      - a draw with a different FBW than the target (e.g. the FBW-4 fast clear of
        the FBW-7 scene buffer) is page-remapped (flat untextured fills only);
      - a Z write whose ZBP lands inside a colour target writes the Z bytes as
        colour (the clear's "depth" is its colour, covering the buffer's 2nd half);
      - draws to other framebuffers still update a target's depth buffer (the
        display blit at frame start doubles as the scene's Z clear).
    Textures: TBP0 at a target's base samples that buffer (render-to-texture,
    unless a newer upload covers it); otherwise the upload live at the draw
    (tc.get(tex0, seq)). Honours SCISSOR, TEST (alpha test, ZTE/ZTST), ZBUF.ZMSK,
    ALPHA blending incl. destination alpha, TEXA for 24/16-bit reads.
    Returns ({fbp: rgb float array (H, W, 3)}, stats)."""
    Hz = max(v[1] for v in targets.values())
    cs = {fbp: _Surf('c', fbp, fbw, psm, H) for fbp, (W, H, fbw, psm) in targets.items()}
    zs = {}
    for a, b, c, ctx in tris:                      # depth surfaces the targets use
        C = cs.get(_fbp(ctx))
        if C is not None and C.fbw == (ctx['frame'] >> 16) & 0x3f and (ctx['test'] >> 16) & 1:
            zr = ctx['zbufr']; k = (zr & 0x1ff, C.fbw)
            if k not in zs:
                zs[k] = _Surf('z', k[0], k[1], 0x30 | ((zr >> 24) & 0xf), Hz)
    st = dict(drawn=0, textured=0, other_fb=0, z_only=0, page_fill=0, unsupported=0,
              offscreen=0, rt_tex=0, rt_fmt_skip=0, tex_miss_skip=0, atest_fail=0)
    for a, b, c, ctx in tris:
        fbp = _fbp(ctx); ffbw = (ctx['frame'] >> 16) & 0x3f; fpsm = (ctx['frame'] >> 24) & 0x3f
        zr = ctx['zbufr']; zbp = zr & 0x1ff; zpsm = 0x30 | ((zr >> 24) & 0xf); zmsk = (zr >> 32) & 1
        t = ctx.get('test', 0); zte = (t >> 16) & 1; ztst = (t >> 17) & 3
        C = cs.get(fbp)
        direct = C is not None and C.fbw == ffbw
        Z = zs.get((zbp, ffbw)) if zte else None
        if not direct:
            if C is None and Z is None:
                # z-aliasing into a target? (only for flat fills, handled below)
                alias = zte and not zmsk and any(S.base <= zbp < S.base + S.npages for S in cs.values())
                if not alias:
                    st['other_fb'] += 1; continue
        ofx = (ctx['xyoffset'] & 0xffff); ofy = (ctx['xyoffset'] >> 32) & 0xffff
        x0 = (a[0] - ofx) / 16.0; y0 = (a[1] - ofy) / 16.0
        x1 = (b[0] - ofx) / 16.0; y1 = (b[1] - ofy) / 16.0
        x2 = (c[0] - ofx) / 16.0; y2 = (c[1] - ofy) / 16.0
        sx0, sx1, sy0, sy1 = 0, 2047, 0, 2047
        if direct:
            sx1, sy1 = C.W - 1, C.H - 1
        elif Z is not None and C is None:
            sx1, sy1 = Z.W - 1, Z.H - 1
        sc = ctx.get('scissor', 0)
        if sc:
            sx0 = max(sx0, sc & 0x7ff); sx1 = min(sx1, (sc >> 16) & 0x7ff)
            sy0 = max(sy0, (sc >> 32) & 0x7ff); sy1 = min(sy1, (sc >> 48) & 0x7ff)
        minx = int(max(sx0, np.floor(min(x0, x1, x2)))); maxx = int(min(sx1, np.ceil(max(x0, x1, x2))))
        miny = int(max(sy0, np.floor(min(y0, y1, y2)))); maxy = int(min(sy1, np.ceil(max(y0, y1, y2))))
        if maxx < minx or maxy < miny:
            st['offscreen'] += 1; continue
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
        z = w0 * float(a[2]) + w1 * float(b[2]) + w2 * float(c[2])
        zwrite = zte and not zmsk and ztst != 0
        if not direct:
            if C is None and Z is not None:
                # depth-only: a draw to a non-target buffer sharing a target's Z
                if not zwrite:
                    st['other_fb'] += 1; continue
                sub_z = Z.z[miny:maxy + 1, minx:maxx + 1]
                zt = mask if ztst == 1 else (mask & (z >= sub_z) if ztst == 2 else mask & (z > sub_z))
                sub_z[zt] = z[zt]
                st['z_only'] += 1; continue
            # page-remapped flat fill (fast clears): untextured, unblended, Z ALWAYS
            if ctx['tme'] or ctx.get('abe', 0) or (zte and ztst not in (1,)):
                st['unsupported'] += 1; continue
            gx = (px[mask] - 0.5).astype(np.int64); gy = (py[mask] - 0.5).astype(np.int64)
            if C is not None:
                ok, tx, ty = C.remap(fbp, ffbw, _page_h(fpsm), gx, gy)
                col = np.array(b[8:11], np.float32)
                C.rgb[ty[ok], tx[ok]] = col; C.a[ty[ok], tx[ok]] = b[11]
                if ok.any():
                    C.used = True; C.last_seq = ctx.get('seq', 0)
            if zwrite:
                zval = float(b[2])
                Zt = zs.get((zbp, ffbw))
                if Zt is not None:
                    ok, tx, ty = Zt.remap(zbp, ffbw, _page_h(zpsm), gx, gy)
                    Zt.z[ty[ok], tx[ok]] = zval
                for S in cs.values():               # Z memory aliasing a colour buffer
                    ok, tx, ty = S.remap(zbp, ffbw, _page_h(zpsm), gx, gy)
                    if ok.any():
                        rgb, al = _z_to_rgb(zval, zpsm)
                        S.rgb[ty[ok], tx[ok]] = np.array(rgb, np.float32); S.a[ty[ok], tx[ok]] = al
                        S.used = True; S.last_seq = ctx.get('seq', 0)
            st['page_fill'] += 1; continue
        # ---------------- normal path: draw into target C (same FBW)
        if flat:
            col = np.empty(px.shape + (3,), np.float32)
            for k in range(3):
                col[..., k] = (a[8 + k] + b[8 + k] + c[8 + k]) / 3.0
        else:
            col = np.stack([w0 * a[8 + k] + w1 * b[8 + k] + w2 * c[8 + k] for k in range(3)],
                           axis=-1).astype(np.float32)
        va = w0 * a[11] + w1 * b[11] + w2 * c[11]
        As = va
        if ctx['tme'] and not notex:
            f = tex0_fields(ctx['tex0'])
            tb = f['tbp0']
            texW = 1 << f['tw']; texH = 1 << f['th']
            u, v = tex_coords(ctx['fst'], a, b, c, w0, w1, w2, texW, texH)
            R = cs.get(tb // 32) if tb % 32 == 0 else None
            if R is not None and R.used:
                # memory holds whichever wrote last; a small newer upload (e.g. a
                # 16x16 CLUT in the buffer's corner) doesn't replace the buffer
                up = tc.best_at(tb, ctx.get('seq'))
                if up is not None and up['seq'] > R.last_seq and \
                   up['rrw'] * up['rrh'] >= 0.5 * R.W * R.H:
                    R = None
            if R is not None and R.used:
                if (_PSM_BITS.get(R.psm, 32) == 16) != (_PSM_BITS.get(f['psm'], 0) == 16):
                    st['rt_fmt_skip'] += 1; continue    # format reinterpretation: can't emulate
                ui = np.clip(np.floor(u).astype(np.int64), 0, R.W - 1)
                vi = np.clip(np.floor(v).astype(np.int64), 0, R.H - 1)
                trgb = R.rgb[vi, ui]
                if f['psm'] == 0:
                    ta = R.a[vi, ui]
                else:                                       # 24/16-bit read: TEXA
                    tx = ctx.get('texa', 0x80)
                    ta = np.full(ui.shape, float(tx & 0xff), np.float32)
                    if _PSM_BITS.get(f['psm'], 0) == 16:        # A bit = stored As >= 0x80
                        ta[R.a[vi, ui] >= 128] = float((tx >> 32) & 0xff)
                    if (tx >> 15) & 1:                      # AEM: black -> alpha 0
                        ta[trgb.sum(-1) == 0] = 0.0
                st['rt_tex'] += 1
            else:
                tex = tc.get(ctx['tex0'], None if _TEX_LARGEST else ctx.get('seq'))
                if tex is None:
                    st['tex_miss_skip'] += 1; continue
                th, tw = tex.shape[0], tex.shape[1]
                ui, vi = texel_index(u, v, tw, th, texW, texH)
                texel = tex[vi, ui]
                trgb = texel[..., :3]; ta = texel[..., 3]
            out = trgb * (col / 128.0)                    # TFX MODULATE
            As = va * ta / 128.0
            st['textured'] += 1
        else:
            if only_tex:
                continue
            out = col.copy()
        zw_ok = mask.copy(); fb_ok = mask.copy()
        if t & 1:                                         # alpha test
            ok = _atest((t >> 1) & 7, np.floor(np.clip(As, 0, 255)), (t >> 4) & 0xff)
            afail = (t >> 12) & 3
            if (mask & ~ok).any():
                st['atest_fail'] += 1
            if afail == 0:   fb_ok &= ok; zw_ok &= ok      # KEEP
            elif afail == 1: zw_ok &= ok                   # FB_ONLY
            elif afail == 2: fb_ok &= ok                   # ZB_ONLY
            else:            zw_ok &= ok                   # RGB_ONLY (alpha kept; approx)
        sub_z = Z.z[miny:maxy + 1, minx:maxx + 1] if Z is not None else None
        if zte:
            if ztst == 0: continue
            if sub_z is not None and ztst in (2, 3):
                zt = (z >= sub_z) if ztst == 2 else (z > sub_z)
                fb_ok &= zt; zw_ok &= zt
        if not (fb_ok.any() or zw_ok.any()):
            continue
        sub_img = C.rgb[miny:maxy + 1, minx:maxx + 1]
        sub_a = C.a[miny:maxy + 1, minx:maxx + 1]
        if ctx.get('abe', 0):
            bA, bB, bC, bD, fix = ctx.get('blend', (0, 1, 0, 1, 0x80))
            Cs = out; Cd = sub_img
            term = lambda sel: Cs if sel == 0 else (Cd if sel == 1 else 0.0)
            Cf = (np.clip(As, 0, 255) / 128.0)[..., None] if bC == 0 else \
                 ((sub_a / 128.0)[..., None] if bC == 1 else fix / 128.0)
            out = (term(bA) - term(bB)) * Cf + term(bD)
        np.clip(out, 0, 255, out=out)
        sub_img[fb_ok] = out[fb_ok]
        if _PSM_BITS.get(fpsm, 32) != 24:
            sub_a[fb_ok] = np.clip(As, 0, 255)[fb_ok]
        if zwrite and sub_z is not None:
            sub_z[zw_ok] = z[zw_ok]
        C.used = True; C.last_seq = ctx.get('seq', 0); C.psm = fpsm
        st['drawn'] += 1
    return {k: S.rgb for k, S in cs.items()}, st

def render_frame(buf, h, gif, tc, frame='last', fb='main', **kw):
    """Faithful oracle: one frame, the scene framebuffer (plus the render targets
    it samples), texture state as of each draw. Returns (rgb HxWx3 float, info)."""
    a, b = pick_frame(gif, frame)
    tris = gif.tris[a:b]
    disp = display_info(buf, gif)
    if fb == 'main':
        main_fbp, chain = main_framebuffer(tris, disp['fbp'])
        if not any(_fbp(t[3]) == main_fbp for t in tris):
            # double-buffered: the frame draws into the back buffer, not the one
            # on screen -> take the buffer with the most draws
            from collections import Counter
            main_fbp = Counter(_fbp(t[3]) for t in tris).most_common(1)[0][0]
            chain = chain + [f'{main_fbp} (most draws)']
    else:
        main_fbp, chain = int(fb), [int(fb)]
    need = needed_targets(tris, main_fbp)
    from collections import Counter
    targets = {}; drawable = {}
    for f in need:
        ft = [t for t in tris if _fbp(t[3]) == f]
        fbw, psm = Counter(((t[3]['frame'] >> 16) & 0x3f, (t[3]['frame'] >> 24) & 0x3f)
                           for t in ft).most_common(1)[0][0]
        W, H = drawable_size([t for t in ft if (t[3]['frame'] >> 16) & 0x3f == fbw],
                             default=(fbw * 64, 512))
        targets[f] = (max(W, fbw * 64), H, fbw, psm); drawable[f] = (W, H)
    bufs, st = rasterize_targets(tris, tc, targets, **kw)
    W, H = drawable[main_fbp]
    info = dict(frame=(a, b), display=disp, chain=chain, main=main_fbp,
                targets=drawable, stats=st)
    return bufs[main_fbp][:H, :W], info

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = {a.split('=')[0]: (a.split('=', 1)[1] if '=' in a else True)
            for a in sys.argv[1:] if a.startswith('--')}
    notex = '--notex' in opts
    only_tex = '--only-tex' in opts
    flat = '--flat' in opts
    native = '--native' in opts     # resize the drawable to the 640x480 4:3 display
    legacy = '--legacy' in opts or any(k in opts for k in ('--painter', '--no-sentinel-z', '--layer'))
    global _PAINTER, _NO_SENTINEL_Z, _LAYER
    _PAINTER = '--painter' in opts
    _NO_SENTINEL_Z = '--no-sentinel-z' in opts
    _LAYER = '--layer' in opts
    buf = open(args[0], 'rb').read()
    h = read_header(buf)
    gif = build(buf, h, sprite_uv=not legacy)
    print(f"tris={len(gif.tris)} verts={gif.nverts} frames={len(frame_ranges(gif))}")
    from PIL import Image
    global _TEX_LARGEST
    _TEX_LARGEST = opts.get('--tex') == 'largest'
    if not legacy:
        tc = TexCache(args[0], uploads=gif.uploads)
        img, info = render_frame(buf, h, gif, tc, frame=opts.get('--frame', 'last'),
                                 fb=opts.get('--fb', 'main'), notex=notex,
                                 only_tex=only_tex, flat=flat)
        d = info['display']
        print(f"frame tris {info['frame'][0]}..{info['frame'][1]}; displayed FBP {d['fbp']} "
              f"(fbw {d['fbw']}, CRTC reads {d['read_w']:g}x{d['read_h']}); scene FBP chain "
              f"{' <- '.join(map(str, info['chain']))}; targets {info['targets']}")
        out = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB')
        if opts.get('--crop') == 'display':           # what the CRTC shows
            box = (0, 0, min(out.size[0], d['read_w']), min(out.size[1], d['read_h']))
            out = out.resize((640, 480) if native else (int(round(box[2])), int(box[3])), box=box)
        elif native:
            out = out.resize((640, 480))
        out.save(args[1])
        print(f"{info['stats']} -> {args[1]}")
        print(f"tex decode: {tc.stats}  distinct decoded={len(tc.cache)}")
        return
    # legacy: every frame in the dump into one buffer, largest upload per address
    tc = TexCache(args[0])
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
