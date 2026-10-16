/* dirge_wd.c - Dirge of Cerberus (PS2, SLPS-20200) ".wd" SPU2 sound-bank unpacker
 *
 * Decodes the game's SPU2 sound-effect pool banks (data/sound2/wave*.wd) into
 * individual PCM samples. The sample codec is standard PS2 SPU-ADPCM ("VAG"):
 * 16-byte blocks of 1 shift/filter control byte + 1 flag byte + 14 data bytes
 * (28 four-bit nibbles), decoded with Sony's canonical 5-entry filter table.
 *
 * This is a PURIST native-port tool: it only READS the bank bytes, is fully
 * bounds-checked, and writes standard mono 16-bit PCM WAV files. It does not
 * modify dirge_atdec.c (the ATRAC3 streaming-audio decoder).
 *
 * Format (reverse-engineered; see FORMAT.md section 3 for the evidence):
 *
 *   +0x00  char[2]   "WD"
 *   +0x02  u16       bank id (matches the filename: 0, 1, 1000=0x3E8, 1001=0x3E9)
 *   +0x04  u32       sample-data region size (bytes after the descriptor table)
 *   +0x08  u32       entry count
 *   +0x0C  u32       entry count (duplicate)
 *   +0x10  u32[4]    reserved (zero)
 *   +0x20  u32[cnt]  offset table: file offset of each 32-byte descriptor
 *                    (contiguous, 0x20 stride)
 *   ...    cnt * 32-byte descriptors
 *   ...    sample-data region (SPU-ADPCM), starts at last_desc_off + 0x20
 *
 * Descriptor (32 bytes):
 *   +0x00  u32   reserved (0)
 *   +0x04  u32   sample start offset within the sample-data region
 *   +0x08  u32   sample length in bytes  (valid when != +0x04 field; see below)
 *   +0x0C  u32   SPU2 ADSR envelope word (constant 0x5FE800FF for active voices;
 *                0 for an empty / unused slot)
 *   +0x10  u32   per-sample value (unresolved; not needed to decode PCM)
 *   +0x14  f32   base pitch / tuning ratio (a few discrete per-bank values,
 *                e.g. 2.851, 3.0, 3.25, 3.992 - NOT a per-sample sample rate;
 *                final playback rate is set by the sequencer at runtime)
 *   +0x18  u32   reserved (0)
 *   +0x1C  u32   reserved (0)
 *
 * Active vs empty: a descriptor whose ADSR word (+0x0C) is zero AND whose
 * offset/length are zero is an UNUSED index slot and is skipped.
 *
 * Length: field +0x08 is the authoritative byte length for most samples (their
 * ADPCM body carries no end flag). Some descriptors store +0x08 == +0x04 (a
 * sentinel); for those the sample fills its slot (up to the next sample's start)
 * and terminates on a VAG end flag (flag byte 1 or 7). The decoder handles both:
 * it stops at min(declared length, next-sample start) and at any end-flag block.
 *
 * Usage:
 *   dirge_wd BANK.wd                       list the bank's samples
 *   dirge_wd BANK.wd -o OUTDIR [opts]      extract samples to WAV
 * Options:
 *   -o DIR        output directory for WAVs (required to extract)
 *   -i N          extract only sample index N
 *   -r HZ         WAV sample rate (default 44100; see pitch caveat above)
 *   -n NAME       base name for output files (default: input basename)
 *
 * Build:  cc -O2 -Wall -o dirge_wd dirge_wd.c     (or: make dirge_wd)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* ---- Canonical Sony SPU/VAG ADPCM filter coefficients (x64 fixed point) ----
 * Five predictor pairs, index 0..4. These are the well-documented values used
 * by every SPU-ADPCM / VAG decoder (ffmpeg adpcm_psx, vgmstream, jpsxdec...).
 */
static const int32_t vag_coef_pos[5] = {   0,  60, 115,  98, 122 };
static const int32_t vag_coef_neg[5] = {   0,   0, -52, -55, -60 };

static int32_t clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return v;
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Decode one SPU-ADPCM sample.
 *   region, region_len : the whole sample-data region (bounds)
 *   off                : byte offset of this sample within region
 *   max_bytes          : hard cap on bytes to read (declared length / slot)
 *   out                : receives a malloc'd int16 PCM buffer (caller frees)
 *   out_n              : receives the PCM sample count
 *   loop_start_blk     : receives block index of VAG loop-start flag, or -1
 *   loop_end_blk       : receives block index of VAG loop-end flag, or -1
 * Returns bytes actually consumed, or -1 on error.
 */
static long vag_decode(const uint8_t *region, size_t region_len,
                       size_t off, size_t max_bytes,
                       int16_t **out, size_t *out_n,
                       long *loop_start_blk, long *loop_end_blk) {
    if (off >= region_len) return -1;
    size_t avail = region_len - off;
    if (max_bytes > avail) max_bytes = avail;
    max_bytes &= ~(size_t)0xF;            /* whole 16-byte blocks only */
    size_t nblk = max_bytes / 16;
    if (nblk == 0) { *out = NULL; *out_n = 0; return 0; }

    int16_t *pcm = (int16_t *)malloc(nblk * 28 * sizeof(int16_t));
    if (!pcm) return -1;

    int32_t h1 = 0, h2 = 0;
    size_t n = 0;
    long lstart = -1, lend = -1;
    const uint8_t *b = region + off;
    size_t blk;
    for (blk = 0; blk < nblk; blk++, b += 16) {
        uint8_t ctrl = b[0];
        uint8_t flag = b[1];
        int shift  = ctrl & 0x0F;
        int filter = (ctrl >> 4) & 0x0F;
        if (shift > 12)  shift = 9;       /* SPU hardware behaviour */
        if (filter > 4)  filter = 0;      /* clamp to valid predictor set */

        /* VAG stream flags: 4/6 = loop start, 3 = loop end/repeat,
         * 1/7 = end of sample. */
        if (flag == 4 || flag == 6) { if (lstart < 0) lstart = (long)blk; }
        if (flag == 3)              { lend = (long)blk; }

        int k;
        for (k = 0; k < 14; k++) {
            uint8_t byte = b[2 + k];
            int nib;
            for (nib = 0; nib < 2; nib++) {
                int s = (nib == 0) ? (byte & 0x0F) : (byte >> 4);
                if (s > 7) s -= 16;                  /* sign-extend 4-bit */
                int32_t sample = (int32_t)(s << (12 - shift));
                sample += (h1 * vag_coef_pos[filter] +
                           h2 * vag_coef_neg[filter]) >> 6;
                sample = clamp16(sample);
                pcm[n++] = (int16_t)sample;
                h2 = h1; h1 = sample;
            }
        }
        if (flag == 1 || flag == 7) { blk++; break; }  /* end marker */
    }

    *out = pcm;
    *out_n = n;
    if (loop_start_blk) *loop_start_blk = lstart;
    if (loop_end_blk)   *loop_end_blk   = lend;
    return (long)(blk * 16);
}

static int write_wav(const char *path, const int16_t *pcm, size_t n, uint32_t rate) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot open %s for write\n", path); return -1; }
    uint32_t data_bytes = (uint32_t)(n * 2);
    uint32_t byte_rate = rate * 2;         /* mono, 16-bit */
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t riff = 36 + data_bytes;
    h[4]=riff; h[5]=riff>>8; h[6]=riff>>16; h[7]=riff>>24;
    memcpy(h+8, "WAVEfmt ", 8);
    h[16]=16; h[17]=0; h[18]=0; h[19]=0;   /* fmt chunk size */
    h[20]=1;  h[21]=0;                      /* PCM */
    h[22]=1;  h[23]=0;                      /* mono */
    h[24]=rate; h[25]=rate>>8; h[26]=rate>>16; h[27]=rate>>24;
    h[28]=byte_rate; h[29]=byte_rate>>8; h[30]=byte_rate>>16; h[31]=byte_rate>>24;
    h[32]=2; h[33]=0;                       /* block align */
    h[34]=16; h[35]=0;                      /* bits per sample */
    memcpy(h+36, "data", 4);
    h[40]=data_bytes; h[41]=data_bytes>>8; h[42]=data_bytes>>16; h[43]=data_bytes>>24;
    if (fwrite(h, 1, 44, f) != 44 ||
        fwrite(pcm, 1, data_bytes, f) != data_bytes) {
        fprintf(stderr, "write error on %s\n", path); fclose(f); return -1;
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char *in_path = NULL, *out_dir = NULL, *base_name = NULL;
    long only_index = -1;
    uint32_t rate = 44100;

    int i;
    for (i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-o") && i+1 < argc) out_dir   = argv[++i];
        else if (!strcmp(argv[i], "-i") && i+1 < argc) only_index = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-r") && i+1 < argc) rate       = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-n") && i+1 < argc) base_name = argv[++i];
        else if (argv[i][0] != '-') in_path = argv[i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!in_path) {
        fprintf(stderr, "usage: %s BANK.wd [-o OUTDIR] [-i N] [-r HZ] [-n NAME]\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(in_path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", in_path); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz < 0x24) { fprintf(stderr, "file too small\n"); fclose(f); return 1; }
    uint8_t *d = (uint8_t *)malloc((size_t)fsz);
    if (!d || fread(d, 1, (size_t)fsz, f) != (size_t)fsz) {
        fprintf(stderr, "read error\n"); free(d); fclose(f); return 1;
    }
    fclose(f);

    if (d[0] != 'W' || d[1] != 'D') {
        fprintf(stderr, "bad magic (not a .wd bank)\n"); free(d); return 1;
    }
    uint32_t bank_id  = (uint32_t)d[2] | ((uint32_t)d[3] << 8);
    uint32_t region_sz = rd_u32(d + 4);
    uint32_t count     = rd_u32(d + 8);
    uint32_t count2    = rd_u32(d + 12);
    if (count == 0 || count > 65535) {
        fprintf(stderr, "implausible entry count %u\n", count); free(d); return 1;
    }
    /* offset table bounds */
    if ((size_t)0x20 + (size_t)count * 4 > (size_t)fsz) {
        fprintf(stderr, "offset table out of bounds\n"); free(d); return 1;
    }

    /* sample-data region begins right after the last (highest-offset)
     * descriptor. Derive it from the offset table rather than the size field
     * (the size field carries trailing padding in some banks). */
    uint32_t max_off = 0;
    for (i = 0; i < (int)count; i++) {
        uint32_t o = rd_u32(d + 0x20 + i * 4);
        if (o > max_off) max_off = o;
    }
    size_t base = (size_t)max_off + 0x20;
    if (base > (size_t)fsz) { fprintf(stderr, "descriptor region out of bounds\n"); free(d); return 1; }
    const uint8_t *region = d + base;
    size_t region_len = (size_t)fsz - base;

    /* collect active sample start offsets (for slot/length derivation) */
    uint32_t *starts = (uint32_t *)malloc(count * sizeof(uint32_t));
    size_t nstarts = 0;

    printf("bank: %s\n", in_path);
    printf("  magic WD, bank id = %u, region size field = 0x%x, entry count = %u (dup %u)\n",
           bank_id, region_sz, count, count2);
    printf("  descriptor table 0x20..0x%zx, sample region 0x%zx (len 0x%zx)\n",
           base, base, region_len);

    /* first pass: gather active starts */
    for (i = 0; i < (int)count; i++) {
        uint32_t desc_off = rd_u32(d + 0x20 + i * 4);
        if ((size_t)desc_off + 0x20 > (size_t)fsz) continue;
        const uint8_t *dsc = d + desc_off;
        uint32_t off  = rd_u32(dsc + 4);
        uint32_t len  = rd_u32(dsc + 8);
        uint32_t adsr = rd_u32(dsc + 12);
        if (adsr == 0 && off == 0 && len == 0) continue;  /* empty slot */
        starts[nstarts++] = off;
    }

    if (!out_dir) {
        printf("  active samples (idx  off      bytes    blks   pcm     loop        pitch   adsr):\n");
    }

    int active = 0, extracted = 0;
    for (i = 0; i < (int)count; i++) {
        uint32_t desc_off = rd_u32(d + 0x20 + i * 4);
        if ((size_t)desc_off + 0x20 > (size_t)fsz) continue;
        const uint8_t *dsc = d + desc_off;
        uint32_t off  = rd_u32(dsc + 4);
        uint32_t flen = rd_u32(dsc + 8);
        uint32_t adsr = rd_u32(dsc + 12);
        uint32_t pf   = rd_u32(dsc + 20);
        float pitch; memcpy(&pitch, &pf, 4);

        if (adsr == 0 && off == 0 && flen == 0) continue;  /* empty slot */
        active++;
        if (off > region_len) continue;

        /* slot end = next active start strictly greater than off */
        size_t slot_end = region_len;
        for (size_t s = 0; s < nstarts; s++)
            if (starts[s] > off && starts[s] < slot_end) slot_end = starts[s];
        size_t slot = slot_end - off;

        /* length: field +0x08 when it is a sane in-slot block length and not
         * the ==offset sentinel; otherwise the whole slot (end-flag terminated) */
        size_t length;
        if (flen != off && (flen & 0xF) == 0 && flen >= 16 && off + flen <= region_len)
            length = flen;
        else
            length = slot;

        int16_t *pcm = NULL; size_t n = 0; long ls = -1, le = -1;
        long used = vag_decode(region, region_len, off, length, &pcm, &n, &ls, &le);
        if (used < 0) { fprintf(stderr, "  [%d] decode error\n", i); free(pcm); continue; }

        /* stats */
        int64_t peak = 0; double sumsq = 0.0;
        for (size_t j = 0; j < n; j++) {
            int32_t v = pcm[j]; int32_t a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            sumsq += (double)v * (double)v;
        }
        double rms = n ? sqrt(sumsq / (double)n) : 0.0;

        if (!out_dir) {
            char loopbuf[48];
            if (ls >= 0 || le >= 0) snprintf(loopbuf, sizeof loopbuf, "s=%ld e=%ld", ls, le);
            else                    snprintf(loopbuf, sizeof loopbuf, "-");
            printf("  %4d  %08x %08zx %5zu %7zu  %-11s %.3f  %08x  (peak=%lld rms=%.0f)\n",
                   i, off, used > 0 ? (size_t)used : length, n/28, n, loopbuf,
                   pitch, adsr, (long long)peak, rms);
        } else if (only_index < 0 || only_index == i) {
            const char *bn = base_name;
            char bnbuf[256];
            if (!bn) {
                const char *slash = strrchr(in_path, '/');
                const char *stem = slash ? slash + 1 : in_path;
                snprintf(bnbuf, sizeof bnbuf, "%s", stem);
                char *dot = strrchr(bnbuf, '.'); if (dot) *dot = 0;
                bn = bnbuf;
            }
            char path[512];
            snprintf(path, sizeof path, "%s/%s_%03d.wav", out_dir, bn, i);
            if (write_wav(path, pcm, n, rate) == 0) {
                printf("  wrote %s  (%zu samples, %.2fs @ %uHz, peak=%lld rms=%.0f)\n",
                       path, n, (double)n / rate, rate, (long long)peak, rms);
                extracted++;
            }
        }
        free(pcm);
    }

    printf("  %d active sample(s)%s\n", active,
           out_dir ? "" : " listed");
    if (out_dir) printf("  %d WAV file(s) written to %s\n", extracted, out_dir);

    free(starts);
    free(d);
    return 0;
}
