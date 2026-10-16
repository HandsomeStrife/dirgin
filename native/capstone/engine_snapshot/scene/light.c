/* =====================================================================
 * light.c  --  Implementation of the reauthored lighting subsystem.
 *
 * Layout note. The reauthored L stores light i's TOWARD-light unit vector
 * as COLUMN i (column-major m[i*4+row]) and C stores colour i as row i, so
 * doc_light_eval_vertex reads them the way VU program D sums them:
 * d_i = dot(L column i, n); lit = sum_i (C row i)*d_i + ambient. This is
 * the logical form of the params.
 *
 * The guest's ACTUAL VU upload is a component-interleaved TRANSPOSE of this
 * (FUN_004450f0 builds rows = normalize(-dir_i) then FUN_00444b08 transposes
 * it, so the VU's vf05/vf06/vf07 hold the x/y/z component rows and the MADD
 * chain yields the same d_i). That interleave is VIF/DMA pack plumbing below
 * the seam (as camera.c treats the VU0 lane-shuffle inverse): we reproduce
 * the LOGIC (negate, normalize, the d_i/lit/saturate equation) and validate
 * it by the resulting params and lit colour, not the on-wire float order.
 * ===================================================================== */
#include "light.h"
#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * init / reset
 * ------------------------------------------------------------------- */
void doc_light_init(DocLightSet *s)
{
    memset(s, 0, sizeof *s);
    /* FUN_0040c078 / FUN_0040cd58 engine defaults: light 0 is a grey
     * directional, lights 1/2 off, 0.3 grey ambient, shadow alpha 128. */
    s->lights[0].type      = DOC_LIGHT_DIRECTIONAL;
    s->lights[0].enabled   = 1;
    /* default primary direction: the engine stores (0, 1.5, 0.5) and the
     * VU side negates+normalizes it; we keep the from-light direction. */
    s->lights[0].direction = dvec3_make(0.0f, 1.5f, 0.5f);
    s->lights[0].color     = dvec3_make(0.4f, 0.4f, 0.4f);   /* 0x3ecccccd */

    s->lights[1].type = DOC_LIGHT_DIRECTIONAL;
    s->lights[2].type = DOC_LIGHT_DIRECTIONAL;

    s->ambient      = dvec3_make(0.3f, 0.3f, 0.3f);          /* 0x3e99999a */
    s->shadow_dir   = dvec3_make(0.0f, 0.0f, 0.0f);
    s->shadow_alpha = DOC_LIGHT_SHADOW_ALPHA_DEFAULT;
    s->shadow_override = 0;
    s->dirty  = 0;
    s->active = 0;
}

void doc_light_reset(DocLightSet *s)
{
    /* FUN_0037dbf8: ambient := 0, all three colours := 0, re-apply the
     * engine defaults, clear the shadow override. The guest zeroes then
     * re-applies the area defaults via FUN_00269a08; the net observable
     * state is the engine default with the shadow override cleared. */
    doc_light_init(s);
    s->shadow_override = 0;
    s->dirty = 1;   /* a reset is a state change -> dirty for the next commit */
}

/* ---------------------------------------------------------------------
 * KerberosLight state setters
 * ------------------------------------------------------------------- */

/* FUN_003a4fe8: r,g,b ints -> float * (1/128). */
static dvec3 doc_light_scale_color(int r, int g, int b)
{
    return dvec3_make((float)r * DOC_LIGHT_COLOR_SCALE,
                      (float)g * DOC_LIGHT_COLOR_SCALE,
                      (float)b * DOC_LIGHT_COLOR_SCALE);
}

void doc_light_set_ambient(DocLightSet *s, int r, int g, int b)
{
    /* FUN_003a50a8 -> FUN_0040ccd8 (env+0x6d0), dirty. */
    s->ambient = doc_light_scale_color(r, g, b);
    s->dirty = 1;
}

void doc_light_set_direction_color(DocLightSet *s, int index, int r, int g, int b)
{
    /* FUN_003a5140 -> FUN_0040cca0 (env+0x6a0+index*0x10). Guard index<3. */
    if (index < 0 || index >= DOC_LIGHT_COUNT)
        return;
    s->lights[index].color   = doc_light_scale_color(r, g, b);
    s->lights[index].enabled = 1;    /* setting a colour arms the slot */
    s->dirty = 1;
}

void doc_light_set_direction_vector(DocLightSet *s, int index, dvec3 dir)
{
    /* FUN_003a5248: index<3 -> FUN_0040cc40 (env+0x630+index*0x10, rebuild
     * the dir basis); index>=3 -> shadow vector (FUN_00380210). */
    if (index >= DOC_LIGHT_COUNT) {
        doc_light_set_shadow_vector(s, dir);
        return;
    }
    if (index < 0)
        return;
    s->lights[index].type      = DOC_LIGHT_DIRECTIONAL;
    s->lights[index].direction = dir;
    s->lights[index].enabled   = 1;
    s->dirty = 1;
}

int doc_light_set_shadow_vector(DocLightSet *s, dvec3 dir)
{
    /* FUN_00380210: keep + set override only if |dir|^2 > 0.5. */
    float lensq = dvec3_dot(dir, dir);
    if (lensq > DOC_LIGHT_SHADOW_MIN_LENSQ) {
        s->shadow_dir      = dir;            /* FUN_0040cd18 (env+0x720) */
        s->shadow_override = 1;              /* flag |= 0x800000000      */
        return 1;
    }
    s->shadow_override = 0;                  /* FUN_0040ccf8 clears it   */
    return 0;
}

void doc_light_set_shadow_alpha(DocLightSet *s, int alpha)
{
    /* FUN_003a5048 -> FUN_0040cd38 (env+0x730). */
    s->shadow_alpha = alpha;
}

/* ---------------------------------------------------------------------
 * commit (FUN_0040c988)
 * ------------------------------------------------------------------- */
void doc_light_commit(DocLightSet *s)
{
    /* The gate (renderer uses the resolved params rather than the per-area
     * default table) is driven from the dirty bit. */
    s->active = 1;

    if (!s->shadow_override) {
        /* No explicit shadow: derive it from light 0 and default the alpha
         * (FUN_0040c988 else-branch: shadow <- light-0 basis, alpha 0x80). */
        s->shadow_dir   = s->lights[0].direction;
        s->shadow_alpha = DOC_LIGHT_SHADOW_ALPHA_DEFAULT;
    }
    s->dirty = 0;
}

/* ---------------------------------------------------------------------
 * param build (FUN_004450f0 + FUN_005ab0b0 pre-rotation + FUN_005b2ef8)
 * ------------------------------------------------------------------- */

/* FUN_004450f0: row i = normalize(-dir_i), 4th row (0,0,0,1), then the
 * whole 4x4 is TRANSPOSED (FUN_00444b08). Transposing the "rows =
 * normalize(-dir_i)" matrix puts normalize(-dir_i) into COLUMN i, which is
 * exactly the layout VU program D consumes. We build the columns directly
 * (== the transposed result) so the convention is explicit. */
dmat4 doc_light_build_direction_matrix(const DocLightSet *s)
{
    dmat4 L = dmat4_identity();
    int i;
    for (i = 0; i < DOC_LIGHT_COUNT; i++) {
        dvec3 col = dvec3_make(0.0f, 0.0f, 0.0f);
        const DocLight *l = &s->lights[i];
        if (l->enabled && l->type == DOC_LIGHT_DIRECTIONAL) {
            /* -direction, then normalize (FUN_00444c70 x -1, FUN_00444ad0). */
            dvec3 toward = dvec3_scale(l->direction, -1.0f);
            col = dvec3_normalize(toward);
        }
        /* column i (column-major: m[i*4 + row]). */
        L.m[i * 4 + 0] = col.x;
        L.m[i * 4 + 1] = col.y;
        L.m[i * 4 + 2] = col.z;
        L.m[i * 4 + 3] = 0.0f;
    }
    /* columns >= DOC_LIGHT_COUNT left as identity's remainder; m[15] = 1. */
    return L;
}

dmat4 doc_light_build_color_matrix(const DocLightSet *s)
{
    dmat4 C = { { 0 } };
    int i;
    for (i = 0; i < DOC_LIGHT_COUNT; i++) {
        dvec3 c = dvec3_make(0.0f, 0.0f, 0.0f);
        const DocLight *l = &s->lights[i];
        if (l->enabled && l->type == DOC_LIGHT_DIRECTIONAL)
            c = l->color;
        /* row i (column-major: m[col*4 + i]). */
        C.m[0 * 4 + i] = c.x;
        C.m[1 * 4 + i] = c.y;
        C.m[2 * 4 + i] = c.z;
    }
    return C;
}

DocLightParams doc_light_build_vu_params(const DocLightSet *s, const dmat4 *object_basis)
{
    DocLightParams p;
    p.light_dir   = doc_light_build_direction_matrix(s);
    p.light_color = doc_light_build_color_matrix(s);
    p.ambient     = s->ambient;

    if (object_basis) {
        /* FUN_005ab0b0 line 287: L' = object_basis * L. This rotates the
         * toward-light columns into the object/bone frame so the VU dots
         * them against the (skinned) object-space normal. */
        p.light_dir = dmat4_mul(*object_basis, p.light_dir);
    }
    return p;
}

/* ---------------------------------------------------------------------
 * EE point-light -> directional folding (FUN_005ab0b0 155-243)
 * ------------------------------------------------------------------- */
DocLight doc_light_point_to_directional(const DocLight *point, dvec3 ref_pos)
{
    DocLight out = *point;
    dvec3 delta = dvec3_sub(point->position, ref_pos);
    float distsq = dvec3_dot(delta, delta);

    out.type = DOC_LIGHT_DIRECTIONAL;
    if (distsq > 0.0f) {
        float dist = sqrtf(distsq);
        float inv_distsq = 1.0f / distsq;
        /* dir = delta / dist  (FUN_005ab0b0: * (fVar14*fVar15) = *(1/dist));
         * we store the FROM-light direction, so negate the toward vector. */
        out.direction = dvec3_scale(delta, -1.0f / dist);
        /* colour *= 100 / dist^2 (lines 195 / 234). */
        out.color = dvec3_scale(point->color, 100.0f * inv_distsq);
        out.enabled = point->enabled;
    } else {
        out.direction = dvec3_make(0.0f, 0.0f, 0.0f);
        out.color     = dvec3_make(0.0f, 0.0f, 0.0f);
        out.enabled   = 0;
    }
    return out;
}

/* ---------------------------------------------------------------------
 * below-seam VU1 reference (RENDER-MATH 1.1-1.2 / dirge_purist.vert)
 * ------------------------------------------------------------------- */
dvec3 doc_light_eval_vertex(const DocLightParams *p, dvec3 n, dvec3 vertex_color,
                            float saturate)
{
    dvec3 nn = dvec3_normalize(n);
    /* d_i = max(dot(L column i, n), 0). Column i: m[i*4 + 0..2]. */
    float d[DOC_LIGHT_COUNT];
    int i;
    for (i = 0; i < DOC_LIGHT_COUNT; i++) {
        dvec3 col = dvec3_make(p->light_dir.m[i * 4 + 0],
                               p->light_dir.m[i * 4 + 1],
                               p->light_dir.m[i * 4 + 2]);
        float dot = dvec3_dot(col, nn);
        d[i] = dot > 0.0f ? dot : 0.0f;
    }
    /* lit = sum_i (C row i) * d_i + ambient. Row i: m[0..2 *4 + i]. */
    dvec3 lit = p->ambient;
    for (i = 0; i < DOC_LIGHT_COUNT; i++) {
        dvec3 crow = dvec3_make(p->light_color.m[0 * 4 + i],
                                p->light_color.m[1 * 4 + i],
                                p->light_color.m[2 * 4 + i]);
        lit = dvec3_add(lit, dvec3_scale(crow, d[i]));
    }
    /* rgb = min(lit (x) vertex_color, saturate). */
    dvec3 rgb = dvec3_make(lit.x * vertex_color.x,
                           lit.y * vertex_color.y,
                           lit.z * vertex_color.z);
    rgb.x = rgb.x < saturate ? rgb.x : saturate;
    rgb.y = rgb.y < saturate ? rgb.y : saturate;
    rgb.z = rgb.z < saturate ? rgb.z : saturate;
    return rgb;
}
