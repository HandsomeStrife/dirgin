/* swizzle.c - PS2 Graphics Synthesizer deswizzle + CLUT handling.
 *
 * GROUND TRUTH / EVIDENCE
 * -----------------------
 * The game uploads textures to GS VRAM with a host->local BITBLTBUF transfer
 * built in FUN_00441278 (sets BITBLTBUF 0x50 / TRXPOS 0x51 / TRXREG 0x52 /
 * TRXDIR 0x53) and kicked by FUN_004415a0. The qword-count switch there is the
 * standard PS2 PSM table (bytes per pixel): CT32=4 (w*h>>2 qw), CT24=3, CT16=2,
 * T8=1, T4=0.5. A host->local transfer feeds LINEAR raster data; the GS writes
 * it into VRAM in its own page/block/column swizzle. When a texture is uploaded
 * and sampled with the SAME PSM the two swizzles cancel, so the payload is
 * linear. Several of the game's larger 8-bit textures are instead uploaded in a
 * different storage mode than they are sampled, so their payload is stored in
 * the GS PSMT8 page order and must be deswizzled on the CPU here.
 *
 * The 8-bit page/column interleave below is the documented PS2 GS layout (the
 * same tables PCSX2 uses): 128x64-texel pages of 16x16 blocks, each block four
 * 16x4 columns with the odd column-pairs swapped on alternate rows. It is
 * transcribed, not invented, and validated: 512x512 and 256x256 PSMT8 textures
 * come out as clean coherent images only with it applied.
 */
#include "dtex.h"
#include <stdlib.h>

/* Canonical PS2 PSMT8 deswizzle (GS page/block/column interleave). */
void dtex_unswizzle8(uint8_t *dst, const uint8_t *src, int w, int h)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int block_location  = (y & ~0xf) * w + (x & ~0xf) * 2;
            int swap_selector   = (((y + 2) >> 2) & 0x1) * 4;
            int pos_y           = (((y & ~3) >> 1) + (y & 1)) & 0x7;
            int column_location = pos_y * w * 2 + ((x + swap_selector) & 0x7) * 4;
            int byte_num        = ((y >> 1) & 1) + ((x >> 2) & 2);
            dst[y * w + x] = src[block_location + column_location + byte_num];
        }
    }
}

/* ---- Wide PSMT4 deswizzle (RST, >=128px wide) ---------------------------
 *
 * EVIDENCE / MODEL (see RST-FORMAT.md §5). The RST 8-bit payload order is
 * reproduced EXACTLY (bit-for-bit, verified at 128..512px over every size) by:
 * take the file as the linear source of a GS host->local PSMCT32 BITBLTBUF
 * write, then read it back as the sample PSM. For PSMT8 that composition equals
 * `dtex_unswizzle8` above; for PSMT4 it is the function below. So a PSMT4
 * payload is deswizzled by: VRAM nibble address = canonical GS PSMT4 read
 * (columnTable4/blockTable4); that VRAM word (8 nibbles) was written by the
 * CT32 transfer, whose linear source index inverts the canonical GS PSMCT32
 * write (columnTable32/blockTable32). The CT32 transfer region is
 *   Wc = (h >= 128) ? w/2 : h/2      (one GS page is 128px tall for PSMT4;
 *   Hc = (w*h/8) / Wc                 full-page-tall tiles by width, sub-page
 *   DBW(CT32) = Wc,  TBW(read) = w    by height), DBW==Wc.
 * These tables are the canonical PS2 GS layout (transcribed from PCSX2), not
 * invented. Proof: every wide PSMT4 RST decodes to a coherent image (brick
 * walls, metal panels, UI atlases) only with this applied. */

static const int ct4_block[8][4] = {   /* PSMT4 blockTable4 (32x16 blocks) */
    { 0, 2, 8,10},{ 1, 3, 9,11},{ 4, 6,12,14},{ 5, 7,13,15},
    {16,18,24,26},{17,19,25,27},{20,22,28,30},{21,23,29,31}};
static const int ct32_block[4][8] = {  /* PSMCT32 blockTable32 (8x8 blocks) */
    { 0, 1, 4, 5,16,17,20,21},{ 2, 3, 6, 7,18,19,22,23},
    { 8, 9,12,13,24,25,28,29},{10,11,14,15,26,27,30,31}};
static const int ct32_col[8][8] = {    /* PSMCT32 columnTable32 */
    { 0, 1, 4, 5, 8, 9,12,13},{ 2, 3, 6, 7,10,11,14,15},
    {16,17,20,21,24,25,28,29},{18,19,22,23,26,27,30,31},
    {32,33,36,37,40,41,44,45},{34,35,38,39,42,43,46,47},
    {48,49,52,53,56,57,60,61},{50,51,54,55,58,59,62,63}};
static const int ct4_col[16][32] = {   /* PSMT4 columnTable4 (nibble offsets) */
 {0,8,32,40,64,72,96,104,2,10,34,42,66,74,98,106,4,12,36,44,68,76,100,108,6,14,38,46,70,78,102,110},
 {16,24,48,56,80,88,112,120,18,26,50,58,82,90,114,122,20,28,52,60,84,92,116,124,22,30,54,62,86,94,118,126},
 {65,73,97,105,1,9,33,41,67,75,99,107,3,11,35,43,69,77,101,109,5,13,37,45,71,79,103,111,7,15,39,47},
 {81,89,113,121,17,25,49,57,83,91,115,123,19,27,51,59,85,93,117,125,21,29,53,61,87,95,119,127,23,31,55,63},
 {192,200,224,232,128,136,160,168,194,202,226,234,130,138,162,170,196,204,228,236,132,140,164,172,198,206,230,238,134,142,166,174},
 {208,216,240,248,144,152,176,184,210,218,242,250,146,154,178,186,212,220,244,252,148,156,180,188,214,222,246,254,150,158,182,190},
 {129,137,161,169,193,201,225,233,131,139,163,171,195,203,227,235,133,141,165,173,197,205,229,237,135,143,167,175,199,207,231,239},
 {145,153,177,185,209,217,241,249,147,155,179,187,211,219,243,251,149,157,181,189,213,221,245,253,151,159,183,191,215,223,247,255},
 {256,264,288,296,320,328,352,360,258,266,290,298,322,330,354,362,260,268,292,300,324,332,356,364,262,270,294,302,326,334,358,366},
 {272,280,304,312,336,344,368,376,274,282,306,314,338,346,370,378,276,284,308,316,340,348,372,380,278,286,310,318,342,350,374,382},
 {321,329,353,361,257,265,289,297,323,331,355,363,259,267,291,299,325,333,357,365,261,269,293,301,327,335,359,367,263,271,295,303},
 {337,345,369,377,273,281,305,313,339,347,371,379,275,283,307,315,341,349,373,381,277,285,309,317,343,351,375,383,279,287,311,319},
 {448,456,480,488,384,392,416,424,450,458,482,490,386,394,418,426,452,460,484,492,388,396,420,428,454,462,486,494,390,398,422,430},
 {464,472,496,504,400,408,432,440,466,474,498,506,402,410,434,442,468,476,500,508,404,412,436,444,470,478,502,510,406,414,438,446},
 {385,393,417,425,449,457,481,489,387,395,419,427,451,459,483,491,389,397,421,429,453,461,485,493,391,399,423,431,455,463,487,495},
 {401,409,433,441,465,473,497,505,403,411,435,443,467,475,499,507,405,413,437,445,469,477,501,509,407,415,439,447,471,479,503,511}};

/* Canonical GS PSMCT32 word address (dbw in pixels). */
static int gs_addr32(int x, int y, int dbw)
{
    int pw = dbw / 64; if (pw < 1) pw = 1;
    int page = (x / 64) + (y / 32) * pw;
    return page * 2048 + ct32_block[(y % 32) / 8][(x % 64) / 8] * 64
                       + ct32_col[(y % 32) % 8][(x % 64) % 8];
}
/* Canonical GS PSMT4 nibble address (tbw in pixels). */
static int gs_addr4(int x, int y, int tbw)
{
    int pw = (tbw + 127) / 128; if (pw < 1) pw = 1;
    int page = (x / 128) + (y / 128) * pw;
    return page * 16384 + ct4_block[(y % 128) / 16][(x % 128) / 32] * 512
                        + ct4_col[(y % 128) % 16][(x % 128) % 32];
}

/* Deswizzle a wide (>=128px) RST PSMT4 payload. `src` is w*h/2 packed nibbles
 * (GS transfer order); writes w*h linear indices (one 0..15 value per byte) to
 * `dst`. Returns 1 on success, 0 if the geometry is not supported (caller then
 * falls back to a linear copy). */
int dtex_unswizzle4(uint8_t *dst, const uint8_t *src, int w, int h)
{
    if (w < 128 || (w % 32) || (h % 2)) return 0;
    long total_words = ((long)w * h / 2) / 4;      /* file nibbles / 8 */
    int Wc = (h >= 128) ? (w / 2) : (h / 2);
    if (Wc < 1) Wc = 1;
    if (total_words % Wc) return 0;
    int Hc = (int)(total_words / Wc);
    int dbw = Wc;
    int pages = ((w + 127) / 128) * ((h + 127) / 128);
    long invn = (long)pages * 2048;
    int *inv = (int *)malloc(sizeof(int) * (size_t)invn);
    if (!inv) return 0;
    for (long i = 0; i < invn; i++) inv[i] = -1;
    for (int ty = 0; ty < Hc; ty++)
        for (int tx = 0; tx < Wc; tx++) {
            int wd = gs_addr32(tx, ty, dbw);
            if (wd >= 0 && wd < invn) inv[wd] = ty * Wc + tx;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int na = gs_addr4(x, y, w);
            int wd = na >> 3;
            int i = (wd >= 0 && wd < invn) ? inv[wd] : -1;
            if (i < 0) { free(inv); return 0; }   /* unexpected hole: bail */
            long fn = ((long)i << 3) + (na & 7);
            uint8_t byte = src[fn >> 1];
            dst[y * w + x] = (fn & 1) ? (byte >> 4) : (byte & 0x0f);
        }
    free(inv);
    return 1;
}

/* PS2 32-bit CLUT alpha is 0..0x80 (0x80 == fully opaque). */
static inline uint8_t ps2_alpha(uint8_t a)
{
    int v = (a * 255) / 128;
    return (uint8_t)(v > 255 ? 255 : v);
}

/* CSM1 stores a 256-entry palette with, inside every 32-colour block, the two
 * middle groups of 8 swapped. Undo it so palette index N maps to colour N. */
void dtex_read_clut32(uint8_t *out, const uint8_t *clut, int colors, int csm1)
{
    for (int i = 0; i < colors; i++) {
        int s = i;
        if (csm1 && colors == 256) {
            int blk = i & ~0x1f;    /* 32-colour block base */
            int j   = i & 0x1f;
            if      (j >= 8  && j < 16) s = blk + j + 8;  /* 8..15  <- 16..23 */
            else if (j >= 16 && j < 24) s = blk + j - 8;  /* 16..23 <- 8..15  */
        }
        out[i * 4 + 0] = clut[s * 4 + 0];
        out[i * 4 + 1] = clut[s * 4 + 1];
        out[i * 4 + 2] = clut[s * 4 + 2];
        out[i * 4 + 3] = ps2_alpha(clut[s * 4 + 3]);
    }
}
