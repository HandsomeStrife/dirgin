# Player actions (play.html)

`player.js` runs Vincent as a small state machine: **locomotion** (idle / walk / run /
fall, the original controller) plus at most one **active action** taken from a registry.
Actions are separate modules in `native/web/actions/`. `actions/reload.js` is the worked
example. The interface is also written at the top of `player.js`.

With no actions registered, or with `?actions=0`, every code path is the original one.
A headless run of 1500 ticks gave exactly the same position, heading, speed, clip, rate
and camera as the pre-refactor `player.js`.

## Registering

```js
import { reloadAction } from './actions/reload.js';
ctrl.attach({ anim, camera: follow, motions });   // play.js already does this
ctrl.addAction(reloadAction());                   // registry order = priority
```

Each tick with no active action, `canEnter(ctx)` is asked in registry order. The first
action that says yes becomes active, and its `update` runs in the same tick.

## Module shape

| member | when | returns |
|---|---|---|
| `name` | | unique string |
| `canEnter(ctx)` | every tick while locomotion runs | bool; must not change state |
| `enter(ctx)` | once, on entry | optional **animation request** `{ clip, loop, rate, fade, onEnd }` |
| `update(dt, ctx)` | every 60 Hz tick while active (`dt = TICK`) | a **move result** (below) |
| `exit(ctx)` | once, after `done` or `ctrl.cancelAction()` / respawn | nothing; release the animation and camera mode here |

**Move result** (what the action owns this tick):

- `{ locomotion: true }`: locomotion moves him as usual and the action only animates
  (e.g. reload while walking, aim while strafing).
- `{ delta: {x, y, z}, heading?, onGround?, vy?, keepVel? }`: the action owns movement.
  `delta` is applied as is, in game units with -Y up (so up is negative y). The action
  does its own collision through `ctx.col`. Fields you leave out keep their values.
  Horizontal velocity is zeroed unless `keepVel`.
- `{ done: true, ... }`: leave after this tick. Combine it with either of the above.
- `null` / `{}`: stand still.

## ctx

| field | what |
|---|---|
| `ctx.ctrl` | the controller: `pos` (feet), `heading`, `vel {x,z}`, `vy`, `onGround`, `speed` (units/tick, measured), `opts {radius, height, maxDrop}`, `walkSpeed`, `runSpeed`, `gravity`, `scale`, `spawn`. Actions may write `pos/vel/vy/onGround`. |
| `ctx.input` | `{ x, z, run, keys, pressed }`: camera-relative move direction, run, held key codes, and key-down edges since the last tick (each edge is seen by one tick only) |
| `ctx.col` | collision.js: `groundY`, `groundHit`, `move`, `capsulePush`, `segment` |
| `ctx.anim` | animator: `play(name, {loop, rate, fade, onEnd})` crossfades to any loaded clip. `loop:false` plays it once, holds the last frame and calls `onEnd(name)`. Also `release(fade)` (back to the speed blend), `setRate(r)`, `has(name)`, `duration(name)`, `time()`, `clipName()` |
| `ctx.camera` | FollowCamera: `setMode(name, {dist, targetHeight, shoulder})` eases to an aim-style camera (`shoulder` = offset to the right); `clearMode()` eases back |
| `ctx.motions` | clip name -> sidecar entry (`vincent.json` / `vincent_<pack>.json` `clips`): `frames`, `loop`, `in_place_axes`, `root_motion_per_frame_glb`, `root_total_glb`, ... |
| `ctx.rootDelta(name, f0, f1)` | the clip's root motion from frame f0 to f1 (fractional frames are fine), in game space for the current heading and character scale. Add it to `pos` to move him the way the clip does. |
| `ctx.tick`, `ctx.TICK` | ticks since reset, step length (1/60 s) |

## Clips

- `vincent.glb`: mesh plus `h_st`, `h_wk`, `h_rn` (unchanged).
- Clip packs, loaded in the background after start (`?packs=a,b` to choose, `?packs=` for none):
  `vincent_jump.glb` (h_jp0-4, h_div), `vincent_ladder.glb` (c_ld_*, h_ld_*),
  `vincent_shoot.glb` (aim / fire / reload / h_swtch). A pack holds only the joint nodes and
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

## Example: actions/reload.js

F (R is the Report key) on the ground plays `h_st_rd` once and stands him still. The clip's
`onEnd` sets a flag, and `update` returns `{ done: true }` on the next tick. `exit` releases the
animation. The gun's own reload clip (07770580, gun bone 3 turns ~82 deg) isn't exported yet.

## Notes for the action writers

- **jump**: `h_jp0` take-off (3 f) -> `h_jp1` rise (12 up) -> `h_jp2` descend (12 down)
  -> `h_jp3` air loop -> `h_jp4` land. Chain them with `onEnd` + `ctx.anim.play`.
- **ladder**: `c_ld_u` / `c_ld_d` get on at the bottom / top; `c_ld_uml`/`umr` and
  `c_ld_dml`/`dmr` climb one 4.5-unit step up / down, alternating hands;
  `c_ld_lid`/`rid` idle loops; `c_ld_uel`/`uer` and `c_ld_del`/`der` get off at the top / bottom.
- **shoot**: `h_st_a` / `_ad` / `_au` aim loops (level / down / up), `h_st_f` / `_fd` / `_fu`
  fire one-shots (15 f), the same for `h_sq_*` (crouched) and `h_wk_*` / `h_rn_*` (moving,
  use `{locomotion: true}`); `ctx.camera.setMode('aim', {...})` for the aim camera.
  Aimed strafe and backpedal are run clips only: `h_brn_*` (back), `h_lrn_*` (left) and
  `h_rrn_*` (right), each with `_a`/`_au`/`_ad`. There is no aimed strafe walk, so a sideways
  walk plays the run at a lower rate. Rebuild the shoot pack with them:
  `python3 -I native/assets/models/export_web_vincent.py export tools/gsdump/dumps/z128_hero_163235.bin --pack shoot --clips h_wk_a,h_rn_a,h_sq_a,h_st_a,h_sq_f,h_st_f,h_wk_ad,h_rn_ad,h_sq_ad,h_st_ad,h_sq_fd,h_st_fd,h_wk_rd,h_rn_rd,h_sq_rd,h_st_rd,h_swtch,h_wk_au,h_rn_au,h_sq_au,h_st_au,h_sq_fu,h_st_fu,h_fwk_rd,h_sqm_rd,h_frn_rd,h_brn_a,h_brn_au,h_brn_ad,h_lrn_a,h_lrn_au,h_lrn_ad,h_rrn_a,h_rrn_au,h_rrn_ad`
