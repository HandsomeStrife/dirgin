# Dirge of Cerberus (PS2) — FED / FEZ visual-effect format spec

Milestone P1 (effects). `.fed` files are the game's **particle / visual-effect
definitions**; `.fez` is the **compressed** variant. Magic **`FED\x08`**
(`46 45 44 08`). ~311 pathed instances live under
`extract/_KEL/data/effect/**/fer/*.fed`; ~396 more are noPath RFD payloads in
`extract/_KEL/noPath/*.bin` (first bytes `46 45 44 08`).

Reversed from the game's **own** EE parser in `analysis/decomp/` (read-only).
Every field cites the decompiled function that reads or relocates it. Claims
that rest on byte patterns across files rather than parser code are marked
**(empirical)**. Honest gaps are in §7.

Validated by a native C parser (`fed.c`, this directory) against **all 311
pathed files + 2 `.fez` + a noPath sample**: 0 parse failures, 2855 emitters,
24 407 particle definitions, 1 523 embedded textures — all bounds-consistent,
all durations in [11, 3500] frames, all textures valid TIM2 (§5).

---

## 0. Authority: the relocators

Two functions relocate a freshly-loaded FED in place; both encode the exact
container layout:

* **`FUN_002e4418`** — the definitive loader-time relocator (used by the live
  instantiate path). It gates on **`byte@+0x03 == 0x08`**, sets per-table
  "already-relocated" guard bits in the flags word (`+0x08`), and **bounds
  every stored offset to `< 0x500000`**. This is the authority used below.
* **`FUN_002e5550`** — sibling relocator, identical structure (called from the
  RFD/resident path via `FUN_002f0978`).
* **`FUN_002df140`** — the magic test: `(*(u32) & 0xffffff) == 0x444546` = "FED".
* **`FUN_002e4a58`** — instantiate: `byte@0 == 'F'`, allocates a **0xA4-byte**
  runtime object (`FUN_002f3068(..,0xa4,1)`), tags it `0x66`, stores the FED
  base at `obj+0x18` and **`*(fed+0x04)` (the FileCode) at `obj+0x14`**.
* Consumers that give fields meaning: **`FUN_002e63d0`** (emitter spawn),
  **`FUN_002eba88`** (texture table), **`FUN_002ea040`/`FUN_002e7660`**
  (per-frame particle update / channel apply).

### The addressing rule (critical)

From `FUN_002e4418`, every stored 32-bit offset is **relative to the base of
its containing record or table — NOT to the file**:

* the **six header table-offsets** (`+0x60…+0x74`) are relative to the **file**
  start (`param_2 + *(+0x60)` etc.);
* a **slot inside a table** gives a record at **table_base + slot**
  (`iVar9 = iVar7 + *slot`);
* a **pointer inside a record** gives a target at **record_base + ptr**
  (`*p = iVar9 + *p`).

A **zero slot** = an empty slot and is skipped (the loader guards every record
body with `if (slot != 0)`). This nested-relative scheme is why a flat
file-relative walk mis-reads the file; the native parser follows it exactly.

---

## 1. Header (first 0x78 bytes) — SOLVED

| off  | type | field | evidence |
|------|------|-------|----------|
| 0x00 | 3+1  | magic `FED` + version byte `0x08` | `FUN_002df140`; `FUN_002e4418` gates `byte@+3==0x08` |
| 0x04 | u32  | **FileCode / effect id** | `FUN_002e4a58` stores `*(fed+4)` into the runtime obj |
| 0x08 | u32  | **flags**: bit `0x10000`=valid, bit `0x8000`=compressed(FEZ). Bits `0x20000`–`0x800000` are **runtime reloc-guards** (0 on disk) | `FUN_002e5550` gates `&0x10000`; `FUN_002e4418` sets guard bits per table |
| 0x0c | u32  | **total size** (file size; for FEZ = *decompressed* size) | bytes: == filesize on every plain FED |
| 0x10 | u32  | constant `0x04040010` on FED (a VIF STCYCL-shaped tag). For FEZ: **byte@0x10 = compression type** (=2) | bytes |
| 0x14 | u32  | FEZ only: **compressed size** (== file size) | `FUN_002df4d8` returns it; `FUN_002df450` passes `size-0x20` to the decompressor |
| 0x18 | u32  | file-rel offset → table T18 (`u16 count@+4`, stride-0xc array @+8) | `FUN_002e4418` flag `0x800000` |
| 0x1c | u32  | file-rel offset (single relocated ptr) | `FUN_002e4418` (bounded `<0x500000`) |
| 0x20 | u32  | file-rel offset → record (`u16 count@+2`, ptr array @+8) | `FUN_002e4418` |
| 0x40 | u16  | count of **table A** | `FUN_002e4418` flag `0x20000` loops `u16@+0x40` |
| 0x42 | u16  | count of **emitter (MAIN) table** | flag `0x40000` loops `u16@+0x42` |
| 0x44 | u16  | count of **texture table** | flag `0x80000` loops `u16@+0x44` |
| 0x46 | u16  | count of table @0x6c | flag `0x100000` |
| 0x48 | u16  | count of table @0x70 | flag `0x200000` |
| 0x4a | u16  | count of table @0x74 | flag `0x400000` |
| 0x60 | u32  | file-rel offset → **table A** | flag `0x20000` |
| 0x64 | u32  | file-rel offset → **emitter table (MAIN)** | flag `0x40000` |
| 0x68 | u32  | file-rel offset → **texture table** (embedded TIM2) | flag `0x80000`; `FUN_002eba88` |
| 0x6c | u32  | file-rel offset → table @0x6c | flag `0x100000` |
| 0x70 | u32  | file-rel offset → table @0x70 | flag `0x200000` |
| 0x74 | u32  | file-rel offset → table @0x74 | flag `0x400000` |

---

## 2. Emitter table (header +0x64, count @0x42) — SOLVED

An array of u32 **slots** (relative to the table base). Slot → emitter record
`E = table_base + slot`. Field offsets within `E` are from the spawn consumer
**`FUN_002e63d0`** (reads `E` as a `u16*`) plus the relocator; all pointers
inside `E` are relative to `E`.

| E off | type | field | evidence |
|-------|------|-------|----------|
| 0x00 | u16  | emitter / particle **id** (copied to particle+0x04) | `FUN_002e63d0` |
| 0x04 | s32  | **duration** in frames (cast to float, compared to emitter age) | `FUN_002e63d0` |
| 0x08 | u16  | **modifier count** — entries in ptr array @+0x10 | `FUN_002e4418` (`u16@+8`) |
| 0x0a | u16  | **particle-definition count** — sub-records @+0x14 | `FUN_002e4418` (`u16@+0xa`) |
| 0x0c | u16  | **particle max-life / fade-start** (frames) | `FUN_002ea040` compares particle age to it |
| 0x0e | u8   | **render/orientation category** — `<5`→draw-cat 0; `5..8` or `==0x0b`→cat 1 (billboard); else 0 | `FUN_002e63d0` branch constants; corpus only ever shows {0,1,2,4,5,7,8,11} (§5) |
| 0x18 | u16  | **child-output count** — entries in u16 tag array @+0x20 | `FUN_002e63d0` |
| 0x1b | u8   | count of ptr array @+0x28 | `FUN_002e4418` (`byte@+0x1b`) |
| 0x1c | u8   | count of ptr array @+0x2c | `FUN_002e4418` (`byte@+0x1c`) |
| 0x1f | u8   | **death-fade mode**: 0→fade over 30f, 1→15f, 2→kill now | `FUN_002ea040:78–104` |
| 0x10 | u32  | → modifier ptr array (count @+0x08), rel E | relocator |
| 0x14 | u32  | → **particle sub-record** offset array (count @+0x0a), rel E | relocator |
| 0x20 | u32  | → **u16[child_count] child-effect tags** (filtered via `DAT_0046cab0`) | `FUN_002e63d0` |
| 0x24 | u32  | → **exactly 12 channel pointers** (rel E), each → a per-emitter animated parameter (scalar/colour curve) | relocator relocates a fixed 12 (`iVar1=0xb..0`) |
| 0x28 | u32  | → ptr array (count @+0x1b), rel E | relocator |
| 0x2c | u32  | → ptr array (count @+0x1c), rel E | relocator |

The consumer's category branch constants (`<5`, `5..8`, `0x0b`) exactly cover
the set of `E+0x0e` values found across the whole corpus — strong confirmation
of the offset.

### 2.1 Spawn timeline (`E+0x10`, count `E+0x08`) — SOLVED

The "modifier" array is the emitter's **spawn schedule**. It is a `u32[]` at
`E+0x10` (count `E+0x08`); each slot is an offset **relative to E** (not to the
array base — verified: `E+slot` yields `time=0, count=particle_count,
tags=[0,1,2,…]`). Each event record is `{ s16 time, s16 count, u16 tags[count] }`;
at frame `time` the emitter spawns the particle definitions named by the tags,
each `tag & 0x0fff` indexing the particle sub-record array at `E+0x14`
(`FUN_002ea040` → `FUN_002e8ea8`). Validated: **all 21 735** spawn tags across
the corpus index in range (`< particle_count`), 0 out of range.

### 2.2 Blend mode / render category — SOLVED (source); GS values PARTIAL

The GPU blend/orientation is driven by the **render category** — a small enum
at `byte@E+0x0e` (emitter) and `byte@P+0x0b` (per particle). At draw time
`FUN_002e84a8` maps the category to a blend state at `obj+0xc0`:
`cat 0 → FUN_002f0d60(1)`, `cat 5 → FUN_002f0d60(2)`, `cat 4 / 6 →
FUN_002f0d80(texIndex)` (the "animated-texture" preset), else
`FUN_002f0d60(0)`. `FUN_002f0d60` copies one of **three fixed 16-byte blend
presets** (`DAT_004ee9d0`, indices 0/1/2); `FUN_002e9db0` maps the same category
to the billboard/orientation matrix mode (cats 2/6/7/9 take matrix paths). So
the blend mode's **source is the category byte**; the literal GS `ALPHA`
register values live in boot-built `.bss` tables (`DAT_004ee9d0` /
`DAT_004e63b0`) and are not statically readable from the ELF (see §7).

---

## 3. Particle definition sub-record — PARTIALLY SOLVED

Sub-array at `E+0x14` (count `E+0x0a`). Each slot (rel **E**) → particle record
`P = E + slot`. The sub-structure below exists **only when `byte@P+0x08 != 0`**
— the loader guards the entire block on that byte (`FUN_002e4418`,
`if (*(char*)(P+8) != 0) { … }`).

| P off | type | field | evidence |
|-------|------|-------|----------|
| 0x08 | u8   | **curve/channel count** — entries in ptr array @+0x0c; also the master "has sub-structure" gate | `FUN_002e4418` |
| 0x0c | u32  | → ptr array of `P+0x08` curve/channel records (rel P) | relocator |
| 0x21 | u8   | count of ptr array @+0x34 | `FUN_002e4418` (`byte@+0x21`) |
| 0x30 | u32  | single ptr (rel P) — a distinct data block | relocator |
| 0x34 | u32  | → ptr array of `P+0x21` entries (rel P) | relocator |
| 0x38,0x3c,0x40,0x44,0x48,0x4c,0x50,0x54 | u32 ×8 | eight single pointers (rel P) — per-channel data (colour / alpha / scale / velocity over life). Observed to point into a small shared per-particle region, some zero-length channels sharing an end pointer (empirical) | relocator |

### 3.1 Animation channels — SOLVED (structure); type→semantic PARTIAL

A particle's animation is a set of **typed channels**. The meaning of a channel
is **not** its slot position — it is a *type code* that selects a runtime
evaluator. Three structures wire it up (all relative to **P**, authority
`FUN_002eb038` = spawn/init, `FUN_002eadc0` = per-frame update,
`FUN_002e92c8` = death):

* **Descriptor array @`P+0x0c`** (count `byte@P+0x08`): a pointer array; entry
  `idx` (rel to the array base) → a small descriptor with `u16@+0` = **type
  code**, `u16@+4` = **out-slot** (a vec4-aligned byte offset into the runtime
  particle's value region where the evaluated vec4 is written). ~60 distinct
  type codes occur across the corpus.
* **INIT list @`P+0x30`** (count `byte@+4` of that block): stride-8 entries
  `{u16 idx, u16 off}`; the channel's **constant / initial value** is a vec at
  `(P+0x30 block) + off`. Applied once at spawn.
* **UPDATE list @`P+0x3c`** (count `byte@P+0x23`): a `u16[]` of channel indices
  re-evaluated **every frame** over the particle's life (these are the animated
  curves).
* **AUX list @`P+0x34`** (count `byte@P+0x21`): a `u16[]` consumed on
  death/branch.

**Apply semantics (authoritative, `FUN_002e7660` + `FUN_002e7cd0`).** An
evaluated channel writes a vec4 tagged with a flag mask:

| flag | target | runtime op |
|------|--------|-----------|
| `0x1`  | **position** (particle+0x120→+0x80) | **added** to base |
| `0x2`  | **velocity** (+0x130→+0x90)         | **added** |
| `0x4`  | **accel / gravity** (+0x140)        | stored |
| `0x8`  | **colour RGBA** (+0x150→+0xb0, the GS colour) | stored |
| `0x10` | **orientation matrix** (+0xe0)      | matrix mul |
| `0x20` | **billboard byte** (+0xd4)          | stored |

Crucially `FUN_002e7cd0` **adds** position & velocity but **multiplies** the
scale vec4 (`_vmul` into +0xa0) — so a scale channel is a *factor*.

**Empirical channel values** (init vectors, cross-file): colour channels hold
RGBA in **0..128** (PS2 GS convention, 128 = 1.0), e.g. `[128,16,32,48]`;
uniform-scale channels are positive and often equal, e.g. `[600,600,600]` or
`[100,100,100]`; some channels are **integer-coded** (stored as raw ints that
read as tiny float denormals — texture indices / counts). Validation over the
whole corpus: 23 526 uniform-scale channels, **0 negative**; of colour-range
channels, 98.5 % have alpha in `[0,128]`. These ranges are self-consistent but
the per-type-code → exact-semantic table is **not pinned** (§7).

The scalar bytes in the `P` body below 0x30 (beyond the counts) are listed above
where known; the ten `P+0x30..0x54` pointers are the per-channel data blocks
(sub[0]=INIT table, sub[1]=AUX list, sub[3]=UPDATE list; the rest are the
runtime scratch bases set at spawn).

---

## 4. Texture table (header +0x68, count @0x44) — SOLVED

An array of u32 entries (relative to the table base). Each entry →
**a complete embedded `TIM2` image** (magic `TIM2`, standard PS2 TIM2 header).
So a FED's particle textures are **self-contained**, not external FileCodes,
and are decodable by the existing native TIM2 decoder
(`native/assets/textures/tim2.c`).

Decisive cross-check: each TIM2's `totalSize` (u32 @ blob+0x10) **exactly tiles
the table** — `blob_off + totalSize`, 16-byte aligned, lands on the next
entry — across every file with textures. Dimensions parsed from the TIM2
picture header (`width u16@+0x24`, `height u16@+0x26` of the blob) are all sane
powers of two (128×128, 64×64, 256×256, …). `FUN_002eba88` iterates this table
and resolves each image through the shared texture subsystem
(`FUN_00309f28(ctx+0x20, …)`), storing the handle at `runtimeObj+0x24 + i*4`.

> Resident/shared effects (e.g. `r0000`, `r0001`) carry **0** embedded textures
> (count@0x44 = 0) and draw from the globally-loaded texture set instead.

---

## 5. Validation (real files)

Native parser `./fed <file> [--json|--text]` (this directory). Ran over the
full pathed set + 2 `.fez` + a noPath sample:

| metric | value |
|--------|-------|
| files parsed (pathed) | 311 `.fed` + 2 `.fez`, **0 failures** |
| noPath FED sampled | 32, 0 failures |
| emitters parsed | 2 855 |
| particle definitions | 24 407 |
| embedded textures | 1 523, **100 % valid TIM2**, dims all in [1,1024]² |
| emitter duration (frames) | min 11, median 100, max 3500 — **0 negative, 0 absurd** |
| categories observed | {0,1,2,4,5,7,8,11} = exactly the consumer's branch set |
| fade modes observed | {0,1,2,3} |
| **FEZ inflated** | 2/2, bit-exact to `@0x0c` size, inflate to valid FED |
| **spawn tags in range** | 21 735 / 21 735 (0 out of range) |
| **channels decoded (with INIT)** | 133 787; uniform-scale 23 526 (**0 negative**); colour-range alphas in [0,128] 98.5 % |
| **JSON well-formed** | 313 / 313 files |

Sample dumps: `sample_a0269.json` (9 emitters, 303 particle defs, 8 TIM2
textures) and `sample_a0008_curves.json` (an emitter whose particles carry a
uniform-scale channel `[600,600,600]`, a colour channel `[128,16,32,48]`, an
int-coded texture channel, and a per-frame **animated** channel over a
300-frame life). JSON is well-formed.

`a0269.fed` — one emitter, abbreviated:
```
Emitter 0 @0x000730  id=0x0002 duration=160 frames  category=5(billboard?)
  max_life=0 fade_mode=3 particles=32 modifiers=1 children=9
  channels(12-array)=12 present, aux28=3 aux2c=0
Embedded textures:
  [2] @0x02af10  TIM2  256x256  imgtype=5  blob=66640 bytes   (tiles to [3]@0x3b370)
```

---

## 6. FEZ (compressed) — SOLVED (inflated natively)

`.fez` = a FED whose body is **zlib/DEFLATE-compressed and then lightly
encrypted**. Header is the same up to 0x18: flags bit `0x8000` set, `byte@0x10`
= compression type (**2**), `@0x14` = compressed size (== file size), `@0x0c` =
decompressed size. At 0x20 an 8-byte sub-header: `@0x20` = payload length
(compSize − 0x20), `@0x24` = decompressed size echoed. The encrypted stream
starts at **0x28**.

The pipeline (reversed from the game's own decompressor) is, to *read* one:
**decrypt the stream at `fed+0x28`, then zlib-inflate it.**

1. **Decrypt** — a fixed-key 8-byte-block cipher (`FUN_0023c418` →
   `FUN_0041c1d8` → `FUN_0041c0d8` → per-block `FUN_0041bf78`/`FUN_0041bf08`).
   * Seed is the **constant `7`** (`FUN_0023c3f0` fills it); there is no
     per-file key.
   * Key schedule (`FUN_0041bb38`): `key[0]` is derived from the seed with a
     byte-diffusion pass; `key[i] = key[i-1] * 5` (mod 2⁶⁴), 32 u64 words.
   * Each 8-byte block: a byte-substitution + chaining stage using the S-box
     `DAT_0047eb50[x] = (x + 0x78) & 0xFF`, then a 64-bit mix with the block's
     key word and a running-counter constant
     `(c | c<<30 | c<<20 | c<<10) + 0xA1652347` (counter `c` is 64-bit).
2. **Inflate** — the result is a standard zlib stream (`78 9C …`). `FUN_0042b810`
   is the textbook zlib `inflate()` state machine (method-8 check, CMF/FLG %31
   header check, "incorrect data check", etc.), i.e. plain DEFLATE.

Validated: both corpus `.fez` (`r0002`, `r0002_on`) decrypt + inflate to
**exactly** the size at `@0x0c` (377 920 / 468 976 bytes) and begin with
`FED\x08`; the inflated images parse like any FED (36→26 and 36→35 emitters, 0
embedded textures as expected for resident effects). Implemented in **`fez.c`**
(C + system zlib); `fed_load` inflates transparently so `./fed r0002.fez` works
directly. Only 2 `.fez` exist; all 311 gameplay effects are plain FED.

---

## 7. Open unknowns (honest gaps)

**Closed since the previous pass:** FEZ inflation (§6, implemented); the channel
container, the three role-lists and the INIT value blocks (§3.1); the spawn
timeline (§2.1); the apply/consume semantics — position/velocity/scale/colour/
orientation (§3.1); the blend-mode *source* (§2.2).

**Still open:**

1. **Per-type-code → exact curve semantic, and keyframe interpolation.** Each
   channel carries a *type code* (`u16@` its descriptor) that selects a runtime
   evaluator. The evaluators live in a C++ vtable table (`DAT_0046c398` →
   objects at `0x4e8a68+`) that is **constructed at boot in `.bss`**, so the
   type→evaluator mapping — and therefore the exact keyframe layout and
   step/linear/spline mode each type implements — is **not statically
   recoverable from the ELF**. What is recovered: the channel's type code,
   out-slot, role (init/update/aux), and INIT constant vector (all dumped), plus
   the authoritative *apply* side (§3.1). Pinning each type needs either tracing
   the boot constructors or a live/oracle run.
2. **Literal GS blend-register values.** The blend *source* is the category byte
   and the three-preset dispatch is known (§2.2), but the preset contents
   (`DAT_004ee9d0` / `DAT_004e63b0`, additive vs alpha etc.) are `.bss`,
   runtime-initialised — same wall as (1). Mapping cat→{additive,alpha,…} needs
   constructor tracing.
3. **Tables A (+0x60), +0x6c, +0x70, +0x74, +0x18, +0x20.** Relocated correctly
   (parser is bounds-safe over them) but their runtime purpose is unreversed.
4. **The 12 emitter-level channels `E+0x24`.** Relocated and exposed, but not
   decoded as the per-particle channels are (same typed-evaluator system).

---

## 8. Parser & exporter (this directory)

* `fed.h` / `fed.c` — buffer parser → in-memory `FedFile` (header, emitters,
  spawn timeline, particle defs, decoded animation channels, embedded-TIM2
  table). Nested-relative addressing per §0; every read bounds-checked; file
  treated as untrusted (never executed).
* `fez.h` / `fez.c` — FEZ inflation (the §6 cipher + system zlib). `fed_load`
  calls it transparently, so `.fez` parses like `.fed`.
* `fed_dump.c` — text and JSON dumps (channels with type/out-slot/role/init,
  spawn events, render category).
* `main.c`, `Makefile`. Build: `make` (C11, `-Wall -Wextra -Wpedantic`, `-lz`,
  clean). Run: `./fed <file.fed|.fez> [--json|--text]`. `make check` parses a
  sample.
