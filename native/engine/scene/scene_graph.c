/* =====================================================================
 * scene_graph.c  --  Reauthored scene-graph / model-node traversal.
 * See scene_graph.h for the oracle mapping and guest struct offsets.
 *
 * Each function is annotated with the decompiled original it reauthors.
 * The logic is transcribed from analysis/decomp/, not guessed: the branch
 * structure, loop bounds, null-termination and bit arithmetic all match
 * the Ghidra output; only the raw offsets, raw pointers and unreadable
 * names are replaced with named structs and portable code.
 * ===================================================================== */
#include "scene_graph.h"
#include <stddef.h>

/* The main render context the original hardcodes (literal 0x4ee2e0 in
 * FUN_002dbfc8). A module-owned singleton replaces the DAT_ global; later
 * subsystems can register additional contexts. */
static DocRenderQueue g_render_queue_main = { 0, "main", 0, 0, NULL };

/* ---------------------------------------------------------------------
 * FUN_002ebee8(context, node, clear, queue_bit)
 *   if (node->tag == 'l')          // group
 *       for i in 0..child_count-1: if children[i] recurse
 *   else                           // leaf
 *       bit = 1 << (queue_bit & 31)
 *       if (clear == 0) queue_flags |=  bit
 *       else            queue_flags &= ~bit
 * ------------------------------------------------------------------- */
void doc_model_node_set_queue_flag(DocRenderQueue *q, DocModelNode *node,
                                   int clear, unsigned queue_bit)
{
    if (node == NULL)
        return;

    if (node->kind == DOC_NODE_GROUP) {
        if (q) q->visited_groups++;
        for (uint16_t i = 0; i < node->child_count; i++) {
            DocModelNode *child = node->children ? node->children[i] : NULL;
            if (child != NULL)
                doc_model_node_set_queue_flag(q, child, clear, queue_bit);
        }
    } else {
        uint16_t bit = (uint16_t)(1u << (queue_bit & 0x1f));
        if (clear == 0)
            node->queue_flags |= bit;
        else
            node->queue_flags &= (uint16_t)~bit;
        if (q) q->visited_leaves++;
    }
}

/* ---------------------------------------------------------------------
 * FUN_002dbfc8(parts, clear)
 *   if (parts->node_count == 0) return;      // +0x14 guard
 *   for i in 0..7:
 *       root = parts->roots[i];              // +0x1c array
 *       if (root == 0) return;               // null-terminated
 *       FUN_002ebee8(0x4ee2e0, root, clear, 4);
 * The queue_bit is generalised to a parameter here; doc_model_parts_flag_main
 * supplies the original's literal 4 + main context.
 * ------------------------------------------------------------------- */
void doc_model_parts_set_queue_flag(DocModelParts *parts, DocRenderQueue *q,
                                    int clear, unsigned queue_bit)
{
    if (parts == NULL || parts->node_count == 0)
        return;

    for (int i = 0; i < DOC_MODEL_MAX_ROOTS; i++) {
        DocModelNode *root = parts->roots[i];
        if (root == NULL)
            return;
        doc_model_node_set_queue_flag(q, root, clear, queue_bit);
    }
}

void doc_model_parts_flag_main(DocModelParts *parts, int clear)
{
    doc_model_parts_set_queue_flag(parts, &g_render_queue_main, clear, 4);
}

/* ---------------------------------------------------------------------
 * FUN_005a85f8(obj, mode)
 *   parts = obj->model_parts;                           // +0x3e4
 *   obj->draw_flags = (obj->draw_flags & ~0xf00)        // +0x440 bits 8..11
 *                   | ((mode & 0xf) << 8);
 *   if (parts != 0)
 *       FUN_002dbfc8(parts, mode == 0 ? 1 : 0);
 * i.e. mode 0 -> clear the main draw bit; any non-zero mode -> set it.
 * ------------------------------------------------------------------- */
void doc_scene_object_set_draw_mode(DocSceneObject *obj, unsigned mode)
{
    if (obj == NULL)
        return;

    DocModelParts *parts = obj->model_parts;

    obj->draw_flags = (obj->draw_flags & ~(uint64_t)0x0F00)
                    | ((uint64_t)(mode & 0xF) << 8);

    if (parts != NULL)
        doc_model_parts_flag_main(parts, mode == 0 ? 1 : 0);
}

unsigned doc_scene_object_draw_mode(const DocSceneObject *obj)
{
    if (obj == NULL)
        return 0;
    return (unsigned)((obj->draw_flags >> 8) & 0xF);
}
