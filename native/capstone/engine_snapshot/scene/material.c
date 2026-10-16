/* =====================================================================
 * material.c  --  Reauthored material -> texture binding subsystem.
 * See material.h for the oracle mapping and guest struct offsets, and
 * native/assets/models/FORMAT.md §6 for the end-to-end chain with
 * addresses.
 *
 * Each function is annotated with the decompiled original it reauthors.
 * The branch structure, loop bounds, null gates and bit arithmetic are
 * transcribed from analysis/decomp/ (read-only); only the raw offsets,
 * raw pointers, EE/GS packing and Ghidra names are replaced with named
 * structs and portable C.
 * ===================================================================== */
#include "material.h"
#include <stddef.h>

/* ---------------------------------------------------------------------
 * FUN_002e62b0(.., model_ctx, draw_word, ..)   [single-submesh path]
 *
 *   lVar3 = context();  if (lVar3 == 0) return -1;
 *   model = *(context+0x18);
 *   if ((word & 0x200) == 0) {                 // single path
 *       group = (word>>10) & 0x7f;
 *       if (group < *(u16)(model+0x40) &&
 *           (grp = *(model+0x60)[group]) != 0) {
 *           if ((word & 0x1ff) < *(u16)(grp+6)) {
 *               entry = *(grp + (word&0x1ff)*4 + 0x20);
 *               if (entry == 0) return -3;
 *               return FUN_002e63d0(.., *(u16)(grp+8) [selector], entry, ..);
 *           }
 *           return -3;
 *       }
 *       return -2;
 *   } else { ... batched path -> FUN_002e8830 ... return -4 on fail }
 *
 * We take `model` directly (the context->model deref is the caller's job);
 * model == NULL maps to the lVar3 == 0 / -1 case. The FUN_002e63d0 tail
 * (the actual submesh draw emit) is below the seam -- we stop at having
 * selected the submesh and read the selector, which is exactly the bridge
 * this module provides.
 * ------------------------------------------------------------------- */
DocSelectStatus doc_material_select_group_submesh(const DocMaterialModel *model,
                                                  uint32_t draw_word,
                                                  DocGroupSelect *out)
{
    DocGroupSelect tmp;
    if (out == NULL)
        out = &tmp;

    out->group = doc_material_draw_word_group(draw_word);
    out->sub   = doc_material_draw_word_sub(draw_word);
    out->material_selector = 0;
    out->submesh = NULL;

    if (model == NULL) {                       /* lVar3 == 0 */
        out->status = DOC_SEL_NO_MODEL;
        return out->status;
    }

    if (draw_word & DOC_DRAW_BATCH_BIT) {      /* FUN_002e8830 batch path */
        out->status = DOC_SEL_BATCH;           /* below-seam list build   */
        return out->status;
    }

    /* group bounds-check: group < *(u16)(model+0x40) and group ptr != 0 */
    if (out->group >= model->group_count ||
        model->groups == NULL ||
        model->groups[out->group] == NULL) {
        out->status = DOC_SEL_GROUP_RANGE;
        return out->status;
    }

    DocMaterialGroup *grp = model->groups[out->group];

    /* submesh bounds-check: sub < *(u16)(grp+6) */
    if (out->sub >= grp->submesh_count ||
        grp->submeshes == NULL ||
        grp->submeshes[out->sub] == NULL) {
        out->status = DOC_SEL_SUB_RANGE;       /* entry == 0 or OOB -> -3  */
        return out->status;
    }

    out->material_selector = grp->material_selector;   /* *(u16)(grp+8) */
    out->submesh           = grp->submeshes[out->sub];
    out->status            = DOC_SEL_OK;
    return out->status;
}

/* ---------------------------------------------------------------------
 * FUN_002ebc48(.., context, matdesc, release)   [layer-tag decode]
 *
 *   lists = *(matdesc+0x24);                   // ptr to array of 12 lists
 *   for (i = 0; i <= 0xb; i++) {
 *       list  = lists[i];
 *       stage = (i == 0) ? *(u8)(matdesc+0xe) : i;
 *       if (stage == 0) stage = 3;
 *       if (list == 0 || list[0] == 0) continue;   // *puVar3 != 0 gate
 *       count = list[0];
 *       for (t = 1; t <= count; t++) {
 *           tag = list[t];
 *           if ((tag & 0xf000) == 0)        // mode 0: table slot
 *               acquire/release context+0x24[tag] @ stage;
 *           else if (tag>>12 == 1)          // mode 1: passthrough
 *               ;
 *           else if (tag>>12 == 8)          // mode 8: shared/default id
 *               acquire/release default(tag&0xff) @ stage;
 *           // else: ignored
 *       }
 *   }
 *
 * The acquire/release (the FUN_0030b020/afc8/af18/ae90 refcount on the
 * texture handle at the given stage) is a below-seam hook; here we emit the
 * DECODED binding {mode, raw_tag, index, stage} for each tag, which is the
 * information a native renderer needs. Decode order matches the guest.
 * ------------------------------------------------------------------- */
int doc_material_decode_layers(const DocMaterial *mat,
                               DocLayerBinding *out, int max)
{
    if (mat == NULL || out == NULL || max <= 0 || mat->layer_lists == NULL)
        return 0;

    int n = 0;
    for (int i = 0; i < DOC_MATERIAL_MAX_LAYER_LISTS && n < max; i++) {
        const uint16_t *list = mat->layer_lists[i];

        unsigned stage = (i == 0) ? mat->stage0 : (unsigned)i;
        if (stage == 0)
            stage = 3;

        if (list == NULL || list[0] == 0)      /* empty list is skipped */
            continue;

        uint16_t count = list[0];
        for (uint16_t t = 1; t <= count && n < max; t++) {
            uint16_t tag = list[t];
            DocLayerBinding *b = &out[n];
            b->raw_tag = tag;
            b->stage   = stage;

            if ((tag & 0xf000u) == 0) {               /* mode 0 */
                b->mode  = DOC_LAYER_MODE_TABLE;
                b->index = tag;                        /* low 12 bits == tag */
            } else if ((tag >> 12) == 1) {            /* mode 1 */
                b->mode  = DOC_LAYER_MODE_KEEP;
                b->index = (uint16_t)(tag & 0x0fffu);
            } else if ((tag >> 12) == 8) {            /* mode 8 */
                b->mode  = DOC_LAYER_MODE_SHARED;
                b->index = (uint16_t)(tag & 0x00ffu);
            } else {                                   /* any other nibble */
                b->mode  = DOC_LAYER_MODE_OTHER;
                b->index = tag;
            }
            n++;
        }
    }
    return n;
}

/* ---------------------------------------------------------------------
 * FUN_002ebb18(.., context, tag)   [tag -> texture handle]
 *
 *   if (tag < 0) return default_texture(0x4ec8d0, 0);     // FUN_0030ae00
 *   mode = (tag & 0xf000) >> 12;
 *   if (mode != 8) {                     // (mode==1 || mode!=8) == (mode!=8)
 *       slot   = tag & 0xffff0fff;       // clear the mode nibble
 *       handle = *(context + 0x24 + slot*4);
 *       if (*(handle+0x54) != 0) handle = *(handle+0x54);   // override
 *       return handle;
 *   }
 *   return default_texture(0x4ec8d0);                     // FUN_0030ae00
 *
 * Note: the guest's `(mode == 1) || (mode != 8)` is a redundant spelling of
 * `mode != 8` (if mode==8 both disjuncts are false); transcribed as the
 * simplified, behaviourally-identical test. The default-texture lookup
 * walks a global texture-manager list -- below the seam; surfaced as a NULL
 * handle + DOC_TEX_SLOT_DEFAULT. The guest does not range-check the slot;
 * `handle_count` only converts an out-of-range slot to the default rather
 * than an OOB read (native safety; in-range behaviour is identical).
 * ------------------------------------------------------------------- */
DocTextureHandle *doc_texture_table_resolve(const DocTextureContext *ctx,
                                            int32_t tag, int32_t *out_slot)
{
    if (ctx == NULL || tag < 0) {              /* tag < 0 -> default */
        if (out_slot) *out_slot = DOC_TEX_SLOT_DEFAULT;
        return NULL;
    }

    int mode = (int)(((uint32_t)tag & 0xf000u) >> 12);
    if (mode == 8) {                           /* mode 8 -> shared/default */
        if (out_slot) *out_slot = DOC_TEX_SLOT_DEFAULT;
        return NULL;
    }

    int32_t slot = (int32_t)((uint32_t)tag & 0xffff0fffu);   /* clear nibble */

    if (ctx->handles == NULL ||
        (ctx->handle_count > 0 && slot >= ctx->handle_count)) {
        if (out_slot) *out_slot = DOC_TEX_SLOT_DEFAULT;      /* native guard */
        return NULL;
    }

    DocTextureHandle *handle = ctx->handles[slot];
    if (handle != NULL && handle->override != NULL)          /* +0x54 override */
        handle = handle->override;

    if (out_slot) *out_slot = slot;
    return handle;
}

/* ---------------------------------------------------------------------
 * FUN_002f2a88(dim): ceil-log2.
 *   n = 0; p = 1; while (p < dim) { p <<= 1; n++; }  return n;
 * (The guest guards `1 < dim` first; for dim <= 1 the result is 0.)
 * ------------------------------------------------------------------- */
int doc_texture_log2_dim(int dim)
{
    int n = 0;
    int p = 1;
    if (dim > 1) {
        do {
            p <<= 1;
            n++;
        } while (p < dim);
    }
    return n;
}

/* ---------------------------------------------------------------------
 * FUN_0030ab10(.., handle, tbp0)   [REAUTHORED: handle -> texture identity]
 *
 * The original packs a GS TEX0 register and emits two DMA uploads (texels,
 * then CLUT) via FUN_0030a7f8, allocating a transient VRAM CLUT page after
 * the texels. The register it returns maps as:
 *
 *   TEX0 bits          <- source                     captured as
 *   ----------------------------------------------------------------
 *   [0..13]   TBP0     <- tbp0 (transient VRAM page)  (NOT captured: transient)
 *   [14..19]  TBW      <- *(u8)(handle+0x32)          desc.tbw
 *   [20..25]  PSM      <- *(u8)(handle+0x33)          desc.psm
 *   [26..29]  TW       <- ceil-log2(*(u16)(h+0x3c))   log2_dim(desc.width)
 *   [30..33]  TH       <- ceil-log2(*(u16)(h+0x3e))   log2_dim(desc.height)
 *   [34]      TCC = 1  <- literal 0x400000000         (constant)
 *   [37..50]  CBP      <- tbp0 + VRAM pages (transient) (NOT captured)
 *   [51..54]  CPSM     <- *(u8)(handle+0x47)          desc.cpsm
 *   [62]      CLD = 1  <- literal 0x4000000000000000  (constant)
 *   texel EE src       <- *(u32)(handle+0x2c)         desc.has_texels
 *   CLUT  EE src       <- *(u32)(handle+0x40)         desc.has_clut
 *
 * Per FORMAT.md §6, TBP0/CBP are transient GS-VRAM pages assigned at bind
 * time -- not a stable identity -- so they stay below the seam. The stable
 * identity is the context+0x24 `slot` plus the sampler fields above.
 * ------------------------------------------------------------------- */
DocTextureDesc doc_texture_describe(const DocTextureHandle *h, int32_t slot)
{
    DocTextureDesc d;
    d.slot       = slot;
    if (h == NULL) {
        d.slot       = DOC_TEX_SLOT_DEFAULT;
        d.width      = 0;
        d.height     = 0;
        d.psm        = 0;
        d.tbw        = 0;
        d.cpsm       = 0;
        d.has_clut   = 0;
        d.has_texels = 0;
        return d;
    }
    d.width      = h->width;           /* <- handle+0x3c */
    d.height     = h->height;          /* <- handle+0x3e */
    d.psm        = h->psm;             /* <- handle+0x33 */
    d.tbw        = h->tbw;             /* <- handle+0x32 */
    d.cpsm       = h->cpsm;            /* <- handle+0x47 */
    d.has_clut   = (h->clut != NULL);  /* <- handle+0x40 */
    d.has_texels = (h->texels != NULL);/* <- handle+0x2c */
    return d;
}

/* ---------------------------------------------------------------------
 * Convenience bridge: draw word -> group/submesh -> material selector ->
 * first table-mode layer tag -> texture identity. This is the one-call
 * path a renderer uses; each hop is the reauthored function above.
 * ------------------------------------------------------------------- */
DocSelectStatus doc_material_resolve_texture(const DocMaterialModel *model,
                                             const DocTextureContext *ctx,
                                             uint32_t draw_word,
                                             DocTextureDesc *out_desc)
{
    DocGroupSelect sel;
    DocSelectStatus st = doc_material_select_group_submesh(model, draw_word, &sel);
    if (out_desc == NULL)
        return st;

    /* default descriptor unless we resolve a real texture */
    *out_desc = doc_texture_describe(NULL, DOC_TEX_SLOT_DEFAULT);
    if (st != DOC_SEL_OK)
        return st;

    /* selector -> material descriptor (model+0x64 table) */
    if (model->materials == NULL)
        return st;
    const DocMaterial *mat = model->materials[sel.material_selector];
    if (mat == NULL)
        return st;

    /* first table-mode layer -> handle -> identity */
    DocLayerBinding layers[DOC_MATERIAL_MAX_LAYER_LISTS * 16];
    int nlay = doc_material_decode_layers(mat, layers,
                                          (int)(sizeof layers / sizeof layers[0]));
    for (int i = 0; i < nlay; i++) {
        if (layers[i].mode != DOC_LAYER_MODE_TABLE)
            continue;
        int32_t slot = DOC_TEX_SLOT_DEFAULT;
        DocTextureHandle *h =
            doc_texture_table_resolve(ctx, (int32_t)layers[i].raw_tag, &slot);
        *out_desc = doc_texture_describe(h, slot);
        break;
    }
    return st;
}
