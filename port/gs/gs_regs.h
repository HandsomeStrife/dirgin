/* gs_regs.h -- Graphics Synthesizer register addresses, GIF descriptors and
 * enumerations for the Dirge of Cerberus PC port (WS-G).
 *
 * Every constant here is grounded in the decompiled corpus
 * (analysis/decomp corpus) and the PS2 GS User's Manual.  Addresses that the
 * game provably emits are annotated with the function that proves it.
 */
#ifndef GS_REGS_H
#define GS_REGS_H

/* ------------------------------------------------------------------ *
 * GS register addresses (the low byte of an A+D entry / DMAtag A+D).  *
 * Evidence column = decompiled function that writes this id.          *
 * ------------------------------------------------------------------ */
enum gs_reg {
    GS_PRIM       = 0x00, /* VU1 output + PRE bit                       */
    GS_RGBAQ      = 0x01, /* FUN_004115b8 (+0xa8 -> 1)                  */
    GS_ST         = 0x02,
    GS_UV         = 0x03,
    GS_XYZF2      = 0x04,
    GS_XYZ2       = 0x05,
    GS_TEX0_1     = 0x06, /* FUN_004115b8 (+0x78 -> 6)                  */
    GS_TEX0_2     = 0x07,
    GS_CLAMP_1    = 0x08,
    GS_CLAMP_2    = 0x09,
    GS_FOG        = 0x0a,
    GS_XYZF3      = 0x0c,
    GS_XYZ3       = 0x0d,
    GS_TEX1_1     = 0x14, /* FUN_004115b8 (+0x98 -> 0x14)               */
    GS_TEX1_2     = 0x15,
    GS_TEX2_1     = 0x16,
    GS_TEX2_2     = 0x17,
    GS_XYOFFSET_1 = 0x18, /* FUN_004408e0 (param_1[5] -> 0x18)          */
    GS_XYOFFSET_2 = 0x19,
    GS_PRMODECONT = 0x1a, /* FUN_004408e0 (param_1[9] -> 0x1a)          */
    GS_PRMODE     = 0x1b,
    GS_TEXCLUT    = 0x1c,
    GS_SCANMSK    = 0x22,
    GS_MIPTBP1_1  = 0x34,
    GS_MIPTBP1_2  = 0x35,
    GS_MIPTBP2_1  = 0x36,
    GS_MIPTBP2_2  = 0x37,
    GS_TEXA       = 0x3b, /* FUN_004115b8 (+0x88 -> 0x3b)               */
    GS_FOGCOL     = 0x3d,
    GS_TEXFLUSH   = 0x3f,
    GS_SCISSOR_1  = 0x40, /* FUN_004408e0 (param_1[7] -> 0x40)          */
    GS_SCISSOR_2  = 0x41,
    GS_ALPHA_1    = 0x42, /* FUN_004115b8 (+0x58 -> 0x42)               */
    GS_ALPHA_2    = 0x43, /* FUN_004115b8 (+0xb8 -> 0x43)               */
    GS_DIMX       = 0x44,
    GS_DTHE       = 0x45, /* FUN_004408e0 (param_1[0xd] -> 0x45)        */
    GS_COLCLAMP   = 0x46, /* FUN_004408e0 (param_1[0xb] -> 0x46)        */
    GS_TEST_1     = 0x47, /* FUN_004408e0 (param_1[0xf] -> 0x47)        */
    GS_TEST_2     = 0x48,
    GS_PABE       = 0x49,
    GS_FBA_1      = 0x4a,
    GS_FBA_2      = 0x4b,
    GS_FRAME_1    = 0x4c, /* FUN_004408e0 (param_1[1] -> 0x4c)          */
    GS_FRAME_2    = 0x4d,
    GS_ZBUF_1     = 0x4e, /* FUN_004408e0 (param_1[3] -> 0x4e)          */
    GS_ZBUF_2     = 0x4f,
    GS_BITBLTBUF  = 0x50, /* FUN_00441278 (param_1[3] -> 0x50)          */
    GS_TRXPOS     = 0x51, /* FUN_00441278 (param_1[5] -> 0x51)          */
    GS_TRXREG     = 0x52, /* FUN_00441278 (param_1[7] -> 0x52)          */
    GS_TRXDIR     = 0x53, /* FUN_00441278 (param_1[9] -> 0x53)          */
    GS_HWREG      = 0x54,
    GS_SIGNAL     = 0x60,
    GS_FINISH     = 0x61, /* FUN_00441460 (param_1[0x16] -> 0x61)       */
    GS_LABEL      = 0x62,
    GS_NOP        = 0x7f
};

/* GIFtag FLG field (bits 58..59). */
enum gif_flg {
    GIF_FLG_PACKED  = 0, /* every renderer uses this (A+D blocks)       */
    GIF_FLG_REGLIST = 1, /* VU1 vertex streams                          */
    GIF_FLG_IMAGE   = 2, /* texture/CLUT upload payload (TRXDIR host->local) */
    GIF_FLG_DISABLE = 3
};

/* PACKED-mode 4-bit register descriptors (NOT the same as reg addresses). */
enum gif_packed_desc {
    GIF_PD_PRIM  = 0x00,
    GIF_PD_RGBAQ = 0x01,
    GIF_PD_ST    = 0x02,
    GIF_PD_UV    = 0x03,
    GIF_PD_XYZF2 = 0x04,
    GIF_PD_XYZ2  = 0x05,
    GIF_PD_TEX0_1= 0x06,
    GIF_PD_FOG   = 0x0a,
    GIF_PD_XYZF3 = 0x0c,
    GIF_PD_XYZ3  = 0x0d,
    GIF_PD_AD    = 0x0e, /* address+data: every env block in the corpus */
    GIF_PD_NOP   = 0x0f
};

/* PRIM primitive type (bits 0..2). */
enum gs_prim_type {
    GS_PRIM_POINT      = 0,
    GS_PRIM_LINE       = 1,
    GS_PRIM_LINE_STRIP = 2,
    GS_PRIM_TRI        = 3,
    GS_PRIM_TRI_STRIP  = 4,
    GS_PRIM_TRI_FAN    = 5,
    GS_PRIM_SPRITE     = 6,
    GS_PRIM_INVALID    = 7
};

/* Pixel storage modes (PSM) the game's transfer/store funcs branch on
 * (FUN_00441278 / FUN_00441720 switch cases). */
enum gs_psm {
    GS_PSMCT32  = 0x00,
    GS_PSMCT24  = 0x01,
    GS_PSMCT16  = 0x02,
    GS_PSMCT16S = 0x0a,
    GS_PSMT8    = 0x13,
    GS_PSMT4    = 0x14,
    GS_PSMT8H   = 0x1b,
    GS_PSMT4HL  = 0x24,
    GS_PSMT4HH  = 0x2c,
    GS_PSMZ32   = 0x30,
    GS_PSMZ24   = 0x31,
    GS_PSMZ16   = 0x32,
    GS_PSMZ16S  = 0x3a
};

#endif /* GS_REGS_H */
