/*
 * dcc.c - Dirge of Cerberus (FFVII, PS2 SLES-541.85 / SLPS-20200) .class cracker
 *
 * Decrypts the game's embedded-JVM `.class` script files, parses the resulting
 * in-memory classfile image, and disassembles each method's bytecode to a
 * readable JVM-opcode listing.
 *
 * CIPHER (game-custom 8-byte-block transform, NOT DES; ported 1:1 from the EE
 * decompilation, verified by producing readable Java strings):
 *   driver   FUN_003a8320   decrypts bytes [8..EOF]; KEY = file's first 8 bytes
 *   keysched FUN_0041bb38   32 x u64 subkeys + per-block counter/checksum
 *   block    FUN_0041c010 -> FUN_0041bf78 (sbox/subkey + CFB-style feedback)
 *                            FUN_0041bf08 (u64 subtract/xor/rotate stage)
 *   sbox     DAT_0047eb50   = (i + 120) & 0xff   (verified permutation)
 *
 * CONTAINER (after decrypt the engine rewrites magic TTTT->SSSS and, via
 * FUN_003a71c0, relocates embedded file-offset "pointers"): the decrypted file
 * is NOT a stock big-endian .class but a serialized, relocatable in-memory
 * image. Scalars are LITTLE-ENDIAN; stored "pointers" are file-relative byte
 * offsets. Layout reverse-engineered from FUN_003a7700 / FUN_003a71c0 /
 * FUN_003a68e8:
 *   ClassFile header (offsets into the file):
 *     +0x00 u32  magic (SSSS)
 *     +0x04 u16  minor
 *     +0x06 u16  major (0x2e = 46 -> Java 1.2)
 *     +0x08 u16  constant_pool_count
 *     +0x0a u16  access_flags
 *     +0x0c u16  interfaces_count
 *     +0x0e u16  fields_count
 *     +0x10 u16  methods_count
 *     +0x18 u32  -> constant pool array (entry stride 0x10)
 *     +0x1c u32  -> this_class  (points at a Class cp entry)
 *     +0x24 u32  -> super_class (points at a Class cp entry)
 *     +0x28 u32  -> interfaces array (u32 each)
 *     +0x2c u32  -> fields array  (stride 0x18)
 *     +0x30 u32  -> methods array (stride 0x18)
 *   constant-pool entry (0x10 bytes; Long/Double occupy two slots):
 *     +0x00 u8   tag (1..12)
 *     Utf8(1):        +0x08 u16 length, +0x0c u32 -> char data (NUL-terminated)
 *     Integer(3):     +0x08 s32 value (big-endian)
 *     Float(4):       +0x08 u32 bits  (big-endian)
 *     Long(5)/Dbl(6): +0x08 u64 value (big-endian), consumes 2 slots
 *     Class(7):       +0x08 u32 -> name char data (direct pointer)
 *     String(8):      +0x08 u16 -> Utf8 index
 *     Fieldref(9)/Methodref(10)/Iface(11): +0x08 u16 class_idx, +0x0a u16 nt_idx
 *     NameAndType(12):+0x08 u16 name_idx, +0x0a u16 desc_idx
 *   method/field entry (0x18 bytes):
 *     +0x00 u16 access, +0x02 u16 name_idx, +0x04 u16 desc_idx,
 *     +0x06 u16 attr_count, +0x14 u32 -> attr array (methods; fields use +0x08)
 *   attribute entry (0x18 bytes):
 *     +0x00 u16 name_idx; for "Code": +0x02 u16 max_stack, +0x04 u16 max_locals,
 *     +0x08 u32 code_length, +0x0c u32 -> code bytes
 *   Bytecode operands inside the Code array are standard JVM BIG-ENDIAN.
 *
 * Usage:
 *   dcc <file.class>            parse + disassemble (stdout)
 *   dcc -d <file.class> [out]   decrypt only; write raw image (default <in>.dec)
 *   dcc -p <file.class>         parse summary only (no bytecode)
 *   dcc -q <file.class>         quiet: one PASS/FAIL validation line (for batch)
 *
 * Reads bytes only; never executes the input.
 * Build: cc -O2 -std=c11 -o dcc dcc.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- raw readers (file buffer is little-endian for structure) -------- */
static uint64_t rd_u64_le(const uint8_t *p){
    uint64_t v=0; for(int i=7;i>=0;i--) v=(v<<8)|p[i]; return v; }
static void wr_u64_le(uint8_t *p,uint64_t v){ for(int i=0;i<8;i++){p[i]=(uint8_t)v; v>>=8;} }
static uint32_t rd_u32_le(const uint8_t *p){
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static void wr_u32_le(uint8_t *p,uint32_t v){ p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }

/* big-endian (bytecode operands + stored constant values) */
static uint16_t be16(const uint8_t *p){ return (uint16_t)((p[0]<<8)|p[1]); }
static uint32_t be32(const uint8_t *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

static inline uint8_t sbox(uint8_t i){ return (uint8_t)(i+120); } /* DAT_0047eb50 */

/* ====================================================================== */
/* CIPHER                                                                  */
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
    for(int i=0;i<31;i++) wr_u64_le(K->b+(i+1)*8, rd_u64_le(K->b+i*8)*5ull);
    wr_u32_le(K->b+CTR_OFF,0); wr_u32_le(K->b+CSUM_OFF,0);
}
static void stage_byte(keybuf_t *K,const uint8_t *src,uint8_t *dst){
    uint32_t ctr=rd_u32_le(K->b+CTR_OFF);
    uint8_t fb=(uint8_t)((ctr>>3)^0x45);
    uint32_t subbase=(uint32_t)(K->b[CTR_OFF]>>3)*8u;
    for(int i=0;i<8;i++){
        uint8_t c=src[i]; uint8_t t=(uint8_t)(fb^c);
        for(int k=0;k<8;k++) t=(uint8_t)(sbox(t)-K->b[subbase+k]);
        dst[i]=t; fb=c;
    }
}
static void stage_word(keybuf_t *K,uint8_t *dst){
    uint32_t ctr=rd_u32_le(K->b+CTR_OFF); uint64_t u=(uint64_t)ctr;
    uint64_t v=rd_u64_le(dst); uint32_t sidx=ctr&0xf8; uint64_t sk=rd_u64_le(K->b+sidx);
    uint64_t mask=(u|(u<<30)|(u<<20)|(u<<10))+0xa1652347ull;
    v=(v-sk)^mask; v=v^sk; v=(v<<32)|(v>>32); wr_u64_le(dst,v);
}
static void decrypt_block(keybuf_t *K,const uint8_t *src,uint8_t *dst){
    stage_byte(K,src,dst); stage_word(K,dst);
    uint32_t ctr=rd_u32_le(K->b+CTR_OFF)+8;
    uint32_t cs=rd_u32_le(K->b+CSUM_OFF)+dst[0]+dst[4];
    wr_u32_le(K->b+CTR_OFF,ctr); wr_u32_le(K->b+CSUM_OFF,cs);
}
static int decrypt_class(uint8_t *buf,size_t len){
    if(len<16) return -1;
    size_t body=len-8; if(body%8) return -2;
    keybuf_t K; key_schedule(&K,build_key(buf));
    uint8_t *p=buf+8;
    for(size_t o=0;o+8<=body;o+=8) decrypt_block(&K,p+o,p+o);
    buf[0]=buf[1]=buf[2]=buf[3]=0x53; /* TTTT -> SSSS */
    return 0;
}

/* ====================================================================== */
/* CONTAINER PARSE (serialized relocatable image)                          */
/* ====================================================================== */
typedef struct { uint8_t *d; size_t n; } img;

static uint16_t IU16(img *m,size_t o){ return (o+2<=m->n)? (uint16_t)(m->d[o]|(m->d[o+1]<<8)):0; }
static uint32_t IU32(img *m,size_t o){ return (o+4<=m->n)? rd_u32_le(m->d+o):0; }

/* NUL-terminated string at file offset off; returns ptr+len (not copied) */
static const char *cstr(img *m,uint32_t off,size_t *len){
    if(off==0||off>=m->n){ if(len)*len=0; return NULL; }
    size_t e=off; while(e<m->n && m->d[e]) e++;
    if(len)*len=e-off;
    return (const char*)(m->d+off);
}

#define CP_BASE(m)  IU32((m),0x18)
static size_t cp_entry(img *m,uint16_t i){ return (size_t)CP_BASE(m)+(size_t)i*0x10; }
static uint8_t cp_tag(img *m,uint16_t i){ size_t e=cp_entry(m,i); return (e<m->n)?m->d[e]:0; }

/* Utf8 text of entry i */
static const char *cp_utf(img *m,uint16_t i,size_t *len){
    if(cp_tag(m,i)!=1){ if(len)*len=0; return NULL; }
    size_t e=cp_entry(m,i);
    const char *s=cstr(m,IU32(m,e+0x0c),len);
    return s;
}
/* Class name of entry i (direct pointer) */
static const char *cp_class(img *m,uint16_t i,size_t *len){
    if(cp_tag(m,i)!=7){ if(len)*len=0; return NULL; }
    return cstr(m,IU32(m,cp_entry(m,i)+0x08),len);
}
/* name of a class pointed to directly by a header pointer */
static const char *class_at_ptr(img *m,uint32_t p,size_t *len){
    if(p==0||p+8>m->n){ if(len)*len=0; return NULL; }
    if(m->d[p]!=7){ if(len)*len=0; return NULL; }
    return cstr(m,IU32(m,p+0x08),len);
}

static void put_str(const char *s,size_t len){
    if(!s){ fputs("<?>",stdout); return; }
    for(size_t i=0;i<len;i++){ unsigned char c=(unsigned char)s[i];
        if(c>=0x20&&c<0x7f) putchar(c);
        else if(c=='\n') fputs("\\n",stdout);
        else if(c=='\t') fputs("\\t",stdout);
        else printf("\\x%02x",c); }
}

/* resolve a cp index inline for the disassembler */
static void print_ref(img *m,uint16_t idx){
    uint8_t t=cp_tag(m,idx); size_t e=cp_entry(m,idx); size_t l;
    switch(t){
    case 7: { const char*s=cp_class(m,idx,&l); putchar(' '); if(s)put_str(s,l); else printf("#%u",idx); break; }
    case 1: { const char*s=cp_utf(m,idx,&l); putchar(' '); if(s)put_str(s,l); break; }
    case 8: { const char*s=cp_utf(m,IU16(m,e+8),&l); fputs(" \"",stdout); if(s)put_str(s,l); putchar('"'); break; }
    case 9: case 10: case 11: {
        uint16_t ci=IU16(m,e+8), nti=IU16(m,e+0x0a);
        const char*cn=cp_class(m,ci,&l); putchar(' '); if(cn)put_str(cn,l); else printf("#%u",ci);
        size_t ne=cp_entry(m,nti);
        const char*nm=cp_utf(m,IU16(m,ne+8),&l); putchar('.'); if(nm)put_str(nm,l);
        const char*de=cp_utf(m,IU16(m,ne+0x0a),&l); putchar(':'); if(de)put_str(de,l);
        break; }
    case 12: {
        const char*nm=cp_utf(m,IU16(m,e+8),&l); putchar(' '); if(nm)put_str(nm,l);
        const char*de=cp_utf(m,IU16(m,e+0x0a),&l); putchar(':'); if(de)put_str(de,l); break; }
    case 3: printf(" %d",(int32_t)be32(m->d+e+8)); break;
    case 4: { float f; uint32_t b=be32(m->d+e+8); memcpy(&f,&b,4); printf(" %g",f); break; }
    case 5: printf(" %lldL",(long long)rd_u64_le(m->d+e+8)); break;
    case 6: { double dd; uint64_t b=rd_u64_le(m->d+e+8); memcpy(&dd,&b,8); printf(" %g",dd); break; }
    default: printf(" #%u",idx);
    }
}

/* ---- opcode table ---------------------------------------------------- */
typedef struct { const char *name; int len; } op_t;
static op_t OPS[256]; static int ops_inited=0;
static void init_ops(void){
    if(ops_inited) return;
    ops_inited=1;
    for(int i=0;i<256;i++){ OPS[i].name="<unknown>"; OPS[i].len=1; }
#define O(c,nm,ln) OPS[c].name=nm; OPS[c].len=ln;
    O(0x00,"nop",1)O(0x01,"aconst_null",1)O(0x02,"iconst_m1",1)O(0x03,"iconst_0",1)
    O(0x04,"iconst_1",1)O(0x05,"iconst_2",1)O(0x06,"iconst_3",1)O(0x07,"iconst_4",1)O(0x08,"iconst_5",1)
    O(0x09,"lconst_0",1)O(0x0a,"lconst_1",1)O(0x0b,"fconst_0",1)O(0x0c,"fconst_1",1)O(0x0d,"fconst_2",1)
    O(0x0e,"dconst_0",1)O(0x0f,"dconst_1",1)O(0x10,"bipush",2)O(0x11,"sipush",3)
    O(0x12,"ldc",2)O(0x13,"ldc_w",3)O(0x14,"ldc2_w",3)
    O(0x15,"iload",2)O(0x16,"lload",2)O(0x17,"fload",2)O(0x18,"dload",2)O(0x19,"aload",2)
    O(0x1a,"iload_0",1)O(0x1b,"iload_1",1)O(0x1c,"iload_2",1)O(0x1d,"iload_3",1)
    O(0x1e,"lload_0",1)O(0x1f,"lload_1",1)O(0x20,"lload_2",1)O(0x21,"lload_3",1)
    O(0x22,"fload_0",1)O(0x23,"fload_1",1)O(0x24,"fload_2",1)O(0x25,"fload_3",1)
    O(0x26,"dload_0",1)O(0x27,"dload_1",1)O(0x28,"dload_2",1)O(0x29,"dload_3",1)
    O(0x2a,"aload_0",1)O(0x2b,"aload_1",1)O(0x2c,"aload_2",1)O(0x2d,"aload_3",1)
    O(0x2e,"iaload",1)O(0x2f,"laload",1)O(0x30,"faload",1)O(0x31,"daload",1)
    O(0x32,"aaload",1)O(0x33,"baload",1)O(0x34,"caload",1)O(0x35,"saload",1)
    O(0x36,"istore",2)O(0x37,"lstore",2)O(0x38,"fstore",2)O(0x39,"dstore",2)O(0x3a,"astore",2)
    O(0x3b,"istore_0",1)O(0x3c,"istore_1",1)O(0x3d,"istore_2",1)O(0x3e,"istore_3",1)
    O(0x3f,"lstore_0",1)O(0x40,"lstore_1",1)O(0x41,"lstore_2",1)O(0x42,"lstore_3",1)
    O(0x43,"fstore_0",1)O(0x44,"fstore_1",1)O(0x45,"fstore_2",1)O(0x46,"fstore_3",1)
    O(0x47,"dstore_0",1)O(0x48,"dstore_1",1)O(0x49,"dstore_2",1)O(0x4a,"dstore_3",1)
    O(0x4b,"astore_0",1)O(0x4c,"astore_1",1)O(0x4d,"astore_2",1)O(0x4e,"astore_3",1)
    O(0x4f,"iastore",1)O(0x50,"lastore",1)O(0x51,"fastore",1)O(0x52,"dastore",1)
    O(0x53,"aastore",1)O(0x54,"bastore",1)O(0x55,"castore",1)O(0x56,"sastore",1)
    O(0x57,"pop",1)O(0x58,"pop2",1)O(0x59,"dup",1)O(0x5a,"dup_x1",1)O(0x5b,"dup_x2",1)
    O(0x5c,"dup2",1)O(0x5d,"dup2_x1",1)O(0x5e,"dup2_x2",1)O(0x5f,"swap",1)
    O(0x60,"iadd",1)O(0x61,"ladd",1)O(0x62,"fadd",1)O(0x63,"dadd",1)
    O(0x64,"isub",1)O(0x65,"lsub",1)O(0x66,"fsub",1)O(0x67,"dsub",1)
    O(0x68,"imul",1)O(0x69,"lmul",1)O(0x6a,"fmul",1)O(0x6b,"dmul",1)
    O(0x6c,"idiv",1)O(0x6d,"ldiv",1)O(0x6e,"fdiv",1)O(0x6f,"ddiv",1)
    O(0x70,"irem",1)O(0x71,"lrem",1)O(0x72,"frem",1)O(0x73,"drem",1)
    O(0x74,"ineg",1)O(0x75,"lneg",1)O(0x76,"fneg",1)O(0x77,"dneg",1)
    O(0x78,"ishl",1)O(0x79,"lshl",1)O(0x7a,"ishr",1)O(0x7b,"lshr",1)O(0x7c,"iushr",1)O(0x7d,"lushr",1)
    O(0x7e,"iand",1)O(0x7f,"land",1)O(0x80,"ior",1)O(0x81,"lor",1)O(0x82,"ixor",1)O(0x83,"lxor",1)
    O(0x84,"iinc",3)
    O(0x85,"i2l",1)O(0x86,"i2f",1)O(0x87,"i2d",1)O(0x88,"l2i",1)O(0x89,"l2f",1)O(0x8a,"l2d",1)
    O(0x8b,"f2i",1)O(0x8c,"f2l",1)O(0x8d,"f2d",1)O(0x8e,"d2i",1)O(0x8f,"d2l",1)O(0x90,"d2f",1)
    O(0x91,"i2b",1)O(0x92,"i2c",1)O(0x93,"i2s",1)
    O(0x94,"lcmp",1)O(0x95,"fcmpl",1)O(0x96,"fcmpg",1)O(0x97,"dcmpl",1)O(0x98,"dcmpg",1)
    O(0x99,"ifeq",3)O(0x9a,"ifne",3)O(0x9b,"iflt",3)O(0x9c,"ifge",3)O(0x9d,"ifgt",3)O(0x9e,"ifle",3)
    O(0x9f,"if_icmpeq",3)O(0xa0,"if_icmpne",3)O(0xa1,"if_icmplt",3)O(0xa2,"if_icmpge",3)
    O(0xa3,"if_icmpgt",3)O(0xa4,"if_icmple",3)O(0xa5,"if_acmpeq",3)O(0xa6,"if_acmpne",3)
    O(0xa7,"goto",3)O(0xa8,"jsr",3)O(0xa9,"ret",2)O(0xaa,"tableswitch",-1)O(0xab,"lookupswitch",-1)
    O(0xac,"ireturn",1)O(0xad,"lreturn",1)O(0xae,"freturn",1)O(0xaf,"dreturn",1)
    O(0xb0,"areturn",1)O(0xb1,"return",1)
    O(0xb2,"getstatic",3)O(0xb3,"putstatic",3)O(0xb4,"getfield",3)O(0xb5,"putfield",3)
    O(0xb6,"invokevirtual",3)O(0xb7,"invokespecial",3)O(0xb8,"invokestatic",3)
    O(0xb9,"invokeinterface",5)O(0xba,"invokedynamic",5)
    O(0xbb,"new",3)O(0xbc,"newarray",2)O(0xbd,"anewarray",3)O(0xbe,"arraylength",1)O(0xbf,"athrow",1)
    O(0xc0,"checkcast",3)O(0xc1,"instanceof",3)O(0xc2,"monitorenter",1)O(0xc3,"monitorexit",1)
    O(0xc4,"wide",-1)O(0xc5,"multianewarray",4)O(0xc6,"ifnull",3)O(0xc7,"ifnonnull",3)
    O(0xc8,"goto_w",5)O(0xc9,"jsr_w",5)
#undef O
}

static void disasm_code(img *m,const uint8_t *code,uint32_t clen){
    init_ops();
    uint32_t pc=0;
    while(pc<clen){
        uint8_t op=code[pc]; op_t *o=&OPS[op];
        printf("      %5u: %-16s",pc,o->name);
        if(op==0xc4){ uint8_t wop=code[pc+1]; uint16_t idx=be16(code+pc+2);
            printf(" %s #%u",OPS[wop].name,idx);
            if(wop==0x84){ printf(", %d",(int16_t)be16(code+pc+4)); pc+=6; } else pc+=4;
            putchar('\n'); continue; }
        if(op==0xaa){ uint32_t p=(pc+4)&~3u;
            int32_t def=(int32_t)be32(code+p),low=(int32_t)be32(code+p+4),high=(int32_t)be32(code+p+8);
            printf(" default=%d low=%d high=%d",(int)(pc+def),low,high);
            pc=p+12+(uint32_t)(high-low+1)*4; putchar('\n'); continue; }
        if(op==0xab){ uint32_t p=(pc+4)&~3u;
            int32_t def=(int32_t)be32(code+p); uint32_t np=be32(code+p+4);
            printf(" default=%d npairs=%u",(int)(pc+def),np); pc=p+8+np*8; putchar('\n'); continue; }
        int len=o->len;
        switch(op){
        case 0x10: printf(" %d",(int8_t)code[pc+1]); break;
        case 0x11: printf(" %d",(int16_t)be16(code+pc+1)); break;
        case 0x12: print_ref(m,code[pc+1]); break;
        case 0x13: case 0x14: print_ref(m,be16(code+pc+1)); break;
        case 0x15: case 0x16: case 0x17: case 0x18: case 0x19:
        case 0x36: case 0x37: case 0x38: case 0x39: case 0x3a:
        case 0xa9: printf(" %u",code[pc+1]); break;
        case 0x84: printf(" %u, %d",code[pc+1],(int8_t)code[pc+2]); break;
        case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e:
        case 0x9f: case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4:
        case 0xa5: case 0xa6: case 0xa7: case 0xa8: case 0xc6: case 0xc7:
            printf(" %d",(int)(pc+(int16_t)be16(code+pc+1))); break;
        case 0xc8: case 0xc9: printf(" %d",(int)(pc+(int32_t)be32(code+pc+1))); break;
        case 0xb2: case 0xb3: case 0xb4: case 0xb5:
        case 0xb6: case 0xb7: case 0xb8: case 0xbb: case 0xbd: case 0xc0: case 0xc1:
            print_ref(m,be16(code+pc+1)); break;
        case 0xb9: print_ref(m,be16(code+pc+1)); printf(" count=%u",code[pc+3]); break;
        case 0xbc: printf(" %u",code[pc+1]); break;
        case 0xc5: print_ref(m,be16(code+pc+1)); printf(" dims=%u",code[pc+3]); break;
        default: break;
        }
        putchar('\n');
        if(len<=0) len=1;
        pc+=(uint32_t)len;
    }
}

/* disassemble the Code attribute of a method entry at offset `me` */
static int disasm_method(img *m,size_t me,int do_disasm){
    uint16_t acc=IU16(m,me+0x00), ni=IU16(m,me+0x02), di=IU16(m,me+0x04);
    uint16_t nattr=IU16(m,me+0x06); uint32_t attr=IU32(m,me+0x14);
    size_t l;
    fputs("  ",stdout); const char*nm=cp_utf(m,ni,&l); if(nm)put_str(nm,l); else printf("#%u",ni);
    putchar(' '); const char*de=cp_utf(m,di,&l); if(de)put_str(de,l);
    printf("   [access=0x%04x]\n",acc);
    for(uint16_t a=0;a<nattr && attr;a++){
        size_t ae=(size_t)attr+(size_t)a*0x18;
        if(ae+0x18>m->n) break;
        uint16_t an=IU16(m,ae+0x00);
        const char*anm=cp_utf(m,an,&l);
        if(anm && l==4 && memcmp(anm,"Code",4)==0){
            uint16_t ms=IU16(m,ae+0x02), ml=IU16(m,ae+0x04);
            uint32_t clen=IU32(m,ae+0x08), cptr=IU32(m,ae+0x0c);
            printf("    [Code] stack=%u locals=%u len=%u\n",ms,ml,clen);
            if(do_disasm && cptr && cptr+clen<=m->n) disasm_code(m,m->d+cptr,clen);
        }
    }
    return 0;
}

/* ---- validation + summary ------------------------------------------- */
static int readable_run(const char *s,size_t l){
    for(size_t i=0;i<l;i++){ unsigned char c=(unsigned char)s[i];
        if(c<0x09||(c>0x0d&&c<0x20)||c>0x7e) return 0; }
    return 1;
}

int main(int argc,char**argv){
    const char *mode="full",*in=NULL,*out=NULL;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-d")) mode="decrypt";
        else if(!strcmp(argv[i],"-p")) mode="parse";
        else if(!strcmp(argv[i],"-q")) mode="quiet";
        else if(!in) in=argv[i]; else if(!out) out=argv[i];
    }
    if(!in){ fprintf(stderr,"usage: %s [-d|-p|-q] <file.class> [out]\n",argv[0]); return 2; }

    FILE *f=fopen(in,"rb"); if(!f){ fprintf(stderr,"cannot open %s\n",in); return 2; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<=0){ fclose(f); fprintf(stderr,"empty %s\n",in); return 2; }
    uint8_t *buf=malloc((size_t)sz);
    if(!buf||fread(buf,1,(size_t)sz,f)!=(size_t)sz){ fclose(f); fprintf(stderr,"read err\n"); free(buf); return 2; }
    fclose(f);
    size_t len=(size_t)sz;

    int had_tttt=(len>=4&&buf[0]==0x54&&buf[1]==0x54&&buf[2]==0x54&&buf[3]==0x54);
    int rc=decrypt_class(buf,len);
    if(rc){
            if(!strcmp(mode,"quiet")) printf("FAIL\t%s\tdecrypt rc=%d\n",in,rc);
            else fprintf(stderr,"decrypt rc=%d\n",rc);
            free(buf); return 1; }

    if(!strcmp(mode,"decrypt")){
        char ob[1200]; if(!out){ snprintf(ob,sizeof ob,"%s.dec",in); out=ob; }
        FILE *fo=fopen(out,"wb"); if(!fo){ fprintf(stderr,"cannot write %s\n",out); free(buf); return 1; }
        fwrite(buf,1,len,fo); fclose(fo);
        printf("decrypted -> %s  (magic %02x%02x%02x%02x, had_TTTT=%d)\n",out,buf[0],buf[1],buf[2],buf[3],had_tttt);
        free(buf); return 0;
    }

    img m={buf,len};
    uint16_t cpc=IU16(&m,0x08), nmeth=IU16(&m,0x10), nflds=IU16(&m,0x0e), nif=IU16(&m,0x0c);
    uint32_t cpbase=CP_BASE(&m);
    size_t l;
    const char *thisn=class_at_ptr(&m,IU32(&m,0x1c),&l); size_t thisl=l;
    const char *supn =class_at_ptr(&m,IU32(&m,0x24),&l); size_t supl=l;

    int magic_ok=(buf[0]==0x53&&buf[1]==0x53&&buf[2]==0x53&&buf[3]==0x53);
    int struct_ok=(cpbase>=0x18 && cpbase<len && cpc>=1 && cpc<20000 &&
                   (size_t)cpbase+(size_t)cpc*0x10<=len);

    if(!strcmp(mode,"quiet")){
        if(!magic_ok||!struct_ok){ printf("FAIL\t%s\tmagic=%d cpc=%u cpbase=0x%x\n",in,magic_ok,cpc,cpbase); free(buf); return 1; }
        int utf=0,readable=0;
        for(uint16_t i=1;i<cpc;i++) if(cp_tag(&m,i)==1){ utf++; size_t ll; const char*s=cp_utf(&m,i,&ll); if(s&&readable_run(s,ll)) readable++; }
        /* verify every method code pointer is in-bounds */
        int bad=0; uint32_t marr=IU32(&m,0x30);
        for(uint16_t mi=0;mi<nmeth&&marr;mi++){ size_t me=(size_t)marr+(size_t)mi*0x18; if(me+0x18>len){bad++;continue;}
            uint32_t attr=IU32(&m,me+0x14); uint16_t na=IU16(&m,me+0x06);
            for(uint16_t a=0;a<na&&attr;a++){ size_t ae=(size_t)attr+(size_t)a*0x18; if(ae+0x18>len){bad++;break;}
                size_t ll; const char*an=cp_utf(&m,IU16(&m,ae),&ll);
                if(an&&ll==4&&!memcmp(an,"Code",4)){ uint32_t cl=IU32(&m,ae+8),cp=IU32(&m,ae+0x0c); if(cp+cl>len) bad++; } } }
        printf("PASS\t%s\tthis=",in); if(thisn)put_str(thisn,thisl); else fputs("?",stdout);
        printf("\tcp=%u utf8=%d readable=%d methods=%u codebad=%d\n",cpc,utf,readable,nmeth,bad);
        free(buf); return 0;
    }

    if(!magic_ok||!struct_ok){ fprintf(stderr,"not a valid image: magic=%d cpc=%u cpbase=0x%x\n",magic_ok,cpc,cpbase); free(buf); return 1; }

    printf("== %s ==\n",in);
    printf("magic=%02x%02x%02x%02x minor=%u major=%u (Java %s)\n",buf[0],buf[1],buf[2],buf[3],
           IU16(&m,4),IU16(&m,6), IU16(&m,6)==45?"1.1":IU16(&m,6)==46?"1.2":IU16(&m,6)==47?"1.3":"?");
    printf("constant_pool_count=%u access=0x%04x\n",cpc,IU16(&m,0x0a));
    fputs("this:  ",stdout);  if(thisn)put_str(thisn,thisl); putchar('\n');
    fputs("super: ",stdout);  if(supn)put_str(supn,supl); putchar('\n');

    /* interfaces */
    uint32_t iarr=IU32(&m,0x28);
    if(nif&&iarr){ printf("interfaces: %u\n",nif);
        for(uint16_t i=0;i<nif;i++){ uint32_t p=IU32(&m,iarr+i*4); const char*s=class_at_ptr(&m,p,&l);
            fputs("  ",stdout); if(s)put_str(s,l); putchar('\n'); } }

    /* fields */
    uint32_t farr=IU32(&m,0x2c);
    printf("fields: %u\n",nflds);
    for(uint16_t i=0;i<nflds&&farr;i++){ size_t fe=(size_t)farr+(size_t)i*0x18; if(fe+0x18>len) break;
        const char*nm=cp_utf(&m,IU16(&m,fe+2),&l); fputs("  ",stdout); if(nm)put_str(nm,l);
        fputs(" : ",stdout); const char*de=cp_utf(&m,IU16(&m,fe+4),&l); if(de)put_str(de,l); putchar('\n'); }

    /* methods */
    uint32_t marr=IU32(&m,0x30);
    printf("methods: %u\n",nmeth);
    int disasm=strcmp(mode,"parse")!=0;
    for(uint16_t i=0;i<nmeth&&marr;i++){ size_t me=(size_t)marr+(size_t)i*0x18; if(me+0x18>len) break;
        disasm_method(&m,me,disasm); }

    free(buf); return 0;
}
