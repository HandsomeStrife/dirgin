/* png.c - minimal, self-contained RGBA8 -> PNG writer (no zlib).
 * Uses stored (uncompressed) DEFLATE blocks so results are viewable anywhere.
 */
#include "dtex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];
static int crc_ready = 0;
static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320u ^ (c >> 1) : (c >> 1);
        crc_table[n] = c;
    }
    crc_ready = 1;
}
static uint32_t crc32_upd(uint32_t crc, const uint8_t *buf, size_t len) {
    if (!crc_ready) crc_init();
    crc ^= 0xffffffffu;
    for (size_t i = 0; i < len; i++) crc = crc_table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}
static uint32_t adler32(const uint8_t *buf, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) { a = (a + buf[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static int write_chunk(FILE *f, const char *type, const uint8_t *data, size_t len) {
    uint8_t hdr[8];
    put_be32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;
    uint32_t crc = crc32_upd(0, (const uint8_t *)type, 4);
    crc = crc32_upd(crc, data, len);
    uint8_t c[4]; put_be32(c, crc);
    return (fwrite(c, 1, 4, f) == 4) ? 0 : -1;
}

int dtex_write_png(const char *path, const uint8_t *rgba, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    static const uint8_t sig[8] = {0x89,'P','N','G','\r','\n',0x1a,'\n'};
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;    /* bit depth    */
    ihdr[9] = 6;    /* colour type RGBA */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    if (write_chunk(f, "IHDR", ihdr, 13)) { fclose(f); return -1; }

    /* Raw (filtered) image: filter byte 0 per row. */
    size_t raw_len = (size_t)h * (1 + (size_t)w * 4);
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) { fclose(f); return -1; }
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (1 + (size_t)w * 4);
        row[0] = 0;
        memcpy(row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }

    /* zlib stream with stored deflate blocks. */
    size_t nblocks = (raw_len + 65534) / 65535;
    if (nblocks == 0) nblocks = 1;
    size_t z_len = 2 + nblocks * 5 + raw_len + 4;
    uint8_t *z = (uint8_t *)malloc(z_len);
    if (!z) { free(raw); fclose(f); return -1; }
    size_t p = 0;
    z[p++] = 0x78; z[p++] = 0x01;            /* zlib header */
    size_t left = raw_len, src = 0;
    while (left > 0 || raw_len == 0) {
        size_t chunk = left > 65535 ? 65535 : left;
        int final = (left - chunk == 0);
        z[p++] = final ? 1 : 0;              /* BFINAL, BTYPE=00 */
        z[p++] = (uint8_t)(chunk & 0xff);
        z[p++] = (uint8_t)((chunk >> 8) & 0xff);
        z[p++] = (uint8_t)(~chunk & 0xff);
        z[p++] = (uint8_t)((~chunk >> 8) & 0xff);
        memcpy(z + p, raw + src, chunk);
        p += chunk; src += chunk; left -= chunk;
        if (raw_len == 0) break;
    }
    put_be32(z + p, adler32(raw, raw_len)); p += 4;

    int rc = write_chunk(f, "IDAT", z, p);
    free(z); free(raw);
    if (rc) { fclose(f); return -1; }
    if (write_chunk(f, "IEND", NULL, 0)) { fclose(f); return -1; }
    fclose(f);
    return 0;
}
