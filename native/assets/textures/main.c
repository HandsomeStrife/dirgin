/* main.c - dtex CLI: decode DoC TXD/RFD containers (or bare payloads) to PNG.
 *
 *   dtex <input.txd|.rfd|.bin ...> [-o OUTDIR]
 *
 * Writes <OUTDIR>/<stem>_<name>.png for every texture found and prints a line
 * per texture (dimensions / format / deswizzle state). Input files are treated
 * as untrusted data: only read, never executed.
 */
#include "dtex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *sz) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *sz = (size_t)n;
    return buf;
}

static const char *psm_name(int psm) {
    switch (psm) {
        case PSM_CT32: return "PSMCT32"; case PSM_CT24: return "PSMCT24";
        case PSM_CT16: return "PSMCT16"; case PSM_CT16S: return "PSMCT16S";
        case PSM_T8: return "PSMT8"; case PSM_T8H: return "PSMT8H";
        case PSM_T4: return "PSMT4"; case PSM_T4HL: return "PSMT4HL";
        case PSM_T4HH: return "PSMT4HH"; default: return "?";
    }
}

static void stem_of(const char *path, char *out, size_t n) {
    const char *base = path, *p = path;
    for (; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    size_t len = strlen(base);
    const char *dot = base + len;
    for (const char *q = base; *q; q++) if (*q == '.') dot = q;
    size_t m = (size_t)(dot - base);
    if (m >= n) m = n - 1;
    memcpy(out, base, m); out[m] = 0;
}

int main(int argc, char **argv) {
    const char *outdir = ".";
    const char **inputs = (const char **)malloc(sizeof(char *) * (argc > 0 ? argc : 1));
    int ninput = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outdir = argv[++i];
        else inputs[ninput++] = argv[i];
    }
    if (ninput == 0) {
        fprintf(stderr, "usage: %s <input.txd|.rfd|.bin ...> [-o OUTDIR]\n", argv[0]);
        return 2;
    }

    int total = 0, failed = 0;
    for (int i = 0; i < ninput; i++) {
        size_t sz;
        uint8_t *data = read_file(inputs[i], &sz);
        if (!data) { fprintf(stderr, "cannot read %s\n", inputs[i]); failed++; continue; }

        char stem[256]; stem_of(inputs[i], stem, sizeof(stem));
        dtex_set set;
        if (dtex_decode_container(data, sz, &set) != 0) {
            fprintf(stderr, "%s: no textures decoded\n", inputs[i]);
            free(data); failed++; continue;
        }
        for (int k = 0; k < set.count; k++) {
            dtex_image *im = &set.items[k];
            char path[1024];
            char nm[17]; memcpy(nm, im->name, 16); nm[16] = 0;
            if (nm[0] == 0) snprintf(nm, sizeof(nm), "%d", k);
            snprintf(path, sizeof(path), "%s/%s_%s.png", outdir, stem, nm);
            int rc = dtex_write_png(path, im->rgba, im->width, im->height);
            printf("%-10s %-9s %4dx%-4d %-8s %s%s -> %s\n",
                   stem, im->source, im->width, im->height, psm_name(im->psm),
                   im->clut_colors ? "clut" : "direct",
                   im->deswizzled ? "/deswz" : "",
                   rc == 0 ? path : "(write failed)");
            total++;
        }
        dtex_free_set(&set);
        free(data);
    }
    fprintf(stderr, "decoded %d texture(s), %d input error(s)\n", total, failed);
    free(inputs);
    return failed ? 1 : 0;
}
