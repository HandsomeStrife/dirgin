/* gl_api.h -- minimal OpenGL 3.3-core function-pointer loader.
 *
 * We do NOT include the platform <GL/gl.h> (which only declares the ancient
 * 1.1 entry points and would clash). Instead we pull the types/enums/PFN
 * typedefs from <GL/glcorearb.h> and resolve EVERY entry point we use through
 * a getproc callback (eglGetProcAddress here, wglGetProcAddress+opengl32 on
 * Windows). This keeps the renderer core free of any windowing/driver symbol
 * dependency -- the same object links against EGL on Linux and a real
 * swapchain on Windows (see README.md "Windows portability").
 */
#ifndef GL_API_H
#define GL_API_H

/* glcorearb.h gates ALL its entry-point prototypes behind GL_GLEXT_PROTOTYPES.
 * We want the types/enums/PFN typedefs only -- every function is resolved as a
 * loaded pointer below -- so make sure that macro is NOT defined. (Defining it
 * even to 0 would still trip the #ifdef.) */
#undef GL_GLEXT_PROTOTYPES
#include <GL/glcorearb.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The full set of GL entry points the renderer core touches. Keeping it as one
 * X-macro list means the declaration, the storage and the load loop can never
 * drift out of sync. */
#define GL_API_FUNCS(X) \
    X(PFNGLGETSTRINGPROC,            glGetString) \
    X(PFNGLGETERRORPROC,             glGetError) \
    X(PFNGLENABLEPROC,               glEnable) \
    X(PFNGLDISABLEPROC,              glDisable) \
    X(PFNGLCLEARPROC,                glClear) \
    X(PFNGLCLEARCOLORPROC,           glClearColor) \
    X(PFNGLDEPTHFUNCPROC,            glDepthFunc) \
    X(PFNGLDEPTHMASKPROC,            glDepthMask) \
    X(PFNGLBLENDFUNCSEPARATEPROC,    glBlendFuncSeparate) \
    X(PFNGLCULLFACEPROC,             glCullFace) \
    X(PFNGLFRONTFACEPROC,            glFrontFace) \
    X(PFNGLVIEWPORTPROC,             glViewport) \
    X(PFNGLPOLYGONMODEPROC,          glPolygonMode) \
    X(PFNGLLINEWIDTHPROC,            glLineWidth) \
    X(PFNGLDRAWARRAYSPROC,           glDrawArrays) \
    X(PFNGLREADPIXELSPROC,           glReadPixels) \
    X(PFNGLPIXELSTOREIPROC,          glPixelStorei) \
    X(PFNGLFINISHPROC,               glFinish) \
    X(PFNGLGENBUFFERSPROC,           glGenBuffers) \
    X(PFNGLBINDBUFFERPROC,           glBindBuffer) \
    X(PFNGLBUFFERDATAPROC,           glBufferData) \
    X(PFNGLDELETEBUFFERSPROC,        glDeleteBuffers) \
    X(PFNGLGENVERTEXARRAYSPROC,      glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC,      glBindVertexArray) \
    X(PFNGLDELETEVERTEXARRAYSPROC,   glDeleteVertexArrays) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC,  glVertexAttribPointer) \
    X(PFNGLCREATESHADERPROC,         glCreateShader) \
    X(PFNGLSHADERSOURCEPROC,         glShaderSource) \
    X(PFNGLCOMPILESHADERPROC,        glCompileShader) \
    X(PFNGLGETSHADERIVPROC,          glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC,     glGetShaderInfoLog) \
    X(PFNGLDELETESHADERPROC,         glDeleteShader) \
    X(PFNGLCREATEPROGRAMPROC,        glCreateProgram) \
    X(PFNGLATTACHSHADERPROC,         glAttachShader) \
    X(PFNGLLINKPROGRAMPROC,          glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC,         glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC,    glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC,           glUseProgram) \
    X(PFNGLDELETEPROGRAMPROC,        glDeleteProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC,   glGetUniformLocation) \
    X(PFNGLUNIFORMMATRIX4FVPROC,     glUniformMatrix4fv) \
    X(PFNGLUNIFORM3FVPROC,           glUniform3fv) \
    X(PFNGLUNIFORM1IPROC,            glUniform1i) \
    X(PFNGLUNIFORM1FPROC,            glUniform1f) \
    X(PFNGLGENTEXTURESPROC,          glGenTextures) \
    X(PFNGLBINDTEXTUREPROC,          glBindTexture) \
    X(PFNGLTEXIMAGE2DPROC,           glTexImage2D) \
    X(PFNGLTEXPARAMETERIPROC,        glTexParameteri) \
    X(PFNGLDELETETEXTURESPROC,       glDeleteTextures) \
    X(PFNGLACTIVETEXTUREPROC,        glActiveTexture) \
    X(PFNGLGENERATEMIPMAPPROC,       glGenerateMipmap) \
    X(PFNGLGENFRAMEBUFFERSPROC,      glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC,      glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLDELETEFRAMEBUFFERSPROC,   glDeleteFramebuffers) \
    X(PFNGLGENRENDERBUFFERSPROC,     glGenRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC,     glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEPROC,  glRenderbufferStorage) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLDELETERENDERBUFFERSPROC,  glDeleteRenderbuffers) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)

/* Declare the pointers (defined in gl_api.c). */
#define GL_DECL(type, name) extern type name;
GL_API_FUNCS(GL_DECL)
#undef GL_DECL

/* getproc has the eglGetProcAddress / wglGetProcAddress shape. Returns the
 * number of entry points that failed to resolve (0 == all good). */
typedef void *(*gl_getproc_fn)(const char *name);
int gl_api_load(gl_getproc_fn getproc);

#ifdef __cplusplus
}
#endif
#endif /* GL_API_H */
