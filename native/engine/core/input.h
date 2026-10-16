/* =====================================================================
 * input.h  --  Reauthored INPUT subsystem: per-frame controller poll,
 * active-low->active-high button decode, and press/release edge detection
 * (purist port, P4).
 *
 * The real engine's per-frame input lives behind the frame loop's
 * input_poll / input_latch hooks (frame_loop.h: FUN_002548d8 / FUN_002548f8).
 * engine-map.md §1: "FUN_002548d8 -> FUN_00254638  INPUT poll (2 pads; SDK
 * pad syms stripped)". This module is the real subsystem that sits behind
 * those two hooks.
 *
 * ---------------------------------------------------------------------
 * WHERE THE GUEST GETS ITS BYTES (the one intentional substitution):
 *
 * On the PS2, the IOP `padman` module DMAs a 32-byte libpad buffer
 * (scePadRead output) into EE RAM each frame; the engine's accessors
 * (FUN_0042b538 digital, FUN_0042b408/4e0 sticks, FUN_00429bd0 extended,
 * FUN_002584b0-family port probe) read it, and the poll FUN_00254638
 * folds the result into game globals. The padman RPC handshake and those
 * SDK accessors are BELOW THE SEAM (their symbols are stripped; the raw
 * buffer decode is not visible as C). The AUTHORITATIVE buffer layout is
 * documented in port/input/pad.h (read-only reference), reproduced here.
 *
 * The PURIST substitution: a clean DocInputBackend supplies the raw 32
 * bytes for a port (from keyboard/gamepad on the real target, or a
 * settable buffer in tests) and accepts the actuator (rumble) state back.
 * This replaces the padman RPC entirely. Everything ABOVE that seam --
 * the buffer decode, the active-low->active-high inversion, and the
 * current/previous edge detection -- is reauthored faithfully here and
 * validated byte-for-byte against a raw-offset transcription of the
 * decomp (tests/test_input.c, DECOMP-METHOD.md §6).
 *
 * ---------------------------------------------------------------------
 * Reauthored from (analysis/decomp/, read-only oracle):
 *   FUN_002548d8  input begin (thin)        -> doc_input_begin (wraps poll)
 *   FUN_00254638  the poll (per-port fold)  -> doc_input_begin
 *   FUN_002553e8  button latch + edges      -> doc_input_update_edges
 *   FUN_00255880  extended-bitset edges      -> (same edge formula, cited)
 *   FUN_00255658/FUN_00255af0  disconnect clear -> zeroed DocPadState
 *   FUN_00258218  stick dead-zone normalize  -> doc_input_stick_normalize
 *   FUN_002548f8  input end/latch (thin)     -> doc_input_latch
 *   FUN_00257998 -> FUN_00258130  actuator out -> doc_input_latch (backend)
 *   FUN_002584b0-family  per-port probe word  -> DocPadState.probe/connected
 * Button-bit names + buffer layout: port/input/pad.h (authoritative).
 * ===================================================================== */
#ifndef DOC_INPUT_H
#define DOC_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The engine polls two controller ports (engine-map.md §1: "2 pads"). The
 * per-port probe table in the decomp is strided by 0xc0 and indexed 0/1
 * (FUN_002584b0: &DAT_004e1920 + port*0xc0). */
#define DOC_INPUT_NUM_PORTS   2

/* The raw libpad (scePadRead) buffer is 32 bytes (port/input/pad.h). */
#define DOC_PAD_BUFFER_SIZE   32

/* Count of DualShock2 per-button pressures carried at +0x08..0x13. */
#define DOC_PAD_NUM_PRESSURES 12

/* --------------------------------------------------------------------- *
 * Digital button bitset (host-friendly, ACTIVE HIGH = pressed).
 *
 * These are the SAME 16 bits the controller reports, but inverted from the
 * buffer's active-LOW encoding (0 = pressed on the wire). The names and bit
 * positions are taken verbatim from port/input/pad.h's `enum pad_button`
 * (the authoritative port reference). The buffer carries them split across
 * two active-low bytes:
 *
 *   +0x02 "hi" byte  -> bits 0..7 : SELECT L3 R3 START  UP RIGHT DOWN LEFT
 *   +0x03 "lo" byte  -> bits 8..15: L2 R2 L1 R1  TRIANGLE CIRCLE CROSS SQUARE
 *
 * so the combined active-low word is  (hi | (lo << 8))  and the pressed
 * (active-high) bitset is its bitwise complement, masked to 16 bits.
 *
 * NOTE on numbering: standard SCE libpad numbers SELECT at 0x0100 and L2 at
 * 0x0001. pad.h deliberately renumbers to a host-friendly layout with
 * SELECT at bit 0; we match pad.h so the two headers agree. The mapping of
 * physical button -> bit is unchanged; only the integer value differs.
 * --------------------------------------------------------------------- */
typedef enum DocPadButton {
    DOC_PAD_SELECT   = 1u << 0,   /* +0x02 & 0x01 */
    DOC_PAD_L3       = 1u << 1,   /* +0x02 & 0x02 */
    DOC_PAD_R3       = 1u << 2,   /* +0x02 & 0x04 */
    DOC_PAD_START    = 1u << 3,   /* +0x02 & 0x08 */
    DOC_PAD_UP       = 1u << 4,   /* +0x02 & 0x10 */
    DOC_PAD_RIGHT    = 1u << 5,   /* +0x02 & 0x20 */
    DOC_PAD_DOWN     = 1u << 6,   /* +0x02 & 0x40 */
    DOC_PAD_LEFT     = 1u << 7,   /* +0x02 & 0x80 */
    DOC_PAD_L2       = 1u << 8,   /* +0x03 & 0x01 */
    DOC_PAD_R2       = 1u << 9,   /* +0x03 & 0x02 */
    DOC_PAD_L1       = 1u << 10,  /* +0x03 & 0x04 */
    DOC_PAD_R1       = 1u << 11,  /* +0x03 & 0x08 */
    DOC_PAD_TRIANGLE = 1u << 12,  /* +0x03 & 0x10 */
    DOC_PAD_CIRCLE   = 1u << 13,  /* +0x03 & 0x20 */
    DOC_PAD_CROSS    = 1u << 14,  /* +0x03 & 0x40 */
    DOC_PAD_SQUARE   = 1u << 15   /* +0x03 & 0x80 */
} DocPadButton;

/* Status/mode byte values at +0x01 (port/input/pad.h). */
#define DOC_PAD_MODE_ANALOG   0x79u   /* DualShock2 analog (sticks+pressures) */
#define DOC_PAD_MODE_DIGITAL  0x41u   /* digital-only */

/* Centre value of an analog stick axis (0x80), raw range 0..255. */
#define DOC_PAD_STICK_CENTRE  0x80

/* DualShock2 pressure channel order as libpad lays them out at +0x08..0x13.
 * This ordering is the STANDARD libpad layout; the decomp's pressure use is
 * below the stripped-accessor seam, so treat these indices as best-known
 * rather than decomp-proven (see "Honest gaps" in the module notes). */
typedef enum DocPadPressure {
    DOC_PAD_PRES_RIGHT = 0, DOC_PAD_PRES_LEFT,  DOC_PAD_PRES_UP,    DOC_PAD_PRES_DOWN,
    DOC_PAD_PRES_TRIANGLE,  DOC_PAD_PRES_CIRCLE, DOC_PAD_PRES_CROSS, DOC_PAD_PRES_SQUARE,
    DOC_PAD_PRES_L1,        DOC_PAD_PRES_R1,     DOC_PAD_PRES_L2,    DOC_PAD_PRES_R2
} DocPadPressure;

/* --------------------------------------------------------------------- *
 * Decoded controller state for one port -- the host-friendly form of one
 * 32-byte libpad buffer (port/input/pad.h offsets in comments).
 * --------------------------------------------------------------------- */
typedef struct DocPadState {
    uint8_t  status;        /* <- +0x01 mode byte (0x79 analog / 0x41 digital) */
    uint16_t buttons;       /* decoded ACTIVE-HIGH bitset (DocPadButton), from
                             *    ~((+0x02) | ((+0x03) << 8)) & 0xffff          */
    uint8_t  rstick_x;      /* <- +0x04 right stick X (0x80 centre) */
    uint8_t  rstick_y;      /* <- +0x05 right stick Y */
    uint8_t  lstick_x;      /* <- +0x06 left  stick X */
    uint8_t  lstick_y;      /* <- +0x07 left  stick Y */
    uint8_t  pressures[DOC_PAD_NUM_PRESSURES]; /* <- +0x08..0x13 (0 = released) */
    int      connected;     /* port probe non-zero (FUN_002584b0 word != 0 ->
                             *    uRam004e120c = 1 in FUN_00254638)             */
} DocPadState;

/* --------------------------------------------------------------------- *
 * Per-port tracked input: current + previous decoded state plus the three
 * edge bitsets the game reads. FUN_00254638 stores exactly this triple into
 * its output group (current @DAT_004e0fe0, pressed @uRam004e0fe3/0fe1,
 * released @uRam004e0fe2).
 * --------------------------------------------------------------------- */
typedef struct DocInputPad {
    DocPadState current;    /* this frame's decoded state  (<- DAT_004e1228 grp) */
    DocPadState previous;   /* last frame's decoded state  (<- DAT_004e122c grp) */

    uint16_t held;          /* buttons down this frame          (== current.buttons) */
    uint16_t pressed;       /* rising edge: current & ~previous  (<- DAT_004e122b)    */
    uint16_t released;      /* falling edge: previous & ~current (<- DAT_004e122a)    */
} DocInputPad;

/* --------------------------------------------------------------------- *
 * The platform backend seam -- the purist replacement for the padman RPC.
 *
 * read_pad fills `out` with the 32-byte libpad buffer for `port` and returns
 * non-zero if a controller is connected on that port (0 => disconnected;
 * `out` is then treated as all-zero / no buttons). set_actuator receives the
 * 6-byte actuator (rumble) state the engine wants applied (small motor byte,
 * large motor byte, rest 0 -- cf. FUN_00258130 writing DAT_004e1a80.. and
 * calling scePadSetActDirect FUN_00444268); it may be NULL.
 * --------------------------------------------------------------------- */
typedef struct DocInputBackend {
    void *ctx;
    int  (*read_pad)    (void *ctx, int port, uint8_t out[DOC_PAD_BUFFER_SIZE]);
    void (*set_actuator)(void *ctx, int port, const uint8_t act[6]);
} DocInputBackend;

/* --------------------------------------------------------------------- *
 * The whole per-frame input state threaded through the frame loop.
 * --------------------------------------------------------------------- */
typedef struct DocInputState {
    DocInputBackend backend;
    DocInputPad     pads[DOC_INPUT_NUM_PORTS];
    uint8_t         actuator[DOC_INPUT_NUM_PORTS][6]; /* requested rumble per port */
} DocInputState;

/* --------------------------------------------------------------------- *
 * Core decode (the clean reauthor of FUN_00254638's buffer fold).
 * --------------------------------------------------------------------- */

/* Convert a raw ACTIVE-LOW 16-bit button word (hi | (lo<<8)) into the
 * ACTIVE-HIGH pressed bitset the game uses. This is the active-low ->
 * active-high inversion called out in the task and in pad.h. */
uint16_t doc_pad_buttons_from_raw(uint16_t active_low_word);

/* Decode one 32-byte libpad buffer into a DocPadState. `connected` is stored
 * as given (the backend's read_pad return). Faithful to the documented
 * offsets in port/input/pad.h; buttons are inverted active-low->active-high. */
void doc_input_decode_pad(const uint8_t raw[DOC_PAD_BUFFER_SIZE],
                          int connected, DocPadState *out);

/* Recompute held/pressed/released from current vs previous (the exact edge
 * formulas of FUN_002553e8 / FUN_00255880):
 *   held     = current
 *   pressed  = current & ~previous     (DAT_004e1228 & ~DAT_004e122c)
 *   released = previous & ~current     (DAT_004e122c & ~DAT_004e1228)   */
void doc_input_update_edges(DocInputPad *pad);

/* Analog stick dead-zone + normalize -- the clean reauthor of FUN_00258218.
 * `dx`,`dy` are signed axis deltas (raw axis - 0x80). Writes normalized
 * floats in roughly [-1,1] (scale 1/81 == 0.012345679) to *ox,*oy and
 * returns 1 if outside the dead zone, else writes 0,0 and returns 0.
 * The 0x30 gate / 0x2f subtract asymmetry is reproduced exactly from the
 * decomp. Uses ordinary IEEE floats (no PS2 float-quirk dependence). */
int doc_input_stick_normalize(int dx, int dy, float *ox, float *oy);

/* --------------------------------------------------------------------- *
 * Per-frame driver (behind the frame_loop input_poll / input_latch hooks).
 * --------------------------------------------------------------------- */

/* Zero all tracked state and install `backend` (NULL => the all-disconnected
 * default, which reports no controllers and no buttons). */
void doc_input_init(DocInputState *in, const DocInputBackend *backend);

/* FUN_002548d8 -> FUN_00254638: begin-of-frame poll. For each port:
 *   previous <- current, then read the backend's 32-byte buffer, decode it
 *   into current (disconnected => zeroed current), then recompute edges.
 * The previous<-current copy happening HERE (not at latch) matches the
 * decomp: FUN_002553e8 saves prev at the top before reading the new state. */
void doc_input_begin(DocInputState *in);

/* FUN_002548f8 -> FUN_00257998 -> FUN_00258130: end-of-frame latch. In the
 * purist port this flushes the requested actuator (rumble) state to the
 * backend (set_actuator). The PS2 scePadSetActDirect handshake it replaces
 * is below the seam; the ordering (latch at frame end) is preserved. */
void doc_input_latch(DocInputState *in);

/* Set the actuator (rumble) request for a port: small (0/1) + large (0..255)
 * motor, staged for the next doc_input_latch. */
void doc_input_set_rumble(DocInputState *in, int port, uint8_t small_on, uint8_t large);

/* --------------------------------------------------------------------- *
 * Convenience queries (what a user "can do", not just field reads).
 * mask is an OR of DocPadButton; returns whether ANY of the mask bits match.
 * --------------------------------------------------------------------- */
int doc_input_held    (const DocInputState *in, int port, uint16_t mask);
int doc_input_pressed (const DocInputState *in, int port, uint16_t mask);
int doc_input_released(const DocInputState *in, int port, uint16_t mask);

/* Default backend: every port disconnected, buffers all-zero. */
DocInputBackend doc_input_backend_disconnected(void);

#ifdef __cplusplus
}
#endif

#endif /* DOC_INPUT_H */
