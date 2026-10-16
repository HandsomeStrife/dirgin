/* dtex.h - Dirge of Cerberus (PS2, SLPS-20200) native texture decoder.
 *
 * Decodes the game's TXD/RFD texture containers and the two payload kinds
 * they hold:
 *   - TIM2/TIM3/TIM4  (".tm2") : the standard PS2 TIM2 family.
 *   - RST             (".rst") : the game's custom GS raster format.
 *
 * Everything decodes to straight RGBA8 (R,G,B,A bytes, row-major, top-left
 * origin). This is a clean, portable, dependency-free C11 library meant to
 * feed the from-scratch renderer in native/.
 *
 * Reverse-engineered from the game's own decompiled uploader/reader code
 * (analysis/decomp/): FUN_00441278 (BITBLTBUF/TRXREG build, PSM switch),
 * FUN_004415a0 (GIF send), FUN_0030a188 (RST descriptor parse),
 * FUN_002f55d0 (RST sub-table accessors), FUN_0030ab10 (TEX0 build).
 * See RST-FORMAT.md for the full spec and evidence.
 */
#ifndef DTEX_H
#define DTEX_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- GS pixel storage modes (PSM), as used by the game's PSM switch ---- */
enum {
    PSM_CT32  = 0x00, /* RGBA8888 direct                     */
    PSM_CT24  = 0x01, /* RGB888 direct (24-bit, packed)      */
    PSM_CT16  = 0x02, /* RGBA5551 direct                     */
    PSM_CT16S = 0x0a, /* RGBA5551 direct (signed page)       */
    PSM_T8    = 0x13, /* 8-bit palette index                 */
    PSM_T8H   = 0x1b, /* 8-bit palette index (high)          */
    PSM_T4    = 0x14, /* 4-bit palette index                 */
    PSM_T4HL  = 0x24,
    PSM_T4HH  = 0x2c
};

typedef struct {
    int      width;
    int      height;
    int      psm;            /* one of PSM_* (the read/sample PSM)           */
    int      clut_colors;    /* 0 if non-paletted                           */
    int      deswizzled;     /* 1 if a GS deswizzle was applied             */
    char     name[16];       /* texture name from the container/descriptor  */
    char     source[8];      /* "TIM2"/"TIM3"/"TIM4"/"RST"                   */
    uint8_t *rgba;           /* width*height*4 bytes, caller frees          */
} dtex_image;

/* A decoded set of images (a container may hold several). */
typedef struct {
    dtex_image *items;
    int         count;
} dtex_set;

/* Decode a whole container file (TXD or RFD) in memory. Unwraps every entry
 * and decodes the TIM / RST payloads. Returns 0 on success. Caller frees with
 * dtex_free_set(). Non-texture entries are skipped. */
int dtex_decode_container(const uint8_t *data, size_t size, dtex_set *out);

/* Decode a single raw payload (already unwrapped). `name` may be NULL. */
int dtex_decode_payload(const uint8_t *data, size_t size,
                        const char *name, dtex_image *out);

void dtex_free_image(dtex_image *img);
void dtex_free_set(dtex_set *set);

/* ---- low-level pieces (exposed for testing / reuse) ---- */

/* PS2 PSMT8 GS deswizzle: `src` is w*h swizzled indices, writes w*h linear
 * indices to `dst`. w must be a multiple of 16, h a multiple of 4. This is
 * the canonical GS 8-bit page/column interleave (see RST-FORMAT.md). */
void dtex_unswizzle8(uint8_t *dst, const uint8_t *src, int w, int h);

/* PS2 wide-PSMT4 GS deswizzle: `src` is w*h/2 packed swizzled nibbles (GS
 * transfer order), writes w*h linear indices (one 0..15 value per byte) to
 * `dst`. For RST PSMT4 payloads >=128px wide. Returns 1 on success, 0 if the
 * geometry is unsupported (caller should fall back to a linear copy). See
 * RST-FORMAT.md §5. */
int dtex_unswizzle4(uint8_t *dst, const uint8_t *src, int w, int h);

/* Expand a 32-bit PS2 CLUT (RGBA, alpha 0..0x80) into RGBA8 (alpha 0..255),
 * applying the CSM1 256-entry reorder when colors==256. */
void dtex_read_clut32(uint8_t *out_rgba, const uint8_t *clut,
                      int colors, int csm1);

/* Minimal RGBA8 -> PNG writer (self-contained, no zlib). Returns 0 on ok. */
int dtex_write_png(const char *path, const uint8_t *rgba, int w, int h);

/* ---- container unwrap ---- */
typedef struct {
    char            name[8];
    char            ext[4];   /* "rst", "tm2", ...                 */
    const uint8_t  *data;     /* payload bytes (points into input) */
    size_t          size;
} dtex_entry;

/* Parse a TXD/RFD header; fills up to `max` entries, returns the count,
 * or -1 if the header is not a valid container. */
int dtex_unwrap(const uint8_t *data, size_t size,
                dtex_entry *entries, int max);

#ifdef __cplusplus
}
#endif
#endif /* DTEX_H */
