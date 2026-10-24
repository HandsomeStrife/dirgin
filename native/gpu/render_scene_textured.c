/* render_scene_textured.c -- the flagship composed SCENE on the native GPU: the
 * decoded, TEXTURED Dirge of Cerberus character (char_e000 / Vincent, LIT + body
 * atlas) standing inside the decoded, TEXTURED BMD room (a Shinra sci-fi interior,
 * every material group PRELIT + its own TXD texture), both REAL native assets,
 * drawn by our own OpenGL 3.3-core renderer through the PURIST RENDER-MATH shader
 * pair, into ONE GL world with ONE shared depth buffer, via EGL-offscreen -> PNG.
 *
 * This is render_scene.c (grey env) + render_env_textured.c (textured room) fused:
 *   ENVIRONMENT  bmd_<room>_tex.list  -- N material groups, each a DMSH triangle
 *                soup (pos3,nrm3,uv2,rgba4) with REAL decoded UVs + baked PRELIT
 *                vertex colours, drawn PRELIT + GS-MODULATE + its own TXD texture
 *                (largest image, decoded by libdtex read-only). Same coordinate
 *                frame as the grey combined mesh render_scene.c used, so the
 *                hand-set character placement below carries over unchanged.
 *   CHARACTER    char_e000.mesh  -- posed+skinned actor, drawn LIT (3 directional
 *                lights + ambient) + the body atlas (TXD 07650002, largest image).
 *
 * The two assets do NOT share an authored scene-placement frame (the MOD actor is
 * in bone-local units ~20 tall; the BMD room is world float units ~150 tall). The
 * character placement is therefore a HAND-SET transform (uniform scale + yaw +
 * translate), documented in place_character() and in the defaults below, chosen so
 * the character reads as a person standing on the room floor at a plausible scale.
 * Everything lands in one GL world / one depth buffer / one camera, so char and
 * room depth-sort against each other.
 *
 * Env PRELIT display brightness: the room's baked PRELIT vertex colours are a dark
 * interior. -colscale <f> multiplies the env prelit RGB by f (a documented display
 * gain that keeps the real relative shading but lifts the room out of black);
 * -neutralcol forces neutral vertex colour so the decoded textures show at full
 * brightness (texture-only variant). Neither touches the character.
 *
 *   render_scene_textured [-l ENV.list] [-m CHAR.mesh] [-t CHAR_TEX] [-o OUTDIR]
 *                         [-s scale] [-yaw deg] [-x cx] [-y feetY] [-z cz]
 *                         [-colscale f] [-neutralcol] [-flipy] [-notex] [-tag T]
 *
 * Additive: links renderer_gl/gl_api/gfx_context/libdtex READ-ONLY, modifies none.
 */
#include "dtex.h"
#include "gfx_context.h"
#include "renderer_gl.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAXG 64

typedef struct { RVertex *v; size_t n; float bbmin[3], bbmax[3];
                 unsigned char *tex; int tw, th; char texname[64];
                 float aref; char meshf[256], texf[512]; } Group;   /* aref < 0: opaque; else GS ATE pass (list col 4) */

/* Load a bounds-checked "DMSH" triangle soup (the untrusted on-disk format). */
static int mesh_load(const char *path, RVertex **vout, size_t *nout,
                     float bbmin[3], float bbmax[3], char *err, size_t errlen) {
    *vout = NULL; *nout = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open mesh %s", path); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsz < 0 || (size_t)fsz < 4 + 4 + 4 + 24) {
        snprintf(err, errlen, "mesh too small (%ld)", fsz); fclose(f); return 1; }
    char magic[4]; uint32_t version = 0, vcount = 0; float bb[6];
    int ok = fread(magic, 1, 4, f) == 4 && fread(&version, 4, 1, f) == 1 &&
             fread(&vcount, 4, 1, f) == 1 && fread(bb, 4, 6, f) == 6;
    if (!ok || memcmp(magic, "DMSH", 4) != 0) {
        snprintf(err, errlen, "bad mesh magic %s", path); fclose(f); return 1; }
    long remain = fsz - (long)(4 + 4 + 4 + 24);
    if (version != 1 || vcount == 0 || vcount % 3 != 0 ||
        (long)vcount * (long)sizeof(RVertex) != remain) {
        snprintf(err, errlen, "mesh size mismatch (v=%u ver=%u remain=%ld)",
                 vcount, version, remain); fclose(f); return 1; }
    RVertex *v = (RVertex *)malloc((size_t)vcount * sizeof(RVertex));
    if (!v) { snprintf(err, errlen, "oom"); fclose(f); return 1; }
    if (fread(v, sizeof(RVertex), vcount, f) != vcount) {
        snprintf(err, errlen, "short read"); free(v); fclose(f); return 1; }
    fclose(f);
    *vout = v; *nout = vcount;
    memcpy(bbmin, bb, 12); memcpy(bbmax, bb + 3, 12);
    return 0;
}

/* Decode the LARGEST image from a TXD/RST container via libdtex (read-only). */
static int tex_load_largest(const char *path, unsigned char **rgba, int *w, int *h,
                            char *name, size_t namelen) {
    *rgba = NULL; *w = *h = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = (n > 0) ? (uint8_t *)malloc((size_t)n) : NULL;
    int ok = (buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    if (!ok) { free(buf); return 1; }
    dtex_set set;
    int rc = dtex_decode_container(buf, (size_t)n, &set);
    free(buf);
    if (rc != 0 || set.count < 1) { if (rc == 0) dtex_free_set(&set); return 1; }
    int best = 0; long best_px = -1;
    for (int i = 0; i < set.count; i++) {
        long px = (long)set.items[i].width * set.items[i].height;
        if (px > best_px) { best_px = px; best = i; }
    }
    dtex_image *im = &set.items[best];
    *w = im->width; *h = im->height;
    *rgba = (unsigned char *)malloc((size_t)(*w) * (*h) * 4);
    if (!*rgba) { dtex_free_set(&set); return 1; }
    memcpy(*rgba, im->rgba, (size_t)(*w) * (*h) * 4);
    if (name && namelen) snprintf(name, namelen, "%s %dx%d", im->source, *w, *h);
    dtex_free_set(&set);
    return 0;
}

/* Load a raw RGBA sidecar: 8-byte little-endian header (u32 width, u32 height)
 * then width*height*4 bytes of RGBA8. This lets -t accept a pre-decoded atlas
 * (e.g. the MULTI-atlas Vincent texture: his red cloak lives in a different
 * dtex image than his body, so no single "largest image" container holds the
 * whole character -- the atlases are combined into one PNG, converted to .rgba). */
static int tex_load_raw(const char *path, unsigned char **rgba, int *w, int *h,
                        char *name, size_t namelen) {
    *rgba = NULL; *w = *h = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    uint32_t hdr[2];
    if (fread(hdr, 4, 2, f) != 2) { fclose(f); return 1; }
    long ww = (long)hdr[0], hh = (long)hdr[1];
    if (ww <= 0 || hh <= 0 || ww > 8192 || hh > 8192) { fclose(f); return 1; }
    size_t nbytes = (size_t)ww * (size_t)hh * 4;
    unsigned char *buf = (unsigned char *)malloc(nbytes);
    if (!buf) { fclose(f); return 1; }
    if (fread(buf, 1, nbytes, f) != nbytes) { free(buf); fclose(f); return 1; }
    fclose(f);
    *rgba = buf; *w = (int)ww; *h = (int)hh;
    if (name && namelen) snprintf(name, namelen, "raw %ldx%ld", ww, hh);
    return 0;
}

/* dispatch on extension: .rgba -> raw sidecar; else dtex container (largest image). */
static int tex_load_any(const char *path, unsigned char **rgba, int *w, int *h,
                        char *name, size_t namelen) {
    size_t L = strlen(path);
    if (L > 5 && strcmp(path + L - 5, ".rgba") == 0)
        return tex_load_raw(path, rgba, w, h, name, namelen);
    return tex_load_largest(path, rgba, w, h, name, namelen);
}

/* dir(list) so relative mesh basenames resolve next to the .list file. */
static void dirof(const char *path, char *dir, size_t n) {
    snprintf(dir, n, "%s", path);
    char *s = strrchr(dir, '/');
    if (s) *s = 0; else snprintf(dir, n, ".");
}

/* hand-set character placement (identical math to render_scene.c):
 * p' = R_yaw( s * (p - pivot) ) + (TX, FY, TZ), pivot = (char XZ centre, feet Y).
 * Feet land on FY, horizontal centre on (TX,TZ). Uniform positive scale preserves
 * normals; only yaw rotates them. Returns a NEW RVertex array + its world bbox. */
static RVertex *place_character(const RVertex *src, size_t n,
                                const float smin[3], const float smax[3],
                                float s, float yaw_deg, float TX, float FY, float TZ,
                                float up_y, float out_min[3], float out_max[3]) {
    RVertex *d = (RVertex *)malloc(n * sizeof(RVertex));
    if (!d) return NULL;
    float pivx = (smin[0] + smax[0]) * 0.5f;
    float pivz = (smin[2] + smax[2]) * 0.5f;
    /* Feet = the lowest point along the room's up-axis. In a +Y-up room that is
     * min-Y; in a -Y-up room (cam up_y<0, e.g. sea-of-flames z128) it is max-Y, and
     * the body then extends toward -Y (up) instead of +Y. FY is always the feet. */
    float pivy = (up_y < 0.0f) ? smax[1] : smin[1];
    float cy = cosf((float)(yaw_deg * M_PI / 180.0));
    float sy = sinf((float)(yaw_deg * M_PI / 180.0));
    for (int k = 0; k < 3; k++) { out_min[k] = 1e30f; out_max[k] = -1e30f; }
    for (size_t i = 0; i < n; i++) {
        const RVertex *v = &src[i];
        float px = (v->px - pivx) * s;
        float py = (v->py - pivy) * s;
        float pz = (v->pz - pivz) * s;
        float rx =  cy * px + sy * pz;            /* yaw about Y */
        float rz = -sy * px + cy * pz;
        float wx = rx + TX, wy = py + FY, wz = rz + TZ;
        float nx =  cy * v->nx + sy * v->nz;
        float nz = -sy * v->nx + cy * v->nz;
        d[i] = *v;
        d[i].px = wx; d[i].py = wy; d[i].pz = wz;
        d[i].nx = nx; d[i].nz = nz;               /* ny unchanged */
        float wp[3] = { wx, wy, wz };
        for (int k = 0; k < 3; k++) {
            if (wp[k] < out_min[k]) out_min[k] = wp[k];
            if (wp[k] > out_max[k]) out_max[k] = wp[k];
        }
    }
    return d;
}

int main(int argc, char **argv) {
    const char *listpath = "../assets/models/out/bmd_06960212_tex.list";
    const char *charpath = "../assets/models/out/char_e000.mesh";
    const char *chartex  = "/srv/games/dirge-port/extract/_KEL/noPath/07650002.bin";
    const char *outdir   = "out";
    const char *tag      = "21_scene_textured";
    /* HAND-SET placement (see place_character + header). Same room coordinate frame
     * as render_scene.c's grey env, so these carry over. The character stands on
     * the middle floor slab (feet at Y=0; room spans Y[-60,90]) near the open
     * +X/+Z corner of the room (world (268,0,330), room X[110,315] Z[120,370]), so
     * the camera sees it against the interior sci-fi partitions with the roof slab
     * above rather than buried under a ceiling. scale=2.6 puts the ~19.5-unit actor
     * at ~50.7 world units tall -- about 1/3 the ~150-unit room height, i.e. a
     * human against large industrial wall sections. yaw=160 turns it to a 3/4
     * profile toward the hero camera (azimuth 44). These are the TUNED HERO values;
     * every one is overridable on the command line. */
    float scale = 2.6f, yaw = 160.f, TX = 268.f, FY = 0.f, TZ = 330.f;
    /* env PRELIT display gain. The room's baked PRELIT vertex colours are REAL but
     * a dark interior; 1.8 is a documented exposure gain (multiply+clamp) that lifts
     * the room out of near-black while preserving its real relative shading. It
     * touches ONLY the environment, never the LIT character. -colscale 1.0 shows the
     * room exactly as-baked; -neutralcol shows the decoded textures at full bright. */
    float colscale = 1.8f;
    int neutralcol = 0, flipy = 0, notex = 0, charprelit = 0;
    float cam_eye[3], cam_at[3], cam_up[3] = {0,1,0}, cam_fov = 50.f;  /* -eye/-at free camera */
    int have_eye = 0, have_at = 0;
    int opt_W = 1280, opt_H = 960;   /* -W/-H override output size (anamorphic match) */

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-l")  && i+1<argc) listpath = argv[++i];
        else if (!strcmp(argv[i], "-m")  && i+1<argc) charpath = argv[++i];
        else if (!strcmp(argv[i], "-t")  && i+1<argc) chartex  = argv[++i];
        else if (!strcmp(argv[i], "-o")  && i+1<argc) outdir   = argv[++i];
        else if (!strcmp(argv[i], "-tag")&& i+1<argc) tag      = argv[++i];
        else if (!strcmp(argv[i], "-s")  && i+1<argc) scale = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-yaw")&& i+1<argc) yaw   = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-x")  && i+1<argc) TX    = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-y")  && i+1<argc) FY    = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-z")  && i+1<argc) TZ    = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-colscale") && i+1<argc) colscale = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-neutralcol")) neutralcol = 1;
        else if (!strcmp(argv[i], "-charprelit")) charprelit = 1;
        else if (!strcmp(argv[i], "-flipy")) flipy = 1;
        else if (!strcmp(argv[i], "-notex")) notex = 1;
        else if (!strcmp(argv[i],"-eye")&&i+1<argc) have_eye=(sscanf(argv[++i],"%f,%f,%f",&cam_eye[0],&cam_eye[1],&cam_eye[2])==3);
        else if (!strcmp(argv[i],"-at") &&i+1<argc) have_at =(sscanf(argv[++i],"%f,%f,%f",&cam_at[0],&cam_at[1],&cam_at[2])==3);
        else if (!strcmp(argv[i],"-up") &&i+1<argc) sscanf(argv[++i],"%f,%f,%f",&cam_up[0],&cam_up[1],&cam_up[2]);
        else if (!strcmp(argv[i],"-fov")&&i+1<argc) cam_fov=(float)atof(argv[++i]);
        else if (!strcmp(argv[i],"-W")&&i+1<argc) opt_W=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-H")&&i+1<argc) opt_H=atoi(argv[++i]);
    }

    /* ---- load the textured environment groups ---- */
    char ldir[1024]; dirof(listpath, ldir, sizeof ldir);
    FILE *lf = fopen(listpath, "r");
    if (!lf) { fprintf(stderr, "cannot open list %s\n", listpath); return 2; }
    Group g[MAXG]; int ng = 0;
    /* @fog R G B near far min : GS per-vertex fog, F = clamp((far-w)/(far-near), min, 1)
     * with w = view depth (world units); colour = mix(FOGCOL, colour, F). Applies to
     * every env group and the character, except groups named by @nofog <mesh>.
     * Absent = no fog (the image is unchanged). Any malformed directive is fatal:
     * rendering on without it would silently drop the fog. */
    int have_fog = 0; float fogc[3] = {0,0,0}, fog_near = 0, fog_far = 0, fog_min = 0;
    char nofog[MAXG][256]; int nnofog = 0; int lineno = 0, bad_directive = 0;
    float rmin[3] = { 1e30f, 1e30f, 1e30f }, rmax[3] = { -1e30f, -1e30f, -1e30f };
    char line[2048];
    while (fgets(line, sizeof line, lf) && ng < MAXG) {
        lineno++;
        const char *ln = line;
        while (*ln == ' ' || *ln == '\t') ln++;
        if (*ln == '@') {
            char kw[32] = {0}, extra[8] = {0};
            sscanf(ln, "@%31s", kw);
            if (!strcmp(kw, "fog")) {
                float r, gg, b, n, f, mn;
                int k = sscanf(ln, "@fog %f %f %f %f %f %f %7s", &r, &gg, &b, &n, &f, &mn, extra);
                const char *why = NULL;
                if (have_fog) why = "second @fog line";
                else if (k != 6) why = "want exactly '@fog R G B near far min'";
                else if (!isfinite(r) || !isfinite(gg) || !isfinite(b) || !isfinite(n) ||
                         !isfinite(f) || !isfinite(mn)) why = "non-finite value";
                else if (r < 0 || r > 255 || gg < 0 || gg > 255 || b < 0 || b > 255)
                    why = "R G B must be 0..255";
                else if (!(f > n)) why = "far must be greater than near";
                else if (mn < 0 || mn > 1) why = "min must be 0..1";
                if (why) {
                    fprintf(stderr, "error: %s:%d: bad @fog (%s): %s", listpath, lineno, why, line);
                    bad_directive = 1; continue;
                }
                have_fog = 1; fogc[0] = r / 255.f; fogc[1] = gg / 255.f; fogc[2] = b / 255.f;
                fog_near = n; fog_far = f; fog_min = mn;
            } else if (!strcmp(kw, "nofog")) {
                char mname[256] = {0};
                if (sscanf(ln, "@nofog %255s %7s", mname, extra) != 1) {
                    fprintf(stderr, "error: %s:%d: bad @nofog (want '@nofog <mesh>'): %s",
                            listpath, lineno, line);
                    bad_directive = 1; continue;
                }
                if (nnofog >= MAXG) {
                    fprintf(stderr, "error: %s:%d: too many @nofog lines (max %d)\n",
                            listpath, lineno, MAXG);
                    bad_directive = 1; continue;
                }
                snprintf(nofog[nnofog++], sizeof nofog[0], "%s", mname);
            } else {
                fprintf(stderr, "error: %s:%d: unknown directive: %s", listpath, lineno, line);
                bad_directive = 1;
            }
            continue;
        }
        /* columns: mesh, texture container, ABE, [alpha-test ref]. Col 3 (ABE) is
         * informational. Col 4, when present and >= 0, marks a GS alpha-tested
         * blended pass (export_z128_room.py ALPHA_TEST); absent = opaque. */
        char meshf[1024] = {0}, texf[1024] = {0}, areff[64] = {0}; int abe = 0; float aref = -1.0f;
        if (sscanf(line, "%1023s\t%1023s\t%d\t%63s", meshf, texf, &abe, areff) < 1) continue;
        if (meshf[0] == 0 || meshf[0] == '#') continue;
        /* col 4: absent or -1 = opaque, else a ref in [0,1]. Anything else (garbled,
         * NaN, out of range, e.g. '0,75') is drawn opaque, but says so. */
        if (areff[0]) {
            char *end = NULL; float x = strtof(areff, &end);
            if (end != areff && *end == 0 && x == -1.0f) aref = -1.0f;
            else if (end != areff && *end == 0 && x >= 0.0f && x <= 1.0f) aref = x;
            else fprintf(stderr, "warning: %s: bad alpha-test ref '%s' in column 4 "
                         "(want -1 or 0..1); drawing this group OPAQUE, no alpha test\n",
                         meshf, areff);
        }
        char meshpath[2048];
        snprintf(meshpath, sizeof meshpath, "%s/%s", ldir, meshf);
        char err[256] = {0};
        RVertex *v = NULL; size_t n = 0; float bbmn[3], bbmx[3];
        if (mesh_load(meshpath, &v, &n, bbmn, bbmx, err, sizeof err)) {
            fprintf(stderr, "skip %s: %s\n", meshf, err); continue; }
        g[ng].v = v; g[ng].n = n;
        memcpy(g[ng].bbmin, bbmn, 12); memcpy(g[ng].bbmax, bbmx, 12);
        g[ng].tex = NULL; g[ng].tw = g[ng].th = 0; g[ng].texname[0] = 0; g[ng].aref = aref;
        snprintf(g[ng].meshf, sizeof g[ng].meshf, "%s", meshf);
        snprintf(g[ng].texf, sizeof g[ng].texf, "%s", texf[0] ? texf : "no column 2");
        if (!notex && strcmp(texf, "NONE") != 0) {
            if (tex_load_largest(texf, &g[ng].tex, &g[ng].tw, &g[ng].th,
                                 g[ng].texname, sizeof g[ng].texname))
                fprintf(stderr, "warn: tex decode failed for group %d (%s)\n", ng, texf);
        }
        for (int k = 0; k < 3; k++) {
            if (bbmn[k] < rmin[k]) rmin[k] = bbmn[k];
            if (bbmx[k] > rmax[k]) rmax[k] = bbmx[k];
        }
        ng++;
    }
    fclose(lf);
    if (bad_directive) {
        fprintf(stderr, "error: %s: fix the directive(s) above; not rendering\n", listpath);
        return 2;
    }
    if (ng == 0) { fprintf(stderr, "no env groups loaded\n"); return 2; }
    if (nnofog && !have_fog)
        fprintf(stderr, "warning: %s: @nofog without @fog does nothing\n", listpath);
    for (int k = 0; k < nnofog; k++) {
        int hit = 0;
        for (int i = 0; i < ng; i++) if (!strcmp(g[i].meshf, nofog[k])) hit++;
        if (!hit) fprintf(stderr, "warning: @nofog %s matches no group in %s; "
                          "that mesh will NOT be exempt\n", nofog[k], listpath);
    }

    /* env PRELIT brightness controls (character untouched) */
    if (neutralcol) {
        for (int i = 0; i < ng; i++)
            for (size_t k = 0; k < g[i].n; k++) {
                g[i].v[k].r = g[i].v[k].g = g[i].v[k].b = 0.5f; g[i].v[k].a = 0.5f;
            }
    } else if (colscale != 1.0f) {
        for (int i = 0; i < ng; i++)
            for (size_t k = 0; k < g[i].n; k++) {
                float r = g[i].v[k].r * colscale, gg = g[i].v[k].g * colscale, b = g[i].v[k].b * colscale;
                g[i].v[k].r = r > 1.f ? 1.f : r;
                g[i].v[k].g = gg > 1.f ? 1.f : gg;
                g[i].v[k].b = b > 1.f ? 1.f : b;
            }
    }
    if (flipy) {
        for (int i = 0; i < ng; i++)
            for (size_t k = 0; k < g[i].n; k++) { g[i].v[k].py = -g[i].v[k].py; g[i].v[k].ny = -g[i].v[k].ny; }
        float t = rmin[1]; rmin[1] = -rmax[1]; rmax[1] = -t;
    }

    size_t envtri = 0; for (int i = 0; i < ng; i++) envtri += g[i].n / 3;
    printf("env   : %s  groups=%d  tris=%zu  bbox X[%.1f,%.1f] Y[%.1f,%.1f] Z[%.1f,%.1f]\n",
           listpath, ng, envtri, rmin[0],rmax[0],rmin[1],rmax[1],rmin[2],rmax[2]);

    /* ---- load + place the character ---- */
    char err[256] = {0};
    RVertex *cv = NULL; size_t cn = 0; float cbmn[3], cbmx[3];
    if (mesh_load(charpath, &cv, &cn, cbmn, cbmx, err, sizeof err)) {
        fprintf(stderr, "char: %s\n", err);
        for (int i = 0; i < ng; i++) { free(g[i].v); free(g[i].tex); } return 2; }
    if (flipy) {  /* keep the actor consistent with a Y-flipped room */
        for (size_t k = 0; k < cn; k++) { cv[k].py = -cv[k].py; cv[k].ny = -cv[k].ny; }
        float t = cbmn[1]; cbmn[1] = -cbmx[1]; cbmx[1] = -t;
    }
    printf("char  : %s  (%zu tris)  raw height %.2f\n", charpath, cn/3, cbmx[1]-cbmn[1]);

    float cmin[3], cmax[3];
    RVertex *placed = place_character(cv, cn, cbmn, cbmx, scale, yaw, TX, FY, TZ, cam_up[1], cmin, cmax);
    free(cv);
    if (!placed) { fprintf(stderr, "placement alloc failed\n");
        for (int i = 0; i < ng; i++) { free(g[i].v); free(g[i].tex); } return 3; }
    printf("placed: scale=%.2f yaw=%.0f feet=(%.1f,%.1f,%.1f)  bbox X[%.1f,%.1f] Y[%.1f,%.1f] Z[%.1f,%.1f]  worldH=%.1f\n",
           scale, yaw, TX, FY, TZ, cmin[0],cmax[0],cmin[1],cmax[1],cmin[2],cmax[2], cmax[1]-cmin[1]);

    /* ---- GL ---- */
    const int W = (opt_W > 0 ? opt_W : 1280), H = (opt_H > 0 ? opt_H : 960);
    char cerr[256] = {0};
    gfx_context *ctx = gfx_context_create(W, H, cerr, sizeof cerr);
    if (!ctx) { fprintf(stderr, "GL init failed: %s\n", cerr);
        free(placed); for (int i = 0; i < ng; i++) { free(g[i].v); free(g[i].tex); } return 4; }
    printf("gl    : %s\n", gfx_context_describe(ctx));

    unsigned char *ctex = NULL; int ctw = 0, cth = 0; char ctname[64] = {0};
    if (!notex && tex_load_any(chartex, &ctex, &ctw, &cth, ctname, sizeof ctname))
        fprintf(stderr, "warning: char texture decode failed (%s); char untextured\n", chartex);
    else if (!notex)
        printf("ctex  : %s  (%s)\n", chartex, ctname);

    rgl_params p; memset(&p, 0, sizeof p);
    p.width = W; p.height = H; p.bilinear = 1; p.backface_cull = 0;
    p.rig = rgl_default_lightrig(); p.alpha_ref = -1.0f;
    if (have_fog) {
        p.fog_enable = 1;
        p.fog_a = -1.0f / (fog_far - fog_near);       /* F = a*w + b */
        p.fog_b = fog_far / (fog_far - fog_near);
        p.fog_min = fog_min;
        memcpy(p.fog_color, fogc, sizeof fogc);
        printf("fog   : colour %.0f,%.0f,%.0f  F=1 at w=%.2f, 0 at w=%.2f, floor %.3f\n",
               fogc[0]*255, fogc[1]*255, fogc[2]*255, fog_near, fog_far, fog_min);
    }

    /* draws: every env group PRELIT + its texture, then the character LIT + body */
    rgl_scene_draw draws[MAXG + 1];
    memset(draws, 0, sizeof draws);
    for (int i = 0; i < ng; i++) {
        draws[i].verts = g[i].v; draws[i].vcount = g[i].n;
        draws[i].shade_mode = RGL_SHADE_PRELIT; draws[i].tfx = RGL_TFX_MODULATE;
        draws[i].tex_rgba = g[i].tex; draws[i].tex_w = g[i].tw; draws[i].tex_h = g[i].th;
        for (int k = 0; k < nnofog; k++)
            if (!strcmp(g[i].meshf, nofog[k])) draws[i].no_fog = 1;
        if (have_fog && draws[i].no_fog) printf("fog   : group %d (%s) not fogged\n", i, g[i].meshf);
        if (g[i].aref >= 0.0f && g[i].tex) {
            draws[i].gs_atest = 1; draws[i].alpha_ref = g[i].aref;
            printf("atest : group %d ref %.4f (blend, depth only where As >= ref)\n", i, g[i].aref);
        } else if (g[i].aref >= 0.0f && !notex) {
            fprintf(stderr, "warning: group %d (%s): alpha-test ref %.4f but no texture loaded "
                    "(%s); alpha test DROPPED, drawn opaque\n",
                    i, g[i].meshf, g[i].aref, g[i].texf);
        }
    }
    /* The character body atlas can only be MODULATE-combined onto the mesh if
     * the mesh carries real per-vertex UVs. char_e000.mesh (the render_skinned.py
     * export) currently collapses almost all of its UVs onto (0,0) -- which is
     * the atlas's TRANSPARENT padding corner (texel 0,0,0,0). MODULATE there
     * yields rgb=0, alpha=0 over the whole body, so the texture erases both the
     * body colour AND the lit shading, and the alpha=0 makes the actor read as a
     * transparent (page-white) silhouette. Until the mesh exporter emits proper
     * UVs, detect that degeneracy and draw the actor LIT + UNtextured, so its
     * authentic directional shading (cape / hair / arms) is what shows. A mesh
     * with real UVs is textured normally. */
    size_t uv_zero = 0;
    for (size_t k = 0; k < cn; k++)
        if (fabsf(placed[k].u) < 1e-4f && fabsf(placed[k].v) < 1e-4f) uv_zero++;
    int char_uv_degenerate = (cn > 0 && uv_zero * 10 >= cn * 9);  /* >=90% at (0,0) */
    if (char_uv_degenerate)
        printf("char  : UVs degenerate (%zu/%zu at origin) -> LIT untextured (atlas unmappable)\n",
               uv_zero, cn);

    draws[ng].verts = placed; draws[ng].vcount = cn;
    /* -charprelit: shade the actor by its baked GS vertex colour (texel*vcol, like
     * the game's GIF modulate) instead of a synthetic light rig, which over-brightens
     * near-vertical surfaces (e.g. hair) into a washed/blonde look. */
    draws[ng].shade_mode = charprelit ? RGL_SHADE_PRELIT : RGL_SHADE_LIT;
    draws[ng].tfx = RGL_TFX_MODULATE;
    if (char_uv_degenerate) {
        draws[ng].tex_rgba = NULL; draws[ng].tex_w = 0; draws[ng].tex_h = 0;
    } else {
        draws[ng].tex_rgba = ctex; draws[ng].tex_w = ctw; draws[ng].tex_h = cth;
    }
    int ndraws = ng + 1;

    /* frame on the CHARACTER box, pull the eye back to show the room around it */
    struct { const char *tag; float az, el, zoom; } views[] = {
        { "hero",   44.f, 11.f, 1.85f },
        { "corner", 22.f,  9.f, 2.15f },
        { "wide",   58.f, 21.f, 2.9f },
    };

    char path[1024]; int fail = 0;
    if (have_eye && have_at) {
        /* FREE CAMERA: eye/target placed explicitly (room-angle money shot).
         * Focus box = the room bbox (rmin/rmax) so near/far cover the scene. */
        p.use_explicit_cam = 1;
        memcpy(p.eye, cam_eye, sizeof cam_eye);
        memcpy(p.target, cam_at, sizeof cam_at);
        memcpy(p.up, cam_up, sizeof cam_up);
        p.fovy_deg = cam_fov;
        unsigned char *img = NULL; char rerr[256] = {0};
        int rc = rgl_render_scene(draws, ndraws, rmin, rmax, 1.0f, &p, &img, rerr, sizeof rerr);
        if (rc != 0 || !img) { fprintf(stderr, "free-cam render failed: %s\n", rerr); fail = 1; }
        else {
            snprintf(path, sizeof path, "%s/%s_freecam.png", outdir, tag);
            if (dtex_write_png(path, img, W, H) == 0)
                printf("wrote %s (free cam eye=%.0f,%.0f,%.0f at=%.0f,%.0f,%.0f fov=%.0f)\n",
                       path, cam_eye[0],cam_eye[1],cam_eye[2], cam_at[0],cam_at[1],cam_at[2], cam_fov);
            free(img);
        }
    } else
    for (size_t i = 0; i < sizeof views/sizeof views[0]; i++) {
        p.angle_deg = views[i].az; p.elev_deg = views[i].el;
        unsigned char *img = NULL; char rerr[256] = {0};
        int rc = rgl_render_scene(draws, ndraws, cmin, cmax, views[i].zoom, &p, &img, rerr, sizeof rerr);
        if (rc != 0 || !img) { fprintf(stderr, "scene (%s) failed: %s\n", views[i].tag, rerr); fail = 1; continue; }
        snprintf(path, sizeof path, "%s/%s_%s.png", outdir, tag, views[i].tag);
        rc = dtex_write_png(path, img, W, H);
        free(img);
        if (rc != 0) { fprintf(stderr, "PNG write failed: %s\n", path); fail = 1; continue; }
        printf("wrote %-44s (az=%.0f el=%.0f zoom=%.1f)\n", path, views[i].az, views[i].el, views[i].zoom);
    }

    free(ctex); free(placed);
    for (int i = 0; i < ng; i++) { free(g[i].v); free(g[i].tex); }
    gfx_context_destroy(ctx);
    return fail ? 1 : 0;
}
