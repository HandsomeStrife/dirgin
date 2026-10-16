/* =====================================================================
 * material.h  --  Reauthored material -> texture binding subsystem.
 *
 * THIRD reauthored subsystem of the purist port (P4). This is the bridge
 * from the already-ported scene graph (scene_graph.{h,c} / object_list)
 * to the finished native texture decoder (native/assets, textures/): it
 * turns a draw-request word into a group/submesh, reads the group's
 * material selector, decodes the material's layer tags, and resolves each
 * tag to a stable texture identity.
 *
 * The full binding chain is documented with addresses in
 * native/assets/models/FORMAT.md §6; this module reauthors it.
 *
 * Reauthored from (analysis/decomp/, read-only oracle):
 *   FUN_002e62b0  draw-word -> group/submesh select -> doc_material_select_group_submesh
 *   FUN_002e8830  (batch sibling: identical group/sub decode, batched list) -> doc_material_decode_draw_word
 *   FUN_002ebc48  material layer-tag decode (matdesc+0x24)   -> doc_material_decode_layers
 *   FUN_002ebb18  tag -> per-model texture-handle table      -> doc_texture_table_resolve
 *   FUN_0030ab10  handle -> GS TEX0 pack  (REAUTHORED AS     -> doc_texture_describe
 *                 "resolve to a texture identity/descriptor", NOT GS
 *                 register packing; the GS field->offset mapping is kept
 *                 verbatim in comments as evidence.)
 * Helper: FUN_002f2a88 (ceil-log2) -> doc_texture_log2_dim (GS TW/TH).
 *
 * Every struct field records the guest offset it maps, so the mapping is
 * auditable and a byte-accurate transcription can be diffed against these
 * clean structs (see tests/test_material.c).
 *
 * BELOW THE SEAM (documented hooks, not implemented here):
 *   - GS VRAM allocation of TBP0/CBP pages and the DMA/GIF texel+CLUT
 *     upload (FUN_0030a7f8). TBP0 is a TRANSIENT GS-VRAM page assigned at
 *     bind time -- NOT a stable identity and NOT a FileCode (FORMAT.md §6).
 *     The stable identity is the context+0x24 slot, captured here.
 *   - The default/shared texture lookup (FUN_0030ae00 walks a global
 *     texture-manager list); surfaced as a DEFAULT sentinel + NULL handle.
 *   - Per-layer texture reference counting (the acquire/release half of
 *     FUN_002ebc48, FUN_0030b020/afc8/af18/ae90): the DECODE is reauthored,
 *     the refcount side-effect is a hook.
 * ===================================================================== */
#ifndef DOC_MATERIAL_H
#define DOC_MATERIAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- draw-request word bitfields (FUN_002e62b0 / FUN_002e8830) ------- *
 * A draw-request word decodes to a group index and a submesh index:
 *   group = (word >> 10) & 0x7f        // FUN_002e62b0 line "param_3>>10&0x7f"
 *   sub   =  word        & 0x1ff       // FUN_002e62b0 line "param_3 & 0x1ff"
 *   bit 0x200 selects the BATCHED path (FUN_002e8830) over the single one. */
#define DOC_DRAW_GROUP_SHIFT   10
#define DOC_DRAW_GROUP_MASK    0x7fu
#define DOC_DRAW_SUB_MASK      0x1ffu
#define DOC_DRAW_BATCH_BIT     0x200u

/* Selection status codes, mirroring FUN_002e62b0's negative returns. */
typedef enum {
    DOC_SEL_OK            =  0,   /* resolved group+submesh                 */
    DOC_SEL_NO_MODEL      = -1,   /* no model/context (lVar3 == 0)          */
    DOC_SEL_GROUP_RANGE   = -2,   /* group >= group_count or null group     */
    DOC_SEL_SUB_RANGE     = -3,   /* sub   >= submesh_count or null entry   */
    DOC_SEL_BATCH         = -4    /* batch path taken (bit 0x200) -- hook   */
} DocSelectStatus;

/* ---- layer-tag modes (high nibble of each u16 tag, FUN_002ebc48) ----- */
typedef enum {
    DOC_LAYER_MODE_TABLE  = 0,   /* high nibble 0: low 12 bits index tex table */
    DOC_LAYER_MODE_KEEP   = 1,   /* high nibble 1: passthrough, no acquire     */
    DOC_LAYER_MODE_SHARED = 8,   /* high nibble 8: tag & 0xff = shared/def id   */
    DOC_LAYER_MODE_OTHER  = -1   /* any other nibble: ignored by the decoder    */
} DocLayerMode;

/* Default/invalid texture slot sentinel. FUN_002ebb18 resolves a negative
 * tag, or a mode-8 tag, to the global default texture (FUN_0030ae00) --
 * below the seam; we surface it as this slot with a NULL handle. */
#define DOC_TEX_SLOT_DEFAULT  (-1)

/* ---------------------------------------------------------------------
 * Opaque below-seam leaf: the actual submesh geometry / VIF payload the
 * draw path emits (FUN_002e63d0). Referenced by pointer only -- geometry
 * is a separate subsystem (P1, native/assets/models). */
typedef struct DocSubmesh DocSubmesh;

/* A material group in the model's group table.
 * Guest layout (group entry, indexed out of *(model+0x60)):
 *   +0x06  u16   submesh_count      (FUN_002e62b0 bounds-checks sub here)
 *   +0x08  u16   material_selector  (*(u16)(group+8); indexes model+0x64)
 *   +0x20  u32   submeshes[]        (array of submesh-entry pointers)      */
typedef struct DocMaterialGroup {
    uint16_t     submesh_count;        /* <- u16 @ +0x06 */
    uint16_t     material_selector;    /* <- u16 @ +0x08 */
    DocSubmesh **submeshes;            /* <- ptr array @ +0x20 */
} DocMaterialGroup;

/* A material descriptor.
 * Guest layout (matdesc, indexed out of *(model+0x64) by the selector;
 * walked by FUN_002ebc48):
 *   +0x0e  u8    stage0        (layer-list 0's GS texture stage)
 *   +0x24  u16** layer_lists   (pointer to an array of up to 12 tag-list
 *                               pointers; each list is a u16 array whose
 *                               element [0] is a tag count, [1..count] are
 *                               the layer tags).
 * We model stage0 and the list-of-lists; the per-list u16[0]=count /
 * u16[1..] = tags convention is read directly as the guest does. */
typedef struct DocMaterial {
    uint8_t                stage0;        /* <- u8  @ +0x0e */
    const uint16_t *const *layer_lists;   /* <- ptr @ +0x24 -> array[12] of list ptrs */
} DocMaterial;

#define DOC_MATERIAL_MAX_LAYER_LISTS 12   /* FUN_002ebc48: list idx guard "0xb < uVar8" */

/* One decoded layer binding produced by doc_material_decode_layers. */
typedef struct DocLayerBinding {
    DocLayerMode mode;       /* decoded from the tag's high nibble          */
    uint16_t     raw_tag;    /* the original u16 tag                        */
    uint16_t     index;      /* table slot (mode 0) or shared id (mode 8)   */
    unsigned     stage;      /* GS texture stage (uVar5): list idx or
                              * matdesc+0x0e for list 0, defaulting to 3    */
} DocLayerBinding;

/* The model's material-side tables.
 * Guest layout (model = *(context+0x18); FUN_002e62b0 / FUN_002e63d0):
 *   +0x40  u16   group_count      (FUN_002e62b0 bounds-checks group here)
 *   +0x60  u32   groups[]         (array of group pointers, by group idx)
 *   +0x64  u32   materials[]      (array of matdesc pointers, by selector;
 *                                  read by FUN_002e63d0 at *(model+0x64)) */
typedef struct DocMaterialModel {
    uint16_t            group_count;   /* <- u16 @ +0x40 */
    DocMaterialGroup  **groups;        /* <- ptr array @ +0x60 */
    DocMaterial       **materials;     /* <- ptr array @ +0x64 */
} DocMaterialModel;

/* The per-model texture-handle table context.
 * Guest layout (context, param_2 of FUN_002ebb18 / FUN_002ebc48):
 *   +0x24  u32   handles[]   (array of texture-handle pointers, by slot)
 * `handle_count` is a NATIVE-ONLY bound (the guest trusts the tag and does
 * not range-check the slot); it only turns an out-of-range slot into the
 * default texture instead of an OOB read -- in-range behaviour is byte
 * identical. */
typedef struct DocTextureContext {
    struct DocTextureHandle **handles;   /* <- ptr array @ +0x24 */
    int                       handle_count;
} DocTextureContext;

/* A per-model texture handle (the entry stored in the context+0x24 table).
 * Guest layout (handle, param_2 of FUN_0030ab10; the GS TEX0 packer):
 *   +0x2c  u32   texels     (EE source pointer to the texel data)
 *   +0x32  u8    tbw        (GS TEX0.TBW -- texture buffer width)
 *   +0x33  u8    psm        (GS TEX0.PSM -- pixel storage mode; also
 *                            indexes the DAT_0046db40 bytes table for the
 *                            VRAM page count used to place the CLUT)
 *   +0x34  u16   trx_x      (texel transfer region, FUN_0030a7f8 param)
 *   +0x36  u16   trx_y
 *   +0x38  u16   trx_wh
 *   +0x3c  u16   width      (GS TEX0.TW = ceil-log2 of this)
 *   +0x3e  u16   height     (GS TEX0.TH = ceil-log2 of this)
 *   +0x40  u32   clut       (EE source pointer to the CLUT; 0 if none)
 *   +0x46  u8    cbp_hint   (CLUT transfer param, FUN_0030a7f8)
 *   +0x47  u8    cpsm       (GS TEX0.CPSM -- CLUT pixel format)
 *   +0x48  u16   clut_x     (CLUT transfer region)
 *   +0x4a  u16   clut_y
 *   +0x4c  u16   clut_wh
 *   +0x54  u32   override   (if non-zero, resolve returns this handle
 *                            instead; FUN_002ebb18 / FUN_0030b020 '+0x54') */
typedef struct DocTextureHandle {
    const void              *texels;    /* <- ptr @ +0x2c */
    uint8_t                  tbw;       /* <- u8  @ +0x32 */
    uint8_t                  psm;       /* <- u8  @ +0x33 */
    uint16_t                 trx_x;     /* <- u16 @ +0x34 */
    uint16_t                 trx_y;     /* <- u16 @ +0x36 */
    uint16_t                 trx_wh;    /* <- u16 @ +0x38 */
    uint16_t                 width;     /* <- u16 @ +0x3c */
    uint16_t                 height;    /* <- u16 @ +0x3e */
    const void              *clut;      /* <- ptr @ +0x40 */
    uint8_t                  cbp_hint;  /* <- u8  @ +0x46 */
    uint8_t                  cpsm;      /* <- u8  @ +0x47 */
    uint16_t                 clut_x;    /* <- u16 @ +0x48 */
    uint16_t                 clut_y;    /* <- u16 @ +0x4a */
    uint16_t                 clut_wh;   /* <- u16 @ +0x4c */
    struct DocTextureHandle *override;  /* <- ptr @ +0x54 */
} DocTextureHandle;

/* The stable texture IDENTITY a submesh binds to -- what FUN_0030ab10
 * would have packed into GS TEX0, captured as data rather than a register.
 * This is the native equivalent of a GS TEX0 bind: the renderer uses `slot`
 * to find the TXD/tim2 entry loaded into that per-model slot, and the rest
 * to describe the sampler. TBP0/CBP (the transient VRAM pages) are NOT
 * here -- they are allocated below the seam at upload time. */
typedef struct DocTextureDesc {
    int32_t  slot;        /* stable per-model slot (context+0x24 index);
                           * DOC_TEX_SLOT_DEFAULT for the default texture  */
    uint16_t width;       /* <- handle+0x3c (GS TEX0.TW = log2)            */
    uint16_t height;      /* <- handle+0x3e (GS TEX0.TH = log2)            */
    uint8_t  psm;         /* <- handle+0x33 (GS TEX0.PSM)                  */
    uint8_t  tbw;         /* <- handle+0x32 (GS TEX0.TBW)                  */
    uint8_t  cpsm;        /* <- handle+0x47 (GS TEX0.CPSM)                 */
    int      has_clut;    /* handle+0x40 (CLUT src) != 0                   */
    int      has_texels;  /* handle+0x2c (texel src) != 0                  */
} DocTextureDesc;

/* Result of a single-submesh selection. */
typedef struct DocGroupSelect {
    DocSelectStatus status;
    uint32_t        group;             /* (word >> 10) & 0x7f               */
    uint32_t        sub;               /* word & 0x1ff                      */
    uint16_t        material_selector; /* *(u16)(group+8)                   */
    DocSubmesh     *submesh;           /* group->submeshes[sub]             */
} DocGroupSelect;

/* ---- reauthored API -------------------------------------------------- */

/* Decode just the group/submesh indices from a draw-request word. Shared
 * by the single path (FUN_002e62b0) and each element of the batch path
 * (FUN_002e8830, which applies the identical `(elem>>10)&0x7f` /
 * `elem & 0x1ff` decode per list entry). */
static inline uint32_t doc_material_draw_word_group(uint32_t word)
{
    return (word >> DOC_DRAW_GROUP_SHIFT) & DOC_DRAW_GROUP_MASK;
}
static inline uint32_t doc_material_draw_word_sub(uint32_t word)
{
    return word & DOC_DRAW_SUB_MASK;
}

/* FUN_002e62b0: decode a draw-request word to a group+submesh and read the
 * group's material selector. Fills *out and returns out->status.
 * The batch path (word & 0x200) is a documented hook: it returns
 * DOC_SEL_BATCH (the batched list build/emit in FUN_002e8830 is below the
 * seam; the per-element group/sub decode it uses is the same as here). */
DocSelectStatus doc_material_select_group_submesh(const DocMaterialModel *model,
                                                  uint32_t draw_word,
                                                  DocGroupSelect *out);

/* FUN_002ebc48: decode a material's layer-tag lists into up to `max`
 * bindings. Returns the number of bindings written (the acquire/release
 * reference-count side effect is a below-seam hook; only the decode --
 * which tag binds which slot/id at which stage -- is reauthored here). */
int doc_material_decode_layers(const DocMaterial *mat,
                               DocLayerBinding *out, int max);

/* FUN_002ebb18: resolve one layer tag to a texture handle via the per-model
 * context+0x24 table, honouring the handle's +0x54 override. A negative
 * tag, or a mode-8 (shared) tag, resolves to the default texture: returns
 * NULL and sets *out_slot = DOC_TEX_SLOT_DEFAULT. Otherwise returns the
 * handle and sets *out_slot to the resolved table slot. out_slot may be
 * NULL. */
DocTextureHandle *doc_texture_table_resolve(const DocTextureContext *ctx,
                                            int32_t tag, int32_t *out_slot);

/* FUN_0030ab10 (reauthored): describe a resolved handle as a stable texture
 * identity instead of packing a GS TEX0 register. `slot` is the stable
 * table slot from doc_texture_table_resolve. A NULL handle yields the
 * default descriptor (slot = DOC_TEX_SLOT_DEFAULT, zeroed identity). */
DocTextureDesc doc_texture_describe(const DocTextureHandle *h, int32_t slot);

/* Convenience: select -> material -> first table-mode layer -> describe.
 * Walks the whole chain for a single draw word and returns the texture
 * identity its first resolvable (mode-0/table) layer binds to. Returns the
 * selection status; on DOC_SEL_OK, *out_desc is filled (default descriptor
 * if the material has no table-mode layer). */
DocSelectStatus doc_material_resolve_texture(const DocMaterialModel *model,
                                             const DocTextureContext *ctx,
                                             uint32_t draw_word,
                                             DocTextureDesc *out_desc);

/* FUN_002f2a88 (reauthored): ceil-log2 of a GS texture dimension, i.e. the
 * number of left shifts of 1 needed to reach or exceed `dim` (the GS TW/TH
 * field). doc_texture_describe stores raw width/height; this derives the
 * GS log2 field from them. */
int doc_texture_log2_dim(int dim);

#ifdef __cplusplus
}
#endif
#endif /* DOC_MATERIAL_H */
