/* gfx_context.h -- platform GL context abstraction (the ONLY platform seam).
 *
 * The renderer core (renderer_gl.c) never touches EGL / WGL / a swapchain. It
 * is handed a current GL 3.3-core context by one of these backends and renders
 * into an FBO it owns. Swapping EGL (this headless Linux build) for a Win32 +
 * WGL swapchain on Windows means replacing ONLY this file's implementation --
 * the render code, shaders and GL loader are untouched. See README.md.
 *
 * Contract: gfx_context_create() returns a backend with a CURRENT GL context
 * and a resolved gl_api function table (via gl_api_load). The renderer then
 * draws to its own framebuffer object, so the backend does not need a visible
 * window or a correctly-sized default framebuffer -- a surfaceless/pbuffer
 * context is sufficient.
 */
#ifndef GFX_CONTEXT_H
#define GFX_CONTEXT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gfx_context gfx_context;

/* Create an offscreen GL 3.3-core context and make it current. `want_w/h` size
 * the backing pbuffer where relevant (the renderer uses its own FBO regardless).
 * Returns NULL on failure, writing a reason to `err` (size `errlen`). */
gfx_context *gfx_context_create(int want_w, int want_h, char *err, int errlen);

/* Human-readable backend + GL driver description (e.g. "EGL surfaceless /
 * AMD Radeon / OpenGL 4.6 (Core Profile)"). Valid until destroy. */
const char *gfx_context_describe(gfx_context *c);

void gfx_context_destroy(gfx_context *c);

#ifdef __cplusplus
}
#endif
#endif /* GFX_CONTEXT_H */
