/* env_decode.c -- CLI around doc_env: decode a room BMD chunk and print its
 * vertex-exact parity signature. Used to validate the native decoder against the
 * Python oracle (export_bmd_textured.py).
 *
 *   env_decode <chunk.bin ...>
 *     -> one line per input: "<chunk> npk <n> nv <total> fnv <hex>"
 *
 * Inputs are untrusted extracted game data: only read, never executed.
 */
#include "doc_env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *sz) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *sz = (size_t)n; return b;
}

static const char *stem(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <chunk.bin ...>\n", argv[0]); return 2; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        size_t sz;
        uint8_t *b = read_file(argv[i], &sz);
        if (!b) { fprintf(stderr, "%s: cannot read\n", argv[i]); rc = 1; continue; }
        long pay = doc_env_find_payload(b, sz);
        if (pay < 0) { fprintf(stderr, "%s: no BMD payload\n", argv[i]); free(b); rc = 1; continue; }
        DocEnvPackets pk;
        if (doc_env_decode(b, sz, pay, &pk) != 0) { fprintf(stderr, "%s: decode error\n", argv[i]); free(b); rc = 1; continue; }
        long nv = 0; for (int k = 0; k < pk.n; k++) nv += pk.pk[k].nv;
        printf("%.8s npk %d nv %ld fnv %016llx\n", stem(argv[i]), pk.n, nv,
               (unsigned long long)doc_env_fnv(&pk));
        doc_env_free(&pk);
        free(b);
    }
    return rc;
}
