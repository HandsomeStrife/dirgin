/*
 * doctext.c - Dirge of Cerberus (FFVII) "text-bin" decryptor + string-table parser
 *
 * PURIST native C port. No external deps except zlib (body section is zlib-compressed).
 *
 * Cipher: transcribed from the Shademp / DoCCryptTool algorithm
 *   (https://github.com/Surihix/DoCCryptTool, CryptoClasses) and cross-checked
 *   against the game's own decompiled routine FUN_0041bbf8 in
 *   /srv/games/dirge-port/analysis/decomp/ which carries the same magic constant
 *   0xA1652347, the same keyblock index (counter & 0xF8) and the same
 *   (c | c<<30 | c<<20 | c<<10) block-counter expansion, confirming the transcription.
 *
 * File layout (encrypted, magic 45 90 AF F9 B7 47 F6 94 == encrypted seed {1,0}):
 *   [0x00 .. 0x1F]  Header section (4 blocks, 32 bytes) - encrypted w/ header key
 *   [0x20 .. N]     Body section    (M blocks)          - encrypted w/ body key, then zlib
 *   [tail]          Decrypted text snippet (plaintext, size at header+0x0C)
 *
 * Header (after decrypt):
 *   0x00 u32 seedA (==1)     0x04 u32 seedB (==0)
 *   0x08 u16 lineCount       0x0A u8 reserved   0x0B u8 compressionFlag
 *   0x0C u32 txtSnippetSize  0x10 u32 decompBodySize  0x14 u32 fileSize
 *   0x18 u32 headerSize(24)  0x1C u32 headerChecksum
 *
 * Decompressed body: per-line table of 3 u32 { id, lineIdPos(abs), linePos(abs) },
 *   lineCount entries; then NUL-terminated line strings, then NUL-terminated line-id
 *   strings. Strings are Shift-JIS (with a few game-specific control bytes).
 *
 * Usage: doctext <encrypted.bin> [--dec out.dec] [--body out.body] [--json]
 *        default: prints decoded lines as UTF-8.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>

/* ---- Integers[] lookup from DoCCryptTool: Integers[x] = (x + 120) & 0xFF ---- */
static inline uint8_t integ(uint8_t x) { return (uint8_t)(x + 120); }

/* rotate helpers */
static inline uint32_t rotl32(uint32_t v, int n){ return (v << n) | (v >> (32 - n)); }

/* Generate the 256-byte keyblocks table (32 blocks of 8) from an 8-byte seed. */
static void gen_keyblocks(const uint8_t seed[8], uint8_t table[256])
{
    uint32_t a = (uint32_t)seed[0] | (seed[1]<<8) | (seed[2]<<16) | (uint32_t)seed[3]<<24;
    uint32_t b = (uint32_t)seed[4] | (seed[5]<<8) | (seed[6]<<16) | (uint32_t)seed[7]<<24;
    a = rotl32(a, 8);
    b = rotl32(b, 16);
    uint8_t kb[8];
    /* keyblock = bytes(b) ++ bytes(a), little-endian */
    kb[0] = b & 0xFF; kb[1] = (b>>8)&0xFF; kb[2] = (b>>16)&0xFF; kb[3] = (b>>24)&0xFF;
    kb[4] = a & 0xFF; kb[5] = (a>>8)&0xFF; kb[6] = (a>>16)&0xFF; kb[7] = (a>>24)&0xFF;
    kb[0] = (uint8_t)(kb[0] + 0x45);
    /* Loop 1 */
    for (int i = 1; i < 8; i++) {
        int tmp = kb[i] + 0xD4 + kb[i-1];
        tmp ^= (kb[i-1]) << 2;
        tmp ^= 0x45;
        kb[i] = (uint8_t)tmp;
    }
    memcpy(table, kb, 8);
    /* Loop 2 */
    uint64_t prev = 0;
    for (int i = 0; i < 8; i++) prev |= (uint64_t)kb[i] << (8*i);
    int copy = 8;
    for (int i = 1; i < 0x20; i++) {
        uint64_t cur = (prev << 2) + prev;   /* 64-bit add, matches split halves + carry */
        for (int j = 0; j < 8; j++) table[copy + j] = (uint8_t)(cur >> (8*j));
        prev = cur;
        copy += 8;
    }
}

/* LoopAByte: 8 iterations of b = (Integers[b] - key[off+i]) & 0xFF */
static inline uint8_t loop_a_byte(uint8_t b, const uint8_t *table, uint32_t off)
{
    for (int i = 0; i < 8; i++) {
        int v = (int)integ(b) - (int)table[off + i];
        b = (uint8_t)(v & 0xFF);
    }
    return b;
}

/*
 * Decrypt `blocks` 8-byte blocks from in[readPos..] to out[writePos..].
 * blockCounter restarts at 0 each call (per DoCCryptTool).
 */
static void decrypt_blocks(const uint8_t *table, uint32_t blocks,
                           const uint8_t *in, size_t in_off,
                           uint8_t *out, size_t out_off)
{
    uint32_t blockCounter = 0;
    for (uint32_t i = 0; i < blocks; i++) {
        uint32_t currentBlockId = blockCounter >> 3;
        const uint8_t *c = in + in_off + (size_t)i * 8;

        /* tableOffset = ((blockCounter & 0xFFFF) >> 3 << 3) & 0xF8 */
        uint32_t tableOffset = (uint32_t)(((uint16_t)(blockCounter & 0xFFFF) >> 3) << 3) & 0xF8;

        uint8_t d1 = loop_a_byte((uint8_t)(((currentBlockId ^ 69) & 255) ^ c[0]), table, tableOffset);
        uint8_t d2 = loop_a_byte((uint8_t)(c[0] ^ c[1]), table, tableOffset);
        uint8_t d3 = loop_a_byte((uint8_t)(c[1] ^ c[2]), table, tableOffset);
        uint8_t d4 = loop_a_byte((uint8_t)(c[2] ^ c[3]), table, tableOffset);
        uint8_t d5 = loop_a_byte((uint8_t)(c[3] ^ c[4]), table, tableOffset);
        uint8_t d6 = loop_a_byte((uint8_t)(c[4] ^ c[5]), table, tableOffset);
        uint8_t d7 = loop_a_byte((uint8_t)(c[5] ^ c[6]), table, tableOffset);
        uint8_t d8 = loop_a_byte((uint8_t)(c[6] ^ c[7]), table, tableOffset);

        /* arr = {d5,d6,d7,d8, d1,d2,d3,d4}; higher=u32(arr[0..3]) lower=u32(arr[4..7]) */
        uint32_t higher = (uint32_t)d5 | (d6<<8) | (d7<<16) | ((uint32_t)d8<<24);
        uint32_t lower  = (uint32_t)d1 | (d2<<8) | (d3<<16) | ((uint32_t)d4<<24);

        uint32_t kLow  = (uint32_t)table[tableOffset+0] | (table[tableOffset+1]<<8) |
                         (table[tableOffset+2]<<16) | ((uint32_t)table[tableOffset+3]<<24);
        uint32_t kHigh = (uint32_t)table[tableOffset+4] | (table[tableOffset+5]<<8) |
                         (table[tableOffset+6]<<16) | ((uint32_t)table[tableOffset+7]<<24);

        /* BlockCounterEval/Fval = 64-bit ( bc | bc<<10 | bc<<20 | bc<<30 ) */
        uint64_t ev64 = (uint64_t)blockCounter;
        ev64 |= (uint64_t)blockCounter << 10;
        ev64 |= (uint64_t)blockCounter << 20;
        ev64 |= (uint64_t)blockCounter << 30;
        uint32_t Eval = (uint32_t)(ev64 & 0xFFFFFFFF);
        uint32_t Fval = (uint32_t)(ev64 >> 32);

        uint32_t carry = (Eval > 0x5E9ADCB8u) ? 1u : 0u;   /* ~0xA1652347 */
        int64_t specialKey1 = (int64_t)Eval + 0xA1652347LL;
        int64_t specialKey2 = (int64_t)Fval + carry;

        int64_t lo = (int64_t)lower;
        int64_t hi = (int64_t)higher;

        uint32_t c2 = (lo < (int64_t)kLow) ? 1u : 0u;
        lo -= (int64_t)kLow;
        hi -= (int64_t)kHigh;
        hi -= c2;

        lo ^= specialKey1;
        hi ^= specialKey2;

        lo ^= (int64_t)kLow;
        hi ^= (int64_t)kHigh;

        uint32_t outHigh = (uint32_t)hi;
        uint32_t outLow  = (uint32_t)lo;

        uint8_t *o = out + out_off + (size_t)i * 8;
        o[0] = outHigh & 0xFF; o[1] = (outHigh>>8)&0xFF; o[2] = (outHigh>>16)&0xFF; o[3] = (outHigh>>24)&0xFF;
        o[4] = outLow  & 0xFF; o[5] = (outLow>>8)&0xFF;  o[6] = (outLow>>16)&0xFF;  o[7] = (outLow>>24)&0xFF;

        blockCounter += 8;
    }
}

static uint32_t rd_u32(const uint8_t *p){ return (uint32_t)p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t rd_u16(const uint8_t *p){ return (uint16_t)(p[0]|(p[1]<<8)); }

/* Public API: decrypt a whole text-bin into a malloc'd buffer (same size as input). */
uint8_t *doctext_decrypt(const uint8_t *data, size_t len, size_t *out_len)
{
    static const uint8_t headerSeed[8] = {136, 86, 49, 149, 241, 163, 137, 87};
    static const uint8_t bodySeed[8]   = {1, 0, 0, 0, 0, 0, 0, 0};
    if (len < 32) return NULL;

    uint8_t htab[256], btab[256];
    gen_keyblocks(headerSeed, htab);
    gen_keyblocks(bodySeed, btab);

    uint8_t *out = (uint8_t*)malloc(len);
    if (!out) return NULL;
    memcpy(out, data, len);

    /* header: 4 blocks @ 0 */
    decrypt_blocks(htab, 4, data, 0, out, 0);

    uint32_t txtSnippetSize = rd_u32(out + 0x0C);
    if ((uint64_t)txtSnippetSize + 32 > len) { free(out); return NULL; }
    size_t bodySize = len - txtSnippetSize - 32;
    if (bodySize % 8 != 0) { free(out); return NULL; }
    uint32_t blocks = (uint32_t)(bodySize / 8);

    decrypt_blocks(btab, blocks, data, 32, out, 32);
    /* tail (txt snippet) already copied verbatim from data */
    if (out_len) *out_len = len;
    return out;
}

/* Inflate the body (if compressed) into a malloc'd buffer. Returns NULL on failure. */
static uint8_t *inflate_body(const uint8_t *dec, size_t len, uint32_t *out_len)
{
    uint8_t compFlag = dec[0x0B];
    uint32_t decompSize = rd_u32(dec + 0x10);
    uint32_t txtSnippetSize = rd_u32(dec + 0x0C);
    size_t bodySize = len - txtSnippetSize - 32;
    if (bodySize < 8) return NULL;
    const uint8_t *body = dec + 32;
    size_t bodyDataLen = bodySize - 8;    /* exclude 8-byte body footer (size+checksum) */

    if (!compFlag) {
        uint8_t *buf = (uint8_t*)malloc(decompSize ? decompSize : bodyDataLen);
        if (!buf) return NULL;
        memcpy(buf, body, decompSize ? decompSize : bodyDataLen);
        *out_len = decompSize ? decompSize : (uint32_t)bodyDataLen;
        return buf;
    }

    uint8_t *buf = (uint8_t*)malloc(decompSize);
    if (!buf) return NULL;
    uLongf dl = decompSize;
    /* zlib stream; trailing footer/padding is harmless to uncompress() which stops at Z_STREAM_END */
    int rc = uncompress(buf, &dl, body, (uLong)bodyDataLen + 8);
    if (rc != Z_OK && rc != Z_BUF_ERROR) {
        /* retry giving only bodyDataLen */
        dl = decompSize;
        rc = uncompress(buf, &dl, body, (uLong)bodyDataLen);
    }
    if (rc != Z_OK) { fprintf(stderr, "zlib uncompress failed rc=%d\n", rc); free(buf); return NULL; }
    *out_len = (uint32_t)dl;
    return buf;
}

/* ---- minimal Shift-JIS -> UTF-8 using iconv-free table? Use iconv. ---- */
#include <iconv.h>
static char *sjis_to_utf8(const uint8_t *s, size_t n)
{
    iconv_t cd = iconv_open("UTF-8", "SHIFT_JIS");
    if (cd == (iconv_t)-1) return NULL;
    size_t inleft = n, outcap = n * 4 + 16, outleft = outcap;
    char *out = (char*)malloc(outcap);
    char *inp = (char*)s, *outp = out;
    while (inleft > 0) {
        size_t r = iconv(cd, &inp, &inleft, &outp, &outleft);
        if (r == (size_t)-1) {
            /* skip one offending byte, emit '?' */
            if (outleft < 1) break;
            *outp++ = '?'; outleft--;
            inp++; inleft--;
        }
    }
    *outp = 0;
    iconv_close(cd);
    return out;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <file.bin> [--dec out] [--body out] [--raw]\n", argv[0]); return 2; }
    const char *path = argv[1];
    const char *decOut = NULL, *bodyOut = NULL; int raw = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--dec") && i+1 < argc) decOut = argv[++i];
        else if (!strcmp(argv[i], "--body") && i+1 < argc) bodyOut = argv[++i];
        else if (!strcmp(argv[i], "--raw")) raw = 1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t*)malloc(len);
    if (fread(data, 1, len, f) != (size_t)len) { perror("read"); return 1; }
    fclose(f);

    static const uint8_t MAGIC[8] = {0x45,0x90,0xAF,0xF9,0xB7,0x47,0xF6,0x94};
    if (len < 8 || memcmp(data, MAGIC, 8) != 0)
        fprintf(stderr, "warning: magic mismatch (not an encrypted text-bin?)\n");

    size_t dlen = 0;
    uint8_t *dec = doctext_decrypt(data, len, &dlen);
    if (!dec) { fprintf(stderr, "decrypt failed\n"); return 1; }

    if (decOut) { FILE *o = fopen(decOut, "wb"); fwrite(dec, 1, dlen, o); fclose(o); }

    uint16_t lineCount = rd_u16(dec + 0x08);
    uint32_t bodyLen = 0;
    uint8_t *body = inflate_body(dec, dlen, &bodyLen);
    if (!body) { fprintf(stderr, "body inflate failed\n"); return 1; }
    if (bodyOut) { FILE *o = fopen(bodyOut, "wb"); fwrite(body, 1, bodyLen, o); fclose(o); }

    printf("# %s  lineCount=%u  compFlag=%u  bodyLen=%u\n", path, lineCount, dec[0x0B], bodyLen);

    for (uint32_t i = 0; i < lineCount; i++) {
        const uint8_t *e = body + (size_t)i * 12;
        if ((size_t)(e - body) + 12 > bodyLen) break;
        uint32_t id      = rd_u32(e + 0);
        uint32_t idPos   = rd_u32(e + 4);
        uint32_t linePos = rd_u32(e + 8);
        const char *lineId = (idPos   < bodyLen) ? (const char*)(body + idPos)   : "";
        const uint8_t *line = (linePos < bodyLen) ? (body + linePos) : (const uint8_t*)"";
        size_t slen = strlen((const char*)line);
        if (raw) {
            printf("[%u] id=%u idpos=%u linepos=%u idstr='%s' raw=", i, id, idPos, linePos, lineId);
            for (size_t k = 0; k < slen; k++) printf("%02X ", line[k]);
            printf("\n");
        } else {
            char *u = sjis_to_utf8(line, slen);
            printf("[%u] id=%u (%s): %s\n", i, id, lineId, u ? u : "");
            free(u);
        }
    }

    free(body); free(dec); free(data);
    return 0;
}
