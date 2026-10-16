/* fed_dump.c — human-readable (text) and JSON dumps of a parsed FedFile.
 * A .fez is inflated by fed_load, so by the time we dump we always have a plain
 * FED image (f->compressed records that the SOURCE was compressed). */

#include "fed.h"
#include <math.h>

/* Print a float as a JSON number, or null when non-finite (init blocks can hold
 * int-coded or garbage bit patterns that are not valid JSON numbers). */
static void json_f(FILE *o, float v){
    if (isfinite(v)) fprintf(o, "%.6g", (double)v);
    else             fprintf(o, "null");
}

static const char *fade_name(uint8_t m){
    switch (m){ case 0: return "30-frame"; case 1: return "15-frame";
                case 2: return "immediate"; case 3: return "duration-gated";
                default: return "?"; }
}
static void role_str(uint8_t roles, char out[16]){
    int n=0; out[0]=0;
    if (roles & FED_ROLE_INIT){ out[n++]='i'; }
    if (roles & FED_ROLE_UPD ){ out[n++]='u'; }
    if (roles & FED_ROLE_AUX ){ out[n++]='a'; }
    if (!n) out[n++]='-';
    out[n]=0;
}

/* ---------------------------------------------------------------- text ---- */
void fed_dump_text(const FedFile *f, FILE *o){
    fprintf(o, "FED effect  (version byte 0x%02x)%s\n", f->version,
            f->compressed ? "  [inflated from FEZ]" : "");
    fprintf(o, "  FileCode      : 0x%08x\n", f->filecode);
    fprintf(o, "  flags         : 0x%08x  valid=%d source=%s\n",
            f->flags, (f->flags&0x10000)!=0, f->compressed?"FEZ(zlib)":"FED");
    fprintf(o, "  total size    : %u bytes\n", f->total_size);
    if (f->compressed)
        fprintf(o, "  FEZ           : comp-type=%u comp-size=%u (inflated OK)\n",
                f->comp_type, f->comp_size);
    fprintf(o, "  tables        : emitters=%u textures=%u  tableA=%u t6c=%u t70=%u t74=%u\n",
            f->emitter_count, f->texture_count, f->tableA_count,
            f->table6c_count, f->table70_count, f->table74_count);
    fprintf(o, "  parsed        : %u emitters, %u embedded textures\n\n",
            f->num_emitters, f->num_textures);

    for (uint32_t i = 0; i < f->num_emitters; i++){
        const FedEmitter *e = &f->emitters[i];
        fprintf(o, "  Emitter %u @0x%06x  id=0x%04x duration=%d frames  category=%u %s\n",
                i, e->file_off, e->id, e->duration, e->category,
                fed_category_name(e->category));
        fprintf(o, "    max_life=%u  fade_mode=%u(%s)  particles=%u modifiers=%u children=%u\n",
                e->max_life, e->fade_mode, fade_name(e->fade_mode),
                e->particle_count, e->modifier_count, e->child_count);

        /* spawn timeline */
        for (uint32_t s = 0; s < e->num_spawns; s++){
            const FedSpawnEvent *sv = &e->spawns[s];
            if (!sv->file_off) continue;
            fprintf(o, "    spawn @%d frames: %u particle-def(s) [", sv->time, sv->count);
            uint32_t show = sv->count < 16 ? sv->count : 16;
            for (uint32_t k=0;k<show;k++){
                uint32_t off = sv->tags_off + 2u*k;
                uint16_t tag = (off+2<=f->size)? (uint16_t)(f->data[off]|(f->data[off+1]<<8)) : 0;
                fprintf(o, "%s%u", k?",":"", tag & 0xfff);
            }
            fprintf(o, "%s]\n", sv->count>16?",…":"");
        }

        for (uint32_t j = 0; j < e->num_particles; j++){
            const FedParticle *p = &e->particles[j];
            if (!p->file_off) continue;
            fprintf(o, "      particle %u @0x%06x  render_cat=%u  curves=%u\n",
                    j, p->file_off, p->render_cat, p->curve_count);
            for (uint32_t c = 0; c < p->num_channels; c++){
                const FedChannel *ch = &p->channels[c];
                char rs[16]; role_str(ch->roles, rs);
                fprintf(o, "        ch%-2u type=%-3u slot=0x%02x roles=%-3s %-20s",
                        ch->idx, ch->type, ch->out_slot, rs, ch->sem);
                if (ch->has_init)
                    fprintf(o, " init=[%.3g, %.3g, %.3g, %.3g]",
                            ch->init_vec[0], ch->init_vec[1], ch->init_vec[2], ch->init_vec[3]);
                fputc('\n', o);
            }
        }
        fputc('\n', o);
    }

    if (f->num_textures){
        fprintf(o, "  Embedded textures:\n");
        for (uint32_t i = 0; i < f->num_textures; i++){
            const FedTexture *t = &f->textures[i];
            fprintf(o, "    [%u] @0x%06x  %s  %ux%u  imgtype=%u  blob=%u bytes\n",
                    i, t->file_off, t->is_tim2 ? "TIM2" : "(unknown)",
                    t->width, t->height, t->image_type, t->blob_size);
        }
    }
}

/* ---------------------------------------------------------------- json ---- */
static void json_channels(const FedFile *f, FILE *o, const FedParticle *p){
    (void)f;
    fprintf(o, "\"channels\": [");
    for (uint32_t c = 0; c < p->num_channels; c++){
        const FedChannel *ch = &p->channels[c];
        char rs[16]; role_str(ch->roles, rs);
        fprintf(o, "%s{\"idx\": %u, \"type\": %u, \"out_slot\": %u, \"roles\": \"%s\", "
                   "\"sem_hint\": \"%s\"", c?", ":"", ch->idx, ch->type, ch->out_slot, rs, ch->sem);
        if (ch->has_init){
            fprintf(o, ", \"init\": [");
            for (int k=0;k<4;k++){ if(k) fprintf(o,", "); json_f(o, ch->init_vec[k]); }
            fprintf(o, "]");
        }
        fprintf(o, "}");
    }
    fprintf(o, "]");
}

void fed_dump_json(const FedFile *f, FILE *o){
    fprintf(o, "{\n");
    fprintf(o, "  \"version\": %u,\n", f->version);
    fprintf(o, "  \"filecode\": \"0x%08x\",\n", f->filecode);
    fprintf(o, "  \"flags\": \"0x%08x\",\n", f->flags);
    fprintf(o, "  \"valid\": %s,\n", (f->flags&0x10000)?"true":"false");
    fprintf(o, "  \"source\": \"%s\",\n", f->compressed?"fez":"fed");
    fprintf(o, "  \"total_size\": %u,\n", f->total_size);
    if (f->compressed)
        fprintf(o, "  \"fez\": { \"comp_type\": %u, \"comp_size\": %u, \"inflated\": true },\n",
                f->comp_type, f->comp_size);
    fprintf(o, "  \"table_counts\": { \"tableA\": %u, \"emitters\": %u, \"textures\": %u, "
               "\"t6c\": %u, \"t70\": %u, \"t74\": %u },\n",
            f->tableA_count, f->emitter_count, f->texture_count,
            f->table6c_count, f->table70_count, f->table74_count);

    fprintf(o, "  \"emitters\": [\n");
    for (uint32_t i = 0; i < f->num_emitters; i++){
        const FedEmitter *e = &f->emitters[i];
        fprintf(o, "    {\n");
        fprintf(o, "      \"offset\": \"0x%06x\", \"id\": %u, \"duration_frames\": %d,\n",
                e->file_off, e->id, e->duration);
        fprintf(o, "      \"category\": %u, \"category_name\": \"%s\", \"max_life\": %u, "
                   "\"fade_mode\": %u,\n",
                e->category, fed_category_name(e->category), e->max_life, e->fade_mode);
        fprintf(o, "      \"modifier_count\": %u, \"child_count\": %u,\n",
                e->modifier_count, e->child_count);

        /* spawn timeline */
        fprintf(o, "      \"spawns\": [");
        int first=1;
        for (uint32_t s = 0; s < e->num_spawns; s++){
            const FedSpawnEvent *sv = &e->spawns[s];
            if (!sv->file_off) continue;
            fprintf(o, "%s{\"time\": %d, \"count\": %u, \"defs\": [", first?"":", ",
                    sv->time, sv->count);
            uint32_t show = sv->count < 64 ? sv->count : 64;
            for (uint32_t k=0;k<show;k++){
                uint32_t off = sv->tags_off + 2u*k;
                uint16_t tag = (off+2<=f->size)? (uint16_t)(f->data[off]|(f->data[off+1]<<8)) : 0;
                fprintf(o, "%s%u", k?",":"", tag & 0xfff);
            }
            fprintf(o, "]}");
            first=0;
        }
        fprintf(o, "],\n");

        fprintf(o, "      \"particles\": [\n");
        for (uint32_t j = 0; j < e->num_particles; j++){
            const FedParticle *p = &e->particles[j];
            fprintf(o, "        {\"offset\": \"0x%06x\", \"render_cat\": %u, "
                       "\"curve_count\": %u, ",
                    p->file_off, p->render_cat, p->curve_count);
            json_channels(f, o, p);
            fprintf(o, "}%s\n", (j+1<e->num_particles)?",":"");
        }
        fprintf(o, "      ]\n    }%s\n", (i+1<f->num_emitters)?",":"");
    }
    fprintf(o, "  ],\n");

    fprintf(o, "  \"textures\": [\n");
    for (uint32_t i = 0; i < f->num_textures; i++){
        const FedTexture *t = &f->textures[i];
        fprintf(o, "    {\"offset\": \"0x%06x\", \"format\": \"%s\", \"width\": %u, "
                   "\"height\": %u, \"image_type\": %u, \"blob_size\": %u}%s\n",
                t->file_off, t->is_tim2?"TIM2":"unknown", t->width, t->height,
                t->image_type, t->blob_size, (i+1<f->num_textures)?",":"");
    }
    fprintf(o, "  ]\n}\n");
}
