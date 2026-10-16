# `port/oracle/` — reusable executable-oracle harness

Use the statically-recompiled EE build (`port/recomp/generated_all` + `hle/` +
`sif/` + `cop2/` + `gs/` + `vu/` + `input/` + `src/kel.c`) as an **executable
oracle**: create a guest machine, inject known bytes into guest RAM, **call any
recompiled `func_<addr>(ee_state*)`**, and read the result back out. This is the
ground-truth reference the native `native/engine/` decomposition is validated
against.

Everything here lives in this directory (plus `gen/`, see below). **No file
outside `port/oracle/` is modified** — the stock `port/` sources and the stock
`recomp/generated_all/*.o` are linked as-is.

---

## What it is

| file | role |
|------|------|
| `oracle.h` / `oracle.c` | the library: machine create, memory inject/read, arena alloc, register/arg setup, `oracle_call()`, hex + mat4-JSON dump, SIGSEGV fault guard |
| `oracle_cli.c` → `oracle-cli` | **general** command-line driver: set up memory, call one func, dump regions |
| `oracle_test.c` → `oracle-test` | self-test: proves real recompiled code runs (memset + 4×4·vec4 VU0 math) |
| `oracle_bones.c` → `oracle-bones` | the km01vnw MOD bone-matrix dump attempt (see "Bone-matrix result") |
| `tools/recompile_keldraw.py` | segment-aware copy of the stock recompiler (see "The overlay fix") |
| `gen/eetu_0038.c`, `gen/eetu_0039.c` | the two re-translated overlay TUs this harness links (real bodies for the character/skin subsystem) |
| `out/km01vnw_bones.json` | honest status output of `oracle-bones` |

Build & run:

```sh
source /home/dan/.config/dirge-port.env      # (only needed to regenerate gen/)
cd port/oracle
make            # builds oracle-cli + oracle-bones
make test       # builds + runs the self-test  (expect ALL SELF-TESTS PASS)
make bones      # builds + runs the km01vnw dump attempt
```

Prerequisite: the stock EE objects must exist
(`cd ../recomp && make build-all`); they already do.

---

## General oracle-call usage (`oracle-cli`)

```
oracle-cli [options] --call 0xADDR
```

| option | meaning |
|--------|---------|
| `--elf PATH` | override the EE ELF (default: the game's `SLES_541.85`) |
| `--io` | open `FILELIST.BIN`/`KEL.DAT` + prime the SIF handshake (only for funcs that do file I/O) |
| `--sp 0xADDR` | set the guest stack pointer (default: high-RAM top) |
| `--write ADDR:FILE` | load a host file into guest RAM at ADDR |
| `--write ADDR=HHHH…` | write literal hex bytes at ADDR |
| `--alloc-file FILE` | `oracle_alloc` + load FILE; prints the chosen guest addr |
| `--a0 V … --a3 V`, `--t0 … --t3` | set argument registers (hex or decimal) |
| `--arg N:V` | set arg N (0‑3 → `$a0‑$a3`, 4‑7 → `$t0‑$t3`, ≥8 → stack `$sp+(N‑8)*4`) |
| `--reg IDX:V` | set GPR by `R_` index (0‑31) |
| `--dump ADDR:LEN` | hex-dump LEN bytes at ADDR after the call |
| `--dump-mat4 ADDR:N[:row\|col]` | dump N consecutive 0x40-byte blocks as 4×4-matrix JSON |

Float arguments go in `$f12…` — set them via the library
(`oracle_ee(o)->f[12] = <bits>`) or a small custom driver; the CLI covers the
integer/pointer ABI, which is what parse/transform funcs use.

**Worked example — the engine's 4×4·vec4 (`FUN_0026138c`), the CPU reference for
the skin transform.** Column-major matrix = identity with translation (5,6,7);
vector = (2,3,4,1):

```sh
# 16 floats, column-major: identity basis + col3 = translate(5,6,7)
MAT=0000803f000000000000000000000000000000000000803f000000000000000000000000000000000000803f000000000000a0400000c0400000e0400000803f
VEC=0000004000004040000080400000803f   # vec4 (2,3,4,1)
./oracle-cli --write 0x900040=$MAT --write 0x900080=$VEC \
             --a0 0x900000 --a1 0x900040 --a2 0x900080 \
             --call 0x0026138c --dump 0x900000:16
# -> 0000 e040 0000 1041 0000 3041 0000 803f  =  (7, 9, 11, 1)   ✔
```

This is exactly the validation pattern for `native/engine/`: feed a known input
to the real recompiled function and compare its output to the native
reimplementation.

### Library API (for custom drivers — e.g. float args, multi-call sequences)

```c
oracle *o = oracle_create(NULL, /*want_io=*/0);
uint32_t a = oracle_alloc(o, 64);           // bump-alloc free high guest RAM
oracle_write_mem(o, a, src, n);             // inject
oracle_set_arg(o, 0, a);                    // $a0 = a   (1->$a1 … 7->$t3, ≥8->stack)
oracle_ee(o)->f[12] = fbits(1.0f);          // float arg
int rc = oracle_call(o, 0x005aecf8);        // rc 0 ok / -1 not a func / -2 faulted
uint32_t v0 = oracle_ret(o);
oracle_read_mem(o, a, dst, n);              // read back
oracle_dump_mat4_json(o, a, count, /*row*/0, stdout);
oracle_destroy(o);
```

`oracle_call()` runs the recompiled function **directly** on the `ee_state`
(no scheduler). That is correct for compute / parse / transform functions. A
function that **blocks on a semaphore** (`WaitSema`) needs the cooperative
scheduler + a guest thread (that is the boot harness in `port/boot/`, not this
tool). A hard dereference fault inside the call is caught (SIGSEGV/SIGBUS →
`oracle_call` returns `-2`, `oracle_last_fault()` describes it) so one bad call
cannot take the tool down.

---

## The overlay fix (why `gen/` exists)

The stock recompiler (`recomp/tools/recompile.py`) translates **only the primary
`.text` section** `[0x00100000, 0x00460720)` — its `TEXT_END_VA` is the start of
`.vutext`, and its address→file map (`va2off`) is a single linear map valid only
for the first LOAD segment. The EE binary, however, has several **`.kel_*`
overlay code sections** above that (`.kel_us_override` @0x5183c0,
`.kel_extended` @0x528580, **`.kel_draw_section` @0x5a6d00**, `.kel_title`,
`.kel_net`), carried in a *second* LOAD segment at a different file offset.

Consequence: **every function in those overlays was emitted as an empty stub**
(`{ (void)ee; }`) — 3 322 of 13 083 functions. That includes the **entire MOD
character / skin / skeleton subsystem** in `.kel_draw_section` (335 funcs:
`FUN_005aecf8` parse, `FUN_005b0470` dms-reloc, `FUN_005b0d30` palette builder,
`FUN_005b2550` per-frame, `FUN_005b1260` skin emitter, `FUN_005a8850`
instantiation, …). The stock oracle literally **could not run** the bone-pose
path — the code was absent.

`tools/recompile_keldraw.py` is a local copy with a **segment-aware `va2off`**
(derived from the ELF program headers) and `TEXT_END_VA` raised to `0x5c0000`.
Re-running it translates those overlays with **0 `ee_trap_unimpl` sites**. The
two TUs that hold `.kel_extended` + `.kel_draw_section` (TU 0038, 0039) are
regenerated into `gen/` and **swapped into the link** in place of their stock
stub objects; every other TU and the function table come from the stock corpus
unchanged (no duplicate / missing symbols — verified by `make test` still
passing). The verified `FUN_005aecf8` relocation in `oracle-bones` is proof the
swapped-in bodies are real and correct.

To regenerate `gen/` from scratch:

```sh
python3 -I tools/recompile_keldraw.py emit \
  --elf /srv/games/dirge-port/elf/SLES_541.85 \
  --csv /srv/games/dirge-port/analysis/ee_boundaries.csv --outdir gen
```

(This writes all 44 TUs; the Makefile only links `gen/eetu_0038` + `gen/eetu_0039`.)

---

## Bone-matrix result (km01vnw) — BLOCKED, honestly

Goal: dump the per-bone 4×4 matrices the skin path reads at
`*(ctx+0x20) + boneIndex*0x40` for km01vnw (container `0c064400.bin`, `evm000`).

**Reached (real code ran):** `oracle-bones` injects the container, locates the
MOD payload at `+0x60`, and calls the **real** `FUN_005aecf8`. Relocation is
verified live: the entry-offset array base `0x28 → 0x800088` (`+payloadBase`),
entry[0] `0x2c → 0x80008c`. The oracle executes genuine `.kel_draw_section`
code, not a stub.

**Blocked (no matrices — not fabricated):** the bone palette is **not** built by
the MOD parse. It is built by `FUN_005b0d30`/`FUN_005b2550` from a **runtime
model-instance object** whose fields (`+0x10` bone count, `+0x1c` matrix base,
`+0x20` draw array, `+0x24` per-bone pointer array) are produced by the full
**character-instantiation chain**:

```
heap alloc (FUN_00407d38 / FUN_005a6d00)
  -> FUN_005a8850        (allocate palette+scratch, read bone count from 'dat')
    -> FUN_005a86f0/FUN_005b2c88   (ctx+0x20 = palette base; init to IDENTITY)
    -> FUN_005a87a0/FUN_005b2d58   (per-frame bind of ctx+0x20 / model+0x1c)
    -> pose evaluator               (compose 'dat' skeleton + 'dks' local xforms
                                      into real bone matrices)
```

and it needs **`dks` + `dat` skeleton chunks**, which **km01vnw's container does
not contain** (only `chrid.chi` + `mod` parts — consistent with
`native/assets/models/FORMAT.md §7`'s proven "skeleton is runtime-only, no
skeleton file" finding). Calling `FUN_005b0d30` on the parsed payload runs over
garbage (the payload is not a model instance) and builds no valid palette.
`out/km01vnw_bones.json` records this: `status: BLOCKED`, `bones_dumped: 0`,
`matrices: {}`.

**Exact next step** (for whoever owns the skeleton work — not this harness):

1. **Extend the stock recompiler** the way `tools/recompile_keldraw.py` does
   (segment-aware mapping + overlay sections), so the whole `.kel_*` subsystem
   has real bodies in `recomp/generated_all` (currently only this harness has
   them, for TU 38/39).
2. Pick a character that actually carries a `dks` + `dat` skeleton, drive the
   instantiation chain up to `FUN_005a8850` (needs the heap allocator + the
   character resource archive loaded **and relocated** by
   `FUN_002585e0`/`FUN_002588c8`), then step a frame of the pose evaluator and
   `oracle_dump_mat4_json(o, ctx+0x20, boneCount, /*row*/0, …)`.
   *Or* statically bake the `dat`/`dks` per-bone local transforms (written by
   `FUN_005b27c8`) without the animator — the realistically reachable static
   path.

The skin math that consumes the matrices
(`world = BoneMatrix·((localInt/4096)·meshScale, 1)`, VU0 micro @0x198) is
already fully known — `port/vu/disasm/vu0_skin_micro.txt`. Only the matrices
are missing, and they are genuinely runtime-built from skeleton data this model
does not ship.
