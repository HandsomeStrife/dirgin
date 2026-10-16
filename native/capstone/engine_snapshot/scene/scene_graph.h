/* =====================================================================
 * scene_graph.h  --  Reauthored scene-graph / model-node cluster.
 *
 * FIRST reauthored subsystem of the purist port (P4). This is the clean
 * C surface for the object/model tree traversal and draw-queue flagging
 * that the EE engine does above the hardware seam (engine-map.md §4:
 * "Scene graph ABOVE the seam ... FUN_005a85f8 -> FUN_002dbfc8 ->
 * FUN_002ebee8 ... render-queue flagging into contexts 0x4ee2e0 family").
 *
 * Reauthored from (analysis/decomp/, read-only oracle):
 *   FUN_005a85f8  per-object draw entry  -> doc_scene_object_set_draw_mode
 *   FUN_002dbfc8  model-parts fan-out    -> doc_model_parts_set_queue_flag
 *   FUN_002ebee8  model-node tree walk   -> doc_model_node_set_queue_flag
 * Sibling walkers confirming the layout: FUN_002dc158 (same ModelParts,
 * queue bit 10 instead of 4), FUN_00408900 (same SceneObject +0x3e4).
 *
 * The raw guest struct offsets are recorded against each field so the
 * mapping is auditable and so a byte-accurate reference can be diffed
 * against these clean structs (see tests/test_scene_graph.c).
 * ===================================================================== */
#ifndef DOC_SCENE_GRAPH_H
#define DOC_SCENE_GRAPH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Defined by the object-list traversal module (object_list.h), referenced
 * here only by pointer so the two struct families stay one module. */
struct DocObjectDrawInfo;
struct DocNodeBinding;

/* Max root nodes a model-parts block fans out to. In the original the
 * array at ModelParts+0x1c is a fixed 8 slots, null-terminated. */
#define DOC_MODEL_MAX_ROOTS 8

/* The group-node tag byte. In the guest, an internal "list" node carries
 * the ASCII byte 'l' at offset 0; anything else is a drawable leaf. */
#define DOC_NODE_TAG_GROUP 'l'

typedef enum {
    DOC_NODE_LEAF  = 0,   /* drawable node: carries per-queue draw flags  */
    DOC_NODE_GROUP = 1    /* internal 'l' node: recurses into children    */
} DocNodeKind;

/* Leaf geometry payload. STUB: real mesh/material binding is a later
 * subsystem (P1/P2 geometry + material->texture). Kept as an opaque
 * leaf attachment so the tree walk is complete and the struct is real. */
typedef struct DocMesh {
    const char *name;
    int         submesh_count;   /* informational for now */
} DocMesh;

/* A node in the model-node tree.
 * Guest layout (ModelNode, param_2 of FUN_002ebee8):
 *   +0x00  char   tag            ('l' => group; else leaf)
 *   +0x04  uint16 child_count    (group only)
 *   +0x0a  uint16 queue_flags    (leaf: per-render-queue draw bits)
 *   +0x30  int32  children[]     (group: array of child node pointers)
 * We store kind explicitly (derived from the tag) and use a count+pointer
 * child list rather than a fixed guest array. */
typedef struct DocModelNode {
    DocNodeKind           kind;         /* <- tag byte @ +0x00            */
    uint16_t              queue_flags;  /* <- u16  @ +0x0a (leaf)         */
    uint16_t              child_count;  /* <- s16  @ +0x04 (group)        */
    struct DocModelNode **children;     /* <- ptr array @ +0x30 (group)   */
    DocMesh              *mesh;         /* leaf geometry (stub)           */
    const char           *name;         /* debug label (not in guest)     */

    /* ---- render-context registration list (object_list.c walkers) ----
     * The same node is also an entry in a render queue's flat list of
     * registered nodes (DocRenderQueue.node_list), walked by the
     * FUN_002ebdb8 / FUN_002ebe38 family. These fields are distinct from
     * the tree-child linkage above (+0x30). */
    uint16_t               node_flags;   /* <- u16 @ +0x02 (bit 0x100 = skip) */
    struct DocModelNode   *next;         /* <- ptr @ +0x14 (ctx list next)    */
    int32_t                select_kind;  /* <- s32 @ +0x54 (walker match key) */
    struct DocNodeBinding *binding;      /* <- ptr @ +0x78 (id carrier @+0x2e)*/
} DocModelNode;

/* The drawable-parts block hung off a scene object.
 * Guest layout (ModelParts, param_1 of FUN_002dbfc8 / FUN_002dc158):
 *   +0x08  uint32 flags       (bit 0x80 = visible/enabled toggle)
 *   +0x14  int32  node_count  (0 => no nodes; guards the fan-out)
 *   +0x1c  int32  roots[8]    (null-terminated array of root node ptrs) */
typedef struct DocModelParts {
    uint32_t      flags;                        /* <- u32 @ +0x08         */
    int32_t       node_count;                   /* <- int @ +0x14         */
    DocModelNode *roots[DOC_MODEL_MAX_ROOTS];   /* <- ptr array @ +0x1c   */
} DocModelParts;

/* A scene object (entity). Only the fields this cluster touches are
 * modelled; the real guest object is 0x448+ bytes.
 * Guest layout (SceneObject, param_1 of FUN_005a85f8 / FUN_00408900 /
 * FUN_00408758):
 *   +0x000 int32  next          (intrusive object-list next pointer)
 *   +0x3e4 int32  model_parts   (pointer to the DocModelParts block)
 *   +0x428 uint32 render_state  (bit 0 toggled by the list traversal)
 *   +0x42c int32  draw_info     (pointer to the DocObjectDrawInfo block)
 *   +0x440 uint64 draw_flags    (bits 8..11 hold the 4-bit draw mode) */
typedef struct DocSceneObject {
    struct DocSceneObject    *next;         /* <- int @ +0x000             */
    DocModelParts            *model_parts;  /* <- int @ +0x3e4             */
    uint32_t                  render_state; /* <- u32 @ +0x428 (bit 0)     */
    struct DocObjectDrawInfo *draw_info;    /* <- int @ +0x42c             */
    uint64_t                  draw_flags;   /* <- u64 @ +0x440 (mode 8..11)*/
    const char               *name;         /* debug label (not in guest)  */
} DocSceneObject;

/* A render queue / draw context (the 0x4ee2e0 "contexts family"). The
 * original threads the context pointer through the walk but this cluster
 * does not dereference it (the flag bits live on the nodes); it is a
 * handle that will matter once draw submission is ported. The `bit` a
 * walk sets/clears selects which queue's lane in queue_flags: the main
 * pass uses bit 4 (FUN_002dbfc8), an alternate pass uses bit 10
 * (FUN_002dc158). We keep visit counters for observability/testing. */
typedef struct DocRenderQueue {
    int          id;              /* which context (0x4ee2e0 instance)    */
    const char  *name;
    unsigned     visited_groups;  /* nodes recursed into this run         */
    unsigned     visited_leaves;  /* leaves flagged this run              */
    /* Head of the flat list of model nodes registered against this
     * context, walked by the object_list.c kind/id walkers. The guest
     * reads this at context offset +0x84 (FUN_002ebdb8 / FUN_002ebe38);
     * nodes are linked via DocModelNode.next (+0x14). */
    struct DocModelNode *node_list;   /* <- head ptr @ guest +0x84        */
} DocRenderQueue;

/* ---- reauthored API -------------------------------------------------- */

/* FUN_002ebee8: recursively flag/unflag a node subtree for a render
 * queue. For a group node ('l'), recurse into up to child_count children
 * (skipping null slots). For a leaf, set (clear==0) or clear (clear!=0)
 * bit `queue_bit` in the node's queue_flags. */
void doc_model_node_set_queue_flag(DocRenderQueue *q, DocModelNode *node,
                                   int clear, unsigned queue_bit);

/* FUN_002dbfc8: fan a flag change across a model-parts block's up-to-8
 * root nodes (stops at the first null root). `clear` is passed straight
 * through to the node walk; the original hardcodes render queue bit 4. */
void doc_model_parts_set_queue_flag(DocModelParts *parts, DocRenderQueue *q,
                                    int clear, unsigned queue_bit);

/* Convenience matching the original's hardcoded call site exactly:
 * queue bit 4, against the engine's main render context. */
void doc_model_parts_flag_main(DocModelParts *parts, int clear);

/* FUN_005a85f8: per-object draw entry. Store the low 4 bits of `mode`
 * into the object's draw-mode field (draw_flags bits 8..11), then, if the
 * object has model parts, propagate: mode 0 clears the main draw bit,
 * any non-zero mode sets it. */
void doc_scene_object_set_draw_mode(DocSceneObject *obj, unsigned mode);

/* Accessor for the stored 4-bit draw mode (draw_flags bits 8..11). */
unsigned doc_scene_object_draw_mode(const DocSceneObject *obj);

#ifdef __cplusplus
}
#endif
#endif /* DOC_SCENE_GRAPH_H */
