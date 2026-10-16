/* dirge_sound_demo.c - validation driver for dirge_sound.{h,c}
 *
 * Proves the playback engine end to end, with REAL decoded game audio:
 *
 *   1. Loads db.bin and resolves a handful of sound ids/names to their concrete
 *      source (bank+sample / resident stream + loop / streamed-by-id) - showing
 *      the game-visible KerberosSound name->sound resolution works.
 *   2. Decodes real samples with the EXISTING decoders (read-only): a voice
 *      stream via dirge_atdec (ATRAC3 .at2) and SE samples via dirge_wd
 *      (SPU-ADPCM .wd bank), to temporary WAVs.
 *   3. Plays them on separate mixer voices with distinct volume/pan (one looped),
 *      mixes ~5 s to a stereo 16-bit WAV, and reports peak/RMS per channel.
 *
 * It shells out to the already-built ./dirge_atdec and ./dirge_wd binaries, so
 * it does not recompile or alter any decoder. All decode output is genuine game
 * audio; the mix is a genuine sum of those decoded voices.
 *
 * Usage:
 *   dirge_sound_demo [ASSET_DIR] [DECODER_DIR] [OUT_WAV]
 * Defaults:
 *   ASSET_DIR   = /srv/games/dirge-port/extract/_KEL/data/sound2
 *   DECODER_DIR = .  (where dirge_atdec / dirge_wd live)
 *   OUT_WAV     = /srv/shared/dirge-port/sound_mix_sample.wav
 */
#include "dirge_sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *kindname(ds_src_kind k) {
    switch (k) {
        case DS_SRC_STREAM_RES: return "STREAM_RES (resat3 ATRAC3)";
        case DS_SRC_STREAM_ID:  return "STREAM_ID  (streamed voice line)";
        case DS_SRC_BANK:       return "BANK       (wave*.wd SPU-ADPCM)";
        default:                return "UNKNOWN";
    }
}

static void show_resolve(ds_db *db, const char *name) {
    const ds_db_rec *r = ds_db_find(db, name);
    if (!r) { printf("  %-10s : NOT FOUND in db.bin\n", name); return; }
    ds_resolved res;
    ds_db_resolve(db, r, &res);
    printf("  %-10s : slot=%3u flags=0x%02x field=0x%04x f12=%-3u loop_start=%-6u length=%-6u\n",
           r->name, r->slot, r->flags, r->field, r->f12, r->loop_start, r->length);
    printf("               -> %s", kindname(res.kind));
    if (res.kind == DS_SRC_STREAM_RES) {
        printf("  path=%s", res.stream_path);
        if (res.loop_start >= 0) printf("  loop=[%ld..%ld] frames", res.loop_start, res.loop_end);
        else printf("  one-shot");
    } else if (res.kind == DS_SRC_BANK) {
        printf("  bank_group=0x%04x  sample_index=%d", res.bank_group, res.bank_index);
    } else if (res.kind == DS_SRC_STREAM_ID) {
        printf("  stream_id=%u", res.stream_id);
    }
    printf("\n");
}

static int run(const char *cmd) {
    fprintf(stderr, "  $ %s\n", cmd);
    int rc = system(cmd);
    if (rc != 0) fprintf(stderr, "  (command returned %d)\n", rc);
    return rc;
}

/* peak/RMS per channel over an interleaved stereo buffer */
static void analyse(const int16_t *pcm, size_t nframes, const char *label) {
    long pL = 0, pR = 0; double sqL = 0, sqR = 0;
    for (size_t i = 0; i < nframes; i++) {
        long l = pcm[i*2], r = pcm[i*2+1];
        long al = l<0?-l:l, ar = r<0?-r:r;
        if (al > pL) pL = al;
        if (ar > pR) pR = ar;
        sqL += (double)l*l; sqR += (double)r*r;
    }
    double rmsL = nframes ? sqrt(sqL/nframes) : 0;
    double rmsR = nframes ? sqrt(sqR/nframes) : 0;
    printf("  %-18s L: peak=%5ld (%.1f dBFS) rms=%6.0f   R: peak=%5ld (%.1f dBFS) rms=%6.0f\n",
           label,
           pL, pL>0?20*log10(pL/32767.0):-99.0, rmsL,
           pR, pR>0?20*log10(pR/32767.0):-99.0, rmsR);
}

static void put32(FILE *f, uint32_t v){fputc(v&0xff,f);fputc((v>>8)&0xff,f);fputc((v>>16)&0xff,f);fputc((v>>24)&0xff,f);}
static void put16(FILE *f, uint16_t v){fputc(v&0xff,f);fputc((v>>8)&0xff,f);}
static int write_wav_stereo(const char *path, const int16_t *pcm, size_t nframes, int rate) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    uint32_t data = (uint32_t)(nframes * 2 * 2);
    fwrite("RIFF",1,4,f); put32(f, 36+data); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); put32(f,16); put16(f,1); put16(f,2); put32(f,rate);
    put32(f,(uint32_t)rate*2*2); put16(f,4); put16(f,16);
    fwrite("data",1,4,f); put32(f,data);
    fwrite(pcm,2,nframes*2,f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char *asset = (argc>1)?argv[1]:"/srv/games/dirge-port/extract/_KEL/data/sound2";
    const char *decdir = (argc>2)?argv[2]:".";
    const char *outwav = (argc>3)?argv[3]:"/srv/shared/dirge-port/sound_mix_sample.wav";
    const char *tmp = getenv("DS_TMP"); if (!tmp) tmp = "/tmp";

    char dbpath[512];
    snprintf(dbpath, sizeof dbpath, "%s/db.bin", asset);

    printf("=== dirge_sound validation ===\n\n");

    /* ---- 1. db.bin load + id/name resolution --------------------------- */
    ds_db *db = ds_db_load(dbpath);
    if (!db) { fprintf(stderr, "failed to load %s\n", dbpath); return 1; }
    printf("[1] db.bin loaded: %d records (24 bytes each)\n", ds_db_count(db));
    printf("    id/name -> sound resolution:\n");
    show_resolve(db, "bgm_070");   /* looping BGM (ATRAC3plus stream)      */
    show_resolve(db, "bgm_000");   /* looping BGM                          */
    show_resolve(db, "on_v132");   /* voice (ATRAC3 .at2, one-shot)        */
    show_resolve(db, "on_v001");   /* voice                                */
    show_resolve(db, "se_0014");   /* sound effect (wave*.wd bank)         */
    show_resolve(db, "se_0000");   /* sound effect                         */
    show_resolve(db, "csj_001");   /* streamed-by-id voice line            */
    show_resolve(db, "bt_v000");   /* battle voice (streamed-by-id)        */

    /* ---- 2. decode real samples via the existing decoders -------------- */
    printf("\n[2] decoding real samples with existing decoders (read-only):\n");
    char voice_wav[512], se14_wav[512], se15_wav[512], cmd[1600];

    snprintf(voice_wav, sizeof voice_wav, "%s/_ds_voice.wav", tmp);
    snprintf(cmd, sizeof cmd, "%s/dirge_atdec %s/resat3/on_v132.at2 %s 2>/dev/null",
             decdir, asset, voice_wav);
    run(cmd);

    /* two SE samples from wave0000.wd (idx 14 and 15: known coherent, FORMAT.md) */
    snprintf(cmd, sizeof cmd, "%s/dirge_wd %s/wave0000.wd -o %s -i 14 -n _ds_se >/dev/null 2>&1",
             decdir, asset, tmp);
    run(cmd);
    snprintf(se14_wav, sizeof se14_wav, "%s/_ds_se_014.wav", tmp);
    snprintf(cmd, sizeof cmd, "%s/dirge_wd %s/wave0000.wd -o %s -i 15 -n _ds_se >/dev/null 2>&1",
             decdir, asset, tmp);
    run(cmd);
    snprintf(se15_wav, sizeof se15_wav, "%s/_ds_se_015.wav", tmp);

    ds_sample_t voice = {0}, se14 = {0}, se15 = {0};
    int ok = 1;
    if (ds_sample_load_wav(&voice, voice_wav)) { fprintf(stderr, "no voice sample\n"); ok = 0; }
    if (ds_sample_load_wav(&se14,  se14_wav))  { fprintf(stderr, "no se14 sample\n");  ok = 0; }
    if (ds_sample_load_wav(&se15,  se15_wav))  { fprintf(stderr, "no se15 sample\n");  ok = 0; }
    if (!ok) { ds_db_free(db); return 1; }
    printf("    voice on_v132: %zu frames @ %dHz (%.2fs)\n", voice.nframes, voice.rate, (double)voice.nframes/voice.rate);
    printf("    se   14      : %zu frames @ %dHz (%.2fs)\n", se14.nframes, se14.rate, (double)se14.nframes/se14.rate);
    printf("    se   15      : %zu frames @ %dHz (%.2fs)\n", se15.nframes, se15.rate, (double)se15.nframes/se15.rate);

    /* ---- 3. mix a 5-second scene -------------------------------------- */
    printf("\n[3] mixing a 5.0s stereo scene @ 44100 Hz:\n");
    int RATE = 44100;
    ds_mixer *mix = sound_init(RATE, 2, 16);
    if (!mix) { fprintf(stderr, "sound_init failed\n"); ds_db_free(db); return 1; }

    /* Scene:
     *  - voice (on_v132) panned slightly LEFT, full volume, one-shot
     *  - SE 14 looped, panned RIGHT, moderate volume (loop proves loop path)
     *  - SE 15 one-shot, centre, fired twice at 1.0s and 3.0s
     * Loop the SE over its whole decoded length (its VAG stream is a one-shot;
     * we loop it here to exercise the loop engine with real audio). */
    se14.loop_start = 0; se14.loop_end = (long)se14.nframes;

    int total_frames = RATE * 5;
    int16_t *outbuf = (int16_t*)calloc((size_t)total_frames * 2, sizeof(int16_t));
    if (!outbuf) { fprintf(stderr, "oom\n"); return 1; }

    const int BLK = 1024;
    int vVoice = DS_INVALID_VOICE, vSe14 = DS_INVALID_VOICE;
    int fired15a = 0, fired15b = 0;
    /* Gains chosen to leave headroom: three overlapping sources that already
     * peak near full-scale individually must sum below clipping. */
    const float VOL_VOICE = 0.70f, VOL_SE14 = 0.45f, VOL_SE15 = 0.55f;
    printf("    voice start @0.0s panL vol%.2f | SE14 loop @0.0s panR vol%.2f | SE15 one-shot @1.0s & @3.0s centre vol%.2f\n",
           VOL_VOICE, VOL_SE14, VOL_SE15);

    for (int pos = 0; pos < total_frames; pos += BLK) {
        int n = BLK; if (pos + n > total_frames) n = total_frames - pos;
        double t = (double)pos / RATE;

        if (pos == 0) {
            vVoice = sound_play(mix, &voice, VOL_VOICE, -0.4f, 0);
            vSe14  = sound_play(mix, &se14,  VOL_SE14,   0.5f, 1);   /* looped */
        }
        if (!fired15a && t >= 1.0) { sound_play(mix, &se15, VOL_SE15, 0.0f, 0); fired15a = 1; }
        if (!fired15b && t >= 3.0) { sound_play(mix, &se15, VOL_SE15, 0.0f, 0); fired15b = 1; }
        /* fade the looped SE down over the last second to show set_volume */
        if (t >= 4.0) sound_set_volume(mix, vSe14, VOL_SE14 * (float)(5.0 - t));

        sound_update(mix, outbuf + (size_t)pos*2, n);
    }
    (void)vVoice;

    printf("    active voices at end: %d\n", sound_active_count(mix));

    /* ---- 4. report + write -------------------------------------------- */
    printf("\n[4] mix analysis (per channel):\n");
    analyse(outbuf, total_frames, "full 5s mix");
    analyse(outbuf, RATE/2, "first 0.5s");          /* voice+SE present      */
    analyse(outbuf + (size_t)(RATE*1)*2, RATE/2, "around 1.0s (SE15 hit)");

    if (write_wav_stereo(outwav, outbuf, total_frames, RATE) == 0)
        printf("\n    wrote %s  (%d frames, 5.00s, stereo 16-bit @ %dHz)\n", outwav, total_frames, RATE);

    free(outbuf);
    sound_shutdown(mix);
    ds_sample_free(&voice); ds_sample_free(&se14); ds_sample_free(&se15);
    ds_db_free(db);
    printf("\n=== done ===\n");
    return 0;
}
