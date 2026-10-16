/* =====================================================================
 * material_loader.h  --  Build a DocMaterialModel + DocTextureContext for a
 * real z143 room chunk from a baked binding MANIFEST.
 *
 * material.{c,h} reauthored the runtime material->texture CONSUMER chain but
 * is otherwise fed only synthetic fixtures. This loader closes that gap: it
 * SYNTHESIZES a faithful model that reproduces the empirically-recovered
 * per-packet FileCode binding (FORMAT.md §6: the real runtime model is not
 * stored in the BMD, so it is reconstructed from the recovered binding, not
 * parsed). The fragile VIF packet enumeration + the binding live in the
 * Python decoder that owns them (native/assets/models/export_bmd_textured.py);
 * emit_loader_manifest.py bakes a stable decode-order manifest this C reads.
 *
 * The built model is exactly what material.c walks:
 *   - one DocMaterialGroup per bound FileCode (group g, selector g); its
 *     DocMaterial has a single mode-0 layer whose tag == the texture slot g,
 *     so doc_material_resolve_texture(..) yields desc.slot == g.
 *   - one trailing "default" group (index = nslots) for the unbound packets;
 *     its material has a single mode-8 layer, so resolve yields the default
 *     descriptor (slot DOC_TEX_SLOT_DEFAULT) -- an honest untextured surface.
 *   - a dense DocTextureContext.handles[slot], one non-null handle per slot.
 *   - a per-packet draw_word (decode order) that selects the right group/sub.
 *
 * Milestone 1 carries texture IDENTITY by FileCode only; the GS sampler
 * fields (w/h/psm from the TXD via libdtex) are a later milestone -- the
 * handles are present and non-null so the resolve chain is exercised end to
 * end. Addressing limits (group < 128, submesh < 512) are asserted; a
 * FileCode with >511 packets in one chunk needs sharding (milestone 2).
 * ===================================================================== */
#ifndef DOC_MATERIAL_LOADER_H
#define DOC_MATERIAL_LOADER_H

#include "material.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DocRoomBinding {
    DocMaterialModel   model;          /* pass to material.c as-is            */
    DocTextureContext  ctx;

    int   nslots;                      /* distinct bound FileCodes            */
    int   npackets;                    /* decode-order packet count           */
    int   unbound_count;               /* packets with no binding (slot -1)   */
    char  chunk[9];

    /* public: slot -> FileCode, expected per-slot packet count, per-packet
     * draw word + expected slot (the baked ground truth the test asserts). */
    char      (*slot_filecode)[9];     /* [nslots]  "068fffNN"+NUL            */
    int        *slot_count;            /* [nslots]  expected resolved count   */
    uint32_t   *draw_words;            /* [npackets] decode order             */
    int32_t    *expect_slot;           /* [npackets] slot, or -1 = unbound    */

    /* owned backing storage (released by doc_room_binding_free) */
    DocMaterialGroup  *groups_store;   DocMaterialGroup **group_ptrs;
    DocMaterial       *mats_store;     DocMaterial      **mat_ptrs;
    DocTextureHandle  *handles_store;  DocTextureHandle **handle_ptrs;
    DocSubmesh       **submesh_pool;
    uint16_t          *lists_backing;  const uint16_t   **list_ptr_store;
} DocRoomBinding;

/* Build a DocRoomBinding from a manifest file (emit_loader_manifest.py output).
 * Returns 0 on success; on failure returns non-zero and writes a message into
 * err (if err != NULL). On success the caller must doc_room_binding_free. */
int  doc_room_binding_load(const char *manifest_path, DocRoomBinding *out,
                           char *err, size_t errlen);

void doc_room_binding_free(DocRoomBinding *b);

/* The FileCode a resolved desc.slot maps to; NULL for the default/unbound
 * slot or an out-of-range slot. */
const char *doc_room_binding_slot_filecode(const DocRoomBinding *b, int32_t slot);

#ifdef __cplusplus
}
#endif
#endif /* DOC_MATERIAL_LOADER_H */
