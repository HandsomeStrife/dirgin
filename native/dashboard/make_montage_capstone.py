#!/usr/bin/env python3
# -I invoked. Rebuild the hero montage with the CAPSTONE (26) as the lead tile.
# Matches the current dashboard montage aesthetic: dark bg, title + subtitle,
# 3x2 grid of black-letterboxed tiles each with a caption bar.
import os
from PIL import Image, ImageDraw, ImageFont

SHARED = "/srv/shared/dirge-port"
OUT = os.path.join(SHARED, "00_hero_montage.png")

FONT  = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONTB = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

def font(sz, bold=False):
    return ImageFont.truetype(FONTB if bold else FONT, sz)

# palette (matches dashboard CSS)
BG     = (13, 15, 20)
PANEL  = (20, 23, 31)
BORDER = (42, 48, 60)
TEXT   = (223, 228, 238)
SUB    = (150, 160, 178)
MUTED  = (107, 117, 137)
ACCENT = (122, 178, 255)
GOLD   = (212, 170, 90)
BLACK  = (0, 0, 0)

# tiles: (filename, title, sub, lead?)
tiles = [
    ("26_capstone_engine_gpu.png", "CAPSTONE — engine loop drives the GPU",
        "native engine + renderer + real decoded assets, one program", True),
    ("21_scene_textured_hero.png", "Textured character in a textured level",
        "native GPU, one depth-sorted pass", False),
    ("17_character_gpu.png", "Textured character on a real GPU",
        "AMD gfx1103 / OpenGL, not software", False),
    ("19_environment_textured_hero.png", "Textured Shinra interior",
        "real BMD level, TXD atlases", False),
    ("15_movie_frame.png", "Decoded CG cutscene frame",
        "FMV decrypted, demuxed, decoded", False),
    ("04_texture_copyright_text.png", "Readable in-game copyright text",
        "proves GS swizzle + CLUT exact", False),
]

COLS, ROWS = 3, 2
MARGIN = 16
GAP = 14
HEADER = 95
IMG_H = 294
LABEL_H = 50
TILE_H = IMG_H + LABEL_H

W = MARGIN*2 + COLS*0  # placeholder, set after TILE_W
TILE_W = (1444 - MARGIN*2 - (COLS-1)*GAP) // COLS
W = MARGIN*2 + COLS*TILE_W + (COLS-1)*GAP
H = HEADER + ROWS*TILE_H + (ROWS-1)*GAP + MARGIN

canvas = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(canvas)

# header
d.text((MARGIN, 16), "Dirge of Cerberus: FFVII — purist native PC port",
       font=font(34, True), fill=TEXT)
d.text((MARGIN, 60),
       "The capstone just landed: the native engine's frame loop now drives the GPU renderer to draw a real DoC scene — zero emulation.",
       font=font(18), fill=SUB)

def fit(im, bw, bh):
    iw, ih = im.size
    s = min(bw/iw, bh/ih)
    return im.resize((max(1,int(iw*s)), max(1,int(ih*s))), Image.LANCZOS)

for idx, (fn, title, sub, lead) in enumerate(tiles):
    r, c = idx // COLS, idx % COLS
    x = MARGIN + c*(TILE_W+GAP)
    y = HEADER + r*(TILE_H+GAP)
    accent = GOLD if lead else BORDER
    bw = 3 if lead else 1
    # image area (black letterbox)
    d.rectangle((x, y, x+TILE_W, y+IMG_H), fill=BLACK)
    path = os.path.join(SHARED, fn)
    try:
        im = Image.open(path).convert("RGB")
        im = fit(im, TILE_W, IMG_H)
        ox = x + (TILE_W - im.size[0])//2
        oy = y + (IMG_H - im.size[1])//2
        canvas.paste(im, (ox, oy))
    except Exception:
        d.text((x+16, y+16), "missing: "+fn, font=font(16), fill=(200,90,90))
    # caption bar
    d.rectangle((x, y+IMG_H, x+TILE_W, y+TILE_H), fill=PANEL)
    d.text((x+14, y+IMG_H+8), title, font=font(16, True),
           fill=(GOLD if lead else TEXT))
    d.text((x+14, y+IMG_H+29), sub, font=font(13), fill=SUB)
    # tile border (gold + thicker for the lead)
    d.rectangle((x, y, x+TILE_W, y+TILE_H), outline=accent, width=bw)

canvas.save(OUT)
print("wrote", OUT, canvas.size)
