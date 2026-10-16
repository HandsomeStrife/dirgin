/* =====================================================================
 * object_list.h  --  Reauthored scene-object list traversal.
 *
 * SECOND reauthored subsystem of the purist port (P4). This is the layer
 * that enumerates scene objects into the per-object draw entry already
 * ported in scene_graph.{h,c} (engine-map.md §4: "Scene graph ABOVE the
 * seam ... object list FUN_00408758 -> per-object draw FUN_005a85f8 ...").
 * It also holds the render-context node-list walkers that select model
 * nodes by kind (and by a binding id) and reflag their subtrees.
 *
 * Reauthored from (analysis/decomp/, read-only oracle):
 *   FUN_00408758  scene-object list traversal -> doc_scene_object_list_*
 *   FUN_002ebdb8  ctx node-list walk (by kind) -> ..._flag_nodes_by_kind
 *   FUN_002ebe38  same + flag/id filters        -> ..._flag_nodes_by_kind_id
 *   FUN_005a6d70  tiny pass-flag setter         -> doc_scene_set_pass_flag
 * Already ported and reused: FUN_005a85f8 (doc_scene_object_set_draw_mode),
 * FUN_002ebee8 (doc_model_node_set_queue_flag), FUN_002dbfc8 (flag_main).
 *
 * The DAT_ globals the traversal reads become named module state here,
 * exposed through setters so the not-yet-ported subsystems that feed them
 * (object spawner, visibility pass) can wire in. Guest struct offsets are
 * recorded against every field; a byte-accurate transcription is diffed
 * against these clean structs in tests/test_object_list.c.
 * ===================================================================== */
#ifndef DOC_OBJECT_LIST_H
#define DOC_OBJECT_LIST_H

#include <stdint.h>
#include "scene_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- DocObjectDrawInfo flag bits (the u32 at draw_info +0x00) --------- *
 * FUN_00408758 tests the whole word; bit 0x200 gates visibility, bit
 * 0x100 selects whether the object's render_state bit 0 is set, and byte
 * +0x02 (bits 16..23, little-endian) carries the draw mode. */
#define DOC_OBJ_VISIBLE      0x0200u   /* bit 9:  object is drawn this pass */
#define DOC_OBJ_LIST_ACTIVE  0x0100u   /* bit 8:  => set render_state bit 0 */

/* ---- DocModelNode.node_flags bit (the u16 at node +0x02) -------------- *
 * FUN_002ebe38 excludes a node from the id-filtered walk when set. */
#define DOC_NODE_FLAG_SKIP   0x0100u

/* The visibility / draw descriptor hung off a scene object (object +0x42c).
 * Guest layout (param read as uint* in FUN_00408758):
 *   +0x00  uint32 flags   (bit 0x200 visible, bit 0x100 list-active,
 *                          byte @ +0x02 == bits 16..23 == draw mode) */
typedef struct DocObjectDrawInfo {
    uint32_t flags;   /* <- u32 @ +0x00 */
} DocObjectDrawInfo;

/* Draw mode = byte @ +0x02 of the flags word (little-endian bits 16..23).
 * FUN_005a85f8 only keeps its low 4 bits, but the traversal passes the
 * whole byte, so we preserve it. */
static inline unsigned doc_object_draw_info_mode(const DocObjectDrawInfo *info)
{
    return (info->flags >> 16) & 0xFFu;
}

/* The object a model node is bound to. The id-filtered walker FUN_002ebe38
 * selects nodes by a single byte at +0x2e of this block. Its full meaning
 * (part / bone / material id) is not yet reversed; modelled as an opaque
 * id carrier so the walker's logic is complete and honest. */
typedef struct DocNodeBinding {
    uint8_t id;   /* <- u8 @ +0x2e */
} DocNodeBinding;

/* ---- reauthored API -------------------------------------------------- */

/* FUN_00408758 core: walk a scene-object list (linked via DocSceneObject
 * .next, guest +0x00). For each object that has a draw_info whose
 * DOC_OBJ_VISIBLE bit is set, store its draw mode via
 * doc_scene_object_set_draw_mode and set/clear the object's render_state
 * bit 0 from the DOC_OBJ_LIST_ACTIVE bit. Objects with no draw_info, or
 * not visible, are skipped. Taking the head explicitly makes the walk
 * testable; doc_scene_object_list_update supplies the module head. */
void doc_scene_object_list_traverse(DocSceneObject *head);

/* FUN_00408758 guest entry (void->void): if the traversal is enabled,
 * publish the pass-source flag via doc_scene_set_pass_flag, then walk the
 * module's object-list head. */
void doc_scene_object_list_update(void);

/* FUN_002ebdb8: walk a render context's registered-node list (q->node_list,
 * guest +0x84; nodes linked by DocModelNode.next, +0x14) and, for every
 * node whose select_kind (+0x54) equals `kind`, flag its subtree with
 * doc_model_node_set_queue_flag(q, node, clear, queue_bit). */
void doc_render_queue_flag_nodes_by_kind(DocRenderQueue *q, int32_t kind,
                                         int clear, unsigned queue_bit);

/* FUN_002ebe38: same walk as above with two extra filters before flagging
 * a matching-kind node: the node's DOC_NODE_FLAG_SKIP bit (+0x02) must be
 * clear, and the node's binding id (binding->id, +0x78 then +0x2e) must
 * equal `match_id`. */
void doc_render_queue_flag_nodes_by_kind_id(DocRenderQueue *q, int32_t kind,
                                            uint8_t match_id, int clear,
                                            unsigned queue_bit);

/* FUN_005a6d70: publish the per-pass flag (DAT_005b37ac). The subsystem
 * that consumes it is below the current seam / not yet ported, so this is
 * a faithful store kept as readable module state. */
void doc_scene_set_pass_flag(int flag);
int  doc_scene_pass_flag(void);

/* ---- module-state wiring (replaces the DAT_ globals) ----------------- */
void            doc_scene_object_list_set_enabled(int enabled);      /* DAT_004fae00 */
int             doc_scene_object_list_enabled(void);
void            doc_scene_object_list_set_head(DocSceneObject *head); /* DAT_005b3788 */
DocSceneObject *doc_scene_object_list_head(void);
void            doc_scene_set_pass_source_flag(int flag);            /* DAT_004fb3d8 */
int             doc_scene_pass_source_flag(void);

#ifdef __cplusplus
}
#endif
#endif /* DOC_OBJECT_LIST_H */
