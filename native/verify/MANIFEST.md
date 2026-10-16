# Dirge of Cerberus — Native Port: Settled-Subsystem Manifest

Index of the key deliverables per settled standalone subsystem (libraries, tools,
format docs). Paths are relative to `/srv/games/dirge-port/`. Excludes the
in-progress subsystems (`native/engine`, `native/assets/models`, `native/gpu`,
`native/text`, dashboard).

## textures — `native/assets/textures/`
- **Library:** `libdtex.a` (static; feeds the renderer)
- **Tool:** `dtex` (CLI: texture → PNG exporter)
- **Headers:** `dtex.h`, `dtex_internal.h`
- **Sources:** `swizzle.c`, `decode.c`, `txd.c`, `tim2.c`, `rst.c`, `png.c`, `main.c`
- **Format doc:** `RST-FORMAT.md`
- **Samples:** `validation/*.png` (13 decoded reference images)
- **Build:** `make`

## audio — `native/assets/audio/`
- **Tools:** `dirge_atdec` (ATRAC3/ATRAC3plus → WAV, via libavcodec), `dirge_wd` (SPU2 `.wd` sound banks → WAV, self-contained), `dirge_sound_demo` (playback/mixer validation)
- **Library:** `dirge_sound.o` (playback/mixing engine + db.bin sound-request API) + `dirge_sound.h`
- **Sources:** `dirge_atdec.c`, `dirge_wd.c`, `dirge_sound.c`, `dirge_sound_demo.c`
- **Format doc:** `FORMAT.md`
- **Build/test:** `make`, `make sndcheck` (also `make check`, `make wdcheck`)
- **Deps:** `libavcodec libavutil libswresample` (pkg-config), `-lm`

## collision — `native/assets/collision/`
- **Tool:** `cidtool` (CID map-collision loader + OBJ exporter)
- **Headers:** `cid.h`
- **Sources:** `cid.c`, `obj_export.c`, `main.c`
- **Format doc:** `FORMAT.md`
- **Output:** `out/*.obj` (exported meshes)
- **Build/test:** `make`, `make validate` (also `make check`)

## effects — `native/assets/effects/`
- **Tool:** `fed` (FED/FEZ effect-definition parser + dumper)
- **Headers:** `fed.h`, `fez.h`
- **Sources:** `fed.c`, `fez.c`, `fed_dump.c`, `main.c`
- **Format doc:** `FED-FORMAT.md`
- **Samples:** `sample_a0008_curves.json`, `sample_a0269.json`
- **Build/test:** `make`, `make check`
- **Deps:** `-lz`

## movie — `native/movie/`
- **Scripts (`scripts/`):** `decrypt_movie.py` (DoCMovieTool EU-key decrypt), `parse_toc.py` (archive TOC), `demux_audio.py` (PS2 PSS 0xBD demux), `decode_movie_audio.py` (SShd/SSbd PCM → stereo WAV)
- **Output:** `out/` (decoded frames, `.m2v`/`.pss`/`.wav`/`.mp4` samples)
- **Check:** `python3 -I -m py_compile scripts/*.py` (stdlib only)

## script — `native/script/`
- **Tools:** `dcc` (class disassembler), `jvmrun` (interpreter), `jvmtest` (self-test), `jvmbatch` (corpus batch + coverage)
- **Shared parser:** `classfile.c`/`.h` (decrypt + container parse + flat resolve)
- **VM:** `jvm.c`/`.h`, `bridge.c`/`.h`, generated `bridge_api_gen.h` (281-method native bridge table)
- **Sources:** `dcc.c`, `jvmrun.c`, `jvm_test.c`, `jvmbatch.c`
- **Reports:** `coverage_report.txt`, `batch_results.txt`, `engine_bridge_api.txt`
- **Build/test:** `make`, `make test`, `make jvmbatch`

## kel loader — `port/`  (note: NOT `native/src/`)
- **Tool:** `kel_tool` (KEL.DAT archive list/extract/extractcode)
- **Library:** `src/kel.c` + `src/kel.h` (KEL loader), `src/kel_tool.c`
- **Build:** `make` (in `port/`)
- **Deps:** `-lz`

## verification harness — `native/verify/`
- `verify_all.sh` — the integration-verification runner
- `VERIFY_REPORT.md` — build+test status per subsystem, aggregate health
- `MANIFEST.md` — this file
- `results.tsv` — machine-readable summary
- `logs/*.build.log`, `logs/*.test.log` — full captured output
