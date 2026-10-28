#!/usr/bin/env python3
"""Texture-coordinate tests for gsrender2 (GS rules; no game data).

python3 -I tools/gsdump/src/test_stq.py      (exit 0 = pass)

1. STQ (FST=0): a perspective triangle is fed through the GIF register path
   (PRIM/TEX0/ST/RGBAQ/XYZ2, as the game sends it: S = s*Q, T = t*Q, Q = 1/w)
   and rasterized. Ground truth is computed independently by casting each
   pixel's ray onto the 3D triangle and reading (s, t) there.
2. UV (FST=1): texel i covers [i, i+1), so u = i + 0.6 samples texel i.
3. REGLIST RGBAQ carries Q in bits 63:32.
"""
import sys, struct, numpy as np
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import gsparse
from gsrender2 import DepthInterp, rasterize_targets, tex_coords, texel_index

TW = TH = 16                                        # 2^4 texels
TEX = np.zeros((TH, TW, 4), np.float32)             # texel (i, j) -> R=8i, G=8j
TEX[..., 0] = np.arange(TW)[None, :] * 8
TEX[..., 1] = np.arange(TH)[:, None] * 8
TEX[..., 3] = 128
TEX0 = 1000 | (1 << 14) | (0 << 20) | (4 << 26) | (4 << 30)   # TBP0 1000 (not a target), CT32


class FakeTex:
    def get(self, tex0, seq=None):
        return TEX
    def best_at(self, tb, seq=None):
        return None


def f2u(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def draw(verts, fst, W=64, H=64):
    """verts: [(x, y, S, T, Q, U, V)] in pixels / GS units. Returns rgb (H, W, 3)."""
    g = DepthInterp()
    g.write_reg(gsparse.R_FRAME_1, 0 | (1 << 16))   # FBP 0, FBW 1 (64 px), PSMCT32
    g.write_reg(gsparse.R_XYOFFSET_1, 0)
    g.write_reg(gsparse.R_TEX0_1, TEX0)
    g.write_reg(gsparse.R_PRIM, 3 | (1 << 4) | (fst << 8))   # TRIANGLE, TME, FST
    for x, y, s, t, q, u, v in verts:
        g.write_reg(gsparse.R_ST, f2u(s) | (f2u(t) << 32))
        g.write_reg(gsparse.R_RGBAQ, 0x80808080 | (f2u(q) << 32))
        g.write_reg(gsparse.R_UV, int(u * 16) | (int(v * 16) << 16))
        g.write_reg(gsparse.R_XYZ2, int(x * 16) | (int(y * 16) << 16) | (1 << 32))
    assert len(g.tris) == 1, len(g.tris)
    bufs, st = rasterize_targets(g.tris, FakeTex(), {0: (W, H, 1, 0)})
    assert st['textured'] == 1, st
    return bufs[0], g.tris[0]


def test_stq_perspective():
    f, cx, cy = 24.0, 32.0, 32.0
    # camera-space triangle, strongly receding (w from 1 to 6)
    P = np.array([[-1.2, -1.0, 1.0], [1.5, -0.8, 6.0], [-0.6, 1.4, 2.0]])
    st = np.array([[0.0, 0.0], [1.0, 0.1], [0.2, 1.0]])     # (s, t) in 0..1
    verts = []
    for (X, Y, w), (s, t) in zip(P, st):
        q = 1.0 / w
        verts.append((f * X / w + cx, f * Y / w + cy, s * q, t * q, q, 0, 0))
    rgb, tri = draw(verts, fst=0)
    # the parser kept S, T, Q as sent (pre-multiplied), no extra scaling
    for v, (_, _, s, t, q, _, _) in zip(tri[:3], verts):
        assert abs(v[3] - s) < 1e-6 and abs(v[4] - t) < 1e-6 and abs(v[5] - q) < 1e-6
    # ground truth: ray through pixel centre hits the 3D triangle at (s, t)
    n = np.cross(P[1] - P[0], P[2] - P[0])
    M = np.stack([P[0], P[1], P[2]], 1)             # P @ bary = point
    good = bad = 0
    for py in range(64):
        for px in range(64):
            d = np.array([(px + 0.5 - cx) / f, (py + 0.5 - cy) / f, 1.0])
            k = n @ P[0] / (n @ d)
            bary = np.linalg.solve(M, k * d)
            if (bary < 0.02).any():                 # skip edges (rasterizer coverage rule)
                continue
            s, t = bary @ st
            ei, ej = np.floor(s * TW), np.floor(t * TH)
            if max(abs(s * TW - ei - 0.5), abs(t * TH - ej - 0.5)) > 0.45:
                continue                            # skip texel boundaries (float noise)
            r, g = rgb[py, px, 0], rgb[py, px, 1]
            if r == ei * 8 and g == ej * 8:
                good += 1
            else:
                bad += 1
    assert good > 250, good
    assert bad == 0, f'STQ: {bad} of {good + bad} interior pixels sample the wrong texel'
    return good


def test_uv_floor():
    # affine FST=1 triangle; u at pixel centre px+0.5 is (px+0.5)*0.25 + 0.1 ... simpler:
    # a big triangle with UV = pixel coordinate * 0.25 texels, so pixel 2 (centre 2.5)
    # -> u = 0.625 -> texel 0 (round() would give 1).
    verts = [(0, 0, 0, 0, 1, 0, 0), (64, 0, 0, 0, 1, 16, 0), (0, 64, 0, 0, 1, 0, 16)]
    rgb, _ = draw(verts, fst=1)
    for px in range(1, 40):
        u = (px + 0.5) * 0.25
        assert rgb[1, px, 0] == np.floor(u) * 8, (px, u, rgb[1, px, 0])


def test_helpers():
    a = (0, 0, 0, 0.5, 0.25, 0.5, 0, 0); b = (0, 0, 0, 0.5, 0.25, 0.5, 0, 0)
    u, v = tex_coords(0, a, b, a, np.array(1.0), np.array(0.0), np.array(0.0), 16, 16)
    assert abs(u - 16.0) < 1e-9 and abs(v - 8.0) < 1e-9, (u, v)   # S/Q = 1.0, T/Q = 0.5
    ui, vi = texel_index(np.array(3.99), np.array(4.0), 16, 16, 16, 16)
    assert ui == 3 and vi == 4


def test_reglist_q():
    g = gsparse.GIFInterp() if hasattr(gsparse, 'GIFInterp') else None
    if g is None or not hasattr(g, '_reglist_write'):
        return
    g._reglist_write(0x1, 0x80808080 | (f2u(0.125) << 32))
    assert abs(g.q - 0.125) < 1e-9, g.q


if __name__ == '__main__':
    n = test_stq_perspective()
    test_uv_floor(); test_helpers(); test_reglist_q()
    print(f'ok: STQ perspective triangle matches ray-cast texels at {n} interior pixels; '
          'UV floor; S/Q helper; REGLIST Q')
