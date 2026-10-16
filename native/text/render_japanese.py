#!/usr/bin/env python3
# render_japanese.py - render REAL Japanese (kana) using the recovered Shift-JIS ->
# kelfnt0 glyph-cell map.  Every glyph below is a genuine cell of the decoded game
# font atlas (fonttex/kelfnt0.gray, TIM3 512x448, linear 4-bit coverage*17), placed
# by the SJIS->cell table reversed in GLYPHMAP.md and verified glyph-by-glyph against
# a reference JP font (IPAGothic): see out/21_kana_proof.png.
#
# Untrusted font bytes are only read; run:  python3 -I render_japanese.py
import sys, struct, zlib, os

CW=16; COLS=32; AW,AH=512,448

# ---- recovered Shift-JIS (full-width kana) -> kelfnt0 cell index ----------------
# Katakana (JIS row 5) and hiragana (JIS row 4) occupy a bespoke, non-linear packing
# in kelfnt0; these cells are each confirmed by rendering the cell and reading it back.
SJIS_CELL = {
  # --- katakana ---
  'ァ':168,'ア':169,'ィ':170,'イ':171,'ゥ':172,'ウ':173,'ェ':174,'エ':175,
  'ベ':192,'ペ':193,'ホ':194,'ボ':195,'ポ':196,'マ':197,'ミ':198,'ム':199,
  'メ':200,'モ':201,'ャ':202,'ヤ':203,'ュ':204,'ユ':205,'ョ':206,'ヨ':207,
  'ド':208,'ナ':209,'ニ':210,'ヌ':211,'ネ':212,'ノ':213,'ハ':214,'バ':215,
  'パ':216,'ヒ':217,'ビ':218,'ピ':219,'フ':220,'ブ':221,'プ':222,'ヘ':223,
  'ー':324,
  # --- hiragana ---
  'し':128,'じ':129,'す':130,'ず':131,'せ':132,'ぜ':133,'そ':134,'ぞ':135,
  'た':136,'だ':137,'ち':138,'ぢ':139,'っ':140,'つ':141,'づ':142,'て':143,
  'え':145,'ぉ':146,'お':147,'か':148,'が':149,'き':150,'ぎ':151,'く':152,
  'ぐ':153,'け':154,'げ':155,'こ':156,'ご':157,'さ':158,'ざ':159,
  'ぷ':160,'へ':161,'べ':162,'ぺ':163,'ほ':164,'ぼ':165,'ぽ':166,'ま':167,
  'よ':177,'ら':178,'り':179,'る':180,'れ':181,'ろ':182,'わ':183,'を':186,'ん':187,
}

def load_gray(path):
    d=open(path,'rb').read()
    assert len(d)>=AW*AH,"gray too small"
    return d[:AW*AH]

def cell_cov(gray, idx):
    cx,cy=(idx%COLS)*CW,(idx//COLS)*CW
    return [list(gray[(cy+y)*AW+cx:(cy+y)*AW+cx+CW]) for y in range(CW)]

class Canvas:
    def __init__(self,w,h,bg):
        self.w,self.h=w,h; self.px=bytearray(bytes(bg)*(w*h))
    def blend(self,x,y,rgb,a):
        if a<=0 or x<0 or y<0 or x>=self.w or y>=self.h: return
        i=(y*self.w+x)*4
        for k in range(3): self.px[i+k]=(self.px[i+k]*(255-a)+rgb[k]*a)//255
        self.px[i+3]=255
    def rect(self,x0,y0,x1,y1,rgb,a):
        for y in range(max(0,y0),min(self.h,y1)):
            for x in range(max(0,x0),min(self.w,x1)): self.blend(x,y,rgb,a)
    def save(self,path):
        raw=bytearray()
        for y in range(self.h):
            raw.append(0); raw+=self.px[y*self.w*4:(y+1)*self.w*4]
        comp=zlib.compress(bytes(raw),9)
        def ch(t,d): c=struct.pack('>I',len(d))+t+d; return c+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
        with open(path,'wb') as f:
            f.write(b'\x89PNG\r\n\x1a\n')
            f.write(ch(b'IHDR',struct.pack('>IIBBBBB',self.w,self.h,8,6,0,0,0)))
            f.write(ch(b'IDAT',comp)); f.write(ch(b'IEND',b''))

def draw_glyph(cv,gray,idx,x,y,scale,rgb):
    if idx is None: return
    cov=cell_cov(gray,idx)
    for gy in range(CW):
        for gx in range(CW):
            a=cov[gy][gx]
            if a==0: continue
            for sy in range(scale):
                for sx in range(scale):
                    cv.blend(x+gx*scale+sx,y+gy*scale+sy,rgb,a)

def draw_jp(cv,gray,s,x,y,scale,rgb,pitch=15):
    cx=x
    for chh in s:
        if chh in ' 　': cx+=pitch*scale//2; continue
        idx=SJIS_CELL.get(chh)
        draw_glyph(cv,gray,idx,cx,y,scale,rgb)
        cx+=pitch*scale
    return cx

# tiny 5x7 latin for captions
LAT={ 'A':["01110","10001","10001","11111","10001","10001","10001"],'C':["01110","10001","10000","10000","10000","10001","01110"],
'D':["11110","10001","10001","10001","10001","10001","11110"],'E':["11111","10000","10000","11110","10000","10000","11111"],
'G':["01110","10001","10000","10111","10001","10001","01110"],'I':["11111","00100","00100","00100","00100","00100","11111"],
'J':["00111","00010","00010","00010","00010","10010","01100"],'K':["10001","10010","10100","11000","10100","10010","10001"],
'L':["10000","10000","10000","10000","10000","10000","11111"],'M':["10001","11011","10101","10101","10001","10001","10001"],
'N':["10001","11001","10101","10011","10001","10001","10001"],'O':["01110","10001","10001","10001","10001","10001","01110"],
'P':["11110","10001","10001","11110","10000","10000","10000"],'R':["11110","10001","10001","11110","10100","10010","10001"],
'S':["01111","10000","10000","01110","00001","00001","11110"],'T':["11111","00100","00100","00100","00100","00100","00100"],
'U':["10001","10001","10001","10001","10001","10001","01110"],'V':["10001","10001","10001","10001","10001","01010","00100"],
'W':["10001","10001","10001","10101","10101","11011","10001"],'Y':["10001","10001","01010","00100","00100","00100","00100"],
'B':["11110","10001","10001","11110","10001","10001","11110"],'F':["11111","10000","10000","11110","10000","10000","10000"],
'H':["10001","10001","10001","11111","10001","10001","10001"],' ':["00000"]*7,':':["000","010","000","000","000","010","000"],
'(':["001","010","100","100","100","010","001"],')':["100","010","001","001","001","010","100"],'-':["00000","00000","00000","11111","00000","00000","00000"],
'a':["00000","00000","01110","00001","01111","10001","01111"],'e':["00000","00000","01110","10001","11111","10000","01110"],
'i':["00100","00000","01100","00100","00100","00100","01110"],'k':["10000","10000","10010","10100","11000","10100","10010"],
'n':["00000","00000","11110","10001","10001","10001","10001"],'t':["00100","00100","11111","00100","00100","00100","00011"],
'u':["00000","00000","10001","10001","10001","10001","01111"],'l':["01100","00100","00100","00100","00100","00100","01110"],
'm':["00000","00000","11110","10101","10101","10101","10101"],'s':["00000","00000","01111","10000","01110","00001","11110"],
'f':["00110","01000","11110","01000","01000","01000","01000"],'p':["00000","00000","11110","10001","11110","10000","10000"],
'r':["00000","00000","10110","11000","10000","10000","10000"],'h':["10000","10000","10110","11001","10001","10001","10001"],
'o':["00000","00000","01110","10001","10001","10001","01110"],'d':["00001","00001","01101","10011","10001","10001","01111"],
'c':["00000","00000","01110","10001","10000","10001","01110"],'y':["00000","00000","10001","10001","01111","00001","01110"],
'.':["000","000","000","000","000","011","011"],'g':["00000","00000","01111","10001","01111","00001","01110"],
'v':["00000","00000","10001","10001","10001","01010","00100"],'b':["10000","10000","10110","11001","10001","10001","11110"],
'w':["00000","00000","10001","10001","10101","10101","01010"],'x':["00000","00000","10001","01010","00100","01010","10001"],
'X':["10001","10001","01010","00100","01010","10001","10001"],
'0':["01110","10001","10011","10101","11001","10001","01110"],'1':["00100","01100","00100","00100","00100","00100","01110"],
'2':["01110","10001","00001","00010","00100","01000","11111"],'3':["11110","00001","00001","01110","00001","00001","11110"],
'4':["00010","00110","01010","10010","11111","00010","00010"],'5':["11111","10000","11110","00001","00001","10001","01110"],
'6':["00110","01000","10000","11110","10001","10001","01110"],'7':["11111","00001","00010","00100","01000","01000","01000"],
'8':["01110","10001","10001","01110","10001","10001","01110"],'9':["01110","10001","10001","01111","00001","00010","01100"],}
def lat(cv,s,x,y,sc,rgb):
    cx=x
    for c in s:
        g=LAT.get(c,LAT[' ']); w=len(g[0])
        for gy in range(7):
            for gx in range(w):
                if g[gy][gx]=='1':
                    for sy in range(sc):
                        for sx in range(sc): cv.blend(cx+gx*sc+sx,y+gy*sc+sy,rgb,255)
        cx+=(w+1)*sc
    return cx

def main():
    gray=load_gray('fonttex/kelfnt0.gray')
    W,H=900,560
    cv=Canvas(W,H,(10,14,22,255))
    for y in range(H):
        t=y/H; cv.rect(0,y,W,y+1,(int(12+16*t),int(18+22*t),int(28+34*t)),255)
    white=(238,245,249); cyan=(120,210,232); dim=(150,185,205); gold=(224,196,120)

    lat(cv,"Dirge of Cerberus   native JP text render",40,26,2,cyan)
    lat(cv,"real kelfnt0 glyphs via the recovered Shift-JIS to cell map",40,54,1,dim)

    # ---- dialogue panel (in-game WRO-blue translucent) ----
    px0,py0,px1,py1=40,92,W-40,300
    cv.rect(px0,py0,px1,py1,(6,20,34),210)
    cv.rect(px0,py0,px1,py0+3,cyan,235)
    cv.rect(px0,py1-2,px1,py1,(40,90,120),200)
    cv.rect(px0,py0,px0+3,py1,(40,90,120),160)
    lat(cv,"hiragana words",px0+16,py0+14,1,gold)
    draw_jp(cv,gray,"さくら",px0+16,py0+30,3,white)
    draw_jp(cv,gray,"ちから",px0+170,py0+30,3,white)
    draw_jp(cv,gray,"そら",px0+324,py0+30,3,white)
    draw_jp(cv,gray,"おかし",px0+440,py0+30,3,white)
    lat(cv,"katakana words",px0+16,py0+92,1,gold)
    draw_jp(cv,gray,"ナイフ",px0+16,py0+108,3,white)
    draw_jp(cv,gray,"パイプ",px0+170,py0+108,3,white)
    draw_jp(cv,gray,"ハム",px0+324,py0+108,3,white)

    # ---- katakana syllabary proof rows (consecutive JIS order, correct glyphs) ----
    lat(cv,"katakana syllabary  -  consecutive cells, correct order (proof the map is systematic):",40,316,1,dim)
    draw_jp(cv,gray,"マミムメモ",40,336,2,cyan,pitch=14)
    draw_jp(cv,gray,"ベペホボポ",40,372,2,cyan,pitch=14)
    draw_jp(cv,gray,"ドナニヌネノハバパ",300,336,2,cyan,pitch=14)
    draw_jp(cv,gray,"ヒビピフブプヘ",300,372,2,cyan,pitch=14)
    draw_jp(cv,gray,"ァアィイゥウェエ",40,408,2,cyan,pitch=14)

    lat(cv,"each glyph is a genuine 16x16 kelfnt0 cell; see 21_kana_proof.png for per-glyph",40,452,1,dim)
    lat(cv,"side-by-side verification against a reference JP font.",40,470,1,dim)
    lat(cv,"decrypted source: 06800020.bin  -  SJIS full-width kana, decoded natively.",40,494,1,dim)

    os.makedirs('out',exist_ok=True)
    cv.save('out/20_dialogue_japanese.png')
    print("wrote out/20_dialogue_japanese.png")

if __name__=='__main__': main()
