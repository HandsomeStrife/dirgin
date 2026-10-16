/* dirge_sound.h - Dirge of Cerberus (PS2, SLPS-20200) native sound-playback engine
 *
 * This is the PLAYBACK / MIXING layer of the purist native port. The decoders
 * are done elsewhere and are used READ-ONLY by this module:
 *   - dirge_atdec.c : ATRAC3 / ATRAC3plus voice/SE/BGM streams (.at2/.at3) -> PCM
 *   - dirge_wd.c    : SPU2 SPU-ADPCM / VAG sound-effect banks (wave*.wd)   -> PCM
 *
 * What this module provides:
 *   1. ds_db_*   - the game-visible SOUND-REQUEST database. Loads db.bin (the
 *                  7681 x 24-byte master table) and resolves a sound id/name to
 *                  a concrete source: a resident ATRAC3 stream file, a .wd bank
 *                  sample, or a streamed-by-id voice line, plus loop points.
 *                  This backs the KerberosSound.* native script methods.
 *   2. ds_mix_*  - a portable software mixer: N voices, each playing a decoded
 *                  PCM sample at a given volume / pan / pitch, looped or one-shot,
 *                  summed to a stereo 16-bit PCM output block.
 *
 * On the real PS2 the SPU2 voice mixing runs on the IOP (Sony libsd / sceSd*),
 * not in the EE program; the EE decompiled corpus contains the request/dispatch
 * side only (name -> db.bin record -> SIF request), with no SPU2 registers or
 * mixing code. This module therefore re-authors the game-VISIBLE request logic
 * faithfully and supplies a portable software SPU2-equivalent mixer in its place.
 *
 * Pure C11, no external deps (libm only). All inputs are bounds-checked.
 */
#ifndef DIRGE_SOUND_H
#define DIRGE_SOUND_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== *
 *  Decoded PCM sample (what a voice plays)
 * ======================================================================== */

/* A decoded, ready-to-mix sample. The mixer does not own `pcm` unless the
 * sample was produced by a ds_sample_* loader that says so (own_pcm). */
typedef struct {
    int16_t *pcm;         /* interleaved PCM, `channels` channels              */
    size_t   nframes;     /* number of sample frames (pcm has nframes*channels)*/
    int      channels;    /* 1 (mono) or 2 (stereo)                            */
    int      rate;        /* source sample rate in Hz (e.g. 44100)             */
    long     loop_start;  /* loop start in frames, or -1 for one-shot          */
    long     loop_end;    /* loop end (exclusive) in frames, or -1 = nframes   */
    int      own_pcm;     /* 1 if ds_sample_free() should free(pcm)            */
} ds_sample_t;

/* Load a 16-bit PCM WAV (mono or stereo) as a sample. This is how decoded
 * output from dirge_atdec / dirge_wd is brought into the mixer. loop_start /
 * loop_end default to -1 (one-shot); set them afterwards for looped playback.
 * Returns 0 on success, non-zero on error. */
int  ds_sample_load_wav(ds_sample_t *out, const char *path);

/* Free a sample's PCM if it owns it. Safe to call on a zeroed sample. */
void ds_sample_free(ds_sample_t *s);

/* ======================================================================== *
 *  Software mixer  (the SPU2-equivalent voice/mix engine)
 * ======================================================================== */

#define DS_MAX_VOICES   48      /* the PS2 SPU2 has 48 hardware voices        */
#define DS_INVALID_VOICE (-1)

typedef struct ds_mixer ds_mixer;

/* sound_init: create a mixer that outputs `out_channels` (1 or 2) at out_rate
 * Hz, with `nvoices` (1..DS_MAX_VOICES) polyphony. Returns NULL on error. */
ds_mixer *sound_init(int out_rate, int out_channels, int nvoices);

/* sound_shutdown: destroy a mixer (does not free the ds_sample_t's you played). */
void sound_shutdown(ds_mixer *m);

/* sound_play: start `smp` on a free voice.
 *   vol   : 0.0 .. 1.0 (linear gain; values >1 allowed but may clip)
 *   pan   : -1.0 (hard left) .. 0.0 (centre) .. +1.0 (hard right); equal-power
 *   loop  : 0 one-shot, 1 loop using the sample's loop_start/loop_end
 * Returns an opaque voice handle (>=0), or DS_INVALID_VOICE if none free.
 * The handle encodes a generation so a stopped/recycled voice can't be poked
 * by a stale handle. `smp` must outlive the playback. */
int  sound_play(ds_mixer *m, const ds_sample_t *smp, float vol, float pan, int loop);

/* As sound_play, but also set an initial pitch ratio (1.0 = source rate). */
int  sound_play_ex(ds_mixer *m, const ds_sample_t *smp,
                   float vol, float pan, float pitch, int loop);

/* sound_stop: stop a playing voice. Unknown/stale handles are ignored. */
void sound_stop(ds_mixer *m, int voice);

/* sound_stop_all: stop every voice. */
void sound_stop_all(ds_mixer *m);

/* Per-voice parameter updates (ignored for stale handles). */
void sound_set_volume(ds_mixer *m, int voice, float vol);
void sound_set_pan(ds_mixer *m, int voice, float pan);
void sound_set_pitch(ds_mixer *m, int voice, float pitch);

/* Is this voice still producing audio? */
int  sound_is_playing(ds_mixer *m, int voice);

/* Number of currently active voices. */
int  sound_active_count(ds_mixer *m);

/* sound_update: mix `nframes` output frames into `out` (interleaved, 16-bit,
 * out_channels per frame). `out` must hold nframes*out_channels int16 samples.
 * The accumulator is int32 and clamped to int16, so simultaneous voices never
 * wrap/garble; they can still clip if their summed amplitude exceeds full scale
 * (the mixer reports nothing - the caller/headroom policy decides gain). */
void sound_update(ds_mixer *m, int16_t *out, int nframes);

/* ======================================================================== *
 *  Sound database  (db.bin)  -  the game-visible request resolution
 * ======================================================================== */

/* One 24-byte db.bin record, decoded (see FORMAT.md section 2). */
typedef struct {
    char     name[9];     /* NUL-terminated 8-char id, e.g. "on_v132"         */
    uint8_t  slot;        /* +0x08 slot / sub-index within its table group    */
    uint8_t  flags;       /* +0x09 0xFE streamed-resident, 0x00 resident,
                           *       0xFF streamed-by-id, else SE bank group     */
    uint16_t field;       /* +0x0A per-group constant (bank/group selector)   */
    uint32_t f12;         /* +0x0C type/profile id (66 voice, 148/212 bgm, 2 SE)*/
    uint32_t loop_start;  /* +0x10 loop start in frames (0 = none, voice = 0)  */
    uint32_t length;      /* +0x14 length / loop end in frames (BGM) or
                           *       duration hint (SE)                          */
} ds_db_rec;

/* How a resolved sound is sourced. */
typedef enum {
    DS_SRC_UNKNOWN = 0,
    DS_SRC_STREAM_RES,   /* resident ATRAC3 stream: resat3/<name>.at2|.at3    */
    DS_SRC_STREAM_ID,    /* streamed-by-id voice line (flags 0xFF): see f16    */
    DS_SRC_BANK          /* SPU-ADPCM sample in a wave*.wd bank (SE)           */
} ds_src_kind;

typedef struct {
    ds_src_kind kind;
    const ds_db_rec *rec;    /* the backing db.bin record                     */
    /* For DS_SRC_STREAM_RES: suggested relative path + extension guess.       */
    char   stream_path[64];  /* e.g. "resat3/on_v132.at2"                      */
    /* For DS_SRC_BANK: which bank group + index within it.                    */
    int    bank_group;       /* db `field` value (bank/sub-bank selector)      */
    int    bank_index;       /* db `slot` (sample index within the group)      */
    /* For DS_SRC_STREAM_ID: the stream id carried in loop_start (f16).        */
    uint32_t stream_id;
    /* Loop points in source frames (-1 = one-shot). Valid for BGM.            */
    long   loop_start;
    long   loop_end;
} ds_resolved;

typedef struct ds_db ds_db;

/* Load and parse db.bin. Returns NULL on error. Bounds-checked. */
ds_db *ds_db_load(const char *path);
void   ds_db_free(ds_db *db);

/* Number of records. */
int    ds_db_count(const ds_db *db);

/* Record by index (0..count-1), or NULL. */
const ds_db_rec *ds_db_at(const ds_db *db, int index);

/* Find a record by exact name ("on_v132", "se_0014", "bgm_070"). O(1) hashed.
 * Returns NULL if absent. This is the setSoundEffect(name)/playVoice(name) path. */
const ds_db_rec *ds_db_find(const ds_db *db, const char *name);

/* Resolve a record to its concrete source (bank/sample or stream + loop). */
void   ds_db_resolve(const ds_db *db, const ds_db_rec *rec, ds_resolved *out);

#ifdef __cplusplus
}
#endif
#endif /* DIRGE_SOUND_H */
