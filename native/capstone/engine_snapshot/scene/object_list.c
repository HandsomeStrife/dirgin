/* =====================================================================
 * object_list.c  --  Reauthored scene-object list traversal.
 * See object_list.h for the oracle mapping and guest struct offsets.
 *
 * Each function is annotated with the decompiled original it reauthors.
 * The branch structure, loop bounds, null-termination and bit arithmetic
 * are transcribed from analysis/decomp/ (read-only); only raw offsets,
 * raw pointers, DAT_ globals and Ghidra names are replaced.
 * ===================================================================== */
#include "object_list.h"
#include <stddef.h>

/* ---- module state (replaces the DAT_ globals FUN_00408758 reads) ------ */
static int             g_object_list_enabled = 0;    /* DAT_004fae00  */
static DocSceneObject *g_object_list_head    = NULL; /* DAT_005b3788  */
static int             g_pass_source_flag    = 0;    /* DAT_004fb3d8  */
static int             g_scene_pass_flag     = 0;    /* DAT_005b37ac  */

void doc_scene_object_list_set_enabled(int enabled) { g_object_list_enabled = enabled; }
int  doc_scene_object_list_enabled(void)            { return g_object_list_enabled; }
void doc_scene_object_list_set_head(DocSceneObject *head) { g_object_list_head = head; }
DocSceneObject *doc_scene_object_list_head(void)    { return g_object_list_head; }
void doc_scene_set_pass_source_flag(int flag)       { g_pass_source_flag = flag; }
int  doc_scene_pass_source_flag(void)               { return g_pass_source_flag; }

/* ---------------------------------------------------------------------
 * FUN_005a6d70(flag):  DAT_005b37ac = flag;
 * A one-line setter. Its reader lives below the current seam, so we keep
 * the store as named module state rather than stub it away.
 * ------------------------------------------------------------------- */
void doc_scene_set_pass_flag(int flag) { g_scene_pass_flag = flag; }
int  doc_scene_pass_flag(void)         { return g_scene_pass_flag; }

/* ---------------------------------------------------------------------
 * FUN_00408758 (core walk).  Original, decoded:
 *   obj  = head;  info = head[0x42c];           // obj+0x42c = draw_info
 *   loop:
 *     if (info == 0)                 obj = *obj;            // obj+0x00 = next
 *     else if ((info->flags & 0x200) == 0) obj = *obj;     // not visible
 *     else {
 *         FUN_005a85f8(obj, info_byte@+2);                 // set draw mode
 *         if ((info->flags >> 8 & 1) == 0) obj[0x10a] &= ~1;   // +0x428
 *         else                             obj[0x10a] |=  1;
 *         obj = *obj;
 *     }
 *     if (obj == 0) break;
 *     info = obj[0x42c];
 * i.e. a linked-list walk that, for each VISIBLE object, publishes its
 * draw mode and toggles render_state bit 0 from the list-active bit.
 * ------------------------------------------------------------------- */
void doc_scene_object_list_traverse(DocSceneObject *head)
{
    for (DocSceneObject *obj = head; obj != NULL; obj = obj->next) {
        DocObjectDrawInfo *info = obj->draw_info;

        if (info == NULL)
            continue;                                   /* no descriptor  */
        if ((info->flags & DOC_OBJ_VISIBLE) == 0)
            continue;                                   /* not visible    */

        doc_scene_object_set_draw_mode(obj, doc_object_draw_info_mode(info));

        if (info->flags & DOC_OBJ_LIST_ACTIVE)
            obj->render_state |= 1u;
        else
            obj->render_state &= ~1u;
    }
}

/* ---------------------------------------------------------------------
 * FUN_00408758 (guest entry, void->void):
 *   if (DAT_004fae00 != 0) {
 *       FUN_005a6d70(DAT_004fb3d8 != 0);
 *       if (DAT_005b3788 != 0) <walk DAT_005b3788>;
 *   }
 * ------------------------------------------------------------------- */
void doc_scene_object_list_update(void)
{
    if (g_object_list_enabled == 0)
        return;
    doc_scene_set_pass_flag(g_pass_source_flag != 0);
    if (g_object_list_head != NULL)
        doc_scene_object_list_traverse(g_object_list_head);
}

/* ---------------------------------------------------------------------
 * FUN_002ebdb8(ctx, kind, clear, queue_bit):
 *   node = ctx[0x84];                        // head
 *   while (node) {
 *       if (node[0x54] == kind)              // select_kind
 *           FUN_002ebee8(ctx, node, clear, queue_bit);
 *       node = node[0x14];                   // next
 *   }
 * ------------------------------------------------------------------- */
void doc_render_queue_flag_nodes_by_kind(DocRenderQueue *q, int32_t kind,
                                         int clear, unsigned queue_bit)
{
    DocModelNode *node = q ? q->node_list : NULL;
    for (; node != NULL; node = node->next) {
        if (node->select_kind == kind)
            doc_model_node_set_queue_flag(q, node, clear, queue_bit);
    }
}

/* ---------------------------------------------------------------------
 * FUN_002ebe38(ctx, kind, match_id, clear, queue_bit):
 *   node = ctx[0x84];
 *   while (node) {
 *       if (node[0x54] == kind
 *           && (node->flags@+2 & 0x100) == 0          // DOC_NODE_FLAG_SKIP
 *           && *(byte*)(node[0x78] + 0x2e) == match_id)
 *           FUN_002ebee8(ctx, node, clear, queue_bit);
 *       node = node[0x14];
 *   }
 * The original dereferences node[0x78] (+0x2e) unconditionally; we guard
 * the null it would crash on, which only changes behaviour for a node the
 * original could not survive, and never the matched path.
 * ------------------------------------------------------------------- */
void doc_render_queue_flag_nodes_by_kind_id(DocRenderQueue *q, int32_t kind,
                                            uint8_t match_id, int clear,
                                            unsigned queue_bit)
{
    DocModelNode *node = q ? q->node_list : NULL;
    for (; node != NULL; node = node->next) {
        if (node->select_kind != kind)
            continue;
        if (node->node_flags & DOC_NODE_FLAG_SKIP)
            continue;
        if (node->binding != NULL && node->binding->id == match_id)
            doc_model_node_set_queue_flag(q, node, clear, queue_bit);
    }
}
