/* =====================================================================
 * input.c  --  Reauthored INPUT subsystem (see input.h).
 *
 * Faithful reauthor of the per-frame controller poll. The decomp this
 * traces to is quoted inline against each clause so the mapping stays
 * auditable:
 *
 *   FUN_00254638  (the poll)           analysis/decomp/00254638_FUN_00254638.c
 *   FUN_002553e8  (button latch+edges) analysis/decomp/002553e8_FUN_002553e8.c
 *   FUN_00255880  (extended edges)     analysis/decomp/00255880_FUN_00255880.c
 *   FUN_00258218  (stick normalize)    analysis/decomp/00258218_FUN_00258218.c
 *   FUN_00258130  (actuator out)       analysis/decomp/00258130_FUN_00258130.c
 *
 * The raw buffer layout + button-bit names are from port/input/pad.h
 * (read-only, authoritative). Everything from the 32-byte buffer down to
 * the padman RPC is below the seam and is supplied by DocInputBackend.
 * ===================================================================== */
#include "input.h"
#include <string.h>

/* --------------------------------------------------------------------- *
 * Active-low -> active-high button decode.
 *
 * The wire word is active LOW (0 == pressed). pad.h's host bitset is active
 * HIGH (1 == pressed), so the pressed set is the complement, masked to the
 * 16 defined bits. (port/input/pad.h: "the same 16 bits ... but NOT
 * inverted" for the host enum; the buffer bytes themselves are active low.)
 * --------------------------------------------------------------------- */
uint16_t doc_pad_buttons_from_raw(uint16_t active_low_word) {
    return (uint16_t)(~active_low_word & 0xffffu);
}

/* --------------------------------------------------------------------- *
 * Decode one 32-byte libpad buffer (port/input/pad.h offsets).
 *   +0x01 status/mode           +0x04..0x07 sticks (R then L, 0x80 centre)
 *   +0x02 buttons hi (bits 0..7)  +0x08..0x13 pressures
 *   +0x03 buttons lo (bits 8..15)
 * --------------------------------------------------------------------- */
void doc_input_decode_pad(const uint8_t raw[DOC_PAD_BUFFER_SIZE],
                          int connected, DocPadState *out) {
    uint16_t lo_word;

    memset(out, 0, sizeof(*out));
    out->connected = connected ? 1 : 0;
    if (!connected || raw == NULL) {
        /* disconnected => zeroed state == no buttons pressed. Mirrors the
         * disconnect-clear paths FUN_00255658 / FUN_00255af0, which zero the
         * current button word. (See note in doc_input_begin.) */
        return;
    }

    out->status = raw[0x01];

    /* hi byte -> bits 0..7, lo byte -> bits 8..15, then invert active-low. */
    lo_word = (uint16_t)((uint16_t)raw[0x02] | ((uint16_t)raw[0x03] << 8));
    out->buttons = doc_pad_buttons_from_raw(lo_word);

    out->rstick_x = raw[0x04];
    out->rstick_y = raw[0x05];
    out->lstick_x = raw[0x06];
    out->lstick_y = raw[0x07];

    memcpy(out->pressures, raw + 0x08, DOC_PAD_NUM_PRESSURES);
}

/* --------------------------------------------------------------------- *
 * Edge detection -- verbatim from the decomp.
 *
 * FUN_002553e8:
 *   DAT_004e122c = DAT_004e1228;                     // prev = cur (saved first)
 *   ... DAT_004e1228 = <new buttons> ...
 *   DAT_004e122b = DAT_004e1228 & ~DAT_004e122c;     // pressed  = cur & ~prev
 *   DAT_004e122a = DAT_004e122c & ~DAT_004e1228;     // released = prev & ~cur
 * FUN_00255880 (independent witness, extended bitset):
 *   DAT_004e17c2 = DAT_004e17c0 & ~uVar2;            // pressed  = new & ~old
 *   DAT_004e17c4 = uVar2 & ~DAT_004e17c0;            // released = old & ~new
 * --------------------------------------------------------------------- */
void doc_input_update_edges(DocInputPad *pad) {
    uint16_t cur  = pad->current.buttons;
    uint16_t prev = pad->previous.buttons;
    pad->held     = cur;
    pad->pressed  = (uint16_t)(cur & ~prev);
    pad->released = (uint16_t)(prev & ~cur);
}

/* --------------------------------------------------------------------- *
 * Stick dead-zone + normalize -- clean reauthor of FUN_00258218.
 *
 *   if (|dx| < 0x30 && |dy| < 0x30) { *ox = *oy = 0; return 0; }
 *   // per-axis dead-zone subtract (note 0x2f, not 0x30):
 *   x = (dx >=  0x2f) ? dx - 0x2f
 *     : (dx <= -0x2f) ? dx + 0x2f     // decomp: (dx < -0x2e) => dx + 0x2f
 *     : 0;
 *   *ox = x * (1/81); ...; return 1;
 * The 0x30 gate vs 0x2f subtract asymmetry is the original's, kept as-is.
 * --------------------------------------------------------------------- */
int doc_input_stick_normalize(int dx, int dy, float *ox, float *oy) {
    int ix, iy;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;

    if (adx < 0x30 && ady < 0x30) {
        *ox = 0.0f;
        *oy = 0.0f;
        return 0;
    }

    /* FUN_00258218: iVar2 = param_4 - 0x2f; if (param_4 < 0x2f) { iVar2 = 0;
     *               if (param_4 < -0x2e) iVar2 = param_4 + 0x2f; } */
    if (dx >= 0x2f)       ix = dx - 0x2f;
    else if (dx < -0x2e)  ix = dx + 0x2f;
    else                  ix = 0;

    if (dy >= 0x2f)       iy = dy - 0x2f;
    else if (dy < -0x2e)  iy = dy + 0x2f;
    else                  iy = 0;

    *ox = (float)ix * 0.012345679f;   /* 1/81, verbatim from the decomp */
    *oy = (float)iy * 0.012345679f;
    return 1;
}

/* --------------------------------------------------------------------- *
 * Default backend: all ports disconnected.
 * --------------------------------------------------------------------- */
static int def_read_pad(void *ctx, int port, uint8_t out[DOC_PAD_BUFFER_SIZE]) {
    (void)ctx; (void)port;
    memset(out, 0, DOC_PAD_BUFFER_SIZE);
    return 0;   /* disconnected */
}

DocInputBackend doc_input_backend_disconnected(void) {
    DocInputBackend b;
    memset(&b, 0, sizeof(b));
    b.ctx = NULL;
    b.read_pad = def_read_pad;
    b.set_actuator = NULL;
    return b;
}

/* --------------------------------------------------------------------- *
 * Per-frame driver.
 * --------------------------------------------------------------------- */
void doc_input_init(DocInputState *in, const DocInputBackend *backend) {
    memset(in, 0, sizeof(*in));
    in->backend = backend ? *backend : doc_input_backend_disconnected();
    if (in->backend.read_pad == NULL) {
        in->backend.read_pad = def_read_pad;
    }
}

/* FUN_002548d8 -> FUN_00254638: the begin-of-frame poll, per port.
 *
 * The original reads two controller ports into the game globals (engine-map
 * §1: "INPUT poll (2 pads)"). For each port: save previous<-current (the
 * prev save is at the top of FUN_002553e8, before the new read), pull the
 * raw buffer through the backend (the padman-RPC substitution), decode it,
 * and recompute the press/release edges.
 *
 * Divergence (documented, DECOMP-METHOD.md §6 "don't blindly match the
 * oracle"): on disconnect we zero `current` and let the edges fall out
 * normally (pressed=0, released = whatever was held). The original's
 * FUN_00255658 clear leaves the stale pressed/released words untouched on a
 * mid-session unplug; the clean behaviour -- a clean "everything released"
 * edge -- is the harmless direction to be wrong (per the nullable/unknown
 * default) and is NOT part of the oracle-parity claim (which covers the
 * connected decode + edge formulas). */
void doc_input_begin(DocInputState *in) {
    uint8_t raw[DOC_PAD_BUFFER_SIZE];
    int port;

    for (port = 0; port < DOC_INPUT_NUM_PORTS; port++) {
        DocInputPad *pad = &in->pads[port];
        int connected;

        pad->previous = pad->current;                 /* DAT_004e122c = DAT_004e1228 */
        connected = in->backend.read_pad(in->backend.ctx, port, raw);
        doc_input_decode_pad(raw, connected, &pad->current);
        doc_input_update_edges(pad);
    }
}

/* FUN_002548f8 -> FUN_00257998 -> FUN_00258130: end-of-frame latch.
 * Flush the staged actuator (rumble) state to the backend. The scePadSetAct-
 * Direct RPC (FUN_00444268) the original drives is below the seam. */
void doc_input_latch(DocInputState *in) {
    int port;
    if (in->backend.set_actuator == NULL) {
        return;
    }
    for (port = 0; port < DOC_INPUT_NUM_PORTS; port++) {
        in->backend.set_actuator(in->backend.ctx, port, in->actuator[port]);
    }
}

void doc_input_set_rumble(DocInputState *in, int port, uint8_t small_on, uint8_t large) {
    if (port < 0 || port >= DOC_INPUT_NUM_PORTS) {
        return;
    }
    /* FUN_00258130: DAT_004e1a80 = small (bool), DAT_004e1a81 = large, rest 0. */
    memset(in->actuator[port], 0, sizeof(in->actuator[port]));
    in->actuator[port][0] = small_on ? 1 : 0;
    in->actuator[port][1] = large;
}

/* --------------------------------------------------------------------- *
 * Convenience queries.
 * --------------------------------------------------------------------- */
int doc_input_held(const DocInputState *in, int port, uint16_t mask) {
    if (port < 0 || port >= DOC_INPUT_NUM_PORTS) return 0;
    return (in->pads[port].held & mask) != 0;
}
int doc_input_pressed(const DocInputState *in, int port, uint16_t mask) {
    if (port < 0 || port >= DOC_INPUT_NUM_PORTS) return 0;
    return (in->pads[port].pressed & mask) != 0;
}
int doc_input_released(const DocInputState *in, int port, uint16_t mask) {
    if (port < 0 || port >= DOC_INPUT_NUM_PORTS) return 0;
    return (in->pads[port].released & mask) != 0;
}
