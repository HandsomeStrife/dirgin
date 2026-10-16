/* =====================================================================
 * light.h  --  Reauthored LIGHTING subsystem (purist port, P4).
 *
 * "The light STATE the scripts control + the per-light params the renderer
 * uploads to VU1." This module owns the directional-light / ambient /
 * shadow state that the KerberosLight script bridge drives, the commit
 * that resolves the editable state into the block the renderer reads, and
 * the build of the final L (direction) / C (colour) / A (ambient) params
 * the EE hands VU1 program D each actor draw.
 *
 * WHAT IS *NOT* HERE (below the seam -- the renderer/VU1 reproduces it):
 *   The per-vertex Lambert evaluation itself is VU1 program-D microcode,
 *   NOT EE code (port/vu/RENDER-MATH.md section 1). The EE only stores and
 *   transforms the light PARAMS; VU1 does   d_i = max(Ldir_i . n, 0) ;
 *   lit = C.d + ambient ; rgb = saturate(lit (x) vertexColour). A native
 *   reference of that equation is provided (doc_light_eval_vertex) so the
 *   unit test can validate the whole param chain by the lit colour it
 *   produces; the real renderer implements it per RENDER-MATH / the GLSL in
 *   port/vu/shaders/dirge_purist.vert.
 *
 * The lighting model (traced to RENDER-MATH.md + the decomp):
 *   3 DIRECTIONAL lights + 1 ambient. NO point lights in the resident
 *   microcode (RENDER-MATH section 1.2); the EE folds dynamic point lights
 *   into the spare directional rows as an approximation (FUN_005ab0b0
 *   lines 155-243) -- surfaced here as doc_light_point_to_directional, a
 *   documented secondary path, not something KerberosLight sets.
 *
 * Reauthored from (analysis/decomp/, read-only oracle):
 *   Script bridge natives (fetch VM args -> env setter):
 *     FUN_003a50a8  set_ambientcolor(r,g,b)        -> doc_light_set_ambient
 *     FUN_003a5140  set_directioncolor(i,r,g,b)    -> doc_light_set_direction_color
 *     FUN_003a5248  set_directionvector(i,vec)     -> doc_light_set_direction_vector
 *                   (i>=3 routes to the shadow vector via FUN_00380210)
 *     FUN_003a5048  set_shadowalpha(a)             -> doc_light_set_shadow_alpha
 *     FUN_0037dbf8  reset()                        -> doc_light_reset
 *     FUN_00380210  set_shadowvector(vec)  (|v|^2>0.5 guard) -> doc_light_set_shadow_vector
 *   Env setters (this = env object @ base 0x4fc7d0; store into +off):
 *     FUN_0040ccd8  ambient  -> env+0x6d0,  flag |= 0x2000000000 (dirty)
 *     FUN_0040cca0  colour i -> env+0x6a0 + i*0x10 (i<3), dirty
 *     FUN_0040cc40  dir i    -> env+0x630 + i*0x10 (i<3); rebuild dir basis
 *                              @ env+0x660 via FUN_004450f0; dirty
 *     FUN_0040cd18  shadow vec   -> env+0x720, flag |= 0x800000000 (override)
 *     FUN_0040cd38  shadow alpha -> env+0x730
 *     FUN_0040ccf8  clear shadow override (flag &= ~0x800000000)
 *   Colour int->float scale: FUN_003a4fe8 multiplies the r,g,b ints by
 *     0x3c000000 == 1/128 (so a script value of 128 == 1.0).
 *   Direction-matrix build: FUN_004450f0 (+ FUN_00444c70 scale-by,
 *     FUN_00444ad0 normalize, FUN_00444b08 4x4 transpose) ->
 *     doc_light_build_direction_matrix: each row = normalize(-dir_i), a
 *     (0,0,0,1) 4th row, then TRANSPOSED, so the VU-loaded matrix's COLUMN i
 *     is normalize(-dir_i) and the VU MADD chain yields d_i = col_i . n.
 *   Commit: FUN_0040c988 copies editable (0x630..0x6d0) -> the resolved
 *     "override" block (0x580 dir matrix, 0x5c0/5d0/5e0 colours, 0x5f0
 *     ambient, 0x600 shadow vec, 0x610 shadow alpha) and derives the
 *     renderer gate bit 0x1000000000 from the dirty bit; with no shadow
 *     override it fills shadow from light 0's basis and alpha = 0x80 (128).
 *   Per-frame consumer: FUN_005ab0b0 reads the resolved block (gate set) or
 *     the per-area default table @ DAT_004e6370 (gate clear), pre-rotates
 *     the directions into the object/bone frame (FUN_0026125c), and scales
 *     the colours by the per-object brightness (obj+0x450 * 1/128) -- the
 *     pre-rotation is modelled (doc_light_build_vu_params takes an object
 *     basis); the brightness/material modulation is a documented hook.
 *   EE packer: FUN_005b2ef8 UNPACKs (VIFcode 0x6c110000, 17 qw to VU 0x00)
 *     MVP -> VU 0x00-0x30, L -> 0x40-0x60, C -> 0x70-0x90, A -> 0xa0,
 *     fog/shadow -> 0xb0-0x100 (RENDER-MATH section 1.1, exact match).
 *   Defaults: FUN_0040c078 / FUN_0040cd58 install the per-area default
 *     table @ DAT_004e6370: colour 0.4 grey (0x3ecccccd), ambient 0.3 grey
 *     (0x3e99999a), a normalized primary direction, shadow alpha 128.
 *
 * The JVM native-method REGISTRY that binds the literal
 * "fake/KerberosLight.set_ambientcolor" string to the engine entry is the
 * documented engine GAP (as for the camera bridge); we model what each
 * method reads/writes, not the dispatch address.
 * ===================================================================== */
#ifndef DOC_LIGHT_H
#define DOC_LIGHT_H

#include "dmath.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Three directional lights (RENDER-MATH section 1.2; FUN_0040cca0/cc40
 * guard the script index with "< 3"). */
#define DOC_LIGHT_COUNT 3

/* Script colour ints are scaled to float by 1/128 (FUN_003a4fe8, the
 * 0x3c000000 broadcast multiply). A script value of 128 maps to 1.0; the
 * full 0..255 range maps to 0..~1.99. */
#define DOC_LIGHT_COLOR_SCALE (1.0f / 128.0f)

/* Default shadow alpha when no explicit shadow override is set
 * (FUN_0040c988: "*(env+0x610) = 0x80"). 0..128 == 0..1 opacity. */
#define DOC_LIGHT_SHADOW_ALPHA_DEFAULT 128

/* set_shadowvector keeps the vector only if |v|^2 exceeds this, else it
 * clears the override (FUN_00380210: "if (0.5 < |v|^2) ... else clear"). */
#define DOC_LIGHT_SHADOW_MIN_LENSQ 0.5f

/* Light kind. KerberosLight only ever sets DIRECTIONAL; POINT is the EE
 * dynamic-light-as-directional approximation (FUN_005ab0b0 155-243),
 * exposed via doc_light_point_to_directional and never uploaded as a
 * distinct VU form (the microcode has no attenuation; RENDER-MATH 1.2). */
typedef enum {
    DOC_LIGHT_DIRECTIONAL = 0,
    DOC_LIGHT_POINT       = 1
} DocLightType;

/* One light slot.
 *   DIRECTIONAL: `direction` is the world-space direction the light travels
 *     (FROM the light). The VU consumes normalize(-direction) so N.L uses
 *     the TOWARD-light vector (see doc_light_build_direction_matrix).
 *   POINT (secondary/EE-folded): `position` is the world-space light
 *     position; doc_light_point_to_directional turns it into a DIRECTIONAL
 *     slot relative to a reference point with 1/dist^2 * 100 attenuation.
 * `color` is the already-scaled RGB (script ints * 1/128). `enabled` is a
 * native convenience: the guest has no per-light enable, it expresses
 * "off" by a zero colour (reset() zeroes all three colours); a disabled
 * slot contributes nothing to the built params. */
typedef struct DocLight {
    DocLightType type;        /* <- DIRECTIONAL for all KerberosLight lights */
    dvec3        direction;   /* <- env+0x630 + i*0x10 (world, from-light)   */
    dvec3        position;    /* POINT only (EE-folded path)                 */
    dvec3        color;       /* <- env+0x6a0 + i*0x10, RGB * 1/128          */
    int          enabled;     /* native: 0 -> slot drops out of the params   */
} DocLight;

/* The scene's active lighting state (the env object's light fields, base
 * 0x4fc7d0). `dirty` mirrors flag bit 0x2000000000 (a setter ran since the
 * last commit); `active` mirrors the renderer gate bit 0x1000000000 (use
 * the resolved block vs the per-area default table); `shadow_override`
 * mirrors bit 0x800000000 (an explicit shadow vector was set). */
typedef struct DocLightSet {
    DocLight lights[DOC_LIGHT_COUNT];  /* 3 directional slots                */
    dvec3    ambient;                  /* <- env+0x6d0, RGB * 1/128          */
    dvec3    shadow_dir;               /* <- env+0x720 (shadow projection)   */
    int      shadow_alpha;             /* <- env+0x730 (0..128)              */
    int      shadow_override;          /* flag bit 0x800000000               */
    int      dirty;                    /* flag bit 0x2000000000              */
    int      active;                   /* flag bit 0x1000000000 (gate)       */
} DocLightSet;

/* The final per-light params the renderer uploads (what FUN_005b2ef8 packs
 * into VU memory 0x40-0xa0 for program D). L and C are column-major dmat4;
 * only the upper-left 3x3 and the ambient are meaningful.
 *   L: column i = the TOWARD-light unit vector normalize(-direction_i).
 *      VU program D does ACC = L_col0*n.x + L_col1*n.y + L_col2*n.z, i.e.
 *      d_i = dot(L column i, n)  (RENDER-MATH section 1.1 idx 88-93).
 *   C: row i (i<3) = light i's colour; row 3 would be the ambient, kept
 *      separately here as `ambient`. VU does lit = C.d + ambient.
 *      (The engine lays C out as rows 0..2 = colours, row 3 = ambient; we
 *      split ambient out for clarity and re-join it at eval time.) */
typedef struct DocLightParams {
    dmat4 light_dir;    /* L : columns 0..2 = normalize(-direction_i)        */
    dmat4 light_color;  /* C : rows 0..2 = colour_i                          */
    dvec3 ambient;      /* A : ambient RGB                                   */
} DocLightParams;

/* ---- init / reset ---------------------------------------------------- */

/* Engine defaults (FUN_0040c078 / FUN_0040cd58): light 0 enabled, a
 * normalized primary direction, 0.4 grey colour, 0.3 grey ambient, lights
 * 1/2 off, shadow alpha 128, no shadow override. */
void doc_light_init(DocLightSet *s);

/* KerberosLight.reset():  FUN_0037dbf8 -- ambient := 0, all three colours
 * := 0, re-apply the engine defaults, clear the shadow override. */
void doc_light_reset(DocLightSet *s);

/* ---- KerberosLight state setters ------------------------------------- *
 * The (int r,g,b) overloads take the RAW script ints and apply the 1/128
 * scale themselves (FUN_003a4fe8), exactly as the bridge natives do. */

/* set_ambientcolor(r,g,b):  FUN_003a50a8 -> FUN_0040ccd8 (env+0x6d0). */
void doc_light_set_ambient(DocLightSet *s, int r, int g, int b);

/* set_directioncolor(index,r,g,b): FUN_003a5140 -> FUN_0040cca0
 * (env+0x6a0+index*0x10). index >= DOC_LIGHT_COUNT is ignored (the guest's
 * "index < 3" guard). */
void doc_light_set_direction_color(DocLightSet *s, int index, int r, int g, int b);

/* set_directionvector(index,vec): FUN_003a5248 -> FUN_0040cc40
 * (env+0x630+index*0x10). index < 3 sets light `index`'s direction;
 * index >= 3 routes to the shadow vector (FUN_00380210), matching the
 * bridge native's branch. */
void doc_light_set_direction_vector(DocLightSet *s, int index, dvec3 dir);

/* set_shadowvector(vec): FUN_00380210 -> FUN_0040cd18. Stores the shadow
 * projection direction and sets the override flag ONLY if |vec|^2 > 0.5;
 * otherwise clears the override (FUN_0040ccf8). Returns 1 if the override
 * is now active, 0 if it was cleared. */
int doc_light_set_shadow_vector(DocLightSet *s, dvec3 dir);

/* set_shadowalpha(a): FUN_003a5048 -> FUN_0040cd38 (env+0x730). */
void doc_light_set_shadow_alpha(DocLightSet *s, int alpha);

/* ---- commit + param build (what feeds the renderer) ------------------ */

/* FUN_0040c988: resolve the editable state for the renderer. Clears the
 * dirty flag and sets the gate (`active`). With no shadow override, derives
 * the shadow vector from light 0 and the shadow alpha to the default 128.
 * (In the guest this also copies into the absolute override block
 * DAT_004fcd50.. that FUN_005ab0b0/FUN_005acc00 read; here the state IS the
 * resolved form, so commit only normalises the flags + shadow fallback.) */
void doc_light_commit(DocLightSet *s);

/* FUN_004450f0 (+ FUN_00444c70/ad0/b08): build L for the VU. Each column i
 * = normalize(-lights[i].direction) for an enabled DIRECTIONAL slot, else a
 * zero column (disabled). Returned column-major; the 3x3 upper-left is the
 * meaningful part, with m[15] = 1. */
dmat4 doc_light_build_direction_matrix(const DocLightSet *s);

/* Build C (colours as rows 0..2; disabled/non-directional slots -> zero
 * row). The ambient travels alongside in DocLightParams. */
dmat4 doc_light_build_color_matrix(const DocLightSet *s);

/* Assemble the full renderer params. `object_basis` is the per-object (or
 * bone) rotation the directions are pre-rotated into (FUN_0026125c in
 * FUN_005ab0b0 line 287 does L' = object_basis * L); pass NULL / identity
 * for world-space. The result is exactly what FUN_005b2ef8 uploads to VU
 * 0x40-0xa0. */
DocLightParams doc_light_build_vu_params(const DocLightSet *s, const dmat4 *object_basis);

/* ---- EE point-light -> directional folding (secondary, FUN_005ab0b0) -- *
 * dir = normalize(light_pos - ref_pos); the colour is attenuated by
 * 100 / dist^2 (FUN_005ab0b0 lines 194-197 / 233-235). Returns a
 * DIRECTIONAL DocLight. This is the EE's point-light APPROXIMATION -- the
 * VU microcode itself has no attenuation (RENDER-MATH section 1.2). */
DocLight doc_light_point_to_directional(const DocLight *point, dvec3 ref_pos);

/* ---- below-seam VU1 reference (NOT EE code; for test/validation) ------ *
 * Reproduces VU1 program D per-vertex shading (RENDER-MATH section 1.1-1.2
 * / dirge_purist.vert) on one vertex, so a built DocLightParams can be
 * validated by the lit colour it yields:
 *     d_i = max(dot(L column i, n), 0)
 *     lit = (C row 0)*d0 + (C row 1)*d1 + (C row 2)*d2 + ambient
 *     rgb = min(lit (x) vertex_color, saturate)
 * `n` need not be unit (the shader normalizes). `vertex_color` and the
 * result are 0..1 RGB; `saturate` is the per-channel clamp (e.g. 1.0, or
 * 255/128 for the guest's 0..255 then-times-(1/128) space). */
dvec3 doc_light_eval_vertex(const DocLightParams *p, dvec3 n, dvec3 vertex_color,
                            float saturate);

#ifdef __cplusplus
}
#endif
#endif /* DOC_LIGHT_H */
