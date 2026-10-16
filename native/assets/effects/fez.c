/* fez.c — ".fez" inflation.  See fez.h for the format and the decompiled-
 * decompressor evidence.  Portable C11 + system zlib.  The input is treated as
 * untrusted: every access is bounds-checked and the output size is capped. */

#include "fez.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define FEZ_MAX_OUT (64u * 1024u * 1024u)   /* sanity cap on decompressed size */

static uint32_t rd_u32(const uint8_t *d, size_t o){
    return (uint32_t)d[o] | ((uint32_t)d[o+1]<<8)
         | ((uint32_t)d[o+2]<<16) | ((uint32_t)d[o+3]<<24);
}

/* DAT_0047eb50: SBOX_DEC[x] == (x + 0x78) & 0xff (verified against the ELF). */
static inline uint8_t sbox_dec(uint8_t x){ return (uint8_t)(x + 0x78u); }

/* FUN_0041bb38: build the 0x20-word u64 key schedule from the 8-byte seed. */
static void build_key(uint64_t seed, uint64_t key[32]){
    uint32_t uV4 = (uint32_t)seed;
    uint32_t uV6 = (uint32_t)(seed >> 32);
    uint32_t hi = (uV4 << 8) | (uV4 >> 24);
    uint32_t lo = (uV6 >> 16) | (uV6 << 16);
    uint64_t k0 = ((uint64_t)hi << 32) | lo;
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(k0 >> (8*i));   /* little-endian */
    b[0] = (uint8_t)(b[0] + 0x45u);
    for (int i = 1; i < 8; i++){
        uint8_t prev = b[i-1], orig = b[i];
        b[i] = (uint8_t)(((uint8_t)(prev + (uint8_t)(orig - 0x2cu)))
                         ^ (uint8_t)(prev << 2) ^ 0x45u);
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)b[i] << (8*i);
    key[0] = v;
    for (int i = 1; i < 32; i++) key[i] = key[i-1] * 5u;
}

/* FUN_0041c010 over 'len' bytes (multiple of 8): per-block FUN_0041bf78 (byte
 * substitution + chaining) then FUN_0041bf08 (64-bit mix). 'count' runs
 * continuously (bytes processed so far). */
static void decrypt(const uint8_t *src, uint8_t *dst, size_t len,
                    const uint64_t key[32]){
    uint8_t keyb[256];
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++)
            keyb[i*8+j] = (uint8_t)(key[i] >> (8*j));

    uint32_t count = 0;
    for (size_t pos = 0; pos + 8 <= len; pos += 8){
        /* FUN_0041bf78: byte stage src[pos..] -> tmp */
        uint8_t tmp[8];
        uint8_t uV7 = (uint8_t)((count >> 3) ^ 0x45u);
        uint32_t ki = ((count >> 3) & 0x1f) * 8u;
        for (int i = 0; i < 8; i++){
            uint8_t bb = src[pos+i];
            uint8_t v = (uint8_t)(uV7 ^ bb);
            for (int j = 0; j < 8; j++)
                v = (uint8_t)(sbox_dec(v) - keyb[ki+j]);
            tmp[i] = v;
            uV7 = bb;
        }
        /* FUN_0041bf08: 64-bit mix tmp -> dst[pos..] */
        uint64_t inq = 0;
        for (int i = 0; i < 8; i++) inq |= (uint64_t)tmp[i] << (8*i);
        uint64_t keyq = key[(count >> 3) & 0x1f];
        uint64_t u = count;   /* the decomp's uVar3 is 64-bit (shifts exceed 32) */
        uint64_t rot = (u | (u << 0x1e) | (u << 0x14) | (u << 0xa)) + 0xa1652347u;
        uint64_t x = (inq - keyq) ^ rot;
        x ^= keyq;
        x = (x << 32) | (x >> 32);
        for (int i = 0; i < 8; i++) dst[pos+i] = (uint8_t)(x >> (8*i));
        count += 8;
    }
}

int fez_inflate(const uint8_t *data, size_t size,
                uint8_t **out_buf, size_t *out_size,
                char *errbuf, size_t errlen){
    #define FAIL(m) do{ if(errbuf&&errlen) snprintf(errbuf,errlen,"%s",m); return -1; }while(0)
    *out_buf = NULL; *out_size = 0;
    if (size < 0x30) FAIL("fez too small");
    if (data[0x10] != 2) FAIL("fez: unexpected compression type (byte@0x10 != 2)");

    uint32_t dec_size  = rd_u32(data, 0x0c);     /* decompressed size      */
    uint32_t comp_size = rd_u32(data, 0x14);     /* compressed (file) size */
    if (comp_size < 0x30 || comp_size > size) FAIL("fez: bad compressed size");
    if (dec_size == 0 || dec_size > FEZ_MAX_OUT) FAIL("fez: implausible decompressed size");

    /* stream = file+0x28; decrypt (comp_size-0x20-8) bytes rounded down to /8. */
    size_t total  = (size_t)comp_size - 0x20;
    if (total < 8) FAIL("fez: stream too short");
    size_t declen = (total - 8) & ~(size_t)7;
    if (0x28u + declen > size) FAIL("fez: stream out of bounds");

    uint8_t *plain = (uint8_t*)malloc(declen ? declen : 1);
    if (!plain) FAIL("fez: out of memory (stream)");
    uint64_t key[32];
    build_key(7u, key);                           /* seed is the constant 7 */
    decrypt(data + 0x28, plain, declen, key);

    uint8_t *out = (uint8_t*)malloc(dec_size);
    if (!out){ free(plain); FAIL("fez: out of memory (output)"); }

    z_stream zs; memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK){ free(plain); free(out); FAIL("fez: inflateInit failed"); }
    zs.next_in = plain; zs.avail_in = (uInt)declen;
    zs.next_out = out;  zs.avail_out = (uInt)dec_size;
    int zr = inflate(&zs, Z_FINISH);
    size_t produced = zs.total_out;
    inflateEnd(&zs);
    free(plain);
    if (zr != Z_STREAM_END && produced != dec_size){
        free(out); FAIL("fez: zlib inflate failed");
    }
    *out_buf = out; *out_size = produced;
    return 0;
    #undef FAIL
}
