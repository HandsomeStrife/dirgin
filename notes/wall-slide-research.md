# Wall slide: game vs page (worker 0096, 2026-10-28)

**Result: no defect in player.js or collision.js, and no patch.** On every wall trace, the
page's slide is the game's to within 0.6 u/s per frame (most traces within 0.01) and 0.022
units in position. The reported "game 42 u/s, page 59.5" matches the game's actual movement
(60 × cos 45° = 42.4) set against the *commanded* speed C+0x1d8, which the page's HUD shows
(`speed … u/s` = `ctrl.speed × 60`). Both the game and the page keep C+0x1d8 at the commanded
speed against a wall (FUN_0034c388, flag 0x2000 clear). In c_run frames 38-56 both read
60.00 while the body slides at 47-51.

## Traces (tools/trace/scripts, output in tools/trace/out, gitignored)

From the sea-of-flames spawn, toward the outer deck edge (walk-mesh boundary, z = −1113.35,
2.3 units away). The stick is aimed through FUN_00258218's dead zone
(axis = (|raw| − 0x2f) / 81; the first version of these scripts sat inside it).

| trace | aim | game slide (u/s, first wall frames) | page vs game |
|---|---|---|---|
| j_wall30 | 30° | 52.1 → 37.5 (camera turns), then a corner | pos ≤ 0.0026, slide ≤ 0.12 u/s |
| j_wall45 | 45° | **41.5**, 39.7, 36.7 … | pos ≤ 0.0055, slide ≤ 0.008 |
| j_wall60 | 60° | 28.7, 27.0 … | pos ≤ 0.0056, slide ≤ 0.007 |
| j_wall80 | 80° | 12.5 | pos ≤ 0.022 (linear drift, see below), slide ≤ 0.006 |
| j_wall_along | 45° then "parallel" (walk magnitude after the dead zone) | 47.1 | pos ≤ 0.004 |
| c_run (existing) | full run, ~38° | 47.2 → 51.3 | pos ≤ 0.0023 |
| f_railing (existing) | walk | 12.0 | pos ≤ 0.010 |
| e_jumps / i_air_release (existing) | airborne, 38° | 47.1, and 54.0 × 0.785 = 42.4 with the stick released | not replayable: replay.mjs and test_wall.mjs don't feed buttons, so the page never jumps |

The slide speed is always the commanded speed × cos(angle to the edge). The camera's
auto turn-behind slowly changes the stick's world angle, so the speed drifts.

## Decode (FUN_00357d48, the physics-body walker; called from FUN_003590c8)

- Entry: clears C+0x1c0 bits 0x400 and 0x40000. With body flags 0x236 (bits 1-2 set) it
  takes the FUN_0035a440 branch (sub-tree-0 tag-1 primitive push-out).
- Boundary edge (no neighbour, or a wall-flagged neighbour): the remaining move is projected
  onto the edge (FUN_0025a778(0, …)), then pushed back in by 0.01 × its length along the edge
  plane (`-sqrt(|m|²) * 0.01`). In air state 2 the projection is horizontal and the y is
  kept. That is collision.js navMove, unchanged.
- Flags: angle(move, edge vector from FUN_0035d6e0) via FUN_00261038. If it is > 0.7853982
  and < 2.3561945 (45°..135°), set C+0x1c0 |= 0x40000 | 0x80000. On the first hit, set
  C+0x1c0 |= 0x400 and C+0xbc = 1. All of these are visible in the traces (0x400 on any
  slide, 0xc0400 when he hits steeper than 45°).
- No speed change: C+0xcc (commanded speed) is untouched by a hit. Every reader of 0x40000
  that I found is enemy AI (FUN_00361690, FUN_00518b30, FUN_003667b0) or the player's state
  0x12 branch of FUN_00578748 (not on-foot running). Nobody seen reads 0x80000 / C+0xbc on
  the on-foot path.

## Open

- FUN_0035a440 (1771 lines, primitive push-out) is still not translated (fidelity-audit
  "Physics-body step" row). There are no sub-tree-0 primitives on the start deck
  (x −1500..−1000, z −1140..−1060), so none of these traces touch it. Near a box or
  cylinder elsewhere, the page would run straight through at full speed. If a 59.5-vs-42 gap
  is seen *in positions* somewhere, check for a primitive there first.
- j_wall80 drifts 0.00012 u/frame along the wall (0.022 after 180 frames). That is the size
  of the open-ground drift: navMove works in doubles, while the PS2 rounds every VU0 op
  toward zero. The test allows 0.03.
- To compare jumps with the page, replay.mjs needs the pad buttons mapped to `pressed`/`keys`.

## Test

`node --import ./test_hooks.mjs test_wall.mjs` in native/web: it passes on the current code.
A mutant whose slide keeps the full move length (the "59.5" behaviour) fails all 7 traces:
the page slides at 59.8 where the game slides at 47.1.
