# Session 2026-10-17 (pt 5) — Vincent body texturing substantially FIXED (rip-UV transfer)

## Result
Vincent's torso scramble ("4 red circles/eyes" on the chest) is resolved. He now
renders as coherent dark armor + red shoulder accents + dark-red cloak + spiky hair
— recognizably the game Vincent. See
`/srv/shared/dirge-port/binding-renders/z143-roomgrade/vincent_BEFORE_AFTER.png`
(game vs old scramble vs fixed) and `vincent_FIXED_front.png` / `_3q.png`.
Not pixel-perfect: lower legs/boots are rough (nearest-vertex transfer is ambiguous
where the cloak overlaps the legs) and the gold boot accents aren't resolved. The
upper body / cloak / silhouette are correct.

## How (the chain that worked)
1. The on-disk container has no per-group UV binding and d03 only drew ~1/3 of
   Vincent's UVs, so neither the dump nor the on-disk read alone can texture the
   torso ([[session-2026-10-17-vincent-dumpbind]]).
2. The community rip (`Vincent.dae`) is a FULL, correctly-UV'd model with its own
   atlases (p005_1_a..f body variants, p005_2 hair, p005_3 cloak).
3. **The dump confirmed the rip's UV layout is authentic**: the dump's runtime body
   texture `TBP0 0x2e50` is the SAME layout as the rip's `p005_1_c` (both are the
   RED variant); `p005_1_a` is the natural-colour variant, same layout. So rip UVs
   index the real atlas layout.
4. **Transfer**: align p005 into rip space (similarity solve, chamfer 0.0136),
   nearest-rip-corner → transfer UV + the rip material's atlas to each p005 vertex,
   render sampling the rip's own (natural) atlases. Sampling the dump's 0x2e50
   instead gives the right structure but a RED cast (it's the red variant), which is
   why `render_p005_riptransfer.py` (dump atlas) looks red and
   `render_p005_riptransfer2.py` (rip natural atlas) looks correct.

## Tools (committed, source only; depend on the rip + game data staged locally)
- `native/assets/models/char_dumpbind.py` — recovers per-group TBP0 from the dump
  (confirms atlas layout; `p005_dumpbinding.json`).
- `native/assets/models/render_p005_riptransfer.py` — rip-UV transfer, dump atlas.
- `native/assets/models/render_p005_riptransfer2.py` — rip-UV transfer, rip natural
  atlas (THE good render). `python3 -I render_p005_riptransfer2.py <outdir>`.

## Caveats / cleaner-pipeline follow-ups
- Uses the COMMUNITY rip's UVs+atlases (game-derived but third-party). For a pure
  port, extract the natural body variant from our own `07700142.bin` (render_p005
  already decodes that container's atlases) and keep using rip UVs, OR finish the
  on-disk UV read so the container's own UVs texture correctly (the agent's GIFtag
  LIST/STRIP fix got match 21%->27%; the remainder is a d03 coverage ceiling, not a
  read bug — so the on-disk UVs may actually be close and worth re-checking against
  p005_1_a directly).
- `render_p005.RIP` points at a scratchpad path — stage the rip at a stable local
  location and update that constant for reproducibility.
- Lower-leg roughness: transfer per-triangle or respect rip part-connectivity.
See [[session-2026-10-17-convergence]] for the project-wide runtime-binding context.
