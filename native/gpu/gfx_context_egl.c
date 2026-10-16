/* gfx_context_egl.c -- EGL offscreen GL 3.3-core context (headless Linux).
 *
 * Strategy, most portable first:
 *   1. EGL_MESA_platform_surfaceless display  -> no window system needed.
 *   2. fall back to the default display.
 *   3. bind the desktop OpenGL API, request a 3.3 CORE context.
 *   4. try a tiny pbuffer surface; if the config has none, use a surfaceless
 *      make-current (EGL_KHR_surfaceless_context / GL 3.0+). The renderer draws
 *      into its own FBO either way, so the surface only needs to exist (or not).
 *
 * This file is the Linux half of the platform seam declared in gfx_context.h.
 */
#include "gfx_context.h"
#include "gl_api.h"

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gfx_context {
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surf;      /* may be EGL_NO_SURFACE (surfaceless) */
    char       desc[256];
};

static void seterr(char *err, int n, const char *fmt, EGLint code)
{
    if (err && n > 0) snprintf(err, (size_t)n, fmt, (unsigned)code);
}

gfx_context *gfx_context_create(int want_w, int want_h, char *err, int errlen)
{
    if (want_w < 1) want_w = 1;
    if (want_h < 1) want_h = 1;

    gfx_context *c = (gfx_context *)calloc(1, sizeof(*c));
    if (!c) { if (err) snprintf(err, (size_t)errlen, "out of memory"); return NULL; }
    c->dpy  = EGL_NO_DISPLAY;
    c->ctx  = EGL_NO_CONTEXT;
    c->surf = EGL_NO_SURFACE;

    /* 1. surfaceless platform display if available */
    const char *client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (client_exts && strstr(client_exts, "EGL_MESA_platform_surfaceless")) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC getpd =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (getpd)
            c->dpy = getpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    }
    if (c->dpy == EGL_NO_DISPLAY)
        c->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (c->dpy == EGL_NO_DISPLAY) {
        if (err) snprintf(err, (size_t)errlen, "no EGL display");
        free(c); return NULL;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(c->dpy, &major, &minor)) {
        seterr(err, errlen, "eglInitialize failed (0x%x)", eglGetError());
        free(c); return NULL;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        seterr(err, errlen, "eglBindAPI(OpenGL) failed (0x%x)", eglGetError());
        eglTerminate(c->dpy); free(c); return NULL;
    }

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    EGLConfig cfg; EGLint ncfg = 0;
    if (!eglChooseConfig(c->dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
        seterr(err, errlen, "eglChooseConfig found no config (0x%x)", eglGetError());
        eglTerminate(c->dpy); free(c); return NULL;
    }

    /* Request a 3.3 CORE context (the target baseline; the driver may hand back
     * a higher compatible core version, which is fine). */
    const EGLint ctx_attr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    c->ctx = eglCreateContext(c->dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (c->ctx == EGL_NO_CONTEXT) {
        seterr(err, errlen, "eglCreateContext 3.3 core failed (0x%x)", eglGetError());
        eglTerminate(c->dpy); free(c); return NULL;
    }

    const EGLint pb_attr[] = { EGL_WIDTH, want_w, EGL_HEIGHT, want_h, EGL_NONE };
    c->surf = eglCreatePbufferSurface(c->dpy, cfg, pb_attr); /* may fail -> surfaceless */

    if (!eglMakeCurrent(c->dpy, c->surf, c->surf, c->ctx)) {
        /* retry surfaceless (needs GL 3.0+/EGL_KHR_surfaceless_context) */
        c->surf = EGL_NO_SURFACE;
        if (!eglMakeCurrent(c->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, c->ctx)) {
            seterr(err, errlen, "eglMakeCurrent failed (0x%x)", eglGetError());
            eglDestroyContext(c->dpy, c->ctx); eglTerminate(c->dpy); free(c);
            return NULL;
        }
    }

    if (gl_api_load((gl_getproc_fn)eglGetProcAddress) != 0) {
        if (err) snprintf(err, (size_t)errlen, "could not resolve all GL 3.3 entry points");
        gfx_context_destroy(c); return NULL;
    }

    const char *ver = (const char *)glGetString(GL_VERSION);
    const char *ren = (const char *)glGetString(GL_RENDERER);
    snprintf(c->desc, sizeof(c->desc),
             "EGL %d.%d %s / %s / %s / surface=%s",
             major, minor,
             eglQueryString(c->dpy, EGL_VENDOR),
             ren ? ren : "?", ver ? ver : "?",
             c->surf == EGL_NO_SURFACE ? "surfaceless" : "pbuffer");
    return c;
}

const char *gfx_context_describe(gfx_context *c)
{
    return c ? c->desc : "(null)";
}

void gfx_context_destroy(gfx_context *c)
{
    if (!c) return;
    if (c->dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(c->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (c->surf != EGL_NO_SURFACE) eglDestroySurface(c->dpy, c->surf);
        if (c->ctx  != EGL_NO_CONTEXT) eglDestroyContext(c->dpy, c->ctx);
        eglTerminate(c->dpy);
    }
    free(c);
}
