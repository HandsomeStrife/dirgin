# Session 2026-10-17 (pt 3) — the convergent root cause + the one path forward

## THE finding that unifies the room and the character
Both remaining problems have the **same root cause**, confirmed independently this
session:

**Per-surface / per-draw-group texture binding is RUNTIME state (EE-RAM texture
handles), NOT stored in the on-disk containers.**
- Room: the BMD packets carry no TEX0/TBP0; the binding is reconstructed from the
  GS-dump per-packet oracle (`z143_pktfc`), which only covers what the dump cameras
  saw (~55%).
- Vincent p005: every draw sub-packet carries a byte-identical GIFtag with **no
  TEX0**, and every group header shares one EE-RAM texture-handle pointer
  (`0x0022f868`). The lost ad-hoc "combined atlas" builder *guessed* the per-group
  atlas/region and mis-guessed the body → the torso scramble.

So chasing an on-disk binding is a dead end for BOTH. **The GS dumps are the sole
ground-truth oracle**, and this session validated that we read them correctly end
to end (see [[session-2026-10-17-closedloop]] / gsrender2).

## The authoritative next step (applies to BOTH Vincent body AND room pod)
**UV-triple matching against the dump draws.** The dump gives, per draw: real TEX0
(TBP0/atlas-region) + per-vertex UVs + screen XYZ. The BMD room packets and the
p005 mesh groups both store per-vertex UVs (p005 as raw/4096 triples). Match a
mesh group's UV-triples to the dump draw that used the same UV-triples → assign that
draw's real TEX0/TBP0 to the group. This is how `z143_room_uvtriple_binding.json`
was built for the room; do the same for p005's body groups, and extend it to any
room surface a dump angle covers. gsrender2/gsbind already decode every dump
texture and expose per-draw TEX0+UV, so the reader half is done.
Caveat: a surface/figure only binds if a dump camera actually rasterized it
(Vincent is smallish in d03; the room pod shell + some upper walls were never seen
— those need a runtime EE-RAM savestate, not more dump angles).

## Room status (graded from 7 angles this session — honest)
- Layout (see `/srv/shared/dirge-port/binding-renders/z143-roomgrade/`): a 12-sided
  central reactor pod ringed by perimeter walkway platforms. Vincent walks the
  perimeter (matches game_d_143200).
- From interior/walkway angles it READS as the FFVII reactor: riveted dome, hanging
  pods w/ red markings, lit ring platform, metal wall panels — mostly textured.
- The orbit-"hero" white cylinder was a camera artifact aimed at the one unbound
  part. Unbound = central pod shell + some upper-wall segments (no dump coverage);
  green areas in shots are the GL clear colour through missing geometry.
- Coverage 52.9% → **55.2%** (agent folded in the regenerated 068fff12 column atlas;
  pktfc 52.3%→54.5%). Build on disk: `out/z143_room/room_oracle2.*` (gitignored,
  not committed). The `.list`/`.mesh` pipeline renders via render_scene_textured
  (needs a dummy -m char) or render_env_textured; render_room_native uses the
  separate chunk/.manifest pipeline.

## Vincent status (graded — honest)
- `native/assets/models/render_p005.py` (new, committed): multi-atlas software
  renderer. Decodes the 3 real atlases from 07700142 (body 256x512 `p005_1_a`,
  hair 256x256 `p005_2`, cloak 256x128 `p005_3`), classifies groups BODY/HAIR/CLOAK
  by aligning to the Vincent rip, drops strip-bridge tris.
- **Cloak = correct red again** (render_skinned's single-atlas had it brown).
  Silhouette reads as Vincent from any angle.
- **Body/torso/legs still scrambled** (front AND back) — runtime per-group→region
  binding, unrecoverable from disk. NOT fixed. The HAIR classification is low
  confidence (green leaks onto shoulders in the class-debug). render_skinned.py is
  single-atlas/char_e000 only and was NOT the p005 path (that builder is out-of-repo).

## Artifacts for Dan
`/srv/shared/dirge-port/binding-renders/z143-roomgrade/STATUS_triptych.png` — game
reference vs our room @ game-angle vs our Vincent, labelled honestly.
`/srv/shared/dirge-port/binding-renders/z143-closedloop/` — the dump→frame validation.
