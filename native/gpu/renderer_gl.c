/* renderer_gl.c -- the GL 3.3-core render core (portable; no platform code).
 *
 * Pipeline:
 *   - expand DocModel triangle-strips into an unindexed triangle list (same
 *     alternating-winding flip as the software reference), attributes = world
 *     position + provisional UV. Unindexed so each triangle gets its own flat
 *     face normal (matching the reference's per-face flat shading).
 *   - upload VBO + VAO, build the program, create an RGBA8 + depth FBO.
 *   - auto-frame an orbit camera with the SAME math/constants as
 *     ../renderer/renderer.c, draw, read pixels back flipped to top-left.
 *
 * Flat shading is reproduced without a geometry shader: the fragment shader
 * takes the face normal from screen-space derivatives of the view-space
 * position (constant across a flat triangle), then flips it toward the viewer
 * exactly as the two-sided reference does, so the shade value matches.
 */
#include "renderer_gl.h"
#include "gl_api.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG2RAD(d) ((float)((d) * M_PI / 180.0))

/* ------------------------------------------------------------ mat math --- */
/* Column-major, identical to the software reference. */
typedef struct { float x, y, z; } v3;
typedef struct { float m[16]; } mat4;

static v3 v3sub(v3 a, v3 b){ return (v3){a.x-b.x, a.y-b.y, a.z-b.z}; }
static v3 v3cross(v3 a, v3 b){
    return (v3){a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
static float v3dot(v3 a, v3 b){ return a.x*b.x + a.y*b.y + a.z*b.z; }
static v3 v3norm(v3 a){
    float l = sqrtf(a.x*a.x + a.y*a.y + a.z*a.z);
    if (l < 1e-20f) return (v3){0,0,0};
    return (v3){a.x/l, a.y/l, a.z/l};
}
static mat4 mat_mul(mat4 a, mat4 b){
    mat4 r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++){
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k*4+row] * b.m[c*4+k];
            r.m[c*4+row] = s;
        }
    return r;
}
static mat4 mat_lookat(v3 eye, v3 center, v3 up){
    v3 f = v3norm(v3sub(center, eye));
    v3 s = v3norm(v3cross(f, up));
    v3 u = v3cross(s, f);
    mat4 r = {{0}};
    r.m[0]=s.x; r.m[4]=s.y; r.m[8] =s.z;  r.m[12]=-v3dot(s, eye);
    r.m[1]=u.x; r.m[5]=u.y; r.m[9] =u.z;  r.m[13]=-v3dot(u, eye);
    r.m[2]=-f.x;r.m[6]=-f.y;r.m[10]=-f.z; r.m[14]= v3dot(f, eye);
    r.m[15]=1.0f;
    return r;
}
static mat4 mat_persp(float fovy_rad, float aspect, float znear, float zfar){
    float ff = 1.0f / tanf(fovy_rad * 0.5f);
    mat4 r = {{0}};
    r.m[0]  = ff / aspect;
    r.m[5]  = ff;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
    return r;
}

/* --------------------------------------------------------------- shaders - */
static const char *VS_SRC =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "uniform mat4 uMVP;\n"
    "uniform mat4 uMV;\n"       /* model-view (model = identity) */
    "out vec3 vViewPos;\n"
    "out vec2 vUV;\n"
    "void main(){\n"
    "    vec4 vp = uMV * vec4(aPos,1.0);\n"
    "    vViewPos = vp.xyz;\n"
    "    vUV = aUV;\n"
    "    gl_Position = uMVP * vec4(aPos,1.0);\n"
    "}\n";

/* Flat per-face shading via derivatives; two-sided flip toward the viewer, then
 * N.L directional + ambient, matching ../renderer/renderer.c raster_tri(). */
static const char *FS_SRC =
    "#version 330 core\n"
    "in vec3 vViewPos;\n"
    "in vec2 vUV;\n"
    "uniform vec3  uLightDir;\n"   /* view-space dir TOWARD the light */
    "uniform float uAmbient;\n"
    "uniform int   uMode;\n"       /* 0 textured, 1 shaded, 2 wire */
    "uniform int   uUseTex;\n"
    "uniform vec3  uSurface;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 fragColor;\n"
    "void main(){\n"
    "    vec3 n = normalize(cross(dFdx(vViewPos), dFdy(vViewPos)));\n"
    "    vec3 viewDir = normalize(-vViewPos);\n"
    "    if (dot(n, viewDir) < 0.0) n = -n;\n"      /* two-sided */
    "    float ndl = max(dot(n, uLightDir), 0.0);\n"
    "    float shade = uAmbient + (1.0 - uAmbient) * ndl;\n"
    "    if (uMode == 2) { fragColor = vec4(vec3(shade), 1.0); return; }\n"
    "    vec3 base;\n"
    "    if (uMode == 0 && uUseTex == 1) base = texture(uTex, vUV).rgb;\n"
    "    else base = uSurface;\n"
    "    fragColor = vec4(base * shade, 1.0);\n"
    "}\n";

/* ----------------------------------------------------- PURIST shaders (RENDER-MATH)
 * A faithful GLSL port of the VU1 program A/B/D per-vertex math, adapted from
 * port/vu/shaders/dirge_purist.{vert,frag}. Differences from that sketch:
 *   - vertex attributes arrive already dequantised (the ITOF4/ITOF12/ITOF0
 *     divides are done CPU-side by the mesh builder), so no uPosScale/uUVScale.
 *   - the two 3x3 light matrices (L, C) are passed as six explicit vec3 uniforms
 *     instead of mat3 -- algebraically identical (d_i = Ldir_i . n ; lit =
 *     sum_i Lcol_i*d_i), and it keeps the GL entry-point loader unchanged.
 * Lighting is per-vertex (Gouraud), exactly as the PS2 did it. */
static const char *SHADE_VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec3 aNormal;\n"
    "layout(location=2) in vec2 aUV;\n"
    "layout(location=3) in vec4 aColor;\n"      /* 0..1 */
    "uniform mat4  uMVP;\n"
    "uniform int   uLit;\n"                      /* 1 = program D, 0 = A/B */
    "uniform vec3  uLdir0, uLdir1, uLdir2;\n"    /* L : 3 light directions */
    "uniform vec3  uLcol0, uLcol1, uLcol2;\n"    /* C : 3 light RGB colours */
    "uniform vec3  uAmbient;\n"                  /* A */
    "uniform float uSaturate;\n"                 /* MINIi I */
    "uniform int   uFogEnable;\n"
    "uniform float uFogA, uFogB, uFogMin;\n"
    "out vec2  vUV;\n"
    "out vec4  vColor;\n"
    "out float vFog;\n"
    "void main(){\n"
    "    vec4 clip = uMVP * vec4(aPos, 1.0);\n"
    "    vUV = aUV;\n"                           /* perspective-correct by the rasteriser */
    "    vec3 col = aColor.rgb;\n"
    "    if (uLit == 1){\n"
    "        vec3 n = normalize(aNormal);\n"
    "        float d0 = max(dot(uLdir0, n), 0.0);\n"   /* [VU1-D idx 88-94] */
    "        float d1 = max(dot(uLdir1, n), 0.0);\n"
    "        float d2 = max(dot(uLdir2, n), 0.0);\n"
    "        vec3 lit = uLcol0*d0 + uLcol1*d1 + uLcol2*d2 + uAmbient;\n" /* [idx 98-102] */
    "        col = min(lit * col, vec3(uSaturate));\n"  /* saturate [idx 111-115] */
    "    }\n"
    "    vColor = vec4(col, aColor.a);\n"        /* alpha rides through lighting */
    "    vFog = (uFogEnable == 1) ? clamp(uFogA*clip.w + uFogB, uFogMin, 1.0) : 1.0;\n"
    "    gl_Position = clip;\n"
    "}\n";

static const char *SHADE_FS =
    "#version 330 core\n"
    "in vec2  vUV;\n"
    "in vec4  vColor;\n"
    "in float vFog;\n"
    "uniform sampler2D uTex;\n"
    "uniform int   uTexEnable;\n"
    "uniform int   uTFX;\n"           /* 0 MODULATE 1 DECAL 2 HIGHLIGHT 3 HIGHLIGHT2 */
    "uniform vec3  uFogColor;\n"
    "uniform int   uFogEnable;\n"
    "uniform float uAlphaRef;\n"      /* <0 disables */
    "out vec4 fragColor;\n"
    "void main(){\n"
    "    vec4 c = vColor;\n"
    "    if (uTexEnable == 1){\n"
    "        vec4 t = texture(uTex, vUV);\n"
    /* PS2 MODULATE: (Ct*Cv)>>7, i.e. the 8-bit 128==1.0 doubling. */
    "        if (uTFX == 0){\n"
    "            c.rgb = min(t.rgb * vColor.rgb * 2.0, 1.0);\n"
    "            c.a   = min(t.a   * vColor.a   * 2.0, 1.0);\n"
    "        } else if (uTFX == 1){\n"            /* DECAL */
    "            c.rgb = t.rgb; c.a = t.a;\n"
    "        } else {\n"                          /* HIGHLIGHT(2) */
    "            c.rgb = min(t.rgb * vColor.rgb * 2.0 + vColor.a, 1.0);\n"
    "            c.a   = (uTFX == 2) ? t.a : vColor.a;\n"
    "        }\n"
    "    }\n"
    "    if (uAlphaRef >= 0.0 && c.a < uAlphaRef) discard;\n"
    "    if (uFogEnable == 1) c.rgb = mix(uFogColor, c.rgb, vFog);\n"
    "    fragColor = vec4(c.rgb, 1.0);\n"
    "}\n";

/* 3-light rig for VU1 program D (RENDER-MATH §6).
 *
 * PROVENANCE / STATUS (oracle investigation 2026-10-14, see
 * port/oracle/out/light_rig.json):  These remain DOCUMENTED PLACEHOLDERS, not
 * authentic captured values.  The lighting MODEL and the full EE->VU1 compose/
 * upload path ARE recovered and oracle-verified, but the per-light CONSTANTS
 * could NOT be recovered, and were deliberately NOT fabricated:
 *
 *   - The engine has NO static directional-light default.  Verified by running
 *     the real recompiled env-init via the oracle
 *     (oracle-cli --a0 0 --call 0x002daeb8 --dump 0x4e9dd0:0x120): the env/
 *     camera struct comes up as projection/identity only (screen 512x448, near/
 *     far 2048), with no baked L/C/A.  The ELF has zero "light"/"ambient"
 *     strings and no 3-dir+colour rig in .data/.rodata.
 *   - The real rig is runtime .bss, set PER SCENE by KEL-VM script bytecode
 *     (fake/KerberosLight.set_directionvector/_directioncolor/_ambientcolor),
 *     which bind to native code by a baked index (no names) and push literal
 *     args onto the VM stack -- not reachable by a direct oracle_call, and not
 *     present as any shipped light file in KEL.DAT.
 *   - The composed block is uploaded to VU1 micromem via VIF UNPACK V4-32
 *     NUM=11qw (vifcode 0x6c0b00f0) in FUN_0027e690/FUN_0027da78, built by the
 *     compose funcs FUN_002e3498 / FUN_002e3948 (adds ambient) from EE
 *     scratchpad 0x70003c00; render context 0x4eb500.  11 qw = Mvp(4)+L(3)+
 *     C(3)+A(1), matching the 0x00/0x40/0x70/0xa0 micromem layout.
 *
 * To replace these with authentic numbers, read out a real scene's rig from the
 * composed VU1 block (scratchpad 0x70003c00, just before an actor MSCAL) after
 * running one field/boss script under the full VM, or disassemble the KEL
 * bytecode of a per-zone program and read the set_direction* literal args.
 *
 * Until then: a key/fill/rim placeholder so a test mesh reads with an obvious
 * gradient:  0 key = warm upper-right-front, 1 fill = cool left, 2 rim = dim
 * behind.  Directions are auto-normalised; colours/ambient are plausible 0..1. */
static const rgl_lightrig DEFAULT_RIG = {
    .dir = {
        {  0.50f,  0.70f,  0.55f },   /* key  (toward light) */
        { -0.75f,  0.15f,  0.30f },   /* fill */
        {  0.10f, -0.45f, -0.85f },   /* rim/back */
    },
    .col = {
        { 1.00f, 0.95f, 0.85f },      /* key : warm white */
        { 0.28f, 0.33f, 0.45f },      /* fill: cool */
        { 0.22f, 0.20f, 0.26f },      /* rim : dim violet */
    },
    .ambient  = { 0.12f, 0.12f, 0.15f },
    .saturate = 1.0f,
};

const rgl_lightrig *rgl_default_lightrig(void){ return &DEFAULT_RIG; }

static GLuint compile(GLenum type, const char *src, char *err, int errlen){
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok){
        char log[1024]; glGetShaderInfoLog(s, sizeof(log), NULL, log);
        if (err) snprintf(err, (size_t)errlen, "shader compile: %s", log);
        glDeleteShader(s); return 0;
    }
    return s;
}

static GLuint build_program_src(const char *vs_src, const char *fs_src,
                                char *err, int errlen){
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src, err, errlen);
    if (!vs) return 0;
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src, err, errlen);
    if (!fs){ glDeleteShader(vs); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok){
        char log[1024]; glGetProgramInfoLog(p, sizeof(log), NULL, log);
        if (err) snprintf(err, (size_t)errlen, "program link: %s", log);
        glDeleteProgram(p); return 0;
    }
    return p;
}

static GLuint build_program(char *err, int errlen){
    return build_program_src(VS_SRC, FS_SRC, err, errlen);
}

/* vertex attribute: 3 pos + 2 uv */
typedef struct { float px, py, pz, u, v; } GVert;

/* Expand strips -> unindexed triangle list with the strip winding flip. */
static GVert *build_vertices(const DocModel *m, size_t *out_count){
    size_t cap = 0;
    for (int o = 0; o < m->nobjs; o++)
        for (int s = 0; s < m->objs[o].nstrips; s++){
            int nv = m->objs[o].strips[s].nverts;
            if (nv >= 3) cap += (size_t)(nv - 2) * 3;
        }
    GVert *out = (GVert *)malloc((cap ? cap : 1) * sizeof(GVert));
    if (!out){ *out_count = 0; return NULL; }
    size_t n = 0;
    for (int o = 0; o < m->nobjs; o++){
        const DocObject *ob = &m->objs[o];
        for (int s = 0; s < ob->nstrips; s++){
            const DocStrip *st = &ob->strips[s];
            for (int i = 0; i + 2 < st->nverts; i++){
                const DocVertex *p0 = &st->verts[i];
                const DocVertex *p1 = &st->verts[i+1];
                const DocVertex *p2 = &st->verts[i+2];
                if (i & 1){ const DocVertex *t = p1; p1 = p2; p2 = t; }
                const DocVertex *tri[3] = { p0, p1, p2 };
                for (int k = 0; k < 3; k++){
                    out[n].px = tri[k]->x; out[n].py = tri[k]->y; out[n].pz = tri[k]->z;
                    out[n].u  = tri[k]->u; out[n].v  = tri[k]->v;
                    n++;
                }
            }
        }
    }
    *out_count = n;
    return out;
}

int rgl_render(const DocModel *m, const rgl_params *p,
               unsigned char **out_rgba, char *err, int errlen){
    const int W = p->width, H = p->height;
    *out_rgba = NULL;

    /* ---- geometry ---- */
    size_t vcount = 0;
    GVert *verts = build_vertices(m, &vcount);
    if (!verts){ if (err) snprintf(err,(size_t)errlen,"vertex alloc failed"); return 1; }

    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vcount * sizeof(GVert)), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GVert), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GVert), (void*)(3*sizeof(float)));
    free(verts);

    /* ---- program ---- */
    GLuint prog = build_program(err, errlen);
    if (!prog){ glDeleteBuffers(1,&vbo); glDeleteVertexArrays(1,&vao); return 1; }

    /* ---- texture ---- */
    GLuint tex = 0;
    int use_tex = (p->tex_rgba && p->tex_w > 0 && p->tex_h > 0);
    if (use_tex){
        glGenTextures(1, &tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, p->tex_w, p->tex_h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, p->tex_rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        if (p->bilinear){
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
    }

    /* ---- FBO (RGBA8 colour + depth24) ---- */
    GLuint fbo = 0, color = 0, depth = 0;
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &color); glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glGenRenderbuffers(1, &depth); glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE){
        if (err) snprintf(err,(size_t)errlen,"framebuffer incomplete");
        goto fail;
    }

    /* ---- camera: identical framing to the software reference ---- */
    v3 center = { (m->bbmin[0]+m->bbmax[0])*0.5f,
                  (m->bbmin[1]+m->bbmax[1])*0.5f,
                  (m->bbmin[2]+m->bbmax[2])*0.5f };
    v3 ext = { (m->bbmax[0]-m->bbmin[0])*0.5f,
               (m->bbmax[1]-m->bbmin[1])*0.5f,
               (m->bbmax[2]-m->bbmin[2])*0.5f };
    float radius = sqrtf(ext.x*ext.x + ext.y*ext.y + ext.z*ext.z);
    if (radius < 1e-4f) radius = 1.0f;
    float fovy = DEG2RAD(45.0f);
    float aspect = (float)W / (float)H;
    float fit = fminf(fovy, 2.0f*atanf(tanf(fovy*0.5f)*aspect));
    float dist = radius / sinf(fit * 0.5f) * 1.25f;
    float az = DEG2RAD(p->angle_deg), el = DEG2RAD(p->elev_deg);
    v3 eye = { center.x + dist*cosf(el)*sinf(az),
               center.y + dist*sinf(el),
               center.z + dist*cosf(el)*cosf(az) };
    v3 up = {0,1,0};
    mat4 view = mat_lookat(eye, center, up);
    mat4 proj = mat_persp(fovy, aspect, dist*0.01f + 0.01f, dist + radius*2.0f + 1.0f);
    mat4 mvp  = mat_mul(proj, view);
    v3 light  = v3norm((v3){0.35f, 0.55f, 0.75f});

    /* ---- draw ---- */
    glViewport(0, 0, W, H);
    glClearColor(24/255.0f, 26/255.0f, 32/255.0f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    if (p->backface_cull){ glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW); }
    else glDisable(GL_CULL_FACE);
    if (p->mode == RGL_WIRE){ glPolygonMode(GL_FRONT_AND_BACK, GL_LINE); glLineWidth(1.0f); }
    else glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog,"uMVP"), 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(glGetUniformLocation(prog,"uMV"),  1, GL_FALSE, view.m);
    glUniform3fv(glGetUniformLocation(prog,"uLightDir"), 1, &light.x);
    glUniform1f (glGetUniformLocation(prog,"uAmbient"), 0.22f);
    glUniform1i (glGetUniformLocation(prog,"uMode"), p->mode);
    glUniform1i (glGetUniformLocation(prog,"uUseTex"), use_tex ? 1 : 0);
    { float surf[3] = {0.82f, 0.80f, 0.78f};
      glUniform3fv(glGetUniformLocation(prog,"uSurface"), 1, surf); }
    glUniform1i (glGetUniformLocation(prog,"uTex"), 0);
    if (use_tex){ glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex); }

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)vcount);
    glFinish();

    /* ---- readback (flip to top-left origin) ---- */
    {
        unsigned char *raw = (unsigned char *)malloc((size_t)W*H*4);
        unsigned char *flip = (unsigned char *)malloc((size_t)W*H*4);
        if (!raw || !flip){
            free(raw); free(flip);
            if (err) snprintf(err,(size_t)errlen,"readback alloc failed");
            goto fail;
        }
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, raw);
        for (int y = 0; y < H; y++)
            memcpy(flip + (size_t)y*W*4, raw + (size_t)(H-1-y)*W*4, (size_t)W*4);
        free(raw);
        *out_rgba = flip;
    }

    glDeleteFramebuffers(1,&fbo); glDeleteRenderbuffers(1,&depth);
    glDeleteTextures(1,&color); if (tex) glDeleteTextures(1,&tex);
    glDeleteProgram(prog); glDeleteBuffers(1,&vbo); glDeleteVertexArrays(1,&vao);
    return 0;

fail:
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo)   glDeleteFramebuffers(1,&fbo);
    if (depth) glDeleteRenderbuffers(1,&depth);
    if (color) glDeleteTextures(1,&color);
    if (tex)   glDeleteTextures(1,&tex);
    if (prog)  glDeleteProgram(prog);
    if (vbo)   glDeleteBuffers(1,&vbo);
    if (vao)   glDeleteVertexArrays(1,&vao);
    return 1;
}

/* ------------------------------------------------- PURIST mesh render path --- */
/* Auto-frame an orbit camera from [bbmin,bbmax] -- same math/constants as the
 * DocModel path above -- and build the MVP. */
/* `dist_scale` pulls the eye back past a tight fit (1.0 = tight); `far_pad`
 * extra world units added to the far plane so geometry behind the focus box
 * (a surrounding room) isn't clipped. */
static mat4 frame_camera_ex(const float bbmin[3], const float bbmax[3],
                            float angle_deg, float elev_deg, int W, int H,
                            float dist_scale, float far_pad){
    v3 center = { (bbmin[0]+bbmax[0])*0.5f,
                  (bbmin[1]+bbmax[1])*0.5f,
                  (bbmin[2]+bbmax[2])*0.5f };
    v3 ext = { (bbmax[0]-bbmin[0])*0.5f,
               (bbmax[1]-bbmin[1])*0.5f,
               (bbmax[2]-bbmin[2])*0.5f };
    float radius = sqrtf(ext.x*ext.x + ext.y*ext.y + ext.z*ext.z);
    if (radius < 1e-4f) radius = 1.0f;
    if (dist_scale <= 0.0f) dist_scale = 1.0f;
    float fovy = DEG2RAD(45.0f);
    float aspect = (float)W / (float)H;
    float fit = fminf(fovy, 2.0f*atanf(tanf(fovy*0.5f)*aspect));
    float dist = radius / sinf(fit * 0.5f) * 1.25f * dist_scale;
    float az = DEG2RAD(angle_deg), el = DEG2RAD(elev_deg);
    v3 eye = { center.x + dist*cosf(el)*sinf(az),
               center.y + dist*sinf(el),
               center.z + dist*cosf(el)*cosf(az) };
    v3 up = {0,1,0};
    mat4 view = mat_lookat(eye, center, up);
    mat4 proj = mat_persp(fovy, aspect, dist*0.01f + 0.01f,
                          dist + radius*2.0f + far_pad + 1.0f);
    return mat_mul(proj, view);
}

static mat4 frame_camera(const float bbmin[3], const float bbmax[3],
                         float angle_deg, float elev_deg, int W, int H){
    return frame_camera_ex(bbmin, bbmax, angle_deg, elev_deg, W, H, 1.0f, 0.0f);
}

int rgl_render_mesh(const RVertex *verts, size_t vcount,
                    const float bbmin[3], const float bbmax[3],
                    const rgl_params *p,
                    unsigned char **out_rgba, char *err, int errlen){
    const int W = p->width, H = p->height;
    *out_rgba = NULL;
    if (!verts || vcount < 3){
        if (err) snprintf(err,(size_t)errlen,"empty mesh");
        return 1;
    }
    const rgl_lightrig *rig = p->rig ? p->rig : &DEFAULT_RIG;

    /* ---- geometry (interleaved RVertex: pos3, nrm3, uv2, rgba4) ---- */
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vcount * sizeof(RVertex)), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)(6*sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)(8*sizeof(float)));

    /* ---- program (purist RENDER-MATH shader pair) ---- */
    GLuint prog = build_program_src(SHADE_VS, SHADE_FS, err, errlen);
    if (!prog){ glDeleteBuffers(1,&vbo); glDeleteVertexArrays(1,&vao); return 1; }

    /* ---- texture ---- */
    GLuint tex = 0;
    int use_tex = (p->tex_rgba && p->tex_w > 0 && p->tex_h > 0);
    if (use_tex){
        glGenTextures(1, &tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, p->tex_w, p->tex_h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, p->tex_rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        if (p->bilinear){
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
    }

    /* ---- FBO (RGBA8 colour + depth24) ---- */
    GLuint fbo = 0, color = 0, depth = 0;
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &color); glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glGenRenderbuffers(1, &depth); glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE){
        if (err) snprintf(err,(size_t)errlen,"framebuffer incomplete");
        goto fail;
    }

    mat4 mvp = frame_camera(bbmin, bbmax, p->angle_deg, p->elev_deg, W, H);

    /* ---- draw ---- */
    glViewport(0, 0, W, H);
    glClearColor(24/255.0f, 26/255.0f, 32/255.0f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    if (p->backface_cull){ glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW); }
    else glDisable(GL_CULL_FACE);
    if (p->wire){ glPolygonMode(GL_FRONT_AND_BACK, GL_LINE); glLineWidth(1.0f); }
    else glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog,"uMVP"), 1, GL_FALSE, mvp.m);
    glUniform1i(glGetUniformLocation(prog,"uLit"), p->shade_mode == RGL_SHADE_LIT ? 1 : 0);

    /* light rig (directions normalised here, as the PS2 L rows are unit dirs) */
    { const char *nm[3] = { "uLdir0", "uLdir1", "uLdir2" };
      for (int i = 0; i < 3; i++){
          v3 d = v3norm((v3){rig->dir[i][0], rig->dir[i][1], rig->dir[i][2]});
          float a[3] = { d.x, d.y, d.z };
          glUniform3fv(glGetUniformLocation(prog, nm[i]), 1, a);
      }
    }
    glUniform3fv(glGetUniformLocation(prog,"uLcol0"), 1, rig->col[0]);
    glUniform3fv(glGetUniformLocation(prog,"uLcol1"), 1, rig->col[1]);
    glUniform3fv(glGetUniformLocation(prog,"uLcol2"), 1, rig->col[2]);
    glUniform3fv(glGetUniformLocation(prog,"uAmbient"), 1, rig->ambient);
    glUniform1f(glGetUniformLocation(prog,"uSaturate"), rig->saturate > 0 ? rig->saturate : 1.0f);

    glUniform1i(glGetUniformLocation(prog,"uTexEnable"), use_tex ? 1 : 0);
    glUniform1i(glGetUniformLocation(prog,"uTFX"), p->tfx);
    glUniform1i(glGetUniformLocation(prog,"uTex"), 0);
    glUniform1f(glGetUniformLocation(prog,"uAlphaRef"), p->alpha_ref);

    glUniform1i(glGetUniformLocation(prog,"uFogEnable"), p->fog_enable ? 1 : 0);
    glUniform1f(glGetUniformLocation(prog,"uFogA"), p->fog_a);
    glUniform1f(glGetUniformLocation(prog,"uFogB"), p->fog_b);
    glUniform1f(glGetUniformLocation(prog,"uFogMin"), p->fog_min);
    glUniform3fv(glGetUniformLocation(prog,"uFogColor"), 1, p->fog_color);

    if (use_tex){ glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex); }

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)vcount);
    glFinish();

    /* ---- readback (flip to top-left origin) ---- */
    {
        unsigned char *raw = (unsigned char *)malloc((size_t)W*H*4);
        unsigned char *flip = (unsigned char *)malloc((size_t)W*H*4);
        if (!raw || !flip){
            free(raw); free(flip);
            if (err) snprintf(err,(size_t)errlen,"readback alloc failed");
            goto fail;
        }
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, raw);
        for (int y = 0; y < H; y++)
            memcpy(flip + (size_t)y*W*4, raw + (size_t)(H-1-y)*W*4, (size_t)W*4);
        free(raw);
        *out_rgba = flip;
    }

    glDeleteFramebuffers(1,&fbo); glDeleteRenderbuffers(1,&depth);
    glDeleteTextures(1,&color); if (tex) glDeleteTextures(1,&tex);
    glDeleteProgram(prog); glDeleteBuffers(1,&vbo); glDeleteVertexArrays(1,&vao);
    return 0;

fail:
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo)   glDeleteFramebuffers(1,&fbo);
    if (depth) glDeleteRenderbuffers(1,&depth);
    if (color) glDeleteTextures(1,&color);
    if (tex)   glDeleteTextures(1,&tex);
    if (prog)  glDeleteProgram(prog);
    if (vbo)   glDeleteBuffers(1,&vbo);
    if (vao)   glDeleteVertexArrays(1,&vao);
    return 1;
}

/* --------------------------------------------------- PURIST scene render ----
 * Draw several meshes into one GL world (shared camera + depth buffer). Each
 * draw sets its own shade_mode / texture / tfx; the light rig, fog and camera
 * are common. The purist SHADE_VS/FS program is built once and reused. */
int rgl_render_scene(const rgl_scene_draw *draws, int ndraws,
                     const float focus_min[3], const float focus_max[3],
                     float dist_scale, const rgl_params *p,
                     unsigned char **out_rgba, char *err, int errlen){
    const int W = p->width, H = p->height;
    *out_rgba = NULL;
    if (!draws || ndraws < 1){
        if (err) snprintf(err,(size_t)errlen,"no scene draws");
        return 1;
    }
    const rgl_lightrig *rig = p->rig ? p->rig : &DEFAULT_RIG;

    /* far-plane padding: the diagonal of the focus box scaled with the pull-back,
     * so a room surrounding a character-sized focus box stays in the frustum. */
    float fe[3] = { focus_max[0]-focus_min[0],
                    focus_max[1]-focus_min[1],
                    focus_max[2]-focus_min[2] };
    float far_pad = sqrtf(fe[0]*fe[0]+fe[1]*fe[1]+fe[2]*fe[2]) * (dist_scale + 2.0f);

    GLuint prog = build_program_src(SHADE_VS, SHADE_FS, err, errlen);
    if (!prog) return 1;

    /* ---- FBO (RGBA8 colour + depth24), built once ---- */
    GLuint fbo = 0, color = 0, depth = 0;
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &color); glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glGenRenderbuffers(1, &depth); glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE){
        if (err) snprintf(err,(size_t)errlen,"framebuffer incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1,&fbo); glDeleteRenderbuffers(1,&depth);
        glDeleteTextures(1,&color); glDeleteProgram(prog);
        return 1;
    }

    mat4 mvp = frame_camera_ex(focus_min, focus_max, p->angle_deg, p->elev_deg,
                               W, H, dist_scale, far_pad);

    /* ---- common GL state + clear (ONCE: one shared depth buffer) ---- */
    glViewport(0, 0, W, H);
    glClearColor(24/255.0f, 26/255.0f, 32/255.0f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    if (p->backface_cull){ glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW); }
    else glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog,"uMVP"), 1, GL_FALSE, mvp.m);
    /* light rig (common) */
    { const char *nm[3] = { "uLdir0", "uLdir1", "uLdir2" };
      for (int i = 0; i < 3; i++){
          v3 d = v3norm((v3){rig->dir[i][0], rig->dir[i][1], rig->dir[i][2]});
          float a[3] = { d.x, d.y, d.z };
          glUniform3fv(glGetUniformLocation(prog, nm[i]), 1, a);
      } }
    glUniform3fv(glGetUniformLocation(prog,"uLcol0"), 1, rig->col[0]);
    glUniform3fv(glGetUniformLocation(prog,"uLcol1"), 1, rig->col[1]);
    glUniform3fv(glGetUniformLocation(prog,"uLcol2"), 1, rig->col[2]);
    glUniform3fv(glGetUniformLocation(prog,"uAmbient"), 1, rig->ambient);
    glUniform1f(glGetUniformLocation(prog,"uSaturate"), rig->saturate > 0 ? rig->saturate : 1.0f);
    glUniform1f(glGetUniformLocation(prog,"uAlphaRef"), p->alpha_ref);
    glUniform1i(glGetUniformLocation(prog,"uFogEnable"), p->fog_enable ? 1 : 0);
    glUniform1f(glGetUniformLocation(prog,"uFogA"), p->fog_a);
    glUniform1f(glGetUniformLocation(prog,"uFogB"), p->fog_b);
    glUniform1f(glGetUniformLocation(prog,"uFogMin"), p->fog_min);
    glUniform3fv(glGetUniformLocation(prog,"uFogColor"), 1, p->fog_color);
    glUniform1i(glGetUniformLocation(prog,"uTex"), 0);

    GLint locLit = glGetUniformLocation(prog,"uLit");
    GLint locTE  = glGetUniformLocation(prog,"uTexEnable");
    GLint locTFX = glGetUniformLocation(prog,"uTFX");

    int rc_fail = 0;
    for (int di = 0; di < ndraws; di++){
        const rgl_scene_draw *d = &draws[di];
        if (!d->verts || d->vcount < 3) continue;

        GLuint vao = 0, vbo = 0, tex = 0;
        glGenVertexArrays(1, &vao); glBindVertexArray(vao);
        glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(d->vcount * sizeof(RVertex)),
                     d->verts, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)(6*sizeof(float)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)(8*sizeof(float)));

        int use_tex = (d->tex_rgba && d->tex_w > 0 && d->tex_h > 0);
        if (use_tex){
            glGenTextures(1, &tex);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, d->tex_w, d->tex_h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, d->tex_rgba);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            if (p->bilinear){
                glGenerateMipmap(GL_TEXTURE_2D);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            } else {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            }
        }

        glUniform1i(locLit, d->shade_mode == RGL_SHADE_LIT ? 1 : 0);
        glUniform1i(locTE,  use_tex ? 1 : 0);
        glUniform1i(locTFX, d->tfx);
        if (use_tex){ glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex); }

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)d->vcount);

        if (tex) glDeleteTextures(1,&tex);
        glDeleteBuffers(1,&vbo); glDeleteVertexArrays(1,&vao);
    }
    glFinish();

    /* ---- readback (flip to top-left origin) ---- */
    {
        unsigned char *raw  = (unsigned char *)malloc((size_t)W*H*4);
        unsigned char *flip = (unsigned char *)malloc((size_t)W*H*4);
        if (!raw || !flip){
            free(raw); free(flip);
            if (err) snprintf(err,(size_t)errlen,"readback alloc failed");
            rc_fail = 1;
        } else {
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, raw);
            for (int y = 0; y < H; y++)
                memcpy(flip + (size_t)y*W*4, raw + (size_t)(H-1-y)*W*4, (size_t)W*4);
            free(raw);
            *out_rgba = flip;
        }
    }

    glDeleteFramebuffers(1,&fbo); glDeleteRenderbuffers(1,&depth);
    glDeleteTextures(1,&color); glDeleteProgram(prog);
    return rc_fail ? 1 : 0;
}
