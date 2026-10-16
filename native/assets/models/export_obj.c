/* export_obj.c -- write a DocModel as Wavefront OBJ (universally viewable).
 * Triangle-strips are expanded to a triangle list with alternating winding. */
#include "doc_model.h"
#include <stdio.h>

int doc_export_obj(const DocModel *m, const char *out_path)
{
    FILE *f = fopen(out_path, "w");
    if (!f) return DOC_ERR_IO;
    fprintf(f, "# Dirge of Cerberus %s model (native loader export)\n", m->kind);
    fprintf(f, "# strips=%d verts=%d tris=%d\n",
            m->total_strips, m->total_verts, m->total_tris);

    int base = 1;  /* OBJ indices are 1-based and global */
    for (int o = 0; o < m->nobjs; o++) {
        const DocObject *ob = &m->objs[o];
        for (int s = 0; s < ob->nstrips; s++) {
            const DocStrip *st = &ob->strips[s];
            fprintf(f, "g %s_strip%d_mat%d\n", ob->name, s, st->material);
            for (int v = 0; v < st->nverts; v++)
                fprintf(f, "v %g %g %g\n", st->verts[v].x, st->verts[v].y, st->verts[v].z);
            for (int v = 0; v < st->nverts; v++)
                fprintf(f, "vt %g %g\n", st->verts[v].u, st->verts[v].v);
            for (int v = 0; v + 2 < st->nverts; v++) {
                int a = base + v, b = base + v + 1, c = base + v + 2;
                if (v & 1) { int t = b; b = c; c = t; }   /* flip odd triangles */
                fprintf(f, "f %d/%d %d/%d %d/%d\n", a,a, b,b, c,c);
            }
            base += st->nverts;
        }
    }
    fclose(f);
    return DOC_OK;
}
