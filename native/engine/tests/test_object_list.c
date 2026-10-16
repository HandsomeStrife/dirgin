/* =====================================================================
 * test_object_list.c  --  Unit + oracle-parity test for the reauthored
 * scene-object list traversal (object_list.{h,c}).
 *
 * Two scenarios, each checked two ways (behaviour + oracle parity):
 *
 *   A. OBJECT LIST (FUN_00408758): a linked list of scene objects, some
 *      visible, some not, one with no draw_info. The clean traversal must
 *      publish the draw mode and toggle render_state bit 0 for exactly the
 *      visible objects, leaving the rest untouched.
 *
 *   B. CONTEXT NODE-LIST WALKERS (FUN_002ebdb8 / FUN_002ebe38): a render
 *      context's flat list of registered nodes, walked selecting by kind,
 *      and by kind + skip-flag + binding id.
 *
 * ORACLE PARITY: a byte-accurate transcription of the decompiled originals
 * (reading the documented offsets out of a raw "guest memory" buffer, with
 * 32-bit buffer-relative pointers exactly as the decomp dereferences ints)
 * is driven with the same inputs, and its results are asserted identical
 * to the clean implementation's. This proves the logic AND the offset map.
 * ===================================================================== */
#include "object_list.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ===================================================================== *
 * Raw "guest memory" accessors (0 == null, buffer-relative pointers).
 * ===================================================================== */
static uint16_t rld16(const uint8_t *m, uint32_t o) { uint16_t v; memcpy(&v, m + o, 2); return v; }
static void     rst16(uint8_t *m, uint32_t o, uint16_t v) { memcpy(m + o, &v, 2); }
static uint32_t rld32(const uint8_t *m, uint32_t o) { uint32_t v; memcpy(&v, m + o, 4); return v; }
static void     rst32(uint8_t *m, uint32_t o, uint32_t v) { memcpy(m + o, &v, 4); }
static uint64_t rld64(const uint8_t *m, uint32_t o) { uint64_t v; memcpy(&v, m + o, 8); return v; }
static void     rst64(uint8_t *m, uint32_t o, uint64_t v) { memcpy(m + o, &v, 8); }

/* ---- transcriptions of the already-ported leaf cluster (reused) ------- */
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
/* FUN_002dbfc8 (queue bit generalised, as on the clean side) */
static void ref_parts(uint8_t *m, uint32_t parts, long clear, unsigned bit) {
    if (rld32(m, parts + 0x14) == 0) return;
    for (int i = 0; i < 8; i++) {
        uint32_t root = rld32(m, parts + 0x1c + 4 * (uint32_t)i);
        if (root == 0) return;
        ref_node(m, root, clear, bit);
    }
}
/* FUN_005a85f8 */
static void ref_set_draw_mode(uint8_t *m, uint32_t obj, uint64_t mode) {
    uint32_t parts = rld32(m, obj + 0x3e4);
    uint64_t df = rld64(m, obj + 0x440);
    df = (df & 0xfffffffffffff0ffULL) | ((mode & 0xf) << 8);
    rst64(m, obj + 0x440, df);
    if (parts) ref_parts(m, parts, mode == 0 ? 1 : 0, 4);
}

/* ---- transcription of FUN_00408758 (core walk, explicit head) -------- */
static void ref_object_list(uint8_t *m, uint32_t head) {
    uint32_t puVar3 = rld32(m, head + 0x42c);   /* head->draw_info */
    uint32_t puVar2 = head;
    while (1) {
        if (puVar3 == 0) {
            puVar2 = rld32(m, puVar2 + 0);
        } else if ((rld32(m, puVar3) & 0x200) == 0) {
            puVar2 = rld32(m, puVar2 + 0);
        } else {
            ref_set_draw_mode(m, puVar2, m[puVar3 + 2]);
            uint32_t r;
            if (((int32_t)rld32(m, puVar3) >> 8 & 1) == 0)
                r = rld32(m, puVar2 + 0x428) & 0xfffffffeu;
            else
                r = rld32(m, puVar2 + 0x428) | 1u;
            rst32(m, puVar2 + 0x428, r);
            puVar2 = rld32(m, puVar2 + 0);
        }
        if (puVar2 == 0) break;
        puVar3 = rld32(m, puVar2 + 0x42c);
    }
}

/* ---- transcriptions of FUN_002ebdb8 / FUN_002ebe38 ------------------- */
static void ref_flag_by_kind(uint8_t *m, uint32_t ctx, int32_t kind,
                             long clear, unsigned bit) {
    uint32_t node = rld32(m, ctx + 0x84);
    if (node != 0) {
        int32_t k = (int32_t)rld32(m, node + 0x54);
        while (1) {
            if (k == kind) {
                ref_node(m, node, clear, bit);
                node = rld32(m, node + 0x14);
            } else {
                node = rld32(m, node + 0x14);
            }
            if (node == 0) break;
            k = (int32_t)rld32(m, node + 0x54);
        }
    }
}
static void ref_flag_by_kind_id(uint8_t *m, uint32_t ctx, int32_t kind,
                                unsigned match, long clear, unsigned bit) {
    uint32_t node = rld32(m, ctx + 0x84);
    if (node != 0) {
        int32_t k = (int32_t)rld32(m, node + 0x54);
        while (1) {
            if (k == kind) {
                if ((rld16(m, node + 2) & 0x100) == 0) {
                    if (m[rld32(m, node + 0x78) + 0x2e] == match) {
                        ref_node(m, node, clear, bit);
                        node = rld32(m, node + 0x14);
                    } else {
                        node = rld32(m, node + 0x14);
                    }
                } else {
                    node = rld32(m, node + 0x14);
                }
            } else {
                node = rld32(m, node + 0x14);
            }
            if (node == 0) break;
            k = (int32_t)rld32(m, node + 0x54);
        }
    }
}

/* ===================================================================== *
 * SCENARIO A: object list.
 * ===================================================================== */
enum {
    A_OBJ_BASE = 0x1000, A_OBJ_STRIDE = 0x500,
    A_INFO_BASE = 0x2400, A_INFO_STRIDE = 0x10,
    A_PARTS_BASE = 0x2500, A_PARTS_STRIDE = 0x40,
    A_NODE_BASE = 0x3000, A_NODE_STRIDE = 0x100
};
#define A_NOBJ 4
#define A_LEAF_PRESET 0x0001u

/* clean side */
static DocSceneObject    A_obj[A_NOBJ];
static DocObjectDrawInfo A_info[A_NOBJ];
static DocModelParts     A_parts[A_NOBJ];
static DocModelNode      A_group[A_NOBJ];
static DocModelNode      A_leaf[A_NOBJ][2];
static DocModelNode     *A_kids[A_NOBJ][2];

static const uint32_t A_flags[A_NOBJ] = {
    0x00030300u,  /* obj0: visible(0x200) + list-active(0x100) + mode 3 */
    0x00040000u,  /* obj1: NOT visible (no 0x200), mode 4               */
    0x00050200u,  /* obj2: visible, list-active clear, mode 5           */
    0u            /* obj3: draw_info == NULL (unused)                   */
};

static void A_build_clean(void) {
    for (int i = 0; i < A_NOBJ; i++) {
        for (int j = 0; j < 2; j++) {
            memset(&A_leaf[i][j], 0, sizeof A_leaf[i][j]);
            A_leaf[i][j].kind = DOC_NODE_LEAF;
            A_leaf[i][j].queue_flags = A_LEAF_PRESET;
            A_kids[i][j] = &A_leaf[i][j];
        }
        memset(&A_group[i], 0, sizeof A_group[i]);
        A_group[i].kind = DOC_NODE_GROUP;
        A_group[i].child_count = 2;
        A_group[i].children = A_kids[i];

        memset(&A_parts[i], 0, sizeof A_parts[i]);
        A_parts[i].node_count = 1;
        A_parts[i].roots[0] = &A_group[i];

        memset(&A_obj[i], 0, sizeof A_obj[i]);
        A_obj[i].model_parts = &A_parts[i];
        A_obj[i].render_state = 0xF1u;   /* bit0 preset set */
        A_obj[i].draw_flags = 0;
        A_obj[i].next = (i + 1 < A_NOBJ) ? &A_obj[i + 1] : NULL;

        if (i != 3) {
            A_info[i].flags = A_flags[i];
            A_obj[i].draw_info = &A_info[i];
        } else {
            A_obj[i].draw_info = NULL;
        }
    }
}

static uint32_t A_obj_off(int i)  { return (uint32_t)(A_OBJ_BASE + i * A_OBJ_STRIDE); }
static uint32_t A_info_off(int i) { return (uint32_t)(A_INFO_BASE + i * A_INFO_STRIDE); }
static uint32_t A_parts_off(int i){ return (uint32_t)(A_PARTS_BASE + i * A_PARTS_STRIDE); }
static uint32_t A_group_off(int i){ return (uint32_t)(A_NODE_BASE + (i * 3 + 0) * A_NODE_STRIDE); }
static uint32_t A_leaf_off(int i, int j) {
    return (uint32_t)(A_NODE_BASE + (i * 3 + 1 + j) * A_NODE_STRIDE);
}

static void A_build_raw(uint8_t *m) {
    memset(m, 0, 0x10000);
    for (int i = 0; i < A_NOBJ; i++) {
        uint32_t o = A_obj_off(i);
        rst32(m, o + 0x000, (i + 1 < A_NOBJ) ? A_obj_off(i + 1) : 0);
        rst32(m, o + 0x3e4, A_parts_off(i));
        rst32(m, o + 0x428, 0xF1u);
        rst32(m, o + 0x42c, (i != 3) ? A_info_off(i) : 0);
        rst64(m, o + 0x440, 0);

        if (i != 3)
            rst32(m, A_info_off(i) + 0, A_flags[i]);

        rst32(m, A_parts_off(i) + 0x14, 1);
        rst32(m, A_parts_off(i) + 0x1c, A_group_off(i));

        uint32_t g = A_group_off(i);
        m[g] = 'l';
        rst16(m, g + 4, 2);
        rst32(m, g + 0x30, A_leaf_off(i, 0));
        rst32(m, g + 0x34, A_leaf_off(i, 1));

        for (int j = 0; j < 2; j++) {
            uint32_t lf = A_leaf_off(i, j);
            m[lf] = 0;                       /* leaf */
            rst16(m, lf + 0x0a, A_LEAF_PRESET);
        }
    }
}

static void A_run(uint8_t *m) {
    A_build_clean();
    A_build_raw(m);

    doc_scene_object_list_traverse(&A_obj[0]);   /* clean */
    ref_object_list(m, A_obj_off(0));            /* oracle */

    /* behaviour: expected per-object outcome */
    const unsigned exp_mode[A_NOBJ]  = { 3, 0, 5, 0 };     /* draw_flags 8..11 */
    const uint32_t exp_state[A_NOBJ] = { 0xF1u, 0xF1u, 0xF0u, 0xF1u };
    const int      touched[A_NOBJ]   = { 1, 0, 1, 0 };     /* leaves get bit4? */

    for (int i = 0; i < A_NOBJ; i++) {
        unsigned mode = (unsigned)((A_obj[i].draw_flags >> 8) & 0xF);
        CHECK(mode == exp_mode[i], "A: obj%d draw mode got %u want %u",
              i, mode, exp_mode[i]);
        CHECK(A_obj[i].render_state == exp_state[i],
              "A: obj%d render_state got 0x%02x want 0x%02x",
              i, A_obj[i].render_state, exp_state[i]);
        uint16_t want_leaf = touched[i] ? (uint16_t)(A_LEAF_PRESET | 0x10)
                                        : (uint16_t)A_LEAF_PRESET;
        for (int j = 0; j < 2; j++)
            CHECK(A_leaf[i][j].queue_flags == want_leaf,
                  "A: obj%d leaf%d got 0x%04x want 0x%04x",
                  i, j, A_leaf[i][j].queue_flags, want_leaf);

        /* oracle parity: clean vs raw, field by field */
        CHECK((A_obj[i].draw_flags & 0x0F00) == (rld64(m, A_obj_off(i) + 0x440) & 0x0F00),
              "A: obj%d draw_flags oracle mismatch", i);
        CHECK(A_obj[i].render_state == rld32(m, A_obj_off(i) + 0x428),
              "A: obj%d render_state oracle mismatch (clean 0x%02x raw 0x%02x)",
              i, A_obj[i].render_state, rld32(m, A_obj_off(i) + 0x428));
        for (int j = 0; j < 2; j++)
            CHECK(A_leaf[i][j].queue_flags == rld16(m, A_leaf_off(i, j) + 0x0a),
                  "A: obj%d leaf%d queue_flags oracle mismatch (clean 0x%04x raw 0x%04x)",
                  i, j, A_leaf[i][j].queue_flags, rld16(m, A_leaf_off(i, j) + 0x0a));
    }
}

/* ===================================================================== *
 * SCENARIO B: render-context node-list walkers.
 * ===================================================================== */
enum {
    B_CTX_OFF = 0x6000,
    B_NODE_BASE = 0x4000, B_NODE_STRIDE = 0x100,
    B_BIND_BASE = 0x5000, B_BIND_STRIDE = 0x40
};
#define B_NNODE 5
#define B_LEAF_PRESET 0x0001u

/* per-node descriptors: select_kind, skip-flag, binding id */
static const int32_t  B_kind[B_NNODE] = { 1, 2, 1, 1, 1 };
static const uint16_t B_nflag[B_NNODE] = { 0, 0, (uint16_t)DOC_NODE_FLAG_SKIP, 0, 0 };
static const uint8_t  B_id[B_NNODE] = { 7, 7, 7, 9, 7 };

static DocRenderQueue B_q;
static DocModelNode   B_node[B_NNODE];
static DocNodeBinding B_bind[B_NNODE];

static void B_build_clean(void) {
    memset(&B_q, 0, sizeof B_q);
    B_q.id = 1;
    B_q.name = "ctx";
    for (int i = 0; i < B_NNODE; i++) {
        memset(&B_node[i], 0, sizeof B_node[i]);
        B_node[i].kind = DOC_NODE_LEAF;         /* tree-leaf: flag hits it directly */
        B_node[i].queue_flags = B_LEAF_PRESET;
        B_node[i].select_kind = B_kind[i];
        B_node[i].node_flags = B_nflag[i];
        B_bind[i].id = B_id[i];
        B_node[i].binding = &B_bind[i];
        B_node[i].next = (i + 1 < B_NNODE) ? &B_node[i + 1] : NULL;
    }
    B_q.node_list = &B_node[0];
}

static uint32_t B_node_off(int i) { return (uint32_t)(B_NODE_BASE + i * B_NODE_STRIDE); }
static uint32_t B_bind_off(int i) { return (uint32_t)(B_BIND_BASE + i * B_BIND_STRIDE); }

static void B_build_raw(uint8_t *m) {
    memset(m, 0, 0x10000);
    rst32(m, B_CTX_OFF + 0x84, B_node_off(0));
    for (int i = 0; i < B_NNODE; i++) {
        uint32_t o = B_node_off(i);
        m[o] = 0;                                  /* leaf */
        rst16(m, o + 0x02, B_nflag[i]);
        rst16(m, o + 0x0a, (uint16_t)B_LEAF_PRESET);
        rst32(m, o + 0x14, (i + 1 < B_NNODE) ? B_node_off(i + 1) : 0);
        rst32(m, o + 0x54, (uint32_t)B_kind[i]);
        rst32(m, o + 0x78, B_bind_off(i));
        m[B_bind_off(i) + 0x2e] = B_id[i];
    }
}

static void B_run(uint8_t *m) {
    /* --- by kind (kind 1): flags n0,n2,n3,n4; n1 (kind 2) untouched --- */
    B_build_clean();
    B_build_raw(m);
    doc_render_queue_flag_nodes_by_kind(&B_q, 1, 0, 4);
    ref_flag_by_kind(m, B_CTX_OFF, 1, 0, 4);

    const int kind_expect[B_NNODE] = { 1, 0, 1, 1, 1 };
    for (int i = 0; i < B_NNODE; i++) {
        uint16_t want = kind_expect[i] ? (uint16_t)(B_LEAF_PRESET | 0x10)
                                       : (uint16_t)B_LEAF_PRESET;
        CHECK(B_node[i].queue_flags == want,
              "B.kind: node%d got 0x%04x want 0x%04x", i, B_node[i].queue_flags, want);
        CHECK(B_node[i].queue_flags == rld16(m, B_node_off(i) + 0x0a),
              "B.kind: node%d oracle mismatch (clean 0x%04x raw 0x%04x)",
              i, B_node[i].queue_flags, rld16(m, B_node_off(i) + 0x0a));
    }

    /* --- by kind+id (kind 1, id 7): flags only n0,n4 ------------------
     * n1 wrong kind, n2 skip-flag set, n3 id 9. */
    B_build_clean();
    B_build_raw(m);
    doc_render_queue_flag_nodes_by_kind_id(&B_q, 1, 7, 0, 4);
    ref_flag_by_kind_id(m, B_CTX_OFF, 1, 7, 0, 4);

    const int id_expect[B_NNODE] = { 1, 0, 0, 0, 1 };
    for (int i = 0; i < B_NNODE; i++) {
        uint16_t want = id_expect[i] ? (uint16_t)(B_LEAF_PRESET | 0x10)
                                     : (uint16_t)B_LEAF_PRESET;
        CHECK(B_node[i].queue_flags == want,
              "B.id: node%d got 0x%04x want 0x%04x", i, B_node[i].queue_flags, want);
        CHECK(B_node[i].queue_flags == rld16(m, B_node_off(i) + 0x0a),
              "B.id: node%d oracle mismatch (clean 0x%04x raw 0x%04x)",
              i, B_node[i].queue_flags, rld16(m, B_node_off(i) + 0x0a));
    }
}

/* ===================================================================== *
 * Guest-entry gating (FUN_00408758 void->void) over module state.
 * ===================================================================== */
static void entry_run(void) {
    A_build_clean();

    /* disabled => no-op, pass flag untouched */
    doc_scene_object_list_set_enabled(0);
    doc_scene_object_list_set_head(&A_obj[0]);
    doc_scene_set_pass_flag(-1);                 /* sentinel */
    doc_scene_set_pass_source_flag(1);
    doc_scene_object_list_update();
    CHECK(doc_scene_pass_flag() == -1, "entry: disabled must not touch pass flag");
    CHECK(A_obj[0].render_state == 0xF1u, "entry: disabled must not walk list");

    /* enabled => publishes pass flag (source != 0 -> 1) and walks */
    doc_scene_object_list_set_enabled(1);
    doc_scene_set_pass_source_flag(5);
    doc_scene_object_list_update();
    CHECK(doc_scene_pass_flag() == 1, "entry: pass flag should be (source!=0)=1, got %d",
          doc_scene_pass_flag());
    CHECK(((A_obj[0].draw_flags >> 8) & 0xF) == 3, "entry: list should have been walked");

    /* enabled but null head => still sets pass flag, no crash */
    A_build_clean();
    doc_scene_object_list_set_head(NULL);
    doc_scene_set_pass_source_flag(0);
    doc_scene_set_pass_flag(-1);
    doc_scene_object_list_update();
    CHECK(doc_scene_pass_flag() == 0, "entry: source 0 -> pass flag 0, got %d",
          doc_scene_pass_flag());
}

int main(void) {
    static uint8_t mem[0x10000];

    A_run(mem);
    B_run(mem);
    entry_run();

    if (g_fail == 0) printf("test_object_list: ALL PASS\n");
    else             printf("test_object_list: %d FAILURE(S)\n", g_fail);
    return g_fail ? 1 : 0;
}
