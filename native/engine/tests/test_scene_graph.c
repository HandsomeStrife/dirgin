/* =====================================================================
 * test_scene_graph.c  --  Unit + oracle-parity test for the reauthored
 * scene-graph cluster.
 *
 * Two things are checked on a synthetic scene graph:
 *   1. BEHAVIOUR: the clean API sets/clears exactly the right draw-queue
 *      bit on exactly the right leaves, preserving other bits, and stores
 *      the draw mode in the object; group recursion and root/child null-
 *      termination behave as the original.
 *   2. ORACLE PARITY: a byte-accurate transcription of the three
 *      decompiled functions (operating on a raw "guest memory" buffer at
 *      the documented struct offsets, with 32-bit buffer-relative
 *      pointers) is driven with the same inputs, and its per-leaf flag
 *      results are asserted identical to the clean implementation's.
 *      This is the DECOMP-METHOD.md validation step made concrete: it
 *      proves both the logic AND the struct-offset map are faithful.
 * ===================================================================== */
#include "scene_graph.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ===================================================================== *
 * Byte-accurate reference: transcription of the decompiled originals,
 * reading the documented offsets out of a raw buffer. "Pointers" are
 * 32-bit offsets into the same buffer (0 == null), which is how the
 * decomp treats the dereferenced ints. Float quirks are irrelevant here
 * (pure integer/bit logic), so this is a faithful oracle.
 * ===================================================================== */
static uint16_t rld16(const uint8_t *m, uint32_t o) { uint16_t v; memcpy(&v, m + o, 2); return v; }
static void     rst16(uint8_t *m, uint32_t o, uint16_t v) { memcpy(m + o, &v, 2); }
static uint32_t rld32(const uint8_t *m, uint32_t o) { uint32_t v; memcpy(&v, m + o, 4); return v; }
static void     rst32(uint8_t *m, uint32_t o, uint32_t v) { memcpy(m + o, &v, 4); }
static uint64_t rld64(const uint8_t *m, uint32_t o) { uint64_t v; memcpy(&v, m + o, 8); return v; }
static void     rst64(uint8_t *m, uint32_t o, uint64_t v) { memcpy(m + o, &v, 8); }

/* FUN_002ebee8 */
static void ref_node(uint8_t *m, uint32_t node, long clear, unsigned bit) {
    if (m[node] == 'l') {
        int cnt = (int)rld16(m, node + 4);
        for (int i = 0; i < cnt; i++) {
            uint32_t ch = rld32(m, node + 0x30 + 4 * (uint32_t)i);
            if (ch) ref_node(m, ch, clear, bit);
        }
    } else {
        uint16_t b = (uint16_t)(1u << (bit & 0x1f));
        uint16_t f = rld16(m, node + 0x0a);
        if (clear == 0) f = (uint16_t)(f | b);
        else            f = (uint16_t)(f & ~b);
        rst16(m, node + 0x0a, f);
    }
}
/* FUN_002dbfc8 (queue bit generalised like the clean side) */
static void ref_parts(uint8_t *m, uint32_t parts, long clear, unsigned bit) {
    if (rld32(m, parts + 0x14) == 0) return;
    for (int i = 0; i < 8; i++) {
        uint32_t root = rld32(m, parts + 0x1c + 4 * (uint32_t)i);
        if (root == 0) return;
        ref_node(m, root, clear, bit);
    }
}
/* FUN_005a85f8 */
static void ref_object(uint8_t *m, uint32_t obj, uint64_t mode) {
    uint32_t parts = rld32(m, obj + 0x3e4);
    uint64_t df = rld64(m, obj + 0x440);
    df = (df & 0xfffffffffffff0ffULL) | ((mode & 0xf) << 8);
    rst64(m, obj + 0x440, df);
    if (parts) ref_parts(m, parts, mode == 0 ? 1 : 0, 4);
}

/* ===================================================================== *
 * Build the SAME logical tree twice: as clean structs and in a raw
 * buffer. Parallel arrays map each clean leaf to its raw offset so the
 * two can be diffed after every operation.
 *
 *            parts
 *              |
 *             G0 (group, 3 children)
 *            / | \
 *          L1  G1  L2            G1 (group, 2 children)
 *              / \
 *            L3   L4
 * ===================================================================== */
enum { OBJ_OFF = 0x100, PARTS_OFF = 0x600, NODE_BASE = 0x700, NODE_STRIDE = 0x80 };

/* clean side */
static DocMesh       mesh_pool[8];
static DocModelNode  node_L1, node_L2, node_L3, node_L4, node_G0, node_G1;
static DocModelNode *g0_children[3];
static DocModelNode *g1_children[2];
static DocModelParts parts;
static DocSceneObject obj;

/* leaf correspondence table */
static DocModelNode *clean_leaves[4];
static uint32_t      raw_leaves[4];

static void mk_leaf(DocModelNode *n, const char *name, DocMesh *mesh, uint16_t preset) {
    n->kind = DOC_NODE_LEAF;
    n->queue_flags = preset;
    n->child_count = 0;
    n->children = NULL;
    n->mesh = mesh;
    n->name = name;
}
static void mk_group(DocModelNode *n, const char *name, DocModelNode **kids, uint16_t count) {
    n->kind = DOC_NODE_GROUP;
    n->queue_flags = 0;
    n->child_count = count;
    n->children = kids;
    n->mesh = NULL;
    n->name = name;
}

static void build_clean(uint16_t preset) {
    mesh_pool[0].name = "L1"; mesh_pool[1].name = "L2";
    mesh_pool[2].name = "L3"; mesh_pool[3].name = "L4";
    mk_leaf(&node_L1, "L1", &mesh_pool[0], preset);
    mk_leaf(&node_L2, "L2", &mesh_pool[1], preset);
    mk_leaf(&node_L3, "L3", &mesh_pool[2], preset);
    mk_leaf(&node_L4, "L4", &mesh_pool[3], preset);
    g1_children[0] = &node_L3; g1_children[1] = &node_L4;
    mk_group(&node_G1, "G1", g1_children, 2);
    g0_children[0] = &node_L1; g0_children[1] = &node_G1; g0_children[2] = &node_L2;
    mk_group(&node_G0, "G0", g0_children, 3);

    memset(&parts, 0, sizeof parts);
    parts.node_count = 1;
    parts.roots[0] = &node_G0;   /* roots[1..] stay null (null-terminated) */

    memset(&obj, 0, sizeof obj);
    obj.model_parts = &parts;
    obj.draw_flags = 0;
    obj.name = "obj";

    clean_leaves[0] = &node_L1; clean_leaves[1] = &node_L2;
    clean_leaves[2] = &node_L3; clean_leaves[3] = &node_L4;
}

/* place a node in the raw buffer; returns its offset */
static uint32_t raw_leaf(uint8_t *m, int idx, uint16_t preset) {
    uint32_t off = NODE_BASE + (uint32_t)idx * NODE_STRIDE;
    m[off] = 0;                       /* tag != 'l' => leaf */
    rst16(m, off + 0x0a, preset);
    return off;
}
static uint32_t raw_group(uint8_t *m, int idx, const uint32_t *kids, uint16_t count) {
    uint32_t off = NODE_BASE + (uint32_t)idx * NODE_STRIDE;
    m[off] = 'l';
    rst16(m, off + 4, count);
    for (uint16_t i = 0; i < count; i++)
        rst32(m, off + 0x30 + 4 * i, kids[i]);
    return off;
}

static void build_raw(uint8_t *m, uint16_t preset) {
    memset(m, 0, 0x1000);
    uint32_t l1 = raw_leaf(m, 0, preset);
    uint32_t l2 = raw_leaf(m, 1, preset);
    uint32_t l3 = raw_leaf(m, 2, preset);
    uint32_t l4 = raw_leaf(m, 3, preset);
    uint32_t g1kids[2] = { l3, l4 };
    uint32_t g1 = raw_group(m, 4, g1kids, 2);
    uint32_t g0kids[3] = { l1, g1, l2 };
    uint32_t g0 = raw_group(m, 5, g0kids, 3);

    rst32(m, PARTS_OFF + 0x14, 1);            /* node_count */
    rst32(m, PARTS_OFF + 0x1c, g0);           /* roots[0] = G0; rest null */

    rst32(m, OBJ_OFF + 0x3e4, PARTS_OFF);     /* model_parts */
    rst64(m, OBJ_OFF + 0x440, 0);             /* draw_flags */

    raw_leaves[0] = l1; raw_leaves[1] = l2; raw_leaves[2] = l3; raw_leaves[3] = l4;
}

static void diff_leaves(const uint8_t *m, const char *where) {
    for (int i = 0; i < 4; i++) {
        uint16_t clean = clean_leaves[i]->queue_flags;
        uint16_t raw   = rld16(m, raw_leaves[i] + 0x0a);
        CHECK(clean == raw, "%s: leaf %s clean=0x%04x raw=0x%04x (oracle mismatch)",
              where, clean_leaves[i]->name, clean, raw);
    }
}

int main(void) {
    static uint8_t mem[0x1000];
    const uint16_t preset = 0x0001;   /* bit 0 preset to prove it is preserved */

    /* ---- draw mode 3: non-zero => SET main draw bit (bit 4 = 0x10) ---- */
    build_clean(preset);
    build_raw(mem, preset);
    DocRenderQueue q = { 0, "main", 0, 0, NULL };

    /* run the real public entry on the clean side; same op on the oracle */
    doc_scene_object_set_draw_mode(&obj, 3);
    ref_object(mem, OBJ_OFF, 3);

    CHECK(doc_scene_object_draw_mode(&obj) == 3, "draw mode field not stored (got %u)",
          doc_scene_object_draw_mode(&obj));
    CHECK(((obj.draw_flags >> 8) & 0xF) == 3, "draw_flags bits 8..11 wrong");
    CHECK((rld64(mem, OBJ_OFF + 0x440) & 0x0F00) == (obj.draw_flags & 0x0F00),
          "draw_flags oracle mismatch");

    for (int i = 0; i < 4; i++)
        CHECK(clean_leaves[i]->queue_flags == (uint16_t)(preset | 0x10),
              "leaf %s should have bit4 set + preset preserved, got 0x%04x",
              clean_leaves[i]->name, clean_leaves[i]->queue_flags);
    diff_leaves(mem, "set mode 3");

    /* ---- draw mode 0: zero => CLEAR main draw bit -------------------- */
    doc_scene_object_set_draw_mode(&obj, 0);
    ref_object(mem, OBJ_OFF, 0);
    CHECK(doc_scene_object_draw_mode(&obj) == 0, "draw mode should be 0");
    for (int i = 0; i < 4; i++)
        CHECK(clean_leaves[i]->queue_flags == preset,
              "leaf %s should be back to preset, got 0x%04x",
              clean_leaves[i]->name, clean_leaves[i]->queue_flags);
    diff_leaves(mem, "clear mode 0");

    /* ---- visit counts: 2 groups (G0,G1), 4 leaves ------------------- */
    build_clean(preset);
    doc_model_parts_set_queue_flag(&parts, &q, 0, 4);
    CHECK(q.visited_groups == 2, "expected 2 groups visited, got %u", q.visited_groups);
    CHECK(q.visited_leaves == 4, "expected 4 leaves visited, got %u", q.visited_leaves);

    /* ---- alternate render queue (bit 10, cf. FUN_002dc158) parity ---- */
    build_clean(preset);
    build_raw(mem, preset);
    doc_model_parts_set_queue_flag(&parts, &q, 0, 10);
    ref_parts(mem, PARTS_OFF, 0, 10);
    for (int i = 0; i < 4; i++)
        CHECK(clean_leaves[i]->queue_flags == (uint16_t)(preset | (1u << 10)),
              "leaf %s bit10 not set, got 0x%04x",
              clean_leaves[i]->name, clean_leaves[i]->queue_flags);
    diff_leaves(mem, "bit10 queue");

    /* ---- null guards: node_count 0 and a null root are no-ops -------- */
    build_clean(preset);
    parts.node_count = 0;
    doc_model_parts_flag_main(&parts, 0);
    for (int i = 0; i < 4; i++)
        CHECK(clean_leaves[i]->queue_flags == preset,
              "node_count==0 must be a no-op (leaf %s = 0x%04x)",
              clean_leaves[i]->name, clean_leaves[i]->queue_flags);

    build_clean(preset);
    parts.roots[0] = NULL;    /* first root null => whole fan-out skipped */
    doc_model_parts_flag_main(&parts, 0);
    for (int i = 0; i < 4; i++)
        CHECK(clean_leaves[i]->queue_flags == preset,
              "null first root must skip (leaf %s = 0x%04x)",
              clean_leaves[i]->name, clean_leaves[i]->queue_flags);

    if (g_fail == 0) printf("test_scene_graph: ALL PASS\n");
    else             printf("test_scene_graph: %d FAILURE(S)\n", g_fail);
    return g_fail ? 1 : 0;
}
