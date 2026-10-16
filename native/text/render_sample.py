#!/usr/bin/env python3
"""
render_sample.py - assemble the Dirge-of-Cerberus text/font hero image.

Combines, in one PNG:
  (A) real strings decrypted by ./doctext from encrypted text-bin files (Shift-JIS),
      rendered with a fallback face (IPAGothic) for the glyph SHAPES -- clearly
      labelled, because the game's internal Shift-JIS -> glyph-cell index table lives
      in EE code and is not reversed here;
  (B) a strip of the REAL game font: kelfnt4 glyphs decoded natively by ./docfont and
      composited by ./docrender (fonttex/real_font_line.png).

Run:  python3 -I render_sample.py <real_font_line.png> <out.png>
(Only reads asset bytes; all decryption/decoding already done by the native C tools.)
"""
import sys, re
from PIL import Image, ImageDraw, ImageFont

JP = "/usr/share/fonts/opentype/ipafont-gothic/ipagp.ttf"
MONO = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

def clean(s):
    """Turn in-game control codes into readable text."""
    s = s.replace("$r", "\n")
    s = re.sub(r"\$i([a-z0-9]{2})\$?", lambda m: "["+m.group(1).upper()+"]", s)  # button icons
    s = re.sub(r"\$c[0-9a-fA-F]{2}", "", s)   # colour codes
    s = re.sub(r"\$x[0-9]+", "  ", s)          # tab stops
    s = re.sub(r"\$[a-z]", "", s)
    return s.strip()

# real decrypted lines (verbatim from ./doctext output, then control-codes cleaned)
EN = clean("This ability is useful for moving in$rtight areas. Through training, you$rmay acquire the rush technique.")
JP1 = "中断セーブデータ:カームの街"                       # 06800020.bin  (save-slot label)
JP2 = clean("攻撃で吹き飛ばされた時、地面に落ちる$rタイミングに合わせて以下のキー操作を$rすると、転倒を回避できます")  # 0c7ee1c8.bin

def main():
    strip_path, out = sys.argv[1], sys.argv[2]
    W, H = 900, 772
    img = Image.new("RGB", (W, H), (14, 17, 30))
    d = ImageDraw.Draw(img)
    fh = ImageFont.truetype(MONO, 26)
    fl = ImageFont.truetype(MONO, 16)
    fb = ImageFont.truetype(MONO, 19)
    fj = ImageFont.truetype(JP, 22)
    fjb = ImageFont.truetype(JP, 26)
    gold = (210, 180, 120); grey = (150, 160, 180); white = (235, 238, 245)
    cyan = (120, 200, 230)

    y = 24
    d.text((30, y), "Dirge of Cerberus (FFVII) — native text & font pipeline", font=fh, fill=gold); y += 44
    d.text((30, y), "text-bin decrypt + string table (Shift-JIS) · kelfnt font decode · native render",
           font=fl, fill=grey); y += 36

    # Panel A: decrypted strings
    d.rectangle((24, y, W-24, y+348), outline=(60,70,100), width=2, fill=(20,25,42));
    ay = y + 14
    d.text((38, ay), "DECRYPTED GAME STRINGS  (native ./doctext — real bytes)", font=fl, fill=cyan); ay += 28
    d.text((38, ay), "JP save label  06800020.bin:", font=fl, fill=grey); ay += 22
    d.text((54, ay), JP1, font=fjb, fill=white); ay += 40
    d.text((38, ay), "JP tutorial   0c7ee1c8.bin:", font=fl, fill=grey); ay += 22
    for ln in JP2.split("\n"):
        d.text((54, ay), ln, font=fj, fill=white); ay += 28
    ay += 6
    d.text((38, ay), "EN tutorial   0c7ee100.bin:", font=fl, fill=grey); ay += 22
    for ln in EN.split("\n"):
        d.text((54, ay), ln, font=fb, fill=white); ay += 24
    d.text((38, y+348-22), "(glyph shapes via fallback face IPAGothic — the game's SJIS->cell index map is not reversed here)",
           font=ImageFont.truetype(MONO,12), fill=(120,110,90))
    y += 348 + 24

    # Panel B: REAL game glyphs
    d.text((38, y), "REAL GAME FONT — kelfnt4 glyphs, natively decoded (./docfont) + rendered (./docrender):",
           font=fl, fill=cyan); y += 28
    strip = Image.open(strip_path).convert("RGB")
    # scale strip to fit width
    sw = W - 120
    sh = int(strip.height * sw / strip.width)
    strip = strip.resize((sw, sh), Image.NEAREST)
    img.paste(strip, (60, y)); y += sh + 10
    d.text((60, y), "TIM4 512x256 PSMT4 16-colour CLUT, GS CT32-composition deswizzle, 16x16 cell grid — genuine in-game bitmaps.",
           font=ImageFont.truetype(MONO,12), fill=(120,110,90))

    img.save(out)
    print("wrote", out, img.size)

main()
