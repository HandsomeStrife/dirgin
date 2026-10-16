/* =====================================================================
 * test_material_loader.c  --  Parity test for the room-binding loader
 * (scene/material_loader.{h,c}) driving the reauthored consumer (material.c).
 *
 * The loader synthesizes a DocMaterialModel + DocTextureContext from a baked
 * manifest. This test proves the synthesized model, fed through the UNMODIFIED
 * material.c resolve chain (doc_material_resolve_texture), reproduces the baked
 * per-packet binding EXACTLY -- i.e. the group/submesh encoding, selector
 * indexing, layer-tag format and default path are all correct.
 *
 *   1. SYNTHETIC fixture (always runs, no game data): a tiny hand-written
 *      manifest with bound + unbound packets; every packet must resolve to its
 *      manifest slot, the per-slot histogram must match, unbound -> slot -1.
 *   2. REAL z143 parity (runs iff a manifest path is given AND exists): the
 *      068f0112 manifest baked by emit_loader_manifest.py. Asserts the resolved
 *      per-slot histogram == the baked counts (which == the recovered
 *      z143_room_uvtriple_binding.json by_filecode), unbound == baked count,
 *      and every packet's resolved slot == the baked slot. Skips cleanly when
 *      the manifest is absent (no disc / bake not run), so CI stays green.
 * ===================================================================== */
#include "material.h"
#include "material_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* Resolve every packet through the real material.c chain and tally per slot.
 * Returns matched (resolved to a real slot). Fills tally[0..nslots-1] and
 * *unbound. Also checks per-packet resolved slot == baked expect_slot. */
static int resolve_all(const DocRoomBinding *b, int *tally, int *unbound,
                       const char *label)
{
    int matched = 0;
    *unbound = 0;
    for (int s = 0; s < b->nslots; s++) tally[s] = 0;

    for (int i = 0; i < b->npackets; i++) {
        DocTextureDesc desc;
        DocSelectStatus st = doc_material_resolve_texture(&b->model, &b->ctx,
                                                          b->draw_words[i], &desc);
        CHECK(st == DOC_SEL_OK, "%s: packet %d select status %d (expected OK)", label, i, (int)st);
        CHECK(desc.slot == b->expect_slot[i],
              "%s: packet %d resolved slot %d != baked %d", label, i,
              (int)desc.slot, (int)b->expect_slot[i]);
        if (desc.slot >= 0 && desc.slot < b->nslots) { tally[desc.slot]++; matched++; }
        else (*unbound)++;
    }
    return matched;
}

/* ---- 1. synthetic fixture (always runs) ------------------------------ */
static void test_synthetic(void)
{
    const char *path = "test_material_loader_synth.manifest";
    FILE *f = fopen(path, "w");
    if (!f) { printf("FAIL: cannot write synthetic manifest\n"); g_fail++; return; }
    fprintf(f,
        "# synthetic fixture\n"
        "chunk testzz00\n"
        "slots 3\n"
        "slot 0 068faaaa 2 oracle\n"
        "slot 1 068fbbbb 1 oracle\n"
        "slot 2 068fcccc 1 oracle\n"
        "packets 6\n"
        "0\n-1\n1\n0\n2\n-1\n");
    fclose(f);

    DocRoomBinding b;
    char err[256] = {0};
    int rc = doc_room_binding_load(path, &b, err, sizeof err);
    remove(path);
    CHECK(rc == 0, "synthetic: load failed: %s", err);
    if (rc != 0) return;

    CHECK(b.nslots == 3, "synthetic: nslots %d != 3", b.nslots);
    CHECK(b.npackets == 6, "synthetic: npackets %d != 6", b.npackets);
    CHECK(b.unbound_count == 2, "synthetic: unbound %d != 2", b.unbound_count);
    CHECK(b.model.group_count == 4, "synthetic: group_count %d != 4", (int)b.model.group_count);
    CHECK(strcmp(doc_room_binding_slot_filecode(&b, 1), "068fbbbb") == 0,
          "synthetic: slot 1 filecode wrong");

    int tally[3], unbound;
    int matched = resolve_all(&b, tally, &unbound, "synthetic");
    CHECK(matched == 4, "synthetic: matched %d != 4", matched);
    CHECK(unbound == 2, "synthetic: unbound %d != 2", unbound);
    CHECK(tally[0] == 2 && tally[1] == 1 && tally[2] == 1,
          "synthetic: histogram {%d,%d,%d} != {2,1,1}", tally[0], tally[1], tally[2]);

    doc_room_binding_free(&b);
    printf("synthetic fixture: 6 packets, 4 matched + 2 unbound, histogram OK\n");
}

/* ---- 2. real z143 068f0112 parity (runs iff manifest present) -------- */
static void test_real(const char *manifest_path)
{
    FILE *probe = fopen(manifest_path, "r");
    if (!probe) {
        printf("SKIP real parity: no manifest at %s\n", manifest_path);
        printf("  (bake it: python3 -I native/assets/models/emit_loader_manifest.py \\\n");
        printf("            extract/_KEL/noPath/068f0112.bin \\\n");
        printf("            native/assets/models/out/z143_pktfc/pf_068f0112.json \\\n");
        printf("            -o native/assets/models/out/z143_loader/068f0112.manifest)\n");
        return;
    }
    fclose(probe);

    DocRoomBinding b;
    char err[256] = {0};
    int rc = doc_room_binding_load(manifest_path, &b, err, sizeof err);
    CHECK(rc == 0, "real: load failed: %s", err);
    if (rc != 0) return;

    int *tally = calloc((size_t)(b.nslots ? b.nslots : 1), sizeof *tally);
    int unbound;
    int matched = resolve_all(&b, tally, &unbound, "real");

    /* resolved histogram must equal the baked per-slot counts exactly */
    int baked_matched = 0;
    for (int s = 0; s < b.nslots; s++) {
        CHECK(tally[s] == b.slot_count[s],
              "real: slot %d (%s) resolved %d != baked %d", s,
              doc_room_binding_slot_filecode(&b, s), tally[s], b.slot_count[s]);
        baked_matched += b.slot_count[s];
    }
    CHECK(matched == baked_matched, "real: matched %d != baked %d", matched, baked_matched);
    CHECK(unbound == b.unbound_count, "real: unbound %d != baked %d", unbound, b.unbound_count);

    printf("real parity [%s]: %d packets, %d matched across %d textures, %d unbound%s\n",
           b.chunk, b.npackets, matched, b.nslots, unbound,
           g_fail ? " (WITH FAILURES)" : " -- histogram matches recovered binding");

    free(tally);
    doc_room_binding_free(&b);
}

int main(int argc, char **argv)
{
    test_synthetic();

    /* argv[1] overrides; otherwise try the default baked location. */
    const char *manifest = (argc > 1)
        ? argv[1]
        : "../assets/models/out/z143_loader/068f0112.manifest";
    test_real(manifest);

    if (g_fail) { printf("\n%d CHECK(s) FAILED\n", g_fail); return 1; }
    printf("\nall material_loader checks passed\n");
    return 0;
}
