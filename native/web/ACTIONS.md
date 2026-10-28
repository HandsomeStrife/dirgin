# Player actions (play.html)

`player.js` runs Vincent as a small state machine: **locomotion** (the game's own
on-foot movement, translated from FUN_00578748 / FUN_0034c388 mode 0xc, with its
idle / walk / run clip choice and walk-off falls) plus at most one **active action** taken
from a registry.
Actions are separate modules in `native/web/actions/`. The page has one: `actions/ladder.js`.
The interface is also written at the top of `player.js`.

**Faithful port (CLAUDE.md).** An action must translate the game's code (cite the `FUN_`
address) or be a small `// GUESS:` stand-in listed in `notes/fidelity-audit.md`. Worker 0078
removed the invented actions: the jump (`jump.js`; a later job translates FUN_00575768's
physics-body branch), shooting with its aim mode, aim camera, overlays and muzzle flash
(`shoot.js`), and reload (`reload.js`; the game only reloads a magazine below 9,
FUN_00579570 state 7, so it waits for the weapon translation, worker 0077).

With no actions registered, or with `?actions=0`, only the game's locomotion runs.

**Weapon (not an action).** `aim.js` + `aim_core.js` (workers 0077, 0098) run the game's weapon in
every on-foot frame through `ctrl.weaponHook` (the aim ray and input before the intent, the weapon
state machine FUN_00579570 and the clip requests after the move), install the aimed clip tables
through `SpeedAnimator.setTable` / `playRequest`, and draw the reticle into the frame (`output.hud`,
so the PCRTC merge and the report capture include it). The camera's weapon views (types 0xd / 0xe,
the switch ramp, recoil, eye kick) run inside `FollowCamera` through `follow.weapon`. It needs
`data/aim/` from `native/assets/models/export_web_aim.py` and the `shoot` clip pack. Ladder and
other actions pause it. `?aim=0` turns it off. Keys (GUESS mapping, `aim.js`): left mouse button =
R1 (draw, then fire), Z or middle mouse button = R3 (weapon view <-> first person; with the gun away it draws straight into first person), C = circle
(holster). Tests: `node --import ./test_hooks.mjs test_aim.mjs`
and `test_aim_camera.mjs` (aim traces, `$AIM_TRACES`); `test_cam_traces.mjs` (camera traces, `$CAM_TRACES`, default tools/trace/out/cam).

**Timing.** `PlayerController.step()` is one PAL game frame (1/50 s, `TICK`), in which the
game advances `STEP` = 1.1999999 ticks. Actions are still timed in 60 Hz ticks: `step()`
runs `canEnter` / `update` from a 60 Hz accumulator, once or twice per game frame, with
`dt = ACTION_TICK` (1/60 s). This keeps the existing actions' tick counts unchanged; it is
a port convenience, not the game's timing (the game's states run once per frame with
`step`). An action that returns `{ locomotion: true }` in any of a frame's ticks lets the
game frame's locomotion run once.

## Registering

```js
import { ladderAction } from './actions/ladder.js';
ctrl.attach({ anim, motions });                   // play.js already does this
ctrl.addAction(ladderAction());                   // registry order = priority
```

Each action tick with no active action, `canEnter(ctx)` is asked in registry order. The
first action that says yes becomes active, and its `update` runs in the same tick.

## Module shape

| member | when | returns |
|---|---|---|
| `name` | | unique string |
| `canEnter(ctx)` | every tick while locomotion runs | bool; must not change state |
| `enter(ctx)` | once, on entry | optional **animation request** `{ clip, loop, rate, onEnd }` |
| `update(dt, ctx)` | every 60 Hz action tick while active (`dt = ACTION_TICK`) | a **move result** (below) |
| `exit(ctx)` | once, after `done` or `ctrl.cancelAction()` / respawn | nothing; release the animation here |

**Move result** (what the action owns this tick):

- `{ locomotion: true }`: locomotion moves him as usual and the action only animates
  (the ladder's jump-off fall uses this: the body's own fall moves him).
- `{ delta: {x, y, z}, heading?, onGround?, vy?, keepVel? }`: the action owns movement.
  `delta` is applied as is, in game units with -Y up (so up is negative y). The action
  does its own collision through `ctx.col`. Fields you leave out keep their values.
  Horizontal velocity is zeroed unless `keepVel`.
- `{ done: true, ... }`: leave after this tick. Combine it with either of the above.
- `null` / `{}`: stand still.

## ctx

| field | what |
|---|---|
| `ctx.ctrl` | the controller: `pos` (feet), `heading`, `vel {x,z}`, `vy`, `onGround`, `speed` (units per 60 Hz tick = the game's commanded speed C+0x1d8 / 60), `opts {radius, height}`, `gravity` (mode-0xc gravity, 235.2 u/s² = 0.0653 u/tick²), `scale`, `spawn`. Actions may write `pos/vel/vy/onGround`; when an action owns movement the controller finds the body's walk polygon again afterwards, and `onGround: false` hands him to the body's fall. |
| `ctx.input` | `{ x, z, run, keys, pressed, fwd, side }`: camera-relative move direction (through this frame's camera matrix, player.js `FollowCamera.basis()`), run, held key codes, key-down edges since the last tick (each edge is seen by one tick only), and the raw key axes |
| `ctx.col` | collision.js: `groundY`, `groundHit`, `move`, `capsulePush`, `segment` |
| `ctx.anim` | animator: `play(name, {loop, rate, onEnd})` crossfades to any loaded clip. `loop:false` plays it once, holds the last frame and calls `onEnd(name)`. Also `release()` (back to the game's locomotion clips), `setRate(r)`, `has(name)`, `duration(name)`, `time()`, `clipName()`. Every action crossfade lasts `ACTION_FADE` (player.js; GUESS, the game's blend length for these isn't decoded), so actions don't pick their own fade times. |
| `ctx.motions` | clip name -> sidecar entry (`vincent.json` / `vincent_<pack>.json` `clips`): `frames`, `loop`, `in_place_axes`, `root_motion_per_frame_glb`, `root_total_glb`, ... |
| `ctx.rootDelta(name, f0, f1)` | the clip's root motion from frame f0 to f1 (fractional frames are fine), in game space for the current heading and character scale. Add it to `pos` to move him the way the clip does. |
| `ctx.tick`, `ctx.TICK` | 60 Hz action ticks since reset, action tick length (1/60 s) |

With the detached debug camera (`?cam=`, until H) the follow camera keeps running unseen and takes no mouse input, so `ctx.input` (and WASD) stay relative to the follow camera's matrix, not the view on screen.

## Clips

- `vincent.glb`: mesh plus `h_st`, `h_wk`, `h_rn` (unchanged).
- Clip packs, loaded in the background after start (`?packs=a,b` to choose, `?packs=` for none):
  default `ladder` = `vincent_ladder.glb` (c_ld_*, h_ld_*); `jump` (h_jp0-4, h_div) and `shoot`
  (aim / fire / reload / h_swtch) still export but nothing on the page uses them. A pack holds only the joint nodes and
  the clips. Its tracks use the same joint names (`j00..j92`), so the main mixer binds them.
  Check `ctx.anim.has(name)` in `canEnter`, because a pack may still be loading.
- Every clip is in place. Its root motion is in the sidecar, per axis: x/z are always
  removed, and y is removed too when bone 1 moves vertically (`in_place_axes: "xyz"`: jumps
  h_jp1/2, ladder climbs, knockbacks). The action applies the root motion itself, e.g.
  `ctx.rootDelta(name, prevFrame, frame)`, or it uses its own physics.
- Loops (`loop: true`) have a closing key equal to frame 0. One-shots end on their last
  key, so `duration = (frames - 1) / 60`.
- 60 fps per clip frame is assumed (unproven, see HANDOFF).
- Every motion Vincent has, with purpose guesses: `data/vincent/motions.json`
  (`export_web_vincent.py catalogue`).

Make a pack: `python3 -I native/assets/models/export_web_vincent.py export <dump> --clips a,b,c --pack NAME`.

## Example: actions/ladder.js

Space (✕) with a ladder trigger box within the 7-unit ray along his facing (FUN_00544200)
gets on: `enter` starts `c_ld_u` / `c_ld_d`, and `update` returns `{ delta, heading,
onGround: false }` each tick, positions from the clip's root-motion curve between the game's
end points (5.6 snap, 4.5 rungs, 14 ticks a step; FUN_0059a5d0, FUN_00599f10). The exits
return `{ onGround: true, done: true }`. The jump-off plays `c_ld_jp`, returns `onGround:
false` (the body's fall starts) and then `{ locomotion: true }` until he lands.

## Notes for the action writers

- **ladder**: `c_ld_u` / `c_ld_d` get on at the bottom / top; `c_ld_uml`/`umr` and
  `c_ld_dml`/`dmr` climb one 4.5-unit step up / down, alternating hands;
  `c_ld_lid`/`rid` idle loops; `c_ld_uel`/`uer` and `c_ld_del`/`der` get off at the top / bottom.
- **jump** (not on the page): the e_jumps trace plays `h_jp0` -> `h_jp3` -> `h_jp4`, never
  `h_jp1` / `h_jp2`. Translate FUN_00575768's physics-body branch (take-off FUN_00357168,
  already in player.js `startFall(h)`) and the jump state class's motion requests.
- **shoot / reload** (not on the page): wait for worker 0077's aiming and weapon translation
  (FUN_00579570, notes/aim-research.md). The game has no held aim mode or aim camera.
