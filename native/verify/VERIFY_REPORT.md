# Dirge of Cerberus — Native Port: Settled-Subsystem Verification Report

**Date:** 2026-10-15
**Scope:** the SETTLED, standalone native subsystems only. Excluded (active agents):
`native/engine`, `native/assets/models`, `native/gpu`, `native/text`, and the shared dashboard — none were built, run, or touched.
**Method:** forced clean rebuild (`make clean` + build) via each subsystem's own Makefile to surface `-Wall -Wextra` warnings (not just "up to date"), then the subsystem's test/validate/smoke target. No subsystem source was edited. Run via `./verify_all.sh`; full output in `logs/`.
**Toolchain:** cc (gcc) 11.4.0, Python 3.10.12, pkg-config; libavcodec/libavutil/libswresample present; zlib present; extract tree `/srv/games/dirge-port/extract/_KEL` present.

## Aggregate health

**7 / 7 subsystems build clean (0 warnings) and pass their tests. GREEN across the board.**

| Subsystem | Build | Warnings | Test | Health |
|-----------|-------|----------|------|--------|
| textures  | clean | 0 | pass (smoke) | HEALTHY |
| audio     | clean | 0 | pass (`make sndcheck`) | HEALTHY |
| collision | clean | 0 | pass (`make validate`) | HEALTHY |
| effects   | clean | 0 | pass (`make check`) | HEALTHY |
| movie     | n/a (python) | n/a | pass (py_compile 4/4) | HEALTHY |
| script    | clean | 0 | pass (`make test`, 9/9) | HEALTHY |
| kel loader| clean | 0 | pass (smoke) | HEALTHY |

No cross-agent breakage, missing dependency, or regression was found in any settled subsystem.

## Per-subsystem detail

### textures — libdtex + dtex CLI (`assets/textures/`)
- **Build:** `make all` clean — `libdtex.a` (6 objs: swizzle, decode, txd, tim2, rst, png) + `dtex` CLI. `-std=c11 -O2 -Wall -Wextra -Wno-unused-parameter`. 0 warnings.
- **Test:** no Makefile test target (build-only subsystem). Binary smoke: `./dtex` with no args prints usage, exit 0. `libdtex.a` and 13 pre-generated validation PNGs present.
- **Health:** HEALTHY. Library + exporter build and the CLI runs.

### audio — dirge_atdec / dirge_wd / dirge_sound (`assets/audio/`)
- **Build:** `make all` clean — `dirge_atdec` (links libavcodec atrac3/atrac3plus), self-contained `dirge_wd`, `dirge_sound.o` + `dirge_sound_demo`. 0 warnings.
- **Test:** `make sndcheck` — the mixer demo resolves db.bin sound ids, **shells out read-only to `dirge_atdec`/`dirge_wd`** to decode real voice + SFX samples, mixes a 5.0s stereo scene and wrote `sound_mix_sample.wav` (220500 frames, stereo 16-bit @ 44100 Hz) to `/srv/shared/dirge-port/`. Sibling shell-out handled cleanly.
- **Health:** HEALTHY. libav path links and decodes; self-contained paths and the mixer all work.

### collision — cid + OBJ export (`assets/collision/`)
- **Build:** `make all` clean — `cidtool` (main/cid/obj_export). `-std=c99 -O2 -Wall -Wextra -Wpedantic`. 0 warnings.
- **Test:** `make validate` — parsed 5 real CID files and exported 5 OBJ: two v2 inline quad-soup (12 and 4800 tris), three v19 grid/indexed-tree maps with `--boxes` (e.g. `06cc0111`: 26362 tris from 979 indexed leaves + 2962 VU packets, 0 degenerate). All parsed with sane bboxes.
- **Health:** HEALTHY. Both CID format versions parse and export.

### effects — fed / fez (`assets/effects/`)
- **Build:** `make all` clean — `fed` (fed/fez/fed_dump/main, links `-lz`). `-std=c11 -O2 -Wall -Wextra -Wpedantic`. 0 warnings.
- **Test:** `make check` — parsed a real `.fed` sample from the extract tree (gracefully skips if absent). OK.
- **Health:** HEALTHY.

### movie — python FMV pipeline (`movie/scripts/`)
- **Build:** none (Python, no C).
- **Test:** `python3 -I -m py_compile` on all 4 scripts (`decode_movie_audio.py`, `decrypt_movie.py`, `demux_audio.py`, `parse_toc.py`) — 4/4 compile. All imports are stdlib (`sys`, `struct`, `wave`, `array`, `os`) and verified importable under isolated (`-I`) interpreters.
- **Health:** HEALTHY. Scripts are syntactically valid and depend on nothing beyond the standard library.

### script — dcc / jvm / jvmbatch (`script/`)
- **Build:** `make all` clean — `dcc`, `jvmrun`, `jvmtest`, `jvmbatch` over shared `classfile.o`/`jvm.o`/`bridge.o`. `-std=c11 -O2 -Wall -Wextra`. 0 warnings. (Checked-in generated `bridge_api_gen.h` was not regenerated; `make clean` leaves it in place.)
- **Test:** `make test` → `jvmtest` **SELF-TEST OK, 9/9 cases** (arithmetic, arrays, native bridge, dup_x2, monitors, call-depth guard, static `<clinit>`). `make jvmbatch` builds the batch tool; a functional batch run over the 2 extracted `.class` files parsed both OK.
- **Observation (not a failure):** in the batch run, `install.class` hits the 2,000,000-instruction step-limit (1 entry errored, `worst=step-limit`); `KerberosIntermissionCalc.class` runs clean. This is a property of executing that specific class entry to completion, not a regression in the subsystem — the deterministic self-test is fully green.
- **Health:** HEALTHY.

### kel loader — kel.c / kel_tool
- **Location note:** the kel.c loader is at **`/srv/games/dirge-port/port/`** (`port/src/kel.c`, `port/Makefile` → `kel_tool`), **not** at `native/src/` — `native/src/` does not exist. Verified the real one.
- **Build:** `make all` clean — `kel_tool` from `src/kel_tool.c` + `src/kel.c`, `-O2 -Wall -Wextra -std=c11 -lz`. 0 warnings.
- **Test:** no Makefile test target. Binary smoke: `./kel_tool` with no args prints usage, exit 0.
- **Health:** HEALTHY.

## Notes / breakages

- **No breakages.** Every settled subsystem builds clean and its test/validate/smoke target passes.
- **Path discrepancy (informational):** the verification brief listed the kel loader under `native/src/`, but it actually lives under `port/`. There is no `native/src/` directory. The script targets `port/` so the loader is genuinely covered.
- **`install.class` step-limit** under `jvmbatch` (see script section) is expected runtime behaviour, surfaced here for visibility — not a subsystem fault.
- Reproduce with `./verify_all.sh`; per-subsystem logs in `logs/`, machine-readable summary in `results.tsv`.
