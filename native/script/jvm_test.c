/*
 * jvm_test.c - deterministic self-test for the JVM-subset interpreter.
 *
 * Builds minimal classfile methods BY HAND (raw bytecode + a tiny resolved
 * constant pool, no container/decrypt needed) and checks the interpreter
 * returns the spec-correct result. Covers: int arithmetic, loads/stores,
 * a loop (branches + iinc + goto), the object/array model (newarray/iastore/
 * iaload/arraylength), and native-bridge dispatch (invokestatic -> C handler).
 *
 * Exit 0 = all pass. Prints PASS/FAIL per case.
 */
#include "jvm.h"
#include "classfile.h"
#include <stdio.h>
#include <string.h>

static int fails=0;
static void check(const char *name,long got,long want){
    int ok=(got==want);
    printf("  [%s] %-28s got=%ld want=%ld\n", ok?"PASS":"FAIL", name, got, want);
    if(!ok) fails++;
}

/* run a hand-built method on a fresh vm, with cp `cp` of `cpn` items */
static jval run(cf_cp_item *cp,uint16_t cpn,const char *this_class,
                const uint8_t *code,uint32_t clen,uint16_t maxloc,uint16_t maxstk,
                const jval *args,int nargs, jvm **out_vm_for_natives){
    static cf_class cls;       /* reused; fields are plain pointers */
    static cf_method md;
    memset(&cls,0,sizeof cls); memset(&md,0,sizeof md);
    cls.cp=cp; cls.cp_count=cpn; cls.this_class=this_class;
    md.name="t"; md.desc="()I"; md.code=code; md.code_len=clen;
    md.max_locals=maxloc; md.max_stack=maxstk;
    cls.methods=&md; cls.method_count=1;

    static jvm vm; jvm_init(&vm,&cls);
    if(out_vm_for_natives) *out_vm_for_natives=&vm;  /* caller registers natives then calls run again? no */
    return jvm_invoke(&vm,&md,args,nargs);
}

/* native: int add(int,int) */
static jval nat_add(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)vm;(void)k;(void)d; jval r; r.i=(n>=2)?a[0].i+a[1].i:0; return r; }

int main(void){
    printf("== JVM interpreter self-test ==\n");

    /* ---- Case 1: (a+b)*2  with a=20,b=1 -> 42 ------------------------- */
    {
        static const uint8_t code[]={
            0x1a,       /* iload_0  */
            0x1b,       /* iload_1  */
            0x60,       /* iadd     */
            0x05,       /* iconst_2 */
            0x68,       /* imul     */
            0xac        /* ireturn  */
        };
        jval args[2]; args[0].i=20; args[1].i=1;
        jval r=run(NULL,0,NULL,code,sizeof code,2,2,args,2,NULL);
        check("(a+b)*2", r.i, 42);
    }

    /* ---- Case 2: sum 1..n loop (branches+iinc+goto), n=10 -> 55 ------- */
    {
        static const uint8_t code[]={
            0x03,              /* 0: iconst_0        */
            0x3c,              /* 1: istore_1  sum=0 */
            0x04,              /* 2: iconst_1        */
            0x3d,              /* 3: istore_2  i=1   */
            0x1c,              /* 4: iload_2   (L)   */
            0x1a,              /* 5: iload_0         */
            0xa3,0x00,0x0d,    /* 6: if_icmpgt +13 -> 19 */
            0x1b,              /* 9: iload_1         */
            0x1c,              /*10: iload_2         */
            0x60,              /*11: iadd            */
            0x3c,              /*12: istore_1 sum+=i */
            0x84,0x02,0x01,    /*13: iinc 2,1        */
            0xa7,0xff,0xf4,    /*16: goto -12 -> 4   */
            0x1b,              /*19: iload_1  (END)  */
            0xac               /*20: ireturn         */
        };
        jval args[1]; args[0].i=10;
        jval r=run(NULL,0,NULL,code,sizeof code,3,2,args,1,NULL);
        check("sum(1..10)", r.i, 55);
    }

    /* ---- Case 3: array model: arr[5]; arr[2]=99; arr[2]+arr.length -> 104 */
    {
        static const uint8_t code[]={
            0x08,       /* 0: iconst_5      */
            0xbc,0x0a,  /* 1: newarray int  */
            0x4b,       /* 3: astore_0      */
            0x2a,       /* 4: aload_0       */
            0x05,       /* 5: iconst_2      */
            0x10,0x63,  /* 6: bipush 99     */
            0x4f,       /* 8: iastore       */
            0x2a,       /* 9: aload_0       */
            0x05,       /*10: iconst_2      */
            0x2e,       /*11: iaload        */
            0x2a,       /*12: aload_0       */
            0xbe,       /*13: arraylength   */
            0x60,       /*14: iadd          */
            0xac        /*15: ireturn       */
        };
        jval r=run(NULL,0,NULL,code,sizeof code,1,4,NULL,0,NULL);
        check("array iastore/iaload/len", r.i, 104);
    }

    /* ---- Case 4: native bridge dispatch: Foo.add(5,37) -> 42 ---------- */
    {
        static cf_cp_item cp[2];
        memset(cp,0,sizeof cp);
        cp[1].kind=CF_CP_METHODREF;
        cp[1].u.ref.cls="test/Foo"; cp[1].u.ref.name="add"; cp[1].u.ref.desc="(II)I";
        static const uint8_t code[]={
            0x1a,            /* 0: iload_0        */
            0x1b,            /* 1: iload_1        */
            0xb8,0x00,0x01,  /* 2: invokestatic #1 */
            0xac             /* 5: ireturn        */
        };
        /* build the vm manually so we can register the native before invoking */
        static cf_class cls; static cf_method md;
        memset(&cls,0,sizeof cls); memset(&md,0,sizeof md);
        cls.cp=cp; cls.cp_count=2; cls.this_class="test/Caller";
        md.name="t"; md.desc="()I"; md.code=code; md.code_len=sizeof code; md.max_locals=2; md.max_stack=2;
        cls.methods=&md; cls.method_count=1;
        jvm vm; jvm_init(&vm,&cls);
        jvm_register_native(&vm,"test/Foo.add:(II)I",nat_add);
        jval args[2]; args[0].i=5; args[1].i=37;
        jval r=jvm_invoke(&vm,&md,args,2);
        check("native add(5,37)", r.i, 42);
        check("bridge-call tally", (long)vm.native_calls, 1);
        jvm_destroy(&vm);
    }

    /* ---- Case 5: dup_x2 : push 3,5,7 -> (7,3,5,7), sum via 3x iadd -> 22 -- */
    {
        static const uint8_t code[]={
            0x10,0x03,  /* bipush 3 */
            0x10,0x05,  /* bipush 5 */
            0x10,0x07,  /* bipush 7 */
            0x5b,       /* dup_x2 -> stack 7,3,5,7 */
            0x60,       /* iadd */
            0x60,       /* iadd */
            0x60,       /* iadd */
            0xac        /* ireturn */
        };
        jval r=run(NULL,0,NULL,code,sizeof code,1,5,NULL,0,NULL);
        check("dup_x2 (3+5+7+7)", r.i, 22);
    }

    /* ---- Case 6: monitorenter/monitorexit are no-ops that pop one ref ---- */
    {
        static const uint8_t code[]={
            0x01,       /* aconst_null */
            0xc2,       /* monitorenter (pops null) */
            0x10,0x2a,  /* bipush 42 */
            0x01,       /* aconst_null */
            0xc3,       /* monitorexit (pops null) */
            0xac        /* ireturn 42 */
        };
        jval r=run(NULL,0,NULL,code,sizeof code,1,2,NULL,0,NULL);
        check("monitorenter/exit no-op", r.i, 42);
    }

    /* ---- Case 7: call-depth guard raises a clean error, never crashes ---- */
    {
        /* method t() that invokestatic this.t():()I forever -> must error, not
         * segfault. cp[1] is a self methodref; name "t" matches run()'s md. */
        static cf_cp_item cp[2]; memset(cp,0,sizeof cp);
        cp[1].kind=CF_CP_METHODREF;
        cp[1].u.ref.cls="test/Rec"; cp[1].u.ref.name="t"; cp[1].u.ref.desc="()I";
        static const uint8_t code[]={ 0xb8,0x00,0x01, 0xac }; /* invokestatic #1; ireturn */
        static cf_class cls; static cf_method md;
        memset(&cls,0,sizeof cls); memset(&md,0,sizeof md);
        cls.cp=cp; cls.cp_count=2; cls.this_class="test/Rec";
        md.name="t"; md.desc="()I"; md.code=code; md.code_len=sizeof code; md.max_locals=1; md.max_stack=2;
        cls.methods=&md; cls.method_count=1;
        jvm vm; jvm_init(&vm,&cls);
        jvm_invoke(&vm,&md,NULL,0);
        check("call-depth guard trips", vm.error?1:0, 1);
        jvm_destroy(&vm);
    }

    /* ---- Case 8: static field + lazy <clinit> : clinit sets S.val=99, --- */
    /*             t() getstatics it -> 99 (proves clinit runs before use) -- */
    {
        static cf_cp_item cp[2]; memset(cp,0,sizeof cp);
        cp[1].kind=CF_CP_FIELDREF;
        cp[1].u.ref.cls="test/S"; cp[1].u.ref.name="val"; cp[1].u.ref.desc="I";
        static const uint8_t clinit_code[]={ 0x10,0x63, 0xb3,0x00,0x01, 0xb1 }; /* bipush99;putstatic#1;return */
        static const uint8_t t_code[]={ 0xb2,0x00,0x01, 0xac };                 /* getstatic#1;ireturn */
        static cf_class cls; static cf_method md[2];
        memset(&cls,0,sizeof cls); memset(md,0,sizeof md);
        md[0].name="<clinit>"; md[0].desc="()V"; md[0].code=clinit_code; md[0].code_len=sizeof clinit_code; md[0].max_locals=0; md[0].max_stack=2;
        md[1].name="t";        md[1].desc="()I"; md[1].code=t_code;      md[1].code_len=sizeof t_code;      md[1].max_locals=0; md[1].max_stack=2;
        cls.cp=cp; cls.cp_count=2; cls.this_class="test/S"; cls.methods=md; cls.method_count=2;
        jvm vm; jvm_init(&vm,&cls);
        jval r=jvm_invoke(&vm,&md[1],NULL,0);
        check("static field via <clinit>", r.i, 99);
        jvm_destroy(&vm);
    }

    printf(fails? "\nSELF-TEST FAILED (%d)\n" : "\nSELF-TEST OK\n", fails);
    return fails?1:0;
}
