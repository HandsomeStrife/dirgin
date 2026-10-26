/* cid.c - CID map-collision parser. See cid.h / FORMAT.md. C99, no deps. */
#include "cid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ----- little-endian, bounds-checked readers over the CID payload ----- */

typedef struct {
    const uint8_t *p;   /* payload base (CID data; all offsets relative to this) */
    size_t         len; /* payload length                                        */
} blob;

static int in_range(const blob *b, size_t off, size_t need) {
    return off <= b->len && need <= b->len - off;
}
static uint32_t rd_u32(const blob *b, size_t off, int *ok) {
    if (!in_range(b, off, 4)) { *ok = 0; return 0; }
    const uint8_t *q = b->p + off;
    return (uint32_t)q[0] | (uint32_t)q[1] << 8 |
           (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
}
static int32_t rd_i32(const blob *b, size_t off, int *ok) {
    return (int32_t)rd_u32(b, off, ok);
}
static uint16_t rd_u16(const blob *b, size_t off, int *ok) {
    if (!in_range(b, off, 2)) { *ok = 0; return 0; }
    const uint8_t *q = b->p + off;
    return (uint16_t)(q[0] | q[1] << 8);
}
static float rd_f32(const blob *b, size_t off, int *ok) {
    uint32_t u = rd_u32(b, off, ok);
    float f; memcpy(&f, &u, 4); return f;
}

/* FourCC ext-ints, little-endian: "rfd"->0x646672, "cid"->0x646963 */
#define EXT_RFD 0x646672u
#define EXT_CID 0x646963u

/* ----------------------------- dynamic arrays ----------------------------- */

typedef struct { void *d; int n, cap, esz; } vec;
static int vec_push(vec *v, const void *e) {
    if (v->n == v->cap) {
        int nc = v->cap ? v->cap * 2 : 256;
        void *nd = realloc(v->d, (size_t)nc * v->esz);
        if (!nd) return 0;
        v->d = nd; v->cap = nc;
    }
    memcpy((char *)v->d + (size_t)v->n * v->esz, e, v->esz);
    v->n++;
    return 1;
}

/* ----------------------------- mesh building ------------------------------ */

typedef struct {
    vec verts;   /* cid_vec3 */
    vec tris;    /* cid_tri  */
    vec boxes;   /* cid_box  */
    vec polys;   /* cid_poly (sub-tree-0 tag-0 walk mesh) */
    int oom;
    uint16_t cur_mask;   /* tag-2 packet surface-type mask (pkt+0x08); 0 otherwise */
    uint8_t cur_subtree; /* sub-tree being walked (0..3 = +0x38/+0x58/+0x78/+0x98) */
} builder;

static uint32_t push_vert(builder *bd, float x, float y, float z) {
    cid_vec3 v = { x, y, z };
    if (!vec_push(&bd->verts, &v)) bd->oom = 1;
    return (uint32_t)(bd->verts.n - 1);
}
static void push_tri(builder *bd, uint32_t a, uint32_t b, uint32_t c,
                     uint32_t surf, uint8_t src) {
    cid_tri t; t.v[0] = a; t.v[1] = b; t.v[2] = c; t.surface = surf; t.source = src;
    t.subtree = bd->cur_subtree; t.mask = bd->cur_mask;
    if (!vec_push(&bd->tris, &t)) bd->oom = 1;
}

/* A finite float within a generous world bound (reject NaN/Inf/garbage). */
static int finite_coord(float f) {
    /* reject NaN (f!=f) and anything beyond +/-1e7 world units */
    return (f == f) && (f > -1.0e7f) && (f < 1.0e7f);
}

/* ================================================================
 *  Version 2 : inline quad-soup leaves
 *  Each leaf header is 0x34 bytes:
 *    +0x00 center.xyz, +0x0c =1.0f
 *    +0x10 half.xyz,   +0x1c =1.0f
 *    +0x20 =0, +0x24 faceCount, +0x28 =7 (leaf tag), +0x2c=0, +0x30=0
 *  Then faceCount quads, each 4 verts (xyz f32) = 0x30 bytes, separated by a
 *  0x00000000 word (last quad omits it -> stride 0x34).
 *  Leaves are found by signature scan (robust to the interior-node tree).
 * ================================================================ */
#define F_ONE 0x3f800000u

static void parse_v2(blob *b, builder *bd, cid_mesh *out) {
    int ok = 1;
    uint32_t geo_end = rd_u32(b, 0x1c, &ok);        /* offset to tail metadata   */
    size_t lim = (ok && geo_end > 0x60 && geo_end <= b->len) ? geo_end : b->len;
    if (lim > b->len) lim = b->len;

    size_t i = 0x20; /* safe start (root info at +0x20 is not a leaf) */
    while (i + 0x34 <= lim) {
        int k = 1;
        uint32_t w_c  = rd_u32(b, i + 0x0c, &k);     /* must be 1.0f */
        uint32_t w_h  = rd_u32(b, i + 0x1c, &k);     /* must be 1.0f */
        uint32_t tag  = rd_u32(b, i + 0x28, &k);     /* must be 7    */
        uint32_t cnt  = rd_u32(b, i + 0x24, &k);
        if (!k || w_c != F_ONE || w_h != F_ONE || tag != 7u ||
            cnt == 0 || cnt > 100000u) { i += 4; continue; }

        /* leaf size = header(0x34) + cnt*0x34 - 4 (last quad has no separator) */
        size_t need = 0x34 + (size_t)cnt * 0x34;
        if (i + need - 4 > b->len) { i += 4; continue; }

        out->n_leaves_v2++;
        size_t fbase = i + 0x34;
        for (uint32_t q = 0; q < cnt; q++) {
            size_t fo = fbase + (size_t)q * 0x34;
            float vx[4], vy[4], vz[4];
            int good = 1;
            for (int j = 0; j < 4; j++) {
                vx[j] = rd_f32(b, fo + (size_t)j * 12 + 0, &k);
                vy[j] = rd_f32(b, fo + (size_t)j * 12 + 4, &k);
                vz[j] = rd_f32(b, fo + (size_t)j * 12 + 8, &k);
                if (!k || !finite_coord(vx[j]) || !finite_coord(vy[j]) ||
                    !finite_coord(vz[j])) { good = 0; break; }
            }
            if (!good) continue;
            uint32_t idx[4];
            for (int j = 0; j < 4; j++) idx[j] = push_vert(bd, vx[j], vy[j], vz[j]);
            /* strip-pattern quad [v0 v1 v2 v3] -> (0,1,2)+(2,1,3) */
            push_tri(bd, idx[0], idx[1], idx[2], 0, CID_SRC_INLINE_QUAD);
            push_tri(bd, idx[2], idx[1], idx[3], 0, CID_SRC_INLINE_QUAD);
        }
        i += need - 4;
    }
}

/* ================================================================
 *  Version >= 9 : grid-accelerated, vertex-indexed tree
 *  Payload header (relocated by FUN_00271208):
 *    +0x20 surfTableCount, +0x24 surfTablePtr (stride 0x10)
 *    +0x38 / +0x58 / +0x78 / +0x98  : four sub-tree descriptors (0x20 bytes):
 *       [+0]=nodeCount [+2]=? [+8]=vertexPool ptr [+0xc]=nodePtrArray [+0x10]=grid
 *  Each node (pointed to from nodePtrArray):
 *    +0x00 tag (0 indexed / 2 VU-packed)
 *    +0x10 center.xyz(+w), +0x20 half.xyz(+w), +0x30 1/half.xyz(+w)
 *    tag 0: +0x04 faceCount, faces at +0x40 stride 0x20:
 *             word0 low-4-bits = vertexCount; ushort indices at +0x08;
 *             vertex = vertexPool + index*0x10 (4 f32 xyzw)
 *    tag 2: +0x04 packetCount, packet-ptr array at +0x40 (VU/VIF-packed leaves)
 *  All stored offsets are relative to the payload base.
 *
 *  tag-2 packet (DMAed to VIF0, processed by VU0 micro _vcallms 0xa90).  The
 *  packet IS a VIF code+data stream (FUN_0026f8a0 sets QWC = *(short*)(pkt+0x0e)
 *  and surface mask = *(short*)(pkt+0x08)).  Decoded VIF layout, confirmed over
 *  all 449 v>=9 maps (530,640 packets, 0 arithmetic failures, 0 verts outside
 *  the node AABB):
 *     UNPACK S-16  x6  @VU addr0 : header [Ntri, 0, surfMask, 0xffff, 0, QWC]
 *     UNPACK S-32  xNp @VU addr6 : Np per-polygon words (Np = total polygons;
 *                                  high short = per-face material/surface index)
 *     STROW / STMOD 2            : base position + difference mode (V3-16 only)
 *     UNPACK V3-16 xNv @addr..   : Nv int16 vertex deltas; world = acc/16 (ITOF4)
 *                 -- OR --
 *     UNPACK V3-32 xNv @addr..   : Nv absolute int32 verts (no STROW/STMOD);
 *                                  world = val/16.  Rare (~1% of packets).
 *     MSCAL                      : run the ray/triangle micro
 *  Polygon split: Ntri = header[0] triangles (3 consecutive verts each) come
 *  first, then (Np - Ntri) quads (4 verts each), so Nv = 3*Ntri + 4*Nquad.
 *  Quads are fanned (0,1,2)+(0,2,3).  The int16 /16 dequant matches both the
 *  node AABB and the model-geometry VU micro's ITOF4 (.vutext).
 * ================================================================ */

static const size_t SUBTREE_OFF[4] = { 0x38, 0x58, 0x78, 0x98 };

static void parse_indexed_node(blob *b, builder *bd, size_t node, size_t pool,
                               cid_mesh *out) {
    int ok = 1;
    int32_t fc = rd_i32(b, node + 4, &ok);
    if (!ok || fc <= 0 || fc > 2000000) return;
    int decoded_any = 0;
    for (int32_t f = 0; f < fc; f++) {
        size_t fo = node + 0x40 + (size_t)f * 0x20;
        int k = 1;
        uint32_t w0 = rd_u32(b, fo, &k);
        if (!k) break;
        int vc = (int)(w0 & 0xf);
        uint32_t surf = w0 >> 4;             /* high bits: per-face flag word */
        if (vc < 3 || vc > 8) continue;      /* convex polygon, 3..8 verts    */
        uint32_t idx[8];
        int good = 1;
        for (int j = 0; j < vc; j++) {
            uint16_t vi = rd_u16(b, fo + 8 + (size_t)j * 2, &k);
            size_t vo = pool + (size_t)vi * 0x10;
            float x = rd_f32(b, vo + 0, &k);
            float y = rd_f32(b, vo + 4, &k);
            float z = rd_f32(b, vo + 8, &k);
            if (!k || !finite_coord(x) || !finite_coord(y) || !finite_coord(z)) {
                good = 0; break;
            }
            idx[j] = push_vert(bd, x, y, z);
        }
        if (!good) continue;
        /* Sub-tree 0 faces are also the character's walk mesh (FORMAT.md 3d):
         * keep the polygon with its flags and neighbour pointers (resolved to
         * polygon indices once the whole file is read). Only 3/4-vertex
         * records carry 4 neighbour slots in the 0x20-byte record. */
        if (bd->cur_subtree == 0 && vc <= 4) {
            cid_poly p; memset(&p, 0, sizeof p);
            p.nv = (uint8_t)vc;
            p.off = (uint32_t)fo;
            p.flags = rd_u16(b, fo + 4, &k);
            p.word6 = rd_u16(b, fo + 6, &k);
            for (int j = 0; j < 4; j++) {
                p.v[j] = j < vc ? idx[j] : 0;
                p.raw_nbr[j] = j < vc ? rd_u32(b, fo + 0x10 + (size_t)j * 4, &k) : 0;
                p.nbr[j] = -1;
            }
            if (k && !vec_push(&bd->polys, &p)) bd->oom = 1;
        }
        /* convex polygon -> triangle fan (matches FUN_0026f578 winding) */
        for (int j = 1; j + 1 < vc; j++)
            push_tri(bd, idx[0], idx[j], idx[j + 1], surf, CID_SRC_INDEXED);
        decoded_any = 1;
    }
    if (decoded_any) out->n_nodes_tag0++;
}

/* Emit the node's coarse AABB box as a fallback for undecoded geometry. */
static void emit_vu_box(blob *b, builder *bd, size_t node, int32_t npackets) {
    int ok = 1;
    cid_box box;
    box.center.x = rd_f32(b, node + 0x10, &ok);
    box.center.y = rd_f32(b, node + 0x14, &ok);
    box.center.z = rd_f32(b, node + 0x18, &ok);
    box.half.x   = rd_f32(b, node + 0x20, &ok);
    box.half.y   = rd_f32(b, node + 0x24, &ok);
    box.half.z   = rd_f32(b, node + 0x28, &ok);
    box.npackets = npackets;
    if (!ok || !finite_coord(box.center.x) || !finite_coord(box.center.y) ||
        !finite_coord(box.center.z) || !finite_coord(box.half.x) ||
        !finite_coord(box.half.y) || !finite_coord(box.half.z)) return;
    if (!vec_push(&bd->boxes, &box)) bd->oom = 1;
}

/* True if the triangle (a,b,c) is degenerate (zero / near-zero area). The
 * source data quantizes to 1/16 u, so the smallest real triangle has |cross|
 * ~ 1.5e-5; a cross magnitude below 1e-3 (sq < 1e-6) is a true collinear or
 * coincident-vertex degenerate and carries no collision surface. */
static int tri_degenerate(const float a[3], const float b[3], const float c[3]) {
    float ux = b[0]-a[0], uy = b[1]-a[1], uz = b[2]-a[2];
    float vx = c[0]-a[0], vy = c[1]-a[1], vz = c[2]-a[2];
    float cx = uy*vz - uz*vy, cy = uz*vx - ux*vz, cz = ux*vy - uy*vx;
    return (cx*cx + cy*cy + cz*cz) < 1.0e-6f;
}

/* Read one VIF-unpack element (component) from the packet data stream as a
 * signed (or unsigned, if the USN flag was set) integer, bounds-checked. */
static int32_t read_unpack_elem(blob *b, size_t dbyte, size_t idx, int ew,
                                int usn, int *ok) {
    if (ew == 16) {
        uint16_t u = rd_u16(b, dbyte + idx * 2, ok);
        return usn ? (int32_t)u : (int32_t)(int16_t)u;
    } else if (ew == 32) {
        return (int32_t)rd_u32(b, dbyte + idx * 4, ok);
    } else { /* 8-bit */
        size_t off = dbyte + idx;
        if (!in_range(b, off, 1)) { *ok = 0; return 0; }
        uint8_t u = b->p[off];
        return usn ? (int32_t)u : (int32_t)(int8_t)u;
    }
}

/* Decode ONE tag-2 VIF packet into triangles. Returns the number of triangles
 * emitted, or -1 on a structural failure (caller should box the leaf).
 * Walks the packet as a VIF0 code+data stream, bounds-checked against the
 * payload. See the format block above. */
#define VU_MAXPOLY 65536
#define VU_MAXVERT 65536

static int decode_vu_packet(blob *b, builder *bd, size_t pkt, cid_mesh *out) {
    int ok = 1;
    int16_t qwc   = (int16_t)rd_u16(b, pkt + 0x0e, &ok);
    uint16_t smask = rd_u16(b, pkt + 0x08, &ok);
    if (!ok || qwc <= 0) return -1;
    bd->cur_mask = smask;
    size_t nwords_total = (size_t)qwc * 4;
    if (!in_range(b, pkt, (size_t)qwc * 16)) return -1;

    int mode = 0;                 /* STMOD: 0 normal, 1 offset, 2 difference */
    int32_t row[4] = { 0, 0, 0, 0 };
    int Ntri = -1;                /* header[0] */
    int nfaces = -1;              /* polygon count (S-32 block num) */
    uint32_t *facew = NULL;       /* per-polygon words (surface) */
    int nfacew = 0;

    size_t w = 0;                 /* word index within the packet */
    int result_tris = -1;         /* set once vertices are decoded */

    while (w < nwords_total) {
        uint32_t code = rd_u32(b, pkt + w * 4, &ok); w++;
        if (!ok) { result_tris = -1; break; }
        uint32_t cmd = (code >> 24) & 0x7f;
        uint32_t num = (code >> 16) & 0xff;
        uint32_t imm = code & 0xffff;

        if (cmd == 0x00) continue;                       /* NOP     */
        if (cmd == 0x01) continue;                       /* STCYCLE */
        if (cmd == 0x05) { mode = imm & 3; continue; }   /* STMOD   */
        if (cmd == 0x20) { w += 1; continue; }           /* STMASK  */
        if (cmd == 0x31) { w += 4; continue; }           /* STCOL   */
        if (cmd == 0x30) {                               /* STROW   */
            for (int k = 0; k < 4; k++)
                row[k] = rd_i32(b, pkt + (w + k) * 4, &ok);
            w += 4;
            continue;
        }
        if (cmd == 0x10 || cmd == 0x11 || cmd == 0x13) continue;        /* FLUSH */
        if (cmd == 0x14 || cmd == 0x15 || cmd == 0x17) continue;        /* MSCAL */
        if ((cmd & 0x60) != 0x60) continue; /* unknown/ignore; stay in bounds */

        /* ---- UNPACK ---- */
        uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
        int ncomp = (int)vn + 1;
        int ew = (vl == 0) ? 32 : (vl == 1) ? 16 : (vl == 2) ? 8 : 16;
        int usn = (imm & 0x4000) != 0;
        uint32_t addr = imm & 0x3ff;
        size_t units = (size_t)num * (size_t)ncomp;
        size_t bits  = units * (size_t)ew;
        size_t uw    = (bits + 31) / 32;                 /* data words */
        size_t data0 = w;
        w += uw;
        if (w > nwords_total) { result_tris = -1; break; }
        size_t dbyte = pkt + data0 * 4;

        /* read element e, component c as a signed/unsigned integer */
        #define ELEM(e, c) read_unpack_elem(b, dbyte, ((size_t)(e)*ncomp+(c)), ew, usn, &ok)

        if (ncomp == 1 && ew == 16 && addr == 0) {       /* header */
            if (num >= 1) Ntri = (int)(ELEM(0, 0));
        } else if (ncomp == 1 && ew == 32) {             /* per-poly words */
            nfaces = (int)num;
            if (nfaces > 0 && nfaces <= VU_MAXPOLY) {
                facew = (uint32_t *)malloc((size_t)nfaces * 4);
                if (facew) {
                    nfacew = nfaces;
                    for (int e = 0; e < nfaces; e++)
                        facew[e] = (uint32_t)ELEM(e, 0);
                }
            }
        } else if (ncomp == 3 && (ew == 16 || ew == 32)) { /* vertices */
            /* V3-16 (0x69): int16 deltas in STMOD difference mode (seeded by the
             * STROW base).  V3-32 (0x68): absolute int32 in STMOD normal mode.
             * Both dequant by /16 (ITOF4).  Header + faces precede the verts, so
             * Ntri / nfaces are known here; decode with the current state. */
            if (Ntri < 0 || nfaces < 0 || num > VU_MAXVERT) { result_tris = -1; break; }
            int Nv = (int)num;
            int Nquad = nfaces - Ntri;
            if (Nquad < 0 || 3 * Ntri + 4 * Nquad != Nv) { result_tris = -1; break; }

            float *vx = (float *)malloc((size_t)Nv * sizeof(float) * 3);
            if (!vx) { bd->oom = 1; result_tris = -1; break; }
            float *vy = vx + Nv, *vz = vx + 2 * Nv;
            int32_t acc[3] = { row[0], row[1], row[2] };
            int good = 1;
            for (int e = 0; e < Nv; e++) {
                int32_t d0 = ELEM(e, 0), d1 = ELEM(e, 1), d2 = ELEM(e, 2);
                if (!ok) { good = 0; break; }
                if (mode == 2) { acc[0] += d0; acc[1] += d1; acc[2] += d2; }
                else if (mode == 1) { acc[0] = row[0] + d0; acc[1] = row[1] + d1;
                                      acc[2] = row[2] + d2; }
                else { acc[0] = d0; acc[1] = d1; acc[2] = d2; }
                vx[e] = (float)acc[0] / 16.0f;
                vy[e] = (float)acc[1] / 16.0f;
                vz[e] = (float)acc[2] / 16.0f;
                if (!finite_coord(vx[e]) || !finite_coord(vy[e]) ||
                    !finite_coord(vz[e])) { good = 0; break; }
            }
            if (!good) { free(vx); result_tris = -1; break; }

            /* Emit one world-space triangle, skipping zero-area degenerates
             * (collinear / coincident source verts). */
            #define EMIT_TRI(ia, ib, ic, sf) do {                               \
                float pa[3] = { vx[ia], vy[ia], vz[ia] };                       \
                float pb[3] = { vx[ib], vy[ib], vz[ib] };                       \
                float pc[3] = { vx[ic], vy[ic], vz[ic] };                       \
                if (tri_degenerate(pa, pb, pc)) { out->n_tag2_degenerate++; }   \
                else {                                                          \
                    uint32_t A = push_vert(bd, pa[0], pa[1], pa[2]);            \
                    uint32_t B = push_vert(bd, pb[0], pb[1], pb[2]);            \
                    uint32_t C = push_vert(bd, pc[0], pc[1], pc[2]);            \
                    push_tri(bd, A, B, C, (sf), CID_SRC_VU_PACKED);             \
                    tris++;                                                     \
                }                                                               \
            } while (0)

            /* Build polygons: Ntri triangles, then Nquad quads (tris first). */
            int tris = 0, poly = 0, vi = 0;
            for (int t = 0; t < Ntri; t++, poly++) {
                uint32_t surf = (poly < nfacew) ? facew[poly] : smask;
                EMIT_TRI(vi, vi+1, vi+2, surf);
                vi += 3;
            }
            for (int q = 0; q < Nquad; q++, poly++) {
                uint32_t surf = (poly < nfacew) ? facew[poly] : smask;
                EMIT_TRI(vi, vi+1, vi+2, surf);   /* fan (0,1,2) */
                EMIT_TRI(vi, vi+2, vi+3, surf);   /* fan (0,2,3) */
                vi += 4;
            }
            #undef EMIT_TRI
            free(vx);
            result_tris = tris;
        }
        #undef ELEM
    }

    free(facew);
    return result_tris;
}

static void parse_vu_node(blob *b, builder *bd, size_t node, cid_mesh *out) {
    int ok = 1;
    int32_t pc = rd_i32(b, node + 4, &ok);
    if (!ok || pc <= 0 || pc >= 1000000) { emit_vu_box(b, bd, node, 0); return; }
    out->n_nodes_tag2++;
    out->n_tag2_packets += pc;

    int node_tris = 0;
    for (int32_t p = 0; p < pc; p++) {
        uint32_t pkt = rd_u32(b, node + 0x40 + (size_t)p * 4, &ok);
        if (!ok || (size_t)pkt >= b->len) { out->n_tag2_pkts_boxed++; continue; }
        int tris = decode_vu_packet(b, bd, (size_t)pkt, out);
        bd->cur_mask = 0;
        if (bd->oom) return;
        if (tris >= 0) { out->n_tag2_pkts_decoded++; node_tris += tris;
                         out->n_tag2_tris += tris; }
        else           { out->n_tag2_pkts_boxed++; }
    }
    /* Fallback: if no packet in this leaf decoded, keep its exact AABB box so
     * the mesh retains full spatial coverage for that leaf. */
    if (node_tris == 0) emit_vu_box(b, bd, node, pc);
}

/* ================================================================
 *  tag-1 node: one oriented primitive (FORMAT.md 3c). Decoded from
 *  FUN_00270938 (broad phase, shape type at +0x40) and FUN_0035dcf0 (the
 *  character's closest-point test): the point is taken into unit space by the
 *  inverse matrix at +0xa0, tested against a unit shape, and mapped back by
 *  the matrix at +0x60 (row vectors: world = [x y z 1] * M).
 *    type 0/1  sphere, radius 10                   (FUN_0025dae8)
 *    type 2    box (-10,-10,-10)..(10,10,10)       (FUN_0025cf00)
 *    type 3/4  radius 10 around local Y, y -10..10 (FUN_0025d1b8; the node
 *              AABB bounds y at +-10 scaled, so the ends are flat: cylinder)
 *  +0x44 = class word (1,2,3,4,0xc on z128 sub-tree 0), +0x48 = flag word
 *  (bit 0 set: FUN_0035a440 skips the ground test against it).
 * ================================================================ */
#define PRIM_SEG 16
static void parse_prim_node(blob *b, builder *bd, size_t node, cid_mesh *out) {
    int ok = 1;
    uint32_t type = rd_u32(b, node + 0x40, &ok);
    uint32_t cls  = rd_u32(b, node + 0x44, &ok);
    uint32_t flag = rd_u32(b, node + 0x48, &ok);
    float M[16];
    for (int i = 0; i < 16; i++) {
        M[i] = rd_f32(b, node + 0x60 + (size_t)i * 4, &ok);
        if (ok && !finite_coord(M[i])) ok = 0;
    }
    if (!ok || type > 4) return;
    float P[2 + PRIM_SEG * 8][3];            /* unit-space points (<= 130)        */
    int np = 0;
    uint16_t T[PRIM_SEG * 16][3];            /* triangles over P (<= 256)         */
    int nt = 0;
#define PT(X, Y, Z) (P[np][0] = (X), P[np][1] = (Y), P[np][2] = (Z), np++)
#define TR(A, B, C) (T[nt][0] = (uint16_t)(A), T[nt][1] = (uint16_t)(B), T[nt][2] = (uint16_t)(C), nt++)
    const float R = 10.0f, PI2 = 6.28318530718f;
    if (type == 2) {
        for (int i = 0; i < 8; i++) PT(i & 1 ? R : -R, i & 2 ? R : -R, i & 4 ? R : -R);
        static const uint8_t F[6][4] = { {0,2,3,1}, {4,5,7,6}, {0,1,5,4},
                                         {2,6,7,3}, {0,4,6,2}, {1,3,7,5} };
        for (int f = 0; f < 6; f++) { TR(F[f][0], F[f][1], F[f][2]); TR(F[f][0], F[f][2], F[f][3]); }
    } else if (type >= 3) {                  /* cylinder: rings at y=-10 and y=+10 */
        for (int i = 0; i < PRIM_SEG; i++) {
            float a = PI2 * (float)i / PRIM_SEG;
            PT(R * cosf(a), -R, R * sinf(a));
            PT(R * cosf(a),  R, R * sinf(a));
        }
        for (int i = 0; i < PRIM_SEG; i++) {
            int j = (i + 1) % PRIM_SEG;
            TR(2 * i, 2 * j, 2 * j + 1); TR(2 * i, 2 * j + 1, 2 * i + 1);
        }
        for (int i = 1; i + 1 < PRIM_SEG; i++) { TR(0, 2 * (i + 1), 2 * i); TR(1, 2 * i + 1, 2 * (i + 1) + 1); }
    } else {                                 /* sphere: 8 latitude bands          */
        const int LAT = 8;
        PT(0, -R, 0); PT(0, R, 0);
        for (int l = 1; l < LAT; l++) {
            float t = 3.14159265359f * (float)l / LAT;
            for (int i = 0; i < PRIM_SEG; i++) {
                float a = PI2 * (float)i / PRIM_SEG;
                PT(R * sinf(t) * cosf(a), -R * cosf(t), R * sinf(t) * sinf(a));
            }
        }
        for (int i = 0; i < PRIM_SEG; i++) {
            int j = (i + 1) % PRIM_SEG;
            TR(0, 2 + j, 2 + i);
            TR(1, 2 + (LAT - 2) * PRIM_SEG + i, 2 + (LAT - 2) * PRIM_SEG + j);
            for (int l = 0; l + 2 < LAT; l++) {
                int a0 = 2 + l * PRIM_SEG, a1 = a0 + PRIM_SEG;
                TR(a0 + i, a0 + j, a1 + j); TR(a0 + i, a1 + j, a1 + i);
            }
        }
    }
#undef PT
#undef TR
    uint32_t idx[2 + PRIM_SEG * 8];
    for (int i = 0; i < np; i++) {
        float x = P[i][0], y = P[i][1], z = P[i][2];
        idx[i] = push_vert(bd, x * M[0] + y * M[4] + z * M[8] + M[12],
                               x * M[1] + y * M[5] + z * M[9] + M[13],
                               x * M[2] + y * M[6] + z * M[10] + M[14]);
    }
    uint16_t save = bd->cur_mask;
    bd->cur_mask = (uint16_t)(cls & 0xffff);
    for (int i = 0; i < nt; i++) push_tri(bd, idx[T[i][0]], idx[T[i][1]], idx[T[i][2]], flag, CID_SRC_PRIM);
    bd->cur_mask = save;
    out->n_nodes_tag1++;
}

static void parse_tree(blob *b, builder *bd, cid_mesh *out) {
    int ok = 1;
    out->surf_table_count = rd_i32(b, 0x20, &ok);
    if (!ok || out->surf_table_count < 0) out->surf_table_count = 0;

    for (int s = 0; s < 4; s++) {
        size_t sub = SUBTREE_OFF[s];
        bd->cur_subtree = (uint8_t)s;
        int k = 1;
        int32_t  ncount = rd_i32(b, sub + 0x00, &k);
        uint32_t pool   = rd_u32(b, sub + 0x08, &k);
        uint32_t arr    = rd_u32(b, sub + 0x0c, &k);
        if (!k || ncount <= 0 || ncount > 2000000) continue;
        if ((size_t)arr >= b->len) continue;
        int pool_ok = ((size_t)pool < b->len);

        for (int32_t i = 0; i < ncount; i++) {
            uint32_t nodep = rd_u32(b, (size_t)arr + (size_t)i * 4, &k);
            if (!k || (size_t)nodep >= b->len) continue;
            uint32_t tag = rd_u32(b, nodep, &k);
            if (!k) continue;
            if (tag == 0) {
                if (pool_ok) parse_indexed_node(b, bd, nodep, pool, out);
                else         parse_vu_node(b, bd, nodep, out); /* no pool: box it */
            } else if (tag == 2) {
                parse_vu_node(b, bd, nodep, out);
            } else if (tag == 1) {
                parse_prim_node(b, bd, nodep, out);
            }
        }
    }
}

/* ----------------------------- RFD container ------------------------------ */

/* Find the CID payload inside an RFD container. Returns 1 and sets poff/plen
 * (both relative to the whole file 'f'), else 0. */
static int find_cid(const uint8_t *f, size_t flen, size_t *poff, size_t *plen) {
    blob fb = { f, flen };
    int ok = 1;
    /* Case A: RFD container. ext-int at +8 == "rfd". */
    uint32_t ext0 = rd_u32(&fb, 8, &ok);
    if (ok && ext0 == EXT_RFD) {
        uint16_t count = rd_u16(&fb, 0x10, &ok);
        if (ok && count > 0 && count < 4096) {
            for (uint16_t i = 0; i < count; i++) {
                uint32_t eoff = rd_u32(&fb, 0x14 + (size_t)i * 4, &ok);
                if (!ok || (size_t)eoff + 0x10 > flen) continue;
                uint32_t ext = rd_u32(&fb, (size_t)eoff + 8, &ok);
                uint32_t esz = rd_u32(&fb, (size_t)eoff + 0xc, &ok);
                if (ok && ext == EXT_CID) {
                    *poff = (size_t)eoff + 0x10;
                    *plen = (*poff + esz <= flen) ? esz : (flen - *poff);
                    return 1;
                }
            }
        }
    }
    /* Case B: raw CID fallback (version field looks sane, small int). */
    ok = 1;
    uint32_t ver = rd_u32(&fb, 4, &ok);
    if (ok && (ver == 2 || (ver >= 9 && ver <= 64))) {
        *poff = 0; *plen = flen; return 1;
    }
    return 0;
}

/* ------------------------------- public API -------------------------------- */

static const char *g_err[] = {
    "ok", "I/O error", "bad format", "no CID chunk in container",
    "out of memory", "unsupported CID version"
};
const char *cid_strerror(int code) {
    int idx = -code;
    if (idx < 0 || idx >= (int)(sizeof g_err / sizeof g_err[0])) return "unknown";
    return g_err[idx];
}

static void finalize_bbox(cid_mesh *m) {
    m->bb_min.x = m->bb_min.y = m->bb_min.z =  1.0e30f;
    m->bb_max.x = m->bb_max.y = m->bb_max.z = -1.0e30f;
    for (int i = 0; i < m->nverts; i++) {
        cid_vec3 v = m->verts[i];
        if (v.x < m->bb_min.x) m->bb_min.x = v.x;
        if (v.y < m->bb_min.y) m->bb_min.y = v.y;
        if (v.z < m->bb_min.z) m->bb_min.z = v.z;
        if (v.x > m->bb_max.x) m->bb_max.x = v.x;
        if (v.y > m->bb_max.y) m->bb_max.y = v.y;
        if (v.z > m->bb_max.z) m->bb_max.z = v.z;
    }
    if (m->nverts == 0) {
        m->bb_min.x = m->bb_min.y = m->bb_min.z = 0;
        m->bb_max.x = m->bb_max.y = m->bb_max.z = 0;
    }
}

/* Neighbour pointers (payload offsets of face records) -> polygon indices.
 * Polygons are appended in file order, so their offsets are already sorted
 * within a node but not across nodes: sort a copy of (off, index). If two
 * nodes share a record (the same offset twice), a pointer to it resolves to
 * the lowest polygon index: the sort breaks ties on the index and the lookup
 * takes the first match (qsort is not stable and bsearch may return any). */
typedef struct { uint32_t off; int32_t i; } poly_key;
static int poly_key_cmp(const void *a, const void *b) {
    const poly_key *x = a, *y = b;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    return x->i < y->i ? -1 : x->i > y->i;
}
static void resolve_poly_links(cid_mesh *m) {
    if (m->npolys <= 0) return;
    poly_key *k = malloc((size_t)m->npolys * sizeof *k);
    if (!k) return;                       /* links stay -1: fails toward walls */
    for (int i = 0; i < m->npolys; i++) { k[i].off = m->polys[i].off; k[i].i = i; }
    qsort(k, (size_t)m->npolys, sizeof *k, poly_key_cmp);
    for (int i = 1; i < m->npolys; i++)
        if (k[i].off == k[i - 1].off) m->n_poly_dup_off++;
    for (int i = 0; i < m->npolys; i++) {
        cid_poly *p = &m->polys[i];
        for (int j = 0; j < p->nv; j++) {
            uint32_t want = p->raw_nbr[j];
            if (!want) continue;
            size_t lo = 0, hi = (size_t)m->npolys;      /* first key with off >= want */
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (k[mid].off < want) lo = mid + 1; else hi = mid;
            }
            if (lo < (size_t)m->npolys && k[lo].off == want) { p->nbr[j] = k[lo].i; m->n_poly_links++; }
            else m->n_poly_dangling++;
        }
    }
    free(k);
}

int cid_load_mem(const uint8_t *buf, size_t len, cid_mesh *out,
                 char *errbuf, size_t errlen) {
    memset(out, 0, sizeof *out);
    if (errbuf && errlen) errbuf[0] = 0;
    if (!buf || len < 16) return CID_ERR_FORMAT;

    size_t poff = 0, plen = 0;
    if (!find_cid(buf, len, &poff, &plen)) {
        if (errbuf) snprintf(errbuf, errlen, "no 'cid' chunk / not a CID file");
        return CID_ERR_NOCID;
    }
    blob b = { buf + poff, plen };
    int ok = 1;
    out->version = rd_i32(&b, 4, &ok);
    if (!ok) return CID_ERR_FORMAT;

    builder bd; memset(&bd, 0, sizeof bd);
    bd.verts.esz = sizeof(cid_vec3);
    bd.tris.esz  = sizeof(cid_tri);
    bd.boxes.esz = sizeof(cid_box);
    bd.polys.esz = sizeof(cid_poly);

    if (out->version <= 8) {
        if (out->version != 2) {
            /* versions 3..8 unseen in this build; attempt inline anyway */
            if (errbuf) snprintf(errbuf, errlen,
                "version %d (low): attempting inline leaf scan", out->version);
        }
        parse_v2(&b, &bd, out);
    } else {
        parse_tree(&b, &bd, out);
    }

    if (bd.oom) {
        free(bd.verts.d); free(bd.tris.d); free(bd.boxes.d); free(bd.polys.d);
        return CID_ERR_OOM;
    }

    out->verts  = (cid_vec3 *)bd.verts.d; out->nverts  = bd.verts.n;
    out->tris   = (cid_tri  *)bd.tris.d;  out->ntris   = bd.tris.n;
    out->boxes  = (cid_box  *)bd.boxes.d; out->nboxes  = bd.boxes.n;
    out->polys  = (cid_poly *)bd.polys.d; out->npolys  = bd.polys.n;
    resolve_poly_links(out);
    finalize_bbox(out);
    return CID_OK;
}

int cid_load_file(const char *path, cid_mesh *out, char *errbuf, size_t errlen) {
    memset(out, 0, sizeof *out);
    FILE *fp = fopen(path, "rb");
    if (!fp) { if (errbuf) snprintf(errbuf, errlen, "cannot open %s", path);
               return CID_ERR_IO; }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return CID_ERR_IO; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return CID_ERR_IO; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return CID_ERR_IO; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(fp); return CID_ERR_OOM; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (rd != (size_t)sz) { free(buf); return CID_ERR_IO; }
    int r = cid_load_mem(buf, (size_t)sz, out, errbuf, errlen);
    free(buf);
    return r;
}

void cid_free(cid_mesh *m) {
    if (!m) return;
    free(m->verts); free(m->tris); free(m->boxes); free(m->polys);
    memset(m, 0, sizeof *m);
}
