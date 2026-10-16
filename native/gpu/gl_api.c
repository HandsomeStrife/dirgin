/* gl_api.c -- storage + loader for the GL 3.3-core entry points. */
#include "gl_api.h"
#include <stdio.h>

#define GL_DEF(type, name) type name = NULL;
GL_API_FUNCS(GL_DEF)
#undef GL_DEF

int gl_api_load(gl_getproc_fn getproc)
{
    int missing = 0;
#define GL_LOAD(type, name)                                        \
    name = (type)getproc(#name);                                   \
    if (!name) { fprintf(stderr, "gl_api: missing %s\n", #name); missing++; }
    GL_API_FUNCS(GL_LOAD)
#undef GL_LOAD
    return missing;
}
