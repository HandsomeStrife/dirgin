# dirgin

A from-scratch **native PC port** of *Dirge of Cerberus: Final Fantasy VII* (PS2),
in the decompilation/reauthor tradition — **no emulation, no game data included**.

> **Bring your own disc.** This repository contains only original port code,
> reverse-engineering tooling, and research notes. It contains **no** copyrighted
> game assets, no game executable, and no decompiled or machine-recompiled game
> code. To build or run anything here you must supply your own legally-owned copy
> of the game and extract its data yourself.

## Try the browser prototype

One room (the "sea of flames" walkway) with a playable Vincent, in the browser, built from
your own disc: `tools/byo/build_web.sh /path/to/your.iso`. The supported disc,
prerequisites, controls and limitations are in [docs/BROWSER.md](docs/BROWSER.md). If you'd
rather have a coding agent set it up for you, see [docs/AGENT-SETUP.md](docs/AGENT-SETUP.md).

## What's here

| Path | What it is |
|------|------------|
| `port/` | Low-level reference layer: KEL.DAT archive loader (`src/`), the static-recompilation + hardware-HLE harness kept as a validation *oracle* (`oracle/`, `recomp/`, `hle/`, `gs/`, `vu/`, `cop2/`, `sif/`, `boot/`). |
| `native/` | The native reauthor: `engine/` (math, scene graph, material, camera, actor, …), software + OpenGL renderers (`renderer/`, `gpu/`), `script/` (JVM subset), `text/`, `movie/` pipeline, and asset **loader** code under `assets/*/` (the code, not the data it reads). |
| `tools/` | Reverse-engineering tooling: KEL unpacker, GS-dump toolkit, Ghidra scripts. |
| `notes/` | Research notes, format write-ups (`FORMAT.md`), status, and session logs. |

## Approach

The game ships its own asset parsers; each format was reversed *from the game's
own code*, reauthored as clean native C, and validated by **oracle-parity diff +
mutation testing** against the recompiled original. The static-recomp hybrid boots
to the game loop and is retained only as ground truth; active development targets
the native layer.

See `notes/STATE-OF-THE-PORT.md` for the full status and `notes/PLAN.md` for direction.

## Legal

*Dirge of Cerberus: Final Fantasy VII* and all its assets are © Square Enix. This
project is an unaffiliated, non-commercial reverse-engineering effort for
interoperability and preservation. No copyrighted game data is distributed here.
