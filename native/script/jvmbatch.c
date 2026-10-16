/*
 * jvmbatch.c - run the whole decrypted-script corpus through the native VM and
 * measure coverage.  For every .class given on the command line (or listed, one
 * path per line, on stdin when no paths are given) this:
 *
 *   1. decrypts+parses it (classfile.c),
 *   2. invokes every ZERO-ARG non-<init> code-bearing method (the game's real
 *      event entry points, e.g. gmap_exec/calc) under the stub bridge table and
 *      a bounded step limit.  Intra-class helpers that take arguments run for
 *      real, transitively, through same-class invokes - so this faithfully
 *      exercises the whole script without fabricating argument values.
 *
 * Records, per script and in aggregate: parse OK, exec outcome (complete / step
 * limit / interpreter error + category), the opcode histogram across all runs,
 * and the set of distinct native-bridge methods actually called.
 *
 * It NEVER executes a .class as native code: the bytes only ever run inside the
 * interpreter.  Byte scanning for the corpus list is bounds-checked.
 *
 * Usage:
 *   jvmbatch <file.class> ...            # explicit files
 *   jvmbatch < list.txt                  # newline-separated paths on stdin
 *   jvmbatch --limit N ...               # per-method step cap (default 2000000)
 */
#define _POSIX_C_SOURCE 200809L   /* strdup / getline / ssize_t */
#include "classfile.h"
#include "jvm.h"
#include "bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- aggregate opcode histogram --------------------------------------- */
static uint64_t g_ophist[256];

/* ---- aggregate bridge-key set (key -> call count) --------------------- */
typedef struct { char *key; uint64_t count; } brow;
static brow  *g_bridge; static int g_bn, g_bcap;
static void bridge_record(const char *key){
    for(int i=0;i<g_bn;i++) if(strcmp(g_bridge[i].key,key)==0){ g_bridge[i].count++; return; }
    if(g_bn==g_bcap){ g_bcap=g_bcap?g_bcap*2:256; g_bridge=realloc(g_bridge,(size_t)g_bcap*sizeof(brow)); }
    g_bridge[g_bn].key=strdup(key); g_bridge[g_bn].count=1; g_bn++;
}
static void on_native(jvm *vm,const char *key){ (void)vm; bridge_record(key); }

/* ---- error categorisation --------------------------------------------- */
enum { OUT_COMPLETE=0, OUT_STEP, OUT_DEPTH, OUT_UNIMPL, OUT_UNDERFLOW, OUT_OVERFLOW,
       OUT_ARRAY_OOB, OUT_NULL, OUT_DIV0, OUT_HEAP, OUT_BADREF, OUT_OTHER, OUT__N };
static const char *OUTNAME[OUT__N]={
    "complete","step-limit","call-depth(poll-loop)","unimpl-opcode","stack-underflow","stack-overflow",
    "array-oob","null-ref","div-by-zero","heap-exhausted","bad/unresolved-ref","other-error" };
static int categorize(const char *msg){
    if(strstr(msg,"unimplemented opcode")) return OUT_UNIMPL;
    if(strstr(msg,"call depth"))           return OUT_DEPTH;
    if(strstr(msg,"step limit"))           return OUT_STEP;
    if(strstr(msg,"underflow"))            return OUT_UNDERFLOW;
    if(strstr(msg,"overflow"))             return OUT_OVERFLOW;
    if(strstr(msg,"oob")||strstr(msg,"negative array")) return OUT_ARRAY_OOB;
    if(strstr(msg,"null")||strstr(msg,"non-array"))     return OUT_NULL;
    if(strstr(msg,"/0"))                   return OUT_DIV0;
    if(strstr(msg,"heap exhausted"))       return OUT_HEAP;
    if(strstr(msg,"methodref")||strstr(msg,"no Code")) return OUT_BADREF;
    return OUT_OTHER;
}

/* keep a sample errmsg per category for the report */
static char g_sample[OUT__N][200];
static uint64_t g_outcome_methods[OUT__N];   /* per-method tally */

static int desc_is_zero_arg(const char *desc){
    return desc && desc[0]=='(' && desc[1]==')';
}

int main(int argc,char**argv){
    long limit=2000000;
    const char **files=calloc((size_t)argc,sizeof(char*)); int nf=0;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--limit")&&i+1<argc){ limit=strtol(argv[++i],NULL,10); continue; }
        files[nf++]=argv[i];
    }
    /* stdin list when no explicit files */
    char **owned=NULL; int nowned=0;
    if(nf==0){
        size_t cap=0; char *line=NULL; ssize_t n;
        while((n=getline(&line,&cap,stdin))>0){
            while(n>0 && (line[n-1]=='\n'||line[n-1]=='\r'||line[n-1]==' ')) line[--n]=0;
            if(n==0) continue;
            owned=realloc(owned,(size_t)(nowned+1)*sizeof(char*));
            owned[nowned]=strdup(line); files=realloc(files,(size_t)(nowned+1)*sizeof(char*));
            files[nowned]=owned[nowned]; nowned++;
        }
        free(line); nf=nowned;
    }

    int n_total=0, n_parse_ok=0, n_parse_fail=0;
    int n_no_entry=0;              /* parsed but no zero-arg entry method       */
    int n_exec_clean=0;           /* >=1 entry, ALL entries ran to completion  */
    int n_exec_partial=0;         /* some entry completed, some errored        */
    int n_exec_fail=0;            /* >=1 entry, NONE completed                 */
    uint64_t tot_methods=0, tot_insns=0, tot_native=0;

    printf("# per-script results (script\\tentries\\tclean\\tfail\\tworst\\tclass)\n");

    for(int fi=0; fi<nf; fi++){
        const char *path=files[fi];
        n_total++;
        cf_class cls; const char *err=NULL;
        if(cf_load(path,&cls,&err)!=0){
            n_parse_fail++;
            printf("PARSEFAIL\t%s\t%s\n", path, err?err:"?");
            continue;
        }
        n_parse_ok++;

        int entries=0, clean=0, failed=0, worst=OUT_COMPLETE;
        for(uint16_t mi=0; mi<cls.method_count; mi++){
            const cf_method *m=&cls.methods[mi];
            if(!m->name || !m->code) continue;
            if(strcmp(m->name,"<init>")==0) continue;
            if(strcmp(m->name,"<clinit>")==0) continue; /* runs lazily on first static access */
            /* entry points: any no-arg method, plus the conventional
             * main([Ljava/lang/String;)V invoked with an empty argv (a real
             * "no command-line args" call, not a fabricated argument). */
            int is_main = m->desc && strcmp(m->desc,"([Ljava/lang/String;)V")==0;
            if(!desc_is_zero_arg(m->desc) && !is_main) continue;
            entries++;

            jvm vm; jvm_init(&vm,&cls);
            DirgeEngine eng; dirge_engine_init(&eng);
            bridge_install(&vm,&eng);
            vm.native_hook=on_native;
            vm.quiet=1;
            vm.step_limit=(uint64_t)limit;

            jval a0[1]; int na=0;
            if(is_main){ a0[0].a=jvm_new_array(&vm,'L',0); na=1; }
            jvm_invoke(&vm,m, na?a0:NULL, na);

            /* fold instrumentation into globals */
            for(int o=0;o<256;o++) g_ophist[o]+=vm.op_hist[o];
            tot_insns+=vm.insn_count; tot_native+=vm.native_calls; tot_methods++;

            int out;
            if(vm.error){ out=categorize(vm.errmsg);
                if(!g_sample[out][0]) snprintf(g_sample[out],sizeof g_sample[out],"%s (%s::%s)",vm.errmsg,cls.this_class?cls.this_class:"?",m->name);
            } else out=OUT_COMPLETE;
            g_outcome_methods[out]++;
            if(out==OUT_COMPLETE) clean++; else { failed++; if(out>worst) worst=out; }

            jvm_destroy(&vm);
        }

        if(entries==0){ n_no_entry++;
            printf("NOENTRY\t%s\tclass=%s\n", path, cls.this_class?cls.this_class:"?");
        } else {
            if(failed==0) n_exec_clean++;
            else if(clean>0) n_exec_partial++;
            else n_exec_fail++;
            printf("%s\t%s\tentries=%d\tclean=%d\tfail=%d\tworst=%s\tclass=%s\n",
                   failed==0?"OK":(clean>0?"PARTIAL":"FAIL"),
                   path, entries, clean, failed, OUTNAME[worst],
                   cls.this_class?cls.this_class:"?");
        }
        cf_free(&cls);
    }

    /* ================= REPORT ================= */
    printf("\n========================= COVERAGE REPORT =========================\n");
    printf("scripts total          : %d\n", n_total);
    printf("  parsed OK            : %d\n", n_parse_ok);
    printf("  parse failed         : %d\n", n_parse_fail);
    printf("  parsed, no 0-arg entry: %d\n", n_no_entry);
    printf("scripts with entries   : %d\n", n_exec_clean+n_exec_partial+n_exec_fail);
    printf("  all entries clean    : %d\n", n_exec_clean);
    printf("  partial (some clean) : %d\n", n_exec_partial);
    printf("  all entries errored  : %d\n", n_exec_fail);
    printf("entry methods invoked  : %llu\n",(unsigned long long)tot_methods);
    printf("  instructions executed: %llu\n",(unsigned long long)tot_insns);
    printf("  native-bridge calls  : %llu\n",(unsigned long long)tot_native);

    printf("\n---- per-method outcome tally ----\n");
    for(int i=0;i<OUT__N;i++) if(g_outcome_methods[i])
        printf("  %-20s %8llu   %s\n", OUTNAME[i],(unsigned long long)g_outcome_methods[i],
               g_sample[i][0]?g_sample[i]:"");

    printf("\n---- opcode histogram (opcodes exercised across all scripts) ----\n");
    int distinct=0;
    for(int o=0;o<256;o++) if(g_ophist[o]){ distinct++;
        printf("  0x%02x  %12llu\n", o,(unsigned long long)g_ophist[o]); }
    printf("  (%d distinct opcodes exercised)\n", distinct);

    /* sort bridge keys by count desc for prioritisation */
    for(int i=0;i<g_bn;i++) for(int j=i+1;j<g_bn;j++)
        if(g_bridge[j].count>g_bridge[i].count){ brow t=g_bridge[i]; g_bridge[i]=g_bridge[j]; g_bridge[j]=t; }
    printf("\n---- distinct native-bridge methods actually called (%d) ----\n", g_bn);
    printf("     (sorted by call count; these are the bridges that matter)\n");
    for(int i=0;i<g_bn;i++)
        printf("  %8llu  %s\n",(unsigned long long)g_bridge[i].count, g_bridge[i].key);

    for(int i=0;i<g_bn;i++) free(g_bridge[i].key);
    free(g_bridge);
    for(int i=0;i<nowned;i++) free(owned[i]);
    free(owned); free((void*)files);
    return (n_parse_fail||n_exec_fail)?1:0;
}
