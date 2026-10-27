# Setting up the browser prototype with a coding agent

You can give the text in the box below to your own coding agent (Claude Code or similar)
running on your machine. It sets up the browser prototype from **your own** disc image and
checks each step. It doesn't need any server of ours. Human-readable details:
[BROWSER.md](BROWSER.md).

Replace `/path/to/your.iso` before you paste it, or let the agent ask you for the path. Keep the quotes around it: disc image names often contain spaces.

---

```text
Set up the "dirgin" browser prototype on this machine from my own disc image. Work step by
step, run the check after each step, and stop and tell me if a check fails. Don't improvise
fixes beyond the ones listed here.

Inputs:
- Repository: https://github.com/HandsomeStrife/dirgin (public).
- My disc image: /path/to/your.iso  (ask me if this path doesn't exist).
- Full documentation: docs/BROWSER.md in the repository.

Hard rules (copyright: the repository holds no game data, and it must stay that way):
- NEVER commit, push, upload, attach, paste or share the disc image, anything under
  extract/, native/web/data/ or native/assets/models/out/, or any file made from them.
  This includes pasting file contents or hex dumps into chats or bug reports.
- NEVER edit .gitignore, and never use `git add -f`. If a step says a path "is not
  gitignored", restore .gitignore with `git checkout .gitignore` and stop.
- Don't run `git add`, `git commit` or `git push` at all during this setup.
- Don't download the game, disc images, or game files from anywhere. Use only my image.
- Don't change the exporter scripts or the page code to work around an error. Report it.
- Keep servers on 127.0.0.1 (the scripts already do).

Step 1: get the repository.
  git clone https://github.com/HandsomeStrife/dirgin.git && cd dirgin
  (If I already have a clone, cd into it and run `git status`. It must be a git checkout.)
  Check: `git status` works and `ls tools/byo/build_web.sh docs/BROWSER.md` lists both files.

Step 2: check the prerequisites.
  python3 --version          -> 3.8 or newer
  python3 -I -c "import numpy, PIL; print('ok')"   -> ok   (-I: the build uses it)
  cc --version (or gcc/clang) and make --version -> both present
  df -h .                    -> at least 3.5 GB free
  Fixes: numpy/Pillow: the OS packages, or a venv OUTSIDE the clone (`python3 -m venv ~/.venvs/dirgin &&
  . ~/.venvs/dirgin/bin/activate && pip install numpy Pillow`, then run the build in that shell;
  never inside the repo, where git would see its files). A `pip install --user` is NOT
  seen by the build (it runs `python3 -I`). OS packages
  (Debian/Ubuntu: `sudo apt install python3-numpy python3-pil build-essential`; macOS:
  `xcode-select --install` for the compiler). Ask me before using sudo or installing
  system packages. On Windows, this must run inside WSL2.
  Check: rerun the commands above. All pass.

Step 3: check the disc image.
  python3 -I tools/byo/disc.py check "/path/to/your.iso"
  Check: it prints "disc: SLES_541.85" and three lines ending in "ok".
  If it says "wrong disc": only the PAL SLES-54185 disc is supported. Stop and tell me.
  If it says "not an ISO 9660 image": the image must be a plain .iso. Stop and tell me
  (I may need to convert .bin/.cue, .mdf, .nrg or .chd).
  If a hash is a MISMATCH: the image is damaged or another revision. Stop and tell me.

Step 4: build.
  tools/byo/build_web.sh "/path/to/your.iso"
  It takes 5 to 10 minutes. It prints "==> ..." for each step and ends with "Done in ...".
  Check:
    ls native/web/data/scene.json native/web/data/z128_collision.bin \
       native/web/data/z128_ladders.json native/web/data/vincent/vincent.glb \
       native/web/data/vincent/vincent_jump.glb native/web/data/vincent/vincent_ladder.glb \
       native/web/data/vincent/vincent_shoot.glb native/web/data/vincent/cerberus_web.bin \
       native/web/data/vincent/cloth.json
    -> all exist
    git status --short       -> shows NO files under extract/, native/web/data/ or
                                native/assets/models/out/ (they are ignored)
    git check-ignore -q native/web/data/scene.json && echo ignored   -> "ignored"
  If the build fails, it names the failing step and its log file
  (native/assets/models/out/byo/logs/<step>.log). Common causes:
    - "missing: ..." or "python3 has no numpy": go back to step 2.
    - "need about 3.5 GB free": free disk space, run again.
    - "extract/_KEL exists and was not made by this script": tell me. Don't delete it
      yourself; it may be mine. If I agree, move it: mv extract/_KEL extract/_KEL.old
    - "unpacked files do not match": the image is damaged. Stop and tell me.
  Running the build again is safe (it reuses a verified extract).

Step 5: serve.
  tools/byo/serve.sh            (python, port 8000; leave it running, e.g. in another terminal
                                 or in the background)
  Check: curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8000/data/scene.json -> 200
         curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8000/play.html       -> 200
  If port 8000 is busy: tools/byo/serve.sh --port 8123 (and use that port below).
  Optional, only if I ask for the Report button and Docker is installed:
    tools/byo/serve.sh docker   (port 8796; stop with tools/byo/serve.sh docker-down)

Step 6: tell me what to open.
  http://127.0.0.1:8000/index.html  (free camera)
  http://127.0.0.1:8000/play.html   (walk around: click, WASD, Shift run, Space jump,
                                     E ladder, mouse buttons aim/fire, F reload, H respawn)
  The browser needs WebGL and internet access (three.js comes from cdn.jsdelivr.net).
  If I report a blank page: ask me for the browser console errors (F12). Check that the
  server is serving native/web (serve.sh does this) and that step 4's files exist.
  Optional headless check, if Playwright is installed: load play.html, wait 10 seconds,
  and confirm there are no page errors and that window.__play exists.

Reporting problems: if something fails, give me
  - the failing command and its last 30 lines of output,
  - the log file it names (native/assets/models/out/byo/logs/<step>.log),
  - native/web/data/byo-build.json (build time, commit, image size: no game data),
  - OS, `python3 --version`, numpy/Pillow versions, `cc --version`.
  I'll open an issue at https://github.com/HandsomeStrife/dirgin/issues. Don't attach
  screenshots of game content to public issues unless I say so, and never attach files
  from extract/ or native/web/data/ other than byo-build.json.

When done, summarise: what passed, the URLs, and anything that failed or was skipped.
```

---

## What the agent should end up with

- `extract/_KEL/noPath/`: the unpacked game files (about 1.2 GB, gitignored).
- `native/web/data/`: the built scene (about 70 MB, gitignored).
- A local server on `127.0.0.1` serving `native/web/`.
- `git status` showing no new tracked or untracked files from the build.
