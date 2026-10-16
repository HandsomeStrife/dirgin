/* fed.h — parser for Dirge of Cerberus (PS2) ".fed" / ".fez" visual-effect
 * definition files.  Magic "FED\x08" (46 45 44 08).
 *
 * The format and every field below was reversed from the game's OWN parser in
 * analysis/decomp/ (read-only), principally:
 *   FUN_002e4418  — the definitive load-time relocator (per-table guard bits,
 *                   bounds the stored offsets to < 0x500000).  THIS is the
 *                   authority for the container layout.
 *   FUN_002e5550  — sibling relocator (same structure).
 *   FUN_002df140  — magic check  ((*u32 & 0xffffff)==0x444546, "FED").
 *   FUN_002df520/4d8/450, FUN_0023c418 — the FEZ (compressed) path.
 *   FUN_002e4a58  — instantiate (alloc 0xA4 runtime obj, FileCode @fed+4).
 *   FUN_002e63d0  — emitter spawn: reads the emitter record fields.
 *   FUN_002eba88  — texture-table consumer (fed+0x68, count u16@0x44).
 *   FUN_002ea040 / FUN_002e7660 — per-frame particle update / channel apply.
 * See FED-FORMAT.md for the full evidence trail.
 *
 * KEY addressing rule (from FUN_002e4418): every stored 32-bit offset is
 * RELATIVE TO THE BASE OF ITS CONTAINING record or table, never absolute and
 * never (except the top-level table offsets) relative to the file start:
 *   - the 6 table offsets in the header are relative to the FILE start;
 *   - a slot inside a table points to a record RELATIVE TO THAT TABLE's base;
 *   - a pointer inside a record points RELATIVE TO THAT RECORD's base.
 * A zero slot means "empty slot" and is skipped (loader guards `!= 0`).
 *
 * All reads are bounds-checked; the file is treated as untrusted data and is
 * never executed.  Portable C11, no dependencies.
 */
#ifndef FED_H
#define FED_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define FED_MAX_CHANNELS 12   /* fixed 12-pointer channel array @E+0x24 */
#define FED_NUM_PSUBPTR  10   /* P+0x30,0x34,0x38,0x3c,0x40,0x44,0x48,0x4c,0x50,0x54 */

/* Channel roles (how the update code consumes a channel index). A channel may
 * carry several: INIT ones are applied once at spawn (FUN_002eb038 over the
 * table at P+0x30); UPD ones are re-evaluated every frame (FUN_002eadc0 over
 * the index list at P+0x3c, count byte@P+0x23); AUX ones are the list at P+0x34
 * (count byte@P+0x21, consumed on death/branch). */
#define FED_ROLE_INIT 0x1
#define FED_ROLE_UPD  0x2
#define FED_ROLE_AUX  0x4

/* A particle animation channel.  Each channel index selects a typed evaluator
 * via the descriptor pointer array at P+0x0c: descriptor = (P+0x0c-array)[idx],
 * type = u16@descriptor, out_slot = u16@descriptor+4 (a vec4-aligned byte
 * offset into the runtime particle's value region where this channel writes).
 *
 * The *type* (~60 distinct codes across the corpus) names a runtime evaluator
 * living in a C++ vtable table that is constructed at boot (DAT_0046c398 ->
 * objects in .bss); the per-type semantic is therefore NOT statically
 * recoverable from the ELF.  What IS known & authoritative is the APPLY path
 * (FUN_002e7660): the evaluated vec4 is written with a flag mask —
 *   bit 0x1 position, 0x2 velocity, 0x4 accel/gravity, 0x8 colour RGBA,
 *   0x10 orientation matrix, 0x20 billboard byte — and FUN_002e7cd0 adds
 *   position & velocity but *multiplies* the scale vec4 (so scale is a factor).
 * `sem` is a best-effort label inferred from the channel's init-vector ranges
 * (colour comps cluster in [0,128]=PS2 0..1; scale is positive & often
 * uniform); it is a HINT, not from the type table. */
typedef struct {
    uint16_t idx;            /* channel index (0..curve_count-1)                */
    uint16_t type;           /* descriptor type code (selects the evaluator)    */
    uint16_t out_slot;       /* u16@desc+4: vec4-aligned runtime write offset    */
    uint8_t  roles;          /* FED_ROLE_* bitmask                              */
    int      has_init;       /* 1 if an INIT constant vector was found          */
    float    init_vec[4];    /* the INIT constant/initial value (when has_init) */
    uint32_t desc_off;       /* abs file off of the descriptor (0 if absent)    */
    uint32_t init_off;       /* abs file off of the INIT value block (0 if none)*/
    const char *sem;         /* heuristic semantic label (hint only; never NULL)*/
} FedChannel;

/* An emitter spawn-timeline event (the "modifier" array at E+0x10, count E+0x08).
 * At `time` frames the emitter spawns the particle definitions named by `tags`
 * (each tag & 0xfff indexes the particle sub-record array at E+0x14). */
typedef struct {
    uint32_t file_off;       /* abs off of the event record                     */
    int32_t  time;           /* s16@+0: spawn time in frames                    */
    uint16_t count;          /* s16@+2: number of tags                          */
    uint32_t tags_off;       /* abs off of the u16 tag array (@+4)              */
} FedSpawnEvent;

/* An embedded particle texture.  Entries of the table at header off 0x68 are,
 * on disk, complete TIM2 images (magic "TIM2") — decodable by the existing
 * native TIM2 decoder (native/assets/textures/tim2.c). */
typedef struct {
    uint32_t file_off;   /* absolute offset of the TIM2 blob in the file      */
    uint32_t blob_size;  /* TIM2 totalSize (u32 @ blob+0x10), 0 if not TIM2   */
    int      is_tim2;    /* 1 if the blob starts with "TIM2"                  */
    uint16_t width;      /* parsed from the TIM2 picture header (0 if n/a)    */
    uint16_t height;
    uint8_t  image_type; /* TIM2 image type / GS PSM hint (0 if n/a)          */
} FedTexture;

/* A particle-definition sub-record (sub-array at emitter+0x14). Offsets below
 * are absolute file offsets (0 = not present). The 10 sub-pointers and the
 * curve-node array exist only when curve_count (byte@P+8) != 0 — the loader
 * guards the whole block on that byte. */
typedef struct {
    uint32_t file_off;                 /* absolute offset of the P record     */
    uint8_t  curve_count;              /* byte@P+0x08: entries in curve array  */
    uint8_t  aux_count;                /* byte@P+0x21: entries in array @P+0x34*/
    uint32_t curve_array_off;          /* abs off of ptr array @P+0x0c         */
    uint32_t aux_array_off;            /* abs off of ptr array @P+0x34         */
    uint32_t sub_ptr[FED_NUM_PSUBPTR]; /* abs offs of P+0x30..0x54 (0=null)    */
    uint8_t  render_cat;               /* byte@P+0x0b: per-particle draw/blend */
                                       /*   category (see FedEmitter.category) */
    FedChannel *channels;              /* curve_count decoded channels (or NULL)*/
    uint32_t    num_channels;
} FedParticle;

/* An emitter definition.  Record layout from FUN_002e63d0 (spawn) + the
 * relocator.  Scalar fields are read straight from the record; the *_off
 * fields are absolute file offsets to sub-structures (0 = none). */
typedef struct {
    uint32_t file_off;        /* absolute offset of the emitter record         */
    uint16_t id;              /* +0x00  emitter/particle id                    */
    int32_t  duration;        /* +0x04  emitter duration in frames             */
    uint16_t modifier_count;  /* +0x08  count of ptr array @+0x10              */
    uint16_t particle_count;  /* +0x0a  count of particle sub-records @+0x14   */
    uint16_t max_life;        /* +0x0c  particle max-life / fade-start frames  */
    uint8_t  category;        /* +0x0e  render/orientation category            */
    uint8_t  fade_mode;       /* +0x1f  death-fade mode (0:30f 1:15f 2:kill)   */
    uint16_t child_count;     /* +0x18  count of child-tag u16 array @+0x20    */
    uint8_t  aux28_count;     /* +0x1b  count of ptr array @+0x28              */
    uint8_t  aux2c_count;     /* +0x1c  count of ptr array @+0x2c              */
    uint32_t modifier_off;    /* +0x10  -> ptr array (modifier_count)          */
    uint32_t particle_off;    /* +0x14  -> particle sub-record offset array    */
    uint32_t child_tags_off;  /* +0x20  -> u16[child_count] child-effect tags  */
    uint32_t channels_off;    /* +0x24  -> 12 channel pointers                 */
    uint32_t aux28_off;       /* +0x28  -> ptr array (aux28_count)             */
    uint32_t aux2c_off;       /* +0x2c  -> ptr array (aux2c_count)             */
    uint32_t channel_off[FED_MAX_CHANNELS]; /* resolved abs offs (0=null)      */
    FedParticle *particles;   /* particle_count entries (null slots have off 0)*/
    uint32_t    num_particles;
    FedSpawnEvent *spawns;    /* modifier_count spawn-timeline events (or NULL) */
    uint32_t       num_spawns;
} FedEmitter;

/* Human-readable name for a render/orientation category (E+0x0e / P+0x0b) and
 * the blend-state it resolves to at draw time (FUN_002e84a8 -> FUN_002f0d60
 * fixed presets 0/1/2, or FUN_002f0d80 per-texture preset for the "animated
 * texture" categories).  The exact GS ALPHA register values live in boot-built
 * .bss tables (DAT_004ee9d0 / DAT_004e63b0) and are not statically readable. */
const char *fed_category_name(uint8_t cat);

typedef struct {
    /* borrowed view of the whole file (not owned by FedFile) */
    const uint8_t *data;
    size_t         size;

    /* header */
    uint8_t  version;      /* byte @0x03 (always 0x08 — part of the magic)     */
    uint32_t filecode;     /* @0x04  effect FileCode / id                      */
    uint32_t flags;        /* @0x08  bit16 valid, bit15 compressed; 0x2000..   */
                           /*        are runtime reloc-guards (0 on disk)      */
    uint32_t total_size;   /* @0x0c  size (decompressed size for FEZ)          */
    int      compressed;   /* flags & 0x8000 (FEZ)                             */
    uint8_t  comp_type;    /* FEZ only: byte @0x10 (=2 observed)               */
    uint32_t comp_size;    /* FEZ only: @0x14 compressed size                  */

    /* six top-level tables (file-relative offsets, u16 counts) */
    uint32_t tableA_off;   uint16_t tableA_count;   /* @0x60 / @0x40           */
    uint32_t emitter_off;  uint16_t emitter_count;  /* @0x64 / @0x42  (MAIN)   */
    uint32_t texture_off;  uint16_t texture_count;  /* @0x68 / @0x44  (TIM2)   */
    uint32_t table6c_off;  uint16_t table6c_count;  /* @0x6c / @0x46           */
    uint32_t table70_off;  uint16_t table70_count;  /* @0x70 / @0x48           */
    uint32_t table74_off;  uint16_t table74_count;  /* @0x74 / @0x4a           */

    FedEmitter *emitters;   uint32_t num_emitters;  /* parsed non-null slots   */
    FedTexture *textures;   uint32_t num_textures;

    /* When the input was a .fez, fed_load inflates it and parses the result.
     * `inflated` owns that buffer (freed by fed_free); `data`/`size` point into
     * it.  NULL for a plain .fed (data is borrowed from the caller). */
    uint8_t *inflated;
} FedFile;

/* Parse `data`/`size` into `out`.  Returns 0 on success, negative on error
 * (writes a message into errbuf).  On success call fed_free(out).
 * NOTE: `data` must stay alive for the lifetime of `out` (borrowed). */
int  fed_load(const uint8_t *data, size_t size, FedFile *out,
              char *errbuf, size_t errlen);
void fed_free(FedFile *out);

/* True if the buffer looks like a FED/FEZ (magic check only). */
int  fed_check_magic(const uint8_t *data, size_t size);

/* Human-readable dumps of a parsed effect. */
void fed_dump_text(const FedFile *f, FILE *out);
void fed_dump_json(const FedFile *f, FILE *out);

#endif /* FED_H */
