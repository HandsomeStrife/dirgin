/* fez.h — inflation of ".fez" files (LZ/zlib-compressed, lightly encrypted FED).
 *
 * A .fez is a FED whose body is (1) DEFLATE-compressed with a standard zlib
 * wrapper, then (2) run through a fixed-key 8-byte-block stream cipher, and
 * finally (3) wrapped in the FED container header (magic "FED\x08", flags bit
 * 0x8000 set, byte@0x10 == 2).  To read one we reverse the pipeline: decrypt
 * the stream at file+0x28, then zlib-inflate it.
 *
 * Reversed from the game's own decompressor:
 *   FUN_002df450  gates on byte@0x10==2 and calls the codec with src=fed+0x20,
 *                 len = compSize(@0x14) - 0x20.
 *   FUN_0023c418  inits the cipher seed (FUN_0023c3f0 -> constant 7), decrypts
 *                 the stream in place (FUN_0041c1d8 -> FUN_0041c0d8), then runs
 *                 a standard zlib inflate (FUN_0042b810: the classic inflate
 *                 state machine — "unknown compression method", method 8, the
 *                 CMF/FLG %31 header check, "incorrect data check", etc.).
 *   FUN_0041bb38  builds the 0x20-entry u64 key schedule from the 8-byte seed
 *                 (key[0] from the seed with a byte-diffusion pass; key[i] =
 *                 key[i-1] * 5).
 *   FUN_0041bf78/FUN_0041bf08  the per-block decrypt rounds (byte substitution
 *                 via DAT_0047eb50[x] = (x + 0x78) & 0xff, then a 64-bit mix
 *                 with the block's key word and a counter-derived constant).
 *
 * Validated: both corpus .fez files (r0002, r0002_on) inflate to exactly the
 * size stored at header @0x0c and begin with "FED\x08".
 */
#ifndef FEZ_H
#define FEZ_H

#include <stddef.h>
#include <stdint.h>

/* Inflate a .fez buffer.  On success returns a malloc'd buffer of *out_size
 * bytes (a plain FED image) and returns 0; the caller must free() it.
 * Returns negative on error and writes a message into errbuf. */
int fez_inflate(const uint8_t *data, size_t size,
                uint8_t **out_buf, size_t *out_size,
                char *errbuf, size_t errlen);

#endif /* FEZ_H */
