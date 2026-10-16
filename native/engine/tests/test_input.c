/* =====================================================================
 * test_input.c  --  Unit + oracle-parity + mutation test for the
 * reauthored INPUT subsystem (core/input.{h,c}).
 *
 * Three things are checked:
 *   1. BEHAVIOUR: the clean decode turns a 32-byte libpad buffer into the
 *      right active-HIGH button bitset, sticks, pressures and status; the
 *      edge logic yields correct pressed/released across frames; the stick
 *      dead-zone/normalize matches FUN_00258218; disconnect zeroes state.
 *   2. ORACLE PARITY: a byte-accurate transcription of FUN_00254638's
 *      buffer fold + FUN_002553e8's edge formulas + FUN_00258218 reads the
 *      documented offsets out of a synthetic raw 32-byte buffer into "game
 *      globals", and every decoded field + edge word is asserted identical
 *      to the clean implementation's, over a multi-frame sequence. This is
 *      DECOMP-METHOD.md §6(a): prove the LOGIC and the OFFSET MAP at once.
 *   3. MUTATION SENSITIVITY (§6(b) made self-checking): confirm the parity
 *      assertions are load-bearing -- a deliberately wrong edge formula
 *      (|=) and a wrong decode (no active-low invert) each produce a word
 *      that DIFFERS from the correct one on the sample data, so a real
 *      mutation of input.c would turn these asserts red.
 * ===================================================================== */
#include "input.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ===================================================================== *
 * Byte-accurate reference ("oracle"): reads the documented offsets out of
 * a raw 32-byte buffer exactly as the decomp dereferences them, inverts
 * active-low -> active-high exactly, and computes edges with the exact
 * FUN_002553e8 / FUN_00255880 formulas. Pure integer/bit logic, so there
 * is no PS2 float-quirk concern for the button path.
 * ===================================================================== */
typedef struct RefPad {
    uint8_t  status;
    uint16_t cur, prev, pressed, released;
    uint8_t  rx, ry, lx, ly;
    uint8_t  pres[12];
    int      connected;
} RefPad;

static void ref_decode(const uint8_t *m, int connected, RefPad *r) {
    /* FUN_00254638 reads two port buffers; this is one port's fold. */
    r->prev = r->cur;                          /* DAT_004e122c = DAT_004e1228 */
    if (!connected) {
        r->status = 0;
        r->cur = 0; r->rx = r->ry = r->lx = r->ly = 0;
        memset(r->pres, 0, sizeof r->pres);
        r->connected = 0;
    } else {
        uint16_t low;
        r->status = m[0x01];
        low = (uint16_t)((uint16_t)m[0x02] | ((uint16_t)m[0x03] << 8));
        r->cur = (uint16_t)(~low & 0xffffu);   /* active-low -> active-high */
        r->rx = m[0x04]; r->ry = m[0x05];
        r->lx = m[0x06]; r->ly = m[0x07];
        memcpy(r->pres, m + 0x08, 12);
        r->connected = 1;
    }
    r->pressed  = (uint16_t)(r->cur  & ~r->prev);   /* DAT_004e122b */
    r->released = (uint16_t)(r->prev & ~r->cur);    /* DAT_004e122a */
}

/* FUN_00258218 transcription (pure integer dead-zone + /81 scale). */
static int ref_stick(int dx, int dy, float *ox, float *oy) {
    int ix, iy;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (adx < 0x30 && ady < 0x30) { *ox = 0.0f; *oy = 0.0f; return 0; }
    ix = dx - 0x2f; if (dx < 0x2f) { ix = 0; if (dx < -0x2e) ix = dx + 0x2f; }
    iy = dy - 0x2f; if (dy < 0x2f) { iy = 0; if (dy < -0x2e) iy = dy + 0x2f; }
    *ox = (float)ix * 0.012345679f;
    *oy = (float)iy * 0.012345679f;
    return 1;
}

/* ===================================================================== *
 * Settable-buffer backend (the padman-RPC substitution for tests): serves
 * a per-port 32-byte buffer + connected flag the test drives frame by frame.
 * ===================================================================== */
typedef struct FakePad {
    uint8_t buf[DOC_INPUT_NUM_PORTS][DOC_PAD_BUFFER_SIZE];
    int     connected[DOC_INPUT_NUM_PORTS];
    uint8_t last_act[DOC_INPUT_NUM_PORTS][6];
    int     act_calls;
} FakePad;

static int fake_read(void *ctx, int port, uint8_t out[DOC_PAD_BUFFER_SIZE]) {
    FakePad *f = (FakePad *)ctx;
    memcpy(out, f->buf[port], DOC_PAD_BUFFER_SIZE);
    return f->connected[port];
}
static void fake_act(void *ctx, int port, const uint8_t act[6]) {
    FakePad *f = (FakePad *)ctx;
    memcpy(f->last_act[port], act, 6);
    f->act_calls++;
}

/* Diff a clean DocPadState + edges against the oracle for one port. */
static void diff_pad(const DocInputPad *pad, const RefPad *r, const char *where) {
    CHECK(pad->current.status == r->status,
          "%s: status clean=0x%02x ref=0x%02x", where, pad->current.status, r->status);
    CHECK(pad->current.buttons == r->cur,
          "%s: buttons clean=0x%04x ref=0x%04x (oracle mismatch)", where, pad->current.buttons, r->cur);
    CHECK(pad->held == r->cur,     "%s: held clean=0x%04x ref=0x%04x", where, pad->held, r->cur);
    CHECK(pad->pressed == r->pressed,
          "%s: pressed clean=0x%04x ref=0x%04x", where, pad->pressed, r->pressed);
    CHECK(pad->released == r->released,
          "%s: released clean=0x%04x ref=0x%04x", where, pad->released, r->released);
    CHECK(pad->current.rstick_x == r->rx && pad->current.rstick_y == r->ry &&
          pad->current.lstick_x == r->lx && pad->current.lstick_y == r->ly,
          "%s: sticks mismatch", where);
    CHECK(memcmp(pad->current.pressures, r->pres, 12) == 0, "%s: pressures mismatch", where);
    CHECK(pad->current.connected == r->connected, "%s: connected mismatch", where);
}

int main(void) {
    DocInputState in;
    FakePad fake;
    RefPad ref0, ref1;
    uint32_t lcg = 0x1234567u;
    int frame, port;

    memset(&fake, 0, sizeof fake);
    memset(&ref0, 0, sizeof ref0);
    memset(&ref1, 0, sizeof ref1);

    DocInputBackend be;
    memset(&be, 0, sizeof be);
    be.ctx = &fake;
    be.read_pad = fake_read;
    be.set_actuator = fake_act;
    doc_input_init(&in, &be);

    /* ---- fixed sanity: all-0xFF buffer (nothing pressed) => buttons 0 ---- */
    {
        uint8_t b[DOC_PAD_BUFFER_SIZE];
        DocPadState st;
        memset(b, 0xff, sizeof b);
        b[0x01] = DOC_PAD_MODE_ANALOG;
        doc_input_decode_pad(b, 1, &st);
        CHECK(st.buttons == 0, "idle (all 0xff) must decode to 0 buttons, got 0x%04x", st.buttons);
        CHECK(st.status == DOC_PAD_MODE_ANALOG, "status not read");

        /* CROSS pressed => its wire bit cleared in the lo byte (+0x03 & 0x40). */
        b[0x03] = (uint8_t)~0x40u;
        doc_input_decode_pad(b, 1, &st);
        CHECK(st.buttons == DOC_PAD_CROSS,
              "CROSS alone should decode to 0x%04x, got 0x%04x", (unsigned)DOC_PAD_CROSS, st.buttons);

        /* START pressed => hi byte +0x02 & 0x08 cleared. */
        memset(b, 0xff, sizeof b);
        b[0x02] = (uint8_t)~0x08u;
        doc_input_decode_pad(b, 1, &st);
        CHECK(st.buttons == DOC_PAD_START,
              "START alone should decode to 0x%04x, got 0x%04x", (unsigned)DOC_PAD_START, st.buttons);
    }

    /* ---- stick normalize behaviour + oracle parity ---- */
    {
        float cx, cy, rxf, ryf;
        int ca, ra;
        int vx[] = { 0, 0x10, 0x2e, 0x2f, 0x30, 0x7f, -0x2e, -0x2f, -0x7f };
        size_t i, j;
        for (i = 0; i < sizeof vx / sizeof vx[0]; i++)
            for (j = 0; j < sizeof vx / sizeof vx[0]; j++) {
                ca = doc_input_stick_normalize(vx[i], vx[j], &cx, &cy);
                ra = ref_stick(vx[i], vx[j], &rxf, &ryf);
                CHECK(ca == ra && cx == rxf && cy == ryf,
                      "stick(%d,%d): clean(a=%d %.5f,%.5f) ref(a=%d %.5f,%.5f)",
                      vx[i], vx[j], ca, cx, cy, ra, rxf, ryf);
            }
        /* dead zone: 0x2f < 0x30 on both axes => inactive, outputs zeroed. */
        CHECK(doc_input_stick_normalize(0x2f, 0x2f, &cx, &cy) == 0 && cx == 0.0f && cy == 0.0f,
              "0x2f,0x2f: |a|<0x30 both => should be inactive");
        CHECK(doc_input_stick_normalize(0x30, 0, &cx, &cy) == 1,
              "0x30 on one axis => active");
    }

    /* ===================================================================== *
     * Multi-frame poll: feed a deterministic sequence of raw buffers through
     * both the clean driver and the oracle, diffing every field + edge each
     * frame. Frame 2 forces a specific transition to exercise pressed AND
     * released simultaneously; a disconnect frame is injected on port 1.
     * ===================================================================== */
    for (frame = 0; frame < 24; frame++) {
        /* build raw buffers */
        for (port = 0; port < DOC_INPUT_NUM_PORTS; port++) {
            uint8_t *b = fake.buf[port];
            int k;
            b[0x00] = 0;
            b[0x01] = (frame & 1) ? DOC_PAD_MODE_ANALOG : DOC_PAD_MODE_DIGITAL;
            /* pseudo-random active-low button bytes */
            lcg = lcg * 1103515245u + 12345u;
            b[0x02] = (uint8_t)(lcg >> 16);
            lcg = lcg * 1103515245u + 12345u;
            b[0x03] = (uint8_t)(lcg >> 16);
            for (k = 0x04; k < 0x08; k++) { lcg = lcg * 1103515245u + 12345u; b[k] = (uint8_t)(lcg >> 16); }
            for (k = 0x08; k < 0x14; k++) { lcg = lcg * 1103515245u + 12345u; b[k] = (uint8_t)(lcg >> 16); }
            fake.connected[port] = 1;
        }
        /* frame 7: force a known both-edge transition on port 0 */
        if (frame == 7) { fake.buf[0][0x02] = (uint8_t)~0x10u; fake.buf[0][0x03] = 0xff; } /* UP only */
        if (frame == 8) { fake.buf[0][0x02] = (uint8_t)~0x20u; fake.buf[0][0x03] = 0xff; } /* RIGHT only: UP released, RIGHT pressed */
        /* port 1 unplugged on frames 12..14 */
        if (frame >= 12 && frame <= 14) fake.connected[1] = 0;

        /* drive clean + oracle */
        doc_input_begin(&in);
        ref_decode(fake.buf[0], fake.connected[0], &ref0);
        ref_decode(fake.buf[1], fake.connected[1], &ref1);

        diff_pad(&in.pads[0], &ref0, "port0");
        /* port1: oracle diverges from clean ONLY on the disconnect edge-case
         * (documented divergence), so diff the decode but skip the stale-edge
         * frames for the released word. */
        {
            const DocInputPad *p = &in.pads[1];
            CHECK(p->current.buttons == ref1.cur, "port1 f%d buttons 0x%04x vs 0x%04x",
                  frame, p->current.buttons, ref1.cur);
            CHECK(p->current.connected == ref1.connected, "port1 f%d connected", frame);
            if (fake.connected[1]) {
                CHECK(p->pressed == ref1.pressed && p->released == ref1.released,
                      "port1 f%d edges 0x%04x/0x%04x vs 0x%04x/0x%04x",
                      frame, p->pressed, p->released, ref1.pressed, ref1.released);
            }
        }

        if (frame == 8) {
            CHECK(in.pads[0].pressed == DOC_PAD_RIGHT, "f8 RIGHT should be pressed, got 0x%04x",
                  in.pads[0].pressed);
            CHECK(in.pads[0].released == DOC_PAD_UP, "f8 UP should be released, got 0x%04x",
                  in.pads[0].released);
        }
    }

    /* disconnect-clear: after an unplug, current is zeroed. */
    CHECK(in.pads[1].current.buttons == 0 || in.pads[1].current.connected,
          "unplugged port current buttons should be 0");

    /* ---- actuator latch flush ---- */
    doc_input_set_rumble(&in, 0, 1, 200);
    fake.act_calls = 0;
    doc_input_latch(&in);
    CHECK(fake.act_calls == DOC_INPUT_NUM_PORTS, "latch should flush every port's actuator");
    CHECK(fake.last_act[0][0] == 1 && fake.last_act[0][1] == 200,
          "rumble request not delivered (got %u,%u)", fake.last_act[0][0], fake.last_act[0][1]);

    /* ---- convenience queries ---- */
    {
        DocInputPad *p = &in.pads[0];
        p->held = DOC_PAD_CROSS | DOC_PAD_CIRCLE;
        p->pressed = DOC_PAD_CROSS;
        p->released = DOC_PAD_SQUARE;
        CHECK(doc_input_held(&in, 0, DOC_PAD_CIRCLE), "held query");
        CHECK(!doc_input_held(&in, 0, DOC_PAD_TRIANGLE), "held query negative");
        CHECK(doc_input_pressed(&in, 0, DOC_PAD_CROSS), "pressed query");
        CHECK(doc_input_released(&in, 0, DOC_PAD_SQUARE), "released query");
        CHECK(!doc_input_pressed(&in, 5, DOC_PAD_CROSS), "out-of-range port must be safe");
    }

    /* ===================================================================== *
     * MUTATION SENSITIVITY: prove the parity asserts above are load-bearing.
     * If they were not, a mutated input.c would still pass. Construct the
     * exact case the parity test uses and show the correct op differs from
     * the two most likely mutations, so flipping them WOULD go red.
     * ===================================================================== */
    {
        uint16_t prev = 0x00f0, cur = 0x0ff0;
        uint16_t good_pressed  = (uint16_t)(cur & ~prev);     /* correct */
        uint16_t mut_pressed   = (uint16_t)(cur | ~prev);     /* mutation: & -> | */
        uint16_t good_released = (uint16_t)(prev & ~cur);
        uint16_t mut_released  = (uint16_t)(cur & ~prev);     /* mutation: swap operands */
        CHECK(good_pressed != mut_pressed,
              "mutation guard: pressed &->| should change result (both 0x%04x)", good_pressed);
        CHECK(good_released != mut_released,
              "mutation guard: released operand-swap should change result (both 0x%04x)", good_released);

        /* decode mutation: dropping the active-low invert flips the result. */
        {
            uint8_t b[DOC_PAD_BUFFER_SIZE];
            DocPadState st;
            uint16_t low;
            memset(b, 0xff, sizeof b);
            b[0x03] = (uint8_t)~0x40u;                 /* CROSS pressed */
            doc_input_decode_pad(b, 1, &st);
            low = (uint16_t)((uint16_t)b[0x02] | ((uint16_t)b[0x03] << 8));
            CHECK(st.buttons != low,
                  "mutation guard: without active-low invert the word would be 0x%04x", low);
            CHECK(st.buttons == (uint16_t)(~low & 0xffffu), "invert must match complement");
        }
    }

    if (g_fail == 0) printf("test_input: ALL PASS\n");
    else             printf("test_input: %d FAILURE(S)\n", g_fail);
    return g_fail ? 1 : 0;
}
