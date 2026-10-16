/* =====================================================================
 * test_recomp.c -- correctness harness for the WS-R recompiler prototype.
 *
 * Proves two statically-recompiled EE leaf functions compute the same
 * results as an independent reference implementation of their logic:
 *
 *   func_00417940  signed-LEB128 (SLEB128) decoder  -- loads/stores,
 *                  a loop + delay-slot branch, shifts, sign extension.
 *   func_0036c338  linked-list "distance between two chains" counter --
 *                  guest-pointer loads, branch-likely (BNEL) delay slot.
 *
 * The reference versions below are hand-written from the decompiled C
 * (analysis/decomp/), NOT from the recompiler output, so agreement is a
 * real cross-check.
 * ===================================================================== */
#include "ee_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int  ee_machine_init(ee_state *ee);
void ee_machine_free(ee_state *ee);
void func_00417940(ee_state *ee);
void func_0036c338(ee_state *ee);

/* ---- reference SLEB128 (mirrors the decompiled logic exactly) -------- */
static uint32_t ref_sleb128(const uint8_t *p, int *consumed) {
    uint32_t result = 0; int shift = 0; uint8_t b; const uint8_t *s = p;
    do { b = *p++; result |= (uint32_t)(b & 0x7f) << shift; shift += 7; }
    while (b & 0x80);
    if (shift < 32 && (b & 0x40)) result |= (~0u) << shift;
    *consumed = (int)(p - s);
    return result;
}
static int enc_sleb128(int32_t v, uint8_t *out) {
    int n = 0, more = 1;
    while (more) {
        uint8_t b = v & 0x7f; v >>= 7;   /* arithmetic shift */
        if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) more = 0;
        else b |= 0x80;
        out[n++] = b;
    }
    return n;
}

#define IN_ADDR  0x00300000u
#define OUT_ADDR 0x00301000u

static int test_leb128(ee_state *ee) {
    static const int32_t vals[] = {
        0, 1, -1, 2, -2, 63, 64, -64, -65, 127, 128, -128, 300, -300,
        1000000, -1000000, 2147483647, (int32_t)-2147483648, 12345, -98765
    };
    int fails = 0;
    for (size_t k = 0; k < sizeof vals / sizeof vals[0]; ++k) {
        uint8_t enc[8]; int n = enc_sleb128(vals[k], enc);
        memcpy(ee->ram + IN_ADDR, enc, n);
        memset(&ee->r[0], 0, sizeof ee->r);
        ee->r[R_A0].ud[0] = IN_ADDR;   /* param_1: source ptr   */
        ee->r[R_A1].ud[0] = OUT_ADDR;  /* param_2: result ptr   */
        func_00417940(ee);
        uint32_t got; memcpy(&got, ee->ram + OUT_ADDR, 4);
        uint32_t ret = ee->r[R_V0].uw[0];        /* returned advanced ptr */
        int rc; uint32_t ref = ref_sleb128(enc, &rc);
        int ok = (got == ref) && (ret == IN_ADDR + (uint32_t)rc);
        printf("  leb128 v=%-12d enc=%dB -> recomp=0x%08x ref=0x%08x adv=%u/%u  %s\n",
               vals[k], n, got, ref, ret - IN_ADDR, (unsigned)rc, ok ? "OK" : "**FAIL**");
        if (!ok) fails++;
    }
    return fails;
}

/* ---- reference for FUN_0036c338 (chain-distance counter) ------------- */
static int ref_chaincount(ee_state *ee, uint32_t a0, uint32_t a1) {
    int32_t i3; memcpy(&i3, ee->ram + a0 + 0x3c, 4);
    int cnt = 0;
    int32_t i2; memcpy(&i2, ee->ram + a1 + 0x3c, 4);
    while (i3 != i2 && i3 != 0 && i2 != 0) {
        memcpy(&i3, ee->ram + (uint32_t)i3 + 0x3c, 4);
        cnt++;
        memcpy(&i2, ee->ram + (uint32_t)i2 + 0x3c, 4);
    }
    return cnt;
}

#define NODE(i) (0x00310000u + (i) * 0x40u)
static void set_next(ee_state *ee, uint32_t node, uint32_t next) {
    memcpy(ee->ram + node + 0x3c, &next, 4);
}

static int test_chaincount(ee_state *ee) {
    /* Build: node0 -> node1 -> node2 -> node3 -> 0  (a chain)
     * Walk from node0 (a0) and node2 (a1); both advance one step per
     * iteration until they collide or hit 0. */
    for (int i = 0; i < 6; i++) set_next(ee, NODE(i), (i < 5) ? NODE(i + 1) : 0);
    struct { uint32_t a0, a1; } cases[] = {
        { NODE(0), NODE(2) }, { NODE(0), NODE(0) }, { NODE(1), NODE(4) },
        { NODE(0), NODE(5) }, { NODE(3), NODE(1) },
    };
    int fails = 0;
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; ++k) {
        memset(&ee->r[0], 0, sizeof ee->r);
        ee->r[R_A0].ud[0] = cases[k].a0;
        ee->r[R_A1].ud[0] = cases[k].a1;
        func_0036c338(ee);
        int got = ee->r[R_V0].sw[0];
        int ref = ref_chaincount(ee, cases[k].a0, cases[k].a1);
        int ok = got == ref;
        printf("  chaincount(node@%08x,node@%08x) -> recomp=%d ref=%d  %s\n",
               cases[k].a0, cases[k].a1, got, ref, ok ? "OK" : "**FAIL**");
        if (!ok) fails++;
    }
    return fails;
}

int main(void) {
    ee_state ee;
    if (ee_machine_init(&ee) != 0) return 2;
    printf("== func_00417940  (SLEB128 decoder) ==\n");
    int f1 = test_leb128(&ee);
    printf("== func_0036c338  (chain-distance counter) ==\n");
    int f2 = test_chaincount(&ee);
    ee_machine_free(&ee);
    int fails = f1 + f2;
    printf("\n%s  (%d failure%s)\n", fails ? "FAILURES" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
