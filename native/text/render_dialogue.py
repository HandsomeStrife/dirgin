#!/usr/bin/env python3
# render_dialogue.py - composite REAL Dirge of Cerberus kelfnt glyphs into an
# in-game-style dialogue box, using the recovered Shift-JIS/ASCII -> glyph-cell map.
#
# The glyphs come straight from fonttex/kelfnt0.gray (produced by docfont: the
# TIM3 font decoded LINEAR, 512x448, 32x28 cells of 16x16, 4-bit coverage*17).
# Western dialogue is 1-byte ASCII in the decrypted text-bins; each code maps to a
# half-width cell via the table below (reversed by reading the decoded atlas).
#
# Untrusted font bytes are only read; run with: python3 -I render_dialogue.py
import sys, struct, zlib

CW = 16            # cell pixels
COLS = 32          # cells per atlas row
AW, AH = 512, 448  # kelfnt0 atlas size

# --- recovered half-width ASCII -> kelfnt0 cell index (verified by rendering) ----
# Uppercase: A-J at 630..639, K-Z at 608..623.  Lowercase: a-j at 662..671,
# k-z at 640..655.  Punctuation: '!'=22, ':'=21, '@'=629, '{|}'=656..658,
# '_'=660, '~'=661.  Space has no glyph (advance only).
def ascii_cell(ch):
    o = ord(ch)
    if 'A' <= ch <= 'J': return 630 + (o - ord('A'))
    if 'K' <= ch <= 'Z': return 608 + (o - ord('K'))
    if 'a' <= ch <= 'j': return 662 + (o - ord('a'))
    if 'k' <= ch <= 'z': return 640 + (o - ord('k'))
    return {' ': None, '!': 22, ':': 21, '@': 629,
            '{': 656, '|': 657, '}': 658, '_': 660, '~': 661}.get(ch, None)

# A few directly-confirmed katakana cells (ma/ya rows), read off the decoded atlas.
KATA_CONFIRMED = {'マ':197,'ミ':198,'ム':199,'メ':200,'モ':201,
                  'ャ':202,'ヤ':203,'ュ':204,'ユ':205,'ョ':206,'ヨ':207,
                  'ボ':195,'ポ':196,'ー':12}

def load_gray(path, w, h):
    data = open(path, 'rb').read()
    assert len(data) >= w*h, "gray too small"
    return data[:w*h]

def cell_cov(gray, idx):
    """Return 16x16 list of coverage (0..255) for cell idx."""
    cx, cy = (idx % COLS)*CW, (idx // COLS)*CW
    out = []
    for y in range(CW):
        row = gray[(cy+y)*AW + cx : (cy+y)*AW + cx + CW]
        out.append(list(row))
    return out

# ---- tiny RGBA canvas ----
class Canvas:
    def __init__(self, w, h, bg):
        self.w, self.h = w, h
        self.px = bytearray()
        for _ in range(w*h):
            self.px += bytes(bg)
    def blend(self, x, y, rgb, a):
        if a <= 0 or x < 0 or y < 0 or x >= self.w or y >= self.h: return
        i = (y*self.w + x)*4
        for k in range(3):
            self.px[i+k] = (self.px[i+k]*(255-a) + rgb[k]*a)//255
        self.px[i+3] = 255
    def rect(self, x0, y0, x1, y1, rgb, a):
        for y in range(max(0,y0), min(self.h,y1)):
            for x in range(max(0,x0), min(self.w,x1)):
                self.blend(x, y, rgb, a)
    def save(self, path):
        raw = bytearray()
        for y in range(self.h):
            raw.append(0)
            raw += self.px[y*self.w*4:(y+1)*self.w*4]
        comp = zlib.compress(bytes(raw), 9)
        def chunk(t, d):
            c = struct.pack('>I', len(d)) + t + d
            return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
        with open(path, 'wb') as f:
            f.write(b'\x89PNG\r\n\x1a\n')
            f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', self.w, self.h, 8, 6, 0, 0, 0)))
            f.write(chunk(b'IDAT', comp))
            f.write(chunk(b'IEND', b''))

def ink_bounds(gray, idx):
    """(xmin,xmax) inclusive columns that carry ink; (-1,-1) if blank."""
    if idx is None: return (-1, -1)
    cov = cell_cov(gray, idx)
    xmin, xmax = 99, -1
    for gy in range(CW):
        for gx in range(CW):
            if cov[gy][gx] > 24:
                if gx < xmin: xmin = gx
                if gx > xmax: xmax = gx
    return (xmin, xmax)

def draw_glyph(cv, gray, idx, x, y, scale, rgb, gx0=0):
    """Blit a cell's coverage as coloured text, shifted so column gx0 lands at x."""
    if idx is None: return
    cov = cell_cov(gray, idx)
    for gy in range(CW):
        for gx in range(CW):
            a = cov[gy][gx]
            if a == 0: continue
            px = x + (gx-gx0)*scale
            for sy in range(scale):
                for sx in range(scale):
                    cv.blend(px+sx, y+gy*scale+sy, rgb, a)

def draw_text(cv, gray, s, x, y, scale, rgb, space=4, kern=2):
    """Proportional layout: each glyph advanced by its own ink width."""
    cx = x
    for ch in s:
        if ch == ' ':
            cx += space*scale; continue
        idx = ascii_cell(ch)
        xmin, xmax = ink_bounds(gray, idx)
        if xmax < 0:
            cx += space*scale; continue
        draw_glyph(cv, gray, idx, cx, y, scale, rgb, gx0=xmin)
        cx += (xmax-xmin+1)*scale + kern*scale
    return cx

def draw_cells(cv, gray, idxs, x, y, scale, rgb, gap=2):
    """Fixed-pitch blit of raw cells (used for the kana strip)."""
    cx = x
    for idx in idxs:
        draw_glyph(cv, gray, idx, cx, y, scale, rgb)
        cx += (CW+gap)*scale
    return cx

def main():
    gray = load_gray('fonttex/kelfnt0.gray', AW, AH)

    S = 3
    W, H = 860, 400
    cv = Canvas(W, H, (10, 14, 22, 255))
    for y in range(H):                      # faint scene gradient
        t = y / H
        cv.rect(0, y, W, y+1, (int(12+20*t), int(18+26*t), int(28+40*t)), 255)

    white = (236, 244, 248)
    cyan  = (120, 210, 232)

    # --- in-game dialogue panel (translucent, WRO blue) ---
    px0, py0, px1, py1 = 40, 150, W-40, 300
    cv.rect(px0, py0, px1, py1, (6, 20, 34), 210)
    cv.rect(px0, py0, px1, py0+3, cyan, 235)             # top accent
    cv.rect(px0, py1-2, px1, py1, (40, 90, 120), 200)    # bottom rule
    cv.rect(px0, py0, px0+3, py1, (40, 90, 120), 160)    # left rule

    # speaker name tag
    cv.rect(px0, py0-30, px0+128, py0, (16, 50, 78), 235)
    cv.rect(px0, py0-30, px0+128, py0-27, cyan, 235)
    draw_text(cv, gray, "Reeve", px0+14, py0-21, 2, (185, 230, 244))

    # dialogue lines (real decrypted strings, half-width game glyphs)
    draw_text(cv, gray, "Deactivate the barricade!", px0+22, py0+26, S, white)
    draw_text(cv, gray, "Find Reeve!",               px0+22, py0+26+CW*S+10, S, white)

    # --- title + Japanese sample strip (confirmed real katakana glyphs) ---
    draw_text(cv, gray, "Dirge of Cerberus   native text render",
              px0, 40, 2, cyan)
    draw_text(cv, gray, "real kelfnt glyphs via recovered cell map",
              px0, 74, 1, (150, 185, 205))

    draw_text(cv, gray, "katakana:", px0, py1+28, 2, (150, 185, 205))
    strip = [KATA_CONFIRMED[c] for c in "マミムメモ"]
    draw_cells(cv, gray, strip, px0+150, py1+18, 2, white, gap=3)

    import os
    os.makedirs('out', exist_ok=True)
    cv.save('out/13_dialogue_native.png')
    print("wrote out/13_dialogue_native.png")

if __name__ == '__main__':
    main()
