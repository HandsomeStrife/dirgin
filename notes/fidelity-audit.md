# Fidelity audit: browser gameplay vs the game's code (worker 0070, 2026-10-26)

Sections 2-3 updated by worker 0078 (2026-10-27) after the invented behaviour was removed.

Read-only audit of `native/web/{player,play,room,collision}.js` and `native/web/actions/*.js`
against `analysis/decomp/` and the ELF (SLES_541.85, PAL). Rule (CLAUDE.md): gameplay must be a
translation of the game's own functions. Line numbers are a snapshot: two workers were editing
`shoot.js`, `jump.js` and `play.js` during the audit.

Source categories:
- **DECODED**: our code follows a decoded game function (address given).
- **DATA**: the value is read from a game file or the ELF, but the code around it is ours.
- **GUESS**: a stand-in for something not decoded yet. It should be marked `// GUESS:` in the code.
- **INVENTED**: behaviour the game doesn't have, or that has a known, different game function.
  Translate it or remove it.

Difficulty to translate: **S** = under a day, the decode is done or nearly done. **M** = some decoding
left. **L** = a large function or chain with runtime data still to trace.

## 1. The game's per-tick player path (found during this audit)

The movement mode is `char+0x1c8`, and the movement-mode dispatcher is **FUN_0034c388**. Its
second `switch` sends mode 10 to FUN_003539b0 (walk), 0xb to FUN_003543b8 (airborne) and 0xc to
the physics body, where z128 runs: FUN_00358fb8 + **FUN_003590c8** → FUN_00357d48 (walk mesh) →
FUN_0035a440. The mode-0xc branch also derives `char+0x1d8` (ground speed in u/s) from the
measured displacement: `|Δpos|·60 / *(DAT_0046e290+4)`, with a 0.1 floor and a 0.001 dead zone. It
sets the facing `char+0x90` from the move direction when flag 0x4000 is set and the turn is under
80° (1.3962635 rad). It also sets the on-ground bit 0x200 from FUN_003577c8.

Call tree, top down (✔ = already decoded or translated in the port, ◐ = values read but the code
is not translated, ✘ = not decoded):

```
FUN_00322f88  frame: step = vblanks × scale (PAL ×1.2), cap 5 ticks          ◐ (research §0)
└ FUN_00323128  world update
  ├ FUN_0053b500  actor list update → actor vtable                               ✘
  │ └ FUN_005747a0  PLAYER TICK (player vtable 0x58f320 slot 3, ptr at 0x58f33c)  ✘
  │   ├ FUN_005734a8  actor base: FUN_005712c8 (equip/motion slots), FUN_0055af58  ✘
  │   └ FUN_005786e8
  │     ├ FUN_00578748  MOVE INTENT: stick ramp, walk/run speed, turn, air control ✘ (this audit read it)
  │     │   → FUN_0034af98 (set char+0xc0 dir, +0xcc speed/tick)                 ✔ read
  │     │   → FUN_0034b638 (set char+0x2c0 turn target, +0x2cc turn rate)        ✔ read
  │     │   → FUN_0034e960 (rotation mode char+0x214: 4 / 6)                      ◐
  │     │   → FUN_0034a968 / FUN_0034b3a8 / FUN_0034b2c8 (air velocity get/set)   ✘
  │     ├ FUN_00579570  weapon state machine (fire interval, reload)              ◐ (research §5)
  │     └ FUN_005799f0  (small; weapon/HUD side, not read)                       ✘
  │ input handlers (controller vtable at 0x4a0940, called by the input system;
  │ caller not found in two attempts):
  │   FUN_003320c0  R1/L1/○ weapon buttons                                        ◐
  │   FUN_00333150  ✕ jump/ladder, stick → player+0x2c0 (dir), +0x298 (0..255)    ◐
  │ player state changes: FUN_0055b8e0 (state byte +0x104; state class
  │   table DAT_004eedf0[state]); enter/exit hooks FUN_005738d0 / FUN_00573a38;
  │   jump state class vtable 0x5a5b78 (FUN_00598f50 = motion timeline)          ◐
  └ FUN_00348fe8  character list
    └ FUN_00343160  character tick (motion, attachments)                          ✘
      └ FUN_0034ea18  movement tick: motion time += step, rotation (FUN_0034de50)  ✘
        └ FUN_0034c388  MOVEMENT-MODE DISPATCH (char+0x1c8)                       ✘ (structure read)
          ├ 10  FUN_003539b0  walk mode                                           ◐ (values only)
          ├ 0xb FUN_003543b8  airborne                                            ◐ (values only)
          └ 0xc FUN_00358fb8 + FUN_003590c8 → FUN_00357d48 → FUN_0035a440         ✔ walk part (navMove)
```

So the game splits a tick into two halves. The **player half** (input → state → intent:
FUN_00333150, FUN_00578748) writes direction, speed and turn target into the character. The
**character half** (FUN_0034ea18 → FUN_0034c388) moves the body, turns it and measures its speed.
Our `PlayerController.locomotion` merges both halves into one invented function.

## 2. Audit table

**Updated by worker 0078 (2026-10-27, "strip the inventions").** Every row was re-checked
against the code as it now stands (player.js / play.js / cloth.js / gun.js / actions/ladder.js;
the locomotion and cloth translations of workers 0072-0074 are in it). The jump (`jump.js`),
shooting (`shoot.js`) and reload (`reload.js`) are gone from the page, and so is every row
marked "removed (worker 0078)". Rows the earlier workers already replaced say so. Line
references are function names, since line numbers move.

| behaviour | our code | source | game function that does it | diff. | notes |
|---|---|---|---|---|---|
| Game frame = one logic step, PAL step 1.2 ticks (TICK 1/50 s, STEP 1.1999999, DT) | player.js `TICK`, `STEP`, `DT`, `step()` | DECODED | FUN_00322f88 / FUN_0040ce28: step = vblanks × 1.2 (DAT_004eeb24), DAT_004eeb28 = step / 60 | — | Was "tick 1/60, fixed 60 Hz" (worker 0072/0074 changed it). |
| Frame catch-up: up to 8 game frames per browser frame, then the time is dropped | play.js `frame()` | GUESS (port convenience) | the game caps its step at 5 ticks (FUN_00322f88) | S | Labelled in play.js. Only matters when the tab stalls. |
| Action registry (`canEnter` in registry order, one active action) | player.js `step()`, `addAction` | GUESS (port scaffold) | Player state byte +0x104, FUN_0055b8e0; ✕ → ladder check in FUN_00333150 | L | Was INVENTED for its ordering rules. With one action (ladder) left there is no ordering. Entry needs the decoded trigger and the ground (states 2–5). Replace with the state machine when it is translated. |
| Stick byte ramp (±25 × step per frame toward 0 / 235 / 255) | player.js `intent()` | DECODED | FUN_00578748, ramp bytes P+0x29a/0x29b from FUN_005748b0 | — | Was "accel 14, eased velocity" (INVENTED); translated by 0072. Trace: stick bytes match. |
| Walk / run speed from the ramped byte (walkFrac·b/235, run curve, × C+0x1d4, ≤ 0.75 → 0) | player.js `intent()`, `GAME.maxSpeed/walkSpeed` | DECODED (values DATA: runtime record values read from the save state) | FUN_00578748 states 2–5; FUN_00317618 | — | Was "clip root speed" (INVENTED rule). b_walk / c_run: speed error < 4e-6 u/s. |
| Keyboard → stick tilt (W = byte 53, Shift = 255) | player.js `KEY_WALK`, `KEY_RUN`, `stickInput()` | GUESS (mapping) | The game has no run button; tilt sets the byte | S | Labelled. |
| Turn (rate (b/512 + 0.5) × 0.25, rotation mode 4, FUN_00323318 per-frame fraction) | player.js `intent()`, `rotate()`, `fracStep()` | DECODED | FUN_00578748 → FUN_0034b638; FUN_0034de50 mode 4; FUN_00323318 | — | Was "11 rad/s" (INVENTED). d_turn_circle: heading error < 1e-5°. |
| Move direction C+0x90 (flag 0x4000 clear in z128) | player.js `moveTick()` | DECODED | FUN_0034c388 case 0xc | — | Was "missing". |
| Ground speed C+0x1d8 = commanded speed × 60 (flag 0x2000 clear) | player.js `moveTick()` | DECODED | FUN_0034c388 case 0xc | — | Was "exp smoothing" (INVENTED). |
| Locomotion clip choice idle / walk / run from C+0x1d8 (≤ 0.01, ≤ 30 / 29.9) | player.js `SpeedAnimator.pick()` | DECODED | FUN_00346860, table C+0x5f4 | — | Was "hysteresis 0.12 / 0.06" (INVENTED). Clip mismatches vs b_walk / c_run / d_turn_circle: 0. |
| Locomotion table install (first frame C+0x1d8 ≥ 5) | player.js `gameFrame()` | GUESS | writer of C+0x658 for the player not found (two attempts) | M | Labelled. Matches the traces' install frame. |
| Clip playback rate clamp(speed / root, 0.1–1.1, floors 0.5 / 1.0) × 0.5 | player.js `gameFrame()` | DECODED | FUN_00346c10, FUN_003453e0 | — | Was "0.25–1.6" (INVENTED). Rate error ~1e-7. |
| Gait switch keeps the clip frame (clamped to n − 0.5) | player.js `switchTo()` | DECODED | FUN_00346860 → FUN_00345448 → FUN_005a8fd0 | — | Was INVENTED. |
| Locomotion blend (C+0x591 + 1 = 11 units, 21 into idle) and clip advance rate × weight × step | player.js `switchTo()`, `gameFrame()` | DECODED | FUN_0033ff18, FUN_00340bb8, FUN_005aa5b8 / FUN_005aa2a8 | — | Blend weight error ~1e-7 vs the traces. |
| Action clip crossfades (one length, `ACTION_FADE` = 11/60 s) | player.js `ACTION_FADE`, `play()`, `release()` | GUESS | FUN_00342360's blend-length argument, not decoded | M | Was 0.06–0.2 s per call site (INVENTED). Now one labelled constant (the locomotion blend's default); actions can't set their own. |
| Wall response (velocity × max(f, 0.5)) | — | removed (worker 0072) | FUN_00357d48 slides; no damping | — | Gone with the old locomotion. |
| Physics-body walk on the walk mesh | collision.js `navMove`, player.js `integrate()` | DECODED | FUN_00357d48, 0x02 crossing FUN_00359f38 | — | The controller now keeps the polygon (body+0x110) between frames. |
| Walk-polygon re-search after spawn / after an action moved him | player.js `locate()` | GUESS | the game keeps body+0x110; the ladder states' hand-back isn't decoded | S | Labelled. Not used while locomotion runs. |
| Physics-body step: velocity, ground / air states, landing | player.js `moveTick()`, `integrate()` | DECODED (in part) | FUN_0034c388 case 0xc → FUN_00358fb8 + FUN_003590c8 (states 0 / 2) | L | FUN_0035a440 (push-out, slopes) not translated: missing. |
| Landing on a 0x10 (drop) polygon → body state 3 | player.js `integrate()` | GUESS | FUN_00357860(body, 3); state 3 not translated | M | Labelled; lands as state 0. |
| Player state 2 ↔ 9 when the ground bit changes | player.js `moveTick()` | GUESS | FUN_00537f38 returns 9; the event and the landing aren't decoded | M | Labelled. Selects the air-control branch below. |
| Step up / snap down 4 | — | removed (worker 0072) | mode 0xc has no step rule | — | |
| Fall gravity (235.2 u/s², × 0.8 after the apex, +0.01, no cap) | player.js `startFall()`, `gravityStep()` | DECODED | FUN_00357168, FUN_003579a8, FUN_00356358 (cfg+0xcc × 196) | — | Was "0.05 / cap 6" (INVENTED). |
| Respawn when 500 below the spawn | — | removed (worker 0072) | — | — | H (back to the spawn) stays as a debug key, not a game feature. |
| Air control (states 8/9/0x1f–0x21 with mode 0xc) | player.js `airIntent()` | DECODED | FUN_00578748 | — | Runs during walk-off and ladder jump-off falls (state 9). The firing ×0.3 isn't applied (no shooting). |
| Air push direction with the stick released | player.js `stickInput()` air path, `airIntent()` | DECODED (worker 0079) | FUN_00333150 LAB_00333760 → FUN_00574938/FUN_00574920 write P+0x2c0 = raw stick (no 0.05 gate) and P+0x298 in states 8/9/0x1f–0x21; FUN_00578748 length test xyz only (0x578eac), push k = step×cap×0.075 (0x578ee0) | L | Trace i_air_release: released → decay 0.025×cap/frame; airIntent max error 2.2e-7 u/tick over 72 air frames. Ladder jump-off (state 9, no key): speed stays 0. Light stick (0 < \|v\| ≤ 0.05) is not in the trace. |
| Jump take-off, mode-0xb values (1.333 u/tick, gravity 0.07) | — | removed (worker 0078) | FUN_00575768 physics-body branch (z128) → FUN_0034a8e0 / FUN_00357168 | M | Was labelled DECODED; it was the mode-0xb branch, so INVENTED for z128. `startFall(h)` already has the take-off; h = cfg+0xc4 + 1 is not traced. Later job. |
| Jump gravity 0.07 | — | removed (worker 0078) | mode 0xc: 0.0653 (in player.js) | — | |
| No air control in the jump | — | removed (worker 0078) | air control is in player.js now | — | |
| Jump integration (`y += vy + g/2`) | — | removed (worker 0078) | FUN_003590c8 | — | |
| Air wall bounce −0.25 | — | removed (worker 0078) | FUN_003543b8 (mode 0xb only) | — | |
| Head bump | — | removed (worker 0078) | none in mode 0xc | — | |
| Jump landing search (`min(4, 1 + speed)`) | — | removed (worker 0078) | FUN_003590c8 / FUN_003577c8 | — | |
| Jump clip timeline (h_jp0 → h_jp1 → h_jp2 → h_jp3 → h_jp4; land 14 / 6 ticks) | — | removed (worker 0078) | jump state class 0x5a5b78 (FUN_00598f50) | M | **Label was wrong:** said DECODED, but the e_jumps trace plays h_jp0 → h_jp3 → h_jp4 and never h_jp1 / h_jp2. |
| Second (mid-air) jump | not done | missing | FUN_00575768 (+0x79c bit 0x20), gate FUN_00575698 | M | |
| Air-shot hop (3.33 u, max 3) | — | removed (worker 0078) | — | — | **Label was wrong:** the trace shows no hop for a mid-air shot (HANDOFF, worker 0069). |
| Ladder trigger: 7-unit ray along the facing into the trigger box | ladder.js `triggers()`, `inBox()` | DECODED | FUN_00544200, FUN_00576c88 / 576de8 | — | The ±3 y slack and the "within 60° of box +z" test were INVENTED: removed (worker 0078). |
| Ladder trigger ray origin 2 above the feet | ladder.js `RAY_UP` | GUESS | rays from FUN_005635e0 / FUN_00563678 points, not decoded | S | Labelled. |
| Ladder button: Space = ✕ for mount and jump-off | ladder.js `KEY` | GUESS (mapping) | ✕ (id 0xd), ladder before jump, FUN_00333150 | S | Was E for the mount (differed from the game's one button). Open (review of worker 0102): ✕ again during h_jp0's 3 frozen frames can enter the ladder with a jump live; the game's ladder entry has no state test before FUN_003455e8(char, 0x92 / 0x9b), which is undecoded (likely a motion-permission check). |
| Ladder snap 5.6, rung 4.5, 14 ticks per step, hand alternation | ladder.js `SNAP`, `RUNG`, `STEP_TICKS`, `next()` | DECODED | FUN_0059a5d0, FUN_00599f10 | — | `BODY_FWD 5.5` and `MOUNT_RISE 6.1` are DATA (clip) for the in-place export. |
| Ladder positions along each clip's root-motion curve between the game's end points | ladder.js `move()`, `advance()` | DATA | FUN_0059a5d0 (lerp over half the clip) for the mount; the rest not decoded | M | The curve shape is the clip's, the end points the game's. |
| Ladder step → idle clip at once with no key | ladder.js `next()` | GUESS | FUN_00599f10 settles 32 ticks after a step | S | Labelled. |
| Ladder top exit (no face 20.25 above the feet) | ladder.js `TOP_PROBE` | DECODED | FUN_00346fc0, FUN_0059b208 | — | |
| Ladder top landing ("floor within 3 of the top trigger", else the trigger line) | ladder.js `topLanding()` | GUESS | not decoded | S | Labelled. |
| Ladder bottom exit (c_ld_der/del drop 6, 0.5 margin) | ladder.js `next()` | GUESS | FUN_003470d8 probe (5.5 behind, down 8.25, class 0x01), not ported | S | Labelled. |
| Ladder jump-off: c_ld_jp on its clip curve, then the body's fall with c_ld_fa0 | ladder.js `jumpOff()`, `update()` fall phase | DECODED (fall) / DATA (clip) | fall: FUN_00357168 (h = 0) + FUN_003579a8; FUN_00562d30 (jump-off speeds) not decoded | M | Was "gravity 0.07, cap 6, push 0.6" (INVENTED): removed (worker 0078). No push off the ladder until FUN_00562d30 is decoded. |
| Ladder stick deadzone (W/S digital) | ladder.js `next()` | DECODED (±0.3) | FUN_00333150 → FUN_00576f08 / 576f98 / 577038 | — | |
| Ladder warp `?ladder=N` | ladder.js `warpTo` | test aid (URL only) | — | — | Not gameplay. |
| Aim as a held mode (RMB/Q, strafing) | — | removed (worker 0078) | the game has no aim mode; R1 fires toward the reticle, R3 zooms | M | Worker 0077 decodes aiming and the reticle. |
| Aim pose pitch blend (a/au/ad) | — | removed (worker 0078) | not found | M | |
| Strafe direction blend weights | — | removed (worker 0078) | not found | L | |
| Upper-body / additive fire overlays | — | removed (worker 0078) | not found (aimed clips are whole clips) | M | |
| Aim hold 45 ticks after a shot | — | removed (worker 0078) | not found | — | |
| Aim / fire weight ramps | — | removed (worker 0078) | — | — | |
| Turn to camera while aiming (16 rad/s) | — | removed (worker 0078) | FUN_00578748 0x80–0xbf branch (melee); fire facing not traced | M | |
| Aim movement accel 14 | — | removed (worker 0078) | FUN_00578748 | — | |
| Zoom FOV 65 → 25 with an ease; look speed scaled by FOV | — | removed (worker 0078) | FUN_0032a420 / FUN_0032a390 (R3), camera mode 4 | M | |
| Fire interval 38, first delay 0 | — | removed (worker 0078) | FUN_00579570, FUN_00574a80, FUN_003320c0 | S | Values stay in actions-research §5 for the translation. |
| Bullets 100 u/tick, range 565; muzzle flash sprite; spark life 60 | — | removed (worker 0078) | FUN_00552dd8, FUN_00556068, FUN_005442d8, FUN_00577480 | S | No muzzle flash exists in the game (research §4). |
| Magazine 9, reload 80 ticks, reload clip | — | removed (worker 0078) | FUN_00579570 state 7, FUN_00319f80 | S | Reload needs a magazine below 9, so it waits for the weapon translation. |
| Camera kick per shot | player.js `place()`, aim_core.js | DECODED (worker 0098) | FUN_00577480 → FUN_0032a320(0.75, 0.01); FUN_0032ace0 l.2412 eye += forward x kick | — | See 'Aiming' below. |
| Camera follow (type 0xc, view mode 1): base follows the player per axis (0.125 / 0.5 / 0.15 × step), look-at 17 above it, eye offset (0, 1.25, −38) through rotX(pitch)·rotY(yaw) with the y part × (0.8 |cos| + 0.2), look-at z − 3/−4 × pitch, follow nudge (angle change of the look point seen from the old eye, quantised to π/4096, capped at the stick rate), auto turn-behind toward C+0xc0 by the left-stick gain (110°/140° rules), distance in at once / out at 0.03 per frame (× 0.1 while he walks away, + recede × (d − 25)/35) | player.js `FollowCamera.tick()` / `place()` | DECODED | FUN_00330478 → FUN_0032ace0; constants: type object (FUN_00330138; runtime pivot/offset/rates/FOV from the trace), CAM+0x150/+0x160, DAT_0046e404/408/444/448/4dc/4d8/500/504, DAT_004a0650 | — | Worker 0089. Camera runs before the player each frame (it reads last frame's C+0x30; the player's FUN_003295c0 reads this frame's matrix). Six PCSX2 traces (TRACE_FIELDSET=cam: idle, walk, run, right-stick turn, run in a circle, right-stick look), open loop and closed loop with the player: eye ≤ 0.0075, look-at ≤ 0.001, yaw ≤ 6e-6 rad, pitch ≤ 2e-4 rad. Mutations (slow ease, chunk filter, angle quantiser, auto-turn) each fail a trace. Runtime RAM differs from the ELF: DAT_0046e3f0 = 73.5 (ELF 65, so the FOV pitch term is 0), DAT_0046e500 = 60 (ELF 35), DAT_0046e3f8 = 50 (ELF 54). |
| Camera collision: 5 rays from the view-axis point to the eye and a 6×6 square around it (DAT_0058bb90), nearest hit pulls the eye to the same fraction, then forward by 3·sin(angle to the wall) | player.js `cameraRays()`, `place()` | DECODED | FUN_00544f90 → FUN_0054c848 (probe index 6 = flags 0x18b: class bit 0x04 packets, FORMAT.md) | — | Spawn pull-in (30.54 vs 38) and its slow release while walking match the trace to 0.007. |
| Camera collision: which DCOL files the camera's map object holds | player.js `CAM_COL_FILES` = [1, 2] | GUESS | FUN_0053bb58 (current map object) not decoded | S | With all files, file 3's deck blocker (z −1120.1) stops the eye at ±50° pitch and the stick-look trace is off by 22 units; with files 1–2 it matches (0.005). The real rule (chunk streaming?) is not decoded. Tag-1 primitives and FUN_005449f0's objects are left out (no change on the traces). |
| Camera pitch limits +50° / −54° | player.js `PITCH_MAX` / `PITCH_MIN` | DECODED | FUN_0032ace0, DAT_0046e3f8 (50 in RAM) / 0x46e3fc | — | Traced clamp at 0.8727 / −0.9425 (cam_look). Was ±54. |
| Look stick → yaw / pitch: x·|x|, y·|y| (y negated), magnitude ramp +0.01/+0.05 × step up, −0.075 × step down (snap with the mouse), × type +0x30/+0x34 (0.05) × step × DAT_004a0650[8] (1.0) | player.js `tick()` | DECODED | FUN_003256d8, FUN_00254ac8, FUN_0032ace0 (CAM+0x1ec/+0x1f0) | — | cam_look and cam_turn traces exact. |
| Mouse look: right stick += mouse counts × 0.1 (DAT_004e1004), clamped ±1, ÷ step | player.js `addMouse()` / `MOUSE_SCALE`, play.js | DECODED (the game's USB-mouse path; DAT_004e0ff8 = 1 in the state) | FUN_00254ac8, FUN_002553e8, FUN_003256d8 (DAT_004e17d0) | — | Not traceable in PCSX2 (no USB mouse); decoded from code only. |
| Mouse: one browser pixel of movementX/Y = one USB mouse count | player.js `MOUSE_SCALE` comment | GUESS | — | S | The PS2 mouse's counts per cm are unknown. The ±150 px spike clamp stays a port GUESS. |
| Camera reset (spawn, H, ladder warp): snap flag CAM+0x1e8 0x2000 (next frame base = player, no ease), CAM+0x1c0 = 1000 (so the distance is the measured one, after collision), ramp CAM+0x1ec = 0 | player.js `FollowCamera.reset()`, `tick()`; play.js `ctrl.onTeleport` | DECODED | FUN_00329f68 → FUN_003285d0; snap branch in FUN_0032ace0 (CAM+0x1e8 & 0x2000) | — | Worker 0090. reset() places the eye with the collision, as the first frame does (no pop from 38 to 30.5). |
| Camera reset angles and counters: the save state's (yaw 0.9083, pitch −0.0383, idle counter 30, gain 0) at spawn and H; the ladder warp keeps the current yaw/pitch (GUESS: view 1 = the current view); the reset also clears the mouse latch DAT_004e17d0 (page aid) | player.js `CAM_SPAWN`, `reset()` | GUESS | FUN_003285d0 takes the angles from render view 1's forward (FUN_003ee5e8(1) + 0x20), which holds another view in the save state; when the game calls FUN_00329f68 (stage entry, respawn, warp) is not decoded | S | The page's spawn heading (0.908) already matches that yaw. |
| Camera base follow at a 15+ drop: when \|Δy\| ≥ 15 the base lands on the player in one frame; below it, Δ × min(rate × step, 1) when FUN_003264e0() = 0, else Δ × rate | player.js `tick()` | DECODED | FUN_0032ace0 (ABS(dy) < 15.0); FUN_003264e0 = (FUN_00241410() = DAT_0046b1d8) == 0, DAT_0046b1d8 = 1 in the state (set by FUN_00241400), cap DAT_004a05f8 = (1, 1, 1) | — | Worker 0090. Was inverted (raw rates at 15+). Test: worker 0090 scratchpad test_drop.mjs (mutations of the 15 test, the old structure and the snap each fail it). |
| Camera auto turn-behind / follow nudge switch FUN_00253f50 = DAT_004b8ac8 bit 0 && option DAT_004e1070 | player.js `fun253f50` | DECODED + save-state values | FUN_00253f50, FUN_002419d8(0) | — | Both 0 in the state: on. The option's menu is not ported. |
| Look-stick deadzone DAT_0046e464 = 0.5: past it on either axis, FUN_00324440(10) sets bit 10 of DAT_0046e290+0x14 (sx/sy unchanged; the idle counter tests sx = 0, as the game) | player.js `tick()` `flags14` | DECODED; gate GUESS | FUN_0032ace0 (~717–742); FUN_00324440's FUN_00326068(7) gate taken as open (not decoded) | S | Nothing in the port reads the bit yet (FUN_00324530 / FUN_00324598 in the game). |
| Mouse latch DAT_004e17d0: set by mouse counts, cleared when the pad stick (x\|x\| or y\|y\|) passes 0.3 | player.js `tick()` | DECODED | FUN_00254ac8 | — | Worker 0090. Test in native/web/test_camera.mjs (`node --import ./native/web/test_hooks.mjs native/web/test_camera.mjs`; mutation fails it). Not ported: the arrow keys adding ±1 to the look stick in USB mode (DAT_004e1069 = 1, FUN_00429b28 0x25–0x28). |
| Camera drawn between game frames (eye, look-at and forward lerped by the tick fraction) | player.js `update(alpha)` | port convenience | — | — | Same as the body's `ctrl.lerp`; the game draws once per frame. |
| Camera paths not translated: view mode 4 (scope; not reachable with this loadout), target lock (CAM+0x1e8 0x80000/0x10000, CAM+0x218), event cameras (CAM+0x1e8 0x400000/0x200000), blend CAM+0x180/+0x188/+0x18c, the below −54° pull CAM+0x1ac, the look-at wall ray (FUN_0053bb58 vtable+0x17c), the P+8 ≠ 0 path, the reticle lead CAM+0x1fc/+0x200, the jump flag 0x1000000 / CAM+0x220 | — | missing | FUN_0032ace0 | L | Worker 0098 translated view modes 2–3, the switch ramp, recoil, kick, FOV / fade and the shoulder CAM+0x1a0/+0x1a4 (section 'Aiming'). |
| Aim camera (shoulder) | — | removed (worker 0078) | no aim camera in the game | — | |
| Camera-relative stick | play.js `readInput()` / `aim()`, player.js `stickInput()` / `stickToWorld()` | DECODED (concept; `stickToWorld` exact for the pad) | FUN_00333150, FUN_003295c0 | S | The keys use the translated camera's matrix rows 2 / 0 of the same frame (worker 0089). Closed-loop replays (player + camera from the pad): positions as the open-loop baseline (≤ 7.6e-3). |
| Input mapping (WASD, Shift, Space = ✕) | play.js `readInput()`, ladder.js | GUESS (mapping) | pad table 0x46bae0, FUN_00254810 | S | E, F, Q, RMB, LMB, Z bindings removed with their features. |
| Cape | cloth.js | DECODED | FUN_005b0d30 (see "Web frame loop and cape cloth") | — | |
| Gun mesh, static under j56 | gun.js | DECODED (attach) / DATA (mesh) | FUN_00316270 marker 15 → bone 56, FUN_005ab8f0 | — | Moved out of shoot.js (worker 0078). Parts list: GUESS (BYO table). No gun motion. |
| Clip clocks | player.js `gameFrame()` (locomotion); three.js mixer (action clips) | DECODED (locomotion) / GUESS (actions: 60 clip frames/s) | FUN_005aa2a8; FUN_00599f10 `(frames − 2)/60` | M | The ladder sets its own rates from the game's step length. |
| Display output: PCRTC merge, both read circuits on the same framebuffer, circuit 2 one line lower and one VCK right, out = 128/255·fb(y) + 127/255·fb(y−1, x−1/6 px), BGCOLOR 0 where circuit 2 hasn't started | output.js, full-res path (the default); `?res=native` uses the VCK merge row below | DECODED | FUN_0040ce28 → FUN_0040d228(env, buf, 1, 1) every frame: PMODE = env|0x8021, DISPFB1 = DISPFB2, DISPLAY1 = DISPLAY2 − (1<<12 | 1); all 4 GS dumps: ALP 0x80, MMOD 1, SLBG 0, DH 511, MAGH 5, SMODE2 INT 1 FFMD 0 | S | Worker 0082. Scaled to the canvas as one line = H/512 px, one VCK = (4/3·H/425.5)/6 px, sampled bilinearly at the exact offset. Default: OFF in the full-res default (sharp: Dan's choice 2026-10-28), ON with `?res=native`; `?outblur=1`/`0` forces it. PCSX2 here runs `pcrtc_antiblur = true`, which drops this offset, so PCSX2 captures don't show it. |

## 3. Counts

Rows are counted by their first label. Removed rows count as removed. (Worker 0089 replaced
the camera rows: 1 GUESS + 1 DECODED + 1 GUESS became 5 DECODED, 3 GUESS, 1 port
convenience, 1 missing; the table below is not recounted.) Section 2 now has
69 rows (some old rows were split; the 0074 and BYO tables below are counted separately).

| category | worker 0070 (as written, 54 rows) | after worker 0078 (69 rows) |
|---|---|---|
| DECODED | 15 | 24 |
| DATA | 4 | 1 |
| GUESS | 8 | 16 |
| INVENTED | **24** | **0** |
| missing | 3 | 2 |
| removed | — | 25 |
| test aid | — | 1 |

Re-check at the start of worker 0078: 0072-0074 had already replaced the locomotion rows
(accel, turn, speed smoothing, hysteresis, rate, gait phase, gravity, snap, wall response),
but 4 jump rows labelled DECODED were INVENTED for z128 (wrong branch, and the clip timeline
the trace contradicts) and the air-shot hop was contradicted by the trace. The 20 left were:
the action registry ordering, action fades, 7 jump rows, ladder slack and jump-off fall, 7 shoot
rows (aim mode, strafe blends, overlays, ramps, turn-to-camera, aim accel, zoom look speed),
the follow camera and the aim camera. So INVENTED went 24 (as written) / 20 (re-checked) → 0.

## 4. Translation order

Status (worker 0078): 1 is done (player.js `intent()`), 2 is done for body states 0 / 2
without FUN_0035a440, 4 is done for rotation mode 4 and the locomotion clip rate. The jump.js
and shoot.js parts of 2, 3 and 5 no longer exist on the page: 3 starts from player.js
`startFall(h)`, 5 from worker 0077's aim research.

Goal: replace `PlayerController.locomotion/fall` and the jump's own physics with the game's two
halves, keeping navMove (already FUN_00357d48).

1. **FUN_00578748** (player move intent; 599 lines, read in this audit). Stick ramp ±25 per tick,
   walk/run speed curve, turn rate, the jump-state air control and the ladder speed (state 10: b ×
   0.03515625). This replaces player.js accel/turn/speeds and jump.js "no air control". Needs
   rec+0x70/+0x74 and char+0x1d4 (trace FUN_0055a370's record, FUN_0034b978's callers). **M**
2. **FUN_0034c388 case 0xc + FUN_00358fb8 + FUN_003590c8** (body integrator around the
   translated walk-mesh walker: gravity body+0xd0, take-off FUN_00357168, ground bit FUN_003577c8,
   speed char+0x1d8, facing char+0x90). This replaces `fall()`, `snapDown`, gravity 0.05 and jump.js's
   integrator, head bump and landing search. **L**
3. **FUN_00575768, physics-body branch** + FUN_0034a8e0 / FUN_00357168 / FUN_00356358 (jump
   height cfg+0xc4 + 1, gravity cfg+0xcc × 196). Trace cfg+0xc4 (lead: stage 06800001 section
   4). **M**
4. **FUN_0034ea18 + rotation (FUN_0034de50, FUN_0034b4c8, FUN_0034e960 modes 4 / 6)** and
   the motion-rate part of FUN_0034c388's first switch. This replaces the turn clamp, the speed
   smoothing and the playback rate. **M–L**
5. **FUN_00333150 / FUN_003320c0 + FUN_0055b8e0 state changes** (input → player state, ✕
   ladder-before-jump priority), then **FUN_00579570** (weapon state machine) to replace the shoot
   layer's timers. Lower urgency: the values are already right. **M**

After those: the camera (FUN_0032ace0: done for type 0xc on foot, worker 0089), the cloth sim in the web build (FUN_005b0d30, M, its
decode exists), the aim-blend / state-class motion requests.

## 5. Leads and unknowns (two attempts each, then stopped)

- Camera collision file set (worker 0089): FUN_0053bb58's map object; the traces need DCOL
  files 1–2 and not file 3. Two attempts (all files; current chunk only), then stopped with a
  labelled GUESS. Lead: what fills the map object's +0x18 vtable +0x17c list (chunk loading).
- The game's own keyboard mapping (worker 0089 lead): FUN_00254918 builds the left stick from
  keys 0x16–0x19 (±1 per axis, 0.5 with key 0x1a held) when DAT_004e0ff8 = 1 (set in the
  state). The page's KEY_WALK / KEY_RUN GUESS could be replaced from it.

- Who calls the controller vtable 0x4a0940 (FUN_003320c0 / FUN_00333150): not found.
- How FUN_0053b500 reaches the player vtable slot 0x58f33c: the actor vtable call at +0x214
  (likely), not confirmed.
- Consumer of the turn rate +0x2cc: FUN_0034b4c8 / FUN_0034de50, not decoded.
- cfg+0xc4 (jump height) and rec+0x70 / +0x74 (max / walk speed): runtime fields, sources not traced.
- Per-state motion requests for the ground states 2–5 (which clip, which rate): the state class
  table DAT_004eedf0 was not dumped.

## Bring-your-own-ISO build stand-ins (worker 0071, tools/byo)

| behaviour | our code | source | game function | difficulty | notes |
|---|---|---|---|---|---|
| Vincent's start position and facing (yaw, x, y, z) without a capture | export_p000_gpu2.py `NODUMP_PLACEMENT` | GUESS (solved against hero dump 163235, h_st f91) | the stage start record (lead: 06800001 section 1, record 0 'p_Vin') | M | Static viewer (index.html) only. Scale is no longer part of the GUESS: s = 1.0 DECODED (MP+0x10 unit rows; worker 0083). play.js no longer reads scene.char.s in either build. |
| Static Vincent pose in the viewer | build_web.sh `STATIC_CLIP`/`STATIC_FRAME` (h_st f91) | GUESS | the stage start motion | S | Viewer only |
| Gun parts shown | export_web_gun.py `NODUMP_PARTS` (frame0, barrel1); export_cerberus.py part pick | GUESS (list) / DATA (barrel0 dropped) | the weapon-parts loadout (bgd.bin section 13: handgun = parts #20 + #52) | S | Should come from the decoded default loadout. Worker 0091: barrel0 removed. GS dumps hero/185126/185138 draw none of barrel0's own triangles; its 4 triangles that copy barrel1's won by argmin ties (export_cerberus now ignores tied corners). Drawing both barrels z-fought (Dan's report 20261028T125732Z-cb61). |
| Start camera | p000_cape.py `HERO_CAM` (label only) | GUESS | the stage camera setup | M | |
| Sky dome | left out of the BYO build | missing | the sky container is not in the decoded room path | M | Only the capture build has it |

## Web frame loop and cape cloth (worker 0074)

| behaviour | our code | source | game function | difficulty | notes |
|---|---|---|---|---|---|
| Actions timed by a 60 Hz accumulator inside the 50 Hz frame (1 or 2 action ticks per frame) | player.js `step()` (`actAcc`, `ACTION_TICK`) | GUESS (port convenience) | The game runs each player state once per frame and scales by `step` (FUN_00322f88, 1.2 ticks per PAL frame) | M | Goes away as each action is translated to the per-frame `step`. Locomotion and the cape cloth already run once per game frame. |
| Cape cloth stepped once per game frame from the clip-advance hook | play.js `setupCapeCloth` via player.js `SpeedAnimator.drawHook` | DECODED (order) | FUN_005ab8f0: pose (lines 87-113), cloth FUN_005b2ef8 (line 143), then FUN_005aa5b8 advance + window countdown (line 218); arm FUN_005a95f0, turn re-arm FUN_005a6f40 | — | Replaced 0073's separate 50 Hz accumulator. Page replay of cw_walk/run/turn: window total/remaining equal to the game on every frame. |
| Cape drawn between game frames: cloth offset (pos - target at the game frame) added to the target at the rendered pose | play.js `setupCapeCloth().render()` | GUESS (display interpolation, port convenience) | none: the game draws once per frame | S | Keeps the pinned edge on the body when the render interpolates (ctrl.lerp). At a game frame's own pose it is cloth.pos exactly. |
| MP+0x440 bit 0x100000 held clear (blocks the turn re-arm when set) | cloth.js `mpFlag100000` | GUESS (meaning not decoded) | FUN_005a6f40 tests it | S | Clear in every trace record (mp_head). |
| Cloth buffer start state: pos = target, vel = 0 | cloth.js `reset()` | GUESS (not decoded) | the cloth buffer's init (not traced) | S | With k = 1 the game's step reaches its fixed point (vel.y 0.0222) within ~5 frames and pos == target exactly. |
| Cape skinned with the character matrix from before this frame's move | play.js `logicPose` (ctrl.prev / ctrl.prevHeading) | DECODED (writer) + traced (one-frame lag) | FUN_005ab8f0 skins and draws with MP+0x10; FUN_00343de8 writes it (character matrix via vtable+0x24 or C+0x5b0 -> FUN_005a6f40 -> FUN_005a6ea8). Its call order against the move FUN_0034c388 is not decoded | M | Adopted by worker 0083: per record the game's cape_target = the previous record's C matrix x skinned bones to 0.000 (this record's: 0.94 in run). |
| No walk move while an action owns movement (the ladder): `intent(owned)` treats the player as not in an on-foot state | player.js `intent()` | GUESS (the ladder's player-state number not decoded) | FUN_00578748 writes the walk move only for states 2-5 | S | Without it, climbing with W left a 15 u/s walk move that the jump-off fall inherited (manager review fix). The stick ramp still runs in every state. |
| Key edges seen by the first action tick of a frame only | player.js `step()` | GUESS (port convenience, with the 60 Hz accumulator) | the game reads the pad once per frame (FUN_00257de0) | S | Goes away with the accumulator. |
| Cloth restarted after a test-aid move (H, `?ladder=`) | play.js `ctrl.onTeleport` | test aid | — | S | The game has no such move. |
| Frame `dt` clamp = the 8-frame catch-up cap; mouse spike clamp ±150 px | play.js frame loop, `clampMv` | GUESS (port convenience, browser timing / input) | the game caps its step at 5 ticks (FUN_00322f88) | S | |
| An action that drops him (ladder jump-off) hands over in the fall state 9 | player.js `applyOwned()` | GUESS (same state switch as moveTick's 2 <-> 9) | FUN_00537f38 returns state 9 when the ground bit clears; the ladder's own exit state isn't decoded | S | Without it the first fall frame ran as on-foot and picked up the 15 u/s walk move (review fix). |

## Body pose in the run (worker 0083)

Oracle: `TRACE_FIELDSET=pose tools/trace/scripts/c_run_pose.json` logs the game's bone matrices, 93 x 0x40 at *(MP+0x3f8) (FUN_005a87a0 -> FUN_005b2d58; FUN_005b06d0 reads them for the cape target). In steady h_rn they equal `anim_rot.compose(decode_frame_trs(h_rn, frame - rate x step))` (the frame before this frame's advance) to 0.000 for all 93 bones. No partner clip (layer +8 empty), no blend-out layer (MP+0x44a = 0xff, MP+0x408 = 1.0), no post-pose callback (MP+0x414 = 0), MP+0x428 = 0x19 (no FUN_005ade20). So the clip decode, the local nlerp and the export are right; the web differences come after them.

| behaviour | our code | source | game function | difficulty | notes |
|---|---|---|---|---|---|
| Only the head layer and the blend-out layer pose the body; every other clip has weight 0 | player.js `SpeedAnimator.update()` | DECODED | FUN_005a9db0: FUN_005afa30 (layer MP+0x44a while MP+0x408 < 1), FUN_005afa58 (head MP+0x448 at MP+0x408) | — | **Was a bug:** stale clips were zeroed only `if (a.isRunning())`, which three.js reports false whenever timeScale = 0 (always, on the game clock). A finished blend's weights stayed live: steady run was 68% h_rn + 31% h_st + 1% h_wk. Page replay cape-target error, run 3.19 -> 1.63 (max 6.44 -> 3.70); model-space run pose now 0.02 from the game's. |
| Locomotion pose build: per-bone local sample (t/s lerp, q nlerp with hemisphere flip), blend-out layer at weight 1, head lerped in at MP+0x408, then compose; weight set before the draw | player.js `SpeedAnimator.poseLocal()` / `sampleLocal()`, `gameFrame()`; export_web_vincent.py `local_pose` (data/vincent/vincent_local.bin) | DECODED | FUN_005a9db0 -> FUN_005afa30 / FUN_005afa58; FUN_005af6c0 | — | Was three.js mixing model-space TRS (GUESS): up to 1.8 off in the blends, 0.5 in steady run, and the pose used the previous frame's weight (0.13-0.3). Checked on c_run_pose_blend (idle -> walk -> run): median 0.000-0.003, max 0.027. Action clips (ladder etc.) and their crossfades still use the mixer (GUESS row above). |
| Body and cape placed at the character matrix from BEFORE this frame's move | play.js `logicPose`; render loop draws (ctrl.lerp prev -> pos, clip prevFrame -> frame), which at alpha 0 is (prev, prevFrame) = the game's draw | DECODED (writer FUN_00343de8 -> FUN_005a6f40) + traced (lag) | FUN_005ab8f0 uses MP+0x10 | M | Applied. Per record, the game's cape_target = previous record's C matrix x skinned bones to 0.000 (this record's: 0.94 in run). |
| Vincent drawn at unit scale with the model origin at the character position | play.js `loadVincent()` (no scale), no FEET_OFFSET / measureSole, ctrl scale 1; export_web_vincent.py `rootT = 0` | DECODED | MP+0x10 has unit rows (save state); FUN_00343de8 -> FUN_005a6f40 copies the character matrix; the game's cape_target / skinned bones ratio is 1.000, origin = C position (feet) | — | Was GUESS: scale 0.9625 (fitted to the dump-posed mesh), a bind-bbox pivot [0.007, 12.13, 0.31] and a measured sole offset. Offline, those miss the game's cape_target by 0.72; unit scale at the origin: 0.012. Both halves (play.js and the glb root) share one invariant: model origin = character position, unit scale. The ladder's feet now sit SNAP - BODY_FWD = 0.1 from the face (was 0.3 at 0.9625). |
| Cape turn re-arm reads THIS frame's facing while the skin uses the previous matrix | play.js `setupCapeCloth` (`pose.facing`) | GUESS (traced, not decoded why) | FUN_005a6f40 | S | cw_turn / cw_run re-arm frames equal the game's with it. |
| After an action's fade-out (0.18 s) the pose switches from the three.js mixer mix to poseLocal | player.js `SpeedAnimator` | GUESS (port plumbing for the ACTION_FADE GUESS) | FUN_005a9db0 (the game has no such fade) | S | A gait blend starting inside the fade pops when the fade ends. Goes away when the action clips are posed by poseLocal too. |

## Character lighting (worker 0085)

Oracle: hero dump 163235, GIF vertex colours of Vincent's draws vs this model at h_st f91 (worker 0084's fit), save state for the runtime values (MP = C+0x7c8). Program D per vertex: rgb = min(vcol x (C0 max(L0.n,0) + C1 max(L1.n,0) + C2 max(L2.n,0) + A), 255). Error (max channel, levels) on triangles whose pose agrees with the dump (< 2 deg): body median 1, 96% <= 5; opaque hair median 1, 96% <= 5; alpha hair median 1, 79% <= 5. All triangles: body p90 12, alpha hair p90 62. The tail sits on bones whose pose differs from the dump (faces 9-17 deg off: 27, 31, 35, 54), not on the lighting.

| behaviour | our code | source | game function | difficulty | notes |
|---|---|---|---|---|---|
| Body and hair vertex colours lit per vertex by VU1 program D from the skinned game normals | play.js `LIGHT_VS`, `litPatch()`; glb extras.lit | DECODED | VU1 program D (port/vu/RENDER-MATH.md §1), normals skinned by VU0 | — | Was prelit (flat vcol, 126). Hair is lit (dump hair colours 22-184 reproduced, see above). |
| Light 0 + ambient: env slot 0 direction/colours x MP+0x450/128 | play.js `setupCharLighting`; export_web_light.py (bmd_env.py) | DECODED | FUN_005ab0b0 (env table DAT_004e6370 + slot x 0x120), FUN_00342240 sets +0x450 | — | MP+0x450 = 122 (save state; 123 fits the dump worse). Memory env equals the 06800012 file values. |
| Light 2: room light from the BMD light grids (least d^2/k in the actor point's cell, spot cone test), colour rgb x k x 100/d^2, not x +0x450 | play.js `pick()`, `frame()`; export_web_light.py | DECODED | FUN_0026a710 -> FUN_00270d08 (FUN_0026fb90 cell), FUN_005ab0b0 | — | Replaces bmd_env.py's FITTED HERO_POINT_LIGHT: at the hero frame the grid light (06800111, k 5.5) gives dir (-0.143,-0.391,-0.909); the save state's MP+0x110 is (-0.157,-0.368,-0.917) and MP+0x430 points at that record. |
| Light 2 smoothing while the chosen light changes (colour 0.1, dir 0.2 per frame, settle at d^2 <= 0.01) | play.js `frame()` on anim.drawHook | DECODED | FUN_005ab608 (MP+0x110/0x140/0x430) | — | Runs once per game frame after the cloth step. |
| Light-2 smoothing state at load: starts settled on the first light | play.js `frame()` | GUESS | MP+0x110/0x140/0x430 initial values not decoded | S | Only affects the first ~20 frames after load if the game starts at 0. |
| Actor point = MP+0x10 x bbox centre; bbox constant from the save state | export_web_light.py `BBOX_MIN/MAX` | DECODED (use) + save-state constant | FUN_005a72a8; bbox from FUN_005a8850 -> FUN_005b0380/FUN_005affb0 (not decoded) | S | Constant per model; decode FUN_005affb0 to read it from the model file. |
| Light 1: effect point lights (4-slot table, actor light MP+0x160) | not ported | DECODED (source), not ported | FUN_0030e208, FUN_0030e138/180/1e0, FUN_00271fb8 (colour c0 + c1 t + c2 t^2/2 from the effect) | M | Needs the effect system; table empty in the z128 save state. |
| Cape lighting | cape stays prelit (extras.lit = false, own material) | NOT DECODED | program D (dump: ambient-only cape corners = 126 x A x 122/128 exactly: 45,48,60) | M | Which normal the cape draw uses is open: skinned int8 normals (FUN_005b06d0), negated, eye-facing and abs all fail at this frame (lit/unlit agreement at chance; only 6/87 cape tris match the dump pose). Stopped after two attempts. |
| Gun, DAT_004fcaf8 bit-36 fixed rig, MP+0x41c/0x420 ambient tints, env-slot blend FUN_005ab738 | not ported | DECODED (source) | FUN_005ab0b0 | S | Inactive in z128 (save state: tints 0, flag clear, slot fixed). Gun lighting not checked. |

## Display resolution and output (worker 0087)

| behaviour | our code | source | game function that does it | diff. | notes |
|---|---|---|---|---|---|
| Scene drawn at 432 x 512 (FRAME FBP 224 FBW 448 CT32, SCISSOR 0-431 x 0-511, XYOFFSET 1824,1792), same every frame/field | output.js `FB_W/FB_H`, native `fbRT` | DECODED (GS dumps, all 3) | draw env; FUN_0040ce28 flip | — | Default. Was canvas resolution (3420x1912 for Dan, ~4x finer: hair gaps showed over the collar). |
| Projection: scrz = 448·0.5/tan(73.5°/2) = 299.97, ax 1, ay 1.5, centre (2048−11, 2048) | output.js `fitCamera` (native) | DECODED + save-state values | FUN_0040d670 / FUN_0040ce28 (scrz), FUN_0040d478 → FUN_00263c70 (sceVu0ViewScreenMatrix); struct 0x4fc7d0 +0x2e8/+0x2f8/+0x300/+0x304/+0x320/+0x324 | — | Equals the fitted hero camera (fit_dump_camera.py). Hero frame vs GS oracle: shift 0.0/−0.1 px. FOV is per camera (FUN_0040d670 callers FUN_003d3cd0/FUN_003d3d98); only the z128 value is used. |
| GS pixel sample point = pixel centre (GL) | output.js `fitCamera` | GUESS | GS rasteriser | S | Not checked on hardware. Matches the oracle (gsrender2 samples at +0.5) to 0.1 px; a +0.5 shift left 0.5 px. Settle with a PCSX2 native-res capture (pcrtc_antiblur off). |
| Copy FBP 224 → FBP 112 (CT24, 32-px sprites, X 1:1, V 0..513 over 512 rows) | output.js: shows `fbRT` 1:1 | GUESS (V step) | copy draws in the dumps (prim 0x316, TBP 7168) | S | The V 513/512 step may shift rows ≥256 by one; TEX1 (filter) for the copy isn't parsed by gsparse. Not ported. |
| PCRTC merge at VCK resolution (2553 x 512, C1 col k/6, C2 col (k−1)/6 one row up, ALP 0x80) | output.js `merge` | DECODED | FUN_0040d228 (from FUN_0040ce28) | — | Exact; replaces the fractional-offset version in native mode. |
| Display window 2553 VCK x 512 lines shown 4:3, pillar/letterboxed in the canvas | output.js `box()` | DECODED (window) + PAL 4:3 | DISPLAY1/2 DW 2552 DH 511 MAGH 5 MAGV 0, SMODE2 INT 1 FFMD 0 | S | What PCSX2 shows. A real PAL TV also has ~576 visible lines (512 + border) and overscan: not modelled. No 16:9 mode found in the display struct or registers. |
| Final scale analog picture → canvas pixels: linear | output.js `vckRT` filter | GUESS (not decodable: analog output, TV reconstructs) | — | S | Nearest would make uneven row heights (512 lines into ~1900 px). |
| Canvas-resolution render (default), camera aspect fills the canvas, fractional merge; `?res=native` = the game's 432x512, 4:3 | output.js full mode | IMPROVEMENT (ON by default: Dan's choice 2026-10-28, he wants an upscaled crisp render) | — | — | Today's previous behaviour. Hero frame vs game: SSIM 0.70 (native 0.79), head/collar 0.66 (native 0.79). |
| Vincent's body, hair and cape draw back faces only (one-sided) | play.js character materials (`THREE.BackSide`) | DECODED (GS evidence) | hero dump 163235: the game sends hair/cape triangles with ADC=1 when counter-clockwise on screen (1299/1303 suppressed are CCW, 1195/1198 drawn are CW); the culling routine itself is not located | S | Worker 0084. |
| Gun draws back faces only (one-sided, like the body) | gun.js (`THREE.BackSide`) | DATA (GS dumps) | the gun's draw: XYZ3 ADC culling, as the body | — | Worker 0091: every dump-drawn gun triangle (hero 78, 185126 80, 185138 69) is cw on screen in our index order, and none is only ccw. DoubleSide drew both twins of the two-sided cards. In Dan's rect, z-fighting gun pixels went 2545 → 107 of 6316 (the rest: frame0 copies with the same winding, also drawn twice by the game). Worker 0097: the 107 were not game twins: see the qwc row below. |
| Gun alpha-test draws: descriptor flag bit 1 = TEST ATE GEQUAL AREF 100 + AFAIL RGB_ONLY + ABE (Cs-Cd)*As+Cd, two passes as play.js's hair (pass 1 a >= ref blend + Z write, pass 2 a < ref blend only); other gun draws opaque (ATE 0, ABE 0, texture alpha unused); atlas keeps texture alpha | gun.js (`meta.runs`, pass1/pass2, `gsBlend` copied from play.js: not exported there); export_web_gun.py (runs, `ALPHA_REF`) | DECODED (FUN_005b19b8 flag bit 1) + DATA (GS dumps) | FUN_005b19b8 | — | Worker 0097. Hero / 185126 / 185138: 25 gun runs each, all TEX0 PSM T8 TCC 1 MODULATE, ZTE 1 ZTST GEQUAL, ZMSK 0, drawn right after Vincent's last hair draw; exactly the flag-2 triangles (5 drawn: 1 frame0, 4 barrel1) carry ATE 1 ATST GEQUAL AREF 100 AFAIL RGB_ONLY ABE 1 ALPHA (0,1,0,1). |
| Gun triangles in the game's draw order (parts last-to-first, deferred buckets) | export_web_gun.py (`SM.tri_draw_keys`) | DECODED | FUN_005b2ef8 / FUN_005b2550 / FUN_005b26b0 | — | Worker 0097: 0 order breaks against the uniquely matched dump draws (24-27 per dump). Gun vs Vincent: the gun is a child of j56; scene-graph order vs his deferred hair not re-checked in the page. |
| A draw descriptor's VIF run is qwc = u16 desc+6 quadwords (DMA ref tag, address desc+0x14) | export_cerberus.py `descriptor_tri_limits` / `decode_gun` (gun only) | DECODED | FUN_005b2240 (`*(ushort*)(desc+6) \| 0x30000000`, `desc+0x14`) | — | Worker 0097. render_skinned.parse_drawpackets reads a group's last descriptor to EOF: on the gun 8 descriptors read on into the next part (120 of 1058 tris; frame0.g1 d2 18 -> 2, its 16 extra carry topmt0's UV triples on frame0 vertices = the same-place twins that z-fought). None of the 120 is drawn in any dump. Vincent p000: no change (2389 either way). The shared decoder (render_skinned.py) still has the bug for other models; not fixed there (out of scope). Texture per gun triangle is now from the files in both export modes (the dump vote gave frame0.g1 one texture for 3 slots). |
| The viewer's static Vincent (index.html / main.js) uses the room material: double-sided and prelit | main.js / room.js | GUESS (viewer only) | — | S | The play page has the decoded culling and lighting; the viewer lags behind it. |
| Ladder exit puts the player state back to on-foot (2) when the action lands him | player.js `applyOwned()` | GUESS (same state switch as moveTick's 2 <-> 9) | — | S | Review fix: the climb ticks report airborne, so state 9 would otherwise outlive the ladder. |

## Body skin weights and the hair seen from behind (worker 0092)

| behaviour | our code | source | game function | difficulty | notes |
|---|---|---|---|---|---|
| Body vertices skinned with the game's per-vertex weights: a batch's cntA vertices start their slot at w x M.local, its cntB vertices add w x M.local (normals too) | render_skinned.py `build_group_pool`, `_batch_weights`; export_web_vincent.py `slot_skin`/`body_skin` (JOINTS_0/WEIGHTS_0) | DECODED | FUN_005b1190 (loop 1: vf10/vf11 = vf00; loop 2: lqc2 of the slot into vf10/vf11), VU0 0x120 (`vf15 = w*M*pos + vf10`, w = vf09.w), FUN_005b1260 (patches S-32 0x60.. then V3-16 masked 0x79.. so w = the S-32 value) | — | Was last-write-wins at weight 1. p000: weights sum to 1 on all 2037 vertices, 994 have 2-5 bones. Hero dump 163235 at h_st f91 (1571 verts): median 0.056 -> 0.037, p90 0.27 -> 0.077. Bind pose unchanged (<= 0.0016). Mutant (cntB as fresh): median 1.55. |
| 5th influence dropped (three.js takes 4), rest renormalised | export_web_vincent.py `body_skin` | GUESS (port limit) | — | S | 16 of 2037 body vertices, dropped weight 0.05 each; glb vs decoder max 0.08 (h_rn). |
| Batch without an S-32 weight stream -> last write wins at weight 1 | render_skinned.py `build_group_pool` | GUESS (fallback, never hit in p000) | — | S | Old rule kept for other characters' safety. |
| Hair shape from behind | — | MEASURED, no defect | — | — | Walk dump 185126 (the only behind view): no unmatched hair draws (all 12144/11872 tris map to ours on the 4096 UV grid; leftovers are gun/glow/cape); every drawn hair tri has a cw copy the page draws; z-buffered game geometry vs ours (dump pose) at the dump camera: hair 1568 vs 1562 px, rows 219-312 both. The page at that camera (h_wk f4) shows the long locks down the back like the game (review/hair-back.png). Worker 0093: behind+above while running (4 PCSX2 GS dumps, pitch -6.5 to -41 deg) vs the page closed-loop at the same ticks: hair over the collar 202/212, 374/384, 504/500, 879/878 px (game/page), IoU 0.82-0.88 (review/hair-cape-above.png). |
| Hair vs tall collar while RUNNING and turning (Dan's reports e896 / e1d5) | export_web_vincent.py `body_skin` (worker 0092 weights) | MEASURED, fixed by the 0092 skin weights | FUN_005b1190 / VU0 0x120 | — | Worker 0094: 5 PCSX2 GS dumps while running freely (1.2 u/frame, no wall contact) through a left and a right turn, frames 24/29/34/78/83, camera pitched -19 deg (fitted 0.035-0.04 px; dump cape = traced cape_all to 0.006-0.04). Page closed loop from the save-state spawn: position <= 0.003, clip frame exact, bones <= 0.005 (t). Every drawn game vertex (hair 0x2de4, mantle, head, body, arm) has a page vertex within 0.04-0.08; 0 camera-facing page tris without a game counterpart. The data Dan's reports were taken with (pre-0092, one bone per vertex): collar prims 13/15/16 off by median 0.3-0.45, max 1.2-1.7 units, mostly UP into the hair (e.g. d5: 16/16 vincent_15 and 21/22 vincent_16 drawn tris with no game vertex within 0.25); it reproduces Dan's sliced hair + tips through the collar at his e1d5 view. In-between renders at 120 Hz (d4 turn) also match. review/hair-collar-running.png. |
| Page spawn = the hero-dump placement (-1395.63, -1110.51, facing 52 deg), not the save state's player (-1395.15, -1111.02, facing 10 deg) | play.js `main()` spawn (scene.char) | GUESS (not decoded; worker 0094) | save state C matrix (trace record 0) | S | Closed-loop replays must set ctrl.spawn from the trace (worker 0094 pagedrv.py); with the page spawn the page is 0.69 units / up to 7.7 deg off the game from the first step. |
| Idle (h_st) cape target at the save state | player.js pose / cloth.js skin | OPEN (worker 0094) | — | M | Frames 1-18 from the save state: page cape TARGET vs game cape_tall 0.8 mean / 3.4 max (v54); bones j50 t 1.0, j74 rot 0.29 at h_wk 1.4-2.5; from h_rn on 0.005. Not chased (the save state may hold a blend-out layer the page does not restore). |
| Cloth window armed by a turn from standing (game: 100 frames at the frame the stick turns him, before h_wk starts) | cloth.js / play.js turn re-arm (GUESS rule) | OPEN (worker 0094) | mp_cloth 0x64/0x64 at frames 11-12 of w94z | S | Page arms nothing until the walk clip (60) at frame 13; cloth pos error 0.4 mean / 1.2 max at frame 12, decays to 0.02 by frame 39. |
| Vertex normals exported at unit length (export_web_vincent.py normalises the blended normal); the game sums w x int8/128 normals unnormalised before lighting, slightly shorter than 1 where bones blend | export_web_vincent.py `body_skin` | GUESS | FUN_005b1190 / VU0 0x120 (the accumulate); play.js lighting uses the normal as given | — | Code review 2026-10-28. Effect: slightly brighter lighting at blended vertices; not re-measured against the hero dump since the skinning change. Open. |

## Jump (worker 0095, jump_core.js; wired into the page by worker 0102)

| behaviour | our code | source | game function | difficulty | notes |
|---|---|---|---|---|---|
| ✕ gate, take-off h = cfg+0xc4 + 1 = 13.1 via the body (FUN_00357168), state 8 / 0x1f / 0x20 / 0x21 | jump_core.js `canJump`, `startJump` | DECODED | FUN_00333150, FUN_00577230, FUN_00575698, FUN_00575768 | — | Replaces the removed "jump take-off" rows. 7 PCSX2 traces, test_jump.mjs. |
| Wind-up freeze while h_jp0 / c_wjp0 / c_bj0 plays (no move, no turn, no fall clock) | `JumpMotion`, `Jump.frame` | DECODED | FUN_00598f50 / FUN_005a1510 (C+0x1c0 bit 0x20), FUN_0034ea18 | — | |
| Clip timeline (h_jp0 → h_jp1 → h_jp2 → h_jp3 loop → h_jp4; second jump c_wjp; back jump c_bj) and landing exits | `JumpMotion`, `BackJumpMotion`, `LandMotion` | DECODED | FUN_00598f50, FUN_005a1510, FUN_00599aa0 | — | Corrects the old row: h_jp1 / h_jp2 DO play (head layer, not C+0x570). |
| Motion player hold / queue take-over rule | `ClipClock` | GUESS | (FUN_005aa2a8 frame step decoded; the queue not) | M | Exact on all 7 traces. |
| Jump done flag → state 8 → 2 on that frame | `Jump.frame` | GUESS (handlers decoded, event plumbing not) | FUN_00538aa0, FUN_00538a30 | S | Frame-exact on all traces. |
| Motion id for a second jump from states 9 / 0x20 | `Jump.frame` | GUESS | vt+0x24 0x538bd8 / 0x53a478 not read | S | |
| Back-jump turn-round test uses P+0x80 = facing | `stickMode` | GUESS | FUN_00333150 (FUN_00261038(P+0x80, stick)) | S | j_back facing exact. |
| Back-jump turn away from the stick (mode 5) | `stickMode`, `backTurnFix`; on the page player.js `airIntent` (negated before FUN_0034b638, so a mode-6 aim turn still replaces it) | DECODED | FUN_00333150, FUN_00578748 LAB_00579288 | — | |
| Second jump (one per jump) | `startJump`, `JumpMotion(second)` | DECODED | FUN_00575698, FUN_00575768 | — | Was "missing". |
| Jump sounds 0x50 / 0x51, camera hooks, FUN_005ae618 modes, FUN_00574b98 | — | missing | FUN_003412d0, FUN_00329ff0, FUN_005ae618 | S-M | Listed in notes/jump-research.md. The camera hooks show on the page: review/jump-page.png's landing frame is framed differently from the game's. |
| Page wiring (worker 0102): Space = ✕ edge; an action that takes the press or owns him (the ladder) keeps it; take-off before the stick, motion update after the intent, move + turn skipped while frozen; the jump's clip clock is the animator's head layer (blend 0 = cut, blend n = n + 1 frames), hand-back to the locomotion choice from a fresh index | player.js `step`, `SpeedAnimator.jumpFrame`; aim.js `clips()` stands aside while the jump owns the head | DECODED (order: FUN_00333150 → FUN_00578748 → FUN_00598f50 → FUN_0034ea18) | | — | test_jump_page.mjs: 7 jump traces through the real step + animator, every frame: state, ground, head clip, frame, blend countdown exact; position ≤ 3.7e-3 without re-sync; facing ≤ 7e-7. 18/18 mutants killed. |
| Armed jump plays the plain h_jp set | player.js `step` (`armed: false`) | GUESS | FUN_00598f50 phases 4-7 (h_jp*_a with partners via FUN_00345818) | M | The h_jp*_a clips aren't in the exported containers (07700000 / 0770000b), and aim_g shows h_jp1_a → h_jp3_a (h_jp2_a skipped, blends 11 / 2) where jump_core's phase 5 queues h_jp2_a: not matched. The wind-up is 3 frames either way, so the flight is unchanged (aim_g 3e-5). The landing state is 3 with the gun out in the game (P+0x100 bit 44); ctrl.P stays 2. |
| A requested jump clip the page doesn't have (c_wjp*, c_bj*) | player.js `SpeedAnimator.jumpFrame` | GUESS | — | S | Keeps the current head pose; the clock still times it. These clips aren't in the exported containers (which container z128 loads them from isn't decoded). |

## Aiming (workers 0077, 0098: aim.js, aim_core.js, player.js FollowCamera weapon views)

Worker 0077 translated the weapon (decode: notes/aim-research.md); worker 0098 brought it onto the
current page and replaced its GUESS camera with the translated FUN_0032ace0 weapon / first-person
paths. Checks: `AIM_TRACES=<repo>/tools/trace/out/aim node --import ./test_hooks.mjs test_aim.mjs` and
`test_aim_camera.mjs` in native/web (17 mutations of the state machine and the camera rules each fail
one of them). These rows replace section 2's 'Camera kick per shot' (missing) and narrow 'Camera paths
not translated'.

### Decoded

| Behaviour | Where | Status | Game source | Effort | Notes |
|---|---|---|---|---|---|
| Weapon stats W | data/aim/weapon.json (export_web_aim.py) | DECODED | FUN_0031c3f8 over bgd.bin section 5 parts | — | Composer equals the save's live W byte for byte (`--check-state`); interval-sum mutation fails it. Parts = the save's slot (#117, #141, #0); a new game's loadout (FUN_0031b690) isn't translated. bgd.bin is hash-checked (tools/byo/disc.py INPUTS). |
| Draw / holster / R3 / fire input | aim_core.js `input` | DECODED | FUN_003320c0, FUN_005766c0, FUN_00574a80 / 574b00 / 574b98, FUN_00577180 | — | Melee, L1 materia, R2 reload, weapon switch not on the page. |
| Draw / holster timer and states 0xc / 0xe | aim_core.js `draw` / `holster` | DECODED | FUN_00576548: state 0xc / 0xe, T = max(T, DAT_0046ee84 = 6) (0x46ee80 = 15 when FUN_00326500() is false), holster -> FUN_0032a420(CAM, 1) | — | Was GUESS (traced 6). |
| Holster on the ladder mount | aim.js `actionEnter`, aim_core.js `ladderMount` | DECODED | FUN_00576c88 / FUN_00576de8: FUN_005747f8(P, 4), FUN_00576548(P, 0), FUN_00574928(P, 0) | — | Page check: gun out, then the ladder: holstered, camera 0xc. Neither the callers nor FUN_00576548 test for a drawn gun: weapon state 0xe is set with the gun away too, as the game does (worker 0099). P+0x29c = 4 here (2 for circle); P+0x298 / 0x299 not on the page. Open: P+0x29c stays 4 after the ladder on the page (the game's ladder-exit write isn't ported); only isAiming() (== 3) reads it, and a draw sets 3. |
| Weapon state machine | aim_core.js `stateMachine` | DECODED | FUN_00579570 (states 0-4, 0xc-0xf; 1-tick accumulator P+0x294) | — | Runs every frame, also during actions (FUN_005786e8); states 5-11 not on the page. |
| Same view mode: no camera restart | aim_core.js `setView` | DECODED | FUN_0032a420 (LAB_0032a4d0) | — | |
| Turn to aim (rotation mode 6) | player.js `intent` / `airIntent` / `aimTurn` | DECODED | FUN_00578748 (P+0x29c == 3 -> FUN_0034e960(C, 6)); FUN_0034de50 case 6 (rate 0.5 toward C+0x2b0 - C+0x30); the air branch keeps mode 6 | — | Max 0.27 deg per frame over the traces; target flattened (row 2 y = 0 in every record). |
| Aimed move clips | aim.js `makeClipControl`, player.js `setTable` | DECODED | FUN_00595440, FUN_0036a638, FUN_00260e74, FUN_00345b58 / 345ca8 / 345e38 / 345e58; leaves to the idle class at C+0x1d8 == 0 | — | Head clip matches every on-foot frame of the traces. Tables are built only once their clips and sidecars are loaded. |
| One-entry table pick | player.js `pick` | DECODED | FUN_00346860 (index clamped to C+0x658 - 1) | — | |
| Stand-fire clip | aim.js `makeClipControl` | DECODED | FUN_00596460 / 00596678 (h_st_f; back to h_st_a at clip end or 30 ticks; moving leaves) | — | |
| Pitch limits +50 / -54 deg | aim_core.js | DECODED | DAT_0046e3f8 / 0x46e3fc (RAM) | — | |
| Recoil (pitch kick + carry-over) | aim_core.js | DECODED | FUN_00577480 -> message 0x3e9 -> FUN_0032ffb0; l.1747-1762 | — | |
| Face point P+0x120 / aim point P+0x2b0 | aim_core.js | DECODED (not press frames) | FUN_005784d8 (CAM+0xa0 each tick while drawn); presses FUN_003296e0(CAM, ., 0) = CAM+0xb0 (a2 = 0 at all four call sites in FUN_003320c0) | M | Matches every non-press frame. See UNDECODED below. |
| Reticle show / hide, kind, phases | aim_core.js | DECODED | type update -> FUN_0033ba88 -> FUN_0033c060 -> FUN_003b0440 / 0680 / 0738, FUN_003b0ac0 | — | |
| Reticle position | aim_core.js `reticleOffset` | DECODED | FUN_0032ace0 l.1687-1696, FUN_0033c868 | — | HUD+0x78 matches every frame. |
| Reticle pieces | data/aim/reticle.json, aim_core.js `reticleQuads` | DECODED | DAT_00472790 / 0047bfc0 / 00472110-40 (4 entries each; kinds 1-3 exported), FUN_003b1048 | — | 21/21 quads vs the GS dump, worst 0.078 HUD px; alpha and colour exact. |
| Weapon / first-person camera (view modes 2 / 3, types 0xd / 0xe): same FUN_0032ace0 as type 0xc with the type objects (pivot / offset / FOV, traced), the auto-turn gain only decaying (DAT_0046e354 0.06 x step), look table DAT_004a0608[8] = 0.66, flag 0x8000 cleared (no follow nudge, no recede, no look-point angle change), base-follow rates CAM+0x150/+0x160 per type enter (0xd 0.3 / 0.5 / 0.15, 0xe 1 / 0.5 / 1), distance ease-out 0.03 (0.2 with flag 0x100000), no state slow-down outside view 1 | player.js `FollowCamera.tick()` / `place()` with `follow.weapon`, `CAM_TYPE_D/E`, `BASE_RATES` | DECODED | FUN_00330980, FUN_00330f08, FUN_0032ace0 (uStack_15c = view == 1), enters FUN_00330240 / 00330748 / 00330d18 (disassembled from RAM: 0x330368, 0x330938, 0x3308a4, 0x330e64) | — | Worker 0098. test_aim_camera.mjs: 8 aim traces + a new one (w98_aim, draw -> third person -> first person) open loop: eye <= 0.005, look-at <= 0.0005, yaw / pitch / forward <= 7e-6. |
| View 1 near-eye ease-out: when last frame's eye is within 17.5 of base + type pivot (unrotated), the distance eases out at (0.2 − 0.03)·(17.5 − d)/10 + 0.03 | player.js `place()` (near) | DECODED | FUN_0032ace0 l.2302-2340 (uStack_15c branch, DAT_0046e508 = 0.2) | — | Worker 0099: checked per tick on new PCSX2 traces (scripts tools/trace/scripts/cam_near_back / cam_near_turn: run toward the camera into the wall behind, then run away or turn the camera; aim_i_fp_holster: first person, then circle). Every ease-out frame matches CAM+0x1c0 to 0.005 (36 and 44 near-eye frames); aim_i eye <= 0.005. With the rule off: ease-out 0.06 / 0.15 off, aim_i eye 11 off. Test: `node --import ./test_hooks.mjs test_cam_traces.mjs` (CAM_TRACES = tools/trace/out/cam) and test_aim_camera.mjs (aim_i). |
| Camera collision distance at the wall behind the spawn (x ≈ −1418, z ≈ −1113, eye pulled in to ~15) | player.js `place()` collision | OPEN (found by worker 0099) | FUN_00544f90 → FUN_0054c848 | S | While pinned there the page measures the eye 0.048 closer than the game (cam_near_back f40-112, cam_near_turn f95); once (near_back f114) that flips the in / out branch for a frame. Not the ease rules. Collision file set or primitive choice (see the CAM_COL_FILES GUESS) not checked. test_cam_traces allows eye 0.06 in its per-tick traces for it. |
| Walk-away x0.1 test: squared distance eye to CAM+0x140 (last base) below eye to CAM+0x130 (base) | player.js `place()` | DECODED; float ties | FUN_0032ace0 l.2350-2370 | — | Pinned on a wall the base moves by 1e-5 or less, the two sides tie, and the game's f32 / VU rounding decides (flips every other frame: cam_near_turn f171-207, up to 0.53 eye for a frame). The page computes in doubles; test_cam_traces counts these frames (gap under 1e-3 with the stick held) instead of comparing them in its per-tick mode. |
| Type switch ramp: eye CAM+0x90 and forward CAM+0x80 move from LAST frame's values toward this frame's by FUN_00260d44(CAM+0x17c) (taken before the ramp update), forward renormalised with row 1 = (0,1,0); the distance ease runs on the lerped eye | player.js `place()` (ctx.e), aim_core.js `cameraFrame` | DECODED | FUN_0032ace0 l.2088-2140, FUN_002611f4 (lerp) | — | Replaces aim.js GameCamera's fixed start point (GUESS). |
| Switch angles between views 2-4: FUN_0032a420 sets CAM+0x1e8 0x800 (and 0x100000 from first person), CAM+0x100 = old type pivot (y x cos(pitch x 0.017453294)) + offset; FUN_0032aaf8 stores CAM+0x1dc / +0x1d8 = -sign(v) atan(|v| / max(25, |CAM+0xa0 - CAM+0x90| - v.z)); next frame (l.1509) yaw / pitch += them, and with 0x100000 the ramp = 1 | aim_core.js `setView`, `typeVec`, `enterType`, `switchAngles`; player.js `tick()` | DECODED | FUN_0032a420, FUN_0032aaf8, FUN_0032ace0 l.1509-1520 | — | Was the GUESS 'leaving first person in one frame'. Matches the traced 0.06986 / -0.01078 / -0.04756. The game's cos(pitch x 0.017453294) converts a radian pitch again (cos ~ 1); kept. |
| Leaving first person: FUN_00331160 (type 0xe leave slot) sets CAM+0x1a0 = 5 and CAM+0x1a4 = -0.75; the next frame's look-at pivot y += CAM+0x1a4, then CAM+0x1a0 eases to 5 at FUN_00323318(0.25) and CAM+0x1a4 = -0.75 x FUN_00260d44(2t) (t = (x - 5) / -3) | aim_core.js `setView`, player.js `shoulderPre/Post`, `place()` | DECODED | FUN_00331160 (vtable 0x4a072c), FUN_0032ace0 l.1812, l.2256-2264, FUN_00330478 (0xc sets +0x1a4 = 0) | — | One frame 0.75 lower look-at, traced (aim_e f162, aim_f f102). The ease itself is dead on the page and untested: CAM+0x1a0 is only ever set to 5 (DAT_0046e348; 2 = DAT_0046e34c needs flag 0x4000, which nothing on the page sets), so t = 0 and the first `shoulderPost` puts CAM+0x1a4 back to 0. |
| Recoil in the view: matrix pitch + 0.8 x CAM+0x198 x CAM+0x170 / 73.5 (not in the look-at's -3/-4 x pitch shift) | player.js `place()` (ctx.recoil) | DECODED | FUN_0032ace0 l.1762 (DAT_0046e4cc 0.8) | — | Look-at 0.07 off after shots when the recoil was in the shift. |
| Eye kick: eye += forward x DAT_0046e43c (1.0) x CAM+0x224 after the distance ease, then CAM+0x224 decays (FUN_0032a348) | player.js `place()` (ctx.kick), aim_core.js `kickDecay` | DECODED | FUN_0032ace0 l.2412-2423, FUN_0032a330, FUN_0032a348 | — | Was 'left out (vector not decoded)'. |
| Aim ray from the camera's own matrix: F = -sin(a) row 1 + cos(a) row 2, a = CAM+0x1cc = -atan(HUD+0x78 x tan(type FOV x 0.875 / 2)); S / E / far as FUN_00329778 | aim_core.js `reticleRay`, `aimRayPoints`; aim.js `aimRay` | DECODED | FUN_0032ace0 l.1692-1698, FUN_00329778 | — | Far point CAM+0xb0 within 0.006 on every compared frame (the game doesn't update it in view 1). |
| Camera types per view (0xc / 0xd / 0xe; 0xf scope not reachable with this loadout) | player.js `CAM_TYPES`, aim_core.js `CAM_TYPES` | DECODED (runtime values) | FUN_0032a420, FUN_00337338, constructors / enters | — | Look-at exact. |
| Ramp, fade, FOV of the switch | aim_core.js `cameraFrame` | DECODED | FUN_0032ace0 l.2094-2190 | — | Ramp, phase, fade, FOV match every frame (test_aim). |

### GUESS / left out / open

| Behaviour | Where | Status | Game source | Effort | Notes |
|---|---|---|---|---|---|
| Reticle position slides and 0x10000 scale | aim_core.js `reticleQuads` | left out | FUN_003b1048 flags 0x80-0x40000, 0x10000 | S | End at the record position / scale 1 in steady state. |
| HUD clock in ms | aim.js `post` (20 per frame) | GUESS | DAT_004f58ec (FUN_003e2450 <- FUN_0023c1f0) unit not decoded | S | Fits the traced 5-frame fade for length 100. |
| Vincent's first-person fade | aim.js `render` | GUESS | CAM+0x1e0 -> FUN_00342318 | S | Drawn visible / hidden; the 16-frame partial alpha is left out. |
| Aimed table root speed n | aim.js `rootSpeed` | GUESS | FUN_00344340 (\|root delta\| x 60 / n), n = FUN_005ad6e8 | S | n = 2 x frames - 1 reproduces the traced h_wk / h_rn roots to 0.1%. |
| Aimed move class entry speed | aim.js `makeClipControl` | GUESS | class switch idle -> move not decoded | S | Entered at C+0x1d8 >= 5 (player.js's locomotion install rule); traced h_st_a at 1.9 / 3.7, h_wk_a at 5.6. The exit at 0 is decoded. |
| Sideways offset of the face point | player.js `intent` comment | left out | FUN_003424c0 (C row 0 x 0.25..1.25 by C+0x664) | S | Under 1 deg at aim distances. |
| Locomotion clip choice on holster | player.js `setTable` | GUESS (traced) | install path on holster not decoded | S | The choice runs once at the current speed (h_st standing, aim_e f221; h_rn running, aim_h_holster_run f91); the first-install-at-5-u/s rule is kept. |
| Stand-fire: a shot while the class runs | aim.js `makeClipControl` | GUESS (traced) | class dispatcher not decoded | S | Doesn't restart h_st_f (aim_a f64). |
| Face point on press frames | aim_core.js `input` / `draw` | UNDECODED | the game ends a press frame with P+0x120 = CAM+0xa0 although the press writes CAM+0xb0; the later writer isn't found (FUN_003343a8, vtable 0x4a096c, needs state 0) | S | 1 frame per press; the page keeps the decoded CAM+0xb0. test_aim counts these frames separately. |
| Up / down aim partner clips | — | left out | FUN_00345818, FUN_00342be0 | M | Level clip only; weight not decoded. |
| Lock-on / auto-aim | — | left out | FUN_00328968 | M | No actors on the page (step 9). |
| Bullets | — | left out | FUN_00552dd8 / 00556068 / 005442d8 | M | No targets, no decoded effect. |
| Weapon speed modifiers while drawn | player.js `intent` (mods = 1) | DATA (traced) | FUN_00578748: table +0x42c and W+0x30 | S | Net 1.0 for this loadout (traced 15 / 60 u/s). |
| Input mapping LMB = R1 (canvas, pointer locked or lock unavailable), Z / MMB = R3, C = circle | aim.js | GUESS (mapping) | pad table 0x46bae0 | S | Respects the report freeze. The lock-taking click never counts; without pointer lock a still click counts, a drag-look doesn't. Touch input isn't mapped. |
| Reticle texture | aim.js `ReticleLayer`; export_web_aim.py `menu_texture` -> data/aim/reticle.json `textures` | DECODED | 'menu2' TIM in data/etc/menudata.bin (PTR_DAT_0047d500[DAT_004727f0[2k]], FUN_003dd4a8); UVs FUN_003b1048 via FUN_003cb1f0 / 003cb268; GS dump reticle_f200 (TEX0 PSMT4 TBW 2 TBP 10792, CLUT CBP 10788, TEX1_1 0, CLAMP_1 0, TFX 0 TCC 1, ABE (Cs-Cd)As+Cd) | — | Texels and CLUT exported from the disc file equal the GS memory at the reticle draws byte for byte. Full HD: nearest texels at the target resolution (the game's TEX1 = nearest, scaled up; no extra filtering). GS 8-bit blend rounding not reproduced. review/reticle.png: green-channel SSIM 0.34 -> 0.83 (third person) / 0.36 -> 0.88 (first person), edge corr 0.07 -> 0.72 / 0.86 (worker 0100). |
| World probe of the aim ray | aim.js `aimRay` (collision.js `segment`) | GUESS | FUN_005449f0(world, 1, ...) probe class not decoded | M | Hits the game's CAM+0xa0 on 192/269, 269/269, 204/269, 21/116, 30/90, 99/99, 136/189 frames (test_aim, reported not asserted). |
| Reticle lead CAM+0x1fc / +0x200 (the ray and reticle lead the pad's right stick) | — | left out | FUN_0032ace0 l.1700-1722 (FUN_0033bad0 / 0033bc60 cursor) | S | Pad path only: with the USB mouse latch (the page's mouse) both are 0. The page has no right stick. test_aim_camera counts these frames instead of comparing the far point. |
| Camera after a jump with the gun out | — | OPEN (two attempts) | FUN_00575768 -> FUN_0032a0f8 sets CAM+0x1e8 0x1000000 and CAM+0x220; their reader is not in the FUN_0032ace0 decomp | M | aim_g: from the jump on, the game's eye sits ~4.9 off the view axis (look-at and forward exact), and stays off after landing. Not CAM+0xd0 (0 throughout). The page's jump isn't wired, so it can't occur there yet. |
| Aimed clips posed by the three.js mixer | player.js `SpeedAnimator.update` | GUESS (port plumbing) | FUN_005a9db0 (the game's pose build is translated for clips with local-pose records) | S | Data done (worker 0101): packs now carry vincent_<pack>_local.bin (export_web_vincent.py --pack). Still GUESS on the page until the loader patch in HANDOFF "Pending loader patch" is applied (player.js addLocalPose, play.js loadPacks). With it, aimed poses vs game bones (w101_aim_pose): mean 1.14 -> 0.11, max 16.5 -> 1.75; the rest is the unported aim-pitch partner layer. |
| Camera eye distance and mouse look in the weapon views | — | removed (worker 0098) | — | — | aim.js GameCamera (GUESS distance |offset z|, GUESS look rate) replaced by FollowCamera's translation. |
| Auto turn-behind in view 1 after holstering while moving | player.js `tick()` | DECODED (type 0xc path) | FUN_00330478 | — | Not checkable on the aim traces (C+0xc0 not in the aim field set); test_aim_camera stops comparing from the first frame it can act. |
