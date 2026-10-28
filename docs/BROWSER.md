# Browser prototype: build it from your own disc

The browser prototype shows one scene from *Dirge of Cerberus: Final Fantasy VII*: the
"sea of flames" walkway in Kalm (room z128), with Vincent. It has two pages:

- **`index.html`**: a free-camera viewer of the room, with a standing Vincent.
- **`play.html`**: walk Vincent around the room: run, jump, climb ladders, aim and fire.

It is a research prototype of a faithful port: the room, the character, the animation clips,
the collision and the ladders are read from the game's own files by our decoders. Parts that
are not decoded yet are stand-ins marked `GUESS` (see [Known limitations](#known-limitations)).

## Legal

This repository contains **no game data**. No models, textures, animations, sounds, code or
anything made from them. You must own the game and use **your own copy of the disc**. The
build script reads your disc image on your machine and writes the result to folders that git
ignores. Don't share, upload or commit the disc image, the unpacked files or the built data
(`extract/`, `native/web/data/`, `native/assets/models/out/`). *Dirge of Cerberus* and all of
its assets are © Square Enix. This is an unaffiliated, non-commercial project.

## Supported disc

Only one disc is supported: the PAL release, **SLES-54185**, "Dirge of Cerberus: Final
Fantasy VII" (Europe, Australia), English/French/German/Spanish/Italian. Other releases (the
US and Japanese discs, the International edition) have different files and won't pass the
check.

You need a plain **`.iso`** image of the DVD (2048-byte sectors). If your dump is `.bin/.cue`,
`.mdf/.mds`, `.nrg` or `.chd`, convert it to `.iso` first.

The script checks the files inside the image, not the whole image file, so rips that differ
only in padding still pass. It checks these SHA-256 hashes:

| file in the image | SHA-256 |
|---|---|
| `SYSTEM.CNF` (boots `SLES_541.85`) | `2a524ce9bb4d48258721b92e54727d2d32af52a89db48b948304760e650bfb25` |
| `SLES_541.85` | `d51e59218891b61db4438824e04dcb8e19fe8468c7e034b2375856f771c21012` |
| `FILELIST.BIN` | `6ec5b671f107df1249305a52326a6e8c5c737ba675b66164153dd8858655b850` |
| `E419C51B/KEL.DAT` | `7ce51b07f2e218a1ba734c379276ec08a73197d3db337158c897cca35303d9cc` |

Our reference image (4,188,471,296 bytes) has SHA-256
`0b29c6f7947fa6fa3fa1c363fe224856d89b1451c0a561ff63dc7709b39b6e3c`. Yours doesn't have to
match it. To compare: `python3 -I tools/byo/disc.py isohash your.iso`.

## Prerequisites

- **Linux or macOS** (on Windows, use WSL2).
- **python3** 3.8 or newer, with **numpy** and **Pillow** (your OS packages, e.g.
  `python3-numpy python3-pil`, or `pip install numpy Pillow` inside an active venv kept outside the clone; the build runs
  `python3 -I`, which does not see `pip install --user`).
- **A C compiler** (`cc`, `gcc` or `clang`) and **make**. Debian/Ubuntu: `build-essential`.
  macOS: `xcode-select --install`. No other libraries are needed (no zlib).
- **git**: you need a git clone of the repository. The exporters use `git check-ignore` to
  make sure they only write into ignored folders.
- **About 3.5 GB of free disk space** while building (a 1.5 GB archive copy is deleted
  afterwards, leaving about 1.2 GB unpacked and about 70 MB of built data).
- **A browser with WebGL** and internet access: the pages load three.js from
  `cdn.jsdelivr.net`.
- Optional: **Docker** with `docker compose`, for the serve option that includes the Report
  backend. **Node.js** is needed only for the collision tests.

## Build: one command

```sh
git clone https://github.com/HandsomeStrife/dirgin.git
cd dirgin
tools/byo/build_web.sh /path/to/your.iso
```

It takes about 5 to 10 minutes. The script:

1. checks the prerequisites,
2. checks the disc (boot id and file hashes),
3. copies `FILELIST.BIN` and `KEL.DAT` out of the image and unpacks the game archive into
   `extract/_KEL/` (gitignored), then checks every file the build reads against a list of
   hashes,
4. builds two small C tools (the texture decoder `dtex` and the collision exporter `cidweb`),
5. runs the exporters into `native/web/data/` (gitignored).

Running it again is safe. A verified `extract/` is reused and outputs are overwritten. Each
step logs to `native/assets/models/out/byo/logs/<step>.log`. On failure the script prints the
end of that log.

The script never deletes an `extract/_KEL` that it did not create. If one is there and
doesn't match, it stops and asks you to move it away.

## Serve and open

```sh
tools/byo/serve.sh             # python3 http.server on http://127.0.0.1:8000/  (no Docker)
tools/byo/serve.sh docker      # nginx + Report backend on http://127.0.0.1:8796/
tools/byo/serve.sh docker-down # stop the Docker option
```

Then open `http://127.0.0.1:8000/index.html` or `http://127.0.0.1:8000/play.html`. Both
options listen on 127.0.0.1 only. `--port N` changes the port.

The python option is a plain static file server. Everything works except the **Report**
button (R), which posts a screenshot and note to `/api/report`. The Docker option adds that
backend (`tools/byo/report_api.py`) and saves reports to `tools/byo/serve-docker/reports/`.
The backend is optional.

Opening `index.html` straight from disk (`file://`) does not work: browsers block the data
requests. Use one of the servers.

## Controls

**play.html** (click the page first to capture the mouse):

| input | action |
|---|---|
| mouse (or drag) | look around |
| W A S D | move (relative to the camera) |
| Shift | run |
| Space | jump |
| E | grab a ladder in front of you; then W/S climb, Space jumps off |
| left mouse button | fire |
| right mouse button or Q (hold) | aim |
| Z or middle button | zoom |
| F | reload |
| H | respawn at the start |
| R | report (Docker option only) |

**index.html** (free camera):

| input | action |
|---|---|
| mouse (click to capture, or drag) | look around |
| W A S D | fly |
| Space or E / C or Q | up / down |
| Shift | fly faster |
| H | back to the start view |
| R | report (Docker option only) |

Useful URL parameters: `?hud=0` hides the text overlay; `?cam=x,y,z,yaw,pitch` sets the view
(the HUD shows the current values); on play.html, `?ladder=0` starts at the ladder next to the
start deck; `?packs=` skips the extra animation packs; `?actions=0` turns off jump, ladder and
shoot.

## Known limitations

This is a prototype, not a game. What exists: one room, Vincent, walking, running, jumping,
ladders and shooting. No enemies, no story, no sound, no menus.

**GUESS stand-ins.** Many gameplay values are not decoded from the game's code yet: the
acceleration, the turn rate, the follow camera, the aim mode, the gravity while falling, the
animation blends and more. They are marked `// GUESS:` in the code. `notes/fidelity-audit.md`
lists each one and the game function it should come from.

**What the dump-free build leaves out or guesses.** The maintainers build from GS captures
of the running game as well. Your build reads only the disc, so a few things differ from
theirs:

- **No sky dome.** The sky's geometry isn't in the room files: it's only known from a GS
  capture. The sky is the flat clear colour instead.
- **The standing Vincent in `index.html`** uses the decoded idle clip (`h_st`, frame 91) with
  the cape on its skinned target. Its position and facing are fixed numbers marked GUESS:
  Vincent's start position in the stage file is not decoded yet.
- **Vincent's scale** is the game's own: unit scale, with his model origin at the character
  position (decoded from the motion player's matrix). It is the same in this build and in the
  maintainers' build, in `play.html` and in `index.html`.
- **The gun** (Cerberus) takes each triangle's texture from the files. The capture-based
  build picks one texture per part, so some small faces show a different texture. It draws frame0
  and barrel1, the parts the game's GS dumps show; the short barrel0 is never drawn.

See `notes/HANDOFF.md` for the research state.

## Troubleshooting

| message | fix |
|---|---|
| `wrong disc. It boots ...` | Only the PAL SLES-54185 disc is supported. |
| `not an ISO 9660 image` | Convert your image to a plain `.iso`. |
| `... differ from the supported disc` / `does not match` | The image is damaged or another revision. Re-rip it and compare with the hashes above. |
| `python3 -I sees no numpy` / `no Pillow` | The OS packages, or `pip install numpy Pillow` inside an active venv (a `--user` install is not seen). |
| `missing: a C compiler` | Install `build-essential` (Debian/Ubuntu) or the Xcode command line tools (macOS). |
| `need about 3.5 GB free` | Free some disk space. |
| `extract/_KEL exists and was not made by this script` | Move `extract/_KEL` away and run the build again. |
| `... is not gitignored: refusing` | `.gitignore` was changed. Restore it with `git checkout .gitignore`. |
| page stuck on "loading", or a 404 on `data/...` | Build first. Serve the `native/web` folder (use `serve.sh`), not the repository root. |
| black page, three.js errors | The browser needs WebGL and access to `cdn.jsdelivr.net`. |
| Report button fails | Expected with the python option. Use `serve.sh docker` if you want reports. |

Letting your own coding agent do the setup: see [AGENT-SETUP.md](AGENT-SETUP.md).
