# Review: sea-of-flames Vincent + z128 room (2026-10-18)

Images you can view directly on GitHub (click each `.png`). All are graded against
the game's own frame 163235 / the GS dump oracle, not eyeballed.

- **01_scene_vincent_on_deck_vs_game.png** — the headline: GAME (left) vs OUR
  in-engine render (right). Vincent correctly textured (dark hair, red mantle, grey
  armoured legs, complete body) standing on the now-textured brown plank walkway,
  rendered by the port's own GPU pipeline.
- **02_deck_recovery_game_before_after.png** — the walkway deck: GAME / BEFORE
  (flat grey) / AFTER (brown planks, matched to the dump at 1.00 correlation).
- **03_vincent_colour_brown_to_red.png** — the colour root-cause: OLD (wrong palette
  = brown) → NEW (runtime palette = red) → dump oracle. Vincent's body texture ships
  4 palettes; the game picks red per-draw, we were using brown.
- **04_vincent_binding_vs_oracle.png** — per-part palette binding (red mantle / grey
  body / skin face) vs the oracle.
- **05_decoder_coherent_proof.png** — proof our geometry+UV decode is correct (the
  "wrong textures" were palette binding, not a UV bug).

## What changed (code, on GitHub)
Recent commits on `main`:
- `z128 room: recover the walkway deck texture (ground-truth brown planks)`
- `p000 GPU: complete body (all LOD families) + -charprelit GS vertex shading`
- `p000 GPU: in-engine Vincent with correct per-palette binding + vertex shading`
- `p000: per-triangle runtime texture/CLUT binding from the GS dump (not a UV bug)`
- `gsrender2: --native renders at the 432x512 GS drawable, resized to 4:3`
- `p000/in-engine: place Vincent upright on the z128 walkway (-Y-up actor fix)`

## Honest remaining gaps
- The long flowing **cape** (procedural cloth, not in the skinned mesh) — biggest
  silhouette difference, not yet attempted.
- Bind pose vs the game's walking pose.
- Some **dump-unseen room surfaces** (roof facets/fascias) still grey — left honest
  rather than guessed.
