/* txd.c - TXD/RFD container unwrap + whole-file decode.
 *
 * Container layout (notes/doc-research-wiki/RFD-&-TXD-archives.md, bytes
 * verified):
 *   0x00  char[8]  container name
 *   0x08  char[4]  extension "txd" or "rfd"
 *   0x0C  u32      file size
 *   0x10  u16      file count
 *   0x13  u8       type flag (1 or 2) -> entry size 0x20 or 0x10
 *   0x14  u32[cnt] offset of each entry header
 * Each entry header starts with name(8)+ext(4); the payload follows the entry
 * header (0x20 bytes for type 1, 0x10 for type 2) and runs to the next entry.
 */
#include "dtex_internal.h"
#include <stdlib.h>

int dtex_unwrap(const uint8_t *data, size_t size, dtex_entry *entries, int max)
{
    if (size < 0x14) return -1;
    if (memcmp(data + 8, "txd", 3) != 0 && memcmp(data + 8, "rfd", 3) != 0)
        return -1;
    int count = rd16(data, 0x10);
    int typeflag = data[0x13];
    int hdr = (typeflag == 1) ? 0x20 : 0x10;
    if (count <= 0 || count > 4096) return -1;
    if (size < (size_t)(0x14 + count * 4)) return -1;

    uint32_t *off = (uint32_t *)malloc(sizeof(uint32_t) * count);
    if (!off) return -1;
    for (int i = 0; i < count; i++) off[i] = rd32(data, 0x14 + i * 4);

    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        uint32_t o = off[i];
        if (o + hdr > size) continue;
        uint32_t end = (i + 1 < count) ? off[i + 1] : (uint32_t)size;
        if (end > size || end < o + hdr) end = (uint32_t)size;
        dtex_entry *e = &entries[n++];
        memset(e, 0, sizeof(*e));
        memcpy(e->name, data + o, 8);
        memcpy(e->ext, data + o + 8, 3);
        e->data = data + o + hdr;
        e->size = end - (o + hdr);
    }
    free(off);
    return n;
}

int dtex_decode_container(const uint8_t *data, size_t size, dtex_set *out)
{
    out->items = NULL;
    out->count = 0;

    dtex_entry entries[4096];
    int n = dtex_unwrap(data, size, entries, 4096);
    if (n < 0) {
        /* Not a container: maybe a bare payload. */
        dtex_image img;
        if (dtex_decode_payload(data, size, NULL, &img) == 0) {
            out->items = (dtex_image *)malloc(sizeof(dtex_image));
            out->items[0] = img;
            out->count = 1;
            return 0;
        }
        return -1;
    }

    out->items = (dtex_image *)calloc(n, sizeof(dtex_image));
    if (!out->items) return -1;
    for (int i = 0; i < n; i++) {
        char nm[9]; memcpy(nm, entries[i].name, 8); nm[8] = 0;
        dtex_image img;
        if (dtex_decode_payload(entries[i].data, entries[i].size, nm, &img) == 0)
            out->items[out->count++] = img;
    }
    if (out->count == 0) { free(out->items); out->items = NULL; return -1; }
    return 0;
}
