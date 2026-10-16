/* jvm.c - the interpreter. See jvm.h. */
#define _POSIX_C_SOURCE 200809L   /* strdup */
#include "jvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- opcode names (for tracing) --------------------------------------- */
static const char *OPNAME[256];
static int opnames_inited=0;
static void init_opnames(void){
    if(opnames_inited) return;
    opnames_inited=1;
    for(int i=0;i<256;i++) OPNAME[i]="?";
#define N(c,s) OPNAME[c]=s;
    N(0x00,"nop")N(0x01,"aconst_null")N(0x02,"iconst_m1")N(0x03,"iconst_0")N(0x04,"iconst_1")
    N(0x05,"iconst_2")N(0x06,"iconst_3")N(0x07,"iconst_4")N(0x08,"iconst_5")
    N(0x09,"lconst_0")N(0x0a,"lconst_1")N(0x0b,"fconst_0")N(0x0c,"fconst_1")N(0x0d,"fconst_2")
    N(0x0e,"dconst_0")N(0x0f,"dconst_1")N(0x10,"bipush")N(0x11,"sipush")N(0x12,"ldc")N(0x13,"ldc_w")N(0x14,"ldc2_w")
    N(0x15,"iload")N(0x16,"lload")N(0x17,"fload")N(0x18,"dload")N(0x19,"aload")
    N(0x1a,"iload_0")N(0x1b,"iload_1")N(0x1c,"iload_2")N(0x1d,"iload_3")
    N(0x1e,"lload_0")N(0x1f,"lload_1")N(0x20,"lload_2")N(0x21,"lload_3")
    N(0x22,"fload_0")N(0x23,"fload_1")N(0x24,"fload_2")N(0x25,"fload_3")
    N(0x26,"dload_0")N(0x27,"dload_1")N(0x28,"dload_2")N(0x29,"dload_3")
    N(0x2a,"aload_0")N(0x2b,"aload_1")N(0x2c,"aload_2")N(0x2d,"aload_3")
    N(0x2e,"iaload")N(0x2f,"laload")N(0x30,"faload")N(0x31,"daload")N(0x32,"aaload")N(0x33,"baload")N(0x34,"caload")N(0x35,"saload")
    N(0x36,"istore")N(0x37,"lstore")N(0x38,"fstore")N(0x39,"dstore")N(0x3a,"astore")
    N(0x3b,"istore_0")N(0x3c,"istore_1")N(0x3d,"istore_2")N(0x3e,"istore_3")
    N(0x3f,"lstore_0")N(0x40,"lstore_1")N(0x41,"lstore_2")N(0x42,"lstore_3")
    N(0x43,"fstore_0")N(0x44,"fstore_1")N(0x45,"fstore_2")N(0x46,"fstore_3")
    N(0x47,"dstore_0")N(0x48,"dstore_1")N(0x49,"dstore_2")N(0x4a,"dstore_3")
    N(0x4b,"astore_0")N(0x4c,"astore_1")N(0x4d,"astore_2")N(0x4e,"astore_3")
    N(0x4f,"iastore")N(0x50,"lastore")N(0x51,"fastore")N(0x52,"dastore")N(0x53,"aastore")N(0x54,"bastore")N(0x55,"castore")N(0x56,"sastore")
    N(0x57,"pop")N(0x58,"pop2")N(0x59,"dup")N(0x5a,"dup_x1")N(0x5b,"dup_x2")N(0x5c,"dup2")N(0x5d,"dup2_x1")N(0x5e,"dup2_x2")N(0x5f,"swap")
    N(0x60,"iadd")N(0x61,"ladd")N(0x62,"fadd")N(0x63,"dadd")N(0x64,"isub")N(0x65,"lsub")N(0x66,"fsub")N(0x67,"dsub")
    N(0x68,"imul")N(0x69,"lmul")N(0x6a,"fmul")N(0x6b,"dmul")N(0x6c,"idiv")N(0x6d,"ldiv")N(0x6e,"fdiv")N(0x6f,"ddiv")
    N(0x70,"irem")N(0x71,"lrem")N(0x72,"frem")N(0x73,"drem")N(0x74,"ineg")N(0x75,"lneg")N(0x76,"fneg")N(0x77,"dneg")
    N(0x78,"ishl")N(0x79,"lshl")N(0x7a,"ishr")N(0x7b,"lshr")N(0x7c,"iushr")N(0x7d,"lushr")
    N(0x7e,"iand")N(0x7f,"land")N(0x80,"ior")N(0x81,"lor")N(0x82,"ixor")N(0x83,"lxor")N(0x84,"iinc")
    N(0x85,"i2l")N(0x86,"i2f")N(0x87,"i2d")N(0x88,"l2i")N(0x89,"l2f")N(0x8a,"l2d")
    N(0x8b,"f2i")N(0x8c,"f2l")N(0x8d,"f2d")N(0x8e,"d2i")N(0x8f,"d2l")N(0x90,"d2f")N(0x91,"i2b")N(0x92,"i2c")N(0x93,"i2s")
    N(0x94,"lcmp")N(0x95,"fcmpl")N(0x96,"fcmpg")N(0x97,"dcmpl")N(0x98,"dcmpg")
    N(0x99,"ifeq")N(0x9a,"ifne")N(0x9b,"iflt")N(0x9c,"ifge")N(0x9d,"ifgt")N(0x9e,"ifle")
    N(0x9f,"if_icmpeq")N(0xa0,"if_icmpne")N(0xa1,"if_icmplt")N(0xa2,"if_icmpge")N(0xa3,"if_icmpgt")N(0xa4,"if_icmple")
    N(0xa5,"if_acmpeq")N(0xa6,"if_acmpne")N(0xa7,"goto")N(0xa8,"jsr")N(0xa9,"ret")N(0xaa,"tableswitch")N(0xab,"lookupswitch")
    N(0xac,"ireturn")N(0xad,"lreturn")N(0xae,"freturn")N(0xaf,"dreturn")N(0xb0,"areturn")N(0xb1,"return")
    N(0xb2,"getstatic")N(0xb3,"putstatic")N(0xb4,"getfield")N(0xb5,"putfield")
    N(0xb6,"invokevirtual")N(0xb7,"invokespecial")N(0xb8,"invokestatic")N(0xb9,"invokeinterface")N(0xba,"invokedynamic")
    N(0xbb,"new")N(0xbc,"newarray")N(0xbd,"anewarray")N(0xbe,"arraylength")N(0xbf,"athrow")
    N(0xc0,"checkcast")N(0xc1,"instanceof")N(0xc2,"monitorenter")N(0xc3,"monitorexit")
    N(0xc4,"wide")N(0xc5,"multianewarray")N(0xc6,"ifnull")N(0xc7,"ifnonnull")N(0xc8,"goto_w")N(0xc9,"jsr_w")
#undef N
}

/* ---- descriptor helpers ----------------------------------------------- */
int jvm_desc_arg_slots(const char *desc){
    if(!desc||desc[0]!='(') return 0;
    int slots=0; const char *p=desc+1;
    while(*p && *p!=')'){
        char c=*p;
        if(c=='['){ while(*p=='[') p++; if(*p=='L'){ while(*p&&*p!=';') p++; } if(*p) p++; slots+=1; continue; }
        if(c=='L'){ while(*p&&*p!=';') p++; if(*p) p++; slots+=1; continue; }
        if(c=='J'||c=='D'){ slots+=2; p++; continue; }
        slots+=1; p++; /* I F Z B C S */
    }
    return slots;
}
int jvm_desc_arg_count(const char *desc){
    if(!desc||desc[0]!='(') return 0;
    int n=0; const char *p=desc+1;
    while(*p && *p!=')'){
        char c=*p;
        if(c=='['){ while(*p=='[') p++; if(*p=='L'){ while(*p&&*p!=';') p++; } if(*p) p++; n++; continue; }
        if(c=='L'){ while(*p&&*p!=';') p++; if(*p) p++; n++; continue; }
        n++; p++;
    }
    return n;
}
char jvm_desc_ret_kind(const char *desc){
    if(!desc) return 'V';
    const char *p=strchr(desc,')'); if(!p) return 'V';
    p++; if(*p=='[') return '[';
    return *p?*p:'V';
}

/* ---- lifecycle -------------------------------------------------------- */
void jvm_init(jvm *vm, const cf_class *cls){
    init_opnames();
    memset(vm,0,sizeof *vm);
    vm->cls=cls;
    vm->heap_cap=16u*1024u*1024u;
    vm->heap=(uint8_t*)malloc(vm->heap_cap);
    vm->heap_used=0;
    vm->nat_cap=512; vm->nat_count=0;
    vm->natives=(jvm_native_entry*)calloc((size_t)vm->nat_cap,sizeof(jvm_native_entry));
    vm->trace=0; vm->trace_budget=0;
    vm->step_limit=200ull*1000ull*1000ull; /* generous runaway guard */
    vm->call_depth=0;
    vm->max_call_depth=1600; /* deep enough for any real call chain; bounds C recursion */
}
void jvm_destroy(jvm *vm){
    if(vm->natives){ for(int i=0;i<vm->nat_count;i++) free(vm->natives[i].key); free(vm->natives); }
    free(vm->statics);   /* names are borrowed; only the array is owned */
    free(vm->heap);
    memset(vm,0,sizeof *vm);
}

/* ---- native registry -------------------------------------------------- */
static jvm_native_entry *nat_find(jvm *vm,const char *key){
    for(int i=0;i<vm->nat_count;i++) if(strcmp(vm->natives[i].key,key)==0) return &vm->natives[i];
    return NULL;
}
void jvm_register_native(jvm *vm,const char *key,jvm_native_fn fn){
    jvm_native_entry *e=nat_find(vm,key);
    if(e){ e->fn=fn; return; }
    if(vm->nat_count==vm->nat_cap){
        vm->nat_cap*=2;
        vm->natives=(jvm_native_entry*)realloc(vm->natives,(size_t)vm->nat_cap*sizeof(jvm_native_entry));
    }
    vm->natives[vm->nat_count].key=strdup(key);
    vm->natives[vm->nat_count].fn=fn;
    vm->nat_count++;
}
void jvm_register_stub_table(jvm *vm,const char *const *keys,int count){
    for(int i=0;i<count;i++) jvm_register_native(vm,keys[i],NULL);
}

/* default stub: log + typed default return */
static jval default_stub(jvm *vm,const char *key,const jval *args,int nargs,const char *desc){
    if(!vm->quiet){
        fprintf(stderr,"  [bridge-stub] %s  args=%d: ",key,nargs);
        for(int i=0;i<nargs;i++) fprintf(stderr,"%s%d",i?",":"",args[i].i);
        fputc('\n',stderr);
    }
    jval r; r.l=0;
    char rk=jvm_desc_ret_kind(desc);
    if(rk=='F') r.f=0.0f; else if(rk=='D') r.d=0.0; else r.l=0;
    return r;
}

/* ---- heap ------------------------------------------------------------- */
static void *bump(jvm *vm,size_t n){
    n=(n+7)&~(size_t)7;
    if(vm->heap_used+n>vm->heap_cap) return NULL;
    void *p=vm->heap+vm->heap_used; vm->heap_used+=n; return p;
}
jobject *jvm_new_object(jvm *vm,const char *class_name){
    jobject *o=(jobject*)bump(vm,sizeof(jobject));
    if(!o){ vm->error=1; snprintf(vm->errmsg,sizeof vm->errmsg,"heap exhausted (new %s)",class_name?class_name:"?"); return NULL; }
    memset(o,0,sizeof *o); o->class_name=class_name; return o;
}
jobject *jvm_new_array(jvm *vm,char kind,int len){
    if(len<0){ vm->error=1; snprintf(vm->errmsg,sizeof vm->errmsg,"negative array size %d",len); return NULL; }
    jobject *o=(jobject*)bump(vm,sizeof(jobject));
    jval *data=(jval*)bump(vm,(size_t)(len>0?len:1)*sizeof(jval));
    if(!o||!data){ vm->error=1; snprintf(vm->errmsg,sizeof vm->errmsg,"heap exhausted (array)"); return NULL; }
    memset(o,0,sizeof *o); o->is_array=1; o->arr_kind=kind; o->arr_len=len; o->arr=data;
    for(int i=0;i<len;i++) data[i].l=0;
    return o;
}
/* Is `p` a real object/array we bump-allocated? The VM is untyped at runtime,
 * so an int (or a String.value cp-pointer) can end up in a slot later used as an
 * objectref. Dereferencing that would be a wild read; instead we validate the
 * pointer lies inside the live heap and is 8-aligned (bump granularity) and, if
 * not, the caller raises a clean error. This makes every objectref deref
 * fail-safe against unverified bytecode. */
static int heap_obj_ok(const jvm *vm,const void *p){
    if(!p) return 0;
    const uint8_t *b=(const uint8_t*)p;
    if(b<vm->heap || b>=vm->heap+vm->heap_used) return 0;
    if((size_t)(b-vm->heap) & 7u) return 0;
    return 1;
}
/* static-field slot, keyed by field name (cp-owned, stable lifetime). */
static jval *static_slot(jvm *vm,const char *name,int create){
    if(!name) return NULL;
    for(int i=0;i<vm->nstatics;i++) if(strcmp(vm->statics[i].name,name)==0) return &vm->statics[i].val;
    if(!create) return NULL;
    if(vm->nstatics==vm->statics_cap){
        vm->statics_cap=vm->statics_cap?vm->statics_cap*2:16;
        vm->statics=realloc(vm->statics,(size_t)vm->statics_cap*sizeof *vm->statics);
    }
    vm->statics[vm->nstatics].name=name;
    vm->statics[vm->nstatics].val.l=0;
    return &vm->statics[vm->nstatics++].val;
}
/* run <clinit> once (before first static access) so static initializers that
 * build arrays/constants populate the static slots. (jvm_invoke decl in jvm.h) */
static void ensure_clinit(jvm *vm){
    if(vm->clinit_ran) return;
    vm->clinit_ran=1;                       /* set first: <clinit> itself putstatics */
    const cf_method *ci=cf_find_method(vm->cls,"<clinit>","()V");
    if(ci && ci->code) jvm_invoke(vm,ci,NULL,0);
}
static jval *obj_field(jobject *o,const char *name,int create){
    for(int i=0;i<o->nfields;i++) if(strcmp(o->fnames[i],name)==0) return &o->fvals[i];
    if(!create || o->nfields>=32) return NULL;
    /* `name` is always a stable-lifetime pointer: a constant-pool string (freed
     * by cf_free, after jvm_destroy) or a string literal. Objects live on the
     * bump heap and are freed as one block, so strdup'ing here would leak. */
    o->fnames[o->nfields]=name;
    o->fvals[o->nfields].l=0;
    return &o->fvals[o->nfields++];
}

/* public thin wrappers for native bridge handlers (see jvm.h) */
jval *jvm_obj_field(jobject *o,const char *name,int create){
    if(!o) return NULL;
    return obj_field(o,name,create);
}
int jvm_heap_obj_ok(const jvm *vm,const void *p){ return heap_obj_ok(vm,p); }

/* ---- interpreter ------------------------------------------------------ */
#define RD_U8(pc)   (code[(pc)])
#define RD_S8(pc)   ((int8_t)code[(pc)])
#define RD_U16(pc)  ((uint16_t)((code[(pc)]<<8)|code[(pc)+1]))
#define RD_S16(pc)  ((int16_t)((code[(pc)]<<8)|code[(pc)+1]))
#define RD_S32(pc)  ((int32_t)(((uint32_t)code[(pc)]<<24)|((uint32_t)code[(pc)+1]<<16)|((uint32_t)code[(pc)+2]<<8)|code[(pc)+3]))

jval jvm_invoke(jvm *vm,const cf_method *m,const jval *in_args,int nargs){
    jval zero; zero.l=0;
    if(!m||!m->code){ vm->error=1; snprintf(vm->errmsg,sizeof vm->errmsg,"method has no Code"); return zero; }
    if(vm->max_call_depth && vm->call_depth>=vm->max_call_depth){
        vm->error=1; snprintf(vm->errmsg,sizeof vm->errmsg,"call depth limit (%d) exceeded",vm->max_call_depth); return zero; }
    vm->call_depth++;

    const uint8_t *code=m->code; uint32_t clen=m->code_len;
    const cf_class *cls=vm->cls;

    int nloc=m->max_locals+4; if(nloc<nargs+4) nloc=nargs+4;
    int nstk=m->max_stack+8;
    jval *loc=(jval*)calloc((size_t)nloc,sizeof(jval));
    jval *stk=(jval*)calloc((size_t)nstk,sizeof(jval));
    int sp=0;
    for(int i=0;i<nargs && i<nloc;i++) loc[i]=in_args[i];

    jval ret=zero;
    uint32_t pc=0;
#define PUSH(v)  do{ jval _pv=(v); if(sp>=nstk){vm->error=1;snprintf(vm->errmsg,sizeof vm->errmsg,"stack overflow @%u",pc);goto done;} stk[sp++]=_pv;}while(0)
#define POP()    (stk[--sp])
#define PUSHI(x) do{ jval _t; _t.i=(int32_t)(x); PUSH(_t);}while(0)
#define PUSHF(x) do{ jval _t; _t.f=(float)(x);   PUSH(_t);}while(0)
#define PUSHA(x) do{ jval _t; _t.a=(void*)(x);   PUSH(_t);}while(0)
#define NEEDSTK(n) do{ if(sp<(n)){vm->error=1;snprintf(vm->errmsg,sizeof vm->errmsg,"stack underflow @%u op=0x%02x",pc,op);goto done;}}while(0)

    while(pc<clen){
        if(vm->step_limit && vm->insn_count>=vm->step_limit){
            vm->error=1; snprintf(vm->errmsg,sizeof vm->errmsg,"step limit (%llu) exceeded",(unsigned long long)vm->step_limit); goto done; }
        uint8_t op=code[pc];
        vm->insn_count++;
        vm->op_hist[op]++;
        if(vm->trace && vm->trace_budget>0){
            vm->trace_budget--;
            fprintf(stdout,"    %5u: %-14s  [sp=%d",pc,OPNAME[op],sp);
            if(sp>0) fprintf(stdout," top=%d",stk[sp-1].i);
            fputs("]\n",stdout);
        }
        uint32_t next=pc+1; /* default advance; set per-op */
        switch(op){
        case 0x00: break;                                   /* nop */
        case 0x01: PUSHA(NULL); break;                      /* aconst_null */
        case 0x02: PUSHI(-1); break;
        case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08:
            PUSHI(op-0x03); break;                          /* iconst_0..5 */
        case 0x09: case 0x0a: { jval t; t.l=op-0x09; PUSH(t); PUSH(t); break; } /* lconst_0/1 (2 slots) */
        case 0x0b: PUSHF(0.0f); break; case 0x0c: PUSHF(1.0f); break; case 0x0d: PUSHF(2.0f); break;
        case 0x0e: { jval t; t.d=0.0; PUSH(t); PUSH(t); break; }
        case 0x0f: { jval t; t.d=1.0; PUSH(t); PUSH(t); break; }
        case 0x10: PUSHI(RD_S8(pc+1)); next=pc+2; break;    /* bipush */
        case 0x11: PUSHI(RD_S16(pc+1)); next=pc+3; break;   /* sipush */
        case 0x12: case 0x13: {   /* ldc / ldc_w */
            uint16_t idx;
            if(op==0x12){ idx=RD_U8(pc+1); next=pc+2; } else { idx=RD_U16(pc+1); next=pc+3; }
            if(idx<cls->cp_count){ const cf_cp_item *it=&cls->cp[idx];
                if(it->kind==CF_CP_INT) PUSHI(it->u.i);
                else if(it->kind==CF_CP_FLOAT) PUSHF(it->u.f);
                else if(it->kind==CF_CP_STRING){ jobject *s=jvm_new_object(vm,"java/lang/String");
                    if(s){ s->nfields=0; jval *fv=obj_field(s,"value",1); if(fv) fv->a=(void*)it->u.utf.s; } PUSHA(s); }
                else if(it->kind==CF_CP_CLASS){ PUSHA(NULL); }
                else PUSHI(0);
            } else PUSHI(0);
            break; }
        case 0x14: { uint16_t idx=RD_U16(pc+1); next=pc+3;  /* ldc2_w (long/double, 2 slots) */
            jval t; t.l=0;
            if(idx<cls->cp_count){ const cf_cp_item *it=&cls->cp[idx];
                if(it->kind==CF_CP_LONG) t.l=it->u.l; else if(it->kind==CF_CP_DOUBLE) t.d=it->u.d; }
            PUSH(t); PUSH(t); break; }
        case 0x15: case 0x17: case 0x19: PUSH(loc[RD_U8(pc+1)]); next=pc+2; break; /* iload/fload/aload */
        case 0x16: case 0x18: { uint8_t v=RD_U8(pc+1); PUSH(loc[v]); PUSH(loc[v]); next=pc+2; break; } /* lload/dload */
        case 0x1a: case 0x1b: case 0x1c: case 0x1d: PUSH(loc[op-0x1a]); break; /* iload_0..3 */
        case 0x22: case 0x23: case 0x24: case 0x25: PUSH(loc[op-0x22]); break; /* fload_0..3 */
        case 0x2a: case 0x2b: case 0x2c: case 0x2d: PUSH(loc[op-0x2a]); break; /* aload_0..3 */
        case 0x1e: case 0x1f: case 0x20: case 0x21: { int b=op-0x1e; PUSH(loc[b]); PUSH(loc[b]); break; } /* lload_0..3 */
        case 0x26: case 0x27: case 0x28: case 0x29: { int b=op-0x26; PUSH(loc[b]); PUSH(loc[b]); break; } /* dload_0..3 */
        case 0x36: case 0x38: case 0x3a: NEEDSTK(1); loc[RD_U8(pc+1)]=POP(); next=pc+2; break; /* istore/fstore/astore */
        case 0x37: case 0x39: { uint8_t v=RD_U8(pc+1); NEEDSTK(2); jval hi=POP(); (void)POP(); loc[v]=hi; next=pc+2; break; } /* lstore/dstore */
        case 0x3b: case 0x3c: case 0x3d: case 0x3e: NEEDSTK(1); loc[op-0x3b]=POP(); break; /* istore_0..3 */
        case 0x43: case 0x44: case 0x45: case 0x46: NEEDSTK(1); loc[op-0x43]=POP(); break; /* fstore_0..3 */
        case 0x4b: case 0x4c: case 0x4d: case 0x4e: NEEDSTK(1); loc[op-0x4b]=POP(); break; /* astore_0..3 */
        case 0x3f: case 0x40: case 0x41: case 0x42: { int b=op-0x3f; NEEDSTK(2); jval hi=POP(); (void)POP(); loc[b]=hi; break; } /* lstore_0..3 */
        case 0x47: case 0x48: case 0x49: case 0x4a: { int b=op-0x47; NEEDSTK(2); jval hi=POP(); (void)POP(); loc[b]=hi; break; } /* dstore_0..3 */
        /* array load/store */
        case 0x2e: case 0x30: case 0x33: case 0x34: case 0x35: case 0x32: { /* iaload/faload/baload/caload/saload/aaload */
            NEEDSTK(2); int ix=POP().i; jobject *a=(jobject*)POP().a;
            if(!heap_obj_ok(vm,a)||!a->is_array||ix<0||ix>=a->arr_len){ vm->error=1; snprintf(vm->errmsg,sizeof vm->errmsg,"array load oob @%u",pc); goto done; }
            PUSH(a->arr[ix]); break; }
        case 0x4f: case 0x51: case 0x54: case 0x55: case 0x56: case 0x53: { /* iastore/fastore/bastore/castore/sastore/aastore */
            NEEDSTK(3); jval val=POP(); int ix=POP().i; jobject *a=(jobject*)POP().a;
            if(!heap_obj_ok(vm,a)||!a->is_array||ix<0||ix>=a->arr_len){ vm->error=1; snprintf(vm->errmsg,sizeof vm->errmsg,"array store oob @%u",pc); goto done; }
            a->arr[ix]=val; break; }
        case 0x57: NEEDSTK(1); sp--; break;                 /* pop */
        case 0x58: NEEDSTK(2); sp-=2; break;                /* pop2 */
        case 0x59: NEEDSTK(1); PUSH(stk[sp-1]); break;      /* dup */
        case 0x5a: { NEEDSTK(2); jval a=stk[sp-1],b=stk[sp-2]; stk[sp-2]=a; stk[sp-1]=b; PUSH(a); break; } /* dup_x1 */
        case 0x5b: { NEEDSTK(3); jval v1=stk[sp-1],v2=stk[sp-2],v3=stk[sp-3]; /* dup_x2: ..,v3,v2,v1 -> ..,v1,v3,v2,v1 */
            stk[sp-3]=v1; stk[sp-2]=v3; stk[sp-1]=v2; PUSH(v1); break; }
        case 0x5c: { NEEDSTK(2); jval a=stk[sp-2],b=stk[sp-1]; PUSH(a); PUSH(b); break; } /* dup2 */
        case 0x5d: { NEEDSTK(3); jval v1=stk[sp-1],v2=stk[sp-2],v3=stk[sp-3]; /* dup2_x1: ..,v3,v2,v1 -> ..,v2,v1,v3,v2,v1 */
            stk[sp-3]=v2; stk[sp-2]=v1; stk[sp-1]=v3; PUSH(v2); PUSH(v1); break; }
        case 0x5e: { NEEDSTK(4); jval v1=stk[sp-1],v2=stk[sp-2],v3=stk[sp-3],v4=stk[sp-4]; /* dup2_x2 */
            stk[sp-4]=v2; stk[sp-3]=v1; stk[sp-2]=v4; stk[sp-1]=v3; PUSH(v2); PUSH(v1); break; }
        case 0x5f: { NEEDSTK(2); jval t=stk[sp-1]; stk[sp-1]=stk[sp-2]; stk[sp-2]=t; break; } /* swap */
        /* int arithmetic */
        case 0x60: { NEEDSTK(2); int b=POP().i,a=POP().i; PUSHI(a+b); break; }
        case 0x64: { NEEDSTK(2); int b=POP().i,a=POP().i; PUSHI(a-b); break; }
        case 0x68: { NEEDSTK(2); int b=POP().i,a=POP().i; PUSHI(a*b); break; }
        case 0x6c: { NEEDSTK(2); int b=POP().i,a=POP().i; if(b==0){vm->error=1;snprintf(vm->errmsg,sizeof vm->errmsg,"idiv/0 @%u",pc);goto done;} PUSHI((b==-1&&a==(int)0x80000000)?a:a/b); break; }
        case 0x70: { NEEDSTK(2); int b=POP().i,a=POP().i; if(b==0){vm->error=1;snprintf(vm->errmsg,sizeof vm->errmsg,"irem/0 @%u",pc);goto done;} PUSHI((b==-1)?0:a%b); break; }
        case 0x74: { NEEDSTK(1); int a=POP().i; PUSHI(-a); break; }
        case 0x78: { NEEDSTK(2); int b=POP().i,a=POP().i; PUSHI(a<<(b&31)); break; }
        case 0x7a: { NEEDSTK(2); int b=POP().i,a=POP().i; PUSHI(a>>(b&31)); break; }
        case 0x7c: { NEEDSTK(2); int b=POP().i; uint32_t a=(uint32_t)POP().i; PUSHI((int32_t)(a>>(b&31))); break; }
        case 0x7e: { NEEDSTK(2); int b=POP().i,a=POP().i; PUSHI(a&b); break; }
        case 0x80: { NEEDSTK(2); int b=POP().i,a=POP().i; PUSHI(a|b); break; }
        case 0x82: { NEEDSTK(2); int b=POP().i,a=POP().i; PUSHI(a^b); break; }
        case 0x84: { uint8_t v=RD_U8(pc+1); loc[v].i+=RD_S8(pc+2); next=pc+3; break; } /* iinc */
        /* float arithmetic */
        case 0x62: { NEEDSTK(2); float b=POP().f,a=POP().f; PUSHF(a+b); break; }
        case 0x66: { NEEDSTK(2); float b=POP().f,a=POP().f; PUSHF(a-b); break; }
        case 0x6a: { NEEDSTK(2); float b=POP().f,a=POP().f; PUSHF(a*b); break; }
        case 0x6e: { NEEDSTK(2); float b=POP().f,a=POP().f; PUSHF(a/b); break; }
        case 0x76: { NEEDSTK(1); float a=POP().f; PUSHF(-a); break; }
        /* conversions (int/float subset) */
        case 0x86: { NEEDSTK(1); int a=POP().i; PUSHF((float)a); break; }  /* i2f */
        case 0x8b: { NEEDSTK(1); float a=POP().f; PUSHI((int)a); break; }  /* f2i */
        case 0x91: { NEEDSTK(1); int a=POP().i; PUSHI((int8_t)a); break; } /* i2b */
        case 0x92: { NEEDSTK(1); int a=POP().i; PUSHI((uint16_t)a); break; } /* i2c */
        case 0x93: { NEEDSTK(1); int a=POP().i; PUSHI((int16_t)a); break; } /* i2s */
        case 0x95: case 0x96: { NEEDSTK(2); float b=POP().f,a=POP().f; PUSHI(a>b?1:(a<b?-1:(a==b?0:(op==0x95?-1:1)))); break; } /* fcmpl/g */
        /* conditional branches (one-operand) */
        case 0x99: { NEEDSTK(1); int a=POP().i; if(a==0){ next=pc+RD_S16(pc+1); } else next=pc+3; break; }
        case 0x9a: { NEEDSTK(1); int a=POP().i; if(a!=0){ next=pc+RD_S16(pc+1); } else next=pc+3; break; }
        case 0x9b: { NEEDSTK(1); int a=POP().i; if(a<0){ next=pc+RD_S16(pc+1); } else next=pc+3; break; }
        case 0x9c: { NEEDSTK(1); int a=POP().i; if(a>=0){ next=pc+RD_S16(pc+1); } else next=pc+3; break; }
        case 0x9d: { NEEDSTK(1); int a=POP().i; if(a>0){ next=pc+RD_S16(pc+1); } else next=pc+3; break; }
        case 0x9e: { NEEDSTK(1); int a=POP().i; if(a<=0){ next=pc+RD_S16(pc+1); } else next=pc+3; break; }
        /* two-operand int compares */
        case 0x9f: { NEEDSTK(2); int b=POP().i,a=POP().i; next=(a==b)?pc+RD_S16(pc+1):pc+3; break; }
        case 0xa0: { NEEDSTK(2); int b=POP().i,a=POP().i; next=(a!=b)?pc+RD_S16(pc+1):pc+3; break; }
        case 0xa1: { NEEDSTK(2); int b=POP().i,a=POP().i; next=(a<b) ?pc+RD_S16(pc+1):pc+3; break; }
        case 0xa2: { NEEDSTK(2); int b=POP().i,a=POP().i; next=(a>=b)?pc+RD_S16(pc+1):pc+3; break; }
        case 0xa3: { NEEDSTK(2); int b=POP().i,a=POP().i; next=(a>b) ?pc+RD_S16(pc+1):pc+3; break; }
        case 0xa4: { NEEDSTK(2); int b=POP().i,a=POP().i; next=(a<=b)?pc+RD_S16(pc+1):pc+3; break; }
        case 0xa5: { NEEDSTK(2); void *b=POP().a,*a=POP().a; next=(a==b)?pc+RD_S16(pc+1):pc+3; break; }
        case 0xa6: { NEEDSTK(2); void *b=POP().a,*a=POP().a; next=(a!=b)?pc+RD_S16(pc+1):pc+3; break; }
        case 0xc6: { NEEDSTK(1); void *a=POP().a; next=(a==NULL)?pc+RD_S16(pc+1):pc+3; break; } /* ifnull */
        case 0xc7: { NEEDSTK(1); void *a=POP().a; next=(a!=NULL)?pc+RD_S16(pc+1):pc+3; break; } /* ifnonnull */
        case 0xa7: next=pc+RD_S16(pc+1); break;             /* goto */
        case 0xc8: next=pc+RD_S32(pc+1); break;             /* goto_w */
        case 0xaa: { /* tableswitch */
            uint32_t base=(pc+4)&~3u;
            int32_t def=RD_S32(base), low=RD_S32(base+4), high=RD_S32(base+8);
            NEEDSTK(1); int idx=POP().i;
            if(idx<low||idx>high) next=pc+def;
            else next=pc+RD_S32(base+12+(uint32_t)(idx-low)*4);
            break; }
        case 0xab: { /* lookupswitch */
            uint32_t base=(pc+4)&~3u;
            int32_t def=RD_S32(base); uint32_t np=(uint32_t)RD_S32(base+4);
            NEEDSTK(1); int key=POP().i; uint32_t t=pc+def;
            for(uint32_t k=0;k<np;k++){ int32_t mk=RD_S32(base+8+k*8); if(mk==key){ t=pc+RD_S32(base+8+k*8+4); break; } }
            next=t; break; }
        /* returns */
        case 0xac: case 0xae: case 0xb0: NEEDSTK(1); ret=POP(); goto done; /* ireturn/freturn/areturn */
        case 0xad: case 0xaf: NEEDSTK(2); { jval hi=POP(); (void)POP(); ret=hi; } goto done; /* lreturn/dreturn */
        case 0xb1: goto done;                               /* return */
        /* object / field */
        case 0xbb: { uint16_t idx=RD_U16(pc+1); next=pc+3;  /* new */
            const char *cn=(idx<cls->cp_count&&cls->cp[idx].kind==CF_CP_CLASS)?cls->cp[idx].u.cls:"?";
            jobject *o=jvm_new_object(vm,cn); if(vm->error) goto done; PUSHA(o); break; }
        case 0xbc: { uint8_t atype=RD_U8(pc+1); next=pc+2;  /* newarray */
            NEEDSTK(1); int len=POP().i;
            static const char K[]={0,0,0,0,'Z','C','F','D','B','S','I','J'};
            char k=(atype>=4&&atype<=11)?K[atype]:'I';
            jobject *a=jvm_new_array(vm,k,len); if(vm->error) goto done; PUSHA(a); break; }
        case 0xbd: { next=pc+3; NEEDSTK(1); int len=POP().i; jobject *a=jvm_new_array(vm,'L',len); if(vm->error) goto done; PUSHA(a); break; } /* anewarray */
        case 0xbe: { NEEDSTK(1); jobject *a=(jobject*)POP().a; if(!heap_obj_ok(vm,a)||!a->is_array){vm->error=1;snprintf(vm->errmsg,sizeof vm->errmsg,"arraylength on non-array @%u",pc);goto done;} PUSHI(a->arr_len); break; } /* arraylength */
        case 0xb4: { uint16_t idx=RD_U16(pc+1); next=pc+3;  /* getfield */
            NEEDSTK(1); jobject *o=(jobject*)POP().a;
            const char *fn=(idx<cls->cp_count)?cls->cp[idx].u.ref.name:NULL;
            if(!heap_obj_ok(vm,o)){vm->error=1;snprintf(vm->errmsg,sizeof vm->errmsg,"getfield on null/invalid ref @%u",pc);goto done;}
            jval *f=fn?obj_field(o,fn,1):NULL; jval v; v.l=f?f->l:0; PUSH(v); break; }
        case 0xb5: { uint16_t idx=RD_U16(pc+1); next=pc+3;  /* putfield */
            NEEDSTK(2); jval v=POP(); jobject *o=(jobject*)POP().a;
            const char *fn=(idx<cls->cp_count)?cls->cp[idx].u.ref.name:NULL;
            if(!heap_obj_ok(vm,o)){vm->error=1;snprintf(vm->errmsg,sizeof vm->errmsg,"putfield on null/invalid ref @%u",pc);goto done;}
            jval *f=fn?obj_field(o,fn,1):NULL; if(f) *f=v; break; }
        case 0xb2: { uint16_t idx=RD_U16(pc+1); next=pc+3;  /* getstatic */
            ensure_clinit(vm); if(vm->error) goto done;
            const char *fn=(idx<cls->cp_count)?cls->cp[idx].u.ref.name:NULL;
            const char *fd=(idx<cls->cp_count)?cls->cp[idx].u.ref.desc:NULL;
            jval *s=static_slot(vm,fn,1); jval v; v.l=s?s->l:0;
            PUSH(v); if(fd&&(fd[0]=='J'||fd[0]=='D')) PUSH(v); break; }
        case 0xb3: { uint16_t idx=RD_U16(pc+1); next=pc+3;  /* putstatic */
            const char *fn=(idx<cls->cp_count)?cls->cp[idx].u.ref.name:NULL;
            const char *fd=(idx<cls->cp_count)?cls->cp[idx].u.ref.desc:NULL;
            int cat2=(fd&&(fd[0]=='J'||fd[0]=='D'));
            NEEDSTK(cat2?2:1);
            jval v=POP(); if(cat2) (void)POP();   /* hi slot holds the value */
            ensure_clinit(vm); if(vm->error) goto done;
            jval *s=static_slot(vm,fn,1); if(s) *s=v; break; }
        case 0xc0: next=pc+3; break;                        /* checkcast: no-op */
        case 0xc1: { uint16_t idx=RD_U16(pc+1); (void)idx; NEEDSTK(1); jobject *o=(jobject*)POP().a; PUSHI(o?1:0); next=pc+3; break; } /* instanceof: approx */
        /* monitorenter/monitorexit: single-threaded VM -> pop objectref, no lock.
         * Real scripts wrap event bodies in synchronized(this){...}. */
        case 0xc2: case 0xc3: NEEDSTK(1); sp--; break;
        /* method invocation */
        case 0xb6: case 0xb7: case 0xb8: case 0xb9: {        /* invokevirtual/special/static/interface */
            uint16_t idx=RD_U16(pc+1);
            next = (op==0xb9) ? pc+5 : pc+3;
            if(idx>=cls->cp_count){ vm->error=1; snprintf(vm->errmsg,sizeof vm->errmsg,"bad methodref #%u @%u",idx,pc); goto done; }
            const cf_cp_item *it=&cls->cp[idx];
            const char *ocls=it->u.ref.cls, *nm=it->u.ref.name, *desc=it->u.ref.desc;
            if(!nm||!desc){ vm->error=1; snprintf(vm->errmsg,sizeof vm->errmsg,"unresolved methodref @%u",pc); goto done; }
            int argslots=jvm_desc_arg_slots(desc);
            int has_this=(op!=0xb8);                        /* static has no objectref */
            char rk=jvm_desc_ret_kind(desc);

            /* internal call? (same class, method present) */
            const cf_method *target=NULL;
            if(op!=0xb9 && ocls && cls->this_class && strcmp(ocls,cls->this_class)==0)
                target=cf_find_method(cls,nm,desc);

            NEEDSTK(argslots+(has_this?1:0));
            /* collect args (one jval per declared arg; cat-2 collapses its 2 slots) */
            int nparm=jvm_desc_arg_count(desc);
            jval argbuf[32]; if(nparm>32) nparm=32;
            /* pop raw slots into a temp then map; simplest: pop slots, keep order */
            /* Because args were pushed left-to-right, pop into reversed slot array. */
            int topslots=argslots; jval slots_raw[64]; if(topslots>64) topslots=64;
            for(int k=topslots-1;k>=0;k--) slots_raw[k]=POP();
            jobject *thisref=NULL; if(has_this) thisref=(jobject*)POP().a;
            /* map slots_raw -> argbuf per descriptor (cat-2 uses 2 slots) */
            { const char *p=desc+1; int si=0, ai=0;
              while(*p && *p!=')' && ai<nparm){
                char c=*p;
                if(c=='['){ while(*p=='[')p++; if(*p=='L'){while(*p&&*p!=';')p++;} if(*p)p++; argbuf[ai++]=slots_raw[si++]; continue; }
                if(c=='L'){ while(*p&&*p!=';')p++; if(*p)p++; argbuf[ai++]=slots_raw[si++]; continue; }
                if(c=='J'||c=='D'){ argbuf[ai++]=slots_raw[si]; si+=2; p++; continue; }
                argbuf[ai++]=slots_raw[si++]; p++;
              } }

            jval r; r.l=0;
            if(target){
                r=jvm_invoke(vm,target,argbuf,nparm);
                if(vm->error) goto done;
            } else if(nm && strcmp(nm,"<init>")==0 &&
                      (!ocls || strcmp(ocls,"java/lang/Object")==0)){
                /* Object.<init> and bare constructors: no-op */
            } else {
                /* native bridge dispatch */
                char key[256];
                snprintf(key,sizeof key,"%s.%s:%s",ocls?ocls:"?",nm,desc);
                jvm_native_entry *e=nat_find(vm,key);
                vm->native_calls++;
                if(vm->native_hook) vm->native_hook(vm,key);
                vm->bridge_this=thisref;   /* receiver for engine-backed handlers */
                if(e&&e->fn) r=e->fn(vm,key,argbuf,nparm,desc);
                else r=default_stub(vm,key,argbuf,nparm,desc);
                vm->bridge_this=NULL;
                if(vm->error) goto done;
            }
            if(rk!='V'){ if(rk=='J'||rk=='D'){ PUSH(r); PUSH(r);} else PUSH(r); }
            break; }
        case 0xc4: { /* wide */
            uint8_t wop=RD_U8(pc+1); uint16_t v=RD_U16(pc+2);
            if(wop==0x84){ loc[v].i+=RD_S16(pc+4); next=pc+6; }
            else { /* wide load/store */
                switch(wop){
                case 0x15: case 0x17: case 0x19: PUSH(loc[v]); break;
                case 0x36: case 0x38: case 0x3a: NEEDSTK(1); loc[v]=POP(); break;
                default: break;
                }
                next=pc+4;
            }
            break; }
        default:
            vm->error=1; snprintf(vm->errmsg,sizeof vm->errmsg,"unimplemented opcode 0x%02x @%u",op,pc); goto done;
        }
        pc=next;
    }
done:
    vm->call_depth--;
    free(loc); free(stk);
    return ret;
}
