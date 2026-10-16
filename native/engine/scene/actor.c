/* =====================================================================
 * actor.c  --  Reauthored ACTOR / ENTITY subsystem.
 * See actor.h for the oracle mapping and guest struct offsets.
 *
 * Every function is annotated with the decompiled original it reauthors.
 * The branch structure, loop bounds, pool stride, null handling and bit
 * arithmetic are transcribed from analysis/decomp/ (read-only); only raw
 * offsets, raw pointers, DAT_ globals, EE/VU0 intrinsics and Ghidra names
 * are replaced with named fields, the dmath library and the scene_graph
 * API already ported.
 * ===================================================================== */
#include "actor.h"
#include <stddef.h>
#include <string.h>

/* 4096 angle-units <-> radians (engine convention; see camera.h). */
#define DOC_TWO_PI 6.28318530717958647692f
static float units_to_radians(float u) { return u * (DOC_TWO_PI / DOC_ACTOR_ANGLE_UNITS_PER_TURN); }
static float radians_to_units(float r) { return r * (DOC_ACTOR_ANGLE_UNITS_PER_TURN / DOC_TWO_PI); }

/* ---------------------------------------------------------------------
 * Per-actor reset.  Reauthors the relevant subset of FUN_005a7978 (the
 * 1036-byte object initializer called out of the allocator): zero the
 * block, then set the OCCUPIED bit in draw_flags. The full default-state
 * init (skeletal tables, the +0x40c = 0.5f scalar, etc.) belongs to the
 * animation/model subsystems and is a documented hook; only the fields the
 * ported accessors touch are initialised here.
 * ------------------------------------------------------------------- */
void doc_actor_reset(DocActor *a)
{
    if (a == NULL) return;
    memset(a, 0, sizeof *a);
    a->orientation = dvec3_make(0.0f, 0.0f, 0.0f);
    a->object.draw_flags = DOC_ACTOR_FLAG_OCCUPIED;   /* mark slot in use */
}

/* ---------------------------------------------------------------------
 * FUN_005a71b8 (pool register). Original, decoded:
 *   DAT_005b37a0 = base;  DAT_005b37a4 = region_size / 0x480;
 *   for each slot: slot[+0x440] &= ~0x400000;        // clear OCCUPIED
 * ------------------------------------------------------------------- */
void doc_actor_manager_init(DocActorManager *mgr, DocActor *slots, int capacity)
{
    if (mgr == NULL) return;
    mgr->slots       = slots;
    mgr->capacity    = (slots != NULL && capacity > 0) ? capacity : 0;
    mgr->active_head = NULL;
    mgr->live_count  = 0;
    for (int i = 0; i < mgr->capacity; i++)
        mgr->slots[i].object.draw_flags &= ~DOC_ACTOR_FLAG_OCCUPIED;
}

/* ---------------------------------------------------------------------
 * FUN_005a7208 (allocate). Original, decoded:
 *   if (DAT_005b37a0 == 0) return 0;
 *   for i in [0, DAT_005b37a4):
 *       if ((slot[+0x440] & 0x400000) == 0) return slot;   // first free
 *       slot += 0x480;
 *   return 0;                                              // pool full
 * The guest then runs FUN_005a7978 on the slot (which sets OCCUPIED); we
 * do the same via doc_actor_reset.
 * ------------------------------------------------------------------- */
DocActor *doc_actor_alloc(DocActorManager *mgr)
{
    if (mgr == NULL || mgr->slots == NULL) return NULL;
    for (int i = 0; i < mgr->capacity; i++) {
        DocActor *a = &mgr->slots[i];
        if ((a->object.draw_flags & DOC_ACTOR_FLAG_OCCUPIED) == 0) {
            doc_actor_reset(a);                 /* zero + set OCCUPIED      */
            mgr->live_count++;
            return a;
        }
    }
    return NULL;                                 /* pool exhausted          */
}

/* ---------------------------------------------------------------------
 * FUN_005a7148 (free). Original, decoded:
 *   if (ptr in [base, base + count*0x480))  ptr[+0x440] &= ~0x400000;
 *   else if (ptr != 0)                      general_free(ptr);  // hook
 * Returning a slot to the pool is just clearing OCCUPIED.
 * ------------------------------------------------------------------- */
void doc_actor_free(DocActorManager *mgr, DocActor *a)
{
    if (mgr == NULL || a == NULL) return;
    int in_pool = (mgr->slots != NULL &&
                   a >= mgr->slots && a < mgr->slots + mgr->capacity);
    if (!in_pool)
        return;   /* guest would general-free a non-pool pointer: a hook */
    if (a->object.draw_flags & DOC_ACTOR_FLAG_OCCUPIED) {
        a->object.draw_flags &= ~DOC_ACTOR_FLAG_OCCUPIED;
        if (mgr->live_count > 0) mgr->live_count--;
    }
}

/* Link `a` at the head of the active list (the DAT_005b3788 chain). */
void doc_actor_list_push(DocActorManager *mgr, DocActor *a)
{
    if (mgr == NULL || a == NULL) return;
    a->prev = NULL;
    a->next = mgr->active_head;
    if (mgr->active_head != NULL)
        mgr->active_head->prev = a;
    mgr->active_head = a;
}

/* ---------------------------------------------------------------------
 * FUN_005a7898 (active-list tail). Original walks `*p` (the +0x00 next
 * link) from the head until the next pointer is 0, returning the last node.
 * ------------------------------------------------------------------- */
DocActor *doc_actor_list_tail(const DocActorManager *mgr)
{
    if (mgr == NULL || mgr->active_head == NULL) return NULL;
    DocActor *p = mgr->active_head;
    while (p->next != NULL)
        p = p->next;
    return p;
}

/* ---------------------------------------------------------------------
 * FUN_005a7d88 (destroy / unlink). Original, decoded:
 *   - walk the active list; for any actor whose attach-slot 0 == `a`,
 *     clear that attachment (FUN_005a7698(parent,0,0,0));
 *   - unlink `a` from the doubly-linked active list, fixing head/next/prev;
 *   - clear `a`'s OCCUPIED bit (+0x440 &= ~0x400000);
 *   - if (arg & 1) return the slot to the pool (FUN_005a7148).
 * The decomp only clears attach slot 0 in its walk; we clear every slot
 * that holds `a` so no dangling child references survive (the guest's
 * single-slot clear is the common case — this is a safe superset and is
 * flagged here rather than silently matching a narrower original).
 * ------------------------------------------------------------------- */
void doc_actor_destroy(DocActorManager *mgr, DocActor *a, int release)
{
    if (mgr == NULL || a == NULL) return;

    /* detach from any parent that references `a` */
    for (DocActor *p = mgr->active_head; p != NULL; p = p->next) {
        for (int i = 0; i < DOC_ACTOR_MAX_ATTACH; i++)
            if (p->attach[i] == a)
                doc_actor_attach(p, i, NULL, 0);
    }

    /* unlink from the active list */
    if (mgr->active_head == a)
        mgr->active_head = a->next;
    if (a->prev != NULL) a->prev->next = a->next;
    if (a->next != NULL) a->next->prev = a->prev;
    a->next = NULL;
    a->prev = NULL;

    a->object.draw_flags &= ~DOC_ACTOR_FLAG_OCCUPIED;
    if (mgr->live_count > 0) mgr->live_count--;

    if (release & 1)
        doc_actor_free(mgr, a);   /* idempotent: OCCUPIED already clear */
}

/* ---------------------------------------------------------------------
 * FUN_005a7008 / FUN_005a70c8 (set / get position vector @ +0x30).
 * The guest stores a 4-float vector (x,y,z,w); we keep the xyz and let the
 * w component (homogeneous pad) stay implicit.
 * ------------------------------------------------------------------- */
void doc_actor_set_position(DocActor *a, dvec3 pos)
{
    if (a != NULL) a->position = pos;
}
dvec3 doc_actor_get_position(const DocActor *a)
{
    return a != NULL ? a->position : dvec3_make(0.0f, 0.0f, 0.0f);
}

/* FUN_005a7288 / FUN_005a7298 (set / get orientation vector @ +0xe0). */
void doc_actor_set_orientation(DocActor *a, dvec3 euler_radians)
{
    if (a != NULL) a->orientation = euler_radians;
}
dvec3 doc_actor_get_orientation(const DocActor *a)
{
    return a != NULL ? a->orientation : dvec3_make(0.0f, 0.0f, 0.0f);
}

/* KerberosActor.setdir/getdir: fixed-point heading about Y (up). */
void doc_actor_set_dir(DocActor *a, int dir_units)
{
    if (a != NULL) a->orientation.y = units_to_radians((float)dir_units);
}
int doc_actor_get_dir(const DocActor *a)
{
    if (a == NULL) return 0;
    float u = radians_to_units(a->orientation.y);
    return (int)(u >= 0.0f ? u + 0.5f : u - 0.5f);   /* round to nearest */
}

/* KerberosActor.setqt:(FFFF) -> euler (yaw-pitch-roll, ZYX convention). */
void doc_actor_set_quaternion(DocActor *a, dquat q)
{
    if (a == NULL) return;
    q = dquat_normalize(q);
    /* roll (x) */
    float sinr = 2.0f * (q.w * q.x + q.y * q.z);
    float cosr = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    float roll = atan2f(sinr, cosr);
    /* pitch (y), clamped to avoid the asinf domain edge */
    float sinp = 2.0f * (q.w * q.y - q.z * q.x);
    float pitch = asinf(dm_clampf(sinp, -1.0f, 1.0f));
    /* yaw (z) */
    float siny = 2.0f * (q.w * q.z + q.x * q.y);
    float cosy = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    float yaw = atan2f(siny, cosy);
    a->orientation = dvec3_make(roll, pitch, yaw);
}

/* Clean world transform = translate(position) * Rz * Ry * Rx.
 * (The guest VU0 assembly into +0x120 is below the seam.) */
dmat4 doc_actor_world_matrix(const DocActor *a)
{
    if (a == NULL) return dmat4_identity();
    dmat4 rx = dmat4_rotation(dvec3_make(1.0f, 0.0f, 0.0f), a->orientation.x);
    dmat4 ry = dmat4_rotation(dvec3_make(0.0f, 1.0f, 0.0f), a->orientation.y);
    dmat4 rz = dmat4_rotation(dvec3_make(0.0f, 0.0f, 1.0f), a->orientation.z);
    dmat4 rot = dmat4_mul(dmat4_mul(rz, ry), rx);
    dmat4 trans = dmat4_translation(a->position);
    return dmat4_mul(trans, rot);
}

/* ---------------------------------------------------------------------
 * Visibility / draw state.
 * ------------------------------------------------------------------- */

/* Delegates to the already-ported FUN_005a85f8. */
void doc_actor_set_draw_mode(DocActor *a, unsigned mode)
{
    if (a != NULL) doc_scene_object_set_draw_mode(&a->object, mode);
}

/* FUN_005a8658: (int)(draw_flags << 20) >> 28 == bits 8..11. */
unsigned doc_actor_draw_mode(const DocActor *a)
{
    return a != NULL ? (unsigned)((a->object.draw_flags >> 8) & 0xFu) : 0u;
}

/* FUN_00408900 wrapper: visible => draw mode 2, hidden => draw mode 0. */
void doc_actor_set_visible(DocActor *a, int visible)
{
    doc_actor_set_draw_mode(a, visible ? 2u : 0u);
}

/* ---------------------------------------------------------------------
 * FUN_005a6db8 (list-active toggle). Original, decoded:
 *   if (active == 0) { rs &= ~0x100; if (parts) FUN_002dc7c8(parts, 0); }
 *   else            { rs |=  0x100; if (parts) FUN_002dc7c8(parts, 2); }
 *   FUN_005a6e28();                          // deferred-refresh (hook)
 * The model-parts show/hide call FUN_002dc7c8 and the refresh FUN_005a6e28
 * are below the current seam; the render_state bit is honoured here.
 * ------------------------------------------------------------------- */
void doc_actor_set_list_active(DocActor *a, int active)
{
    if (a == NULL) return;
    if (active)
        a->object.render_state |= DOC_ACTOR_RS_LIST_ACTIVE;
    else
        a->object.render_state &= ~DOC_ACTOR_RS_LIST_ACTIVE;
    /* HOOK: FUN_002dc7c8(model_parts, active ? 2 : 0) model show/hide. */
    /* HOOK: FUN_005a6e28() deferred visibility refresh. */
}

/* FUN_005ae618: store the hide-state byte (model-clip side effects hooked). */
void doc_actor_set_hide_state(DocActor *a, DocActorHideState state)
{
    if (a != NULL) a->hide_state = state;
    /* HOOK: FUN_002dc4b0(model_parts, ...) model-clip toggle on state 1/3. */
}

/* FUN_005ae540: store the pose/motion state byte (model side effects hooked). */
void doc_actor_set_state(DocActor *a, uint8_t state)
{
    if (a != NULL) a->pose_state = state;
    /* HOOK: FUN_002dc4b0 model-visibility flip on the 0xb state boundary. */
}

/* The draw gate the pose/render pass (FUN_005ae738/828) applies. */
int doc_actor_is_drawable(const DocActor *a)
{
    if (a == NULL) return 0;
    return (a->object.draw_flags & DOC_ACTOR_FLAG_OCCUPIED) != 0
        && (a->object.render_state & DOC_ACTOR_RS_POSE_READY) != 0
        && (a->object.draw_flags & DOC_ACTOR_FLAG_NO_MODEL) == 0
        && a->hide_state != DOC_ACTOR_HIDDEN;
}

/* ---------------------------------------------------------------------
 * Transparency. FUN_005a86a8 / FUN_005a86d0:
 *   +0x452 = transparency;  +0x459 = base_alpha;
 *   +0x45a = (transparency * base_alpha) >> 7;
 * ------------------------------------------------------------------- */
static void recompute_alpha(DocActor *a)
{
    a->alpha = (uint8_t)(((unsigned)a->transparency * (unsigned)a->base_alpha) >> 7);
}
void doc_actor_set_transparency(DocActor *a, uint8_t transparency)
{
    if (a == NULL) return;
    a->transparency = transparency;
    recompute_alpha(a);
}
void doc_actor_set_base_alpha(DocActor *a, uint8_t base_alpha)
{
    if (a == NULL) return;
    a->base_alpha = base_alpha;
    recompute_alpha(a);
}

/* ---------------------------------------------------------------------
 * Script flag field (+0x424).
 *   FUN_005a7338:  flags |=  (1 << (bit & 31));
 *   FUN_005a7370:  return 0 < (flags & (1 << (bit & 31)));
 * ------------------------------------------------------------------- */
void doc_actor_set_flag(DocActor *a, unsigned bit)
{
    if (a != NULL) a->script_flags |= (1u << (bit & 0x1Fu));
}
int doc_actor_test_flag(const DocActor *a, unsigned bit)
{
    return a != NULL && (a->script_flags & (1u << (bit & 0x1Fu))) != 0;
}

/* ---------------------------------------------------------------------
 * Attach graph.
 *   FUN_005a7698:  parent->attach[index] = child;
 *                  parent->attach_bone[index] = bone;
 *                  if (child) child->draw_flags |= 0x200000;   // ATTACHED
 *   FUN_005a7718:  return (index < 4) ? a->attach[index] : 0;
 * ------------------------------------------------------------------- */
void doc_actor_attach(DocActor *parent, int index, DocActor *child, uint8_t bone)
{
    if (parent == NULL || index < 0 || index >= DOC_ACTOR_MAX_ATTACH) return;
    parent->attach[index] = child;
    parent->attach_bone[index] = bone;
    if (child != NULL)
        child->object.draw_flags |= DOC_ACTOR_FLAG_ATTACHED;
}
DocActor *doc_actor_get_attached(const DocActor *a, int index)
{
    if (a == NULL || index < 0 || index >= DOC_ACTOR_MAX_ATTACH) return NULL;
    return a->attach[index];
}

/* ---------------------------------------------------------------------
 * HOOKS.
 * ------------------------------------------------------------------- */

/* KerberosActor.load / loadchr. Guest: FUN_005a8c40 allocates the object
 * and binds a model via FUN_002db5d8 (doc_model). Until doc_model exists,
 * we allocate + link the actor and flag it NO_MODEL. */
DocActor *doc_actor_load(DocActorManager *mgr, const char *model_name)
{
    DocActor *a = doc_actor_alloc(mgr);
    if (a == NULL) return NULL;
    a->name = model_name;
    a->object.draw_flags |= DOC_ACTOR_FLAG_NO_MODEL;   /* no geometry yet */
    doc_actor_list_push(mgr, a);
    /* HOOK: FUN_002db5d8 model bind -> sets object.model_parts, clears
     * NO_MODEL. Animation tables (+0x200) load here too. */
    return a;
}

/* KerberosActor.setmotion / loadmotion / startanim: select a motion row.
 * Playback (FUN_005ae828 sampling the +0x200 table) is a hook. */
void doc_actor_set_motion(DocActor *a, uint8_t motion_index)
{
    if (a != NULL) a->motion_index = motion_index;
    /* HOOK: FUN_005ae828 pose build / motion-table sampling. */
}
