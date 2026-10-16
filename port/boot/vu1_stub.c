/* =====================================================================
 * vu1_stub.c  --  M1 VU1 stub (does nothing).  With rendering stubbed to a
 * log backend, no geometry is needed, so the VU1 microcode engine (WS-V) is
 * replaced by this no-op that satisfies the vu1.h interface.  The HLE VIF1
 * display-list sink is pointed at vu1_vif1_sink(), which simply discards the
 * DMAtag-referenced qwords (a count is kept for the report).
 * ===================================================================== */
#include "vu1.h"
#include <stdlib.h>

struct vu1_ctx { vu_qword datamem[VU1_DATAMEM_BYTES/16]; unsigned long vif1_qwords; };

vu1_ctx *vu1_create(void){ return calloc(1, sizeof(vu1_ctx)); }
void     vu1_destroy(vu1_ctx *c){ free(c); }
void     vu1_reset(vu1_ctx *c){ (void)c; }
void     vu1_upload_micro(vu1_ctx *c, uint32_t d, const void *s, size_t n){ (void)c;(void)d;(void)s;(void)n; }
void     vu1_upload_data (vu1_ctx *c, uint32_t d, const vu_qword *s, size_t n){ (void)c;(void)d;(void)s;(void)n; }
void     vu1_set_top (vu1_ctx *c, uint32_t t){ (void)c;(void)t; }
void     vu1_set_itop(vu1_ctx *c, uint32_t t){ (void)c;(void)t; }
void     vu1_run(vu1_ctx *c, uint32_t e){ (void)c;(void)e; }
void     vu1_set_xgkick_cb(vu1_ctx *c, vu1_xgkick_fn f, void *u){ (void)c;(void)f;(void)u; }
vu_qword *vu1_datamem(vu1_ctx *c){ return c ? c->datamem : NULL; }

/* HLE VIF1 sink: one DMAtag's referenced qwords (VIFcodes + unpack data). */
unsigned long g_vif1_qwords_seen = 0;
void vu1_vif1_sink(void *user, const uint8_t *qwords, size_t nqw){
    (void)user; (void)qwords; g_vif1_qwords_seen += (unsigned long)nqw;
}
