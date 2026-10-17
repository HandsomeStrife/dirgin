# Session 2026-10-17 (pt 4) — Vincent dump-binding: atlases + binding recovered; UV read is the remaining bug

## What UV-triple matching against the GS dump proved
Tools (committed): `native/assets/models/char_dumpbind.py` (matcher) and
`render_p005_dumpbound.py` (renderer using recovered binding).

1. **The dump holds Vincent's REAL runtime atlases** (decoded from d03, see
   `vincent_REAL_atlases_from_dump.png`):
   - `TBP0 0x2e50` 256x512 = body/face/armor (Vincent's face is visible in it)
   - `TBP0 0x3068` 256x256 = black hair strands
   - `TBP0 0x316c` 256x128 = the red tattered cloak
   - (0x31f8 = Cerberus gun; 0x304c/0x2d88 = room wall / window fire, not Vincent)
   These are ground truth, unlike render_p005's reconstructed atlases.

2. **Per-group atlas binding is recoverable** by matching each p005 draw-group's
   UV-triples to the dump draws: ~27 groups -> body, ~5 -> hair, 1 -> cloak,
   matching Vincent's anatomy. `p005_dumpbinding.json` has the per-group votes.

## The decisive finding (redirects the fix)
Rendering groups against their **correct dump atlas + the ON-DISK UVs STILL
scrambles the torso** (`vincent_dumpbound_still_scrambled.png`). So the atlas
binding was NOT the missing piece.
- Only **21%** of p005 tris' on-disk UV-triples match the dump's UV-triples.
- Rendering **only those 21%** gives a COHERENT figure — hair reads as hair,
  cloak red, no "eyes/circles" scramble (`vincent_matched21pct_coherent.png`).
- => The matched UVs are correct; **the scramble is the on-disk UV read being
  wrong for the other ~79% of triangles.** This is the "backward UV read" the
  geometry notes flagged (UV precedes the S-8 index; V2-16 ordering), still only
  partly right in `render_skinned.R.parse_drawpackets`.

## The remaining fix (clear objective metric)
Fix the UV decode in `render_skinned.py` (`parse_drawpackets` / the VIF unpack that
yields `sub['uv']`) so that **char_dumpbind's match rate rises from 21% toward
~80-90%**. The dump is ground truth, so match rate is an objective loss function —
iterate the UV read (offset/stride/pairing within the VIF packet) against it, then
the body textures correctly. Dump s/q == on-disk raw/4096 (both normalised [0,1]);
char_dumpbind quantises to 1/512 for the signature.
Caveat: the dump only draws the Vincent that was on screen in d03 (small, likely
backface-culled), so a match-rate ceiling < 100% is expected; aim to maximise it
and confirm the render is coherent. Where the dump covers a tri, its UV is the
authoritative value and can be transferred directly.

## Status
Cloak + silhouette correct; **body still scrambled** (not shipped as fixed). The
atlas/binding half is solved; the UV-read half is the next focused debug.
See also [[session-2026-10-17-convergence]] (same runtime-binding root cause as the
room) and [[session-2026-10-17-closedloop]] (the validated dump reader).
