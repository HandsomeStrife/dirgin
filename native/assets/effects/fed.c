/* fed.c — FED/FEZ visual-effect definition parser.  See fed.h for the format
 * and the decompiled-parser evidence.  Portable C11, no dependencies, every
 * read bounds-checked against the (untrusted) file buffer. */

#include "fed.h"
#include "fez.h"
#include <stdlib.h>
#include <string.h>

/* ---- bounds-checked little-endian readers ------------------------------- */
/* Each returns 0 on success and writes *v; returns -1 (leaving *v=0) if the
 * requested field does not lie fully inside [0,size). */
static int rd_u8 (const FedFile *f, uint32_t o, uint8_t  *v){
    if (o + 1u > f->size) { *v = 0; return -1; }
    *v = f->data[o]; return 0;
}
static int rd_u16(const FedFile *f, uint32_t o, uint16_t *v){
    if (o + 2u > f->size || o + 2u < o) { *v = 0; return -1; }
    *v = (uint16_t)(f->data[o] | (f->data[o+1] << 8)); return 0;
}
static int rd_u32(const FedFile *f, uint32_t o, uint32_t *v){
    if (o + 4u > f->size || o + 4u < o) { *v = 0; return -1; }
    *v = (uint32_t)f->data[o] | ((uint32_t)f->data[o+1] << 8)
       | ((uint32_t)f->data[o+2] << 16) | ((uint32_t)f->data[o+3] << 24);
    return 0;
}
static int rd_i32(const FedFile *f, uint32_t o, int32_t *v){
    uint32_t u; int r = rd_u32(f,o,&u); *v = (int32_t)u; return r;
}
static int rd_f32(const FedFile *f, uint32_t o, float *v){
    uint32_t u; int r = rd_u32(f,o,&u); memcpy(v,&u,4); return r;
}

/* Render/orientation category (E+0x0e, P+0x0b) -> label.  The branch constants
 * come from the spawn consumer (FUN_002e63d0), the billboard builder
 * (FUN_002e9db0: cats 2/6/7/9 take matrix paths) and the blend-state setter
 * (FUN_002e84a8: cats 0/5 -> fixed blend preset, 4/6 -> per-texture preset). */
const char *fed_category_name(uint8_t cat){
    switch (cat){
        case 0:  return "cat0 (alpha-blend / default preset)";
        case 1:  return "cat1 (billboard)";
        case 2:  return "cat2 (oriented, matrix)";
        case 4:  return "cat4 (animated-texture preset)";
        case 5:  return "cat5 (additive? blend preset 2)";
        case 6:  return "cat6 (animated-texture, oriented)";
        case 7:  return "cat7 (oriented+)";
        case 8:  return "cat8";
        case 9:  return "cat9 (matrix)";
        case 11: return "cat11 (billboard variant)";
        default: return "cat? (unrecognised)";
    }
}

/* Heuristic semantic hint from a channel's INIT vector.  This is NOT from the
 * (boot-built, .bss) type table and is a GUESS from the value *shape* only, to
 * help a human recognise colour/scale channels.  We first classify whether the
 * three leading components read as real floats or as integer-coded bits (tiny
 * denormals), because the init blocks mix both and a denormal is never a
 * colour. */
static int looks_float(float v){
    float a = v<0 ? -v : v;
    return v==0.0f || (a >= 1e-3f && a <= 1e9f);
}
static const char *channel_sem_hint(const FedChannel *c){
    if (!c->has_init) return "(animated/typed)";
    float a=c->init_vec[0], b=c->init_vec[1], d=c->init_vec[2];
    if (!(looks_float(a)&&looks_float(b)&&looks_float(d))) return "(typed; int-coded init)";
    int pos3 = (a>=0 && b>=0 && d>=0);
    if (pos3 && a==b && b==d && a>0)                 return "~uniform scale/size?";
    if (pos3 && a<=128.5f && b<=128.5f && d<=128.5f) return "~colour/alpha 0..128?";
    if (pos3)                                        return "~vector/scale?";
    return "(typed; see type code)";
}

/* add two offsets guarding against overflow; returns 0xFFFFFFFF on overflow
 * (an always-out-of-bounds sentinel). */
static uint32_t off_add(uint32_t base, uint32_t rel){
    uint32_t s = base + rel;
    if (s < base) return 0xFFFFFFFFu;
    return s;
}

int fed_check_magic(const uint8_t *d, size_t n){
    return n >= 4 && d[0]=='F' && d[1]=='E' && d[2]=='D';
}

/* ---- TIM2 picture-header peek (texture table entries are embedded TIM2) --- */
/* TIM2: "TIM2"(4) ver(1) fmt(1) count(u16) ... then 16-byte-aligned picture
 * header at +0x10: totalSize u32@+0x00, ... imageType u8@+0x13, width u16@+0x14,
 * height u16@+0x16.  We only peek, never decode here. */
static void peek_tim2(FedFile *f, uint32_t off, FedTexture *t){
    t->file_off = off; t->is_tim2 = 0; t->blob_size = 0;
    t->width = t->height = 0; t->image_type = 0;
    if (off + 4u <= f->size &&
        f->data[off]=='T'&&f->data[off+1]=='I'&&f->data[off+2]=='M'&&f->data[off+3]=='2'){
        t->is_tim2 = 1;
        uint32_t ph = off + 0x10;           /* picture header */
        uint32_t tot; if (rd_u32(f, ph + 0x00, &tot)==0) t->blob_size = tot;
        uint8_t  it;  if (rd_u8 (f, ph + 0x13, &it )==0) t->image_type = it;
        uint16_t w,h;
        if (rd_u16(f, ph + 0x14, &w)==0) t->width  = w;
        if (rd_u16(f, ph + 0x16, &h)==0) t->height = h;
    }
}

/* ---- particle sub-record ------------------------------------------------- */
/* P base = emitter_base + slot.  The curve block (array @P+0x0c and the ten
 * P+0x30..0x54 pointers) exists only when byte@P+0x08 != 0 (loader guard). */
/* Tag channel idx with a role, recording INIT constant vector when given. */
static void channel_add_role(FedParticle *p, uint16_t idx, uint8_t role,
                             int has_init, const float iv[4], uint32_t ioff){
    if (idx >= p->num_channels) return;              /* out-of-range index */
    FedChannel *c = &p->channels[idx];
    c->roles |= role;
    if (has_init && !c->has_init){
        c->has_init = 1; c->init_off = ioff;
        for (int k=0;k<4;k++) c->init_vec[k] = iv[k];
    }
}

/* Decode a particle's animation channels.  descriptor array @P+0x0c; per-index
 * type/out-slot from the descriptor; roles from the three index lists
 * (INIT @P+0x30, UPD @P+0x3c, AUX @P+0x34).  All reads bounds-checked. */
static void decode_channels(FedFile *f, uint32_t pbase, FedParticle *p){
    if (!p->curve_count || !p->curve_array_off) return;
    p->channels = (FedChannel*)calloc(p->curve_count, sizeof(FedChannel));
    if (!p->channels) return;
    p->num_channels = p->curve_count;

    /* per-channel descriptor (type + out-slot) */
    for (uint32_t i = 0; i < p->curve_count; i++){
        FedChannel *c = &p->channels[i];
        c->idx = (uint16_t)i; c->sem = "?";
        uint32_t rel;
        if (rd_u32(f, p->curve_array_off + 4u*i, &rel)==0 && rel){
            uint32_t d = off_add(p->curve_array_off, rel);
            c->desc_off = d;
            rd_u16(f, d + 0x00, &c->type);
            rd_u16(f, d + 0x04, &c->out_slot);
        }
    }

    /* INIT channels: table @sub_ptr[0] (P+0x30); count byte@+4; stride-8 entries
     * {u16 idx, u16 off}; the constant/initial vec4 lives at table_base+off. */
    uint32_t CL = p->sub_ptr[0];
    if (CL){
        uint8_t n=0; rd_u8(f, CL + 4, &n);
        for (uint8_t k=0;k<n;k++){
            uint16_t idx=0, off=0;
            uint32_t e = CL + 8u + 8u*k;
            rd_u16(f, e + 0, &idx); rd_u16(f, e + 2, &off);
            uint32_t vo = off_add(CL, off);
            float iv[4]={0,0,0,0};
            for (int t=0;t<4;t++) rd_f32(f, vo + 4u*t, &iv[t]);
            channel_add_role(p, idx, FED_ROLE_INIT, 1, iv, vo);
        }
    }
    /* UPD channels: index list @sub_ptr[3] (P+0x3c); count byte@P+0x21? no:
     * count is byte@P+0x23 (FUN_002eadc0). */
    uint32_t UL = p->sub_ptr[3];
    if (UL){
        uint8_t n=0; rd_u8(f, pbase + 0x23, &n);
        for (uint8_t k=0;k<n;k++){ uint16_t idx=0; rd_u16(f, UL + 2u*k, &idx);
            channel_add_role(p, idx, FED_ROLE_UPD, 0, NULL, 0); }
    }
    /* AUX channels: index list @sub_ptr[1] (P+0x34); count byte@P+0x21. */
    uint32_t AL = p->sub_ptr[1];
    if (AL){
        uint8_t n = p->aux_count;
        for (uint8_t k=0;k<n;k++){ uint16_t idx=0; rd_u16(f, AL + 2u*k, &idx);
            channel_add_role(p, idx, FED_ROLE_AUX, 0, NULL, 0); }
    }

    for (uint32_t i=0;i<p->num_channels;i++)
        p->channels[i].sem = channel_sem_hint(&p->channels[i]);
}

static void parse_particle(FedFile *f, uint32_t pbase, FedParticle *p){
    static const uint32_t SUB_OFF[FED_NUM_PSUBPTR] =
        {0x30,0x34,0x38,0x3c,0x40,0x44,0x48,0x4c,0x50,0x54};
    memset(p, 0, sizeof(*p));
    p->file_off = pbase;
    if (rd_u8(f, pbase + 0x08, &p->curve_count) != 0) { p->curve_count = 0; return; }
    rd_u8(f, pbase + 0x0b, &p->render_cat);
    if (p->curve_count == 0) return;                 /* no sub-structure */
    rd_u8(f, pbase + 0x21, &p->aux_count);
    uint32_t rel;
    if (rd_u32(f, pbase + 0x0c, &rel)==0 && rel) p->curve_array_off = off_add(pbase, rel);
    if (rd_u32(f, pbase + 0x34, &rel)==0 && rel) p->aux_array_off   = off_add(pbase, rel);
    for (int i = 0; i < FED_NUM_PSUBPTR; i++){
        if (rd_u32(f, pbase + SUB_OFF[i], &rel)==0 && rel)
            p->sub_ptr[i] = off_add(pbase, rel);
    }
    decode_channels(f, pbase, p);
}

/* ---- emitter record ------------------------------------------------------ */
static int parse_emitter(FedFile *f, uint32_t ebase, FedEmitter *e){
    memset(e, 0, sizeof(*e));
    e->file_off = ebase;
    /* scalar fields (bounds-checked; absent -> 0) */
    rd_u16(f, ebase + 0x00, &e->id);
    rd_i32(f, ebase + 0x04, &e->duration);
    rd_u16(f, ebase + 0x08, &e->modifier_count);
    rd_u16(f, ebase + 0x0a, &e->particle_count);
    rd_u16(f, ebase + 0x0c, &e->max_life);
    rd_u8 (f, ebase + 0x0e, &e->category);
    rd_u8 (f, ebase + 0x1b, &e->aux28_count);
    rd_u8 (f, ebase + 0x1c, &e->aux2c_count);
    rd_u16(f, ebase + 0x18, &e->child_count);
    rd_u8 (f, ebase + 0x1f, &e->fade_mode);

    uint32_t rel;
    if (rd_u32(f, ebase + 0x10, &rel)==0 && rel) e->modifier_off   = off_add(ebase, rel);
    if (rd_u32(f, ebase + 0x14, &rel)==0 && rel) e->particle_off   = off_add(ebase, rel);
    if (rd_u32(f, ebase + 0x20, &rel)==0 && rel) e->child_tags_off = off_add(ebase, rel);
    if (rd_u32(f, ebase + 0x24, &rel)==0 && rel) e->channels_off   = off_add(ebase, rel);
    if (rd_u32(f, ebase + 0x28, &rel)==0 && rel) e->aux28_off      = off_add(ebase, rel);
    if (rd_u32(f, ebase + 0x2c, &rel)==0 && rel) e->aux2c_off      = off_add(ebase, rel);

    /* resolve the 12 channel pointers (each stored relative to the emitter) */
    if (e->channels_off){
        for (int i = 0; i < FED_MAX_CHANNELS; i++){
            uint32_t cr;
            if (rd_u32(f, e->channels_off + 4u*i, &cr)==0 && cr)
                e->channel_off[i] = off_add(ebase, cr);
        }
    }

    /* spawn timeline: array @E+0x10 (count @E+0x08); each slot is an offset
     * relative to the EMITTER base (verified: slot->E+rel yields time=0,
     * count=particle_count, sequential tags).  Event = {s16 time, s16 count,
     * u16 tags[count]} and FUN_002ea040 spawns tag&0xfff from E+0x14 at `time`. */
    if (e->modifier_off && e->modifier_count){
        e->spawns = (FedSpawnEvent*)calloc(e->modifier_count, sizeof(FedSpawnEvent));
        if (!e->spawns) return -1;
        e->num_spawns = e->modifier_count;
        for (uint32_t i = 0; i < e->modifier_count; i++){
            uint32_t srel;
            if (rd_u32(f, e->modifier_off + 4u*i, &srel) != 0 || srel == 0){
                e->spawns[i].file_off = 0; continue;
            }
            uint32_t ev = off_add(ebase, srel);     /* relative to E */
            FedSpawnEvent *s = &e->spawns[i];
            s->file_off = ev;
            uint16_t tt=0, cc=0;
            rd_u16(f, ev + 0, &tt); rd_u16(f, ev + 2, &cc);
            s->time = (int32_t)(int16_t)tt;         /* s16 time in frames */
            s->count = cc;
            s->tags_off = ev + 4;
        }
    }

    /* particle sub-records: offset array @particle_off, each slot relative to
     * the EMITTER base; a zero slot is an empty particle. */
    if (e->particle_off && e->particle_count){
        e->particles = (FedParticle*)calloc(e->particle_count, sizeof(FedParticle));
        if (!e->particles) return -1;
        e->num_particles = e->particle_count;
        for (uint32_t j = 0; j < e->particle_count; j++){
            uint32_t slot;
            if (rd_u32(f, e->particle_off + 4u*j, &slot) != 0 || slot == 0){
                e->particles[j].file_off = 0;         /* empty slot */
                continue;
            }
            parse_particle(f, off_add(ebase, slot), &e->particles[j]);
        }
    }
    return 0;
}

/* ---- top level ----------------------------------------------------------- */
int fed_load(const uint8_t *data, size_t size, FedFile *out,
             char *errbuf, size_t errlen){
    memset(out, 0, sizeof(*out));
    out->data = data; out->size = size;

    #define FAIL(msg) do{ if(errbuf&&errlen) snprintf(errbuf,errlen,"%s",msg); \
                          fed_free(out); return -1; }while(0)

    if (size < 0x78) FAIL("file too small for a FED header");
    if (!fed_check_magic(data, size)) FAIL("bad magic (expected 'FED')");

    out->version    = data[3];
    rd_u32(out, 0x04, &out->filecode);
    rd_u32(out, 0x08, &out->flags);
    rd_u32(out, 0x0c, &out->total_size);
    out->compressed = (out->flags & 0x8000u) != 0;

    if (out->compressed){
        /* FEZ: fixed-key block-cipher over a zlib DEFLATE stream (see fez.c).
         * Inflate it, then parse the resulting plain FED image in place. */
        rd_u8 (out, 0x10, &out->comp_type);
        rd_u32(out, 0x14, &out->comp_size);
        uint8_t *inf = NULL; size_t infsz = 0;
        char ebuf[128];
        if (fez_inflate(data, size, &inf, &infsz, ebuf, sizeof ebuf) != 0) FAIL(ebuf);
        if (infsz < 0x78 || !fed_check_magic(inf, infsz)){
            free(inf); FAIL("fez: inflated payload is not a FED");
        }
        out->inflated = inf;
        out->data = inf; out->size = infsz;    /* parse the inflated image */
        out->version = inf[3];
        rd_u32(out, 0x04, &out->filecode);
        rd_u32(out, 0x08, &out->flags);
        rd_u32(out, 0x0c, &out->total_size);
        /* fall through and parse like a plain FED */
    }

    /* counts (u16) and table offsets (u32, file-relative) */
    rd_u16(out, 0x40, &out->tableA_count);  rd_u32(out, 0x60, &out->tableA_off);
    rd_u16(out, 0x42, &out->emitter_count); rd_u32(out, 0x64, &out->emitter_off);
    rd_u16(out, 0x44, &out->texture_count); rd_u32(out, 0x68, &out->texture_off);
    rd_u16(out, 0x46, &out->table6c_count); rd_u32(out, 0x6c, &out->table6c_off);
    rd_u16(out, 0x48, &out->table70_count); rd_u32(out, 0x70, &out->table70_off);
    rd_u16(out, 0x4a, &out->table74_count); rd_u32(out, 0x74, &out->table74_off);

    /* --- emitters (MAIN table @0x64).  slot relative to the table base. --- */
    if (out->emitter_off && out->emitter_count){
        out->emitters = (FedEmitter*)calloc(out->emitter_count, sizeof(FedEmitter));
        if (!out->emitters) FAIL("out of memory (emitters)");
        for (uint32_t i = 0; i < out->emitter_count; i++){
            uint32_t slot;
            if (rd_u32(out, out->emitter_off + 4u*i, &slot) != 0) continue;
            if (slot == 0) continue;                         /* empty slot */
            uint32_t ebase = off_add(out->emitter_off, slot);
            if (ebase + 0x30u > size) continue;              /* record OOB */
            if (parse_emitter(out, ebase, &out->emitters[out->num_emitters]) != 0)
                FAIL("out of memory (particles)");
            out->num_emitters++;
        }
    }

    /* --- embedded textures (table @0x68).  entry offs relative to table. --- */
    if (out->texture_off && out->texture_count){
        out->textures = (FedTexture*)calloc(out->texture_count, sizeof(FedTexture));
        if (!out->textures) FAIL("out of memory (textures)");
        for (uint32_t i = 0; i < out->texture_count; i++){
            uint32_t ent;
            if (rd_u32(out, out->texture_off + 4u*i, &ent) != 0) continue;
            if (ent == 0) continue;
            uint32_t toff = off_add(out->texture_off, ent);
            if (toff >= size) continue;
            peek_tim2(out, toff, &out->textures[out->num_textures]);
            out->num_textures++;
        }
    }

    #undef FAIL
    return 0;
}

void fed_free(FedFile *out){
    if (!out) return;
    if (out->emitters){
        for (uint32_t i = 0; i < out->emitter_count; i++){
            FedEmitter *e = &out->emitters[i];
            if (e->particles){
                for (uint32_t j = 0; j < e->num_particles; j++)
                    free(e->particles[j].channels);
                free(e->particles);
            }
            free(e->spawns);
        }
        free(out->emitters);
    }
    free(out->textures);
    free(out->inflated);
    out->emitters = NULL; out->textures = NULL; out->inflated = NULL;
    out->num_emitters = out->num_textures = 0;
}
