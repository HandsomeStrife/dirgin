#include "kel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}

/* zlib inflate a whole buffer into a freshly allocated out buffer of known size. */
static int inflate_all(const uint8_t *src, uLong slen, uint8_t *dst, uLong dlen) {
    z_stream z; memset(&z, 0, sizeof z);
    if (inflateInit(&z) != Z_OK) return -1;
    z.next_in = (Bytef*)src; z.avail_in = slen;
    z.next_out = dst;        z.avail_out = dlen;
    int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return (r == Z_STREAM_END || r == Z_OK) ? 0 : -1;
}

int kel_open(kel_archive *a, const char *filelist_path, const char *dat_path) {
    memset(a, 0, sizeof *a);
    snprintf(a->dat_path, sizeof a->dat_path, "%s", dat_path);

    FILE *f = fopen(filelist_path, "rb");
    if (!f) { perror("open FILELIST"); return -1; }
    fseek(f, 0, SEEK_END); long flen = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *fl = malloc(flen);
    if (fread(fl, 1, flen, f) != (size_t)flen) { fclose(f); free(fl); return -1; }
    fclose(f);

    uint32_t chunk_info_off = rd_u32le(fl + 0);
    uint32_t chunk_data_off = rd_u32le(fl + 4);
    uint32_t file_count     = rd_u32le(fl + 8);
    uint32_t num_chunks     = (chunk_data_off - chunk_info_off) / 0xC;

    /* Decompress every index chunk. */
    uint8_t **chunks = calloc(num_chunks, sizeof(uint8_t*));
    uint32_t *clens  = calloc(num_chunks, sizeof(uint32_t));
    for (uint32_t c = 0; c < num_chunks; c++) {
        const uint8_t *ci = fl + chunk_info_off + c*0xC;
        uint32_t usize = rd_u32le(ci+0), csize = rd_u32le(ci+4), cpos = rd_u32le(ci+8);
        uint8_t *dec = malloc(usize);
        if (inflate_all(fl + chunk_data_off + cpos, csize, dec, usize) != 0) {
            fprintf(stderr, "chunk %u inflate failed\n", c); free(dec); dec = NULL; usize = 0;
        }
        chunks[c] = dec; clens[c] = usize;
    }

    a->entries = calloc(file_count, sizeof(kel_entry));
    a->count = file_count;
    for (uint32_t i = 0; i < file_count; i++) {
        const uint8_t *fe = fl + 0xC + i*8;
        uint32_t code = rd_u32le(fe);
        uint16_t cn   = (uint16_t)(fe[4] | fe[5]<<8);
        uint16_t pc   = (uint16_t)(fe[6] | fe[7]<<8);
        kel_entry *e = &a->entries[i];
        e->filecode = code;
        if (cn < num_chunks && chunks[cn] && pc < clens[cn]) {
            /* read NUL/newline-terminated "pos:usize:csize:path" (hex fields) */
            const char *s = (const char*)(chunks[cn] + pc);
            uint32_t maxlen = clens[cn] - pc, n = 0;
            char buf[160];
            while (n < maxlen && n < sizeof(buf)-1 && s[n] != '\n' && s[n] != '\0') { buf[n] = s[n]; n++; }
            buf[n] = 0;
            char *p1 = strchr(buf, ':');
            char *p2 = p1 ? strchr(p1+1, ':') : NULL;
            char *p3 = p2 ? strchr(p2+1, ':') : NULL;
            if (p1 && p2) {
                e->pos   = (uint32_t)strtoul(buf,   NULL, 16);
                e->usize = (uint32_t)strtoul(p1+1, NULL, 16);
                e->csize = (uint32_t)strtoul(p2+1, NULL, 16);
                if (p3) {
                    const char *path = p3 + 1;
                    /* skip leading spaces; blank => noPath */
                    while (*path == ' ') path++;
                    snprintf(e->path, sizeof e->path, "%s", path);
                }
            }
        }
    }

    for (uint32_t c = 0; c < num_chunks; c++) free(chunks[c]);
    free(chunks); free(clens); free(fl);
    return 0;
}

void kel_close(kel_archive *a) { free(a->entries); a->entries = NULL; a->count = 0; }

const kel_entry *kel_find_path(const kel_archive *a, const char *path) {
    for (uint32_t i = 0; i < a->count; i++)
        if (a->entries[i].path[0] && strcmp(a->entries[i].path, path) == 0) return &a->entries[i];
    return NULL;
}
const kel_entry *kel_find_code(const kel_archive *a, uint32_t code) {
    for (uint32_t i = 0; i < a->count; i++)
        if (a->entries[i].filecode == code) return &a->entries[i];
    return NULL;
}

long kel_read(const kel_archive *a, const kel_entry *e, uint8_t **out) {
    FILE *f = fopen(a->dat_path, "rb");
    if (!f) { perror("open KEL.DAT"); return -1; }
    if (fseek(f, (long)e->pos * 0x800, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t *blob = malloc(e->csize);
    if (fread(blob, 1, e->csize, f) != e->csize) { fclose(f); free(blob); return -1; }
    fclose(f);

    if (e->usize == e->csize) { *out = blob; return e->usize; }  /* stored raw */
    uint8_t *dec = malloc(e->usize);
    if (inflate_all(blob, e->csize, dec, e->usize) != 0) { free(blob); free(dec); return -1; }
    free(blob);
    *out = dec; return e->usize;
}
