/* main.c - CLI for the CID collision loader.
 *
 *   cidtool <mapid.rfd> [out.obj] [--boxes] [--quiet]
 *
 * Parses the CID collision payload, prints a validation report (version,
 * triangle/vertex counts, bbox, decode coverage) and, if an output path is
 * given, writes the collision mesh to OBJ. With --boxes the undecoded tag-2
 * (VU-packed) leaves are emitted as AABB boxes for full spatial coverage.
 */
#include "cid.h"
#include <stdio.h>
#include <string.h>

int cid_write_obj(const cid_mesh *m, const char *path, int include_boxes);

int main(int argc, char **argv) {
    const char *in = NULL, *outobj = NULL;
    int boxes = 0, quiet = 0;
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--boxes") == 0) boxes = 1;
        else if (strcmp(argv[i], "--quiet") == 0) quiet = 1;
        else if (!in)     in = argv[i];
        else if (!outobj) outobj = argv[i];
    }
    if (!in) {
        fprintf(stderr,
            "usage: %s <mapid.rfd|.cid> [out.obj] [--boxes] [--quiet]\n", argv[0]);
        return 2;
    }

    cid_mesh m;
    char err[256];
    int r = cid_load_file(in, &m, err, sizeof err);
    if (r != CID_OK) {
        fprintf(stderr, "error: %s (%s)\n", cid_strerror(r), err[0] ? err : "-");
        return 1;
    }

    if (!quiet) {
        printf("file        : %s\n", in);
        printf("cid version : %d (%s)\n", m.version,
               m.version <= 8 ? "inline quad-soup" : "grid/indexed tree");
        printf("vertices    : %d\n", m.nverts);
        printf("triangles   : %d\n", m.ntris);
        if (m.version <= 8) {
            printf("v2 leaves   : %d (inline type-7 quad leaves)\n", m.n_leaves_v2);
        } else {
            printf("indexed lvs : %d (tag-0, decoded to triangles)\n", m.n_nodes_tag0);
            printf("vu lvs      : %d (tag-2, VU-packed)\n", m.n_nodes_tag2);
            printf("prim nodes  : %d (tag-1 box/cylinder/sphere, tessellated)\n", m.n_nodes_tag1);
            printf("vu packets  : %d total, %d decoded -> %d tris, %d boxed (fallback)\n",
                   m.n_tag2_packets, m.n_tag2_pkts_decoded, m.n_tag2_tris,
                   m.n_tag2_pkts_boxed);
            printf("vu degenerate: %d zero-area tag-2 tris skipped (source artifacts)\n",
                   m.n_tag2_degenerate);
            printf("fallback box: %d (undecoded tag-2 leaves)\n", m.nboxes);
            printf("surf table  : %d entries\n", m.surf_table_count);
        }
        printf("bbox min    : %.2f %.2f %.2f\n", m.bb_min.x, m.bb_min.y, m.bb_min.z);
        printf("bbox max    : %.2f %.2f %.2f\n", m.bb_max.x, m.bb_max.y, m.bb_max.z);
        printf("bbox size   : %.2f x %.2f x %.2f\n",
               m.bb_max.x - m.bb_min.x, m.bb_max.y - m.bb_min.y,
               m.bb_max.z - m.bb_min.z);
        if (err[0]) printf("note        : %s\n", err);
    }

    if (outobj) {
        if (cid_write_obj(&m, outobj, boxes) != 0) {
            fprintf(stderr, "error: cannot write %s\n", outobj);
            cid_free(&m);
            return 1;
        }
        if (!quiet) printf("wrote OBJ   : %s%s\n", outobj,
                            boxes ? " (+ tag-2 AABB boxes)" : "");
    }

    cid_free(&m);
    return 0;
}
