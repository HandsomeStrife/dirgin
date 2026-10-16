/* decode.c - payload dispatch + shared pixel emitters. */
#include "dtex_internal.h"
#include <stdlib.h>

void dtex__emit_paletted(dtex_image *out, const uint8_t *idx, const uint8_t *pal,
                         int is_t4)
{
    int n = out->width * out->height;
    for (int i = 0; i < n; i++) {
        int c;
        if (is_t4) {
            uint8_t b = idx[i >> 1];
            c = (i & 1) ? (b >> 4) : (b & 0x0f);
        } else {
            c = idx[i];
        }
        out->rgba[i * 4 + 0] = pal[c * 4 + 0];
        out->rgba[i * 4 + 1] = pal[c * 4 + 1];
        out->rgba[i * 4 + 2] = pal[c * 4 + 2];
        out->rgba[i * 4 + 3] = pal[c * 4 + 3];
    }
}

void dtex__emit_direct(dtex_image *out, const uint8_t *src, int psm)
{
    int n = out->width * out->height;
    for (int i = 0; i < n; i++) {
        uint8_t r = 0, g = 0, b = 0, a = 255;
        if (psm == PSM_CT32) {
            r = src[i * 4 + 0]; g = src[i * 4 + 1]; b = src[i * 4 + 2];
            int av = (src[i * 4 + 3] * 255) / 128; a = (uint8_t)(av > 255 ? 255 : av);
        } else if (psm == PSM_CT24) {
            r = src[i * 3 + 0]; g = src[i * 3 + 1]; b = src[i * 3 + 2]; a = 255;
        } else { /* PSM_CT16 / CT16S : RGBA5551 */
            uint16_t v = rd16(src, (size_t)i * 2);
            r = (uint8_t)((v & 0x1f) << 3);
            g = (uint8_t)(((v >> 5) & 0x1f) << 3);
            b = (uint8_t)(((v >> 10) & 0x1f) << 3);
            a = (v & 0x8000) ? 255 : 0;
        }
        out->rgba[i * 4 + 0] = r; out->rgba[i * 4 + 1] = g;
        out->rgba[i * 4 + 2] = b; out->rgba[i * 4 + 3] = a;
    }
}

int dtex_decode_payload(const uint8_t *data, size_t size, const char *name,
                        dtex_image *out)
{
    if (size >= 3 && data[0] == 'T' && data[1] == 'I' && data[2] == 'M')
        return dtex__decode_tim(data, size, name, out);
    return dtex__decode_rst(data, size, name, out);
}

void dtex_free_image(dtex_image *img)
{
    if (img && img->rgba) { free(img->rgba); img->rgba = NULL; }
}

void dtex_free_set(dtex_set *set)
{
    if (!set || !set->items) return;
    for (int i = 0; i < set->count; i++) dtex_free_image(&set->items[i]);
    free(set->items);
    set->items = NULL;
    set->count = 0;
}
