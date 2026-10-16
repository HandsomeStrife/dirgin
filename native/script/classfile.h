/*
 * classfile.h - Dirge of Cerberus embedded-JVM .class loader (shared)
 *
 * Decrypts the game-custom cipher, parses the serialized relocatable classfile
 * image (little-endian scalars, file-relative "pointers"), and RESOLVES it into
 * a flat, interpreter-friendly representation (constant pool + method table).
 *
 * The decrypt + container layout here are the exact logic reverse-engineered in
 * dcc.c (see that file's header for the cited EE function addresses). This
 * module factors the loader out so both dcc (disassembler) and jvm (interpreter)
 * share one parser.
 *
 * Reads bytes only; never executes the input.
 */
#ifndef CLASSFILE_H
#define CLASSFILE_H

#include <stddef.h>
#include <stdint.h>

/* ---- little/big-endian scalar readers (exposed for the disassembler) ---- */
uint16_t cf_be16(const uint8_t *p);
uint32_t cf_be32(const uint8_t *p);
uint32_t cf_rd_u32_le(const uint8_t *p);
uint64_t cf_rd_u64_le(const uint8_t *p);

/* ---- raw image view (serialized relocatable classfile in memory) -------- */
typedef struct { uint8_t *d; size_t n; } cf_img;

uint16_t    cf_u16(const cf_img *m, size_t o);
uint32_t    cf_u32(const cf_img *m, size_t o);
const char *cf_cstr(const cf_img *m, uint32_t off, size_t *len);
uint8_t     cf_cp_tag(const cf_img *m, uint16_t i);
const char *cf_cp_utf(const cf_img *m, uint16_t i, size_t *len);
const char *cf_cp_class(const cf_img *m, uint16_t i, size_t *len);
const char *cf_class_at_ptr(const cf_img *m, uint32_t p, size_t *len);

/* Decrypt in place (TTTT -> SSSS, cipher body). Returns 0 on success. */
int cf_decrypt(uint8_t *buf, size_t len);

/* ---- resolved (flat) representation the interpreter consumes ------------ */
/* Constant-pool item kinds mirror the JVM tags we care about. */
enum {
    CF_CP_NONE = 0,
    CF_CP_UTF8 = 1,
    CF_CP_INT  = 3,
    CF_CP_FLOAT= 4,
    CF_CP_LONG = 5,
    CF_CP_DOUBLE=6,
    CF_CP_CLASS= 7,
    CF_CP_STRING=8,
    CF_CP_FIELDREF=9,
    CF_CP_METHODREF=10,
    CF_CP_IFACEREF=11,
    CF_CP_NAMETYPE=12
};

typedef struct {
    uint8_t kind;
    union {
        int32_t  i;      /* INT   */
        float    f;      /* FLOAT */
        int64_t  l;      /* LONG  */
        double   d;      /* DOUBLE*/
        struct { const char *s; size_t len; } utf;   /* UTF8 / resolved STRING text */
        const char *cls;                              /* CLASS name          */
        struct { const char *cls, *name, *desc; } ref; /* FIELD/METHOD/IFACE */
    } u;
} cf_cp_item;

typedef struct {
    const char *name;
    const char *desc;
    uint16_t    access;
    uint16_t    max_stack;
    uint16_t    max_locals;
    const uint8_t *code;   /* points into the owning buffer */
    uint32_t    code_len;
} cf_method;

typedef struct {
    const char *this_class;
    const char *super_class;
    cf_cp_item *cp;          /* index 0 unused, like the JVM     */
    uint16_t    cp_count;
    cf_method  *methods;
    uint16_t    method_count;
    uint8_t    *raw;         /* owning decrypted buffer (free with cf_free) */
    size_t      raw_len;
} cf_class;

/* Load + decrypt + parse + resolve a .class file from disk.
 * Returns 0 on success and fills *out; on failure returns <0 and sets *err. */
int  cf_load(const char *path, cf_class *out, const char **err);
void cf_free(cf_class *c);

/* Find a method by name (and optional descriptor; NULL = first by name). */
const cf_method *cf_find_method(const cf_class *c, const char *name, const char *desc);

#endif
