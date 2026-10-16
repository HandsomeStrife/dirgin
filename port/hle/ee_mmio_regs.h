/* =====================================================================
 * ee_mmio_regs.h  --  EE hardware register address map (WS-K owns this).
 *
 * The recompiler contract (ee_runtime.h §5) says: recompiled code never
 * names a register; it only ever emits ee_sw(ee, addr, v) and the ADDRESS
 * carries the identity.  This header is the single place that decodes those
 * addresses into register banks for the HLE MMIO dispatcher (hle/mmio.c).
 *
 * All addresses are canonical (kseg bits already masked by ee_canon()).
 * Grounded in: the PS2 EE User's Manual + constants proven in the decompiled
 * corpus (annotated).  Only the banks Dirge of Cerberus actually touches are
 * modelled in detail; the rest are tracked as opaque shadow words.
 * =====================================================================*/
#ifndef EE_MMIO_REGS_H
#define EE_MMIO_REGS_H

/* ---- Top-level banks (match ee_runtime.h §5 comment) ---------------- */
#define MMIO_VIF0_BASE   0x10003800u   /* VIF0 FIFO/regs                 */
#define MMIO_VIF1_BASE   0x10003C00u   /* VIF1 FIFO/regs                 */
#define MMIO_GIF_BASE    0x10006000u   /* GIF regs                       */
#define MMIO_VIF_IPU_END 0x10007FFFu

/* ---- DMAC per-channel register files (base + 0x00/0x10/0x20/0x30) ----
 * Proven in FUN_0030b768: VIF1 QWC at 0x10009020, GIF QWC at 0x1000a020,
 * CHCR writes 0x105 (VIF1) / 0x104 (GIF).  Channel layout is uniform:
 *   +0x00 CHCR  +0x10 MADR  +0x20 QWC  +0x30 TADR  +0x40 ASR0  +0x50 ASR1  */
#define DMAC_CH_VIF0     0x10008000u   /* ch0 */
#define DMAC_CH_VIF1     0x10009000u   /* ch1  (display-list transport)  */
#define DMAC_CH_GIF      0x1000A000u   /* ch2  (GIF direct / FUN_00442100)*/
#define DMAC_CH_IPU_FROM 0x1000B000u   /* ch3 */
#define DMAC_CH_IPU_TO   0x1000B400u   /* ch4 */
#define DMAC_CH_SIF0     0x1000C000u   /* ch5  (IOP->EE)                 */
#define DMAC_CH_SIF1     0x1000C400u   /* ch6  (EE->IOP)                 */
#define DMAC_CH_SIF2     0x1000C800u   /* ch7 */
#define DMAC_CH_SPR_FROM 0x1000D000u   /* ch8  (ring append source)      */
#define DMAC_CH_SPR_TO   0x1000D400u   /* ch9 */

#define DMAC_CHAN_MASK   0xFFFFFC00u   /* mask an addr down to its channel base */
#define DMAC_REG_CHCR    0x00u
#define DMAC_REG_MADR    0x10u
#define DMAC_REG_QWC     0x20u
#define DMAC_REG_TADR    0x30u
#define DMAC_REG_ASR0    0x40u
#define DMAC_REG_ASR1    0x50u
#define DMAC_REG_SADR    0x80u          /* SPR channels only              */

/* CHCR bit fields (what the transport writes). */
#define CHCR_DIR         0x001u         /* 0=to mem, 1=from mem           */
#define CHCR_MOD_MASK    0x00Cu         /* 0=normal 1=chain 2=interleave  */
#define CHCR_MOD_CHAIN   0x004u
#define CHCR_ASP_MASK    0x030u
#define CHCR_TTE         0x040u         /* transfer DMAtag               */
#define CHCR_TIE         0x080u
#define CHCR_STR         0x100u         /* START/busy (poll bit)          */

/* ---- DMAC common registers (ch-independent) ------------------------- */
#define DMAC_CTRL        0x1000E000u
#define DMAC_STAT        0x1000E010u
#define DMAC_PCR         0x1000E020u
#define DMAC_SQWC        0x1000E030u
#define DMAC_RBSR        0x1000E040u   /* ring size (FUN_0030b768)        */
#define DMAC_RBOR        0x1000E050u   /* ring base = 0x700000            */
#define DMAC_STADR       0x1000E060u

/* ---- INTC ----------------------------------------------------------- */
#define INTC_STAT        0x1000F000u   /* write-1-to-clear               */
#define INTC_MASK        0x1000F010u   /* write-1-to-toggle              */
/* INTC cause bit numbers (AddIntcHandler $a0). */
enum {
    INTC_GS          = 0,
    INTC_SBUS        = 1,
    INTC_VBLANK_START= 2,   /* frame pacing handler lives here           */
    INTC_VBLANK_END  = 3,
    INTC_VIF0        = 4,
    INTC_VIF1        = 5,
    INTC_VU0         = 6,
    INTC_VU1         = 7,
    INTC_IPU         = 8,
    INTC_TIMER0      = 9,
    INTC_TIMER1      = 10,
};

/* ---- SIF / SBUS registers (FUN_004517f0 sceSifSetReg on 0x1000F2xx) -- */
#define SIF_MSCOM        0x1000F200u
#define SIF_SMCOM        0x1000F210u
#define SIF_MSFLAG       0x1000F220u
#define SIF_SMFLAG       0x1000F230u   /* IOP "ready" flag read by EE     */
#define SIF_CTRL         0x1000F240u
#define SIF_BD6          0x1000F260u

/* ---- GS privileged registers (0x12000000..0x12001FFF) --------------- *
 * Flip path FUN_0040d228 writes PMODE / DISPFB1 / DISPFB2 / DISPLAY1/2.  */
#define GS_PMODE         0x12000000u
#define GS_SMODE1        0x12000010u
#define GS_SMODE2        0x12000020u
#define GS_SRFSH         0x12000030u
#define GS_SYNCH1        0x12000040u
#define GS_SYNCH2        0x12000050u
#define GS_SYNCV         0x12000060u
#define GS_DISPFB1       0x12000070u
#define GS_DISPLAY1      0x12000080u
#define GS_DISPFB2       0x12000090u
#define GS_DISPLAY2      0x120000A0u
#define GS_EXTBUF        0x120000B0u
#define GS_EXTDATA       0x120000C0u
#define GS_EXTWRITE      0x120000D0u
#define GS_BGCOLOR       0x120000E0u
#define GS_CSR           0x12001000u
#define GS_IMR           0x12001010u
#define GS_BUSDIR        0x12001040u
#define GS_SIGLBLID      0x12001080u

static inline int mmio_is_gs_priv(unsigned a){ return a>=0x12000000u && a<=0x12001FFFu; }
static inline int mmio_is_dmac_chan(unsigned a){ return a>=0x10008000u && a<=0x1000DFFFu; }
static inline int mmio_is_dmac_common(unsigned a){ return a>=0x1000E000u && a<=0x1000EFFFu; }
static inline int mmio_is_intc(unsigned a){ return a>=0x1000F000u && a<=0x1000F01Fu; }
static inline int mmio_is_sif(unsigned a){ return a>=0x1000F200u && a<=0x1000F2FFu; }

#endif /* EE_MMIO_REGS_H */
