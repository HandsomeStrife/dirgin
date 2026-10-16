/* export_gltf.c -- write a DocModel as glTF 2.0 (.gltf + .bin sidecar).
 *
 * One primitive per triangle-strip, drawn as a triangle list (mode 4) built
 * from the strip with alternating winding. Buffer layout, per strip:
 *   [ POSITION: vec3 f32 * nverts ][ TEXCOORD_0: vec2 f32 * nverts ]
 *   [ indices: u16 * 3*(nverts-2) ]
 * The .bin path is the output path with its extension replaced by .bin.
 */
#include "doc_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void bin_name(const char *gltf, char *out, size_t n)
{
    snprintf(out, n, "%s", gltf);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash)) strcpy(dot, ".bin");
    else                                strncat(out, ".bin", n - strlen(out) - 1);
}
static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int doc_export_gltf(const DocModel *m, const char *out_path)
{
    char binpath[1024];
    bin_name(out_path, binpath, sizeof binpath);

    FILE *bin = fopen(binpath, "wb");
    if (!bin) return DOC_ERR_IO;

    /* First pass: write buffer, record per-strip byte offsets/counts. */
    typedef struct { long pos_off, uv_off, idx_off; int nv, nidx, mat; } Rec;
    Rec *recs = NULL; int nrec = 0, cap = 0;
    long cur = 0;

    for (int o = 0; o < m->nobjs; o++) {
        const DocObject *ob = &m->objs[o];
        for (int s = 0; s < ob->nstrips; s++) {
            const DocStrip *st = &ob->strips[s];
            if (st->nverts < 3) continue;
            Rec r; r.nv = st->nverts; r.mat = st->material;
            r.pos_off = cur;
            for (int v = 0; v < st->nverts; v++) {
                float p[3] = { st->verts[v].x, st->verts[v].y, st->verts[v].z };
                fwrite(p, 4, 3, bin); cur += 12;
            }
            r.uv_off = cur;
            for (int v = 0; v < st->nverts; v++) {
                float t[2] = { st->verts[v].u, st->verts[v].v };
                fwrite(t, 4, 2, bin); cur += 8;
            }
            r.idx_off = cur;
            int ni = 0;
            for (int v = 0; v + 2 < st->nverts; v++) {
                uint16_t a = (uint16_t)v, b = (uint16_t)(v+1), c = (uint16_t)(v+2);
                if (v & 1) { uint16_t t=b; b=c; c=t; }
                fwrite(&a,2,1,bin); fwrite(&b,2,1,bin); fwrite(&c,2,1,bin);
                cur += 6; ni += 3;
            }
            /* pad buffer to 4 bytes (glTF alignment for next vec3) */
            while (cur & 3) { uint8_t z=0; fwrite(&z,1,1,bin); cur++; }
            r.nidx = ni;
            if (nrec == cap) { cap = cap?cap*2:16; recs = realloc(recs, cap*sizeof(Rec)); if(!recs){fclose(bin);return DOC_ERR_MEM;} }
            recs[nrec++] = r;
        }
    }
    long binlen = cur;
    fclose(bin);

    /* Second pass: write JSON. Three accessors/bufferViews per strip. */
    FILE *g = fopen(out_path, "w");
    if (!g) { free(recs); return DOC_ERR_IO; }

    fprintf(g, "{\n");
    fprintf(g, "  \"asset\": {\"version\": \"2.0\", \"generator\": \"doc_model (DoC native loader)\"},\n");
    fprintf(g, "  \"scene\": 0,\n");
    fprintf(g, "  \"scenes\": [{\"nodes\": [0]}],\n");
    fprintf(g, "  \"nodes\": [{\"mesh\": 0, \"name\": \"%s\"}],\n", m->kind);
    fprintf(g, "  \"buffers\": [{\"uri\": \"%s\", \"byteLength\": %ld}],\n",
            base_name(binpath), binlen);

    /* bufferViews: 3 per strip */
    fprintf(g, "  \"bufferViews\": [\n");
    for (int i = 0; i < nrec; i++) {
        Rec *r = &recs[i];
        fprintf(g, "    {\"buffer\":0,\"byteOffset\":%ld,\"byteLength\":%d,\"target\":34962},\n",
                r->pos_off, r->nv*12);
        fprintf(g, "    {\"buffer\":0,\"byteOffset\":%ld,\"byteLength\":%d,\"target\":34962},\n",
                r->uv_off, r->nv*8);
        fprintf(g, "    {\"buffer\":0,\"byteOffset\":%ld,\"byteLength\":%d,\"target\":34963}%s\n",
                r->idx_off, r->nidx*2, (i==nrec-1)?"":",");
    }
    fprintf(g, "  ],\n");

    /* accessors: 3 per strip (POSITION needs min/max) */
    fprintf(g, "  \"accessors\": [\n");
    for (int i = 0; i < nrec; i++) {
        Rec *r = &recs[i];
        /* recompute this strip's bbox for POSITION min/max */
        /* find strip again: positions already in bin; we trust global bbox is fine,
         * but glTF validators want per-accessor min/max -> compute from recs order */
        /* We re-read from model structure: */
        /* locate strip by linear index i over valid strips */
        float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};
        int idx=0, found=0;
        for (int o=0;o<m->nobjs && !found;o++)
          for (int s=0;s<m->objs[o].nstrips && !found;s++){
            if (m->objs[o].strips[s].nverts<3) continue;
            if (idx==i){ const DocStrip*st=&m->objs[o].strips[s];
                for(int v=0;v<st->nverts;v++){float p[3]={st->verts[v].x,st->verts[v].y,st->verts[v].z};
                  for(int c=0;c<3;c++){if(p[c]<mn[c])mn[c]=p[c];if(p[c]>mx[c])mx[c]=p[c];}}
                found=1;}
            idx++;
          }
        fprintf(g, "    {\"bufferView\":%d,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\","
                   "\"min\":[%g,%g,%g],\"max\":[%g,%g,%g]},\n",
                i*3, r->nv, mn[0],mn[1],mn[2], mx[0],mx[1],mx[2]);
        fprintf(g, "    {\"bufferView\":%d,\"componentType\":5126,\"count\":%d,\"type\":\"VEC2\"},\n",
                i*3+1, r->nv);
        fprintf(g, "    {\"bufferView\":%d,\"componentType\":5123,\"count\":%d,\"type\":\"SCALAR\"}%s\n",
                i*3+2, r->nidx, (i==nrec-1)?"":",");
    }
    fprintf(g, "  ],\n");

    /* one mesh with one primitive per strip */
    fprintf(g, "  \"meshes\": [{\"name\": \"%s\", \"primitives\": [\n", m->kind);
    for (int i = 0; i < nrec; i++) {
        fprintf(g, "    {\"attributes\": {\"POSITION\": %d, \"TEXCOORD_0\": %d}, \"indices\": %d, \"mode\": 4}%s\n",
                i*3, i*3+1, i*3+2, (i==nrec-1)?"":",");
    }
    fprintf(g, "  ]}]\n");
    fprintf(g, "}\n");
    fclose(g);
    free(recs);
    return DOC_OK;
}
