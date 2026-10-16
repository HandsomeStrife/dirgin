/* main.c -- CLI for the Dirge of Cerberus native model loader.
 *
 *   doc_model <container.bin> [out_prefix]
 *
 * Loads the first BMD/MOD payload inside an RFD container, prints geometry
 * stats, and writes <out_prefix>.obj and <out_prefix>.gltf (+ .bin).
 */
#include "doc_model.h"
#include <stdio.h>
#include <string.h>

static const char *errstr(int e)
{
    switch (e) {
        case DOC_OK:         return "ok";
        case DOC_ERR_IO:     return "io error";
        case DOC_ERR_FORMAT: return "format error / no bmd|mod payload";
        case DOC_ERR_MEM:    return "out of memory";
        default:             return "unknown";
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <container.bin> [out_prefix]\n", argv[0]);
        return 2;
    }
    int err = 0;
    DocModel *m = doc_model_load(argv[1], &err);
    if (!m) {
        fprintf(stderr, "load failed: %s\n", errstr(err));
        return 1;
    }

    printf("== %s  (version %d)\n", argv[1], m->version);
    printf("   kind        : %s\n", m->kind);
    printf("   objects     : %d\n", m->nobjs);
    printf("   strips      : %d\n", m->total_strips);
    printf("   vertices    : %d\n", m->total_verts);
    printf("   triangles   : %d\n", m->total_tris);
    printf("   bbox min    : %g %g %g\n", m->bbmin[0], m->bbmin[1], m->bbmin[2]);
    printf("   bbox max    : %g %g %g\n", m->bbmax[0], m->bbmax[1], m->bbmax[2]);
    printf("   uv range    : u[%g..%g] v[%g..%g]\n", m->umin, m->umax, m->vmin, m->vmax);

    const char *prefix = (argc >= 3) ? argv[2] : NULL;
    if (prefix) {
        char path[1024];
        snprintf(path, sizeof path, "%s.obj", prefix);
        err = doc_export_obj(m, path);
        printf("   wrote OBJ   : %s (%s)\n", path, errstr(err));
        snprintf(path, sizeof path, "%s.gltf", prefix);
        err = doc_export_gltf(m, path);
        printf("   wrote glTF  : %s (%s)\n", path, errstr(err));
    }

    doc_model_free(m);
    return 0;
}
