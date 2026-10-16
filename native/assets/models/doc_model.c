/* doc_model.c -- see doc_model.h and FORMAT.md.
 *
 * Pipeline:
 *   RFD container  (FUN_002585e0 relocation/dispatch is the spec)
 *     -> find entry with ext "bmd"/"mod", payload = entry_off + 0x10
 *   MOD payload    (FUN_005aecf8)
 *     -> header, top-entry table, triangle-strip vertex stream
 *   BMD payload    (FUN_0026abe8 / FUN_0026ba88)
 *     -> header, node arrays; geometry recovered by strip-run scan
 *
 * The triangle-strip vertex stream uses a self-validating run format, so the
 * scanner never guesses: a candidate run is accepted only if its per-vertex
 * index bytes match the expected 1,2,...,count-1,0 sequence (see FORMAT.md).
 */
#include "doc_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- MOD position dequantisation --------------------------------------- *
 * The MOD strip positions are the UNSIGNED u8 columns (b0,b2,b4) of each
 * 6-byte record (see scan_strips below and FORMAT.md §2.3 -- NOT b0,b1,b2).
 *
 * The exact dequant for the *character/skinned* MOD path is now CONFIRMED from
 * the VU0 skinning microcode (port/vu/disasm/vu0_skin_micro.txt). The resident
 * VU0 micro image is program_C.bin; its per-vertex program at micromem 0x120
 * dequantises the skinned position with **ITOF12 (/4096)**, and the matrix-setup
 * program at 0x198 then scales by a per-mesh factor meshScale = *(obj+0x2c).
 * So the true world position of a skinned MOD vertex is:
 *     world = BoneMatrix * ( (localInt / 4096) * meshScale , 1 )
 * where BoneMatrix = *(ctx+0x20) + boneIndex*0x40 (a runtime bone palette that
 * is NOT in the file -- see FORMAT.md §7.5, the remaining blocker). The earlier
 * "ITOF4 /16" note was the VU1 (BMD / visible) position stream, a *different*
 * program than the VU0 skin path used here.
 *
 * MOD_POS_SCALE below is a VIEWING convenience only. The renderer auto-frames
 * from the bbox, and the dequant scale is UNIFORM on x,y,z, so its exact value
 * changes neither the shape nor the render -- 1/16 is kept because it gives a
 * pleasant ~0..16 export magnitude (1/4096*meshScale is the physically-correct
 * factor but meshScale is per-mesh and not yet parsed). The positions are in
 * BONE-LOCAL space regardless, so a coherent pose still needs the bone matrices. */
#define MOD_POS_SCALE   (1.0f / 16.0f)    /* viewing scale; true dequant = /4096 * meshScale (VU0 0x120) */

/* ---- little-endian readers with bounds checks -------------------------- */
static uint32_t rd_u32(const uint8_t *b, size_t len, size_t o, int *ok) {
    if (o + 4 > len) { *ok = 0; return 0; }
    return (uint32_t)b[o] | (uint32_t)b[o+1]<<8 | (uint32_t)b[o+2]<<16 | (uint32_t)b[o+3]<<24;
}
static uint16_t rd_u16(const uint8_t *b, size_t len, size_t o, int *ok) {
    if (o + 2 > len) { *ok = 0; return 0; }
    return (uint16_t)(b[o] | b[o+1]<<8);
}

/* ---- dynamic arrays ---------------------------------------------------- */
#define PUSH(arr, cnt, cap, item, errlbl) do {                         \
    if ((cnt) == (cap)) {                                              \
        int ncap = (cap) ? (cap)*2 : 8;                                \
        void *np = realloc((arr), (size_t)ncap * sizeof(*(arr)));      \
        if (!np) goto errlbl;                                          \
        (arr) = np; (cap) = ncap;                                      \
    }                                                                  \
    (arr)[(cnt)++] = (item);                                           \
} while (0)

/* ----------------------------------------------------------------------- *
 *  Triangle-strip vertex-stream scanner.
 *
 *  A run is:  [count:u8][00][00]  then count * 6-byte records.
 *  Record:    b0 b1 b2 b3 b4 idx    (idx = (k+1) % count for the k-th vertex)
 *
 *  We require the idx column to match exactly -> unambiguous, no fabrication.
 *  Returns number of strips appended, parsing the window [start,end).
 * ----------------------------------------------------------------------- */
static int scan_strips(const uint8_t *b, size_t len, size_t start, size_t end,
                       DocStrip **out, int *ncnt, int *ccap)
{
    int added = 0;
    size_t p = start;
    if (end > len) end = len;
    while (p < end) {
        /* skip separator / padding zero bytes */
        if (b[p] == 0) { p++; continue; }
        int C = b[p];
        /* a run header is C,00,00 with 2 <= C (need >=3 for a triangle, but
         * accept >=3; cap at a sane max to avoid runaway on noise) */
        if (C < 3 || C > 200 || p + 3 + (size_t)C * 6 > end ||
            b[p+1] != 0 || b[p+2] != 0) { p++; continue; }
        size_t v0 = p + 3;
        /* validate the index column before committing */
        int valid = 1;
        for (int k = 0; k < C; k++) {
            uint8_t idx = b[v0 + (size_t)k*6 + 5];
            if (idx != (uint8_t)((k + 1) % C)) { valid = 0; break; }
        }
        if (!valid) { p++; continue; }

        DocStrip s;
        memset(&s, 0, sizeof s);
        s.nverts = C;
        s.tag = 0;
        s.material = -1;
        s.group = -1;
        s.verts = (DocVertex *)calloc((size_t)C, sizeof(DocVertex));
        if (!s.verts) return added;
        for (int k = 0; k < C; k++) {
            const uint8_t *r = b + v0 + (size_t)k*6;
            DocVertex *vt = &s.verts[k];
            memcpy(vt->raw, r, 6);
            /* POSITION = (b0, b2, b4), UNSIGNED, dequantised by ITOF4 (/16).
             *
             * Correction (this file previously used (b0,b1,b2) as x,y,z, which
             * produced a SPIKY mesh). Measured from the real sample km01vnw
             * (0c064400): the three per-vertex columns that vary SMOOTHLY within
             * a triangle-strip -- i.e. behave as spatial coordinates -- are
             * bytes 0, 2 and 4, not 0,1,2.  Average |delta| between consecutive
             * strip vertices (558 verts / 62 strips):
             *     byte0 ~15   byte1 ~85   byte2 ~2   byte3 ~74   byte4 ~8
             * Bytes 1 and 3 jump by ~80/255 every vertex: they are NOT a smooth
             * spatial axis (packed normal/weight attribute), so using b1 as "Y"
             * injected the spikes.  Byte 2 is the cleanest axis and is always in
             * [2,127] (never negative) -- the model's vertical/standing axis
             * (feet near 0), so it maps to Y here.  Bytes 0 and 4 cross the
             * 0x7f/0x80 boundary smoothly (signed |delta| > unsigned |delta|)
             * => they are UNSIGNED 0..255, not signed int8.  Under this reading
             * the 62 strips' centroids spread across the whole model volume
             * (they are already in model space; no per-bone bind transform is
             * needed to assemble a coherent silhouette -- see FORMAT.md §2.3). */
            vt->x = (float)(uint8_t)r[0] * MOD_POS_SCALE;  /* b0 */
            vt->y = (float)(uint8_t)r[2] * MOD_POS_SCALE;  /* b2 (height) */
            vt->z = (float)(uint8_t)r[4] * MOD_POS_SCALE;  /* b4 */
            /* b1,b3 are the two NON-spatial attribute bytes (noisy per vertex:
             * packed normal / skin weight). They are NOT smooth texcoords, so
             * they are preserved in raw[] but not emitted as UV. Real per-vertex
             * UVs for MOD live in a separate 16-bit texcoord stream (ITOF12 in
             * the VU, WS-V) that is not part of this 6-byte strip record. */
            vt->u = 0.0f;
            vt->v = 0.0f;
        }
        PUSH(*out, *ncnt, *ccap, s, oom);
        added++;
        p = v0 + (size_t)C * 6;
    }
    return added;
oom:
    return added;
}

/* ----------------------------------------------------------------------- *
 *  MOD parser.  Spec: FUN_005aecf8 (header) + the strip stream.
 *   +0x00 "MOD\0"
 *   +0x04 version (u32, >=16)
 *   +0x10 top-entry count (u32)
 *   +0x14 offset -> array[count] of entry offsets
 *   +0x24 trailer offset (end of geometry stream)
 *  Each entry:
 *   +0x14 strip-table count (u32)
 *   +0x18 strip-table: count * { u32 dataOffset, u32 tag }
 *  dataOffset[0] is the start of the continuous triangle-strip stream.
 * ----------------------------------------------------------------------- */
static int parse_mod(const uint8_t *b, size_t len, DocModel *m)
{
    int ok = 1;
    m->version = (int)rd_u32(b, len, 0x04, &ok);
    uint32_t ecount  = rd_u32(b, len, 0x10, &ok);
    uint32_t earr    = rd_u32(b, len, 0x14, &ok);
    uint32_t trailer = rd_u32(b, len, 0x24, &ok);
    if (!ok) return DOC_ERR_FORMAT;
    if (trailer == 0 || trailer > len) trailer = (uint32_t)len;
    if (ecount == 0 || ecount > 4096) return DOC_ERR_FORMAT;

    DocObject *objs = NULL; int nobj = 0, ocap = 0;

    /* The triangle-strip stream starts well *before* the group table's first
     * dataOffset (the dataOffsets point at a separate per-group control
     * structure, not at strip-run starts -- verified empirically). Scan the
     * whole payload from just past the MOD header: the strip run format is
     * self-validating (count,0,0 header + the exact idx column) so false
     * positives in the header/table region are negligible. This recovers the
     * complete mesh (the earlier earliest-dataOffset start dropped the strips
     * that precede it). */
    DocStrip *strips = NULL; int ns = 0, cap = 0;
    scan_strips(b, len, 0x28, trailer, &strips, &ns, &cap);

    DocObject obj; memset(&obj, 0, sizeof obj);
    snprintf(obj.name, sizeof obj.name, "mod");
    obj.strips = strips; obj.nstrips = ns;
    PUSH(objs, nobj, ocap, obj, oom);

    /* Record the group table as metadata. In the sample (km01vnw) the entry's
     * group count at +0x14 is 9 -- the same count the runtime reads
     * (FUN_002e62b0 bounds a group index against *(model+0x40), group table at
     * *(model+0x60)). Each 8-byte group record is {u32 dataOffset, u32 tag};
     * (tag>>16) is a 1-based group index, (tag&0xff) a per-group id byte. How
     * individual strips map onto these groups is NOT resolved from the file
     * (the dataOffsets do not line up with strip starts); material selection is
     * the context+0x24 texture-handle lookup documented in FORMAT.md, which the
     * oracle can confirm. We expose the first group's id on every strip as a
     * provisional hint only when there is a single group. */
    {
        uint32_t eoff = rd_u32(b, len, earr, &ok);
        if (ok && eoff + 0x18 <= len) {
            uint32_t gc = rd_u32(b, len, eoff + 0x14, &ok);
            if (ok && gc == 1) {
                uint32_t tag = rd_u32(b, len, eoff + 0x18 + 4, &ok);
                if (ok)
                    for (int s = 0; s < ns; s++) {
                        strips[s].tag = tag;
                        strips[s].material = (int)(tag & 0xff);
                        strips[s].group = (int)(tag >> 16);
                    }
            }
        }
    }

    m->objs = objs; m->nobjs = nobj;
    return DOC_OK;
oom:
    free(objs);
    return DOC_ERR_MEM;
}

/* ----------------------------------------------------------------------- *
 *  BMD parser.  Spec: FUN_0026abe8 (header relocation).
 *   +0x00 "BMD\0"
 *   +0x04 version (u32, >=4)
 *   +0x08 (u16) count of +0x1c array      (FUN_0026bc28 nodes)
 *   +0x0e (u16) count of +0x14 array      (FUN_0026ba88 nodes)
 *   +0x14 offset -> array[cnt@0x0e] of node offsets
 *   +0x1c offset -> array[cnt@0x08] of node offsets
 *   +0x20 offset -> bone/matrix tree      (FUN_0026bf88, 8-way)
 *  Geometry leaves (FUN_0026f518) hold the actual strip data. Because the BMD
 *  node graph is deep and version-dependent, we document the structure and
 *  recover visible geometry by scanning the payload for valid strip runs.
 * ----------------------------------------------------------------------- */
/* read a little-endian 32-bit float (host is LE, per the port's runtime
 * contract); returns 0 and clears *ok on out-of-bounds. */
static float rd_f32(const uint8_t *b, size_t len, size_t o, int *ok) {
    uint32_t u = rd_u32(b, len, o, ok);
    float f; memcpy(&f, &u, 4);
    return f;
}
/* a plausible world-space coordinate: finite, not a denormal/garbage magnitude */
static int nice_coord(float v) {
    float a = v < 0 ? -v : v;
    if (v != v) return 0;                 /* NaN  */
    if (a > 1.0e9f) return 0;             /* Inf / absurd */
    return a == 0.0f || (a > 1.0e-4f && a < 1.0e6f);
}

/* ----------------------------------------------------------------------- *
 *  BMD geometry: V4-32 float vertex blocks.
 *
 *  BMD = static/world models. Unlike MOD (quantised int8 + VU skinning), BMD
 *  stores vertices as VIF UNPACK V4-32 (cmd 0x6c in the draw-template palette
 *  seeded by FUN_0026bf88 at +0xb0 with the constant 0x6c080000) -- i.e. four
 *  32-bit floats per vertex (x,y,z, w=1.0), already in world units (NO
 *  dequantisation).
 *
 *  GRAPH IS FOLLOWABLE (file_load_base = 0; the earlier "absolute EE pointer"
 *  diagnosis was wrong -- see FORMAT.md §3.3). The node graph FUN_0026abe8
 *  (header) -> FUN_0026ba88 node -> sub-array of draw-objects / FUN_0026f518
 *  leaf -> leaf+0x40 pointer array are stored as FILE-RELATIVE offsets: e.g. a
 *  node's sub-node array pointer at +0x08 points exactly to node+0x10 (the array
 *  physically follows the 16-byte node), an adjacency that only holds at base 0.
 *  A draw-object is { N * V4-32 float verts ; trailer: u32 vertCount, self-ptr
 *  back to the vertex buffer, STCYCL 0x01000404, UNPACK 0x6c V4-32 ... }, and the
 *  sub-array pointers land exactly on the vertex-buffer starts (verified: the 6
 *  sub-pointers of node 06080112/nodeA[2] == the first 6 run offsets).
 *
 *  The *complete* draw traversal also runs through node-array-B (FUN_0026bc28,
 *  which references node-array-A by index) and the 8-way bone tree's draw
 *  templates (FUN_0026bf88 +0xb0) -- that full path is only partly reversed, so
 *  node-array-A alone reaches a minority of the mesh. We therefore still recover
 *  the vertex blocks by the self-validating V4-32/w=1 pattern over the whole
 *  payload (this gets the bulk coherent geometry), but now split a run at any
 *  (0,0,0) position quadword:
 *
 *  THE NEEDLE FIX.  Between VIF vertex sub-blocks the stream is padded with
 *  (0,0,0, w=1.0) quadwords (the unwritten tail of a fixed-size VU batch -- e.g.
 *  4 real verts followed by 10 zero quads). The old scanner accepted those zeros
 *  as vertices, so the triangle-strip bridged real geometry to the origin and
 *  back -- the long thin spikes ("needles") that converged to a point at (0,0,0)
 *  (gallery cases 068d0912, 06c90512). Treating a (0,0,0) position quad as a
 *  strip SEPARATOR (not a vertex) removes every origin-bridging spike while
 *  preserving the real vertices (068d0912: 7333 -> 7293 verts, only the 40
 *  padding quads dropped; origin verts in strips 40 -> 0). Exact per-leaf
 *  primitive/winding flags remain an oracle item; connectivity is taken as a
 *  triangle-strip per run (same as MOD). */
static int is_zero_pos(float x, float y, float z) {
    return x == 0.0f && y == 0.0f && z == 0.0f;
}
static int scan_bmd_floats(const uint8_t *b, size_t len, size_t start,
                           DocStrip **out, int *ncnt, int *ccap)
{
    int added = 0, ok = 1;
    size_t p = start;
    const int MINRUN = 4;                 /* >=4 verts -> >=2 triangles */
    while (p + 16 <= len) {
        /* A run must START on a real (non-zero) V4-32 vertex quadword. Zero-pad
         * quads and non-vertex words are word-stepped over. */
        ok = 1;
        float x0 = rd_f32(b, len, p,      &ok);
        float y0 = rd_f32(b, len, p + 4,  &ok);
        float z0 = rd_f32(b, len, p + 8,  &ok);
        float w0 = rd_f32(b, len, p + 12, &ok);
        if (!ok || w0 != 1.0f || !nice_coord(x0) || !nice_coord(y0) ||
            !nice_coord(z0) || is_zero_pos(x0, y0, z0)) {
            p += 4; continue;
        }
        /* extend the run; a (0,0,0) padding quad or any non-vertex word ends it */
        size_t q = p; int run = 0;
        while (q + 16 <= len) {
            ok = 1;
            float x = rd_f32(b, len, q,      &ok);
            float y = rd_f32(b, len, q + 4,  &ok);
            float z = rd_f32(b, len, q + 8,  &ok);
            float w = rd_f32(b, len, q + 12, &ok);
            if (!ok || w != 1.0f) break;
            if (!nice_coord(x) || !nice_coord(y) || !nice_coord(z)) break;
            if (is_zero_pos(x, y, z)) break;   /* padding separator ends strip */
            run++; q += 16;
        }
        if (run >= MINRUN) {
            if (run > 4096) run = 4096;    /* sanity cap */
            DocStrip s; memset(&s, 0, sizeof s);
            s.nverts = run; s.material = -1; s.group = -1;
            s.verts = (DocVertex *)calloc((size_t)run, sizeof(DocVertex));
            if (!s.verts) return added;
            for (int k = 0; k < run; k++) {
                DocVertex *vt = &s.verts[k];
                vt->x = rd_f32(b, len, p + (size_t)k*16,     &ok);
                vt->y = rd_f32(b, len, p + (size_t)k*16 + 4, &ok);
                vt->z = rd_f32(b, len, p + (size_t)k*16 + 8, &ok);
                vt->u = vt->v = 0.0f;
            }
            PUSH(*out, *ncnt, *ccap, s, oom);
            added++;
            p = q;                          /* resume at the separator/non-vertex */
        } else {
            p += 4;                         /* word-step the search */
        }
    }
    return added;
oom:
    return added;
}

static int parse_bmd(const uint8_t *b, size_t len, DocModel *m)
{
    int ok = 1;
    m->version = (int)rd_u32(b, len, 0x04, &ok);
    if (!ok || m->version < 3) return DOC_ERR_FORMAT;

    DocObject obj; memset(&obj, 0, sizeof obj);
    snprintf(obj.name, sizeof obj.name, "bmd");
    DocStrip *strips = NULL; int ns = 0, cap = 0;
    scan_bmd_floats(b, len, 0x08, &strips, &ns, &cap);   /* skip "BMD\0"+version */
    obj.strips = strips; obj.nstrips = ns;

    DocObject *objs = (DocObject *)malloc(sizeof(DocObject));
    if (!objs) {
        for (int s = 0; s < ns; s++) free(strips[s].verts);
        free(strips); return DOC_ERR_MEM;
    }
    objs[0] = obj;
    m->objs = objs; m->nobjs = 1;
    return DOC_OK;
}

/* ----------------------------------------------------------------------- *
 *  RFD container: unwrap and dispatch.  Spec: FUN_002585e0.
 *   +0x00 8B name, +0x08 4B ext, +0x0C u32 size, +0x10 u16 file count,
 *   +0x13 type flag, +0x14 u32[count] entry-offset table.
 *  Each entry: +0x00 8B name, +0x08 4B ext, +0x0C u32 size; payload at +0x10.
 * ----------------------------------------------------------------------- */
DocModel *doc_model_load_mem(const uint8_t *b, size_t len, int *err)
{
    int e = DOC_OK, ok = 1;
    DocModel *m = NULL;

    if (len < 0x18) { e = DOC_ERR_FORMAT; goto done; }
    uint16_t count = rd_u16(b, len, 0x10, &ok);
    if (!ok || count == 0 || count > 1024) { e = DOC_ERR_FORMAT; goto done; }

    for (uint16_t i = 0; i < count; i++) {
        uint32_t eoff = rd_u32(b, len, 0x14 + 4u*i, &ok);
        if (!ok || eoff + 0x10 > len) continue;
        const uint8_t *ext = b + eoff + 0x08;     /* 4-byte extension */
        int is_bmd = (ext[0]=='b'&&ext[1]=='m'&&ext[2]=='d');
        int is_mod = (ext[0]=='m'&&ext[1]=='o'&&ext[2]=='d');
        if (!is_bmd && !is_mod) continue;

        size_t poff = eoff + 0x10;                /* payload start */
        const uint8_t *pb = b + poff;
        /* bound payload by the next entry offset (entries are not guaranteed
         * sorted, so take the smallest entry offset greater than this one). */
        size_t pend = len;
        for (uint16_t j = 0; j < count; j++) {
            uint32_t oj = rd_u32(b, len, 0x14 + 4u*j, &ok);
            if (ok && oj > eoff && oj < pend) pend = oj;
        }
        size_t plen = pend - poff;
        /* verify payload magic */
        if (plen < 8) continue;
        if (is_bmd && !(pb[0]=='B'&&pb[1]=='M'&&pb[2]=='D')) continue;
        if (is_mod && !(pb[0]=='M'&&pb[1]=='O'&&pb[2]=='D')) continue;

        m = (DocModel *)calloc(1, sizeof(DocModel));
        if (!m) { e = DOC_ERR_MEM; goto done; }
        memcpy(m->kind, is_bmd ? "BMD" : "MOD", 3);

        e = is_bmd ? parse_bmd(pb, plen, m) : parse_mod(pb, plen, m);
        if (e != DOC_OK) { doc_model_free(m); m = NULL; goto done; }
        doc_model_stats(m);
        goto done;
    }
    e = DOC_ERR_FORMAT;   /* no bmd/mod entry found */
done:
    if (err) *err = e;
    return m;
}

DocModel *doc_model_load(const char *path, int *err)
{
    FILE *f = fopen(path, "rb");
    if (!f) { if (err) *err = DOC_ERR_IO; return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); if (err) *err = DOC_ERR_IO; return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); if (err) *err = DOC_ERR_MEM; return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); if (err) *err = DOC_ERR_IO; return NULL;
    }
    fclose(f);
    DocModel *m = doc_model_load_mem(buf, (size_t)n, err);
    free(buf);
    return m;
}

void doc_model_free(DocModel *m)
{
    if (!m) return;
    for (int o = 0; o < m->nobjs; o++) {
        for (int s = 0; s < m->objs[o].nstrips; s++)
            free(m->objs[o].strips[s].verts);
        free(m->objs[o].strips);
    }
    free(m->objs);
    free(m);
}

void doc_model_stats(DocModel *m)
{
    m->total_strips = m->total_verts = m->total_tris = 0;
    m->bbmin[0]=m->bbmin[1]=m->bbmin[2]= 1e30f;
    m->bbmax[0]=m->bbmax[1]=m->bbmax[2]=-1e30f;
    m->umin=m->vmin= 1e30f; m->umax=m->vmax=-1e30f;
    for (int o = 0; o < m->nobjs; o++) {
        DocObject *ob = &m->objs[o];
        m->total_strips += ob->nstrips;
        for (int s = 0; s < ob->nstrips; s++) {
            DocStrip *st = &ob->strips[s];
            m->total_verts += st->nverts;
            if (st->nverts >= 3) m->total_tris += st->nverts - 2;
            for (int v = 0; v < st->nverts; v++) {
                DocVertex *vt = &st->verts[v];
                float p[3] = { vt->x, vt->y, vt->z };
                for (int c = 0; c < 3; c++) {
                    if (p[c] < m->bbmin[c]) m->bbmin[c] = p[c];
                    if (p[c] > m->bbmax[c]) m->bbmax[c] = p[c];
                }
                if (vt->u < m->umin) m->umin = vt->u;
                if (vt->u > m->umax) m->umax = vt->u;
                if (vt->v < m->vmin) m->vmin = vt->v;
                if (vt->v > m->vmax) m->vmax = vt->v;
            }
        }
    }
    if (m->total_verts == 0) {
        for (int c=0;c<3;c++){m->bbmin[c]=m->bbmax[c]=0;}
        m->umin=m->umax=m->vmin=m->vmax=0;
    }
}
