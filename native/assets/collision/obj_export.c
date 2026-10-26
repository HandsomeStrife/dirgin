/* obj_export.c - write a cid_mesh to Wavefront OBJ. C99, no deps.
 *
 * Emits:
 *   o cid_collision     - the extracted collision triangles (groups split by
 *                         triangle source so the inline vs indexed geometry is
 *                         distinguishable in a viewer).
 *   o cid_tag2_aabb     - [optional] one wireframe-ish box per undecoded tag-2
 *                         (VU-packed) leaf, so the mesh has full spatial coverage.
 *
 * OBJ is Y-up, right-handed; the game's coordinates are written as-is (no axis
 * flip) so the collision overlays the map's visual BMD in the same space.
 */
#include "cid.h"
#include <stdio.h>

static void box_corners(const cid_box *b, cid_vec3 c[8]) {
    float x0 = b->center.x - b->half.x, x1 = b->center.x + b->half.x;
    float y0 = b->center.y - b->half.y, y1 = b->center.y + b->half.y;
    float z0 = b->center.z - b->half.z, z1 = b->center.z + b->half.z;
    c[0] = (cid_vec3){ x0, y0, z0 }; c[1] = (cid_vec3){ x1, y0, z0 };
    c[2] = (cid_vec3){ x1, y1, z0 }; c[3] = (cid_vec3){ x0, y1, z0 };
    c[4] = (cid_vec3){ x0, y0, z1 }; c[5] = (cid_vec3){ x1, y0, z1 };
    c[6] = (cid_vec3){ x1, y1, z1 }; c[7] = (cid_vec3){ x0, y1, z1 };
}

/* include_boxes: 0 = triangles only, 1 = also emit tag-2 AABB boxes */
int cid_write_obj(const cid_mesh *m, const char *path, int include_boxes) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    fprintf(f, "# Dirge of Cerberus CID collision mesh\n");
    fprintf(f, "# version=%d  verts=%d  tris=%d  tag2_boxes=%d (undecoded VU leaves)\n",
            m->version, m->nverts, m->ntris, m->nboxes);
    fprintf(f, "# bbox min %.3f %.3f %.3f  max %.3f %.3f %.3f\n",
            m->bb_min.x, m->bb_min.y, m->bb_min.z,
            m->bb_max.x, m->bb_max.y, m->bb_max.z);

    /* vertices for the triangle mesh */
    for (int i = 0; i < m->nverts; i++)
        fprintf(f, "v %.5f %.5f %.5f\n", m->verts[i].x, m->verts[i].y, m->verts[i].z);

    /* triangles, grouped by source (OBJ faces are 1-based) */
    fprintf(f, "o cid_collision\n");
    int cur = -1;
    for (int i = 0; i < m->ntris; i++) {
        const cid_tri *t = &m->tris[i];
        if ((int)t->source != cur) {
            cur = (int)t->source;
            const char *gn = cur == CID_SRC_INLINE_QUAD ? "inline_quad"
                           : cur == CID_SRC_INDEXED     ? "indexed"
                           : cur == CID_SRC_PRIM        ? "primitive"
                           :                               "vu_packed";
            fprintf(f, "g %s\n", gn);
        }
        fprintf(f, "f %u %u %u\n", t->v[0] + 1u, t->v[1] + 1u, t->v[2] + 1u);
    }

    if (include_boxes && m->nboxes > 0) {
        fprintf(f, "o cid_tag2_aabb\n");
        fprintf(f, "g tag2_boxes\n");
        /* 12 triangles per box; 8 new verts per box after the mesh verts */
        unsigned base = (unsigned)m->nverts + 1u; /* 1-based running vertex id */
        static const int quad[6][4] = {
            {0,1,2,3}, {4,5,6,7}, {0,1,5,4},
            {1,2,6,5}, {2,3,7,6}, {3,0,4,7}
        };
        for (int bi = 0; bi < m->nboxes; bi++) {
            cid_vec3 c[8];
            box_corners(&m->boxes[bi], c);
            for (int j = 0; j < 8; j++)
                fprintf(f, "v %.5f %.5f %.5f\n", c[j].x, c[j].y, c[j].z);
            for (int q = 0; q < 6; q++) {
                unsigned a = base + (unsigned)quad[q][0];
                unsigned b = base + (unsigned)quad[q][1];
                unsigned cc = base + (unsigned)quad[q][2];
                unsigned d = base + (unsigned)quad[q][3];
                fprintf(f, "f %u %u %u\n", a, b, cc);
                fprintf(f, "f %u %u %u\n", a, cc, d);
            }
            base += 8u;
        }
    }

    fclose(f);
    return 0;
}
