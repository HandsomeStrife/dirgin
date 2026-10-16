/* dtex_internal.h - shared helpers between the decoder translation units. */
#ifndef DTEX_INTERNAL_H
#define DTEX_INTERNAL_H

#include "dtex.h"
#include <string.h>

static inline uint16_t rd16(const uint8_t *p, size_t o) {
    return (uint16_t)(p[o] | (p[o + 1] << 8));
}
static inline uint32_t rd32(const uint8_t *p, size_t o) {
    return (uint32_t)(p[o] | (p[o + 1] << 8) | (p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24));
}

/* bytes-per-pixel * 2, to keep T4 (0.5) integral. From FUN_00441278's switch. */
static inline int psm_bpp2(int psm) {
    switch (psm) {
        case PSM_CT32:            return 8;  /* 4 bpp */
        case PSM_CT24:            return 6;  /* 3 bpp */
        case PSM_CT16: case PSM_CT16S: return 4;  /* 2 bpp */
        case PSM_T8:   case PSM_T8H:   return 2;  /* 1 bpp */
        case PSM_T4:   case PSM_T4HL: case PSM_T4HH: return 1; /* 0.5 bpp */
        default: return 0;
    }
}

/* Internal decoders (payload already unwrapped). Return 0 on success. */
int dtex__decode_tim(const uint8_t *data, size_t size,
                     const char *name, dtex_image *out);
int dtex__decode_rst(const uint8_t *data, size_t size,
                     const char *name, dtex_image *out);

/* Expand one paletted/direct image into out->rgba (already allocated). */
void dtex__emit_paletted(dtex_image *out, const uint8_t *idx_or_packed,
                         const uint8_t *pal_rgba, int is_t4);
void dtex__emit_direct(dtex_image *out, const uint8_t *src, int psm);

#endif
