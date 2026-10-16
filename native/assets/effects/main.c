/* main.c — CLI for the FED/FEZ effect-definition parser.
 *
 *   fed <file.fed|file.fez> [--json|--text]
 *
 * Loads the file as untrusted data, parses it, and prints a dump.  Exit code
 * is 0 on a clean parse, 1 on error. */

#include "fed.h"
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *n){
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    rewind(fp);
    uint8_t *buf = (uint8_t*)malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { free(buf); fclose(fp); return NULL; }
    fclose(fp);
    *n = (size_t)sz;
    return buf;
}

int main(int argc, char **argv){
    if (argc < 2){
        fprintf(stderr, "usage: %s <file.fed|file.fez> [--json|--text]\n", argv[0]);
        return 2;
    }
    int json = 0;
    for (int i = 2; i < argc; i++){
        if      (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--text")) json = 0;
    }

    size_t n = 0;
    uint8_t *buf = read_file(argv[1], &n);
    if (!buf){ fprintf(stderr, "error: cannot read '%s'\n", argv[1]); return 1; }

    FedFile f;
    char err[128];
    if (fed_load(buf, n, &f, err, sizeof err) != 0){
        fprintf(stderr, "error: %s\n", err);
        free(buf);
        return 1;
    }

    if (json) fed_dump_json(&f, stdout);
    else      fed_dump_text(&f, stdout);

    fed_free(&f);
    free(buf);
    return 0;
}
