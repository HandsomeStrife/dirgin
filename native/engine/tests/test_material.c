/* =====================================================================
 * test_material.c  --  Unit + oracle-parity test for the reauthored
 * material -> texture binding subsystem (scene/material.{h,c}).
 *
 * Two layers, per DECOMP-METHOD.md §6:
 *   1. BEHAVIOUR: the clean API selects the right group/submesh, reads the
 *      right material selector, decodes layer tags into the right
 *      {mode,index,stage} bindings, resolves a tag to the right table slot
 *      (honouring the +0x54 override and the default-texture paths), and
 *      describes a handle's stable identity (slot/w/h/psm/tbw/cpsm/clut).
 *   2. ORACLE PARITY: byte-accurate transcriptions of FUN_002e62b0 (group/
 *      submesh select), FUN_002ebb18 (tag->handle), FUN_002ebc48 (layer
 *      decode) and FUN_0030ab10 (the GS TEX0 packer) read the documented
 *      offsets out of a raw "guest memory" buffer (32-bit buffer-relative
 *      pointers, exactly as the decomp dereferences ints). The same inputs
 *      drive clean and raw; results are asserted identical. For the TEX0
 *      packer, the raw side builds the real 64-bit register and the clean
 *      DESC is diffed against the IDENTITY bitfields extracted from it
 *      (TBW/PSM/TW/TH/CPSM + texel/CLUT presence) -- the transient
 *      TBP0/CBP VRAM pages are deliberately NOT compared (below the seam).
 * ===================================================================== */
#include "material.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ---- raw guest-memory accessors (buffer-relative) -------------------- */
static uint16_t rld16(const uint8_t *m, uint32_t o) { uint16_t v; memcpy(&v, m + o, 2); return v; }
static void     rst16(uint8_t *m, uint32_t o, uint16_t v) { memcpy(m + o, &v, 2); }
static uint32_t rld32(const uint8_t *m, uint32_t o) { uint32_t v; memcpy(&v, m + o, 4); return v; }
static void     rst32(uint8_t *m, uint32_t o, uint32_t v) { memcpy(m + o, &v, 4); }

/* ===================================================================== *
 * Byte-accurate transcriptions of the decompiled originals.
 * ===================================================================== */

/* FUN_002f2a88 */
static int ref_log2(int dim) {
    int n = 0, p = 1;
    if (dim > 1) { do { p <<= 1; n++; } while (p < dim); }
    return n;
}

/* FUN_002e62b0 (single-submesh path). Returns status; fills *grp_out,
 * *sub_out, *sel_out, *entry_out. `model` is a buffer offset (0 == null). */
static int ref_select(const uint8_t *m, uint32_t model, uint32_t word,
                      uint32_t *grp_out, uint32_t *sub_out,
                      uint16_t *sel_out, uint32_t *entry_out) {
    uint32_t group = (word >> 10) & 0x7f;
    uint32_t sub   = word & 0x1ff;
    *grp_out = group; *sub_out = sub; *sel_out = 0; *entry_out = 0;
    if (model == 0) return -1;
    if (word & 0x200) return -4;
    if (!(group < rld16(m, model + 0x40))) return -2;
    uint32_t grp = rld32(m, rld32(m, model + 0x60) + group * 4);
    if (grp == 0) return -2;
    if (!(sub < rld16(m, grp + 6))) return -3;
    uint32_t entry = rld32(m, grp + sub * 4 + 0x20);
    if (entry == 0) return -3;
    *sel_out = rld16(m, grp + 8);
    *entry_out = entry;
    return 0;
}

/* FUN_002ebb18. Returns the handle buffer-offset (0 == default/NULL) and
 * writes the resolved slot (-1 for default). */
static uint32_t ref_resolve(const uint8_t *m, uint32_t ctx, int32_t tag,
                            int32_t *slot_out) {
    if (tag < 0) { *slot_out = -1; return 0; }
    int mode = (int)(((uint32_t)tag & 0xf000u) >> 12);
    if (mode == 8) { *slot_out = -1; return 0; }
    int32_t slot = (int32_t)((uint32_t)tag & 0xffff0fffu);
    uint32_t handle = rld32(m, rld32(m, ctx + 0x24) + (uint32_t)slot * 4);
    uint32_t over = rld32(m, handle + 0x54);
    if (over != 0) handle = over;
    *slot_out = slot;
    return handle;
}

/* Placeholder GS bytes-per-pixel table (DAT_0046db40). Its exact contents
 * are a VRAM-allocation detail: it feeds ONLY the transient CBP page count,
 * which is below the seam and NOT part of the identity parity. A constant
 * is sufficient to exercise the raw packer's arithmetic faithfully. */
static int ref_psm_bytes(uint8_t psm) { (void)psm; return 4; }

/* FUN_0030ab10. Builds the real 64-bit GS TEX0 register the original
 * returns, from a handle buffer-offset and a (transient) TBP0 page. */
static uint64_t ref_tex0(const uint8_t *m, uint32_t h, uint32_t tbp0) {
    uint16_t w  = rld16(m, h + 0x3c);
    uint16_t ht = rld16(m, h + 0x3e);
    uint8_t psm = m[h + 0x33];
    uint8_t tbw = m[h + 0x32];
    uint8_t cpsm = m[h + 0x47];
    int bytes = (int)w * (int)ht * ref_psm_bytes(psm);
    int i1 = bytes >> 4;                 /* bytes >= 0 => no +0xf rounding */
    int pages = i1 >> 8;                 /* i1 >= 0 => no +0xff rounding   */
    uint64_t cbp = (uint64_t)(tbp0 + (uint32_t)pages);
    uint64_t tw = (uint64_t)ref_log2(w);
    uint64_t th = (uint64_t)ref_log2(ht);
    return (uint64_t)tbp0
         | ((uint64_t)tbw  << 0x0e)
         | ((uint64_t)psm  << 0x14)
         | (tw             << 0x1a)
         | (th             << 0x1e)
         | (cbp            << 0x25)
         | 0x400000000ULL
         | ((uint64_t)cpsm << 0x33)
         | 0x4000000000000000ULL;
}

/* FUN_002ebc48 (decode only). Writes {mode,tag,index,stage} like the clean
 * side; returns the count. `mat` is a buffer offset. */
typedef struct { int mode; uint16_t raw_tag; uint16_t index; unsigned stage; } RefLayer;
static int ref_decode_layers(const uint8_t *m, uint32_t mat, RefLayer *out, int max) {
    uint32_t lists = rld32(m, mat + 0x24);
    int n = 0;
    for (int i = 0; i < 12 && n < max; i++) {
        uint32_t list = rld32(m, lists + (uint32_t)i * 4);
        unsigned stage = (i == 0) ? m[mat + 0x0e] : (unsigned)i;
        if (stage == 0) stage = 3;
        if (list == 0 || rld16(m, list) == 0) continue;
        uint16_t count = rld16(m, list);
        for (uint16_t t = 1; t <= count && n < max; t++) {
            uint16_t tag = rld16(m, list + (uint32_t)t * 2);
            out[n].raw_tag = tag; out[n].stage = stage;
            if ((tag & 0xf000u) == 0)      { out[n].mode = DOC_LAYER_MODE_TABLE;  out[n].index = tag; }
            else if ((tag >> 12) == 1)     { out[n].mode = DOC_LAYER_MODE_KEEP;   out[n].index = (uint16_t)(tag & 0x0fffu); }
            else if ((tag >> 12) == 8)     { out[n].mode = DOC_LAYER_MODE_SHARED; out[n].index = (uint16_t)(tag & 0x00ffu); }
            else                           { out[n].mode = DOC_LAYER_MODE_OTHER;  out[n].index = tag; }
            n++;
        }
    }
    return n;
}

/* ===================================================================== *
 * Raw buffer layout. Offsets are arbitrary but fixed; "pointers" stored in
 * the buffer are offsets into the same buffer.
 * ===================================================================== */
enum {
    MODEL_OFF     = 0x0100,
    GROUPS_ARR    = 0x0200,   /* model+0x60 -> this: array of group ptrs   */
    MATS_ARR      = 0x0280,   /* model+0x64 -> this: array of matdesc ptrs */
    GROUP2_OFF    = 0x0300,   /* groups[2]; submesh ptrs inline at +0x20   */
    SUBMESH1_OFF  = 0x03f0,   /* submeshes[1] (a non-null entry)           */
    CTX_OFF       = 0x0400,
    HANDLES_ARR   = 0x0480,   /* ctx+0x24 -> this: array of handle ptrs    */
    HANDLE7_OFF   = 0x0500,   /* handles[7]                                */
    HANDLE9_OFF   = 0x0560,   /* handles[9]: has an override               */
    HANDLE_OVR    = 0x05c0,   /* the override target of handles[9]         */
    MAT_OFF       = 0x0600,   /* materials[3]                              */
    LISTS_ARR     = 0x0680,   /* mat+0x24 -> this: array of 12 list ptrs   */
    LIST0_OFF     = 0x06c0,
    LIST1_OFF     = 0x0700
};

static void put_handle(uint8_t *m, uint32_t off, uint16_t w, uint16_t h,
                       uint8_t psm, uint8_t tbw, uint8_t cpsm,
                       uint32_t texels, uint32_t clut, uint32_t override) {
    rst32(m, off + 0x2c, texels);
    m[off + 0x32] = tbw;
    m[off + 0x33] = psm;
    rst16(m, off + 0x3c, w);
    rst16(m, off + 0x3e, h);
    rst32(m, off + 0x40, clut);
    m[off + 0x47] = cpsm;
    rst32(m, off + 0x54, override);
}

static uint8_t mem[0x1000];

static void build_raw(void) {
    memset(mem, 0, sizeof mem);

    /* model */
    rst16(mem, MODEL_OFF + 0x40, 4);                 /* group_count = 4     */
    rst32(mem, MODEL_OFF + 0x60, GROUPS_ARR);
    rst32(mem, MODEL_OFF + 0x64, MATS_ARR);
    rst32(mem, GROUPS_ARR + 2 * 4, GROUP2_OFF);      /* groups[2]           */
    rst32(mem, MATS_ARR  + 3 * 4, MAT_OFF);          /* materials[3]        */

    /* group 2 */
    rst16(mem, GROUP2_OFF + 0x06, 5);                /* submesh_count = 5   */
    rst16(mem, GROUP2_OFF + 0x08, 3);                /* material_selector=3 */
    /* submesh entries are an INLINE array at grp+0x20 (FUN_002e62b0 reads
     * *(grp + sub*4 + 0x20) directly, not via a pointer indirection). */
    rst32(mem, GROUP2_OFF + 0x20 + 1 * 4, SUBMESH1_OFF);  /* submeshes[1]   */

    /* texture context + handles */
    rst32(mem, CTX_OFF + 0x24, HANDLES_ARR);
    rst32(mem, HANDLES_ARR + 7 * 4, HANDLE7_OFF);
    rst32(mem, HANDLES_ARR + 9 * 4, HANDLE9_OFF);
    put_handle(mem, HANDLE7_OFF, 128, 64, 0x13, 2, 0x00, 0x900, 0x980, 0);
    put_handle(mem, HANDLE9_OFF, 32, 32, 0x00, 1, 0x02, 0xa00, 0, HANDLE_OVR);
    put_handle(mem, HANDLE_OVR, 256, 256, 0x14, 4, 0x01, 0xb00, 0xb80, 0);

    /* material descriptor */
    mem[MAT_OFF + 0x0e] = 0;                          /* stage0 = 0 -> 3    */
    rst32(mem, MAT_OFF + 0x24, LISTS_ARR);
    rst32(mem, LISTS_ARR + 0 * 4, LIST0_OFF);
    rst32(mem, LISTS_ARR + 1 * 4, LIST1_OFF);
    /* list 0: count 2, tags 0x0007 (mode0 slot7), 0x8005 (mode8 id5) */
    rst16(mem, LIST0_OFF + 0, 2);
    rst16(mem, LIST0_OFF + 2, 0x0007);
    rst16(mem, LIST0_OFF + 4, 0x8005);
    /* list 1: count 1, tag 0x1002 (mode1 keep) */
    rst16(mem, LIST1_OFF + 0, 1);
    rst16(mem, LIST1_OFF + 2, 0x1002);
}

/* ---- clean mirror of the same data ----------------------------------- */
static DocSubmesh *const SUBMESH1 = (DocSubmesh *)0x1;   /* opaque non-null */

static DocSubmesh *submeshes[5];
static DocMaterialGroup group2;
static DocMaterialGroup *groups[4];
static DocMaterial material3;
static DocMaterial *materials[4];
static DocMaterialModel model;

static DocTextureHandle h7, h9, hovr;
static DocTextureHandle *handles[10];
static DocTextureContext ctx;

static const uint16_t clist0[] = { 2, 0x0007, 0x8005 };
static const uint16_t clist1[] = { 1, 0x1002 };
static const uint16_t *const clists[12] = { clist0, clist1, 0,0,0,0,0,0,0,0,0,0 };

static void build_clean(void) {
    memset(submeshes, 0, sizeof submeshes);
    submeshes[1] = SUBMESH1;
    group2.submesh_count = 5;
    group2.material_selector = 3;
    group2.submeshes = submeshes;
    memset(groups, 0, sizeof groups);
    groups[2] = &group2;

    material3.stage0 = 0;
    material3.layer_lists = clists;
    memset(materials, 0, sizeof materials);
    materials[3] = &material3;

    model.group_count = 4;
    model.groups = groups;
    model.materials = materials;

    memset(&h7, 0, sizeof h7);
    h7.width = 128; h7.height = 64; h7.psm = 0x13; h7.tbw = 2; h7.cpsm = 0x00;
    h7.texels = (const void *)0x900; h7.clut = (const void *)0x980; h7.override = NULL;
    memset(&hovr, 0, sizeof hovr);
    hovr.width = 256; hovr.height = 256; hovr.psm = 0x14; hovr.tbw = 4; hovr.cpsm = 0x01;
    hovr.texels = (const void *)0xb00; hovr.clut = (const void *)0xb80;
    memset(&h9, 0, sizeof h9);
    h9.width = 32; h9.height = 32; h9.psm = 0x00; h9.tbw = 1; h9.cpsm = 0x02;
    h9.texels = (const void *)0xa00; h9.clut = NULL; h9.override = &hovr;

    memset(handles, 0, sizeof handles);
    handles[7] = &h7; handles[9] = &h9;
    ctx.handles = handles;
    ctx.handle_count = 10;
}

/* diff a clean DESC against the identity fields of a raw TEX0 register */
static void diff_desc_vs_tex0(const DocTextureDesc *d, uint64_t tex0,
                              uint32_t raw_handle, const char *where) {
    uint8_t tbw  = (uint8_t)((tex0 >> 0x0e) & 0x3f);
    uint8_t psm  = (uint8_t)((tex0 >> 0x14) & 0x3f);
    uint8_t tw   = (uint8_t)((tex0 >> 0x1a) & 0x0f);
    uint8_t th   = (uint8_t)((tex0 >> 0x1e) & 0x0f);
    uint8_t cpsm = (uint8_t)((tex0 >> 0x33) & 0x0f);
    CHECK(d->tbw == tbw, "%s: TBW clean=%u tex0=%u", where, d->tbw, tbw);
    CHECK(d->psm == psm, "%s: PSM clean=%u tex0=%u", where, d->psm, psm);
    CHECK(doc_texture_log2_dim(d->width) == tw,  "%s: TW clean=%d tex0=%u", where, doc_texture_log2_dim(d->width), tw);
    CHECK(doc_texture_log2_dim(d->height) == th, "%s: TH clean=%d tex0=%u", where, doc_texture_log2_dim(d->height), th);
    CHECK(d->cpsm == cpsm, "%s: CPSM clean=%u tex0=%u", where, d->cpsm, cpsm);
    CHECK(d->has_texels == (rld32(mem, raw_handle + 0x2c) != 0), "%s: has_texels mismatch", where);
    CHECK(d->has_clut   == (rld32(mem, raw_handle + 0x40) != 0), "%s: has_clut mismatch", where);
}

int main(void) {
    build_raw();
    build_clean();

    /* ---- 1. group/submesh selection (FUN_002e62b0) ------------------- */
    const uint32_t word = (2u << 10) | 1u;   /* group 2, sub 1, no batch   */
    DocGroupSelect sel;
    DocSelectStatus st = doc_material_select_group_submesh(&model, word, &sel);

    uint32_t rg, rs, rentry; uint16_t rsel;
    int rst_ = ref_select(mem, MODEL_OFF, word, &rg, &rs, &rsel, &rentry);

    CHECK(st == DOC_SEL_OK, "select status %d (want 0)", st);
    CHECK(rst_ == 0, "ref select status %d", rst_);
    CHECK(sel.group == rg && sel.group == 2, "group clean=%u raw=%u", sel.group, rg);
    CHECK(sel.sub == rs && sel.sub == 1, "sub clean=%u raw=%u", sel.sub, rs);
    CHECK(sel.material_selector == rsel && rsel == 3,
          "selector clean=%u raw=%u", sel.material_selector, rsel);
    CHECK(sel.submesh == SUBMESH1, "submesh ptr wrong");
    CHECK((rentry != 0) == (sel.submesh != NULL), "submesh presence parity");

    /* error codes: group OOB (-2), sub OOB (-3), batch (-4), no model (-1) */
    DocGroupSelect e;
    CHECK(doc_material_select_group_submesh(&model, (5u << 10) | 0, &e) == DOC_SEL_GROUP_RANGE, "group OOB");
    CHECK(ref_select(mem, MODEL_OFF, (5u << 10) | 0, &rg,&rs,&rsel,&rentry) == -2, "ref group OOB");
    CHECK(doc_material_select_group_submesh(&model, (2u << 10) | 4, &e) == DOC_SEL_SUB_RANGE, "sub present but entry null -> -3");
    CHECK(ref_select(mem, MODEL_OFF, (2u << 10) | 4, &rg,&rs,&rsel,&rentry) == -3, "ref sub null -> -3");
    CHECK(doc_material_select_group_submesh(&model, word | 0x200, &e) == DOC_SEL_BATCH, "batch bit -> -4");
    CHECK(doc_material_select_group_submesh(NULL, word, &e) == DOC_SEL_NO_MODEL, "null model -> -1");

    /* ---- 2. layer-tag decode (FUN_002ebc48) -------------------------- */
    DocLayerBinding lb[32];
    int nlb = doc_material_decode_layers(&material3, lb, 32);
    RefLayer rlb[32];
    int nrlb = ref_decode_layers(mem, MAT_OFF, rlb, 32);
    CHECK(nlb == nrlb && nlb == 3, "layer count clean=%d raw=%d (want 3)", nlb, nrlb);
    for (int i = 0; i < nlb && i < nrlb; i++) {
        CHECK((int)lb[i].mode == rlb[i].mode, "layer %d mode clean=%d raw=%d", i, lb[i].mode, rlb[i].mode);
        CHECK(lb[i].raw_tag == rlb[i].raw_tag, "layer %d tag mismatch", i);
        CHECK(lb[i].index == rlb[i].index, "layer %d index clean=%u raw=%u", i, lb[i].index, rlb[i].index);
        CHECK(lb[i].stage == rlb[i].stage, "layer %d stage clean=%u raw=%u", i, lb[i].stage, rlb[i].stage);
    }
    /* spot-check the decoded semantics explicitly */
    CHECK(lb[0].mode == DOC_LAYER_MODE_TABLE  && lb[0].index == 7 && lb[0].stage == 3, "layer0 wrong");
    CHECK(lb[1].mode == DOC_LAYER_MODE_SHARED && lb[1].index == 5 && lb[1].stage == 3, "layer1 wrong");
    CHECK(lb[2].mode == DOC_LAYER_MODE_KEEP   && lb[2].index == 2 && lb[2].stage == 1, "layer2 wrong");

    /* ---- 3. tag -> handle resolve (FUN_002ebb18) --------------------- */
    /* mode-0 table hit, slot 7 */
    int32_t slot; DocTextureHandle *h = doc_texture_table_resolve(&ctx, 0x0007, &slot);
    int32_t rslot; uint32_t rh = ref_resolve(mem, CTX_OFF, 0x0007, &rslot);
    CHECK(slot == rslot && slot == 7, "resolve slot clean=%d raw=%d", slot, rslot);
    CHECK((h == &h7) == (rh == HANDLE7_OFF) && h == &h7, "resolve handle7 parity");

    /* override: slot 9 -> hovr */
    h = doc_texture_table_resolve(&ctx, 0x0009, &slot);
    rh = ref_resolve(mem, CTX_OFF, 0x0009, &rslot);
    CHECK(slot == 9 && rslot == 9, "override slot");
    CHECK(h == &hovr, "override handle not followed (+0x54)");
    CHECK(rh == HANDLE_OVR, "ref override handle");

    /* mode 8 -> default; negative tag -> default */
    h = doc_texture_table_resolve(&ctx, 0x8005, &slot);
    rh = ref_resolve(mem, CTX_OFF, 0x8005, &rslot);
    CHECK(h == NULL && slot == DOC_TEX_SLOT_DEFAULT, "mode8 -> default");
    CHECK(rh == 0 && rslot == -1, "ref mode8 -> default");
    h = doc_texture_table_resolve(&ctx, -1, &slot);
    CHECK(h == NULL && slot == DOC_TEX_SLOT_DEFAULT, "negative tag -> default");

    /* ---- 4. handle -> texture identity (FUN_0030ab10) ---------------- */
    DocTextureDesc d7 = doc_texture_describe(&h7, 7);
    uint64_t tex0_7 = ref_tex0(mem, HANDLE7_OFF, 0x1000 /*transient TBP0*/);
    CHECK(d7.slot == 7, "desc slot");
    diff_desc_vs_tex0(&d7, tex0_7, HANDLE7_OFF, "handle7");

    DocTextureDesc dov = doc_texture_describe(&hovr, 9);
    uint64_t tex0_ov = ref_tex0(mem, HANDLE_OVR, 0x2000);
    diff_desc_vs_tex0(&dov, tex0_ov, HANDLE_OVR, "override");

    DocTextureDesc ddef = doc_texture_describe(NULL, DOC_TEX_SLOT_DEFAULT);
    CHECK(ddef.slot == DOC_TEX_SLOT_DEFAULT && ddef.width == 0 && ddef.has_texels == 0,
          "default descriptor not zeroed");

    /* ---- 5. end-to-end bridge (select -> material -> layer -> texture) */
    DocTextureDesc bridged;
    DocSelectStatus bst = doc_material_resolve_texture(&model, &ctx, word, &bridged);
    CHECK(bst == DOC_SEL_OK, "bridge status %d", bst);
    /* first table-mode layer is tag 0x0007 -> slot 7 -> h7 identity */
    CHECK(bridged.slot == 7 && bridged.width == 128 && bridged.height == 64 &&
          bridged.psm == 0x13 && bridged.tbw == 2,
          "bridged identity wrong (slot=%d w=%u h=%u psm=%u tbw=%u)",
          bridged.slot, bridged.width, bridged.height, bridged.psm, bridged.tbw);

    if (g_fail == 0) printf("test_material: ALL PASS\n");
    else             printf("test_material: %d FAILURE(S)\n", g_fail);
    return g_fail ? 1 : 0;
}
