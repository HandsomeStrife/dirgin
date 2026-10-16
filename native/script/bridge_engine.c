/* bridge_engine.c - engine-backed native handlers. See bridge_engine.h. */
#include "bridge_engine.h"
#include "dmath.h"
#include <string.h>

static DirgeEngine *ENG(jvm *vm){ return (DirgeEngine*)vm->user; }

/* ---- KerberosVector helpers ------------------------------------------- *
 * A fake/base/KerberosVector object carries its components in the plain
 * object fields "x","y","z" (populated by the <init>(FFF) handler below). */
static dvec3 read_vec(jvm *vm, const jval *a){
    dvec3 v = dvec3_make(0.0f,0.0f,0.0f);
    if(!a) return v;
    jobject *o=(jobject*)a->a;
    if(!jvm_heap_obj_ok(vm,o)) return v;
    jval *fx=jvm_obj_field(o,"x",0), *fy=jvm_obj_field(o,"y",0), *fz=jvm_obj_field(o,"z",0);
    if(fx) v.x=fx->f;
    if(fy) v.y=fy->f;
    if(fz) v.z=fz->f;
    return v;
}
/* Allocate a KerberosVector object carrying v. Returns NULL (never aborts the
 * VM) if the bump heap is too tight - a long-running script then just sees a
 * null vector back, exactly as the old logging stub returned. */
static jobject *make_vec(jvm *vm, dvec3 v){
    if(vm->heap_used + sizeof(jobject) + 64 > vm->heap_cap) return NULL;
    jobject *o=jvm_new_object(vm,"fake/base/KerberosVector");
    if(!o){ vm->error=0; vm->errmsg[0]=0; return NULL; }   /* stay non-fatal */
    jval *fx=jvm_obj_field(o,"x",1), *fy=jvm_obj_field(o,"y",1), *fz=jvm_obj_field(o,"z",1);
    if(fx) fx->f=v.x;
    if(fy) fy->f=v.y;
    if(fz) fz->f=v.z;
    return o;
}

/* ---- actor receiver resolution ---------------------------------------- *
 * Each KerberosActor object caches its engine DocActor* in a hidden "$actor"
 * field. The receiver is vm->bridge_this (set by the interpreter). */
static DocActor *actor_of(jvm *vm){
    jobject *recv=vm->bridge_this;
    if(!jvm_heap_obj_ok(vm,recv)) return NULL;
    jval *f=jvm_obj_field(recv,"$actor",0);
    return f ? (DocActor*)f->a : NULL;
}

/* =====================================================================
 * KerberosActor.*  ->  DocActorManager / DocActor
 * ===================================================================== */
static jval n_actor_load(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.i=0;
    jobject *recv=vm->bridge_this;
    if(!jvm_heap_obj_ok(vm,recv)) return r;        /* no receiver: 0 (ok) */
    const char *name=NULL;
    if(n>=1){ jobject *s=(jobject*)a[0].a;
        if(jvm_heap_obj_ok(vm,s)){ jval *vf=jvm_obj_field(s,"value",0); if(vf) name=(const char*)vf->a; } }
    DocActor *act=doc_actor_load(&ENG(vm)->actor_mgr, name);
    if(act){ jval *f=jvm_obj_field(recv,"$actor",1); if(f) f->a=act; }
    r.i=0;   /* success handle (matches prior stub; scripts branch on <0) */
    return r;
}
static jval n_actor_getpos(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)a;(void)n;(void)d; jval r; r.a=NULL;
    DocActor *act=actor_of(vm);
    dvec3 p = act ? doc_actor_get_position(act) : dvec3_make(0,0,0);
    r.a=make_vec(vm,p); return r;
}
static jval n_actor_setpos(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.l=0;
    DocActor *act=actor_of(vm);
    if(act && n>=1) doc_actor_set_position(act, read_vec(vm,&a[0]));
    return r;
}
static jval n_actor_setrot(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.l=0;
    DocActor *act=actor_of(vm);
    if(act && n>=1) doc_actor_set_orientation(act, read_vec(vm,&a[0]));
    return r;
}
static jval n_actor_getdir(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)a;(void)n;(void)d; jval r; r.i=doc_actor_get_dir(actor_of(vm)); return r; }
static jval n_actor_setdir(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.l=0;
    DocActor *act=actor_of(vm);
    if(act && n>=1) doc_actor_set_dir(act, a[0].i);
    return r;
}
static jval n_actor_setvisible(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.l=0;
    DocActor *act=actor_of(vm);
    if(act && n>=1) doc_actor_set_visible(act, a[0].i);
    return r;
}

/* =====================================================================
 * KerberosCamera.*  ->  DocCamera (engine singleton)
 * ===================================================================== */
static jval n_cam_move(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.i=0;
    if(n>=3) doc_camera_move(&ENG(vm)->camera, a[0].f, a[1].f, a[2].f);
    return r;
}
static jval n_cam_set(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.l=0;
    if(n>=3) doc_camera_set(&ENG(vm)->camera, read_vec(vm,&a[0]), read_vec(vm,&a[1]), read_vec(vm,&a[2]));
    return r;
}
static jval n_cam_setfov(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.l=0;
    if(n>=1) doc_camera_set_fov(&ENG(vm)->camera, a[0].f);
    return r;
}
static jval n_cam_getvectors(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)a;(void)n;(void)d; jval r; r.a=NULL;
    dvec3 v[3]; doc_camera_get_vectors(&ENG(vm)->camera, v);
    if(vm->heap_used + sizeof(jobject)*4 + 256 > vm->heap_cap) return r;  /* non-fatal */
    jobject *arr=jvm_new_array(vm,'L',3);
    if(!arr){ vm->error=0; vm->errmsg[0]=0; return r; }
    for(int i=0;i<3;i++) arr->arr[i].a = make_vec(vm, v[i]);
    r.a=arr; return r;
}

/* =====================================================================
 * KerberosLight.*  ->  DocLightSet (engine singleton)
 * ===================================================================== */
static jval n_light_ambient(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.l=0;
    if(n>=3) doc_light_set_ambient(&ENG(vm)->lights, a[0].i, a[1].i, a[2].i);
    return r;
}
static jval n_light_dircolor(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.l=0;
    if(n>=4) doc_light_set_direction_color(&ENG(vm)->lights, a[0].i, a[1].i, a[2].i, a[3].i);
    return r;
}
static jval n_light_dirvec(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.l=0;
    if(n>=2) doc_light_set_direction_vector(&ENG(vm)->lights, a[0].i, read_vec(vm,&a[1]));
    return r;
}
static jval n_light_shadowalpha(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.l=0;
    if(n>=1) doc_light_set_shadow_alpha(&ENG(vm)->lights, a[0].i);
    return r;
}
static jval n_light_shadowvec(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.l=0;
    if(n>=1) doc_light_set_shadow_vector(&ENG(vm)->lights, read_vec(vm,&a[0]));
    return r;
}
static jval n_light_reset(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)a;(void)n;(void)d; doc_light_reset(&ENG(vm)->lights); jval r; r.l=0; return r; }

/* =====================================================================
 * KerberosEvent game-state  ->  EngineState (real mode-flag primitive)
 * ===================================================================== */
static jval n_evt_setmainmode(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.l=0;
    if(n>=2) engine_set_mode_flag(&ENG(vm)->estate, (unsigned)(a[0].i)&31u, a[1].i);
    return r;
}

/* =====================================================================
 * fake/base/KerberosVector.<init>(FFF)  ->  store x,y,z on the receiver
 * ===================================================================== */
static jval n_vec_init(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.l=0;
    jobject *o=vm->bridge_this;
    if(jvm_heap_obj_ok(vm,o) && n>=3){
        jval *fx=jvm_obj_field(o,"x",1), *fy=jvm_obj_field(o,"y",1), *fz=jvm_obj_field(o,"z",1);
        if(fx) fx->f=a[0].f;
        if(fy) fy->f=a[1].f;
        if(fz) fz->f=a[2].f;
    }
    return r;
}

/* ---- registration ----------------------------------------------------- */
void bridge_engine_install(jvm *vm, DirgeEngine *eng){
    vm->user=eng;

    /* KerberosVector constructor: carry (x,y,z) across the bridge */
    jvm_register_native(vm,"fake/base/KerberosVector.<init>:(FFF)V", n_vec_init);

    /* KerberosActor.load / loadchr (every String-first overload) */
    static const char *const LOADS[]={
        "fake/KerberosActor.load:(Ljava/lang/String;)I",
        "fake/KerberosActor.load:(Ljava/lang/String;I)I",
        "fake/KerberosActor.load:(Ljava/lang/String;II)I",
        "fake/KerberosActor.load:(Ljava/lang/String;IIII)I",
        "fake/KerberosActor.load:(Ljava/lang/String;IIIII)I",
        "fake/KerberosActor.load:(Ljava/lang/String;IIIIII)I",
        "fake/KerberosActor.loadchr:(Ljava/lang/String;)I",
        "fake/KerberosActor.loadchr:(Ljava/lang/String;I)I",
        "fake/KerberosActor.loadchr:(Ljava/lang/String;II)I",
        "fake/KerberosActor.loadchr:(Ljava/lang/String;III)I",
        "fake/KerberosActor.loadchr:(Ljava/lang/String;IIII)I",
        "fake/KerberosActor.loadchr:(Ljava/lang/String;IIIII)I",
    };
    for(size_t i=0;i<sizeof LOADS/sizeof *LOADS;i++) jvm_register_native(vm,LOADS[i],n_actor_load);

    /* KerberosActor transform / visibility */
    jvm_register_native(vm,"fake/KerberosActor.getpos:()Lfake/base/KerberosVector;", n_actor_getpos);
    jvm_register_native(vm,"fake/KerberosActor.setpos:(Lfake/base/KerberosVector;)V", n_actor_setpos);
    jvm_register_native(vm,"fake/KerberosActor.setrot:(Lfake/base/KerberosVector;)V", n_actor_setrot);
    jvm_register_native(vm,"fake/KerberosActor.getdir:()I",     n_actor_getdir);
    jvm_register_native(vm,"fake/KerberosActor.setdir:(I)V",    n_actor_setdir);
    jvm_register_native(vm,"fake/KerberosActor.setVisible:(Z)V",n_actor_setvisible);

    /* KerberosCamera */
    jvm_register_native(vm,"fake/KerberosCamera.move:(FFF)I", n_cam_move);
    jvm_register_native(vm,"fake/KerberosCamera.set:(Lfake/base/KerberosVector;Lfake/base/KerberosVector;Lfake/base/KerberosVector;)V", n_cam_set);
    jvm_register_native(vm,"fake/KerberosCamera.setfov:(F)V", n_cam_setfov);
    jvm_register_native(vm,"fake/KerberosCamera.getCurrentCameraVectors:()[Lfake/base/KerberosVector;", n_cam_getvectors);

    /* KerberosLight */
    jvm_register_native(vm,"fake/KerberosLight.set_ambientcolor:(III)V",    n_light_ambient);
    jvm_register_native(vm,"fake/KerberosLight.set_directioncolor:(IIII)V", n_light_dircolor);
    jvm_register_native(vm,"fake/KerberosLight.set_directionvector:(ILfake/base/KerberosVector;)V", n_light_dirvec);
    jvm_register_native(vm,"fake/KerberosLight.set_shadowalpha:(I)V",       n_light_shadowalpha);
    jvm_register_native(vm,"fake/KerberosLight.set_shadowvector:(Lfake/base/KerberosVector;)V", n_light_shadowvec);
    jvm_register_native(vm,"fake/KerberosLight.reset:()V",                  n_light_reset);

    /* KerberosEvent game-state -> EngineState */
    jvm_register_native(vm,"fake/KerberosEvent.setMainMode:(IZ)V", n_evt_setmainmode);
}
