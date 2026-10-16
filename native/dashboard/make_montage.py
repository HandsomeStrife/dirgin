#!/usr/bin/env python3
# -I invoked. Build the hero montage from the best renders.
import os
from PIL import Image, ImageDraw, ImageFont

SHARED = "/srv/shared/dirge-port"
OUT = os.path.join(SHARED, "00_hero_montage.png")

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONTB = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

def font(sz, bold=False):
    return ImageFont.truetype(FONTB if bold else FONT, sz)

# background / accent palette (matches dashboard)
BG      = (13, 15, 20)
PANEL   = (20, 23, 31)
BORDER  = (42, 48, 60)
TEXT    = (223, 228, 238)
SUB     = (150, 160, 178)
ACCENT  = (122, 178, 255)
GOLD    = (212, 170, 90)

# tiles: (filename, caption, tag)
tiles = [
    ("13_character_textured_3q.png", "Textured character (Vincent)", "native render"),
    ("10_character_skinned_native.png", "Bind-pose skinned mesh", "skeleton applied"),
    ("08_character_skeleton_posed.png", "Recovered skeleton (103 bones)", "SK2 decode"),
    ("01_model_rendered_native.png", "BMD world geometry", "software raster"),
    ("04_texture_copyright_text.png", "Decoded texture — text reads", "swizzle proof"),
    ("gpu_lit_textured.png", "GPU shading (OpenGL/EGL)", "real AMD GPU"),
]

COLS = 3
ROWS = 2
MARGIN = 34
GAP = 22
IMG_W = 520
IMG_H = 360
LABEL_H = 58
TILE_W = IMG_W
TILE_H = IMG_H + LABEL_H
HEADER = 150

W = MARGIN*2 + COLS*TILE_W + (COLS-1)*GAP
H = HEADER + ROWS*TILE_H + (ROWS-1)*GAP + MARGIN

canvas = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(canvas)

# header
d.text((MARGIN, 30), "Dirge of Cerberus: FFVII", font=font(44, True), fill=TEXT)
d.text((MARGIN, 86), "Purist native PC port · zero emulation · autonomous build run — 2026-10-14",
        font=font(21), fill=SUB)
d.text((MARGIN, 116), "Assets decoded natively · textured character renders · scripts run on native JVM · engine frame-loop runs",
        font=font(18), fill=ACCENT)
d.line((MARGIN, HEADER-16, W-MARGIN, HEADER-16), fill=BORDER, width=2)

def fit(im, bw, bh):
    iw, ih = im.size
    s = min(bw/iw, bh/ih)
    nw, nh = max(1,int(iw*s)), max(1,int(ih*s))
    return im.resize((nw, nh), Image.LANCZOS)

for idx, (fn, cap, tag) in enumerate(tiles):
    r = idx // COLS
    c = idx % COLS
    x = MARGIN + c*(TILE_W+GAP)
    y = HEADER + r*(TILE_H+GAP)
    # panel
    d.rectangle((x, y, x+TILE_W, y+TILE_H), fill=PANEL, outline=BORDER, width=2)
    # image area
    path = os.path.join(SHARED, fn)
    try:
        im = Image.open(path).convert("RGB")
    except Exception as e:
        d.text((x+16, y+16), "missing: "+fn, font=font(16), fill=(200,90,90))
        continue
    im = fit(im, IMG_W-20, IMG_H-20)
    ox = x + (IMG_W - im.size[0])//2
    oy = y + (IMG_H - im.size[1])//2
    canvas.paste(im, (ox, oy))
    # label bar
    ly = y + IMG_H
    d.line((x, ly, x+TILE_W, ly), fill=BORDER, width=2)
    d.text((x+16, ly+9), cap, font=font(20, True), fill=TEXT)
    d.text((x+16, ly+34), tag, font=font(15), fill=GOLD)

canvas.save(OUT)
print("wrote", OUT, canvas.size)
