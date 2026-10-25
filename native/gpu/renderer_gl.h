/* renderer_gl.h -- portable GL 3.3-core renderer for DoC models.
 *
 * Pure OpenGL: it assumes a current 3.3-core context (provided by whatever
 * gfx_context backend -- EGL here, WGL/swapchain on Windows) and knows nothing
 * about the platform. It uploads a DocModel + optional RGBA texture, renders an
 * auto-framed orbit view into its own FBO, and reads the pixels back top-left.
 *
 * The camera framing and the lighting constants are deliberately identical to
 * the software reference rasterizer (../renderer/renderer.c) so the two outputs
 * can be compared pixel-region for pixel-region.
 */
#ifndef RENDERER_GL_H
#define RENDERER_GL_H

#include <stddef.h>   /* size_t */
#include "doc_model.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { RGL_TEXTURED = 0, RGL_SHADED = 1, RGL_WIRE = 2 };

/* --------------------------------------------------------------------------
 * PURIST SHADING (RENDER-MATH.md) -- the VU1 program A/B/D per-vertex model.
 *
 *   PRELIT  = static world geometry (VU1 programs A, B): NO N.L, the baked
 *             per-vertex colour passes through; texture modulates it; fog.
 *   LIT     = character/actor geometry (VU1 program D): 3 directional lights
 *             d_i = max(Ldir_i . n, 0); lit = sum_i Lcol_i*d_i + ambient;
 *             rgb = saturate(lit (x) vertexColour); alpha rides through.
 * -------------------------------------------------------------------------- */
enum { RGL_SHADE_PRELIT = 0, RGL_SHADE_LIT = 1 };

/* GS TEX0.TFX texture combine (asset-authored in the GIFtag). */
enum { RGL_TFX_MODULATE = 0, RGL_TFX_DECAL = 1,
       RGL_TFX_HIGHLIGHT = 2, RGL_TFX_HIGHLIGHT2 = 3 };

/* The directional-light rig for LIT mode. RENDER-MATH.md §6: the real values
 * are runtime VuMem uploads (0x40..0xa0), NOT in the static ELF -- so these are
 * documented PLACEHOLDERS pending an oracle VuMem dump. The *form* (3 directional
 * lights + ambient, max(0) clamp, (x) vertex colour, saturate) is the traced,
 * certain model; only the numbers are provisional. */
typedef struct {
    float dir[3][3];   /* L : 3 light directions (toward light); auto-normalised */
    float col[3][3];   /* C : 3 light RGB colours                                */
    float ambient[3];  /* A : ambient RGB                                         */
    float saturate;    /* MINIi I clamp, as a 0..N fraction (1.0 = clamp to white)*/
} rgl_lightrig;

/* Full per-vertex attributes carried through the purist pipeline (the format
 * RENDER-MATH.md describes: position, normal, UV, RGBA vertex colour). This is
 * what a future real-model loader -- once it emits normals/colour/UV -- would
 * hand rgl_render_mesh(). */
typedef struct {
    float px, py, pz;   /* position, dequantised to world units        */
    float nx, ny, nz;   /* normal (object space; VU0-skinned on PS2)    */
    float u, v;         /* UV in [0..1], NOT pre-divided by w          */
    float r, g, b, a;   /* vertex RGBA, 0..1 (PS2 128/255 == "neutral")*/
} RVertex;

typedef struct {
    int   width, height;     /* output size            */
    float angle_deg;         /* orbit azimuth          */
    float elev_deg;          /* orbit elevation        */
    int   mode;              /* RGL_* (DocModel path)  */
    int   bilinear;          /* texture filter         */
    int   backface_cull;     /* 0 = two-sided (default)*/

    /* optional texture (RGBA8, top-left origin); tex_rgba == NULL -> untextured */
    const unsigned char *tex_rgba;
    int   tex_w, tex_h;

    /* ---- purist (rgl_render_mesh) shading controls ---- */
    int   shade_mode;        /* RGL_SHADE_PRELIT / RGL_SHADE_LIT       */
    const rgl_lightrig *rig; /* lights for LIT mode; NULL -> built-in placeholder */
    int   tfx;               /* RGL_TFX_* texture combine              */
    int   fog_enable;        /* 1 -> apply the affine fog term         */
    float fog_a, fog_b;      /* F = clamp(a*w + b, fog_min, 1)         */
    float fog_min;
    float fog_color[3];      /* GS FOG colour blended toward as F->0   */
    float alpha_ref;         /* alpha-test threshold (0..1); <0 disables */
    int   wire;              /* 1 -> wireframe overlay                 */

    /* ---- FREE (explicit) CAMERA ----
     * When use_explicit_cam != 0, rgl_render_scene() (and rgl_render_mesh())
     * IGNORE the auto-framed orbit (angle_deg/elev_deg/dist_scale) and instead
     * place the camera at `eye`, looking at `target`, with `up` (a zero vector
     * -> world up {0,1,0}) and a vertical FOV of `fovy_deg` (<=0 -> 45). The
     * near/far planes are derived from the scene's focus box so an eye placed
     * INSIDE the room (near floor level, looking across it) frames the textured
     * front-faces the game's own cameras saw. Leave use_explicit_cam = 0 to keep
     * the legacy orbit path (render_char/render_scene/render_env_textured). */
    int   use_explicit_cam;  /* 0 -> orbit (default); 1 -> eye/target/up/fovy  */
    float eye[3];            /* camera position, world units                   */
    float target[3];         /* look-at point, world units                     */
    float up[3];             /* up hint; {0,0,0} -> {0,1,0}                     */
    float fovy_deg;          /* vertical field of view, degrees; <=0 -> 45     */
} rgl_params;

/* Render `m` with `p`. On success writes a freshly malloc'd width*height*4 RGBA
 * buffer (top-left origin, caller frees) to *out_rgba and returns 0. On failure
 * returns non-zero and writes a reason into err (size errlen). Requires a
 * current GL 3.3-core context. */
int rgl_render(const DocModel *m, const rgl_params *p,
               unsigned char **out_rgba, char *err, int errlen);

/* Render a ready-built triangle list (`verts`, `vcount` vertices = 3*triangles)
 * through the PURIST RENDER-MATH shader pair, auto-framed from [bbmin,bbmax].
 * Honours p->shade_mode (PRELIT/LIT), p->rig (lights), the optional texture and
 * p->tfx combine, and p->fog_*. This is the real original-look shading path;
 * rgl_render() above is a DocModel adapter around the legacy flat shader.
 * Same output/return contract as rgl_render(). */
int rgl_render_mesh(const RVertex *verts, size_t vcount,
                    const float bbmin[3], const float bbmax[3],
                    const rgl_params *p,
                    unsigned char **out_rgba, char *err, int errlen);

/* ------------------------------------------------------------- SCENE path ---
 * One mesh in a multi-mesh GL world. Each draw carries its own geometry, shade
 * mode and optional texture; the whole set shares one camera and one depth
 * buffer (so the character and the environment depth-sort against each other).
 * This is how a composed SCENE -- a character standing in a BMD environment --
 * is drawn in a single framed image. */
typedef struct {
    const RVertex *verts;
    size_t         vcount;          /* = 3 * triangles                       */
    int            shade_mode;      /* RGL_SHADE_PRELIT (env) / _LIT (actor) */
    const unsigned char *tex_rgba;  /* optional RGBA8 top-left; NULL = none  */
    int            tex_w, tex_h;
    int            tfx;             /* RGL_TFX_* texture combine             */
    /* GS alpha-tested blended pass (TEST ATE=1 ATST=GEQUAL AFAIL=RGB_ONLY, ALPHA
     * (Cs-Cd)*As+Cd). 0 = off (opaque draw; zero-init keeps old callers opaque).
     * 1 = colour blends by As everywhere, depth is written only where
     * As >= alpha_ref (0..1, As=1.0 at GS 128). Texels below the ref leave no depth
     * and (near As=0) no colour, so what is behind them shows through: the sky drawn
     * before them (game order), or anything drawn later. */
    int            gs_atest;
    float          alpha_ref;
    /* 1 = this draw ignores p->fog_* (e.g. a sky dome the game fogs with its own
     * colour). 0 (zero-init) = fogged whenever p->fog_enable is set. */
    int            no_fog;
    /* LIT only: also light the -n side and pick per fragment by facing (a cloth drawn
     * double-sided, e.g. the cape: the game draws a front and a back copy). 0 (zero-init)
     * = off; 1 = the +n side is GL front-facing; 2 = the +n side is GL back-facing;
     * 3 = per fragment, the side whose normal faces the eye (explicit camera only; works
     * when one draw mixes winding conventions, e.g. Vincent's body vs cape). */
    int            two_sided;
} rgl_scene_draw;

/* Render `ndraws` meshes into one GL world and read back one framed image.
 * The orbit camera is auto-framed from [focus_min,focus_max] -- so you can frame
 * on a hero object (e.g. the placed character) -- at azimuth/elevation p->angle_deg
 * / p->elev_deg. `dist_scale` (>0) pulls the eye back to include surrounding
 * context: 1.0 tightly frames the focus box, larger shows more environment.
 * Common controls (width/height, bilinear, rig, fog) come from `p`; per-draw
 * shade_mode/texture/tfx come from each `rgl_scene_draw`. Same output/return
 * contract as rgl_render_mesh(): malloc'd W*H*4 RGBA top-left on success. */
int rgl_render_scene(const rgl_scene_draw *draws, int ndraws,
                     const float focus_min[3], const float focus_max[3],
                     float dist_scale, const rgl_params *p,
                     unsigned char **out_rgba, char *err, int errlen);

/* The built-in placeholder light rig (RENDER-MATH §6) used when p->rig == NULL. */
const rgl_lightrig *rgl_default_lightrig(void);

#ifdef __cplusplus
}
#endif
#endif /* RENDERER_GL_H */
