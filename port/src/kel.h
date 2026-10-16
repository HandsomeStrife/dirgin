/* kel.h - native loader for Dirge of Cerberus KEL.DAT archive.
 * Reimplements the EE file subsystem mapped in notes/engine-map.md:
 *   FILELIST.BIN index (zlib chunks of "pos:usize:csize:path") + KEL.DAT.
 * PC-port bridge recipe: resolve -> pread(KEL.DAT, pos*0x800) -> zlib inflate. */
#ifndef KEL_H
#define KEL_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t filecode;
    uint32_t pos;      /* file position: multiply by 0x800 for byte offset */
    uint32_t usize;    /* uncompressed size */
    uint32_t csize;    /* compressed size in archive (==usize => stored raw) */
    char     path[128];/* virtual path, or "" if none (noPath) */
} kel_entry;

typedef struct {
    kel_entry *entries;
    uint32_t   count;
    char       dat_path[1024];
} kel_archive;

/* Open: parse FILELIST.BIN, remember KEL.DAT path. Returns 0 on success. */
int  kel_open(kel_archive *a, const char *filelist_path, const char *dat_path);
void kel_close(kel_archive *a);

/* Find an entry by exact virtual path, or by FileCode. NULL if not found. */
const kel_entry *kel_find_path(const kel_archive *a, const char *path);
const kel_entry *kel_find_code(const kel_archive *a, uint32_t filecode);

/* Load (and inflate if needed) one entry's bytes. Caller frees *out.
 * Returns decompressed size, or -1 on error. */
long kel_read(const kel_archive *a, const kel_entry *e, uint8_t **out);

#endif
