/* kel_tool - CLI around the native loader, for verification.
 *   kel_tool <FILELIST.BIN> <KEL.DAT> list [N]
 *   kel_tool <FILELIST.BIN> <KEL.DAT> extract <virtual/path> <out_file>
 *   kel_tool <FILELIST.BIN> <KEL.DAT> extractcode <hexcode> <out_file> */
#include "kel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s FILELIST KEL.DAT list|extract|extractcode ...\n", argv[0]); return 2; }
    kel_archive a;
    if (kel_open(&a, argv[1], argv[2]) != 0) return 1;

    if (strcmp(argv[3], "list") == 0) {
        long n = argc > 4 ? atol(argv[4]) : 10;
        printf("entries=%u\n", a.count);
        for (long i = 0; i < n && i < a.count; i++) {
            kel_entry *e = &a.entries[i];
            printf("  code=%08x pos=%x usize=%x csize=%x %s\n",
                   e->filecode, e->pos, e->usize, e->csize,
                   e->path[0] ? e->path : "(noPath)");
        }
    } else if (strcmp(argv[3], "extract") == 0 || strcmp(argv[3], "extractcode") == 0) {
        if (argc < 6) { fprintf(stderr, "need <path|code> <out_file>\n"); kel_close(&a); return 2; }
        const kel_entry *e = (strcmp(argv[3], "extract") == 0)
            ? kel_find_path(&a, argv[4])
            : kel_find_code(&a, (uint32_t)strtoul(argv[4], NULL, 16));
        if (!e) { fprintf(stderr, "not found: %s\n", argv[4]); kel_close(&a); return 1; }
        uint8_t *buf = NULL;
        long n = kel_read(&a, e, &buf);
        if (n < 0) { fprintf(stderr, "read failed\n"); kel_close(&a); return 1; }
        FILE *o = fopen(argv[5], "wb");
        fwrite(buf, 1, n, o); fclose(o); free(buf);
        printf("wrote %ld bytes to %s (%s)\n", n, argv[5], e->usize==e->csize?"raw":"inflated");
    }
    kel_close(&a);
    return 0;
}
