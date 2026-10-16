# Reauthoring method: Ghidra decomp → clean native C

**Purist port, milestone P4.** How to turn an unreadable decompiled EE
function (`analysis/decomp/<addr>_<name>.c`) into clean, portable, named
C in `native/engine/`. This is the process; the scene-graph cluster in
`scene/` is the first worked example, built exactly this way.

This is *reauthoring*, not recompilation and not emulation. We keep the
game's **logic** and throw away its **machine shape** (raw offsets, EE/VU0
intrinsics, `DAT_` globals, Ghidra names). The result is ordinary C that a
human can read and a modern compiler can target for Linux or Windows.

---

## 0. Sources of truth (what you read, what you must not touch)

| Source | Role | Mutable? |
|---|---|---|
| `analysis/decomp/<addr>_<name>.c` | The authoritative logic. 13,089 Ghidra funcs. | read-only |
| `analysis/funcs.csv`, `notes/engine-map.md` | Where things live, named clusters + addresses. | read-only |
| `port/` (recomp build) + `port/cop2/vu0_macro.c` | **Oracle**: real runtime behaviour + VU0 op semantics. | read-only |
| `native/engine/` | Your output. | **yours** |

Never edit `analysis/decomp/`, `port/`, or the sibling `native/assets|renderer`.
The oracle is a *reference*, not gospel — see §6 (it has at least one bug).

---

## 1. Pick a coherent cluster, not a lone function

Reauthor a **subsystem that calls into itself** and bottoms out at a clean
seam, so it compiles and tests as a unit. Use `engine-map.md` to find the
chokepoints; follow the call graph with `grep -rl FUN_xxxx analysis/decomp`
until you have a closed set plus a short list of *leaf* calls into
not-yet-ported subsystems (those become stubs). A cluster of 3–10 functions
is a good brick.

## 2. Infer real names from behaviour

Names come from evidence, in this order:
1. **engine-map.md** already names many clusters and their job.
2. **Strings / constants** in the function or its callees (format strings,
   FourCCs, magic numbers).
3. **Behaviour**: what fields it reads/writes, what it loops over, what it
   returns. A function that walks an 8-slot null-terminated array and sets a
   bit on leaves is a *tree flag-setter*, not `FUN_002ebee8`.
4. **Sibling functions**: near-identical funcs that differ in one constant
   reveal which constants are parameters (e.g. the render-queue bit).

Prefix everything with the module: `doc_` for engine objects, `vu0_` for the
VU0 math vocabulary, `dmath`/`dvec`/`dmat4` for general math. One module =
one noun family.

## 3. Replace raw offsets with named structs

Every `*(T *)(base + 0xNN)` is a struct field. Build the struct from the
*union of all accesses across the cluster and its siblings*, and record the
guest offset against each field in a comment so the map stays auditable:

```c
typedef struct DocModelNode {
    DocNodeKind  kind;         /* <- tag byte @ +0x00 ('l' => group) */
    uint16_t     queue_flags;  /* <- u16  @ +0x0a (leaf draw bits)   */
    uint16_t     child_count;  /* <- s16  @ +0x04 (group)            */
    struct DocModelNode **children; /* <- ptr array @ +0x30 (group)  */
    ...
} DocModelNode;
```

You are **not** preserving guest layout (this isn't a recomp). You may use
host-width pointers, enums, and a count+pointer list instead of a fixed
guest array — as long as the *logic* is identical. The offsets live in
comments and in the oracle-parity reference (§5), not in the struct packing.

Cross-check the field meaning against sibling funcs: if `FUN_002dbfc8`
passes bit 4 and `FUN_002dc158` passes bit 10 into the same walker over the
same `+0x1c` array, then `+0x0a` is a *per-render-queue bitfield* and the bit
index is a parameter — not a magic number to hardcode.

## 4. Replace EE/VU0 intrinsics with the portable math lib

Inline COP2/VU0 macro SIMD (`_vmul`, `_vmadd`, `_vdiv`, `_vftoi`, `_lqc2`…)
and hand-rolled matrix math become calls into `math/`:

- General math → `math/dmath.h` (`dvec3`, `dmat4`, `dquat`, column-major RH).
- VU0 vector ops → `math/vu0.h`, which re-expresses the oracle's
  `ee_cop2_*` routines as named functions taking plain `float[4]` + an
  explicit **dest mask** and **broadcast/lane** argument, instead of a
  decoded 32-bit instruction word. The semantics that geometry depends on
  (dest mask, broadcast, accumulator MADD, `ITOF4 = /16` dequant, clip) are
  reproduced exactly; the PS2 non-IEEE float quirks are **not** (§ Floats).

When you see inline packet/DMA/GIF emission, that is *below the seam* — do
not port it; stub it and leave the queue-flag/scene-graph logic above it.

## 5. Replace `DAT_` globals with named module state

A `DAT_0046xxxx` that a cluster reads/writes is module state: make it a
`static` in the module's `.c` with a real name, exposed by accessor if other
modules need it. A `DAT_` that is a *context handle* threaded through calls
(e.g. the render-queue context `0x4ee2e0`) becomes a passed-in `struct *`
(here `DocRenderQueue *`), even if this particular cluster never dereferences
it — that keeps the door open for the subsystem that will.

## 6. Validate against the oracle — "same output on sample inputs"

Two layers, both required:

**(a) Oracle-parity reference.** Alongside the clean reauthor, write a
*byte-accurate transcription* of the decompiled originals that reads the
documented offsets out of a raw `uint8_t` "guest memory" buffer (32-bit
buffer-relative pointers, exactly as the decomp dereferences ints). Drive
both with the same synthetic input and assert identical results. This proves
the **logic AND the offset map** at once. `tests/test_scene_graph.c` does
this: build the same tree as clean structs and as a raw buffer, run the
public entry on each, diff every leaf's flag word.

**(b) Mutation.** Per project rule, a passing test means nothing until
watched fail. Break the reauthored op (flip a `|=` to `&=`), rebuild, confirm
the test goes red, restore. Done for the scene graph (16 asserts go red,
incl. the parity diff) and for the math lib (the `opmsub` cross-product
assert).

**The oracle is not infallible.** Validation found that
`port/cop2/vu0_macro.c`'s `ee_cop2_vopmsub` subtracts the *same* products
`VOPMULA` added, yielding **zero** instead of a cross product — a latent
oracle bug. The reauthored `vu0_opmsub` implements correct hardware
semantics (`opmula`+`opmsub` == cross) and is deliberately **not**
bit-compatible with that routine. When the clean version and the oracle
disagree, decide which is right from hardware docs / behaviour; don't blindly
match the oracle. (Flagged for the oracle to be fixed.)

> UPDATE 2026-10-15: the oracle bug is now **fixed** (`ee_cop2_vopmsub` subtracts
> the swapped products; regression test `port/cop2/test_vu0_cross.c`). Oracle and
> reauthored `vu0_opmsub` now agree, so this is no longer a standing divergence —
> it stays on record as the worked example of "the oracle is not infallible."

For functions with real runtime behaviour (geometry, state machines), the
stronger oracle is the booting recomp build in `port/`: dump its inputs and
outputs for a known case (a model, a frame) and assert the reauthor
reproduces them. That is the next escalation when a synthetic tree isn't
enough.

## Floats

The native engine uses ordinary IEEE-754. The PS2's non-IEEE floats (no
NaN/Inf, overflow saturates to ±FLT_MAX, denormals flush to signed zero)
are reproduced **only** in the oracle (`port/cop2/vu0_macro.c`,
`ee_runtime.h`) and used for bit-exact validation. Purist-port logic that
is visually/behaviourally equivalent does not need bit-exact floats;
flag it explicitly if a subsystem turns out to depend on the quirks.

## Build & test discipline

- Everything lands under `native/engine/`, built by `CMakeLists.txt`
  (portable; Linux now, Windows later; MSVC `/W4`, else `-Wall -Wextra`).
- Must build warning-clean under `gcc -Wall -Wextra -pedantic -std=c11`.
- Every cluster ships a unit test wired into `ctest`, including an
  oracle-parity check where the logic is non-trivially mappable.
- Stub leaves (calls into unported subsystems) are explicit and documented,
  never silent no-ops that look like real behaviour.

---

## Worked example: scene-graph / model-node cluster → `scene/`

| Original (addr) | Reauthored name | Job |
|---|---|---|
| `FUN_005a85f8` | `doc_scene_object_set_draw_mode` | per-object draw entry: store 4-bit draw mode, propagate show/hide |
| `FUN_002dbfc8` | `doc_model_parts_set_queue_flag` / `..._flag_main` | fan a flag change across up-to-8 root nodes |
| `FUN_002ebee8` | `doc_model_node_set_queue_flag` | recursive model-node tree walk; set/clear a render-queue bit on leaves |
| `FUN_002dc158` (sibling) | (same walker, bit 10) | confirmed `queue_flags` is a per-context bitfield |
| `FUN_00408900` (sibling) | — | confirmed `SceneObject+0x3e4` = model-parts ptr |

Structs recovered: `DocSceneObject`, `DocModelParts`, `DocModelNode`,
`DocMesh` (leaf geometry, **stub**), `DocRenderQueue` (the `0x4ee2e0`
context). Full offset map in `scene/scene_graph.h`.
