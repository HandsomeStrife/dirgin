/*
 * docfont.c - Dirge of Cerberus (FFVII) font decoder (native C, self-contained).
 *
 * The game fonts live in two RFD "fontdatarfd" containers in the extracted tree:
 *   data/etc/fontdata1.bin  -> kelfnt0..kelfnt3  (TIM3, 512x448, PSMT4, 16-colour CLUT)
 *   data/etc/fontdata2.rfd  -> kelfnt4           (TIM4, 512x256, PSMT4, 16-colour CLUT)
 * (per-locale copies under data/etc/locale/<xx>/ are byte-identical for several langs:
 *  the glyph atlas is the shared Japanese font; Western text uses its half-width cells.)
 *
 * Each kelfnt is a PS2 TIM2-family texture whose 4-bit image is an ALPHA-COVERAGE mask
 * (RGB of the CLUT is white/graded; the index drives alpha). Glyphs sit on a fixed
 * 16x16 cell grid (32 cols). The 4-bit payload is GS-swizzled: it is the linear source
 * of a GS host->local PSMCT32 BITBLTBUF write into a width-(w/2) buffer, then sampled as
 * PSMT4 with TBW=w. Deswizzling = invert that composition (canonical PCSX2 block/column
 * tables, transcribed not invented; same model proven on the game's RST textures).
 *
 * STATUS: kelfnt4 (TIM4, 512x256 = two full 128px GS pages) deswizzles cleanly to real
 * kanji glyphs. kelfnt0..3 (TIM3, 512x448) include a half-height GS page (448 = 3.5
 * pages) whose CT32/T4 partial-page footprints do not coincide, so ~7% (one page) cannot
 * be placed by this model and the output stays scrambled -- a documented open gap
 * (same "odd geometry" caveat the RST decoder notes). This tool therefore fully extracts
 * kelfnt4 and reports the gap for the TIM3 pages.
 *
 * Usage: docfont <fontdata.(bin|rfd)> <outdir>
 *   writes <outdir>/<name>.png (RGBA, glyph in alpha) and <name>.gray (raw 8-bit alpha,
 *   width*height row-major) for every kelfnt it can place, and prints a report.
 *
 * Untrusted input: bytes are only read (pass paths as args; run under python -I for the
 * helper scripts). Depends only on zlib (for PNG deflate).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>

/* ---------- canonical PS2 GS swizzle tables (PCSX2) ---------- */
static const int ct4_block[8][4]={{0,2,8,10},{1,3,9,11},{4,6,12,14},{5,7,13,15},
    {16,18,24,26},{17,19,25,27},{20,22,28,30},{21,23,29,31}};
static const int ct32_block[4][8]={{0,1,4,5,16,17,20,21},{2,3,6,7,18,19,22,23},
    {8,9,12,13,24,25,28,29},{10,11,14,15,26,27,30,31}};
static const int ct32_col[8][8]={{0,1,4,5,8,9,12,13},{2,3,6,7,10,11,14,15},
    {16,17,20,21,24,25,28,29},{18,19,22,23,26,27,30,31},{32,33,36,37,40,41,44,45},
    {34,35,38,39,42,43,46,47},{48,49,52,53,56,57,60,61},{50,51,54,55,58,59,62,63}};
static const int ct4_col[16][32]={
 {0,8,32,40,64,72,96,104,2,10,34,42,66,74,98,106,4,12,36,44,68,76,100,108,6,14,38,46,70,78,102,110},
 {16,24,48,56,80,88,112,120,18,26,50,58,82,90,114,122,20,28,52,60,84,92,116,124,22,30,54,62,86,94,118,126},
 {65,73,97,105,1,9,33,41,67,75,99,107,3,11,35,43,69,77,101,109,5,13,37,45,71,79,103,111,7,15,39,47},
 {81,89,113,121,17,25,49,57,83,91,115,123,19,27,51,59,85,93,117,125,21,29,53,61,87,95,119,127,23,31,55,63},
 {192,200,224,232,128,136,160,168,194,202,226,234,130,138,162,170,196,204,228,236,132,140,164,172,198,206,230,238,134,142,166,174},
 {208,216,240,248,144,152,176,184,210,218,242,250,146,154,178,186,212,220,244,252,148,156,180,188,214,222,246,254,150,158,182,190},
 {129,137,161,169,193,201,225,233,131,139,163,171,195,203,227,235,133,141,165,173,197,205,229,237,135,143,167,175,199,207,231,239},
 {145,153,177,185,209,217,241,249,147,155,179,187,211,219,243,251,149,157,181,189,213,221,245,253,151,159,183,191,215,223,247,255},
 {256,264,288,296,320,328,352,360,258,266,290,298,322,330,354,362,260,268,292,300,324,332,356,364,262,270,294,302,326,334,358,366},
 {272,280,304,312,336,344,368,376,274,282,306,314,338,346,370,378,276,284,308,316,340,348,372,380,278,286,310,318,342,350,374,382},
 {321,329,353,361,257,265,289,297,323,331,355,363,259,267,291,299,325,333,357,365,261,269,293,301,327,335,359,367,263,271,295,303},
 {337,345,369,377,273,281,305,313,339,347,371,379,275,283,307,315,341,349,373,381,277,285,309,317,343,351,375,383,279,287,311,319},
 {448,456,480,488,384,392,416,424,450,458,482,490,386,394,418,426,452,460,484,492,388,396,420,428,454,462,486,494,390,398,422,430},
 {464,472,496,504,400,408,432,440,466,474,498,506,402,410,434,442,468,476,500,508,404,412,436,444,470,478,502,510,406,414,438,446},
 {385,393,417,425,449,457,481,489,387,395,419,427,451,459,483,491,389,397,421,429,453,461,485,493,391,399,423,431,455,463,487,495},
 {401,409,433,441,465,473,497,505,403,411,435,443,467,475,499,507,405,413,437,445,469,477,501,509,407,415,439,447,471,479,503,511}};

static int gs_addr32(int x,int y,int dbw){int pw=dbw/64;if(pw<1)pw=1;int page=(x/64)+(y/32)*pw;
    return page*2048+ct32_block[(y%32)/8][(x%64)/8]*64+ct32_col[(y%32)%8][(x%64)%8];}
static int gs_addr4(int x,int y,int tbw){int pw=(tbw+127)/128;if(pw<1)pw=1;int page=(x/128)+(y/128)*pw;
    return page*16384+ct4_block[(y%128)/16][(x%128)/32]*512+ct4_col[(y%128)%16][(x%128)%32];}

/* Decode a PSMT4 payload stored LINEARLY (row-major scanline, 2 nibbles/byte) to
 * w*h coverage bytes.  The 4-bit index IS the alpha coverage (index*17 -> 0..255),
 * which is how the HUD samples these alpha-mask glyphs.  fontdata1.bin's kelfnt0..3
 * (TIM3, 512x448) are stored this way -- NOT GS-swizzled.  (This replaces the prior
 * swizzle model for the TIM3 fonts, which left a scrambled "odd-geometry" page: the
 * data is simply a linear 4-bit image, and decodes with zero holes.)  Returns 0. */
static int linear4_alpha(uint8_t *alpha, const uint8_t *src, int w, int h)
{
    long n = (long)w * h;
    for (long i = 0; i < n; i++) {
        uint8_t b = src[i >> 1];
        int v = (i & 1) ? (b >> 4) : (b & 0xf);
        alpha[i] = (uint8_t)(v * 17);
    }
    return 0;
}

/* Deswizzle a PSMT4 payload (w*h/2 packed nibbles) to w*h alpha bytes (index*17).
 * Returns number of holes (unplaced pixels); 0 == fully placed. */
static int deswizzle4_alpha(uint8_t *alpha, const uint8_t *src, int w, int h,
                            const uint8_t clut_alpha[16])
{
    int Wc = w / 2;                 /* CT32 buffer width in pixels (h>=128) */
    long words = (long)w * h / 8;
    int Hc = (int)(words / Wc);
    int pages = ((w+127)/128) * ((h+127)/128);
    long invn = (long)pages * 2048;
    int *inv = (int*)malloc(sizeof(int)*(size_t)invn);
    for (long i=0;i<invn;i++) inv[i]=-1;
    for (int ty=0;ty<Hc;ty++) for (int tx=0;tx<Wc;tx++){
        int wd = gs_addr32(tx,ty,Wc);
        if (wd>=0 && wd<invn) inv[wd]=ty*Wc+tx;
    }
    int holes=0;
    for (int y=0;y<h;y++) for (int x=0;x<w;x++){
        int na=gs_addr4(x,y,w), wd=na>>3;
        int i=(wd>=0&&wd<invn)?inv[wd]:-1, v=0;
        if (i>=0){ long fn=((long)i<<3)+(na&7); uint8_t b=src[fn>>1]; v=(fn&1)?(b>>4):(b&0xf); }
        else holes++;
        alpha[y*w+x] = clut_alpha[v & 15];
    }
    free(inv);
    return holes;
}

/* ---------- minimal PNG writer (RGBA, via zlib) ---------- */
static void put32(uint8_t *p, uint32_t v){ p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v; }
static uint32_t crc_table[256]; static int crc_init=0;
static void mkcrc(void){ for(int n=0;n<256;n++){uint32_t c=n;for(int k=0;k<8;k++)c=(c&1)?0xedb88320u^(c>>1):(c>>1);crc_table[n]=c;} crc_init=1; }
static uint32_t crc32b(const uint8_t*d,size_t n){ if(!crc_init)mkcrc(); uint32_t c=0xffffffffu; for(size_t i=0;i<n;i++)c=crc_table[(c^d[i])&0xff]^(c>>8); return c^0xffffffffu; }
static void chunk(FILE*f,const char*type,const uint8_t*data,size_t len){
    uint8_t hdr[8]; put32(hdr,(uint32_t)len); memcpy(hdr+4,type,4); fwrite(hdr,1,8,f);
    if(len)fwrite(data,1,len,f);
    uint8_t*tmp=(uint8_t*)malloc(len+4); memcpy(tmp,type,4); if(len)memcpy(tmp+4,data,len);
    uint8_t c[4]; put32(c,crc32b(tmp,len+4)); fwrite(c,1,4,f); free(tmp);
}
static int write_png_rgba(const char*path,const uint8_t*rgba,int w,int h){
    FILE*f=fopen(path,"wb"); if(!f)return -1;
    uint8_t sig[8]={137,80,78,71,13,10,26,10}; fwrite(sig,1,8,f);
    uint8_t ih[13]; put32(ih,w); put32(ih+4,h); ih[8]=8; ih[9]=6; ih[10]=0; ih[11]=0; ih[12]=0;
    chunk(f,"IHDR",ih,13);
    size_t raw=(size_t)h*(1+(size_t)w*4); uint8_t*rb=(uint8_t*)malloc(raw); size_t o=0;
    for(int y=0;y<h;y++){ rb[o++]=0; memcpy(rb+o,rgba+(size_t)y*w*4,(size_t)w*4); o+=(size_t)w*4; }
    uLongf cl=compressBound(raw); uint8_t*cb=(uint8_t*)malloc(cl);
    compress2(cb,&cl,rb,raw,9); chunk(f,"IDAT",cb,cl); chunk(f,"IEND",NULL,0);
    free(rb);free(cb);fclose(f); return 0;
}

static uint32_t rdu(const uint8_t*p){return (uint32_t)p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24);}
static uint16_t rdh(const uint8_t*p){return (uint16_t)(p[0]|(p[1]<<8));}

/* PS2 32-bit CLUT: CSM1 reorder (middle 8s swapped) only matters for 256-colour; for a
 * 16-colour font palette it is identity. Alpha is 0..0x80 (0x80=opaque) -> scale to 255.*/
static void read_clut16_alpha(const uint8_t*clut,uint8_t out_alpha[16]){
    for(int i=0;i<16;i++){ int a=clut[i*4+3]; int v=a*255/128; out_alpha[i]=(uint8_t)(v>255?255:v); }
}

int main(int argc,char**argv){
    if(argc<3){ fprintf(stderr,"usage: %s <fontdata.(bin|rfd)> <outdir>\n",argv[0]); return 2; }
    FILE*f=fopen(argv[1],"rb"); if(!f){perror("open");return 1;}
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t*d=(uint8_t*)malloc(n); if(fread(d,1,n,f)!=(size_t)n){return 1;} fclose(f);

    /* Walk every TIM2/3/4 picture in the container. */
    int found=0;
    for(long off=0; off+0x30<n; off++){
        if(!(d[off]=='T'&&d[off+1]=='I'&&d[off+2]=='M'&&(d[off+3]=='2'||d[off+3]=='3'||d[off+3]=='4'))) continue;
        int base=off+(d[off+5]==0?0x10:0x80);
        if(base+0x30>n) continue;
        uint32_t imageSize=rdu(d+base+8);
        uint16_t hdrSize=rdh(d+base+12), clutColors=rdh(d+base+14);
        uint8_t imgType=d[base+19];
        int W=rdh(d+base+20), H=rdh(d+base+22);
        if(imgType!=4 || W<=0||H<=0||W>4096||H>4096) continue;  /* only PSMT4 fonts here */
        size_t img_off=(size_t)base+hdrSize, clut_off=img_off+imageSize;
        if(img_off+(size_t)W*H/2>(size_t)n) continue;
        /* locate 16-colour CLUT: shift to payload end if the nominal slot is zero-filled */
        int nc=clutColors?clutColors:16; if(nc>16)nc=16;
        if(clut_off+(size_t)nc*4<=(size_t)n){
            int hz=1; for(size_t b=0;b<(size_t)nc*4;b++) if(d[clut_off+b]){hz=0;break;}
            if(hz) clut_off=(size_t)n-(size_t)nc*4;
        } else clut_off=(size_t)n-(size_t)nc*4;
        uint8_t calpha[16]={0}; read_clut16_alpha(d+clut_off,calpha);

        /* name: nearest preceding "kelfntN" tag in the container, else index */
        char name[32]; snprintf(name,sizeof name,"tex%d",found);
        for(long k=off;k>off-64&&k>=0;k--) if(!memcmp(d+k,"kelfnt",6)){ int j=0; while(j<15&&d[k+j]>=0x20&&d[k+j]<0x7f){name[j]=d[k+j];j++;} name[j]=0; break; }

        uint8_t*alpha=(uint8_t*)malloc((size_t)W*H);
        /* TIM3 fonts (fontdata1.bin -> kelfnt0..3) are stored LINEAR; TIM4
         * (fontdata2.rfd -> kelfnt4) is GS-swizzled.  Branch on the TIM version. */
        int linear = (d[off+3]=='3');
        int holes = linear ? linear4_alpha(alpha,d+img_off,W,H)
                           : deswizzle4_alpha(alpha,d+img_off,W,H,calpha);

        uint8_t*rgba=(uint8_t*)malloc((size_t)W*H*4);
        for(int i=0;i<W*H;i++){ rgba[i*4]=rgba[i*4+1]=rgba[i*4+2]=255; rgba[i*4+3]=alpha[i]; }
        char p1[1024],p2[1024];
        snprintf(p1,sizeof p1,"%s/%s.png",argv[2],name);
        snprintf(p2,sizeof p2,"%s/%s.gray",argv[2],name);
        write_png_rgba(p1,rgba,W,H);
        FILE*g=fopen(p2,"wb"); if(g){fwrite(alpha,1,(size_t)W*H,g);fclose(g);}
        double pct=100.0*holes/((double)W*H);
        printf("%-9s TIM%c %dx%d PSMT4 cells=%dx%d holes=%d (%.1f%%) %s %s -> %s\n",
               name,d[off+3],W,H,W/16,H/16,holes,pct,
               linear?"LINEAR":"SWIZZLED",
               holes==0?"CLEAN":"PARTIAL(odd-geometry gap)",p1);
        free(alpha);free(rgba);
        found++;
        off=(long)img_off; /* skip past header to avoid re-matching */
    }
    if(!found) fprintf(stderr,"no PSMT4 font textures found in %s\n",argv[1]);
    free(d);
    return 0;
}
