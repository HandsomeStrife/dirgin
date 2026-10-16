/*
 * jvmrun.c - load a decrypted Dirge .class and RUN a method on the native VM.
 *
 * Usage:
 *   jvmrun [--trace[=N]] [--method NAME] [--seed] <file.class>
 *     --trace[=N]   print the first N instructions executed (default 80)
 *     --method NAME entry method (default: calc, else main, else first real)
 *     --seed        seed engine flag/stat arrays with a deterministic pattern
 *
 * Shows: entry, a bounded instruction/stack trace, native-bridge call tally,
 * and the engine state the script computed (section/total stats).
 */
#include "classfile.h"
#include "jvm.h"
#include "bridge.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void dump_nonzero(const char *label,const int *arr,int n){
    printf("  %s:",label); int any=0;
    for(int i=0;i<n;i++) if(arr[i]){ printf(" [%d]=%d",i,arr[i]); any=1; }
    if(!any) printf(" (all zero)");
    putchar('\n');
}

int main(int argc,char**argv){
    const char *path=NULL,*method=NULL; int trace=0; long tbudget=80; int seed=0; long limit=0;
    for(int i=1;i<argc;i++){
        if(!strncmp(argv[i],"--trace",7)){ trace=1; if(argv[i][7]=='=') tbudget=strtol(argv[i]+8,NULL,10); }
        else if(!strcmp(argv[i],"--method")&&i+1<argc) method=argv[++i];
        else if(!strcmp(argv[i],"--limit")&&i+1<argc) limit=strtol(argv[++i],NULL,10);
        else if(!strcmp(argv[i],"--seed")) seed=1;
        else if((!strcmp(argv[i],"--eflag")||!strcmp(argv[i],"--zflag"))&&i+1<argc) i++; /* 2nd pass */
        else if(!path) path=argv[i];
    }
    if(!path){ fprintf(stderr,"usage: %s [--trace[=N]] [--method NAME] [--seed] <file.class>\n",argv[0]); return 2; }

    cf_class cls; const char *err=NULL;
    if(cf_load(path,&cls,&err)!=0){ fprintf(stderr,"load failed: %s\n",err); return 1; }

    printf("== loaded %s ==\n",path);
    printf("class=%s super=%s methods=%u cp=%u\n",
           cls.this_class?cls.this_class:"?", cls.super_class?cls.super_class:"?",
           cls.method_count, cls.cp_count);

    /* pick entry method */
    const cf_method *m=NULL;
    if(method) m=cf_find_method(&cls,method,NULL);
    if(!m) m=cf_find_method(&cls,"calc",NULL);
    if(!m) m=cf_find_method(&cls,"main",NULL);
    if(!m){ for(uint16_t i=0;i<cls.method_count;i++)
                if(cls.methods[i].name && strcmp(cls.methods[i].name,"<init>")!=0 && cls.methods[i].code){ m=&cls.methods[i]; break; } }
    if(!m){ fprintf(stderr,"no runnable method found\n"); cf_free(&cls); return 1; }
    printf("entry: %s %s  stack=%u locals=%u codelen=%u\n\n",
           m->name,m->desc,m->max_stack,m->max_locals,m->code_len);

    jvm vm; jvm_init(&vm,&cls);
    DirgeEngine eng; dirge_engine_init(&eng);
    if(seed){
        /* deterministic, plausible gameplay state so calc() exercises its full
         * scoring pipeline: flags -> setSectionStat -> accumulate -> setTotalStat.
         * The tutorial + zone-flag-31==5 block is the one that flows end to end. */
        for(int i=0;i<256;i++){ eng.event_flags[i]=(i*7)%64; }
        eng.is_tutorial  = 1;   /* take the tutorial dispatch   */
        eng.zone_flags[31]=5;   /* select the flag-5 scoring block */
        eng.difficulty   = 1;
        printf("[seed] is_tutorial=1, zone_flags[31]=5, event_flags[i]=(i*7)%%64, difficulty=1\n");
    }
    /* explicit flag overrides: --eflag i=v / --zflag i=v */
    for(int i=1;i<argc;i++){
        int which=0; if(!strcmp(argv[i],"--eflag")) which=1; else if(!strcmp(argv[i],"--zflag")) which=2;
        if(which&&i+1<argc){ int idx=0,val=0; if(sscanf(argv[i+1],"%d=%d",&idx,&val)==2 && idx>=0 && idx<256){
            if(which==1) eng.event_flags[idx]=val; else eng.zone_flags[idx]=val;
            printf("[flag] %s[%d]=%d\n", which==1?"event":"zone", idx, val); } i++; }
    }
    bridge_install(&vm,&eng);
    vm.trace=trace; vm.trace_budget=tbudget;
    if(limit>0) vm.step_limit=(uint64_t)limit;

    if(trace) printf("---- trace (first %ld instructions) ----\n",tbudget);
    jval r=jvm_invoke(&vm,m,NULL,0);
    if(trace) printf("---- end trace ----\n\n");

    if(vm.error){
        printf("RUN ABORTED: %s\n",vm.errmsg);
        printf("instructions executed=%llu native-bridge calls=%llu\n",
               (unsigned long long)vm.insn_count,(unsigned long long)vm.native_calls);
        jvm_destroy(&vm); cf_free(&cls); return 1;
    }

    printf("RUN COMPLETE.\n");
    printf("  instructions executed = %llu\n",(unsigned long long)vm.insn_count);
    printf("  native-bridge calls   = %llu\n",(unsigned long long)vm.native_calls);
    char rk=jvm_desc_ret_kind(m->desc);
    if(rk=='I'||rk=='Z'||rk=='B'||rk=='C'||rk=='S') printf("  return (int)   = %d\n",r.i);
    else if(rk=='F') printf("  return (float) = %g\n",r.f);
    printf("\nengine state after run (what the script computed):\n");
    dump_nonzero("section_stat",eng.section_stat,256);
    dump_nonzero("total_stat  ",eng.total_stat,256);

    /* real native-engine subsystems the script drove through the bridge */
    printf("\nnative engine subsystems (driven via the real DocActor/DocCamera/DocLight/EngineState):\n");
    {
        dvec3 eye=doc_camera_get_position(&eng.camera);
        printf("  DocCamera.eye      = (%.3f, %.3f, %.3f)\n",eye.x,eye.y,eye.z);
        printf("  DocActorManager    = %d live actor(s)\n",eng.actor_mgr.live_count);
        int idx=0;
        for(DocActor *a=eng.actor_mgr.active_head; a && idx<6; a=a->next, idx++){
            dvec3 p=doc_actor_get_position(a);
            printf("    actor[%d] \"%s\" pos=(%.3f, %.3f, %.3f) dir=%d\n",
                   idx, a->name?a->name:"(unnamed)", p.x,p.y,p.z, doc_actor_get_dir(a));
        }
        dvec3 d0=eng.lights.lights[0].direction;
        printf("  DocLight[0].dir    = (%.3f, %.3f, %.3f) enabled=%d  ambient=(%.3f,%.3f,%.3f)\n",
               d0.x,d0.y,d0.z, eng.lights.lights[0].enabled,
               eng.lights.ambient.x,eng.lights.ambient.y,eng.lights.ambient.z);
        printf("  EngineState        = scene_state=0x%x mode_flags=0x%08x\n",
               (unsigned)engine_get_scene_state(&eng.estate),(unsigned)eng.estate.mode_flags);
    }

    jvm_destroy(&vm); cf_free(&cls);
    return 0;
}
