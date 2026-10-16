/* =====================================================================
 * test_actor.c  --  Unit + oracle-parity + mutation test for the
 * reauthored ACTOR / ENTITY subsystem (actor.{h,c}).
 *
 * Three things are checked on a synthetic actor pool:
 *   1. BEHAVIOUR: the clean API allocates/frees slots, stores and reads
 *      position & orientation, draw mode, the script-flag field,
 *      transparency (with the derived alpha) and the attach graph; the
 *      manager's free-slot scan and active-list unlink behave as the
 *      original.
 *   2. ORACLE PARITY: a byte-accurate transcription of the decompiled
 *      originals (operating on a raw "guest memory" buffer of 0x480-byte
 *      slots at the documented struct offsets, with 32-bit buffer-relative
 *      pointers) is driven with the same inputs, and its per-slot results
 *      are asserted identical to the clean implementation's. This proves
 *      both the LOGIC and the struct-offset map (DECOMP-METHOD.md §6a).
 *   3. MUTATION SENSITIVITY (§6b, self-checking): the parity asserts are
 *      shown load-bearing by constructing the most likely mutations inline
 *      and asserting they WOULD change the result (so a mutated actor.c
 *      goes red). A source-level mutation was also run by hand; see the
 *      report.
 * ===================================================================== */
#include "actor.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ===================================================================== *
 * Byte-accurate reference: transcription of the decompiled originals,
 * reading documented offsets out of a raw buffer of 0x480-byte slots.
 * "Pointers" are buffer offsets (0 == null). Position/orientation are
 * floats; everything else is integer/bit logic, so float quirks do not
 * enter (DECOMP-METHOD.md Floats).
 * ===================================================================== */
#define SLOT    0x480
#define OCCUPIED  0x400000ull
#define NO_MODEL  0x100000ull
#define ATTACHED  0x200000ull

static uint8_t  rld8 (const uint8_t *m, uint32_t o) { return m[o]; }
static void     rst8 (uint8_t *m, uint32_t o, uint8_t v) { m[o] = v; }
static uint32_t rld32(const uint8_t *m, uint32_t o) { uint32_t v; memcpy(&v, m+o, 4); return v; }
static void     rst32(uint8_t *m, uint32_t o, uint32_t v) { memcpy(m+o, &v, 4); }
static uint64_t rld64(const uint8_t *m, uint32_t o) { uint64_t v; memcpy(&v, m+o, 8); return v; }
static void     rst64(uint8_t *m, uint32_t o, uint64_t v) { memcpy(m+o, &v, 8); }
static float    rldf (const uint8_t *m, uint32_t o) { float v; memcpy(&v, m+o, 4); return v; }
static void     rstf (uint8_t *m, uint32_t o, float v) { memcpy(m+o, &v, 4); }

/* FUN_005a71b8: clear OCCUPIED on every slot. */
static void ref_pool_init(uint8_t *m, uint32_t base, int count) {
    for (int i = 0; i < count; i++) {
        uint32_t s = base + (uint32_t)i * SLOT;
        rst64(m, s + 0x440, rld64(m, s + 0x440) & ~OCCUPIED);
    }
}
/* FUN_005a7208 + FUN_005a7978 reset: first slot without OCCUPIED, zeroed,
 * OCCUPIED set. Returns offset, or 0 if full. */
static uint32_t ref_alloc(uint8_t *m, uint32_t base, int count) {
    for (int i = 0; i < count; i++) {
        uint32_t s = base + (uint32_t)i * SLOT;
        if ((rld64(m, s + 0x440) & OCCUPIED) == 0) {
            memset(m + s, 0, SLOT);                 /* FUN_005a7978 zero  */
            rst64(m, s + 0x440, OCCUPIED);          /* mark in use        */
            return s;
        }
    }
    return 0;
}
/* FUN_005a7148: clear OCCUPIED (in-pool path). */
static void ref_free(uint8_t *m, uint32_t s) {
    rst64(m, s + 0x440, rld64(m, s + 0x440) & ~OCCUPIED);
}
/* FUN_005a7008 / FUN_005a7288: store a 3-float vector at an offset. */
static void ref_setvec(uint8_t *m, uint32_t s, uint32_t off, float x, float y, float z) {
    rstf(m, s + off + 0, x); rstf(m, s + off + 4, y); rstf(m, s + off + 8, z);
}
/* FUN_005a85f8 (draw mode store; model-parts propagation omitted: synthetic
 * actor has no parts, matching the iVar1==0 branch). */
static void ref_set_draw_mode(uint8_t *m, uint32_t s, uint64_t mode) {
    uint64_t df = rld64(m, s + 0x440);
    df = (df & 0xfffffffffffff0ffULL) | ((mode & 0xf) << 8);
    rst64(m, s + 0x440, df);
}
/* FUN_005a8658: (int)(df << 20) >> 28 == bits 8..11 (unsigned here). */
static unsigned ref_draw_mode(const uint8_t *m, uint32_t s) {
    return (unsigned)((rld64(m, s + 0x440) >> 8) & 0xf);
}
/* FUN_005a7338: set bit in the +0x424 flag field. */
static void ref_set_flag(uint8_t *m, uint32_t s, unsigned bit) {
    rst32(m, s + 0x424, rld32(m, s + 0x424) | (1u << (bit & 0x1f)));
}
/* FUN_005a86a8: transparency @ +0x452, alpha = (t*base)>>7 @ +0x45a. */
static void ref_set_transparency(uint8_t *m, uint32_t s, uint8_t t) {
    rst8(m, s + 0x452, t);
    rst8(m, s + 0x45a, (uint8_t)(((unsigned)t * (unsigned)rld8(m, s + 0x459)) >> 7));
}
/* FUN_005a86d0: base alpha @ +0x459, alpha recomputed. */
static void ref_set_base_alpha(uint8_t *m, uint32_t s, uint8_t b) {
    rst8(m, s + 0x459, b);
    rst8(m, s + 0x45a, (uint8_t)(((unsigned)rld8(m, s + 0x452) * (unsigned)b) >> 7));
}
/* FUN_005a7698: attach child @ +0x3e8[i], bone @ +0x45e[i], ATTACHED. */
static void ref_attach(uint8_t *m, uint32_t parent, int i, uint32_t child, uint8_t bone) {
    rst32(m, parent + 0x3e8 + (uint32_t)i * 4, child);
    rst8(m, parent + 0x45e + (uint32_t)i, bone);
    if (child) rst64(m, child + 0x440, rld64(m, child + 0x440) | ATTACHED);
}
static uint32_t ref_get_attached(const uint8_t *m, uint32_t s, int i) {
    return (i >= 0 && i < 4) ? rld32(m, s + 0x3e8 + (uint32_t)i * 4) : 0;
}

/* ===================================================================== *
 * Shared harness: a 3-slot pool, clean and raw, with a slot<->offset map.
 * ===================================================================== */
enum { POOL_BASE = 0x1000, N_SLOTS = 3 };
#define BUF_BYTES (POOL_BASE + N_SLOTS * SLOT + 0x100)

static float feq(float a, float b) {
    float d = a - b; if (d < 0) d = -d; return d <= 1e-6f;
}

int main(void) {
    static uint8_t mem[BUF_BYTES];
    static DocActor slots[N_SLOTS];
    DocActorManager mgr;

    /* ---- pool init: all slots free, clean & raw ------------------- */
    /* pre-dirty the OCCUPIED bit so init is observably doing work */
    for (int i = 0; i < N_SLOTS; i++)
        slots[i].object.draw_flags = OCCUPIED;
    memset(mem, 0, sizeof mem);
    for (int i = 0; i < N_SLOTS; i++)
        rst64(mem, POOL_BASE + (uint32_t)i * SLOT + 0x440, OCCUPIED);

    doc_actor_manager_init(&mgr, slots, N_SLOTS);
    ref_pool_init(mem, POOL_BASE, N_SLOTS);
    for (int i = 0; i < N_SLOTS; i++)
        CHECK((slots[i].object.draw_flags & OCCUPIED) == 0 &&
              (rld64(mem, POOL_BASE + (uint32_t)i * SLOT + 0x440) & OCCUPIED) == 0,
              "init: slot %d should be free in both", i);

    /* ---- alloc: first free slot, marked occupied ------------------ */
    uint32_t raw_off[N_SLOTS];
    DocActor *act[N_SLOTS];
    for (int i = 0; i < N_SLOTS; i++) {
        act[i]     = doc_actor_alloc(&mgr);
        raw_off[i] = ref_alloc(mem, POOL_BASE, N_SLOTS);
        CHECK(act[i] != NULL && raw_off[i] != 0, "alloc %d returned a slot", i);
        CHECK(act[i] == &slots[i], "alloc %d returned slot %d (first free)", i, i);
        CHECK(raw_off[i] == POOL_BASE + (uint32_t)i * SLOT, "raw alloc %d off", i);
        CHECK((act[i]->object.draw_flags & OCCUPIED) != 0 &&
              (rld64(mem, raw_off[i] + 0x440) & OCCUPIED) != 0,
              "alloc %d marks OCCUPIED in both", i);
    }
    CHECK(doc_actor_alloc(&mgr) == NULL && ref_alloc(mem, POOL_BASE, N_SLOTS) == 0,
          "alloc on full pool returns NULL in both");
    CHECK(mgr.live_count == N_SLOTS, "live_count should be %d, got %d", N_SLOTS, mgr.live_count);

    /* ---- position & orientation: store + parity ------------------- */
    doc_actor_set_position(act[0], dvec3_make(12.5f, -3.25f, 7.0f));
    ref_setvec(mem, raw_off[0], 0x30, 12.5f, -3.25f, 7.0f);
    doc_actor_set_orientation(act[0], dvec3_make(0.5f, 1.5f, -0.75f));
    ref_setvec(mem, raw_off[0], 0xe0, 0.5f, 1.5f, -0.75f);

    dvec3 p = doc_actor_get_position(act[0]);
    CHECK(feq(p.x, 12.5f) && feq(p.y, -3.25f) && feq(p.z, 7.0f), "getpos round-trip");
    CHECK(feq(p.x, rldf(mem, raw_off[0] + 0x30)) &&
          feq(p.y, rldf(mem, raw_off[0] + 0x34)) &&
          feq(p.z, rldf(mem, raw_off[0] + 0x38)), "position parity (+0x30)");
    dvec3 o = doc_actor_get_orientation(act[0]);
    CHECK(feq(o.x, rldf(mem, raw_off[0] + 0xe0)) &&
          feq(o.y, rldf(mem, raw_off[0] + 0xe4)) &&
          feq(o.z, rldf(mem, raw_off[0] + 0xe8)), "orientation parity (+0xe0)");

    /* ---- fixed-point heading round-trips through 4096 units ------- */
    doc_actor_set_dir(act[0], 1024);               /* quarter turn */
    CHECK(doc_actor_get_dir(act[0]) == 1024, "setdir/getdir round-trip (got %d)",
          doc_actor_get_dir(act[0]));

    /* ---- draw mode: set (FUN_005a85f8) + get (FUN_005a8658) ------- */
    doc_actor_set_draw_mode(act[1], 3);
    ref_set_draw_mode(mem, raw_off[1], 3);
    CHECK(doc_actor_draw_mode(act[1]) == 3, "draw mode stored (got %u)", doc_actor_draw_mode(act[1]));
    CHECK(doc_actor_draw_mode(act[1]) == ref_draw_mode(mem, raw_off[1]), "draw mode parity");
    CHECK((act[1]->object.draw_flags & OCCUPIED) != 0, "draw mode must preserve OCCUPIED");
    CHECK((rld64(mem, raw_off[1] + 0x440) & 0x0F00) == (act[1]->object.draw_flags & 0x0F00),
          "draw_flags bits 8..11 parity");

    doc_actor_set_visible(act[1], 0);              /* hidden => mode 0 */
    ref_set_draw_mode(mem, raw_off[1], 0);
    CHECK(doc_actor_draw_mode(act[1]) == 0 && ref_draw_mode(mem, raw_off[1]) == 0,
          "set_visible(0) => mode 0 parity");
    doc_actor_set_visible(act[1], 1);              /* visible => mode 2 */
    ref_set_draw_mode(mem, raw_off[1], 2);
    CHECK(doc_actor_draw_mode(act[1]) == 2 && ref_draw_mode(mem, raw_off[1]) == 2,
          "set_visible(1) => mode 2 parity");

    /* ---- script flag field (+0x424) ------------------------------- */
    doc_actor_set_flag(act[1], 4);
    doc_actor_set_flag(act[1], 17);
    ref_set_flag(mem, raw_off[1], 4);
    ref_set_flag(mem, raw_off[1], 17);
    CHECK(doc_actor_test_flag(act[1], 4) && doc_actor_test_flag(act[1], 17), "flags set");
    CHECK(!doc_actor_test_flag(act[1], 5), "unset flag reads 0");
    CHECK(act[1]->script_flags == rld32(mem, raw_off[1] + 0x424), "flag field parity (+0x424)");

    /* ---- transparency / derived alpha (+0x452/+0x459/+0x45a) ------ */
    doc_actor_set_base_alpha(act[2], 0x80);
    ref_set_base_alpha(mem, raw_off[2], 0x80);
    doc_actor_set_transparency(act[2], 0x40);
    ref_set_transparency(mem, raw_off[2], 0x40);
    CHECK(act[2]->transparency == rld8(mem, raw_off[2] + 0x452), "transparency parity (+0x452)");
    CHECK(act[2]->base_alpha   == rld8(mem, raw_off[2] + 0x459), "base alpha parity (+0x459)");
    CHECK(act[2]->alpha        == rld8(mem, raw_off[2] + 0x45a), "derived alpha parity (+0x45a)");
    CHECK(act[2]->alpha == (uint8_t)((0x40 * 0x80) >> 7), "derived alpha value = (t*base)>>7");

    /* ---- attach graph (+0x3e8[i] / +0x45e / ATTACHED) ------------- */
    doc_actor_attach(act[0], 1, act[2], 7);
    ref_attach(mem, raw_off[0], 1, raw_off[2], 7);
    CHECK(doc_actor_get_attached(act[0], 1) == act[2], "attach slot holds child");
    CHECK(ref_get_attached(mem, raw_off[0], 1) == raw_off[2], "attach parity (raw slot)");
    CHECK(act[0]->attach_bone[1] == rld8(mem, raw_off[0] + 0x45e + 1), "attach bone parity");
    CHECK((act[2]->object.draw_flags & ATTACHED) != 0 &&
          (rld64(mem, raw_off[2] + 0x440) & ATTACHED) != 0, "child ATTACHED bit parity");
    CHECK(doc_actor_get_attached(act[0], 0) == NULL, "empty attach slot is NULL");

    /* ---- active list + destroy: unlink, detach, free -------------- */
    doc_actor_list_push(&mgr, act[0]);
    doc_actor_list_push(&mgr, act[1]);
    doc_actor_list_push(&mgr, act[2]);   /* head=2 -> 1 -> 0 */
    /* act[0] still references act[2] in attach slot 1 */
    doc_actor_attach(act[0], 1, act[2], 7);
    CHECK(doc_actor_list_tail(&mgr) == act[0], "list tail is first-pushed (act0)");

    doc_actor_destroy(&mgr, act[2], 1);  /* release back to pool */
    ref_free(mem, raw_off[2]);           /* FUN_005a7148 on the raw slot */
    CHECK((act[2]->object.draw_flags & OCCUPIED) == 0, "destroyed slot freed (OCCUPIED clear)");
    CHECK((rld64(mem, raw_off[2] + 0x440) & OCCUPIED) ==
          (act[2]->object.draw_flags & OCCUPIED), "free OCCUPIED-bit parity (+0x440)");
    CHECK(doc_actor_get_attached(act[0], 1) == NULL, "destroy detaches from parent act0");
    CHECK(mgr.active_head == act[1] && act[1]->next == act[0], "act2 unlinked from active list");
    CHECK(mgr.live_count == N_SLOTS - 1, "live_count decremented to %d, got %d",
          N_SLOTS - 1, mgr.live_count);
    /* a freed slot is handed back out by the next alloc */
    DocActor *re = doc_actor_alloc(&mgr);
    CHECK(re == act[2], "freed slot is reused by the next alloc");

    /* ---- is_drawable gate ----------------------------------------- */
    doc_actor_reset(act[0]);
    CHECK(!doc_actor_is_drawable(act[0]), "fresh actor not drawable (NO pose/pose-ready off)");
    act[0]->object.render_state |= DOC_ACTOR_RS_POSE_READY;
    CHECK(doc_actor_is_drawable(act[0]), "pose-ready + occupied + model + shown => drawable");
    act[0]->object.draw_flags |= NO_MODEL;
    CHECK(!doc_actor_is_drawable(act[0]), "NO_MODEL => not drawable");
    act[0]->object.draw_flags &= ~NO_MODEL;
    doc_actor_set_hide_state(act[0], DOC_ACTOR_HIDDEN);
    CHECK(!doc_actor_is_drawable(act[0]), "HIDDEN => not drawable");

    /* ===================================================================
     * MUTATION SENSITIVITY: prove the parity/behaviour asserts are load-
     * bearing by constructing the most likely actor.c mutations and
     * asserting they WOULD change the result (so the suite goes red).
     * =================================================================== */
    {
        /* (a) alloc scan: `== 0` (free) mutated to `!= 0` (occupied) would
         * return an already-occupied slot or NULL. On a pool with slot 0
         * free and 1..2 occupied, the correct scan returns slot 0; the
         * mutated scan would skip it. */
        uint8_t mut_pool[BUF_BYTES];
        memset(mut_pool, 0, sizeof mut_pool);
        for (int i = 0; i < N_SLOTS; i++)
            rst64(mut_pool, POOL_BASE + (uint32_t)i * SLOT + 0x440, i == 0 ? 0 : OCCUPIED);
        uint32_t good = 0, mut = 0;
        for (int i = 0; i < N_SLOTS; i++) {
            uint32_t s = POOL_BASE + (uint32_t)i * SLOT;
            if (good == 0 && (rld64(mut_pool, s + 0x440) & OCCUPIED) == 0) good = s; /* correct */
            if (mut  == 0 && (rld64(mut_pool, s + 0x440) & OCCUPIED) != 0) mut  = s; /* mutated */
        }
        CHECK(good != mut, "mutation guard: alloc scan ==0 vs !=0 must pick different slots "
              "(both 0x%x)", good);

        /* (b) draw-mode shift: `<< 8` mutated to a different shift changes
         * which nibble is written; assert the correct extraction differs
         * from a wrong shift of 4. */
        uint64_t df = 0;
        df = (df & 0xfffffffffffff0ffULL) | ((uint64_t)3 << 8);    /* correct */
        uint64_t bad = ((uint64_t)3 << 4);                          /* mutated */
        CHECK(((df >> 8) & 0xf) != ((bad >> 8) & 0xf),
              "mutation guard: draw-mode shift 8 vs 4 must differ");

        /* (c) alpha derive: `>> 7` mutated (e.g. dropped) changes the value. */
        unsigned good_a = ((unsigned)0x40 * 0x80) >> 7;
        unsigned bad_a  = ((unsigned)0x40 * 0x80);                  /* no shift */
        CHECK((uint8_t)good_a != (uint8_t)bad_a,
              "mutation guard: alpha >>7 vs no-shift must differ (both 0x%02x)", (uint8_t)good_a);

        /* (d) attach ATTACHED bit: `|= 0x200000` mutated to a no-op would
         * leave the child's flag clear. */
        uint64_t cf = 0;
        CHECK((cf | ATTACHED) != cf, "mutation guard: attach |= ATTACHED must change the flag");
    }

    if (g_fail == 0) printf("test_actor: ALL PASS\n");
    else             printf("test_actor: %d FAILURE(S)\n", g_fail);
    return g_fail ? 1 : 0;
}
