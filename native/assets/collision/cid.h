/* cid.h - Dirge of Cerberus (PS2) map-collision ".cid" loader.
 *
 * Loads the CID collision payload out of a "mapid.rfd" container into a flat,
 * engine-friendly collision mesh (vertices + triangles + per-triangle surface
 * word), plus the coarse AABB set for the VU-packed leaves we cannot yet decode.
 *
 * Portable C99, dependency-free, fully bounds-checked. Input is untrusted data:
 * every offset read from the file is validated against the payload length before
 * dereference. The parser never trusts a count or offset from the blob.
 *
 * Format reversed from the game's own code (see FORMAT.md for the full spec and
 * cited function addresses). Key anchors:
 *   - RFD container dispatch FUN_002585e0 / FUN_002587e0 / FUN_002588c8 route the
 *     "cid" ext-int 0x646963 to the CID relocator FUN_00271208.
 *   - CID relocation FUN_00271208 (version-gated) + FUN_0026fd90 node-array walk.
 *   - Collision query path: FUN_002714d0/FUN_00270ff8/FUN_00270ed8 ->
 *     FUN_00270088 (grid DDA raycast) -> FUN_0026f750 (tag-0 indexed faces) /
 *     FUN_0026f8a0 (tag-2 VU-packed) -> FUN_0026f578 (ray-vs-convex-polygon).
 */
#ifndef DIRGE_CID_H
#define DIRGE_CID_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { float x, y, z; } cid_vec3;

/* Source classification for each extracted triangle. */
typedef enum {
    CID_SRC_INLINE_QUAD = 0, /* v2 inline quad leaf (split into 2 tris)         */
    CID_SRC_INDEXED     = 1, /* v>=9 tag-0 indexed convex polygon (fan tris)    */
    CID_SRC_VU_PACKED   = 2  /* v>=9 tag-2 VU/VIF-packed leaf (int16/16 polys)  */
} cid_tri_source;

typedef struct {
    uint32_t v[3];       /* indices into cid_mesh.verts                          */
    uint32_t surface;    /* per-face surface/flag word (see FORMAT.md 5); 0 = none */
    uint8_t  source;     /* cid_tri_source                                       */
    uint16_t mask;       /* tag-2: packet surface-type mask (pkt+0x08), the bits the
                          * game's query flags select on (FORMAT.md 5); 0 for tag-0/v2 */
    uint8_t  subtree;    /* v>=9: sub-tree index 0..3 = payload +0x38/+0x58/+0x78/+0x98
                          * (+0x38 segment query, +0x58 floor ray, +0x98 nearest;
                          * see FORMAT.md 3). 0 for v2.                         */
} cid_tri;

/* A coarse axis-aligned box standing in for a tag-2 (VU-packed) collision leaf
 * that could NOT be decoded (fallback only; the common path now decodes tag-2
 * leaves to real triangles). center/half are exact (read from the node header);
 * the box is a conservative bound of that leaf's geometry. */
typedef struct {
    cid_vec3 center;
    cid_vec3 half;
    int32_t  npackets;   /* VU packets in this leaf (undecoded)                  */
} cid_box;

typedef struct {
    int      version;    /* CID header version (payload+4): 2 inline, 9/17/18/19 tree */

    cid_vec3 *verts;     int nverts;
    cid_tri  *tris;      int ntris;
    cid_box  *boxes;     int nboxes;    /* coarse boxes for undecoded tag-2 leaves */

    cid_vec3 bb_min, bb_max;            /* bbox over all extracted triangle verts  */

    /* diagnostics */
    int n_leaves_v2;         /* v2 type-7 inline leaves                          */
    int n_nodes_tag0;        /* v>=9 indexed leaves decoded                      */
    int n_nodes_tag2;        /* v>=9 VU-packed leaves encountered                */
    int n_tag2_packets;      /* total VU packets in tag-2 leaves                 */
    int n_tag2_pkts_decoded; /* VU packets decoded to triangles                  */
    int n_tag2_pkts_boxed;   /* VU packets that fell back to the AABB box        */
    int n_tag2_tris;         /* triangles produced from decoded tag-2 packets    */
    int n_tag2_degenerate;   /* zero-area tag-2 tris skipped (source artifacts)  */
    int surf_table_count;    /* surface/material table entries (payload+0x20)    */
} cid_mesh;

/* Error codes (negative). 0 = ok. */
enum {
    CID_OK = 0,
    CID_ERR_IO = -1,
    CID_ERR_FORMAT = -2,
    CID_ERR_NOCID = -3,
    CID_ERR_OOM = -4,
    CID_ERR_VERSION = -5
};

/* Load a .cid collision mesh from a mapid.rfd container file (or a raw CID
 * payload). Returns CID_OK and fills *out on success; caller frees with
 * cid_free(). On failure returns a negative CID_ERR_*; *out is zeroed.
 * If errbuf != NULL, a human-readable note is written (<= errlen). */
int  cid_load_file(const char *path, cid_mesh *out, char *errbuf, size_t errlen);

/* Parse from an in-memory buffer (caller owns buf). */
int  cid_load_mem(const uint8_t *buf, size_t len, cid_mesh *out,
                  char *errbuf, size_t errlen);

void cid_free(cid_mesh *m);

const char *cid_strerror(int code);

#ifdef __cplusplus
}
#endif
#endif /* DIRGE_CID_H */
