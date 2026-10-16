/* =====================================================================
 * oracle_bones.c  --  drive the recompiled MOD character path on km01vnw
 * (evm000 / container 0c064400.bin) to try to dump the per-bone 4x4 bind
 * matrices the skin path reads at *(ctx+0x20)+boneIndex*0x40.
 *
 * This build links REAL bodies for the .kel_draw_section overlay (the MOD
 * character/skin/skeleton subsystem), which the stock recompiler left as
 * empty stubs (see README.md / Makefile).  So the oracle can actually run
 * FUN_005aecf8 (MOD parse/relocate) etc.
 *
 * It runs two phases and writes an HONEST status to out/km01vnw_bones.json:
 *   Phase 1 -- parse+relocate the MOD payload with the real FUN_005aecf8 and
 *              VERIFY the relocation took effect (proof the oracle executes
 *              real character-subsystem code).
 *   Phase 2 -- attempt the bone-palette builder FUN_005b0d30 and report the
 *              precise blocker (the palette is built from a RUNTIME model
 *              instance that the parse does NOT construct; km01vnw has no
 *              skeleton chunk to build it from).
 * No matrices are fabricated: if real ones cannot be produced, the JSON says
 * so and why.
 * ===================================================================== */
#include "oracle.h"
#include <stdio.h>
#include <string.h>

#define CONTAINER "/srv/games/dirge-port/extract/_KEL/noPath/0c064400.bin"
#define MOD_PAYLOAD_OFF 0x60u     /* "km01vnw"/"mod" entry @0x50, data @entry+0x10 */

#define FUN_MOD_PARSE   0x005aecf8u
#define FUN_PALETTE     0x005b0d30u

static uint32_t fbits(float f){ uint32_t b; memcpy(&b,&f,4); return b; }

int main(void){
    printf("== oracle: km01vnw MOD character bone-matrix dump attempt ==\n");
    oracle *o = oracle_create(NULL, /*want_io=*/0);
    if (!o){ printf("oracle_create failed\n"); return 1; }

    /* ---- load the real character container into guest RAM --------------- */
    size_t csz=0;
    uint32_t cbase = oracle_load_file(o, CONTAINER, &csz);
    if (!cbase){ printf("could not load %s\n", CONTAINER); oracle_destroy(o); return 1; }
    uint32_t payload = cbase + MOD_PAYLOAD_OFF;
    printf("[load] container @%08x size=%zu ; MOD payload @%08x\n", cbase, csz, payload);

    uint32_t magic = oracle_lw(o, payload);        /* "MOD\0" = 0x00444f4d */
    uint32_t ver   = oracle_lw(o, payload + 4);
    uint32_t count = oracle_lw(o, payload + 0x10);
    uint32_t arr0  = oracle_lw(o, payload + 0x14);  /* entry-offset array base (file-rel) */
    printf("[mod ] magic=%08x ('%c%c%c') ver=%u topCount=%u entryArrOff=%08x\n",
           magic, magic&0xff,(magic>>8)&0xff,(magic>>16)&0xff, ver, count, arr0);
    int magic_ok = ((magic & 0xffffff)==0x444f4d);  /* 'M''O''D' */

    /* snapshot a few of the entry-array words that the parse will relocate */
    uint32_t e_before[4];
    for (int i=0;i<4 && (uint32_t)i<count+2;i++) e_before[i]=oracle_lw(o, payload+arr0+(uint32_t)i*4);

    /* ---- PHASE 1: real FUN_005aecf8(payload, 0) parse + relocate -------- */
    oracle_set_arg(o,0,payload);     /* $a0 = chunk data (payload)   */
    oracle_set_arg(o,1,0);           /* $a1 = reloc base 0 -> use payload */
    int rc = oracle_call(o, FUN_MOD_PARSE);
    uint32_t arr0_after = oracle_lw(o, payload + 0x14);
    printf("[parse] FUN_005aecf8 rc=%d  entryArrOff %08x -> %08x (delta=%08x, payloadBase=%08x)\n",
           rc, arr0, arr0_after, arr0_after-arr0, payload);
    int reloc_ok = (rc==0) && (arr0_after == arr0 + payload);
    uint32_t e_after[4];
    for (int i=0;i<4 && (uint32_t)i<count+2;i++) e_after[i]=oracle_lw(o, arr0_after+(uint32_t)i*4);
    for (int i=0;i<4 && (uint32_t)i<count+2;i++)
        printf("        entry[%d]: %08x -> %08x\n", i, e_before[i], e_after[i]);
    printf("[parse] %s -- the oracle executed REAL .kel_draw_section code "
           "(FUN_005aecf8), not a stub.\n", reloc_ok?"RELOCATION VERIFIED":"relocation NOT as expected");

    /* ---- PHASE 2: the bone palette -------------------------------------
     * FUN_005b0d30(blend=$f12, frame=$a1, model=$a0) builds the palette from a
     * RUNTIME model-instance object: bone count *(short)(model+0x10), matrix
     * base *(model+0x1c), draw index *(short)(model+0xe), and a per-bone data
     * POINTER ARRAY at *(model+0x24).  The MOD parse does NOT build that
     * object (it only relocates the file's entry array).  Show that the parsed
     * payload is NOT a valid model instance, then make a GUARDED attempt. */
    int16_t  m_bonecount = (int16_t)oracle_lw(o, payload+0x10);   /* reinterpret */
    uint32_t m_matbase   = oracle_lw(o, payload+0x1c);
    uint32_t m_bonearr   = oracle_lw(o, payload+0x24);
    printf("[pose ] IF payload were the model instance: boneCount(+0x10)=%d "
           "matBase(+0x1c)=%08x boneArr(+0x24)=%08x\n",
           m_bonecount, m_matbase, m_bonearr);
    printf("[pose ] -> these are MOD file bytes, not a bone count / matrix base / "
           "pointer array (out of the 32MB map or nonsensical) => not a model instance.\n");

    oracle_ee(o)->f[12] = fbits(1.0f);     /* blend = 1.0 */
    oracle_ee(o)->f[13] = fbits(1.0f);
    oracle_set_arg(o,0,payload);           /* $a0 = (bogus) model */
    oracle_set_arg(o,1,0);                 /* $a1 = frame */
    int prc = oracle_call(o, FUN_PALETTE);
    if (prc==-2)
        printf("[pose ] FUN_005b0d30 FAULTED as expected: %s\n", oracle_last_fault(o));
    else
        printf("[pose ] FUN_005b0d30 returned rc=%d (ran over garbage; no valid palette "
               "was built -- no model instance exists to fill).\n", prc);

    /* ---- write the honest status JSON ---------------------------------- */
    FILE *j = fopen("out/km01vnw_bones.json","w");
    if (j){
        fprintf(j,
"{\n"
"  \"model\": \"km01vnw\",\n"
"  \"container\": \"0c064400.bin\",\n"
"  \"mod_payload_guest_addr\": \"0x%08x\",\n"
"  \"mod_magic_ok\": %s,\n"
"  \"mod_version\": %u,\n"
"  \"mod_top_count\": %u,\n"
"  \"status\": \"BLOCKED\",\n"
"  \"bones_dumped\": 0,\n"
"  \"matrices\": {},\n"
"  \"phase1_parse_reloc\": {\n"
"    \"function\": \"FUN_005aecf8\",\n"
"    \"executed_real_body\": true,\n"
"    \"relocation_verified\": %s,\n"
"    \"note\": \"oracle runs REAL .kel_draw_section code (stock recomp had it stubbed)\"\n"
"  },\n"
"  \"phase2_palette\": {\n"
"    \"function\": \"FUN_005b0d30\",\n"
"    \"result\": \"%s\",\n"
"    \"reason\": \"The bone palette is built from a RUNTIME model-instance object (fields +0x10 boneCount, +0x1c matrix base, +0x20 draw array, +0x24 per-bone pointer array). That object is NOT produced by the MOD parse; it is allocated + bound by the character-instantiation chain (heap allocator FUN_00407d38/FUN_005a6d00 -> FUN_005a8850 -> FUN_005a86f0/FUN_005a87a0 palette bind -> pose evaluator). It additionally needs 'dks'/'dat' skeleton chunks, which km01vnw's container does NOT contain (only chrid.chi + mod parts). So a rest/bind-pose palette is not reachable by a narrow call; the full instantiation + skeleton subsystem must run, and the skeleton data is absent for this model.\"\n"
"  },\n"
"  \"next_step\": \"Run the character-instantiation chain up to FUN_005a8850 against a character that carries a 'dks'+'dat' skeleton, OR statically bake the 'dat'/'dks' per-bone local transforms (set by FUN_005b27c8) without the animator. The skin math itself is known (port/vu/disasm/vu0_skin_micro.txt).\"\n"
"}\n",
        payload, magic_ok?"true":"false", ver, count,
        reloc_ok?"true":"false",
        (prc==-2)?"faulted (no model instance)":"ran over garbage (no valid palette)");
        fclose(j);
        printf("[out ] wrote out/km01vnw_bones.json (status: BLOCKED, 0 bones -- honest, no fabricated matrices)\n");
    }

    printf("== summary: parse %s ; palette BLOCKED (needs instantiation chain + skeleton chunks) ==\n",
           reloc_ok?"OK (real code ran)":"unexpected");
    oracle_destroy(o);
    return 0;
}
