# dirge-port: project rules for every session and worker

## Faithful port, not a remake

The goal is a **faithful port** of Dirge of Cerberus (PAL SLES_541.85): the game's own
behaviour, decoded from its code and data. Improvements come later, as clearly
separated, optional layers on top of a faithful base.

Every change must decode what the existing game does. That covers gameplay, physics,
animation, camera, rendering, audio and UI.

- **Source of truth:** the game's code (Ghidra project `ghidra-proj/`, decomp in
  `analysis/decomp/`, `tools/DecompAt.java`, the recompiler in `port/recomp`) and its
  data files.
- **How we check it:** the GS dumps (`tools/gsdump/dumps/`) for rendering, and the
  PCSX2 per-tick traces (`tools/trace/`) for gameplay. "Looks about right" does not count.
- **Translate, don't redesign.** Port a game function as a direct translation that
  keeps its structure, constants and order of operations. Cite its address
  (`// FUN_00357d48`) next to the code. Running the recompiled function counts as a
  translation too.
- **No invented behaviour.** Don't add tuning, smoothing, blends, overlays, timers,
  camera moves or "feels right" rules that the game doesn't have.
- **Undecoded parts:** where something can't be decoded yet, the stand-in must be
  small, labelled `// GUESS: <what> (not decoded; see <notes>)` in the code, and
  listed in `notes/fidelity-audit.md`. Nothing should depend on it, and it must be
  easy to replace. Prefer leaving a feature out over faking it.
- **Stuck on decoding:** if decoding fails after two attempts, stop and report what
  was found. Don't ship an invented fix.
- **Improvements later:** extras beyond the original (better filtering, widescreen,
  etc.) go behind an explicit option that is off by default, and only once the
  faithful version exists.

## Repo hygiene

The repo is PUBLIC, with a default-deny `.gitignore`. Never commit game data or
anything derived from it (dumps, traces, exports, tables copied from game files).
Small hand-written constants and addresses in research notes are fine.

Start each task by reading `notes/HANDOFF.md`.
