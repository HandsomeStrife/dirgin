/* web_export.c - merge one or more CID collision files into a compact binary
 * for the browser prototype ("DCOL" v2, little-endian). C99, no deps.
 *
 *   cidweb <out.bin> <mapid.rfd>...
 *
 * Vertices are welded by exact float bits (the game's own data shares them).
 * Format (all little-endian, every section 4-byte aligned; see FORMAT.md 8):
 *   0x00 char[4] "DCOL"
 *   0x04 u32 version (2; v1 = the same without the walk-mesh section)
 *   0x08 u32 nverts
 *   0x0c u32 ntris
 *   0x10 f32 bbox min xyz, 0x1c f32 bbox max xyz
 *   0x28 u32 nfiles (merged input files)
 *   0x2c u32 reserved (0)
 *   0x30 f32 verts[nverts*3]       x,y,z game world units, -Y up (gravity +Y)
 *        u32 tris[ntris*3]         vertex indices
 *        u32 surface[ntris]        per-face surface word (FORMAT.md 5)
 *        u32 info[ntris]           bits 0-1 sub-tree (0=+0x38 segment/wall,
 *                                  1=+0x58 floor, 2=+0x78, 3=+0x98 nearest),
 *                                  bits 2-3 source (cid_tri_source),
 *                                  bits 8-15 input file index (argv order),
 *                                  bits 16-31 tag-2 packet surface mask
 *        u32 npolys, u32 reserved (0)   walk mesh (FORMAT.md 3d, 8)
 *        u32 poly[npolys*10]       per polygon: nv | flags<<16 (face +0x04),
 *                                  word6 (face +0x06) | file<<16,
 *                                  v[4] vertex indices (nv used),
 *                                  nbr[4] polygon index or 0xffffffff
 */
#include "cid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t k[3]; uint32_t idx; int used; } slot;

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <out.bin> <mapid.rfd>...\n", argv[0]);
        return 2;
    }
    int nf = argc - 2;
    if (nf > 256) { fprintf(stderr, "at most 256 input files (info bits 8-15)\n"); return 2; }
    cid_mesh *ms = calloc((size_t)nf, sizeof *ms);
    if (!ms) { fprintf(stderr, "oom\n"); return 1; }
    size_t tot_v = 0, tot_t = 0, tot_p = 0;
    for (int i = 0; i < nf; i++) {
        char err[256];
        int r = cid_load_file(argv[i + 2], &ms[i], err, sizeof err);
        if (r != CID_OK) {
            fprintf(stderr, "error: %s: %s\n", argv[i + 2], cid_strerror(r));
            return 1;
        }
        if (ms[i].nboxes)
            fprintf(stderr, "warn: %s has %d undecoded AABB leaves (not exported)\n",
                    argv[i + 2], ms[i].nboxes);
        tot_v += (size_t)ms[i].nverts; tot_t += (size_t)ms[i].ntris;
        tot_p += (size_t)ms[i].npolys;
        if (ms[i].n_poly_dup_off)
            fprintf(stderr, "warn: %s: %d walk-mesh records reached twice (pointers take the first)\n",
                    argv[i + 2], ms[i].n_poly_dup_off);
        if (ms[i].n_poly_dangling)
            fprintf(stderr, "warn: %s: %d walk-mesh neighbour pointers match no polygon (left as walls)\n",
                    argv[i + 2], ms[i].n_poly_dangling);
    }

    size_t cap = 1; while (cap < tot_v * 2 + 16) cap <<= 1;
    slot *h = calloc(cap, sizeof *h);
    float *V = malloc(tot_v * 3 * sizeof(float) + 12);
    uint32_t *T = malloc(tot_t * 3 * 4 + 4), *S = malloc(tot_t * 4 + 4),
             *I = malloc(tot_t * 4 + 4);
    uint32_t *P = malloc(tot_p * 40 + 4);
    if (!h || !V || !T || !S || !I || !P) { fprintf(stderr, "oom\n"); return 1; }
    uint32_t nv = 0, nt = 0, np = 0;
    float bmin[3] = { 1e30f, 1e30f, 1e30f }, bmax[3] = { -1e30f, -1e30f, -1e30f };

    for (int f = 0; f < nf; f++) {
        cid_mesh *m = &ms[f];
        uint32_t *remap = malloc((size_t)m->nverts * 4 + 4);
        if (!remap) { fprintf(stderr, "oom\n"); return 1; }
        for (int i = 0; i < m->nverts; i++) {
            float p[3] = { m->verts[i].x, m->verts[i].y, m->verts[i].z };
            uint32_t k[3] = { fbits(p[0]), fbits(p[1]), fbits(p[2]) };
            size_t hv = (k[0] * 73856093u ^ k[1] * 19349663u ^ k[2] * 83492791u) & (cap - 1);
            while (h[hv].used && memcmp(h[hv].k, k, 12) != 0) hv = (hv + 1) & (cap - 1);
            if (!h[hv].used) {
                h[hv].used = 1; memcpy(h[hv].k, k, 12); h[hv].idx = nv;
                memcpy(&V[nv * 3], p, 12);
                for (int a = 0; a < 3; a++) {
                    if (p[a] < bmin[a]) bmin[a] = p[a];
                    if (p[a] > bmax[a]) bmax[a] = p[a];
                }
                nv++;
            }
            remap[i] = h[hv].idx;
        }
        for (int t = 0; t < m->ntris; t++) {
            const cid_tri *tr = &m->tris[t];
            for (int a = 0; a < 3; a++) T[nt * 3 + a] = remap[tr->v[a]];
            S[nt] = tr->surface;
            I[nt] = (uint32_t)(tr->subtree & 3) | ((uint32_t)(tr->source & 3) << 2)
                  | ((uint32_t)(f & 0xff) << 8) | ((uint32_t)tr->mask << 16);
            nt++;
        }
        uint32_t pbase = np;            /* neighbour indices are per file: offset them */
        for (int q = 0; q < m->npolys; q++) {
            const cid_poly *pl = &m->polys[q];
            uint32_t *w = &P[(size_t)np * 10];
            w[0] = (uint32_t)pl->nv | ((uint32_t)pl->flags << 16);
            w[1] = (uint32_t)pl->word6 | ((uint32_t)(f & 0xffff) << 16);
            for (int a = 0; a < 4; a++) {
                w[2 + a] = a < pl->nv ? remap[pl->v[a]] : 0;
                w[6 + a] = (a < pl->nv && pl->nbr[a] >= 0) ? pbase + (uint32_t)pl->nbr[a] : 0xffffffffu;
            }
            np++;
        }
        free(remap);
    }

    FILE *o = fopen(argv[1], "wb");
    if (!o) { fprintf(stderr, "error: cannot write %s\n", argv[1]); return 1; }
    uint32_t hdr_u[4] = { 0, 2, nv, nt };
    memcpy(&hdr_u[0], "DCOL", 4);
    uint32_t tail[2] = { (uint32_t)nf, 0 };
    /* Written in host order: the format is little-endian, so this tool must run on a LE host. */
    { uint32_t one = 1; if (*(uint8_t *)&one != 1) { fprintf(stderr, "big-endian host not supported\n"); return 1; } }
    size_t ok = 1;
    ok &= fwrite(hdr_u, 4, 4, o) == 4;
    ok &= fwrite(bmin, 4, 3, o) == 3; ok &= fwrite(bmax, 4, 3, o) == 3;
    ok &= fwrite(tail, 4, 2, o) == 2;
    ok &= fwrite(V, 4, (size_t)nv * 3, o) == (size_t)nv * 3;
    ok &= fwrite(T, 4, (size_t)nt * 3, o) == (size_t)nt * 3;
    ok &= fwrite(S, 4, nt, o) == nt;
    ok &= fwrite(I, 4, nt, o) == nt;
    uint32_t ph[2] = { np, 0 };
    ok &= fwrite(ph, 4, 2, o) == 2;
    ok &= fwrite(P, 4, (size_t)np * 10, o) == (size_t)np * 10;
    long sz = ftell(o);
    if (fclose(o) != 0) ok = 0;
    if (!ok) { fprintf(stderr, "write failed: %s\n", argv[1]); return 1; }
    printf("wrote %s: %u verts (welded from %zu), %u tris, %u walk polygons, %d files, %ld bytes\n",
           argv[1], nv, tot_v, nt, np, nf, sz);
    printf("bbox %.2f %.2f %.2f .. %.2f %.2f %.2f\n",
           bmin[0], bmin[1], bmin[2], bmax[0], bmax[1], bmax[2]);
    for (int i = 0; i < nf; i++) cid_free(&ms[i]);
    free(ms); free(h); free(V); free(T); free(S); free(I); free(P);
    return 0;
}
