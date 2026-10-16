/* =====================================================================
 * doc_env.c -- see doc_env.h. Faithful C port of
 * export_bmd_textured.py:{find_bmd_payload, decode_packets, packet_tris}.
 * ===================================================================== */
#include "doc_env.h"

#include <stdlib.h>
#include <string.h>

/* ---- little-endian readers (bounds are checked by callers) ------------ */
static uint16_t rd_u16(const uint8_t *b, long o) { uint16_t v; memcpy(&v, b + o, 2); return v; }
static uint32_t rd_u32(const uint8_t *b, long o) { uint32_t v; memcpy(&v, b + o, 4); return v; }
static int32_t  rd_i32(const uint8_t *b, long o) { int32_t  v; memcpy(&v, b + o, 4); return v; }
static int16_t  rd_s16(const uint8_t *b, long o) { int16_t  v; memcpy(&v, b + o, 2); return v; }
static uint64_t rd_u64(const uint8_t *b, long o) { uint64_t v; memcpy(&v, b + o, 8); return v; }

/* VIF UNPACK VL table (bits per component): vl 0->32, 1->16, 2->8, 3->5. */
static const int VL_BITS[4] = { 32, 16, 8, 5 };

#define POS_SCALE (1.0f / 16.0f)    /* ITOF4  */
#define UV_SCALE  (1.0f / 4096.0f)  /* ITOF12 */

long doc_env_find_payload(const uint8_t *buf, size_t size)
{
    long N = (long)size;
    if (N < 0x18) return -1;
    unsigned cnt = rd_u16(buf, 0x10);
    if (cnt == 0 || cnt > 1024) return -1;
    for (unsigned i = 0; i < cnt; i++) {
        if (0x14 + 4L * i + 4 > N) break;
        uint32_t eoff = rd_u32(buf, 0x14 + 4L * i);
        if ((long)eoff + 0x10 > N) continue;
        if (memcmp(buf + eoff + 8, "bmd", 3) == 0 &&
            memcmp(buf + eoff + 0x10, "BMD", 3) == 0)
            return (long)eoff + 0x10;
    }
    return -1;
}

/* Decode one VIF UNPACK command word at offset o. Returns 1 and fills the
 * out-params on success (an UNPACK command), 0 otherwise. */
static int rd_unpack(const uint8_t *b, long N, long o,
                     int *nc, int *bits, int *num, int *addr, long *doff, long *nbytes)
{
    if (o + 4 > N) return 0;
    uint32_t c = rd_u32(b, o);
    int n = (c >> 16) & 0xff;
    int cmd = (c >> 24) & 0x7f;
    if (!(0x60 <= cmd && cmd <= 0x7f)) return 0;
    int vn = (cmd >> 2) & 3;
    int vl = cmd & 3;
    int bb = VL_BITS[vl];
    int ncc = vn + 1;
    *nc = ncc; *bits = bb; *num = n;
    *addr = (c & 0xffff) & 0x3ff;
    *doff = o + 4;
    *nbytes = (((long)ncc * bb + 7) / 8) * n;
    return 1;
}

static DocEnvPacket *push_packet(DocEnvPackets *out, int *cap)
{
    if (out->n == *cap) {
        int nc = *cap ? *cap * 2 : 64;
        DocEnvPacket *np = (DocEnvPacket *)realloc(out->pk, (size_t)nc * sizeof *np);
        if (!np) return NULL;
        out->pk = np; *cap = nc;
    }
    DocEnvPacket *pk = &out->pk[out->n];
    memset(pk, 0, sizeof *pk);
    return pk;
}

int doc_env_decode(const uint8_t *buf, size_t size, long payload, DocEnvPackets *out)
{
    long N = (long)size;
    memset(out, 0, sizeof *out);
    if (payload < 0 || payload + 8 > N) return 1;
    int cap = 0;

    /* start at the first STCYCL cl4/wl4 (0x01000404) */
    long start = payload + 8;
    for (long o = payload + 8; o + 4 <= N; o += 4) {
        if (rd_u32(buf, o) == 0x01000404) { start = o; break; }
    }

    long p = start;
    while (p + 4 <= N) {
        uint32_t c = rd_u32(buf, p);
        int cmd = (c >> 24) & 0x7f;
        int imm = c & 0xffff;
        if (!(cmd == 0x01 && imm == 0x0404)) { p += 4; continue; }

        long pp = p + 4;
        int sel = 0, prim = 0, abe = 0, is_list = 0, mode = 0;
        int32_t row[4] = { 0, 0, 0, 0 };

        float   *pos = NULL, *uv = NULL;
        int32_t *pos_i = NULL;
        int16_t *uv_i = NULL;
        uint8_t *col = NULL;
        int nv_pos = 0, nv_uv = 0, nv_col = 0;

        int nc, bits, num, addr; long doff, nb;

        /* per-submesh header: UNPACK V4-32 num=2 @0 -> two control qwords */
        if (rd_unpack(buf, N, pp, &nc, &bits, &num, &addr, &doff, &nb) &&
            nc == 4 && bits == 32 && addr == 0) {
            long gt = doff;
            if (gt + 24 <= N) {
                uint64_t lo = rd_u64(buf, gt);
                prim = (lo >> 47) & 0x7ff;
                abe = (prim & 0x40) ? 1 : 0;
                sel = rd_u32(buf, gt + 16) & 0xff;
                is_list = (rd_u32(buf, gt + 20) & 0x8000) ? 1 : 0;
            }
            pp = doff + nb;
        }

        int guard = 0;
        while (pp + 4 <= N && guard < 48) {
            guard++;
            uint32_t cc = rd_u32(buf, pp);
            int cm = (cc >> 24) & 0x7f;
            int im = cc & 0xffff;
            if (cm == 0x01) { pp += 4; continue; }              /* STCYCL */
            if (cm == 0x05) { mode = im; pp += 4; continue; }   /* STMOD  */
            if (cm == 0x20) { pp += 8; continue; }              /* STMASK */
            if (cm == 0x30) {                                   /* STROW  */
                if (pp + 20 > N) break;
                row[0] = rd_i32(buf, pp + 4);  row[1] = rd_i32(buf, pp + 8);
                row[2] = rd_i32(buf, pp + 12); row[3] = rd_i32(buf, pp + 16);
                pp += 20; continue;
            }
            if (cm == 0x17) { pp += 4; break; }                 /* MSCNT -> end */

            if (!rd_unpack(buf, N, pp, &nc, &bits, &num, &addr, &doff, &nb)) { pp += 4; continue; }
            if (doff + nb > N) break;

            if (addr == 2 && nc == 3 && bits == 16) {           /* POSITION */
                pos   = (float *)  malloc((size_t)num * 3 * sizeof(float));
                pos_i = (int32_t *)malloc((size_t)num * 3 * sizeof(int32_t));
                if (!pos || !pos_i) { free(pos); free(pos_i); goto fail; }
                int32_t acc[3] = { row[0], row[1], row[2] };
                for (int k = 0; k < num; k++) {
                    int32_t r0 = rd_s16(buf, doff + 6 * k);
                    int32_t r1 = rd_s16(buf, doff + 6 * k + 2);
                    int32_t r2 = rd_s16(buf, doff + 6 * k + 4);
                    int32_t x, y, z;
                    if (mode == 2) { acc[0] += r0; acc[1] += r1; acc[2] += r2; x = acc[0]; y = acc[1]; z = acc[2]; }
                    else if (mode == 1) { x = r0 + row[0]; y = r1 + row[1]; z = r2 + row[2]; }
                    else { x = r0; y = r1; z = r2; }
                    pos_i[3 * k] = x; pos_i[3 * k + 1] = y; pos_i[3 * k + 2] = z;
                    pos[3 * k] = x * POS_SCALE; pos[3 * k + 1] = y * POS_SCALE; pos[3 * k + 2] = z * POS_SCALE;
                }
                nv_pos = num;
            } else if (addr == 3 && nc == 4 && bits == 8) {     /* RGBA colour */
                col = (uint8_t *)malloc((size_t)num * 4);
                if (!col) goto fail;
                memcpy(col, buf + doff, (size_t)num * 4);
                nv_col = num;
            } else if (addr == 4 && nc == 2 && bits == 16) {    /* UV */
                uv   = (float *)  malloc((size_t)num * 2 * sizeof(float));
                uv_i = (int16_t *)malloc((size_t)num * 2 * sizeof(int16_t));
                if (!uv || !uv_i) { free(uv); free(uv_i); goto fail; }
                for (int k = 0; k < num; k++) {
                    int16_t u0 = rd_s16(buf, doff + 4 * k);
                    int16_t v0 = rd_s16(buf, doff + 4 * k + 2);
                    uv_i[2 * k] = u0; uv_i[2 * k + 1] = v0;
                    uv[2 * k] = u0 * UV_SCALE; uv[2 * k + 1] = v0 * UV_SCALE;
                }
                nv_uv = num;
            }
            pp += 4 + nb;
            if (pp & 3) pp = (pp + 3) & ~3L;
            continue;

        fail:
            free(pos); free(pos_i); free(uv); free(uv_i); free(col);
            doc_env_free(out);
            return 2;
        }

        if (pos && col && uv && nv_pos == nv_col && nv_pos == nv_uv && nv_pos >= 3) {
            DocEnvPacket *pk = push_packet(out, &cap);
            if (!pk) { free(pos); free(pos_i); free(uv); free(uv_i); free(col); doc_env_free(out); return 3; }
            pk->nv = nv_pos; pk->is_list = is_list; pk->sel = sel; pk->prim = prim; pk->abe = abe;
            pk->pos = pos; pk->pos_i = pos_i; pk->uv = uv; pk->uv_i = uv_i; pk->col = col;
            out->n++;
        } else {
            free(pos); free(pos_i); free(uv); free(uv_i); free(col);
        }
        p = pp;
    }
    return 0;
}

void doc_env_free(DocEnvPackets *p)
{
    if (!p || !p->pk) { if (p) { p->pk = NULL; p->n = 0; } return; }
    for (int i = 0; i < p->n; i++) {
        free(p->pk[i].pos); free(p->pk[i].pos_i);
        free(p->pk[i].uv);  free(p->pk[i].uv_i);
        free(p->pk[i].col);
    }
    free(p->pk);
    p->pk = NULL; p->n = 0;
}

int doc_env_packet_tris(const DocEnvPacket *pk, int *idx, int max_tris)
{
    int n = pk->nv, t = 0;
    if (pk->is_list) {
        for (int i = 0; i + 3 <= n && t < max_tris; i += 3, t++) {
            idx[3 * t] = i; idx[3 * t + 1] = i + 1; idx[3 * t + 2] = i + 2;
        }
    } else {
        int g = n / 4;
        for (int gi = 0; gi < g && t + 2 <= max_tris; gi++) {
            int b = 4 * gi;
            idx[3 * t] = b;     idx[3 * t + 1] = b + 1; idx[3 * t + 2] = b + 2; t++;
            idx[3 * t] = b + 2; idx[3 * t + 1] = b + 1; idx[3 * t + 2] = b + 3; t++;
        }
        if (n - 4 * g == 3 && t < max_tris) {
            int b = 4 * g;
            idx[3 * t] = b; idx[3 * t + 1] = b + 1; idx[3 * t + 2] = b + 2; t++;
        }
    }
    return t;
}

/* FNV-1a 64 over the canonical integer stream (pos_i/uv_i/col + counts). */
static void fnv_bytes(uint64_t *h, const void *data, size_t n)
{
    const uint8_t *d = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) { *h ^= d[i]; *h *= 0x100000001b3ULL; }
}
static void fnv_i32(uint64_t *h, int32_t v) { fnv_bytes(h, &v, 4); }

uint64_t doc_env_fnv(const DocEnvPackets *p)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < p->n; i++) {
        const DocEnvPacket *pk = &p->pk[i];
        fnv_i32(&h, pk->nv); fnv_i32(&h, pk->is_list); fnv_i32(&h, pk->sel);
        fnv_i32(&h, pk->prim); fnv_i32(&h, pk->abe);
        fnv_bytes(&h, pk->pos_i, (size_t)pk->nv * 3 * sizeof(int32_t));
        fnv_bytes(&h, pk->uv_i,  (size_t)pk->nv * 2 * sizeof(int16_t));
        fnv_bytes(&h, pk->col,   (size_t)pk->nv * 4);
    }
    return h;
}
