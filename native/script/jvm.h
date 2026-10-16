/*
 * jvm.h - native JVM-subset interpreter for Dirge of Cerberus .class scripts.
 *
 * Models the embedded VM mapped in notes/build-log.md "3. Script .class VM":
 * dispatcher FUN_003ad288 over a 202-entry handler table (JVM opcodes 0x00-0xca),
 * int/float/ref subset with long/double stubbed, frame model (+0xf010 locals /
 * +0xf014 operand stack), object/array model, and a NATIVE BRIDGE seam routing
 * Kerberos.* calls to registered C handlers.
 *
 * This is a faithful standard-JVM-subset interpreter (semantics per the JVM spec,
 * which the map confirmed the game uses), not an approximation of custom opcodes.
 */
#ifndef JVM_H
#define JVM_H

#include <stdint.h>
#include <stddef.h>
#include "classfile.h"

/* A 64-bit value slot. The JVM is untyped at runtime for int/float/ref; we keep
 * one tagged-by-use union. long/double live in a single slot but still occupy
 * two stack/local indices (JVM category-2), as the original VM stubs them. */
typedef union {
    int32_t  i;
    float    f;
    int64_t  l;
    double   d;
    void    *a;   /* object/array reference (jobject*) */
} jval;

/* Heap object / array (bump-allocated). */
typedef struct jobject {
    const char *class_name;     /* for objects */
    /* object fields: simple name-keyed slots (no static layout needed for scripts) */
    int   nfields;
    const char *fnames[32];     /* borrowed (cp-owned or literal); never freed here */
    jval  fvals[32];
    /* array payload (is_array != 0) */
    int   is_array;
    int   arr_len;
    char  arr_kind;             /* 'I','F','B','C','S','Z','L' (ref) */
    jval *arr;
} jobject;

struct jvm;

/* A native bridge handler. args[0..nargs-1] are the popped arguments in order
 * (objectref NOT included for the static-style bridge). Returns a jval that the
 * VM pushes iff the descriptor's return type is non-void. `desc` is the method
 * descriptor so the handler can interpret arg/return types. */
typedef jval (*jvm_native_fn)(struct jvm *vm, const char *key,
                              const jval *args, int nargs, const char *desc);

typedef struct {
    char         *key;          /* "owner/Class.name:desc" */
    jvm_native_fn fn;           /* NULL => default stub */
} jvm_native_entry;

typedef struct jvm {
    const cf_class *cls;        /* class currently providing the cp / methods */

    /* heap: bump allocator */
    uint8_t *heap;
    size_t   heap_cap, heap_used;

    /* native bridge registry (open-addressed linear table; small, ~300 rows) */
    jvm_native_entry *natives;
    int   nat_count, nat_cap;

    /* tracing / limits */
    int      trace;             /* 0=off, 1=full instr trace */
    long     trace_budget;      /* max instructions to print when trace==1 */
    int      quiet;             /* 1 => suppress per-call default-stub logging */
    uint64_t insn_count;        /* total instructions executed */
    uint64_t native_calls;      /* total bridge calls */
    uint64_t step_limit;        /* hard cap to prevent runaway (0 = none) */

    /* call-depth guard: same-class invokes recurse in C, so a script that
     * tail-recurses forever (the event-poll `loop()` idiom) would blow the
     * native stack. We cap depth and raise a clean error (JVM StackOverflowError
     * analogue) instead of crashing. */
    int      call_depth;
    int      max_call_depth;

    /* static fields (getstatic/putstatic), keyed by field name, with the class
     * <clinit> run lazily on first static access to populate them. */
    struct { const char *name; jval val; } *statics;
    int      nstatics, statics_cap;
    int      clinit_ran;

    /* instrumentation (for the corpus batch runner; zero-cost when unused) */
    uint64_t op_hist[256];      /* per-opcode execution count this vm's lifetime */
    void   (*native_hook)(struct jvm *vm, const char *key); /* called per bridge dispatch */

    void *user;                 /* bridge-owned engine state (DirgeEngine*) */

    /* objectref of the instance method currently dispatched to a native
     * handler (NULL for a static/invokestatic native). Set by the interpreter
     * immediately before each native bridge call so a handler can resolve the
     * receiver (e.g. map a KerberosActor object to its engine DocActor, or
     * populate a freshly-`new`ed KerberosVector in its <init>). Unused by the
     * pure/static handlers. Reset to NULL after each native dispatch. */
    jobject *bridge_this;

    /* set when execution aborts */
    int      error;
    char     errmsg[160];
} jvm;

void jvm_init(jvm *vm, const cf_class *cls);
void jvm_destroy(jvm *vm);

/* Register a native handler for an exact "owner/Class.name:desc" key. */
void jvm_register_native(jvm *vm, const char *key, jvm_native_fn fn);
/* Register every API method as a logging stub (default returns). */
void jvm_register_stub_table(jvm *vm, const char *const *keys, int count);

/* Allocate objects/arrays on the bump heap. */
jobject *jvm_new_object(jvm *vm, const char *class_name);
jobject *jvm_new_array(jvm *vm, char kind, int len);

/* Object-field access for native bridge handlers (wrap the interpreter's own
 * name-keyed field slots). `name` must be stable-lifetime (a string literal or
 * cp-owned string); the object never copies it. `create` appends the field if
 * absent. Returns NULL on a full field table or (for create==0) a missing field. */
jval *jvm_obj_field(jobject *o, const char *name, int create);

/* 1 iff `p` is a live, properly-aligned object/array this VM bump-allocated.
 * Handlers use it to fail safe on a receiver/arg that is not a real heap ref. */
int jvm_heap_obj_ok(const jvm *vm, const void *p);

/* Invoke a static/no-arg-friendly method by name; returns the method's return
 * value (undefined for void). On error sets vm->error and returns a zero jval. */
jval jvm_invoke(jvm *vm, const cf_method *m, const jval *args, int nargs);

/* Descriptor helpers (exposed for the bridge + tests). */
int  jvm_desc_arg_slots(const char *desc);   /* total slot count of all args */
int  jvm_desc_arg_count(const char *desc);   /* number of args */
char jvm_desc_ret_kind(const char *desc);    /* 'V','I','F','J','D','L','[',... */

#endif
