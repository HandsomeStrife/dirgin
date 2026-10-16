/* doc_model.h -- native loader for Dirge of Cerberus (PS2) 3D model formats.
 *
 * Parses the RFD container and the BMD / MOD geometry payloads into a simple
 * in-memory mesh representation (triangle-strips of quantised vertices).
 *
 * Reverse-engineered from the game's own EE parser (analysis/decomp/). The
 * format evidence is documented in FORMAT.md with cited function addresses.
 *
 * This is read-only reverse engineering of untrusted data: the loader never
 * executes anything, only reads bytes, and bounds-checks every access.
 */
#ifndef DOC_MODEL_H
#define DOC_MODEL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One decoded vertex.
 *
 *  - MOD (skinned/character): position is the three UNSIGNED byte columns
 *    b0,b2,b4 of the 6-byte strip record (NOT b0,b1,b2 -- see FORMAT.md §2.3:
 *    b1,b3 are noisy non-spatial attribute bytes and using b1 as Y caused the
 *    spiky render). Dequantised by the VU ITOF4 (divide by 2^4 = 16); see
 *    MOD_POS_SCALE in doc_model.c. b1,b3 (raw[1],raw[3]) are the two per-vertex
 *    attribute bytes (packed normal / skin weight) consumed by the skinning VU0
 *    microprogram (vcallms 0x198, not in the extracted VU1 disasm); preserved in
 *    raw[] but NOT emitted as UV. Real MOD UVs are a separate 16-bit stream.
 *    NOTE: these int8 positions are in BONE-LOCAL space -- a fully assembled
 *    (de-spiked) character also needs each strip's bone bind matrix applied
 *    (FORMAT.md §7.5); u,v are left 0 here.
 *  - BMD (static/world): position is a V4-32 float vertex (x,y,z, w=1.0) already
 *    in world units -- no dequantisation. u,v are 0 (BMD per-vertex UV lives in a
 *    separate VIF stream, see FORMAT.md).
 *
 *  raw[] keeps all 6 MOD source bytes for research (zeroed for BMD). */
typedef struct {
    float x, y, z;      /* world-space position (dequantised)             */
    float u, v;         /* provisional texcoord (MOD: attr/255; BMD: 0)   */
    uint8_t raw[6];     /* verbatim MOD source record (zeroed for BMD)    */
} DocVertex;

/* A triangle-strip run: `count` vertices -> (count-2) triangles. */
typedef struct {
    DocVertex *verts;
    int        nverts;
    uint32_t   tag;     /* MOD group-table tag when known                 */
    int        material;/* texture/material id (MOD low byte), else -1    */
    int        group;   /* MOD group index (tag>>16), else -1             */
} DocStrip;

/* A top-level object (MOD top-entry, or BMD node). */
typedef struct {
    char      name[32];
    DocStrip *strips;
    int       nstrips;
} DocObject;

typedef struct {
    char       kind[4];     /* "BMD" or "MOD" */
    int        version;     /* header +0x04   */
    DocObject *objs;
    int        nobjs;
    /* aggregate stats filled by doc_model_stats() */
    int        total_strips;
    int        total_verts;
    int        total_tris;
    float      bbmin[3], bbmax[3];
    float      umin, umax, vmin, vmax;
} DocModel;

/* Result codes */
enum { DOC_OK = 0, DOC_ERR_IO = -1, DOC_ERR_FORMAT = -2, DOC_ERR_MEM = -3 };

/* Load the first BMD or MOD payload found inside an RFD container file.
 * `path` is a raw .bin container (model*.rfd / evm*.rfd unwrapped tree).
 * On success returns a heap DocModel* (free with doc_model_free). */
DocModel *doc_model_load(const char *path, int *err);

/* Load directly from an in-memory RFD container buffer. */
DocModel *doc_model_load_mem(const uint8_t *buf, size_t len, int *err);

void doc_model_free(DocModel *m);
void doc_model_stats(DocModel *m);   /* (re)compute aggregate stats */

/* Export helpers (defined in export_obj.c / export_gltf.c). */
int doc_export_obj (const DocModel *m, const char *out_path);
int doc_export_gltf(const DocModel *m, const char *out_path);

#ifdef __cplusplus
}
#endif
#endif /* DOC_MODEL_H */
