/* =====================================================================
 * actor.h  --  Reauthored ACTOR / ENTITY subsystem (purist port, P4).
 *
 * An "actor" is the central positioned game entity: the thing scripts
 * load, place, orient, make (in)visible and animate. In the guest it is a
 * single fixed-size block (0x480 bytes) carved from a contiguous pool; the
 * same block is also the renderable SCENE OBJECT (it is linked into the
 * object-list that object_list.c walks, carries the +0x3e4 model-parts
 * pointer, the +0x428 render_state and the +0x440 draw_flags that
 * scene_graph.c already models). So an actor OWNS a DocSceneObject: this
 * module embeds one and drives it through the scene_graph.{h,c} API.
 *
 * This is the engine side of the KerberosActor.* script bridge
 * (native/script/engine_bridge_api.txt, the most-used bridge): load,
 * getpos/setpos, set/getdir, setrot, setqt, setVisible / hide_model,
 * setTransparency, attach, setmotion. The JVM native-method REGISTRY that
 * binds the literal "fake/KerberosActor.setpos" string to the engine entry
 * is a documented engine GAP (engine-map.md section 3) and is NOT reversed
 * here; we model what each method reads/writes on the entity, not the
 * dispatch address.
 *
 * Reauthored from (analysis/decomp/, read-only oracle):
 *   -- the actor/object POOL (manager) --
 *   FUN_005a71b8  register pool + clear all occupied bits -> doc_actor_manager_init
 *   FUN_005a7208  find first free slot (scan occupied bit) -> doc_actor_alloc
 *   FUN_005a7148  return a slot to the pool (clear bit)    -> doc_actor_free
 *   FUN_005a7d88  unlink from active list + detach + free  -> doc_actor_destroy
 *   FUN_005a7898  walk active list to tail                 -> doc_actor_list_tail
 *   -- per-actor transform / spatial state --
 *   FUN_005a7008  set position vector (vec4 @ +0x30)       -> doc_actor_set_position
 *   FUN_005a70c8  get position vector (vec4 @ +0x30)       -> doc_actor_get_position
 *   FUN_005a7288  set orientation vector (vec4 @ +0xe0)    -> doc_actor_set_orientation
 *   FUN_005a7298  get orientation vector (vec4 @ +0xe0)    -> doc_actor_get_orientation
 *   -- visibility / draw state (backs setVisible, hide_model, changeDrawOrder) --
 *   FUN_005a85f8  store draw mode + propagate (ALREADY     -> doc_scene_object_set_draw_mode
 *                 ported in scene_graph.c)                    (reused)
 *   FUN_005a8658  read draw mode (draw_flags bits 8..11)   -> doc_actor_draw_mode
 *   FUN_005a6db8  list-active toggle + model show/hide     -> doc_actor_set_list_active
 *   FUN_005ae618  hide-state byte (+0x457) + model clip    -> doc_actor_set_hide_state
 *   FUN_005ae540  pose/motion state byte (+0x454)          -> doc_actor_set_state
 *   -- misc per-actor script state --
 *   FUN_005a7338  set a bit in the script flag field +0x424-> doc_actor_set_flag
 *   FUN_005a7370  test a bit in the script flag field      -> doc_actor_test_flag
 *   FUN_005a86a8  setTransparency (alpha @ +0x452)         -> doc_actor_set_transparency
 *   FUN_005a86d0  set base alpha (+0x459)                  -> doc_actor_set_base_alpha
 *   FUN_005a7698  attach a child actor (slot @ +0x3e8[i])  -> doc_actor_attach
 *   FUN_005a7718  read an attach slot                      -> doc_actor_get_attached
 *
 * Guest struct offsets are recorded against every field so the mapping is
 * auditable and a byte-accurate transcription can be diffed against these
 * clean structs (tests/test_actor.c).
 *
 * BELOW THE SEAM / HOOKS (documented, not ported this pass):
 *   - model LOAD + bind: FUN_005a8c40 allocates an object and binds a model
 *     via FUN_002db5d8 (doc_model loader). doc_actor_bind_model is the hook.
 *   - ANIMATION / motion playback: the motion-state table at +0x200 (0x30
 *     stride, indexed by the +0x448 byte), the pose build FUN_005ae828 /
 *     FUN_005ae738 and the skeletal transform feed FUN_002db1a0. The world
 *     matrix build from position+orientation is provided here as a clean
 *     dmath TRS helper; the guest VU0 build (FUN_005a7070) is below the seam.
 *   - AI / physics / collision: not in this cluster.
 * ===================================================================== */
#ifndef DOC_ACTOR_H
#define DOC_ACTOR_H

#include <stdint.h>
#include "dmath.h"
#include "scene_graph.h"   /* DocSceneObject, doc_scene_object_set_draw_mode */

#ifdef __cplusplus
extern "C" {
#endif

/* Each guest actor/object block is this many bytes; the pool is a
 * contiguous array of them (FUN_005a7208 strides by 0x480, FUN_005a71b8
 * divides the region size by 0x480 to get the slot count). */
#define DOC_ACTOR_BLOCK_SIZE 0x480

/* draw_flags (u64 @ +0x440) bits this module relies on.
 *   0x00000400000  slot OCCUPIED  (set on init, cleared on free; the pool
 *                  allocator FUN_005a7208 returns the first slot WITHOUT it)
 *   0x00000100000  "placeholder / no model" (set when the object has no
 *                  bound model, FUN_005a8c40 / FUN_005a8588)
 *   0x00000200000  this object is ATTACHED to a parent (FUN_005a7698)
 * Bits 8..11 are the 4-bit draw mode (scene_graph.c owns that field). */
#define DOC_ACTOR_FLAG_OCCUPIED     0x0000400000ull
#define DOC_ACTOR_FLAG_NO_MODEL     0x0000100000ull
#define DOC_ACTOR_FLAG_ATTACHED     0x0000200000ull

/* render_state (u32 @ +0x428) bits.
 *   bit 8 (0x100)   list-active: object_list.c's DOC_OBJ_LIST_ACTIVE result,
 *                   set/cleared by FUN_005a6db8 (setVisible path).
 *   bit 3 (0x08)    "has a pose to build" gate tested by FUN_005ae738/828. */
#define DOC_ACTOR_RS_LIST_ACTIVE    0x00000100u
#define DOC_ACTOR_RS_POSE_READY     0x00000008u

/* hide-state byte (+0x457), set by FUN_005ae618. The pose/render pass
 * (FUN_005ae738/828) skips the actor when this equals 1. */
typedef enum {
    DOC_ACTOR_SHOW   = 0,   /* drawn normally                       */
    DOC_ACTOR_HIDDEN = 1,   /* skipped by the pose/render pass      */
    DOC_ACTOR_HIDE2  = 2,
    DOC_ACTOR_HIDE3  = 3
} DocActorHideState;

/* Max child actors that can be attached to one actor. In the guest the
 * attach array at +0x3e8 is 4 slots (FUN_005a7718 bounds the index < 4),
 * each paired with a bone/joint id byte at +0x45e. */
#define DOC_ACTOR_MAX_ATTACH 4

/* Engine-wide fixed-point angle unit: a full turn is 4096 units (the same
 * convention the camera uses, see camera.h; KerberosActor.getdir/setdir
 * speak these integer units). */
#define DOC_ACTOR_ANGLE_UNITS_PER_TURN 4096.0f

/* ---------------------------------------------------------------------
 * The actor / entity.
 *
 * Guest layout (the 0x480 block; param_1 of the FUN_005a7xxx accessors):
 *   +0x000 int32  next           intrusive active-list next  (FUN_005a7d88)
 *   +0x004 int32  prev           intrusive active-list prev  (FUN_005a7d88)
 *   +0x030 float4 position       getpos/setpos (FUN_005a70c8 / FUN_005a7008)
 *   +0x0e0 float4 orientation    setrot vector (FUN_005a7298 / FUN_005a7288)
 *   +0x090 float4 move_from      interp/move endpoints (FUN_005a72e8/7300)
 *   +0x0a0 float4 move_to        "
 *   +0x120 float  local[16]      world transform matrix (init-zeroed;
 *                                built below the seam, FUN_005a7070)
 *   +0x3e4 int32  model_parts    DocModelParts* (bind = hook, FUN_005a8c40)
 *   +0x3e8 int32  attach[4]      attached child actors (FUN_005a7698/7718)
 *   +0x420 int32  user_scalar_b  (FUN_005a86c8)
 *   +0x424 uint32 script_flags   generic bit field (FUN_005a7338 / 7370)
 *   +0x428 uint32 render_state   (bit 8 list-active, bit 3 pose-ready)
 *   +0x440 uint64 draw_flags     (bits 8..11 mode, 0x400000 occupied, ...)
 *   +0x448 uint8  motion_index   selects a motion-state row @ +0x200
 *   +0x452 uint8  transparency   (FUN_005a86a8)
 *   +0x454 uint8  pose_state     motion/pose state 0..0xb (FUN_005ae540)
 *   +0x457 uint8  hide_state     DocActorHideState (FUN_005ae618)
 *   +0x459 uint8  base_alpha     (FUN_005a86d0)
 *   +0x45a uint8  alpha          computed = transparency*base_alpha >> 7
 *   +0x45e uint8  attach_bone[4] joint id per attach slot (FUN_005a7698)
 *
 * We keep host-width pointers/enums and drop the fields this cluster does
 * not touch; the offsets live in comments and in the oracle-parity
 * reference, not in the struct packing (DECOMP-METHOD.md section 3).
 * ------------------------------------------------------------------- */
typedef struct DocActor {
    /* ---- renderable half: the actor IS a scene object -------------- *
     * Carries model_parts (+0x3e4), render_state (+0x428) and draw_flags
     * (+0x440). Visibility/draw-mode changes go through the already-
     * reauthored doc_scene_object_set_draw_mode(). The pool's occupied
     * bit lives in object.draw_flags (DOC_ACTOR_FLAG_OCCUPIED). */
    DocSceneObject object;

    /* ---- spatial state the scripts drive --------------------------- */
    dvec3   position;      /* <- float4 @ +0x30                          */
    dvec3   orientation;   /* <- float4 @ +0xe0 (euler radians / dir)    */
    dvec3   move_from;     /* <- float4 @ +0x90 (interp start; hook)     */
    dvec3   move_to;       /* <- float4 @ +0xa0 (interp end; hook)       */

    /* ---- per-actor script/display state ---------------------------- */
    uint32_t          script_flags;  /* <- u32  @ +0x424                 */
    uint8_t           motion_index;  /* <- u8   @ +0x448 (motion-row sel)*/
    uint8_t           transparency;  /* <- u8   @ +0x452                 */
    uint8_t           base_alpha;    /* <- u8   @ +0x459                 */
    uint8_t           alpha;         /* <- u8   @ +0x45a (derived)       */
    uint8_t           pose_state;    /* <- u8   @ +0x454                 */
    DocActorHideState hide_state;    /* <- u8   @ +0x457                 */

    /* ---- attach graph ---------------------------------------------- */
    struct DocActor *attach[DOC_ACTOR_MAX_ATTACH];   /* <- int @ +0x3e8  */
    uint8_t          attach_bone[DOC_ACTOR_MAX_ATTACH]; /* <- u8 @ +0x45e */

    /* ---- manager bookkeeping (intrusive active list @ +0x00/+0x04) - */
    struct DocActor *next;
    struct DocActor *prev;

    const char *name;      /* debug label (not in guest)                 */
} DocActor;

/* ---------------------------------------------------------------------
 * The actor manager: the fixed pool the entities are carved from.
 *
 * Guest state (module globals the pool functions read):
 *   DAT_005b37a0  pool base pointer   (FUN_005a71b8 sets it)
 *   DAT_005b37a4  pool slot count     (= region size / 0x480)
 *   DAT_005b3788  head of the ACTIVE object list (object_list.c's head)
 * The clean port holds the slot array and the active-list head directly.
 * ------------------------------------------------------------------- */
typedef struct DocActorManager {
    DocActor *slots;       /* contiguous pool (<- DAT_005b37a0)          */
    int       capacity;    /* number of slots   (<- DAT_005b37a4)        */
    DocActor *active_head; /* head of the active list (<- DAT_005b3788)  */
    int       live_count;  /* occupied slots (observability; not guest)  */
} DocActorManager;

/* ---- manager: pool lifecycle ---------------------------------------- */

/* FUN_005a71b8: register `slots[0..capacity-1]` as the pool and clear the
 * OCCUPIED bit on every slot (mark all free). */
void doc_actor_manager_init(DocActorManager *mgr, DocActor *slots, int capacity);

/* FUN_005a7208: return the first slot whose OCCUPIED bit is clear, mark it
 * occupied, and run the per-actor reset (FUN_005a7978 defaults). Returns
 * NULL when the pool is full. The returned actor is NOT yet on the active
 * list (load/spawn links it). */
DocActor *doc_actor_alloc(DocActorManager *mgr);

/* FUN_005a7148: return a slot to the pool by clearing its OCCUPIED bit.
 * A pointer outside the pool range is ignored here (the guest hands it to
 * the general allocator free; that path is a documented hook). */
void doc_actor_free(DocActorManager *mgr, DocActor *a);

/* FUN_005a7d88: unlink `a` from the active list, detach it from any parent
 * that holds it in an attach slot, clear its OCCUPIED bit, and (when
 * `release` != 0) return it to the pool. This is KerberosActor.unload. */
void doc_actor_destroy(DocActorManager *mgr, DocActor *a, int release);

/* Link a freshly-allocated actor at the head of the active list. */
void doc_actor_list_push(DocActorManager *mgr, DocActor *a);

/* FUN_005a7898: walk the active list to its last element (NULL if empty). */
DocActor *doc_actor_list_tail(const DocActorManager *mgr);

/* ---- per-actor reset (the FUN_005a7978 defaults, clean subset) ------- */
void doc_actor_reset(DocActor *a);

/* ---- transform: position & orientation ------------------------------ */

/* FUN_005a7008 / FUN_005a70c8: set / get the position vector (+0x30). */
void  doc_actor_set_position(DocActor *a, dvec3 pos);
dvec3 doc_actor_get_position(const DocActor *a);

/* FUN_005a7288 / FUN_005a7298: set / get the orientation vector (+0xe0),
 * euler angles in radians (KerberosActor.setrot). */
void  doc_actor_set_orientation(DocActor *a, dvec3 euler_radians);
dvec3 doc_actor_get_orientation(const DocActor *a);

/* KerberosActor.setdir/getdir: the script-facing heading is a fixed-point
 * integer (4096 == one turn) about the Y (up) axis. These convert to/from
 * the stored orientation.y. */
void doc_actor_set_dir(DocActor *a, int dir_units);
int  doc_actor_get_dir(const DocActor *a);

/* KerberosActor.setqt:(FFFF): set orientation from a quaternion. Composed
 * into the stored euler representation (the guest's exact quaternion slot
 * was not isolated this pass; this is a faithful host-side conversion so
 * the bridge method has a real backing). */
void doc_actor_set_quaternion(DocActor *a, dquat q);

/* Build the actor's world transform (translate(position) * R(orientation))
 * as a clean column-major dmat4. The guest assembles an equivalent matrix
 * into the +0x120 region with VU0 ops (FUN_005a7070, below the seam); this
 * is the portable reauthor of that transform. */
dmat4 doc_actor_world_matrix(const DocActor *a);

/* ---- visibility / draw state ---------------------------------------- */

/* KerberosActor.setVisible / changeDrawOrder: store a 4-bit draw mode and
 * propagate it to the bound model parts. Delegates to the already-ported
 * doc_scene_object_set_draw_mode (FUN_005a85f8). mode 0 hides. */
void doc_actor_set_draw_mode(DocActor *a, unsigned mode);

/* FUN_005a8658: read the stored 4-bit draw mode (draw_flags bits 8..11). */
unsigned doc_actor_draw_mode(const DocActor *a);

/* Convenience: setVisible(true/false) == draw mode 2 (list) / 0 (hidden),
 * matching the FUN_00408900 wrapper's "visible => mode 2, else hide". */
void doc_actor_set_visible(DocActor *a, int visible);

/* FUN_005a6db8: the list-active toggle. Sets/clears render_state bit 8 and
 * shows/hides the bound model parts (the model call FUN_002dc7c8 is a
 * documented hook; the render_state bit is honoured here). */
void doc_actor_set_list_active(DocActor *a, int active);

/* FUN_005ae618: set the hide-state byte (+0x457). The model-clip side
 * effect (FUN_002dc4b0) is a hook; the state byte is stored faithfully. */
void doc_actor_set_hide_state(DocActor *a, DocActorHideState state);

/* FUN_005ae540: set the pose/motion state byte (+0x454). Values 0..0xb;
 * the model-visibility side effects on the 0xb boundary are hooks. */
void doc_actor_set_state(DocActor *a, uint8_t state);

/* The pose/render pass (FUN_005ae738/828) draws an actor only when it is
 * occupied, has render_state bit 3 (pose-ready), is not NO_MODEL, and its
 * hide_state is not HIDDEN. Exposed so the (hooked) animation pass and
 * tests can ask the same question the guest does. */
int doc_actor_is_drawable(const DocActor *a);

/* ---- transparency --------------------------------------------------- */

/* FUN_005a86a8 / FUN_005a86d0: set transparency / base alpha. Both recompute
 * the effective alpha = (transparency * base_alpha) >> 7. */
void doc_actor_set_transparency(DocActor *a, uint8_t transparency);
void doc_actor_set_base_alpha(DocActor *a, uint8_t base_alpha);

/* ---- script flag field (+0x424) ------------------------------------- */
void doc_actor_set_flag(DocActor *a, unsigned bit);   /* FUN_005a7338 */
int  doc_actor_test_flag(const DocActor *a, unsigned bit); /* FUN_005a7370 */

/* ---- attach graph --------------------------------------------------- */

/* FUN_005a7698: attach `child` into `parent`'s slot `index` (< 4) at bone
 * `bone`. A non-NULL child is marked ATTACHED (draw_flags bit 0x200000). */
void doc_actor_attach(DocActor *parent, int index, DocActor *child, uint8_t bone);

/* FUN_005a7718: read attach slot `index` (NULL if empty or out of range). */
DocActor *doc_actor_get_attached(const DocActor *a, int index);

/* ---- BELOW-THE-SEAM HOOKS ------------------------------------------- *
 * These stand in for subsystems not ported this pass. They are explicit
 * (never silent no-ops that masquerade as real behaviour) and record what
 * the guest does. A host integrator wires real implementations in. */

/* KerberosActor.load / loadchr: allocate an actor and bind a model by name.
 * The guest path is FUN_005a8c40 -> FUN_002db5d8 (doc_model loader). This
 * hook allocates + resets the actor and flags it NO_MODEL (no geometry yet);
 * doc_model is a separate milestone. Returns NULL if the pool is full. */
DocActor *doc_actor_load(DocActorManager *mgr, const char *model_name);

/* KerberosActor.setmotion / loadmotion / startanim: select a motion row.
 * Animation playback (sampling the motion table at +0x200, FUN_005ae828)
 * is a hook; this stores the motion index so the (future) pass can read it. */
void doc_actor_set_motion(DocActor *a, uint8_t motion_index);

#ifdef __cplusplus
}
#endif
#endif /* DOC_ACTOR_H */
