/*
 * dirge_atdec — native decoder for Dirge of Cerberus (PS2) headerless audio.
 *
 *   .at2  = ATRAC3      (headerless, 1 ch, 44100 Hz, 0x60=96-byte frames, 1024 smp/frame)
 *   .at3  = ATRAC3plus  (headerless, 1 ch, 44100 Hz, 0x180=384-byte frames, 2048 smp/frame)
 *
 * Both are raw ATRAC3-family elementary streams with NO RIFF/OMA header. The game
 * streams them straight into the IOP ATRAC decoder; the per-stream parameters ARE
 * the recovered spec (see FORMAT.md). We reconstruct those params and feed the raw
 * frames one block at a time to libavcodec's atrac3 / atrac3plus decoder, then write
 * a standard 16-bit PCM WAV. We drive the packet boundaries ourselves (block_align),
 * so we don't rely on ffmpeg's OMA/RIFF demux (which mis-batches these streams).
 *
 * Build:  make           (needs libavcodec/libavutil/libswresample dev)
 * Usage:  dirge_atdec <in.at2|in.at3> <out.wav> [--rate N] [--ch N] [--block N] [--at3plus|--at3]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

typedef struct { int is_plus; int channels; int rate; int block; int frame_samples; } at_spec;

/* first 6 bits == 0x28 is the ATRAC3 "sound unit id"; ATRAC3plus frames start bit 0 */
static int sniff_is_plus(const uint8_t *b, size_t n) {
    if (n < 1) return -1;
    if ((b[0] >> 2) == 0x28) return 0;   /* ATRAC3 */
    if ((b[0] >> 7) == 0x00) return 1;   /* ATRAC3plus */
    return -1;
}

/* ATRAC3 frame size is bitrate-dependent (96/152/192 bytes/ch @ 44.1k mono in DoC).
 * Recover it: the smallest candidate that divides the file AND has the 0x28 sound-unit
 * sync at every frame boundary. (voice at2 = 96, title.at3 = 192). */
static int detect_atrac3_block(const uint8_t *b, size_t n) {
    static const int cand[3] = { 96, 152, 192 };
    for (int c = 0; c < 3; c++) {
        int blk = cand[c];
        if (n % blk) continue;
        size_t frames = n / blk, bad = 0;
        for (size_t i = 0; i < frames; i++) if ((b[i*blk] >> 2) != 0x28) { bad = 1; break; }
        if (!bad) return blk;
    }
    return 96; /* safe fallback */
}

/* 14-byte WAV-style extradata the libavcodec ATRAC3 decoder expects:
 * u16=1, u32 samples_per_channel, u16 coding_mode, u16 coding_mode_dup, u16 frame_factor, u16=0
 * coding_mode: 0 = (dual) stereo / mono, 1 = joint stereo.  frame_factor normally 1.
 * block_align must equal {96,152,192} * channels * frame_factor. */
static void make_atrac3_extradata(uint8_t out[14], int coding_mode) {
    uint16_t v;
    memset(out, 0, 14);
    v = 1;                 memcpy(out + 0, &v, 2);
    { uint32_t s = 0x0800; memcpy(out + 2, &s, 4); }
    v = coding_mode;       memcpy(out + 6, &v, 2);
    v = coding_mode;       memcpy(out + 8, &v, 2);
    v = 1;                 memcpy(out + 10, &v, 2); /* frame_factor */
    v = 0;                 memcpy(out + 12, &v, 2);
}

static void put32(FILE *f, uint32_t v){ fputc(v&0xff,f);fputc((v>>8)&0xff,f);fputc((v>>16)&0xff,f);fputc((v>>24)&0xff,f);}
static void put16(FILE *f, uint16_t v){ fputc(v&0xff,f);fputc((v>>8)&0xff,f);}

static void write_wav(const char *path, const int16_t *pcm, size_t nsamples, int ch, int rate) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror("open out"); exit(1); }
    uint32_t data_bytes = (uint32_t)(nsamples * 2);
    fwrite("RIFF",1,4,f); put32(f, 36 + data_bytes); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); put32(f,16); put16(f,1); put16(f,ch); put32(f,rate);
    put32(f,(uint32_t)rate*ch*2); put16(f,(uint16_t)(ch*2)); put16(f,16);
    fwrite("data",1,4,f); put32(f,data_bytes);
    fwrite(pcm,2,nsamples,f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr,"usage: %s <in> <out.wav> [--rate N --ch N --block N --at3plus|--at3]\n",argv[0]); return 2; }
    const char *in = argv[1], *out = argv[2];
    at_spec s = { .is_plus=-1, .channels=1, .rate=44100, .block=0, .frame_samples=0 };
    int force_plus = -1, coding_mode = 0;
    for (int i=3;i<argc;i++){
        if(!strcmp(argv[i],"--rate")&&i+1<argc) s.rate=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--ch")&&i+1<argc) s.channels=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--block")&&i+1<argc) s.block=(int)strtol(argv[++i],0,0);
        else if(!strcmp(argv[i],"--joint")) coding_mode=1;
        else if(!strcmp(argv[i],"--at3plus")) force_plus=1;
        else if(!strcmp(argv[i],"--at3")) force_plus=0;
    }

    FILE *fi = fopen(in,"rb");
    if(!fi){ perror("open in"); return 1; }
    fseek(fi,0,SEEK_END); long fsz=ftell(fi); fseek(fi,0,SEEK_SET);
    uint8_t *raw = malloc(fsz); if(fread(raw,1,fsz,fi)!=(size_t)fsz){fprintf(stderr,"read\n");return 1;} fclose(fi);

    s.is_plus = (force_plus>=0) ? force_plus : sniff_is_plus(raw, fsz);
    if (s.is_plus<0){ fprintf(stderr,"cannot identify ATRAC variant (first byte 0x%02x)\n",raw[0]); return 1; }
    if (!s.block) s.block = s.is_plus ? 0x180 : detect_atrac3_block(raw, fsz);
    s.frame_samples = s.is_plus ? 2048 : 1024;

    enum AVCodecID cid = s.is_plus ? AV_CODEC_ID_ATRAC3P : AV_CODEC_ID_ATRAC3;
    const AVCodec *codec = avcodec_find_decoder(cid);
    if(!codec){ fprintf(stderr,"no decoder for %s\n", s.is_plus?"atrac3plus":"atrac3"); return 1; }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->sample_rate = s.rate;
    ctx->channels = s.channels;
    ctx->channel_layout = (s.channels==1)?AV_CH_LAYOUT_MONO:AV_CH_LAYOUT_STEREO;
    ctx->block_align = s.block;
    uint8_t ed[14];
    if (!s.is_plus) {                 /* ATRAC3 needs the 14-byte extradata */
        make_atrac3_extradata(ed, coding_mode);
        ctx->extradata = av_mallocz(14 + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(ctx->extradata, ed, 14); ctx->extradata_size = 14;
    }
    if (avcodec_open2(ctx, codec, NULL) < 0){ fprintf(stderr,"avcodec_open2 failed\n"); return 1; }

    SwrContext *swr = swr_alloc_set_opts(NULL,
        ctx->channel_layout, AV_SAMPLE_FMT_S16, s.rate,
        ctx->channel_layout, ctx->sample_fmt ? ctx->sample_fmt : AV_SAMPLE_FMT_FLTP, s.rate, 0, NULL);
    /* sample_fmt not known until first frame; (re)configure lazily below */
    int swr_ready = 0;

    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();
    int16_t *pcm = NULL; size_t pcm_cap=0, pcm_len=0;

    long nframes = fsz / s.block;
    int decode_errors=0, frames_ok=0;
    for (long i=0;i<nframes;i++){
        pkt->data = raw + i*s.block;
        pkt->size = s.block;
        int r = avcodec_send_packet(ctx, pkt);
        if (r < 0){ decode_errors++; continue; }
        while ((r = avcodec_receive_frame(ctx, frm)) >= 0){
            if (!swr_ready){
                swr_free(&swr);
                swr = swr_alloc_set_opts(NULL, ctx->channel_layout, AV_SAMPLE_FMT_S16, s.rate,
                                         ctx->channel_layout, frm->format, frm->sample_rate?frm->sample_rate:s.rate, 0, NULL);
                swr_init(swr); swr_ready=1;
            }
            int nb = frm->nb_samples;
            int outcnt = nb; /* same rate */
            size_t need = (pcm_len + (size_t)outcnt*s.channels);
            if (need > pcm_cap){ pcm_cap = need*2+4096; pcm = realloc(pcm, pcm_cap*2); }
            uint8_t *op = (uint8_t*)(pcm + pcm_len);
            int got = swr_convert(swr, &op, outcnt, (const uint8_t**)frm->extended_data, nb);
            if (got>0){ pcm_len += (size_t)got*s.channels; frames_ok++; }
        }
    }
    /* flush */
    avcodec_send_packet(ctx, NULL);
    while (avcodec_receive_frame(ctx, frm) >= 0){
        if(!swr_ready) break;
        int nb=frm->nb_samples; size_t need=pcm_len+(size_t)nb*s.channels;
        if(need>pcm_cap){pcm_cap=need*2+4096;pcm=realloc(pcm,pcm_cap*2);}
        uint8_t*op=(uint8_t*)(pcm+pcm_len);
        int got=swr_convert(swr,&op,nb,(const uint8_t**)frm->extended_data,nb);
        if(got>0) pcm_len+=(size_t)got*s.channels;
    }

    write_wav(out, pcm, pcm_len, s.channels, s.rate);

    /* stats */
    long peak=0; double sq=0; size_t nz=0;
    for(size_t i=0;i<pcm_len;i++){ long v=pcm[i]; long a=v<0?-v:v; if(a>peak)peak=a; sq+=(double)v*v; if(v)nz++; }
    double rms = pcm_len? (double)/*sqrt via*/0:0;
    rms = pcm_len? __builtin_sqrt(sq/pcm_len):0;
    double dur = (double)(pcm_len/s.channels)/s.rate;
    fprintf(stderr,
      "[%s] variant=%s ch=%d rate=%d block=0x%x frames=%ld decoded_ok=%d errors=%d\n"
      "  -> %s : %zu samples, %.2fs, peak=%ld rms=%.0f nonzero=%.1f%%\n",
      in, s.is_plus?"ATRAC3plus":"ATRAC3", s.channels, s.rate, s.block, nframes, frames_ok, decode_errors,
      out, pcm_len/s.channels, dur, peak, rms, pcm_len?100.0*nz/pcm_len:0.0);

    free(pcm); free(raw);
    av_frame_free(&frm); av_packet_free(&pkt); swr_free(&swr); avcodec_free_context(&ctx);
    return 0;
}
