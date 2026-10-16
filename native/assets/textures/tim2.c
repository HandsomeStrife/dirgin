/* tim2.c - PS2 TIM2 family decoder (magic "TIM2"/"TIM3"/"TIM4").
 *
 * Standard, well-documented PS2 texture container. The game stores UI, font,
 * marker, scene and effect textures as this family (e.g. data/etc/marker.rfd,
 * data/effect/effect.rfd). Header layout:
 *
 *   File header (16 bytes):
 *     0x00 char[4] magic "TIMx"
 *     0x04 u8      version
 *     0x05 u8      align/format (0 => 16-byte header, else 128-byte)
 *     0x06 u16     picture count
 *   Picture header (headerSize bytes, >=0x30):
 *     0x00 u32 totalSize   0x04 u32 clutSize   0x08 u32 imageSize
 *     0x0C u16 headerSize   0x0E u16 clutColors
 *     0x10 u8  pictFormat   0x11 u8 mipmapCount 0x12 u8 clutType 0x13 u8 imageColorType
 *     0x14 u16 width        0x16 u16 height
 *     0x18 u64 GsTex0  0x20 u64 GsTex1  0x28 u32 GsRegs  0x2C u32 GsTexClut
 *   Then: image data (imageSize), then CLUT (clutSize).
 *
 * imageColorType: 1=16bpp 2=24bpp 3=32bpp 4=T4 5=T8.
 * clutType low6: 1=16bpp 2=24bpp 3=32bpp; bit7 = CSM2 (else CSM1).
 *
 * This decoder handles the first picture of each payload (the game uses one
 * picture per payload). PSMT8 images use the same GS page swizzle as RST, so
 * dtex_unswizzle8 is applied; paletted images use a CSM1-corrected 32-bit CLUT.
 */
#include "dtex_internal.h"
#include <stdlib.h>

int dtex__decode_tim(const uint8_t *data, size_t size, const char *name,
                     dtex_image *out)
{
    if (size < 0x10) return -1;
    int base = (data[5] == 0) ? 0x10 : 0x80;
    if ((size_t)base + 0x30 > size) return -1;

    uint32_t imageSize  = rd32(data, base + 0x08);
    uint16_t headerSize = rd16(data, base + 0x0C);
    uint16_t clutColors = rd16(data, base + 0x0E);
    uint8_t  clutType   = data[base + 0x12];
    uint8_t  imgType    = data[base + 0x13];
    int      W          = rd16(data, base + 0x14);
    int      H          = rd16(data, base + 0x16);
    if (W <= 0 || H <= 0 || W > 4096 || H > 4096) return -1;
    if (headerSize < 0x30) return -1;

    size_t img_off  = (size_t)base + headerSize;
    size_t clut_off = img_off + imageSize;
    if (img_off > size) return -1;

    memset(out, 0, sizeof(*out));
    out->width = W; out->height = H;
    if (name) { strncpy(out->name, name, sizeof(out->name) - 1); }
    switch (data[3]) { case '2': strcpy(out->source,"TIM2"); break;
                       case '3': strcpy(out->source,"TIM3"); break;
                       case '4': strcpy(out->source,"TIM4"); break;
                       default:  strcpy(out->source,"TIM"); }

    out->rgba = (uint8_t *)malloc((size_t)W * H * 4);
    if (!out->rgba) return -1;

    /* Build the RGBA palette (32-bit CLUT; 16-bit fallback). */
    uint8_t pal[256 * 4];
    int csm1 = (clutType & 0x80) == 0;
    int clutfmt = clutType & 0x3f;
    if (imgType == 4 || imgType == 5) {
        int nc = clutColors ? clutColors : (imgType == 4 ? 16 : 256);
        if (nc > 256) nc = 256;
        /* The CLUT region sits at img_off + imageSize. For 256-colour (PSMT8)
         * palettes the colours are at the region start; for 16-colour (PSMT4)
         * they are at its end, after padding. Detect an all-zero head and shift
         * to the last nc*4 bytes of the payload. */
        if (clutfmt == 3 && clut_off + (size_t)nc * 4 <= size) {
            int head_zero = 1;
            for (size_t b = 0; b < (size_t)nc * 4; b++)
                if (data[clut_off + b]) { head_zero = 0; break; }
            if (head_zero && size >= (size_t)nc * 4)
                clut_off = size - (size_t)nc * 4;
        }
        if (clut_off + (size_t)nc * 4 <= size && clutfmt == 3) {
            dtex_read_clut32(pal, data + clut_off, nc, csm1);
        } else if (clut_off + (size_t)nc * 2 <= size && (clutfmt == 1)) {
            for (int i = 0; i < nc; i++) {
                uint16_t v = rd16(data, clut_off + (size_t)i * 2);
                pal[i*4+0] = (uint8_t)((v & 0x1f) << 3);
                pal[i*4+1] = (uint8_t)(((v >> 5) & 0x1f) << 3);
                pal[i*4+2] = (uint8_t)(((v >> 10) & 0x1f) << 3);
                pal[i*4+3] = (v & 0x8000) ? 255 : 0;
            }
        } else {
            for (int i = 0; i < 256; i++) { pal[i*4]=pal[i*4+1]=pal[i*4+2]=(uint8_t)i; pal[i*4+3]=255; }
        }
        out->clut_colors = nc;
    }

    if (imgType == 5) {                 /* PSMT8 */
        out->psm = PSM_T8;
        if (img_off + (size_t)W * H > size) { dtex_free_image(out); return -1; }
        uint8_t *lin = (uint8_t *)malloc((size_t)W * H);
        if (!lin) { dtex_free_image(out); return -1; }
        if ((W % 16) == 0 && (H % 4) == 0) {
            dtex_unswizzle8(lin, data + img_off, W, H);
            out->deswizzled = 1;
        } else {
            memcpy(lin, data + img_off, (size_t)W * H);
        }
        dtex__emit_paletted(out, lin, pal, 0);
        free(lin);
    } else if (imgType == 4) {          /* PSMT4 (linear) */
        out->psm = PSM_T4;
        if (img_off + (size_t)W * H / 2 > size) { dtex_free_image(out); return -1; }
        dtex__emit_paletted(out, data + img_off, pal, 1);
    } else if (imgType == 3) {          /* RGBA32 */
        out->psm = PSM_CT32;
        if (img_off + (size_t)W * H * 4 > size) { dtex_free_image(out); return -1; }
        dtex__emit_direct(out, data + img_off, PSM_CT32);
    } else if (imgType == 2) {          /* RGB24 */
        out->psm = PSM_CT24;
        if (img_off + (size_t)W * H * 3 > size) { dtex_free_image(out); return -1; }
        dtex__emit_direct(out, data + img_off, PSM_CT24);
    } else if (imgType == 1) {          /* RGBA16 */
        out->psm = PSM_CT16;
        if (img_off + (size_t)W * H * 2 > size) { dtex_free_image(out); return -1; }
        dtex__emit_direct(out, data + img_off, PSM_CT16);
    } else {
        dtex_free_image(out); return -1;
    }
    return 0;
}
