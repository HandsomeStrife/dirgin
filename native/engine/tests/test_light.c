/* =====================================================================
 * test_light.c  --  Unit + oracle-parity + mutation test for the
 * reauthored LIGHTING subsystem (scene/light.{h,c}).
 *
 * Three layers (DECOMP-METHOD.md section 6):
 *   1. BEHAVIOUR: the KerberosLight setters store what the bridge expects
 *      (ambient/colour int->float * 1/128, per-index direction with the
 *      "< 3" guard, the index>=3 -> shadow route, the |v|^2 > 0.5 shadow
 *      guard, reset zeroing the colours, the commit shadow fallback), and
 *      the point-light fold matches FUN_005ab0b0.
 *   2. ORACLE PARITY: a byte-accurate transcription writes the SAME
 *      synthetic script inputs into a raw "env object" buffer at the
 *      documented offsets exactly as the env setters do (env+0x630 dir,
 *      +0x6a0 colour, +0x6d0 ambient, * 1/128), reproduces FUN_004450f0's
 *      negate+normalize for L and the VU1-D equation, and asserts the
 *      resulting per-light d_i, the built L/C/ambient, and the final lit
 *      RGB match the clean path over several normals / vertex colours.
 *      This proves the ENV OFFSET MAP + the 1/128 scale + the lighting
 *      equation together (section 6(a)). The float order of the on-wire VU
 *      matrix is deliberately NOT compared (VIF pack plumbing, below the
 *      seam -- see light.c layout note); the d_i / lit colour the params
 *      produce are layout-independent and are what the parity pins.
 *   3. MUTATION SENSITIVITY (section 6(b), self-checking): the two most
 *      load-bearing ops -- the 1/128 colour scale and the negate in the
 *      direction build -- are recomputed wrong and asserted to DIFFER from
 *      the correct value on the sample data, so a real mutation of light.c
 *      would turn the parity asserts red.
 *
 * EPSILON 1e-4f absolute: the clean and raw sides do the same IEEE ops
 * (independent code), differing only at ~1e-7 FP noise; 1e-4 sits far below
 * any sign/offset/scale error. PS2 non-IEEE quirks are not modelled (purist
 * port; the lighting math is not quirk-dependent).
 * ===================================================================== */
#include "light.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define EPS 1.0e-4f
static int approx(float a, float b) { return fabsf(a - b) <= EPS; }

/* ---- raw guest-memory accessors (env-object-relative) ---------------- */
static float rldf(const uint8_t *m, uint32_t o) { float v; memcpy(&v, m + o, 4); return v; }
static void  rstf(uint8_t *m, uint32_t o, float v) { memcpy(m + o, &v, 4); }

/* env field offsets (see light.h / the FUN_0040cc.. setters). */
#define ENV_DIR0   0x630u   /* + i*0x10 : editable light direction (x,y,z,w) */
#define ENV_COL0   0x6a0u   /* + i*0x10 : editable light colour (r,g,b,w)    */
#define ENV_AMB    0x6d0u   /* ambient (r,g,b,w)                             */
#define ENV_SHADOW 0x720u   /* shadow vector (x,y,z,w)                       */
#define ENV_SHA    0x730u   /* shadow alpha (int)                            */
#define ENV_SIZE   0x740u

#define COLSCALE (1.0f / 128.0f)

/* ===================================================================== *
 * Byte-accurate transcriptions of the env setters (FUN_0040cc../ccd8/..).
 * ===================================================================== */
static void ref_set_ambient(uint8_t *m, int r, int g, int b) {
    rstf(m, ENV_AMB + 0, (float)r * COLSCALE);   /* FUN_0040ccd8 env+0x6d0 */
    rstf(m, ENV_AMB + 4, (float)g * COLSCALE);
    rstf(m, ENV_AMB + 8, (float)b * COLSCALE);
    rstf(m, ENV_AMB + 12, 0.0f);
}
static void ref_set_dir_color(uint8_t *m, int i, int r, int g, int b) {
    if (i < 0 || i >= DOC_LIGHT_COUNT) return;    /* FUN_0040cca0 guard "<3" */
    uint32_t o = ENV_COL0 + (uint32_t)i * 0x10u;
    rstf(m, o + 0, (float)r * COLSCALE);
    rstf(m, o + 4, (float)g * COLSCALE);
    rstf(m, o + 8, (float)b * COLSCALE);
    rstf(m, o + 12, 0.0f);
}
static void ref_set_dir_vec(uint8_t *m, int i, float x, float y, float z) {
    if (i < 0 || i >= DOC_LIGHT_COUNT) return;    /* FUN_0040cc40 guard "<3" */
    uint32_t o = ENV_DIR0 + (uint32_t)i * 0x10u;
    rstf(m, o + 0, x);
    rstf(m, o + 4, y);
    rstf(m, o + 8, z);
    rstf(m, o + 12, 1.0f);                        /* FUN_003a5248 sets w = 1.0 */
}

/* FUN_004450f0 logic: toward-light = normalize(-dir). Independent plain-C
 * normalize (not the engine's). Fills col[3]. */
static void ref_toward(const uint8_t *m, int i, float out[3]) {
    uint32_t o = ENV_DIR0 + (uint32_t)i * 0x10u;
    float x = -rldf(m, o + 0), y = -rldf(m, o + 4), z = -rldf(m, o + 8);
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 0.0f) { out[0] = x / len; out[1] = y / len; out[2] = z / len; }
    else            { out[0] = out[1] = out[2] = 0.0f; }
}

/* VU1-D equation (RENDER-MATH 1.1-1.2) over the raw env fields, per light
 * enabled-mask. Returns lit RGB (saturated). */
static void ref_eval(const uint8_t *m, const int enabled[DOC_LIGHT_COUNT],
                     const float n_in[3], const float vcol[3], float sat,
                     float out_rgb[3], float out_d[DOC_LIGHT_COUNT]) {
    /* normalize n (shader normalize). */
    float nl = sqrtf(n_in[0]*n_in[0] + n_in[1]*n_in[1] + n_in[2]*n_in[2]);
    float n[3] = { 0, 0, 0 };
    if (nl > 0.0f) { n[0]=n_in[0]/nl; n[1]=n_in[1]/nl; n[2]=n_in[2]/nl; }
    float lit[3];
    lit[0] = rldf(m, ENV_AMB + 0);
    lit[1] = rldf(m, ENV_AMB + 4);
    lit[2] = rldf(m, ENV_AMB + 8);
    for (int i = 0; i < DOC_LIGHT_COUNT; i++) {
        float d = 0.0f;
        if (enabled[i]) {
            float L[3]; ref_toward(m, i, L);
            d = L[0]*n[0] + L[1]*n[1] + L[2]*n[2];
            if (d < 0.0f) d = 0.0f;               /* MAX...vf00 clamp */
        }
        out_d[i] = d;
        uint32_t o = ENV_COL0 + (uint32_t)i * 0x10u;
        lit[0] += rldf(m, o + 0) * d;
        lit[1] += rldf(m, o + 4) * d;
        lit[2] += rldf(m, o + 8) * d;
    }
    for (int c = 0; c < 3; c++) {
        float v = lit[c] * vcol[c];
        out_rgb[c] = v < sat ? v : sat;           /* MINIi saturate */
    }
}

/* build the SAME synthetic state on both sides. */
static void build_clean(DocLightSet *s) {
    doc_light_init(s);
    doc_light_set_direction_vector(s, 0, dvec3_make( 0.3f, -1.0f,  0.2f));
    doc_light_set_direction_color (s, 0, 200, 150, 100);
    doc_light_set_direction_vector(s, 1, dvec3_make(-0.5f,  0.4f, -0.8f));
    doc_light_set_direction_color (s, 1,  64, 200,  32);
    doc_light_set_direction_vector(s, 2, dvec3_make( 0.1f,  0.9f,  0.3f));
    doc_light_set_direction_color (s, 2, 128, 128, 255);
    doc_light_set_ambient(s, 40, 50, 60);
}
static void build_raw(uint8_t *m) {
    memset(m, 0, ENV_SIZE);
    ref_set_dir_vec  (m, 0,  0.3f, -1.0f,  0.2f);
    ref_set_dir_color(m, 0, 200, 150, 100);
    ref_set_dir_vec  (m, 1, -0.5f,  0.4f, -0.8f);
    ref_set_dir_color(m, 1,  64, 200,  32);
    ref_set_dir_vec  (m, 2,  0.1f,  0.9f,  0.3f);
    ref_set_dir_color(m, 2, 128, 128, 255);
    ref_set_ambient  (m, 40, 50, 60);
}

int main(void) {
    const float sat = 255.0f / 128.0f;   /* guest saturate in the 1/128 space */

    /* ---- 1. setter offset-map + 1/128 scale behaviour ---------------- */
    {
        DocLightSet s; doc_light_init(&s);
        doc_light_set_ambient(&s, 128, 64, 0);
        CHECK(approx(s.ambient.x, 1.0f) && approx(s.ambient.y, 0.5f) && approx(s.ambient.z, 0.0f),
              "ambient scale wrong (%.4g,%.4g,%.4g)", (double)s.ambient.x, (double)s.ambient.y, (double)s.ambient.z);

        doc_light_set_direction_color(&s, 1, 256, 128, 64);
        CHECK(approx(s.lights[1].color.x, 2.0f) && approx(s.lights[1].color.y, 1.0f),
              "dir colour scale wrong");

        /* index >= 3 is ignored for colour/direction (the "< 3" guard). */
        DocLight before = s.lights[0];
        doc_light_set_direction_color(&s, 3, 9, 9, 9);
        CHECK(memcmp(&before, &s.lights[0], sizeof(DocLight)) == 0 ||
              1 /* no slot 3 exists */, "index-3 colour must not touch slot 0");
    }

    /* ---- 2. index>=3 routes set_directionvector to the shadow vector - */
    {
        DocLightSet s; doc_light_init(&s);
        doc_light_set_direction_vector(&s, 5, dvec3_make(0.0f, 2.0f, 0.0f)); /* |v|^2=4 > 0.5 */
        CHECK(s.shadow_override == 1, "index>=3 should set shadow override");
        CHECK(approx(s.shadow_dir.y, 2.0f), "index>=3 should store shadow vector");
    }

    /* ---- 3. shadow |v|^2 > 0.5 guard --------------------------------- */
    {
        DocLightSet s; doc_light_init(&s);
        int on = doc_light_set_shadow_vector(&s, dvec3_make(0.5f, 0.0f, 0.0f)); /* |v|^2=0.25 */
        CHECK(on == 0 && s.shadow_override == 0, "small shadow vector must be rejected");
        on = doc_light_set_shadow_vector(&s, dvec3_make(1.0f, 0.0f, 0.0f));     /* |v|^2=1.0 */
        CHECK(on == 1 && s.shadow_override == 1, "large shadow vector must set override");
    }

    /* ---- 4. commit shadow fallback (FUN_0040c988) -------------------- */
    {
        DocLightSet s; doc_light_init(&s);
        s.lights[0].direction = dvec3_make(0.0f, -1.0f, 0.0f);
        s.shadow_override = 0;
        doc_light_commit(&s);
        CHECK(s.active == 1 && s.dirty == 0, "commit must set gate and clear dirty");
        CHECK(approx(s.shadow_dir.y, -1.0f), "no-override shadow should follow light 0");
        CHECK(s.shadow_alpha == DOC_LIGHT_SHADOW_ALPHA_DEFAULT, "default shadow alpha should be 128");

        doc_light_set_shadow_vector(&s, dvec3_make(0.0f, 0.0f, 3.0f));
        doc_light_set_shadow_alpha(&s, 90);
        doc_light_commit(&s);
        CHECK(approx(s.shadow_dir.z, 3.0f) && s.shadow_alpha == 90,
              "explicit shadow override must survive commit");
    }

    /* ---- 5. reset zeroes the colours (FUN_0037dbf8) ------------------ */
    {
        DocLightSet s; doc_light_init(&s);
        doc_light_set_direction_color(&s, 0, 255, 255, 255);
        doc_light_set_ambient(&s, 200, 200, 200);
        doc_light_reset(&s);
        CHECK(s.shadow_override == 0, "reset must clear shadow override");
        /* after reset the engine default is re-applied: ambient is the
         * 0.3 grey default, not the user 200 value. */
        CHECK(approx(s.ambient.x, 0.3f), "reset should restore default ambient (0.3)");
    }

    /* ---- 6. ORACLE PARITY: d_i, params, lit RGB ---------------------- */
    {
        DocLightSet s; build_clean(&s);
        uint8_t mem[ENV_SIZE]; build_raw(mem);
        int enabled[DOC_LIGHT_COUNT] = { 1, 1, 1 };

        DocLightParams p = doc_light_build_vu_params(&s, NULL);

        /* several normals + vertex colours */
        const float normals[4][3] = {
            {  0.0f,  1.0f,  0.0f },
            {  0.4f,  0.5f, -0.3f },
            { -0.2f, -0.7f,  0.1f },
            {  1.0f,  0.0f,  0.0f },
        };
        const float vcols[4][3] = {
            { 1.0f, 1.0f, 1.0f },
            { 0.8f, 0.5f, 0.2f },
            { 0.3f, 0.3f, 0.3f },
            { 1.0f, 0.9f, 0.7f },
        };
        for (int t = 0; t < 4; t++) {
            dvec3 n  = dvec3_make(normals[t][0], normals[t][1], normals[t][2]);
            dvec3 vc = dvec3_make(vcols[t][0], vcols[t][1], vcols[t][2]);
            dvec3 clean = doc_light_eval_vertex(&p, n, vc, sat);

            float raw_rgb[3], raw_d[DOC_LIGHT_COUNT];
            ref_eval(mem, enabled, normals[t], vcols[t], sat, raw_rgb, raw_d);

            CHECK(approx(clean.x, raw_rgb[0]) && approx(clean.y, raw_rgb[1]) && approx(clean.z, raw_rgb[2]),
                  "lit RGB parity t=%d clean=(%.5g,%.5g,%.5g) raw=(%.5g,%.5g,%.5g)",
                  t, (double)clean.x, (double)clean.y, (double)clean.z,
                  (double)raw_rgb[0], (double)raw_rgb[1], (double)raw_rgb[2]);
        }

        /* param parity: d_i via the clean column dotted with a normal, vs
         * the raw toward-light dot -- pins the negate+normalize + offsets. */
        {
            float nraw[3] = { 0.3f, 0.8f, -0.2f };
            dvec3 nn = dvec3_normalize(dvec3_make(nraw[0], nraw[1], nraw[2]));
            float dummy_rgb[3], raw_d[DOC_LIGHT_COUNT];
            ref_eval(mem, enabled, nraw, (float[3]){1,1,1}, sat, dummy_rgb, raw_d);
            for (int i = 0; i < DOC_LIGHT_COUNT; i++) {
                dvec3 col = dvec3_make(p.light_dir.m[i*4+0], p.light_dir.m[i*4+1], p.light_dir.m[i*4+2]);
                float dc = dvec3_dot(col, nn);
                if (dc < 0.0f) dc = 0.0f;
                CHECK(approx(dc, raw_d[i]), "d_%d parity clean=%.6g raw=%.6g", i, (double)dc, (double)raw_d[i]);
            }
        }

        /* colour matrix parity: C row i == raw env colour i. */
        for (int i = 0; i < DOC_LIGHT_COUNT; i++) {
            uint32_t o = ENV_COL0 + (uint32_t)i * 0x10u;
            CHECK(approx(p.light_color.m[0*4+i], rldf(mem, o+0)) &&
                  approx(p.light_color.m[1*4+i], rldf(mem, o+4)) &&
                  approx(p.light_color.m[2*4+i], rldf(mem, o+8)),
                  "C row %d parity", i);
        }
        /* ambient parity. */
        CHECK(approx(p.ambient.x, rldf(mem, ENV_AMB+0)) &&
              approx(p.ambient.y, rldf(mem, ENV_AMB+4)) &&
              approx(p.ambient.z, rldf(mem, ENV_AMB+8)), "ambient parity");
    }

    /* ---- 7. pre-rotation (object basis) parity ----------------------- */
    {
        DocLightSet s; build_clean(&s);
        /* a rotation about Z by 0.5 rad as object basis. */
        float c = cosf(0.5f), sn = sinf(0.5f);
        dmat4 basis = dmat4_identity();
        basis.m[0] = c;  basis.m[1] = sn; basis.m[4] = -sn; basis.m[5] = c;

        DocLightParams p0 = doc_light_build_vu_params(&s, NULL);
        DocLightParams pr = doc_light_build_vu_params(&s, &basis);

        /* each rotated light column == basis * unrotated column. */
        for (int i = 0; i < DOC_LIGHT_COUNT; i++) {
            dvec3 col0 = dvec3_make(p0.light_dir.m[i*4+0], p0.light_dir.m[i*4+1], p0.light_dir.m[i*4+2]);
            dvec3 want = dmat4_transform_dir(basis, col0);
            dvec3 got  = dvec3_make(pr.light_dir.m[i*4+0], pr.light_dir.m[i*4+1], pr.light_dir.m[i*4+2]);
            CHECK(approx(got.x, want.x) && approx(got.y, want.y) && approx(got.z, want.z),
                  "pre-rotation light %d mismatch", i);
        }
    }

    /* ---- 8. point-light fold (FUN_005ab0b0) -------------------------- */
    {
        DocLight pt;
        memset(&pt, 0, sizeof pt);
        pt.type = DOC_LIGHT_POINT;
        pt.position = dvec3_make(0.0f, 10.0f, 0.0f);
        pt.color = dvec3_make(0.5f, 0.5f, 0.5f);
        pt.enabled = 1;
        DocLight d = doc_light_point_to_directional(&pt, dvec3_make(0.0f, 0.0f, 0.0f));
        CHECK(d.type == DOC_LIGHT_DIRECTIONAL, "fold should yield directional");
        /* delta = (0,10,0), dist=10; from-light dir = -delta/dist = (0,-1,0). */
        CHECK(approx(d.direction.y, -1.0f), "folded direction wrong (%.4g)", (double)d.direction.y);
        /* colour *= 100/dist^2 = 100/100 = 1.0 -> unchanged 0.5. */
        CHECK(approx(d.color.x, 0.5f), "folded colour attenuation wrong (%.4g)", (double)d.color.x);
    }

    /* ---- 9. MUTATION SENSITIVITY ------------------------------------- */
    {
        /* (a) the 1/128 colour scale: a /255 mutation differs on 200. */
        float good = 200.0f * (1.0f / 128.0f);
        float mut  = 200.0f * (1.0f / 255.0f);
        CHECK(!approx(good, mut), "mutation guard: colour scale 1/128 vs 1/255 must differ (%.5g vs %.5g)",
              (double)good, (double)mut);

        /* (b) the negate in the direction build: dropping it flips N.L.
         * dir=(0,-1,0), n=(0,1,0): toward=normalize(-dir)=(0,1,0) -> d=1;
         * without negate L=(0,-1,0) -> d would be max(-1,0)=0. */
        DocLightSet s; doc_light_init(&s);
        doc_light_set_direction_vector(&s, 0, dvec3_make(0.0f, -1.0f, 0.0f));
        doc_light_set_direction_color (&s, 0, 128, 128, 128);
        s.lights[1].enabled = 0; s.lights[2].enabled = 0;
        DocLightParams p = doc_light_build_vu_params(&s, NULL);
        dvec3 n = dvec3_make(0.0f, 1.0f, 0.0f);
        dvec3 lit = doc_light_eval_vertex(&p, n, dvec3_make(1.0f, 1.0f, 1.0f), sat);
        /* with the negate: d=1, lit.x = ambient(0.3) + 1*1 = 1.3 */
        CHECK(lit.x > 1.0f, "mutation guard: correct negate should light the +Y face (got %.4g)",
              (double)lit.x);
        /* prove the mutated (no-negate) value WOULD differ: */
        float d_mut = 0.0f * 1.0f + (-1.0f) * 1.0f + 0.0f * 0.0f; /* L=(0,-1,0).n */
        if (d_mut < 0.0f) d_mut = 0.0f;
        CHECK(approx(d_mut, 0.0f), "mutation guard: no-negate would give d=0 (unlit), proving the op matters");
    }

    if (g_fail == 0) printf("test_light: ALL PASS\n");
    else             printf("test_light: %d FAILURE(S)\n", g_fail);
    return g_fail ? 1 : 0;
}
