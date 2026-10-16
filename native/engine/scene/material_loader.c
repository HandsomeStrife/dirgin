/* =====================================================================
 * material_loader.c  --  See material_loader.h.
 *
 * Reads a baked binding manifest and synthesizes the DocMaterialModel +
 * DocTextureContext that material.c consumes. Pure C + stdio (no libdtex,
 * no JSON lib): the manifest is a trivial line-based text format so the
 * dependency-free engine core stays dependency-free.
 *
 * Manifest grammar (emit_loader_manifest.py), '#'/blank lines ignored:
 *     chunk   <id8>
 *     slots   <N>
 *     slot <i> <filecode8> <count> <source>      (N lines, i = 0..N-1)
 *     packets <M>
 *     <slot>                                      (M lines; -1 == unbound)
 * ===================================================================== */
#include "material_loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A non-null, never-dereferenced submesh sentinel. material.c's select path
 * only checks the submesh pointer for non-null and returns it; the geometry
 * leaf is a separate subsystem (P1), below this seam. */
static char              g_submesh_sentinel_byte;
#define SUBMESH_SENTINEL ((DocSubmesh *)&g_submesh_sentinel_byte)

#define DOC_MAX_GROUPS   128   /* draw word group field is 7 bits   */
#define DOC_MAX_SUBMESH  512   /* draw word sub field is 9 bits     */

static void seterr(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || errlen == 0) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* skip leading whitespace, return pointer; "" for blank/comment lines */
static const char *lstrip(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

int doc_room_binding_load(const char *manifest_path, DocRoomBinding *out,
                          char *err, size_t errlen)
{
    if (!out) { seterr(err, errlen, "null out"); return 1; }
    memset(out, 0, sizeof *out);

    FILE *f = fopen(manifest_path, "r");
    if (!f) { seterr(err, errlen, "cannot open manifest: %s", manifest_path); return 1; }

    char line[512];
    int nslots = -1, npackets = -1;
    int slots_read = 0, packets_read = 0;
    int rc = 1;                 /* assume failure until the final success path */

    while (fgets(line, sizeof line, f)) {
        const char *p = lstrip(line);
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;

        if (nslots < 0 && strncmp(p, "chunk", 5) == 0) {
            if (sscanf(p, "chunk %8s", out->chunk) != 1) { seterr(err, errlen, "bad chunk line"); goto done; }
            continue;
        }
        if (nslots < 0 && strncmp(p, "slots", 5) == 0) {
            if (sscanf(p, "slots %d", &nslots) != 1 || nslots < 0) { seterr(err, errlen, "bad slots line"); goto done; }
            out->nslots = nslots;
            out->slot_filecode = calloc((size_t)(nslots ? nslots : 1), sizeof *out->slot_filecode);
            out->slot_count    = calloc((size_t)(nslots ? nslots : 1), sizeof *out->slot_count);
            if (!out->slot_filecode || !out->slot_count) { seterr(err, errlen, "oom slots"); goto done; }
            continue;
        }
        if (nslots >= 0 && slots_read < nslots && strncmp(p, "slot ", 5) == 0) {
            int idx = -1, cnt = -1; char fc[16] = {0}, src[16] = {0};
            if (sscanf(p, "slot %d %15s %d %15s", &idx, fc, &cnt, src) < 3) { seterr(err, errlen, "bad slot line"); goto done; }
            if (idx != slots_read || idx < 0 || idx >= nslots || cnt < 0) { seterr(err, errlen, "slot index/count out of order at %d", idx); goto done; }
            strncpy(out->slot_filecode[idx], fc, 8); out->slot_filecode[idx][8] = '\0';
            out->slot_count[idx] = cnt;
            slots_read++;
            continue;
        }
        if (npackets < 0 && strncmp(p, "packets", 7) == 0) {
            if (slots_read != nslots) { seterr(err, errlen, "only %d/%d slot lines before packets", slots_read, nslots); goto done; }
            if (sscanf(p, "packets %d", &npackets) != 1 || npackets < 0) { seterr(err, errlen, "bad packets line"); goto done; }
            out->npackets = npackets;
            out->expect_slot = calloc((size_t)(npackets ? npackets : 1), sizeof *out->expect_slot);
            out->draw_words  = calloc((size_t)(npackets ? npackets : 1), sizeof *out->draw_words);
            if (!out->expect_slot || !out->draw_words) { seterr(err, errlen, "oom packets"); goto done; }
            continue;
        }
        if (npackets >= 0 && packets_read < npackets) {
            int s = -2;
            if (sscanf(p, "%d", &s) != 1 || s < -1 || s >= nslots) { seterr(err, errlen, "bad packet slot '%s'", p); goto done; }
            out->expect_slot[packets_read++] = s;
            continue;
        }
        seterr(err, errlen, "unexpected line: %s", p);
        goto done;
    }

    if (nslots < 0 || npackets < 0 || slots_read != nslots || packets_read != npackets) {
        seterr(err, errlen, "truncated manifest (slots %d/%d, packets %d/%d)",
               slots_read, nslots, packets_read, npackets);
        goto done;
    }

    /* ---- synthesize the model ------------------------------------------ *
     * Buckets: 0..S-1 = bound FileCodes (one material/texture slot each),
     * bucket S = the default/unbound packets. A bucket with more than
     * DOC_MAX_SUBMESH packets is SHARDED into several groups that all share
     * the bucket's one material (the draw-word sub field is only 9 bits).  */
    {
        const int S  = nslots;
        const int NB = S + 1;                /* buckets (slots + default)    */

        out->unbound_count = 0;
        for (int i = 0; i < npackets; i++)
            if (out->expect_slot[i] < 0) out->unbound_count++;

        int *bk_count  = calloc((size_t)NB, sizeof *bk_count);   /* packets per bucket */
        int *bk_gstart = calloc((size_t)NB, sizeof *bk_gstart);  /* first group of bucket */
        int *bk_nshard = calloc((size_t)NB, sizeof *bk_nshard);  /* groups in bucket */
        if (!bk_count || !bk_gstart || !bk_nshard) {
            free(bk_count); free(bk_gstart); free(bk_nshard);
            seterr(err, errlen, "oom buckets"); goto done;
        }
        for (int b = 0; b < S; b++) bk_count[b] = out->slot_count[b];
        bk_count[S] = out->unbound_count;

        int G = 0;                           /* total groups across buckets  */
        for (int b = 0; b < NB; b++) {
            int c = bk_count[b];
            int nsh = (c > 0) ? (c + DOC_MAX_SUBMESH - 1) / DOC_MAX_SUBMESH : 0;
            bk_gstart[b] = G;
            bk_nshard[b] = nsh;
            G += nsh;
        }
        if (G > DOC_MAX_GROUPS) {
            free(bk_count); free(bk_gstart); free(bk_nshard);
            seterr(err, errlen, "group_count %d exceeds %d (draw-word limit)", G, DOC_MAX_GROUPS);
            goto done;
        }

        out->groups_store  = calloc((size_t)(G ? G : 1), sizeof *out->groups_store);
        out->group_ptrs    = calloc((size_t)(G ? G : 1), sizeof *out->group_ptrs);
        out->mats_store    = calloc((size_t)NB, sizeof *out->mats_store);
        out->mat_ptrs      = calloc((size_t)NB, sizeof *out->mat_ptrs);
        out->handles_store = calloc((size_t)(S ? S : 1), sizeof *out->handles_store);
        out->handle_ptrs   = calloc((size_t)(S ? S : 1), sizeof *out->handle_ptrs);
        out->submesh_pool  = calloc((size_t)(npackets ? npackets : 1), sizeof *out->submesh_pool);
        out->lists_backing = calloc((size_t)(2 * NB), sizeof *out->lists_backing);
        out->list_ptr_store= calloc((size_t)(DOC_MATERIAL_MAX_LAYER_LISTS * NB), sizeof *out->list_ptr_store);
        if (!out->groups_store || !out->group_ptrs || !out->mats_store || !out->mat_ptrs ||
            !out->handles_store || !out->handle_ptrs || !out->submesh_pool ||
            !out->lists_backing || !out->list_ptr_store) {
            free(bk_count); free(bk_gstart); free(bk_nshard);
            seterr(err, errlen, "oom model"); goto done;
        }

        /* texture context: one non-null handle per slot */
        for (int s = 0; s < S; s++)
            out->handle_ptrs[s] = &out->handles_store[s];
        out->ctx.handles      = out->handle_ptrs;
        out->ctx.handle_count = S;

        /* submesh pool: all sentinels (geometry leaf is below the seam) */
        for (int i = 0; i < npackets; i++)
            out->submesh_pool[i] = SUBMESH_SENTINEL;

        /* one material per bucket: bound bucket b (<S) -> single mode-0 layer,
         * tag == slot b; default bucket (b == S) -> single mode-8 layer ->
         * default texture (honest untextured). */
        for (int b = 0; b < NB; b++) {
            uint16_t tag = (b < S) ? (uint16_t)b : (uint16_t)0x8000u;
            out->lists_backing[b * 2 + 0] = 1;      /* layer count */
            out->lists_backing[b * 2 + 1] = tag;    /* the one tag  */
            out->list_ptr_store[b * DOC_MATERIAL_MAX_LAYER_LISTS + 0] = &out->lists_backing[b * 2];
            out->mats_store[b].stage0 = 3;
            out->mats_store[b].layer_lists =
                (const uint16_t *const *)&out->list_ptr_store[b * DOC_MATERIAL_MAX_LAYER_LISTS];
            out->mat_ptrs[b] = &out->mats_store[b];
        }

        /* groups: each shard of a bucket is a group selecting the bucket's
         * material; submesh_count = packets in that shard (<= DOC_MAX_SUBMESH). */
        for (int b = 0; b < NB; b++) {
            for (int k = 0; k < bk_nshard[b]; k++) {
                int g   = bk_gstart[b] + k;
                int rem = bk_count[b] - k * DOC_MAX_SUBMESH;
                int cnt = (rem < DOC_MAX_SUBMESH) ? rem : DOC_MAX_SUBMESH;
                out->groups_store[g].submesh_count     = (uint16_t)cnt;
                out->groups_store[g].material_selector = (uint16_t)b;   /* -> material b */
                out->groups_store[g].submeshes         = out->submesh_pool;
                out->group_ptrs[g] = &out->groups_store[g];
            }
        }

        out->model.group_count = (uint16_t)G;
        out->model.groups       = out->group_ptrs;
        out->model.materials    = out->mat_ptrs;

        /* per-packet draw word: bucket = slot (or default S); a running index
         * within the bucket picks the shard group (r / 512) and the submesh
         * (r % 512), preserving decode order. */
        int *run = calloc((size_t)NB, sizeof *run);
        if (!run) { free(bk_count); free(bk_gstart); free(bk_nshard); seterr(err, errlen, "oom run"); goto done; }
        for (int i = 0; i < npackets; i++) {
            int s = out->expect_slot[i];
            int b = (s >= 0) ? s : S;
            int r = run[b]++;
            int g = bk_gstart[b] + r / DOC_MAX_SUBMESH;
            int sub = r % DOC_MAX_SUBMESH;
            out->draw_words[i] = ((uint32_t)g << DOC_DRAW_GROUP_SHIFT) | (uint32_t)sub;
        }
        free(run);
        free(bk_count); free(bk_gstart); free(bk_nshard);
    }

    rc = 0;   /* success */

done:
    fclose(f);
    if (rc != 0)
        doc_room_binding_free(out);
    return rc;
}

void doc_room_binding_free(DocRoomBinding *b)
{
    if (!b) return;
    free(b->slot_filecode);
    free(b->slot_count);
    free(b->draw_words);
    free(b->expect_slot);
    free(b->groups_store);
    free(b->group_ptrs);
    free(b->mats_store);
    free(b->mat_ptrs);
    free(b->handles_store);
    free(b->handle_ptrs);
    free(b->submesh_pool);
    free(b->lists_backing);
    free(b->list_ptr_store);
    memset(b, 0, sizeof *b);
}

const char *doc_room_binding_slot_filecode(const DocRoomBinding *b, int32_t slot)
{
    if (!b || slot < 0 || slot >= b->nslots || !b->slot_filecode)
        return NULL;
    return b->slot_filecode[slot];
}
