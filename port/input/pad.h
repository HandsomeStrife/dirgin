/* =====================================================================
 * pad.h  --  controller (SCPH DualShock2) HLE for the Dirge of Cerberus
 * port (WS-I).  The retail game reads pad state that the IOP padman module
 * DMAs into an EE-side buffer; the per-frame input poll FUN_00254638 then
 * consumes it.  We maintain that buffer natively and fill it from a
 * keyboard/gamepad (or, for M1, from an injected "no buttons" default).
 *
 * BUFFER FORMAT (libpad scePadRead output, 32 bytes, 2 ports):
 *   +0x00  u8   reserved (0)
 *   +0x01  u8   status/mode  (0x79 = DualShock2 analog, 0x41 = digital)
 *   +0x02  u8   buttons hi   (active LOW: 0=pressed)
 *                 0x01 SELECT 0x02 L3 0x04 R3 0x08 START
 *                 0x10 UP     0x20 RIGHT 0x40 DOWN 0x80 LEFT
 *   +0x03  u8   buttons lo   (active LOW)
 *                 0x01 L2 0x02 R2 0x04 L1 0x08 R1
 *                 0x10 TRIANGLE 0x20 CIRCLE 0x40 CROSS 0x80 SQUARE
 *   +0x04  u8   right stick X  (0x80 = centre)
 *   +0x05  u8   right stick Y
 *   +0x06  u8   left  stick X
 *   +0x07  u8   left  stick Y
 *   +0x08..0x13 button pressures (DualShock2; 0 = released)
 *   +0x14..0x1f reserved (0)
 * ===================================================================== */
#ifndef DIRGE_PAD_H
#define DIRGE_PAD_H

#include <stdint.h>
#include "ee_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Logical button bitmask (host-friendly; active HIGH = pressed).  These are
 * the same 16 bits the engine sees, in (hi<<8)|lo order, but NOT inverted. */
enum pad_button {
    PAD_SELECT   = 1u << 0,
    PAD_L3       = 1u << 1,
    PAD_R3       = 1u << 2,
    PAD_START    = 1u << 3,
    PAD_UP       = 1u << 4,
    PAD_RIGHT    = 1u << 5,
    PAD_DOWN     = 1u << 6,
    PAD_LEFT     = 1u << 7,
    PAD_L2       = 1u << 8,
    PAD_R2       = 1u << 9,
    PAD_L1       = 1u << 10,
    PAD_R1       = 1u << 11,
    PAD_TRIANGLE = 1u << 12,
    PAD_CIRCLE   = 1u << 13,
    PAD_CROSS    = 1u << 14,
    PAD_SQUARE   = 1u << 15,
};

typedef struct pad_state pad_state;

pad_state *pad_create(ee_state *ee);
void       pad_destroy(pad_state *p);

/* Inject host input for a port (0 or 1).  `pressed` is an OR of pad_button.
 * Stick axes are 0..255, 0x80 = centre. */
void pad_set_buttons(pad_state *p, int port, uint16_t pressed);
void pad_set_sticks (pad_state *p, int port,
                     uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);

/* Write the 32-byte libpad buffer for `port` into guest RAM at dst_ee_addr.
 * This is the injection point the scePadRead HLE / padman RPC calls each
 * frame (the engine's FUN_00254638 then reads the result). */
void pad_fill_buffer(pad_state *p, int port, uint32_t dst_ee_addr);

/* Introspection for tests: build the 32-byte buffer into `out`. */
void pad_build_buffer(pad_state *p, int port, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif /* DIRGE_PAD_H */
