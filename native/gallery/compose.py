#!/usr/bin/env python3
"""Compose labeled contact sheets from the gallery renders.

Reads picked.tsv (base, ver, strips, verts, tris, bmin, bmax, path) and the
PNGs in renders/, tiles them into dark grids with a caption (filename / verts /
tris) under each, and writes gallery_NN.png.
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
RENDERS = os.path.join(HERE, "renders")
OUT = os.path.join(HERE, "out")
os.makedirs(OUT, exist_ok=True)

BG    = (24, 27, 33)      # matches renderer background
PANEL = (33, 37, 45)
INK   = (223, 226, 232)
DIM   = (150, 157, 170)
ACCENT= (120, 180, 235)

FONT_DIR = "/usr/share/fonts/truetype/dejavu"
def font(name, sz):
    return ImageFont.truetype(os.path.join(FONT_DIR, name), sz)
f_title = font("DejaVuSans-Bold.ttf", 34)
f_sub   = font("DejaVuSans.ttf", 18)
f_name  = font("DejaVuSansMono.ttf", 18)
f_meta  = font("DejaVuSans.ttf", 15)

# ---- load inventory ----
rows = []
with open(os.path.join(HERE, "picked.tsv")) as fh:
    for ln in fh:
        c = ln.rstrip("\n").split("\t")
        if len(c) < 5: continue
        base, ver, strips, verts, tris = c[0], c[1], c[2], c[3], c[4]
        png = os.path.join(RENDERS, base + ".png")
        if os.path.exists(png):
            rows.append(dict(base=base, ver=ver, strips=int(strips),
                             verts=int(verts), tris=int(tris), png=png))
# sort small -> large so sheets read as a progression
rows.sort(key=lambda r: r["tris"])

COLS = 4
PER_SHEET = 16          # 4x4
THUMB = 330             # render drawn at this size
CELL_W = THUMB + 26
CAP_H  = 54
CELL_H = THUMB + CAP_H
MARGIN = 34
HEADER = 112
GAP    = 16

def make_sheet(chunk, idx, total_sheets, nmodels_total):
    nrows = (len(chunk) + COLS - 1) // COLS
    W = MARGIN*2 + COLS*CELL_W + (COLS-1)*GAP
    H = HEADER + MARGIN + nrows*CELL_H + (nrows-1)*GAP + MARGIN
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    # header
    d.text((MARGIN, 30), "Dirge of Cerberus — BMD world geometry", font=f_title, fill=INK)
    sub = ("Native PC port · our own C loader + software rasterizer · zero emulation   "
           f"|   contact sheet {idx}/{total_sheets}   ·   {nmodels_total} BMD v16 models, "
           "shaded, auto-framed 3/4 view")
    d.text((MARGIN, 74), sub, font=f_sub, fill=DIM)
    d.line((MARGIN, HEADER-6, W-MARGIN, HEADER-6), fill=(55,61,72), width=2)

    for i, r in enumerate(chunk):
        cx = MARGIN + (i % COLS)*(CELL_W + GAP)
        cy = HEADER + MARGIN + (i // COLS)*(CELL_H + GAP)
        # panel
        d.rectangle((cx, cy, cx+CELL_W, cy+CELL_H), fill=PANEL)
        # render thumb
        th = Image.open(r["png"]).convert("RGB").resize((THUMB, THUMB), Image.LANCZOS)
        img.paste(th, (cx+13, cy+10))
        # caption
        ty = cy + 10 + THUMB + 6
        d.text((cx+14, ty), r["base"], font=f_name, fill=ACCENT)
        meta = f"{r['verts']:,} verts   ·   {r['tris']:,} tris   ·   {r['strips']} strips"
        d.text((cx+14, ty+24), meta, font=f_meta, fill=DIM)
    outp = os.path.join(OUT, f"gallery_{idx:02d}.png")
    img.save(outp)
    print("wrote", outp, img.size, "tiles", len(chunk))
    return outp

chunks = [rows[i:i+PER_SHEET] for i in range(0, len(rows), PER_SHEET)]
total = len(chunks)
outs = []
for i, ch in enumerate(chunks, 1):
    outs.append(make_sheet(ch, i, total, len(rows)))
print("SHEETS:", len(outs), "MODELS:", len(rows))
