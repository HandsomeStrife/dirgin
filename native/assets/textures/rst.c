/* rst.c - Dirge of Cerberus custom ".rst" raster decoder.
 *
 * REVERSE-ENGINEERING EVIDENCE  (see RST-FORMAT.md for the full write-up)
 * ----------------------------------------------------------------------
 * RST payloads sit inside TXD containers. The game loads an RST blob to EE RAM
 * (DAT_004ee948 = 0xeb0000, set in FUN_002f25d0) and parses it in place:
 *   - FUN_002f55d0(base,type,i): sub-table accessors. type 2 -> image header
 *     base + *(base+0x08) + i*0x20 ; type 3 -> CLUT header base + *(base+0x0c)
 *     + i*0x10. Image count lives at the header's +0x44.
 *   - FUN_0030a188: reads an image header (PSM, dims, data ptr) and a CLUT
 *     header (DBP, data ptr, CPSM) into a GS-upload descriptor.
 *   - FUN_0030aa68 / FUN_0030a5c0: feed those to FUN_00441278 (BITBLTBUF) to
 *     upload image then CLUT.  FUN_0030ab10 builds TEX0 and reads the sample
 *     PSM from the SAME field used for the upload DPSM.
 *
 * On-disk layout recovered empirically and cross-checked against the above:
 *   [optional 16-byte texture name]
 *   top header (0x30 bytes):
 *     +0x04 u32 signature 0x0012FA38 (constant across every RST file)
 *     +0x0c 4   flags (01 01 01 ff typical)
 *   then a sequence of { descriptor(0x40) + data } blocks: the CLUT block
 *   first (for paletted textures), then the image block.
 *   descriptor:  +0x12 u16 PSM   +0x14 u16 width   +0x16 u16 height
 *                +0x18 u16 DBW    +0x1a/+0x1b u8 log2(w)/log2(h)
 *   data (w*h*bpp bytes) immediately follows the 0x40-byte descriptor.
 *
 * CLUT is a 32-bit RGBA table (PS2 alpha 0..0x80), read CSM1 (bit not set in
 * FUN_0030ab10's TEX0), so 256-entry palettes get the CSM1 reorder.
 * PSMT8 image data is stored in GS page order -> dtex_unswizzle8. PSMT4 and the
 * direct PSMCT* formats are linear.  (PSMT4 wider than one GS page uses an
 * additional column swizzle that is not yet cracked; see RST-FORMAT.md.)
 */
#include "dtex_internal.h"
#include <stdlib.h>

#define RST_SIG 0x0012FA38u

typedef struct { int off, psm, w, h; size_t dataoff, nbytes; } rst_desc;

static int rst_base(const uint8_t *p, size_t size)
{
    if (size >= 8  && rd32(p, 4)    == RST_SIG) return 0;
    if (size >= 0x18 && rd32(p, 0x14) == RST_SIG) return 0x10;
    return -1;
}

static int is_clut_psm(int psm) {
    return psm == PSM_CT32 || psm == PSM_CT24 || psm == PSM_CT16 || psm == PSM_CT16S;
}
static int is_idx_psm(int psm) {
    return psm == PSM_T8 || psm == PSM_T8H ||
           psm == PSM_T4 || psm == PSM_T4HL || psm == PSM_T4HH;
}

int dtex__decode_rst(const uint8_t *data, size_t size, const char *name,
                     dtex_image *out)
{
    int base = rst_base(data, size);
    if (base < 0) return -1;

    /* Walk descriptor+data blocks. */
    rst_desc descs[8];
    int nd = 0;
    size_t off = (size_t)base + 0x30;
    while (nd < 8 && off + 0x40 <= size) {
        int psm = rd16(data, off + 0x12);
        int w   = rd16(data, off + 0x14);
        int h   = rd16(data, off + 0x16);
        int bpp2 = psm_bpp2(psm);
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || bpp2 == 0) break;
        size_t dataoff = off + 0x40;
        size_t nbytes  = (size_t)w * h * bpp2 / 2;
        if (dataoff + nbytes > size) break;
        descs[nd].off = (int)off; descs[nd].psm = psm;
        descs[nd].w = w; descs[nd].h = h;
        descs[nd].dataoff = dataoff; descs[nd].nbytes = nbytes;
        nd++;
        off = dataoff + nbytes;
    }
    if (nd == 0) return -1;

    /* Classify CLUT + image descriptors. */
    rst_desc *clut = NULL, *img = NULL;
    for (int i = 0; i < nd; i++) {
        if (is_idx_psm(descs[i].psm)) { img = &descs[i]; }
        else if (is_clut_psm(descs[i].psm) && descs[i].w * descs[i].h <= 256 && !img)
            clut = &descs[i];
    }
    if (!img) { /* direct (non-paletted) image: take the largest direct block */
        for (int i = 0; i < nd; i++)
            if (is_clut_psm(descs[i].psm) &&
                (!img || descs[i].w * descs[i].h > img->w * img->h))
                img = &descs[i];
        clut = NULL;
    }
    if (!img) return -1;

    memset(out, 0, sizeof(*out));
    out->width = img->w; out->height = img->h; out->psm = img->psm;
    strcpy(out->source, "RST");
    if (name) strncpy(out->name, name, sizeof(out->name) - 1);
    out->rgba = (uint8_t *)malloc((size_t)img->w * img->h * 4);
    if (!out->rgba) return -1;

    /* Palette (32-bit RGBA, CSM1). */
    uint8_t pal[256 * 4];
    if (clut) {
        int nc = clut->w * clut->h;
        if (nc > 256) nc = 256;
        out->clut_colors = nc;
        dtex_read_clut32(pal, data + clut->dataoff, nc, 1);
    }

    if (img->psm == PSM_T8 || img->psm == PSM_T8H) {
        uint8_t *lin = (uint8_t *)malloc((size_t)img->w * img->h);
        if (!lin) { dtex_free_image(out); return -1; }
        if ((img->w % 16) == 0 && (img->h % 4) == 0) {
            dtex_unswizzle8(lin, data + img->dataoff, img->w, img->h);
            out->deswizzled = 1;
        } else {
            memcpy(lin, data + img->dataoff, (size_t)img->w * img->h);
        }
        dtex__emit_paletted(out, lin, pal, 0);
        free(lin);
    } else if (is_idx_psm(img->psm)) {              /* PSMT4 */
        /* PSMT4 payloads >=128px wide are stored in GS transfer order and need
         * the wide-PSMT4 deswizzle; narrow (<128) and the T4H* modes are
         * linear. On any unsupported geometry fall back to a linear decode. */
        int done = 0;
        if (img->psm == PSM_T4 && img->w >= 128) {
            uint8_t *lin = (uint8_t *)malloc((size_t)img->w * img->h);
            if (lin && dtex_unswizzle4(lin, data + img->dataoff, img->w, img->h)) {
                dtex__emit_paletted(out, lin, pal, 0);
                out->deswizzled = 1;
                done = 1;
            }
            free(lin);
        }
        if (!done)
            dtex__emit_paletted(out, data + img->dataoff, pal, 1);
    } else {                                        /* direct PSMCT* */
        dtex__emit_direct(out, data + img->dataoff, img->psm);
    }
    return 0;
}
