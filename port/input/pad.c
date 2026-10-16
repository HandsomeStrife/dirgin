/* =====================================================================
 * pad.c  --  DualShock2 pad HLE.  Maintains two ports of logical input and
 * serialises them into the 32-byte libpad buffer the engine reads.
 * ===================================================================== */
#include "pad.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint16_t pressed;              /* OR of pad_button (active HIGH)        */
    uint8_t  lx, ly, rx, ry;       /* 0x80 = centre                         */
    int      connected;
} pad_port;

struct pad_state {
    ee_state *ee;
    pad_port  port[2];
};

pad_state *pad_create(ee_state *ee) {
    pad_state *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->ee = ee;
    for (int i = 0; i < 2; i++) {
        p->port[i].pressed = 0;                 /* nothing pressed (M1)      */
        p->port[i].lx = p->port[i].ly = 0x80;   /* sticks centred            */
        p->port[i].rx = p->port[i].ry = 0x80;
        p->port[i].connected = (i == 0);        /* one pad present by default */
    }
    return p;
}
void pad_destroy(pad_state *p) { free(p); }

void pad_set_buttons(pad_state *p, int port, uint16_t pressed) {
    if (port < 0 || port > 1) return;
    p->port[port].pressed = pressed;
}
void pad_set_sticks(pad_state *p, int port,
                    uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry) {
    if (port < 0 || port > 1) return;
    p->port[port].lx = lx; p->port[port].ly = ly;
    p->port[port].rx = rx; p->port[port].ry = ry;
}

void pad_build_buffer(pad_state *p, int port, uint8_t out[32]) {
    memset(out, 0, 32);
    if (port < 0 || port > 1 || !p->port[port].connected) {
        /* disconnected: buttons read all-released (0xff) */
        out[1] = 0x41; out[2] = 0xff; out[3] = 0xff;
        out[4] = out[5] = out[6] = out[7] = 0x80;
        return;
    }
    pad_port *pp = &p->port[port];
    /* buttons are ACTIVE LOW on hardware: invert the logical mask. */
    uint16_t raw = (uint16_t)~pp->pressed;
    out[0] = 0x00;
    out[1] = 0x79;                 /* DualShock2 analog, 3 words of data    */
    out[2] = (uint8_t)(raw & 0xff);        /* hi group (SELECT..LEFT)       */
    out[3] = (uint8_t)((raw >> 8) & 0xff); /* lo group (L2..SQUARE)         */
    out[4] = pp->rx; out[5] = pp->ry;
    out[6] = pp->lx; out[7] = pp->ly;
    /* pressures 0x08..0x13 left 0 (released); fine for digital reads. */
}

void pad_fill_buffer(pad_state *p, int port, uint32_t dst_ee_addr) {
    uint8_t buf[32];
    pad_build_buffer(p, port, buf);
    uint8_t *dst = ee_host_ptr(p->ee, ee_canon(dst_ee_addr));
    memcpy(dst, buf, 32);
}
