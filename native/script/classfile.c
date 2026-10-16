/* classfile.c - see classfile.h. Decrypt + container parse ported 1:1 from
 * dcc.c (which cites the EE function addresses), plus a flat resolver. */
#include "classfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- scalar readers ---------------------------------------------------- */
uint64_t cf_rd_u64_le(const uint8_t *p){
    uint64_t v=0; for(int i=7;i>=0;i--) v=(v<<8)|p[i]; return v; }
static void wr_u64_le(uint8_t *p,uint64_t v){ for(int i=0;i<8;i++){p[i]=(uint8_t)v; v>>=8;} }
uint32_t cf_rd_u32_le(const uint8_t *p){
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static void wr_u32_le(uint8_t *p,uint32_t v){ p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
uint16_t cf_be16(const uint8_t *p){ return (uint16_t)((p[0]<<8)|p[1]); }
uint32_t cf_be32(const uint8_t *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

static inline uint8_t sbox(uint8_t i){ return (uint8_t)(i+120); } /* DAT_0047eb50 */

/* ====================================================================== */
/* CIPHER (game-custom block transform; FUN_003a8320/0041bb38/0041c010)    */
/* ====================================================================== */
typedef struct { uint8_t b[272]; } keybuf_t;
#define CTR_OFF 0x100
#define CSUM_OFF 0x104

static uint64_t build_key(const uint8_t *first8){
    uint64_t k=0; for(int i=0;i<8;i++) k=(k<<8)|first8[i]; return k; }
static void key_schedule(keybuf_t *K,uint64_t keyval){
    uint32_t hi=(uint32_t)(keyval>>32), lo=(uint32_t)keyval;
    uint64_t s0=((uint64_t)((lo<<8)|(lo>>24))<<32)|(uint64_t)((hi>>16)|(hi<<16));
    wr_u64_le(K->b,s0);
    K->b[0]=(uint8_t)(K->b[0]+0x45);
    for(int pos=1;pos<8;pos++){
        uint8_t b=K->b[pos], prev=K->b[pos-1];
        K->b[pos]=(uint8_t)(((uint8_t)(prev+(uint8_t)(b-0x2c)))^(uint8_t)(prev<<2)^0x45);
    }
    for(int i=0;i<31;i++) wr_u64_le(K->b+(i+1)*8, cf_rd_u64_le(K->b+i*8)*5ull);
    wr_u32_le(K->b+CTR_OFF,0); wr_u32_le(K->b+CSUM_OFF,0);
}
static void stage_byte(keybuf_t *K,const uint8_t *src,uint8_t *dst){
    uint32_t ctr=cf_rd_u32_le(K->b+CTR_OFF);
    uint8_t fb=(uint8_t)((ctr>>3)^0x45);
    uint32_t subbase=(uint32_t)(K->b[CTR_OFF]>>3)*8u;
    for(int i=0;i<8;i++){
        uint8_t c=src[i]; uint8_t t=(uint8_t)(fb^c);
        for(int k=0;k<8;k++) t=(uint8_t)(sbox(t)-K->b[subbase+k]);
        dst[i]=t; fb=c;
    }
}
static void stage_word(keybuf_t *K,uint8_t *dst){
    uint32_t ctr=cf_rd_u32_le(K->b+CTR_OFF); uint64_t u=(uint64_t)ctr;
    uint64_t v=cf_rd_u64_le(dst); uint32_t sidx=ctr&0xf8; uint64_t sk=cf_rd_u64_le(K->b+sidx);
    uint64_t mask=(u|(u<<30)|(u<<20)|(u<<10))+0xa1652347ull;
    v=(v-sk)^mask; v=v^sk; v=(v<<32)|(v>>32); wr_u64_le(dst,v);
}
static void decrypt_block(keybuf_t *K,const uint8_t *src,uint8_t *dst){
    stage_byte(K,src,dst); stage_word(K,dst);
    uint32_t ctr=cf_rd_u32_le(K->b+CTR_OFF)+8;
    uint32_t cs=cf_rd_u32_le(K->b+CSUM_OFF)+dst[0]+dst[4];
    wr_u32_le(K->b+CTR_OFF,ctr); wr_u32_le(K->b+CSUM_OFF,cs);
}
int cf_decrypt(uint8_t *buf,size_t len){
    if(len<16) return -1;
    size_t body=len-8; if(body%8) return -2;
    keybuf_t K; key_schedule(&K,build_key(buf));
    uint8_t *p=buf+8;
    for(size_t o=0;o+8<=body;o+=8) decrypt_block(&K,p+o,p+o);
    buf[0]=buf[1]=buf[2]=buf[3]=0x53; /* TTTT -> SSSS */
    return 0;
}

/* ====================================================================== */
/* CONTAINER accessors                                                     */
/* ====================================================================== */
uint16_t cf_u16(const cf_img *m,size_t o){ return (o+2<=m->n)? (uint16_t)(m->d[o]|(m->d[o+1]<<8)):0; }
uint32_t cf_u32(const cf_img *m,size_t o){ return (o+4<=m->n)? cf_rd_u32_le(m->d+o):0; }

const char *cf_cstr(const cf_img *m,uint32_t off,size_t *len){
    if(off==0||off>=m->n){ if(len)*len=0; return NULL; }
    size_t e=off; while(e<m->n && m->d[e]) e++;
    if(len)*len=e-off;
    return (const char*)(m->d+off);
}
#define CP_BASE(m)  cf_u32((m),0x18)
static size_t cp_entry(const cf_img *m,uint16_t i){ return (size_t)CP_BASE(m)+(size_t)i*0x10; }
uint8_t cf_cp_tag(const cf_img *m,uint16_t i){ size_t e=cp_entry(m,i); return (e<m->n)?m->d[e]:0; }
const char *cf_cp_utf(const cf_img *m,uint16_t i,size_t *len){
    if(cf_cp_tag(m,i)!=1){ if(len)*len=0; return NULL; }
    size_t e=cp_entry(m,i);
    return cf_cstr(m,cf_u32(m,e+0x0c),len);
}
const char *cf_cp_class(const cf_img *m,uint16_t i,size_t *len){
    if(cf_cp_tag(m,i)!=7){ if(len)*len=0; return NULL; }
    return cf_cstr(m,cf_u32(m,cp_entry(m,i)+0x08),len);
}
const char *cf_class_at_ptr(const cf_img *m,uint32_t p,size_t *len){
    if(p==0||p+8>m->n){ if(len)*len=0; return NULL; }
    if(m->d[p]!=7){ if(len)*len=0; return NULL; }
    return cf_cstr(m,cf_u32(m,p+0x08),len);
}

/* ====================================================================== */
/* RESOLVE into flat cf_class                                              */
/* ====================================================================== */
/* copy a (ptr,len) run into a fresh NUL-terminated C string (owned) */
static char *dupn(const char *s,size_t len){
    char *o=(char*)malloc(len+1); if(!o) return NULL;
    if(s&&len) memcpy(o,s,len);
    o[len]=0;
    return o;
}
static char *dupcp_utf(const cf_img *m,uint16_t idx){
    size_t l; const char *s=cf_cp_utf(m,idx,&l); return s?dupn(s,l):NULL;
}
static char *dupcp_class(const cf_img *m,uint16_t idx){
    size_t l; const char *s=cf_cp_class(m,idx,&l); return s?dupn(s,l):NULL;
}

int cf_load(const char *path, cf_class *out, const char **err){
    const char *dummy; if(!err) err=&dummy;
    memset(out,0,sizeof *out);

    FILE *f=fopen(path,"rb"); if(!f){ *err="cannot open file"; return -1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<=0){ fclose(f); *err="empty file"; return -1; }
    uint8_t *buf=(uint8_t*)malloc((size_t)sz);
    if(!buf||fread(buf,1,(size_t)sz,f)!=(size_t)sz){ fclose(f); free(buf); *err="read error"; return -1; }
    fclose(f);
    size_t len=(size_t)sz;

    if(cf_decrypt(buf,len)!=0){ free(buf); *err="decrypt failed"; return -2; }

    cf_img m={buf,len};
    if(!(buf[0]==0x53&&buf[1]==0x53&&buf[2]==0x53&&buf[3]==0x53)){ free(buf); *err="bad magic (not SSSS)"; return -3; }

    uint16_t cpc = cf_u16(&m,0x08);
    uint32_t cpbase = CP_BASE(&m);
    if(!(cpbase>=0x18 && cpbase<len && cpc>=1 && cpc<20000 &&
         (size_t)cpbase+(size_t)cpc*0x10<=len)){ free(buf); *err="bad constant pool"; return -4; }

    out->raw=buf; out->raw_len=len; out->cp_count=cpc;
    { size_t l; const char *s;
      s=cf_class_at_ptr(&m,cf_u32(&m,0x1c),&l); out->this_class  = s?dupn(s,l):NULL;
      s=cf_class_at_ptr(&m,cf_u32(&m,0x24),&l); out->super_class = s?dupn(s,l):NULL; }

    /* flat constant pool */
    out->cp=(cf_cp_item*)calloc(cpc,sizeof(cf_cp_item));
    if(!out->cp){ cf_free(out); *err="oom cp"; return -5; }
    for(uint16_t i=1;i<cpc;i++){
        size_t e=cp_entry(&m,i);
        uint8_t tag=cf_cp_tag(&m,i);
        cf_cp_item *it=&out->cp[i];
        it->kind=tag;
        switch(tag){
        case CF_CP_UTF8: {
            size_t l; const char *s=cf_cp_utf(&m,i,&l);
            it->u.utf.s = s?dupn(s,l):NULL; it->u.utf.len=l; break; }
        case CF_CP_INT:   it->u.i=(int32_t)cf_be32(m.d+e+8); break;
        case CF_CP_FLOAT: { uint32_t b=cf_be32(m.d+e+8); memcpy(&it->u.f,&b,4); break; }
        case CF_CP_LONG:  it->u.l=(int64_t)cf_rd_u64_le(m.d+e+8); break;
        case CF_CP_DOUBLE:{ uint64_t b=cf_rd_u64_le(m.d+e+8); memcpy(&it->u.d,&b,8); break; }
        case CF_CP_CLASS: it->u.cls=dupcp_class(&m,i); break;
        case CF_CP_STRING: {  /* +0x08 u16 -> Utf8 index */
            uint16_t ui=cf_u16(&m,e+8); size_t l; const char *s=cf_cp_utf(&m,ui,&l);
            it->u.utf.s = s?dupn(s,l):NULL; it->u.utf.len=l; break; }
        case CF_CP_FIELDREF: case CF_CP_METHODREF: case CF_CP_IFACEREF: {
            uint16_t ci=cf_u16(&m,e+8), nti=cf_u16(&m,e+0x0a);
            size_t ne=cp_entry(&m,nti);
            it->u.ref.cls  = dupcp_class(&m,ci);
            it->u.ref.name = dupcp_utf(&m, cf_u16(&m,ne+8));
            it->u.ref.desc = dupcp_utf(&m, cf_u16(&m,ne+0x0a));
            break; }
        case CF_CP_NAMETYPE: {
            it->u.ref.cls=NULL;
            it->u.ref.name=dupcp_utf(&m, cf_u16(&m,e+8));
            it->u.ref.desc=dupcp_utf(&m, cf_u16(&m,e+0x0a));
            break; }
        default: break;
        }
    }

    /* methods */
    uint16_t nmeth=cf_u16(&m,0x10);
    uint32_t marr=cf_u32(&m,0x30);
    out->methods=(cf_method*)calloc(nmeth?nmeth:1,sizeof(cf_method));
    if(!out->methods){ cf_free(out); *err="oom methods"; return -5; }
    out->method_count=0;
    for(uint16_t i=0;i<nmeth&&marr;i++){
        size_t me=(size_t)marr+(size_t)i*0x18; if(me+0x18>len) break;
        cf_method *md=&out->methods[out->method_count];
        md->access=cf_u16(&m,me+0x00);
        md->name=dupcp_utf(&m, cf_u16(&m,me+0x02));
        md->desc=dupcp_utf(&m, cf_u16(&m,me+0x04));
        uint16_t nattr=cf_u16(&m,me+0x06); uint32_t attr=cf_u32(&m,me+0x14);
        for(uint16_t a=0;a<nattr&&attr;a++){
            size_t ae=(size_t)attr+(size_t)a*0x18; if(ae+0x18>len) break;
            size_t l; const char *an=cf_cp_utf(&m, cf_u16(&m,ae+0x00), &l);
            if(an&&l==4&&memcmp(an,"Code",4)==0){
                md->max_stack = cf_u16(&m,ae+0x02);
                md->max_locals= cf_u16(&m,ae+0x04);
                md->code_len  = cf_u32(&m,ae+0x08);
                uint32_t cptr = cf_u32(&m,ae+0x0c);
                if(cptr && (size_t)cptr+md->code_len<=len) md->code=buf+cptr;
                else { md->code=NULL; md->code_len=0; }
            }
        }
        out->method_count++;
    }
    return 0;
}

void cf_free(cf_class *c){
    if(!c) return;
    free((void*)c->this_class); free((void*)c->super_class);
    if(c->cp){
        for(uint16_t i=1;i<c->cp_count;i++){
            cf_cp_item *it=&c->cp[i];
            switch(it->kind){
            case CF_CP_UTF8: case CF_CP_STRING: free((void*)it->u.utf.s); break;
            case CF_CP_CLASS: free((void*)it->u.cls); break;
            case CF_CP_FIELDREF: case CF_CP_METHODREF: case CF_CP_IFACEREF:
            case CF_CP_NAMETYPE:
                free((void*)it->u.ref.cls); free((void*)it->u.ref.name); free((void*)it->u.ref.desc); break;
            default: break;
            }
        }
        free(c->cp);
    }
    if(c->methods){
        for(uint16_t i=0;i<c->method_count;i++){
            free((void*)c->methods[i].name); free((void*)c->methods[i].desc);
        }
        free(c->methods);
    }
    free(c->raw);
    memset(c,0,sizeof *c);
}

const cf_method *cf_find_method(const cf_class *c,const char *name,const char *desc){
    for(uint16_t i=0;i<c->method_count;i++){
        const cf_method *md=&c->methods[i];
        if(md->name&&strcmp(md->name,name)==0 && (!desc || (md->desc&&strcmp(md->desc,desc)==0)))
            return md;
    }
    return NULL;
}
