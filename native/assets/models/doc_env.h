/* =====================================================================
 * doc_env.h -- native decoder for Dirge of Cerberus ENVIRONMENT geometry
 * (static room BMD chunks: the `068f0N12` VIF draw chain).
 *
 * Complements doc_model.c (character MOD path) and material_loader.c (the
 * texture binding): this recovers the per-packet world geometry a room chunk
 * draws -- position, UV and baked PRELIT vertex colour -- so the engine can
 * load rooms natively instead of via the Python export_bmd_textured.py.
 *
 * It is a faithful port of export_bmd_textured.py:decode_packets /
 * find_bmd_payload (which own the format); the C output is validated
 * vertex-exact against that Python oracle (test: env_decode --fnv + the
 * env_parity check). See FORMAT.md and BINDING-PIPELINE.md.
 *
 * Packet VIF layout (one submesh): UNPACK V4-32 num=2 @0 (A+D/giftag header:
 * qw0 = GIF register template, qw1.x & 0x7fff = vertex count, qw1.y & 0x8000 =
 * triangle-LIST vs QUAD-GROUP connectivity), then STROW/STMOD framing + UNPACK
 * V3-16 @2 (position, int16, delta/ROW per STMOD), V4-8 @3 (RGBA), V2-16 @4
 * (UV), MSCNT. Positions dequantise /16 (ITOF4), UV /4096 (ITOF12).
 * ===================================================================== */
#ifndef DOC_ENV_H
#define DOC_ENV_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DocEnvPacket {
    int       nv;        /* vertex count                                   */
    int       is_list;   /* 1 = triangle list, 0 = quad group              */
    int       sel;       /* low byte of the vertex count (grouping key,
                          * NOT a material id -- FORMAT.md Section 6)       */
    int       prim;      /* GS PRIM field from the giftag                   */
    int       abe;       /* alpha-blend enable (PRIM & 0x40)                */
    float    *pos;       /* 3*nv, world units (= pos_i / 16)                */
    float    *uv;        /* 2*nv            (= uv_i / 4096)                 */
    uint8_t  *col;       /* 4*nv RGBA baked PRELIT colour                   */
    int32_t  *pos_i;     /* 3*nv pre-scale integer position (exact parity)  */
    int16_t  *uv_i;      /* 2*nv pre-scale raw int16 UV                     */
} DocEnvPacket;

typedef struct DocEnvPackets {
    DocEnvPacket *pk;
    int           n;
} DocEnvPackets;

/* RFD unwrap -> BMD payload offset, or -1 if the buffer is not a room BMD. */
long doc_env_find_payload(const uint8_t *buf, size_t size);

/* Decode the VIF draw chain at `payload`. On success fills *out (caller frees
 * with doc_env_free) and returns 0; returns non-zero on a structural error. */
int  doc_env_decode(const uint8_t *buf, size_t size, long payload,
                    DocEnvPackets *out);

void doc_env_free(DocEnvPackets *p);

/* Triangle index triples for a packet (groups of 3 if is_list, else quad
 * groups of 4). Writes up to max_tris triples (3 ints each) into idx and
 * returns the triple count. */
int  doc_env_packet_tris(const DocEnvPacket *pk, int *idx, int max_tris);

/* FNV-1a 64 over the canonical integer representation of all packets (counts +
 * pre-scale pos_i/uv_i/col), in decode order. Deterministic and identical to
 * the Python oracle's hash -- the vertex-exact parity signature. */
uint64_t doc_env_fnv(const DocEnvPackets *p);

#ifdef __cplusplus
}
#endif
#endif /* DOC_ENV_H */
