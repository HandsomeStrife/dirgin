/* dirge_sound.c - Dirge of Cerberus native sound-playback engine (see .h).
 *
 * Library only (no main). Build:  make dirge_sound   (or link dirge_sound.o).
 * Validation driver lives in dirge_sound_demo.c.
 *
 * Nothing here modifies or depends on the decoders: decoded PCM enters through
 * ds_sample_load_wav() (fed by dirge_atdec / dirge_wd WAV output). The mixer is
 * a portable stand-in for the IOP SPU2 voice mixer; the db.bin layer is the
 * game-visible request/resolution logic recovered from the sound database.
 */
#include "dirge_sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ======================================================================== *
 *  Small endian-safe readers
 * ======================================================================== */
static uint16_t rd16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1]<<8)); }
static uint32_t rd32(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

/* ======================================================================== *
 *  WAV loader (16-bit PCM, mono or stereo) - minimal RIFF chunk walk
 * ======================================================================== */
int ds_sample_load_wav(ds_sample_t *out, const char *path) {
    if (!out || !path) return 1;
    memset(out, 0, sizeof *out);
    out->loop_start = -1; out->loop_end = -1;

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "ds_sample_load_wav: cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsz < 44) { fclose(f); fprintf(stderr, "wav too small: %s\n", path); return 1; }
    uint8_t *d = (uint8_t*)malloc((size_t)fsz);
    if (!d || fread(d, 1, (size_t)fsz, f) != (size_t)fsz) { free(d); fclose(f); return 1; }
    fclose(f);

    if (memcmp(d, "RIFF", 4) || memcmp(d+8, "WAVE", 4)) { free(d); fprintf(stderr, "not a WAV: %s\n", path); return 1; }

    int channels = 0, rate = 0, bits = 0, fmt = 0;
    const uint8_t *data_ptr = NULL; size_t data_bytes = 0;

    size_t pos = 12;
    while (pos + 8 <= (size_t)fsz) {
        const uint8_t *ck = d + pos;
        uint32_t cksz = rd32(ck + 4);
        const uint8_t *body = ck + 8;
        if (pos + 8 + cksz > (size_t)fsz) cksz = (uint32_t)((size_t)fsz - pos - 8); /* clamp */
        if (!memcmp(ck, "fmt ", 4) && cksz >= 16) {
            fmt      = rd16(body + 0);
            channels = rd16(body + 2);
            rate     = (int)rd32(body + 4);
            bits     = rd16(body + 14);
        } else if (!memcmp(ck, "data", 4)) {
            data_ptr = body; data_bytes = cksz;
        }
        pos += 8 + cksz + (cksz & 1); /* chunks are word-aligned */
    }

    if (fmt != 1 || bits != 16 || channels < 1 || channels > 2 || !data_ptr) {
        fprintf(stderr, "unsupported WAV (fmt=%d bits=%d ch=%d) %s\n", fmt, bits, channels, path);
        free(d); return 1;
    }

    size_t nframes = data_bytes / (size_t)(2 * channels);
    int16_t *pcm = (int16_t*)malloc(nframes * (size_t)channels * sizeof(int16_t));
    if (!pcm) { free(d); return 1; }
    /* copy (source may be unaligned); little-endian host assumed, but read safely */
    for (size_t i = 0; i < nframes * (size_t)channels; i++)
        pcm[i] = (int16_t)rd16(data_ptr + i * 2);
    free(d);

    out->pcm = pcm; out->nframes = nframes; out->channels = channels;
    out->rate = rate ? rate : 44100;
    out->loop_start = -1; out->loop_end = -1; out->own_pcm = 1;
    return 0;
}

void ds_sample_free(ds_sample_t *s) {
    if (s && s->own_pcm && s->pcm) { free(s->pcm); s->pcm = NULL; s->own_pcm = 0; }
}

/* ======================================================================== *
 *  Mixer
 * ======================================================================== */
typedef struct {
    const ds_sample_t *smp;
    double pos;          /* play position in source frames (fractional)       */
    double step;         /* frames advanced per output frame (pitch * sr/or)  */
    float  gain_l, gain_r;
    float  vol;          /* stored so pan changes recompute gains             */
    float  pan;
    float  pitch;
    int    loop;
    int    active;
    uint16_t gen;        /* generation for handle validation                  */
} ds_voice;

struct ds_mixer {
    int out_rate;
    int out_channels;
    int nvoices;
    ds_voice v[DS_MAX_VOICES];
    uint16_t gen_counter;
};

static void voice_calc_gains(ds_voice *vo) {
    /* equal-power pan law: angle 0..pi/2 across pan -1..+1 */
    float pan = vo->pan; if (pan < -1.f) pan = -1.f; if (pan > 1.f) pan = 1.f;
    double a = (double)(pan + 1.0) * 0.25 * M_PI;
    vo->gain_l = vo->vol * (float)cos(a);
    vo->gain_r = vo->vol * (float)sin(a);
}

ds_mixer *sound_init(int out_rate, int out_channels, int nvoices) {
    if (out_rate < 8000 || out_rate > 192000) return NULL;
    if (out_channels < 1 || out_channels > 2) return NULL;
    if (nvoices < 1) nvoices = 1;
    if (nvoices > DS_MAX_VOICES) nvoices = DS_MAX_VOICES;
    ds_mixer *m = (ds_mixer*)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->out_rate = out_rate; m->out_channels = out_channels; m->nvoices = nvoices;
    m->gen_counter = 1;
    return m;
}

void sound_shutdown(ds_mixer *m) { free(m); }

/* pack/unpack handle = (slot << 16) | gen */
static int   make_handle(int slot, uint16_t gen){ return (int)(((uint32_t)slot << 16) | gen); }
static int   handle_slot(int h){ return (h >> 16) & 0xFFFF; }
static uint16_t handle_gen(int h){ return (uint16_t)(h & 0xFFFF); }

static ds_voice *resolve_voice(ds_mixer *m, int h) {
    if (!m || h < 0) return NULL;
    int slot = handle_slot(h);
    if (slot < 0 || slot >= m->nvoices) return NULL;
    ds_voice *vo = &m->v[slot];
    if (!vo->active || vo->gen != handle_gen(h)) return NULL;
    return vo;
}

int sound_play_ex(ds_mixer *m, const ds_sample_t *smp,
                  float vol, float pan, float pitch, int loop) {
    if (!m || !smp || !smp->pcm || smp->nframes == 0) return DS_INVALID_VOICE;
    if (smp->channels < 1 || smp->channels > 2) return DS_INVALID_VOICE;
    int slot = -1;
    for (int i = 0; i < m->nvoices; i++) if (!m->v[i].active) { slot = i; break; }
    if (slot < 0) return DS_INVALID_VOICE;  /* no free voice (caller may steal) */

    ds_voice *vo = &m->v[slot];
    memset(vo, 0, sizeof *vo);
    vo->smp = smp;
    vo->pos = 0.0;
    vo->pitch = (pitch > 0.f) ? pitch : 1.f;
    vo->step = (double)vo->pitch * (double)smp->rate / (double)m->out_rate;
    vo->vol = vol; vo->pan = pan;
    voice_calc_gains(vo);
    vo->loop = loop ? 1 : 0;
    vo->active = 1;
    vo->gen = m->gen_counter++;
    if (m->gen_counter == 0) m->gen_counter = 1;
    return make_handle(slot, vo->gen);
}

int sound_play(ds_mixer *m, const ds_sample_t *smp, float vol, float pan, int loop) {
    return sound_play_ex(m, smp, vol, pan, 1.0f, loop);
}

void sound_stop(ds_mixer *m, int voice) {
    ds_voice *vo = resolve_voice(m, voice);
    if (vo) vo->active = 0;
}

void sound_stop_all(ds_mixer *m) {
    if (!m) return;
    for (int i = 0; i < m->nvoices; i++) m->v[i].active = 0;
}

void sound_set_volume(ds_mixer *m, int voice, float vol) {
    ds_voice *vo = resolve_voice(m, voice);
    if (vo) { vo->vol = vol; voice_calc_gains(vo); }
}
void sound_set_pan(ds_mixer *m, int voice, float pan) {
    ds_voice *vo = resolve_voice(m, voice);
    if (vo) { vo->pan = pan; voice_calc_gains(vo); }
}
void sound_set_pitch(ds_mixer *m, int voice, float pitch) {
    ds_voice *vo = resolve_voice(m, voice);
    if (vo && pitch > 0.f) {
        vo->pitch = pitch;
        vo->step = (double)pitch * (double)vo->smp->rate / (double)m->out_rate;
    }
}

int sound_is_playing(ds_mixer *m, int voice) { return resolve_voice(m, voice) != NULL; }

int sound_active_count(ds_mixer *m) {
    if (!m) return 0;
    int n = 0;
    for (int i = 0; i < m->nvoices; i++) if (m->v[i].active) n++;
    return n;
}

static int32_t clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return v;
}

/* Fetch source frame `idx` from a sample as L/R floats (mono duplicated). */
static void fetch_frame(const ds_sample_t *s, long idx, float *l, float *r) {
    if (idx < 0) idx = 0;
    if ((size_t)idx >= s->nframes) { *l = *r = 0.f; return; }
    if (s->channels == 1) {
        float v = s->pcm[idx];
        *l = v; *r = v;
    } else {
        *l = s->pcm[idx*2];
        *r = s->pcm[idx*2 + 1];
    }
}

void sound_update(ds_mixer *m, int16_t *out, int nframes) {
    if (!m || !out || nframes <= 0) return;
    int oc = m->out_channels;
    memset(out, 0, (size_t)nframes * oc * sizeof(int16_t));

    for (int vi = 0; vi < m->nvoices; vi++) {
        ds_voice *vo = &m->v[vi];
        if (!vo->active) continue;
        const ds_sample_t *s = vo->smp;

        long lstart = s->loop_start >= 0 ? s->loop_start : 0;
        long lend   = s->loop_end   >= 0 ? s->loop_end   : (long)s->nframes;
        if (lend > (long)s->nframes) lend = (long)s->nframes;
        if (lstart < 0) lstart = 0;
        if (lstart >= lend) { lstart = 0; lend = (long)s->nframes; }

        for (int i = 0; i < nframes; i++) {
            /* end / loop handling */
            if (vo->pos >= (double)s->nframes) {
                if (vo->loop && lend > lstart) {
                    /* wrap into loop region */
                    double over = vo->pos - (double)lend;
                    if (over < 0) over = 0;
                    double seg = (double)(lend - lstart);
                    vo->pos = (double)lstart + fmod(over, seg);
                } else { vo->active = 0; break; }
            }
            if (vo->loop && vo->pos >= (double)lend) {
                double over = vo->pos - (double)lend;
                double seg  = (double)(lend - lstart);
                vo->pos = (double)lstart + fmod(over, seg);
            }

            /* linear interpolation between adjacent source frames */
            long i0 = (long)vo->pos;
            double frac = vo->pos - (double)i0;
            float l0,r0,l1,r1;
            fetch_frame(s, i0, &l0, &r0);
            long i1 = i0 + 1;
            if (vo->loop && i1 >= lend) i1 = lstart;
            fetch_frame(s, i1, &l1, &r1);
            float ls = l0 + (l1 - l0) * (float)frac;
            float rs = r0 + (r1 - r0) * (float)frac;

            if (oc == 1) {
                /* mono out: average the panned contributions */
                int32_t acc = out[i];
                acc += (int32_t)lrintf((ls * vo->gain_l + rs * vo->gain_r) * 0.5f);
                out[i] = (int16_t)clamp16(acc);
            } else {
                int32_t accL = out[i*2];
                int32_t accR = out[i*2 + 1];
                accL += (int32_t)lrintf(ls * vo->gain_l);
                accR += (int32_t)lrintf(rs * vo->gain_r);
                out[i*2]     = (int16_t)clamp16(accL);
                out[i*2 + 1] = (int16_t)clamp16(accR);
            }
            vo->pos += vo->step;
        }
    }
}

/* ======================================================================== *
 *  db.bin sound database
 * ======================================================================== */
#define DS_DB_HASH 16384   /* > 2x record count, power of two                 */

typedef struct ds_db_bucket { int rec; struct ds_db_bucket *next; } ds_db_bucket;

struct ds_db {
    ds_db_rec *recs;
    int        count;
    ds_db_bucket *buckets[DS_DB_HASH];
    ds_db_bucket *pool;       /* contiguous bucket storage                    */
};

static uint32_t name_hash(const char *s) {
    uint32_t h = 2166136261u;   /* FNV-1a */
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h & (DS_DB_HASH - 1);
}

ds_db *ds_db_load(const char *path) {
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "ds_db_load: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsz < 24 || fsz % 24 != 0) {
        fprintf(stderr, "ds_db_load: %s size %ld not a multiple of 24\n", path, fsz);
        fclose(f); return NULL;
    }
    uint8_t *d = (uint8_t*)malloc((size_t)fsz);
    if (!d || fread(d, 1, (size_t)fsz, f) != (size_t)fsz) { free(d); fclose(f); return NULL; }
    fclose(f);

    int count = (int)(fsz / 24);
    ds_db *db = (ds_db*)calloc(1, sizeof *db);
    if (!db) { free(d); return NULL; }
    db->recs = (ds_db_rec*)calloc((size_t)count, sizeof(ds_db_rec));
    db->pool = (ds_db_bucket*)calloc((size_t)count, sizeof(ds_db_bucket));
    if (!db->recs || !db->pool) { ds_db_free(db); free(d); return NULL; }
    db->count = count;

    for (int i = 0; i < count; i++) {
        const uint8_t *p = d + (size_t)i * 24;
        ds_db_rec *r = &db->recs[i];
        memcpy(r->name, p, 8); r->name[8] = 0;
        /* sanitise name to printable (defensive against junk records) */
        for (int k = 0; k < 8; k++) {
            unsigned char c = (unsigned char)r->name[k];
            if (c == 0) break;
            if (c < 0x20 || c > 0x7e) r->name[k] = 0;  /* truncate at junk      */
        }
        r->slot       = p[8];
        r->flags      = p[9];
        r->field      = rd16(p + 10);
        r->f12        = rd32(p + 12);
        r->loop_start = rd32(p + 16);
        r->length     = rd32(p + 20);

        if (r->name[0]) {
            uint32_t h = name_hash(r->name);
            ds_db_bucket *b = &db->pool[i];
            b->rec = i; b->next = db->buckets[h]; db->buckets[h] = b;
        }
    }
    free(d);
    return db;
}

void ds_db_free(ds_db *db) {
    if (!db) return;
    free(db->recs); free(db->pool); free(db);
}

int ds_db_count(const ds_db *db) { return db ? db->count : 0; }

const ds_db_rec *ds_db_at(const ds_db *db, int index) {
    if (!db || index < 0 || index >= db->count) return NULL;
    return &db->recs[index];
}

const ds_db_rec *ds_db_find(const ds_db *db, const char *name) {
    if (!db || !name) return NULL;
    uint32_t h = name_hash(name);
    for (ds_db_bucket *b = db->buckets[h]; b; b = b->next)
        if (!strcmp(db->recs[b->rec].name, name)) return &db->recs[b->rec];
    return NULL;
}

void ds_db_resolve(const ds_db *db, const ds_db_rec *rec, ds_resolved *out) {
    (void)db;
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->kind = DS_SRC_UNKNOWN;
    out->loop_start = -1; out->loop_end = -1;
    out->bank_group = -1; out->bank_index = -1;
    if (!rec) return;
    out->rec = rec;

    const char *n = rec->name;
    int is_bgm   = !strncmp(n, "bgm_", 4);
    int is_onv   = !strncmp(n, "on_v", 4);
    int is_ona   = !strncmp(n, "on_a", 4);
    int is_se    = !strncmp(n, "se_",  3) || !strncmp(n, "oe_", 3);

    if (rec->flags == 0xFF && rec->field == 0xFFFF) {
        /* streamed-by-id voice/subtitle line: f16 holds the stream id */
        out->kind = DS_SRC_STREAM_ID;
        out->stream_id = rec->loop_start;
        return;
    }

    if (is_bgm || is_onv || is_ona) {
        /* resident ATRAC3 stream in data/sound2/resat3/. BGM carries a loop
         * (loop_start..length in frames); voices are one-shot (loop_start 0). */
        out->kind = DS_SRC_STREAM_RES;
        /* BGM files are .at3 (ATRAC3plus) or .at2; voices are .at2. We cannot
         * know the extension from the record alone, so default by type and let
         * the caller probe: voices -> .at2, bgm -> .at3 (title.at3 excepted). */
        const char *ext = (is_onv || is_ona) ? "at2" : "at3";
        snprintf(out->stream_path, sizeof out->stream_path, "resat3/%s.%s", n, ext);
        if (is_bgm && rec->length > 0 && rec->loop_start < rec->length) {
            out->loop_start = (long)rec->loop_start;
            out->loop_end   = (long)rec->length;
        }
        return;
    }

    if (is_se) {
        /* SPU-ADPCM sample in a wave*.wd bank. `field` selects the bank/sub-bank
         * group; `slot` is the sample index within that group. (The exact
         * group->bank-file + slot->descriptor-index map is a documented open gap
         * in FORMAT.md section 5; we surface the DB's own indexing here.) */
        out->kind = DS_SRC_BANK;
        out->bank_group = rec->field;
        out->bank_index = rec->slot;
        /* SE one-shots; db length is a duration hint, not a loop. */
        return;
    }

    /* csj_/dsj_/.../numeric: treat as streamed-by-id if marked, else unknown. */
    if (rec->flags == 0xFF) { out->kind = DS_SRC_STREAM_ID; out->stream_id = rec->loop_start; }
}
