/*
 * docrender.c - native C text renderer for Dirge of Cerberus.
 *
 * Composites REAL game-font glyphs (decoded by docfont.c from kelfnt4) into an
 * in-game-style dialogue box PNG. Input is the raw 8-bit alpha atlas (<name>.gray,
 * width*height, one coverage byte per pixel) plus a list of glyph CELL indices
 * (row-major, 16x16 cells). Each cell's alpha is alpha-blended as dark ink onto a
 * translucent panel, exactly as the GS blends the PSMT4 coverage mask at runtime.
 *
 * This is the "real font" path: every glyph drawn is a genuine kelfnt4 bitmap, not a
 * substitute. (Mapping an arbitrary Shift-JIS string to its cell index needs the game's
 * internal glyph-index table, which lives in EE code and is not reversed here; so the
 * caller supplies cell indices. The companion render_sample.py renders decrypted strings
 * with a clearly-labelled fallback face for comparison.)
 *
 * Usage: docrender <atlas.gray> <W> <H> <cell> <csv-of-cell-indices> <out.png> [title...]
 * Depends only on zlib.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>

static void put32(uint8_t*p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static uint32_t ct[256]; static int ci=0;
static void mkc(void){for(int n=0;n<256;n++){uint32_t c=n;for(int k=0;k<8;k++)c=(c&1)?0xedb88320u^(c>>1):(c>>1);ct[n]=c;}ci=1;}
static uint32_t crc(const uint8_t*d,size_t n){if(!ci)mkc();uint32_t c=0xffffffffu;for(size_t i=0;i<n;i++)c=ct[(c^d[i])&0xff]^(c>>8);return c^0xffffffffu;}
static void chunk(FILE*f,const char*t,const uint8_t*d,size_t n){uint8_t h[8];put32(h,(uint32_t)n);memcpy(h+4,t,4);fwrite(h,1,8,f);if(n)fwrite(d,1,n,f);uint8_t*tm=malloc(n+4);memcpy(tm,t,4);if(n)memcpy(tm+4,d,n);uint8_t c[4];put32(c,crc(tm,n+4));fwrite(c,1,4,f);free(tm);}
static int write_png(const char*path,const uint8_t*rgba,int w,int h){
    FILE*f=fopen(path,"wb"); if(!f)return -1;
    uint8_t sig[8]={137,80,78,71,13,10,26,10}; fwrite(sig,1,8,f);
    uint8_t ih[13]; put32(ih,w);put32(ih+4,h);ih[8]=8;ih[9]=6;ih[10]=ih[11]=ih[12]=0; chunk(f,"IHDR",ih,13);
    size_t raw=(size_t)h*(1+(size_t)w*4); uint8_t*rb=malloc(raw); size_t o=0;
    for(int y=0;y<h;y++){rb[o++]=0;memcpy(rb+o,rgba+(size_t)y*w*4,(size_t)w*4);o+=(size_t)w*4;}
    uLongf cl=compressBound(raw); uint8_t*cb=malloc(cl); compress2(cb,&cl,rb,raw,9);
    chunk(f,"IDAT",cb,cl); chunk(f,"IEND",NULL,0); free(rb);free(cb);fclose(f); return 0;
}

int main(int argc,char**argv){
    if(argc<7){fprintf(stderr,"usage: %s <atlas.gray> <W> <H> <cell> <csv-cells> <out.png>\n",argv[0]);return 2;}
    const char*apath=argv[1]; int AW=atoi(argv[2]),AH=atoi(argv[3]),cell=atoi(argv[4]);
    const char*csv=argv[5]; const char*out=argv[6];
    FILE*f=fopen(apath,"rb"); if(!f){perror("atlas");return 1;}
    uint8_t*atlas=malloc((size_t)AW*AH); if(fread(atlas,1,(size_t)AW*AH,f)!=(size_t)AW*AH){return 1;} fclose(f);
    int cols=AW/cell;

    int idx[256],ni=0; { char buf[4096]; strncpy(buf,csv,sizeof buf-1); buf[sizeof buf-1]=0;
        char*p=strtok(buf,","); while(p&&ni<256){idx[ni++]=atoi(p);p=strtok(NULL,",");} }

    /* layout: scale glyphs x2, 4px tracking, padded box */
    int scale=2, gw=cell*scale, pad=24, track=6;
    int boxw=pad*2 + ni*gw + (ni-1)*track;
    int boxh=pad*2 + gw;
    if(boxw<360)boxw=360;
    uint8_t*img=malloc((size_t)boxw*boxh*4);
    /* in-game style: deep translucent navy panel with a lighter inner fill */
    for(int y=0;y<boxh;y++)for(int x=0;x<boxw;x++){
        int o=(y*boxw+x)*4; int border=(x<3||y<3||x>=boxw-3||y>=boxh-3);
        int inner=(x>=8&&y>=8&&x<boxw-8&&y<boxh-8);
        uint8_t r,g,b;
        if(border){r=180;g=200;b=235;}
        else if(inner){r=18;g=24;b=44;}
        else {r=40;g=52;b=86;}
        img[o]=r;img[o+1]=g;img[o+2]=b;img[o+3]=255;
    }
    /* draw each real glyph cell, alpha-blended as pale ink (as the HUD renders it) */
    int x0=pad, y0=pad;
    for(int k=0;k<ni;k++){
        int c=idx[k]; int cx=(c%cols)*cell, cy=(c/cols)*cell;
        for(int gy=0;gy<gw;gy++)for(int gx=0;gx<gw;gx++){
            int sx=cx+gx/scale, sy=cy+gy/scale;
            if(sx<0||sy<0||sx>=AW||sy>=AH)continue;
            int a=atlas[sy*AW+sx];
            int px=x0+k*(gw+track)+gx, py=y0+gy;
            if(px<0||py<0||px>=boxw||py>=boxh)continue;
            int o=(py*boxw+px)*4; float af=a/255.0f;
            /* ink colour: warm off-white glyphs */
            img[o]  =(uint8_t)(img[o]  *(1-af)+238*af);
            img[o+1]=(uint8_t)(img[o+1]*(1-af)+240*af);
            img[o+2]=(uint8_t)(img[o+2]*(1-af)+230*af);
        }
    }
    write_png(out,img,boxw,boxh);
    printf("rendered %d real kelfnt glyph(s) -> %s (%dx%d)\n",ni,out,boxw,boxh);
    free(atlas);free(img);
    return 0;
}
