# Fidelity audit: browser gameplay vs the game's code (worker 0070, 2026-10-26)

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

| behaviour | our code | source | game function that does it | diff. | notes |
|---|---|---|---|---|---|
| Tick length 1/60 s | player.js:56 `TICK` | DECODED | FUN_00322f88 (`DAT_004eeb28 = step × 1/60`) | S | Matches. |
| Ticks per frame (PAL 1.2 per vblank) | play.js:410 fixed-step loop on wall-clock time | GUESS | FUN_0040ce28 / FUN_0040cb78: step = vblanks × 1.2 (PAL), **variable step**, cap 5.0 ticks | M | The game runs ONE variable step per frame (×step inside the movers), not N fixed ticks. Our catch-up cap is 8 ticks then a drop (the game's cap is 5). Fixed 60 Hz is a fair stand-in until the movers take `step`. |
| Action/layer state machine (registry, `canEnter` priority, layers) | player.js:156-189 | INVENTED | Player state byte +0x104, FUN_0055b8e0 (state change through state-class vtables, DAT_004eedf0[state]); input → state in FUN_00333150 | L | The game's states: 2–5 ground, 8/9/0x20 jump, 0x1f/0x21 second jump, 10 ladder, 0xd/0xe ladder exits, 0x12, 0x16, 0x1a, 0x80+ melee. Our scaffold is a port convenience, but its ordering rules (registry priority, "layer delta wins") are not the game's. |
| Locomotion acceleration (`accel = 14`, exponential ease of velocity) | player.js:80, 211 | INVENTED | FUN_00578748: stick byte +0x299 ramps toward the target +0x298 by +0x29a = **25 per tick** up and +0x29b = **25 per tick** down (×2 when +0x100 bits 40-41 are set); init FUN_005748b0 | S | Speed is a function of the ramped byte, not an eased velocity. |
| Walk / run speeds (clip root speed 0.505 / 1.60 u/tick) | play.js:24, 274 | DATA (clip root motion), but the rule is INVENTED | FUN_00578748 (states 2–5, 0x16, 0x1a): byte b ≤ 234 → `walkFrac·b/235`, b ≥ 235 → walkFrac + (1 − walkFrac)·0.05·(b − 235); × (1 + mods) × char+0x1d4 (max u/s); under 0.75 u/s → 0. walkFrac = FUN_00317618 = rec+0x74 / rec+0x70 | M | Max speed char+0x1d4 is set at runtime (FUN_0034b978; default 10.0 in FUN_00328368). rec+0x70/+0x74 belong to the record from FUN_0055a370 (equipment/status), not traced. Our speeds come from the clips, not from the game's speed fields. |
| Walk vs run choice (Shift) | play.js:355 | GUESS (mapping) | The game has no run button: stick tilt sets the byte (≥ 235 = run zone). FUN_00333150: state 2 vs 3 picked by +0x100 bit 44 | S | Keyboard has to synthesise a stick byte. Map walk = a fixed byte < 235, Shift = 255. Mark it GUESS. |
| Turn rate (11 rad/s, clamped) | player.js:80, 249 | INVENTED | FUN_00578748 → FUN_0034b638(rate, char, dir): rate = (b/512 + 0.5) × 0.25 on the ground (0.125–0.25), 0.25 in jump states; applied in the character half (FUN_0034de50 rotation modes via char+0x214 = 4 / 6) | M | The consumer of +0x2cc (FUN_0034b4c8 / FUN_0034de50, which has the constants 0.2 / 0.97 / 0.98) was not decoded in two attempts. Rate unit unknown (fraction per tick?). |
| Facing from motion in mode 0xc | not present (heading set by the turn code) | — | FUN_0034c388 case 0xc: char+0x90 = normalised displacement when flag 0x4000 is set and the turn is under 80° | S | Missing behaviour. |
| Measured ground speed (exponential smoothing, `exp(-20·TICK)`) | player.js:202, 242 | INVENTED | FUN_0034c388 case 0xc: `char+0x1d8 = |Δpos|·60 / *(DAT_0046e290+4)`; 0.1 when 0.001 ≤ d ≤ 0.1 and moving forward; else 0 | S | No smoothing in the game. |
| Clip choice idle / walk / run by speed with hysteresis (0.12 / 0.06 / mid ×0.9 / ×1.1) | player.js:350-360 | INVENTED | Player state (2 vs 3) + the state classes' motion requests | M | The hysteresis thresholds are invented. The state class vtables for states 2/3 were not found. |
| Playback rate = speed / root speed, clamped 0.25–1.6 | player.js:374 | INVENTED | FUN_0034c388 first switch: motion rate from char+0x1cc / +0x1d0 / +0x1dc / +0x1d4 (and FUN_00349fb8 / FUN_0034a208); FUN_0034ea18 advances +0x1cc by `*(DAT_0046e290+0xc)` | M | The game has its own rate formula per mode. Not translated. |
| Gait switch keeps the cycle phase | player.js:366-368 | INVENTED | — | M | Unknown what the game does. |
| Crossfade times (0.2 s locomotion, 0.15 play, 0.2 release; actions 0.06–0.15) | player.js:291, 323, 341; jump.js:48,97; ladder.js:106 | INVENTED | The motion blend length is an argument of FUN_00342360 (seen with 1.0, 6), not decoded | M | Every fade value is a stand-in. |
| Wall response while walking (velocity scaled by `max(f, 0.5)`) | player.js:229-232 | INVENTED | FUN_00357d48 slides along the edge (already in navMove); no velocity damping | S | Remove: navMove already does the game's slide. |
| Physics-body walk (mode 0xc) | collision.js:352 `navMove` | DECODED | FUN_00357d48 (8 passes), FUN_0035d788/0035d4e8/002606b8, 0x02 crossing FUN_00359f38 | — | Faithful. Divergence: `move()` (collision.js:457-458) searches for the polygon again each tick (nearestPoly/findPoly, maxDrop 40). The game keeps body+0x110 between ticks. |
| Physics-body wrapper (velocity = dir × speed, ground bit, gravity integration) | player.js `locomotion` / `fall` | INVENTED | FUN_0034c388 case 0xc → FUN_00358fb8 (22 lines) + FUN_003590c8 (577 lines; 8.5 below ground check, 0.3 follow, 3.5 band) → FUN_0035a440 (1767 lines: push-out, 40° slope) | L | The walk-mesh part is translated. The body integrator around it is not. |
| Step up / snap down (`snapDown = 4`) | player.js:80, 238 | INVENTED | Mode 0xc has no step rule (HANDOFF); FUN_003590c8: 8.5 below, ≤ 0.3 per call | M | Mode 10's 7.5 / 5 / 0.2 values don't apply in z128. |
| Gravity while falling (0.05 u/tick², cap 6) | player.js:80, 263 | INVENTED | Mode 0xc: gravity = cfg+0xcc × 196 u/s² (cfg+0xcc forced to 1.2 = DAT_0046ee2c → 235.2 u/s² = **0.0653 u/tick²**), set by FUN_00356358 into body+0xd0; integrated in FUN_003590c8 | M | No cap found. The header comment admits it's a stand-in. Mark it `// GUESS:`. |
| Respawn when 500 below the spawn | player.js:81, 270 | INVENTED | not searched | — | Debug aid. Keep it as an option, off the faithful path. |
| Jump take-off (1.333 u/tick up, horizontal velocity kept) | jump.js:37, 128 | DECODED (for the wrong branch) | FUN_00575768 **mode-0xb branch**. z128 has script flag bit 13 set (HANDOFF "Walk mesh"), so the game takes the **physics-body branch**: FUN_0034db80(char, 0xc); on the ground FUN_0034a8e0(h) → FUN_00357168(h) (take-off −sqrt(2gh)), h = cfg+0xc4 + 1 (normal), 2·cfg+0xc4 + 1 (param 1, high jump), 250 / 0 for other params | M | **Wrong branch for z128.** cfg = `*(DAT_0046dd68+0x14)`. cfg+0xc4 not traced; lead: stage 06800001 section 4, the same block as the bit-13 flag (+0x44). |
| Jump gravity 0.07 u/tick² | jump.js:38 | DECODED (wrong branch) | Mode 0xb: 0.07 (0x46f490). z128 runs mode 0xc: 0.0653 (row above) | S | Use the mode-0xc value. |
| No air control | jump.js header, 57 | DECODED (wrong branch) → INVENTED for z128 | FUN_00578748 states 8/9/0x1f–0x21 with mode 0xc (`FUN_00326298(0xd)` set): vel += dir × (maxSpd/60 × mods) × 0.075 × step, clamped to maxSpd/60; no stick → speed −= maxSpd/60 × 0.025 per tick; × DAT_0046ef24 = 0.3 while firing in the air (weapon class 2 with +0x270 > 0, or FUN_00577358) | M | **The game has air control in z128.** |
| Jump integration (`y += vy + g/2`) | jump.js:71 | GUESS | FUN_003590c8 (body integrator) | M | Comment says the order isn't decoded. |
| Air wall bounce −0.25 | jump.js:42, 68 | DECODED (mode 0xb), not used on the navmesh path | FUN_003543b8 | — | Fine as a fallback. Irrelevant in z128. |
| Head bump (segment cast, cut the rise) | jump.js:75-82 | INVENTED | Mode 0xc has no headroom test (HANDOFF); FUN_0035a440 push-out not decoded | M | Remove, or replace with FUN_0035a440. |
| Landing search (`min(4, 1 + speed)` above the feet) | jump.js:88 | INVENTED | FUN_003590c8 / FUN_003577c8 ground bit | M | |
| Jump clip timeline (h_jp0 3 f → h_jp1 → h_jp2 → h_jp3 loop → h_jp4) | jump.js:39-41, 106-108 | DECODED | FUN_00598f50 (state class 0x5a5b78), FUN_00599820 / 00599aa0 | S | Landing time: LAND_TICKS 14 / LAND_MIN 6 is INVENTED (when the land state ends isn't decoded). |
| Second (mid-air) jump | not done | — | FUN_00575768 (+0x79c bit 0x20, FUN_00357168), gate FUN_00575698; c_wjp0–2 | M | Correctly left out. |
| Air-shot hop (3.33 u, max 3) | shoot.js:65-66, 538; jump.js:116 | DATA + likely | FUN_00577d40 → FUN_00357168; also FUN_00575768 lowers a second jump by +0x270 shots (up to 5) | S | The formula `5 × (W+0x42 + W+0x44 + 2)/60` is L in the research. The fall-case hop uses `c.gravity` (0.05, invented). |
| Ladder trigger (7-unit ray, box test) | ladder.js:62-77 | DECODED + INVENTED slack | FUN_00544200, FUN_00576c88/576de8 | S | The ±3 y slack (l.67) and the "within 60° of box +z" test (l.74) are invented. The game casts the ray from FUN_005635e0 / FUN_00563678 points. |
| Ladder snap 5.6, rung 4.5, 14 ticks per step | ladder.js:16-23 | DECODED | FUN_0059a5d0, FUN_00599f10 | S | `BODY_FWD 5.5` and `MOUNT_RISE 6.1` are DATA (clip) plus port fixes for the in-place export. |
| Ladder exits (top 20.25 probe, bottom) | ladder.js:24-25 | DECODED / DATA | FUN_00346fc0, FUN_003470d8, FUN_0059b208 / 0059b648 | S | `BOTTOM_EXIT 6` comes from the clip, not from the 8.25 probe. Check it against FUN_003470d8. |
| Ladder jump-off fall (gravity 0.07, cap 6) | ladder.js:27-28 | GUESS / INVENTED | FUN_00562d30 (speeds not decoded) | M | Cap 6 is invented. Gravity is the mode-0xb value. |
| Ladder stick deadzone (W/S digital) | ladder.js | DECODED (±0.3) | FUN_00333150 → FUN_00576f08 / 576f98 / 577038 | S | Digital keys are fine. |
| Aim as a held mode (RMB/Q, shoulder camera, strafing) | shoot.js:8-10, 553-629 | INVENTED | The game has no aim mode (research §5.2); R1 fires toward the reticle and R3 zooms | M | The header admits "port addition". Remove from the faithful path. |
| Aim pose pitch blend (a/au/ad by camera pitch over ±54°) | shoot.js:372-374, 687-688 | GUESS | ±54° is DECODED (FUN_0032ace0 normal camera). The linear 3-way blend is not decoded | M | Find the aim-blend code (motion layers in FUN_005712c8 / FUN_00343160). |
| Direction blend weights (cos/stride, PHASE 0.25, backCap) | shoot.js:84, 559-583 | INVENTED | Not found. The aimed strafe sets exist as clips (DATA) | L | Purely ours. |
| Upper-body overlays (upper copy + additive fire overlay, `over()`, j02..j80 mask) | shoot.js:87-114, 388-400 | INVENTED | Unknown whether the game layers motions per bone. Aimed jump clips h_jp*_a exist (FUN_00598f50), so the game more likely swaps whole clips | M | Replace with the h_jp*_a set once exported. |
| Aim hold 45 ticks after a shot | shoot.js:67, 697 | GUESS | not found | — | Labelled GUESS already. Nothing should depend on it. |
| Aim / fire weight ramps (0.15 s, 0.04 s time constant, 0.97 cap) | shoot.js:722-727 | INVENTED | — | M | |
| Turn to camera while aiming (16 rad/s) | shoot.js:72, 631 | INVENTED | FUN_00578748 state 0x80–0xbf branch turns at most DAT_0046ef30 = 20°, then FUN_0034b638(0.25) and FUN_0034bcf8(4/60) (melee). The fire facing path was not traced | M | |
| Aim movement accel 14 | shoot.js:73, 585 | INVENTED | FUN_00578748 (ramp bytes) | S | Same fix as locomotion. |
| Zoom FOV 65 → 25 | shoot.js:68, 426 | DATA (W+0x34) / L | FUN_0032a420 / FUN_0032a390 (R3), camera mode 4 in FUN_0032a708 | M | The FOV ease `exp(-12·TICK)` (l.428) is INVENTED. The zoom-camera offset is not traced. |
| Zoom look-speed scaling by FOV | shoot.js:410 | INVENTED | — | S | |
| Fire interval 38, first delay 0, semi-auto | shoot.js:58-60, 694 | DECODED / DATA | FUN_00579570, FUN_00574a80, FUN_003320c0 | S | Timer runs on our tick count; the game's state machine has states 1→2. Translate FUN_00579570 directly. |
| Bullets 100 u/tick, range 565, no spread, projectile sweep | shoot.js:61-62, 509-535 | DECODED / DATA | FUN_00552dd8, FUN_00556068, FUN_005442d8, FUN_00577480 | S | The aim point is a camera ray in 100-u steps (shoot.js:497) – GUESS; the game's aim point is char+0x2b0, builder not traced. Muzzle flash sprite (random scale/rotation, 3 ticks) is INVENTED (no flash found). Spark/mark life 60 ticks is INVENTED. |
| Magazine 9, reload 80 ticks | shoot.js:63-64 | DECODED / DATA | FUN_00579570 state 7, FUN_00319f80 | S | Reserve not modelled (fine). Reload clip choice and stretching h_st_rd over 80 ticks (shoot.js:639, 711) are INVENTED. |
| Camera kick per shot | not present | — | FUN_00577480 → FUN_0032a320(0.75, 0.01) | M | Missing (meaning still G). |
| Follow camera (dist 52, pitch 0.3, height 15, pull-in −2.5, ease out exp(−4·dt)) | player.js:388-446; play.js:284 | INVENTED | FUN_0032ace0 (camera update; ±54° pitch DAT_0046e3f8/0x46e3fc; FOV 65 at 0x46e3f0; FOV ×0.875 / 1.0 / 1.1429 table at 0x46e360) | L | Our pitch clamp −0.35..1.25 rad (player.js:410) disagrees with the game's ±54°. Distance and collision rules not decoded. |
| Aim camera (dist 20, height 16.5, shoulder 6, ease exp(−8·dt)) | shoot.js:71, 407; player.js:420 | GUESS / INVENTED | No aim camera in the game. Zoom = camera modes 2 / 4 (FUN_0032a420) | M | |
| Camera-relative stick | play.js:348-356 | DECODED (concept) | FUN_00333150: FUN_003295c0 / FUN_003edfd8 turn the stick into world space via the camera | S | The game's stick has magnitude and a deadzone (0.05 / 0.3, DAT_0046e518 = 0.5) – keyboard gives unit vectors. |
| Input mapping (WASD, Space = ✕, E = ladder, F = R2, LMB = R1, Z = R3) | play.js:315-356; actions | GUESS (mapping) | Pad table 0x46bae0, FUN_00254810; ✕ = jump AND ladder (ladder wins), FUN_00333150 | S | Ladder on a separate E key differs from the game (one ✕ button; ladder takes priority when a trigger is in front). |
| Cape | vincent.glb clips (bones keyed per clip) | DATA (keyed) — the sim is missing | Cloth sim FUN_005b0d30 (decoded in `p000_cloth.py`, gravity 0.2/tick, runs every vsync toward the skinned target) | M | The web cape is the skinned target only. Note the open stiffness mismatch (HANDOFF "Cape vs cloth"). |
| Clip packs, 1 clip frame per tick | play.js / ACTIONS.md | DECODED-ish (L) | FUN_00599f10 `(frames − 2)/60` | — | Fine. |

## 3. Counts

54 rows. Mixed rows are counted by their first label:

| category | rows |
|---|---|
| DECODED (incl. DECODED / DATA, 3 of them decoded from the mode-0xb branch that z128 doesn't use) | 15 |
| DATA | 4 |
| GUESS (incl. 2 keyboard mappings) | 8 |
| INVENTED (incl. "no air control", decoded for 0xb but wrong for z128) | 24 |
| missing (the game does it, we don't: mode-0xc facing, second jump, camera kick) | 3 |

## 4. Translation order

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

After those: the camera (FUN_0032ace0, L), the cloth sim in the web build (FUN_005b0d30, M, its
decode exists), the aim-blend / state-class motion requests.

## 5. Leads and unknowns (two attempts each, then stopped)

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
| Vincent's start placement (s, yaw, x, y, z) without a capture | export_p000_gpu2.py `NODUMP_PLACEMENT` | GUESS (solved against hero dump 163235, h_st f91) | the stage start record (lead: 06800001 section 1, record 0 'p_Vin') | M | Reaches gameplay: scene.json char.s scales Vincent in play.js (0.9961 vs the dump build's 0.9625) |
| Static Vincent pose in the viewer | build_web.sh `STATIC_CLIP`/`STATIC_FRAME` (h_st f91) | GUESS | the stage start motion | S | Viewer only |
| Gun parts shown | export_web_gun.py `NODUMP_PARTS` (frame0, barrel0, barrel1) | GUESS | the weapon-parts loadout (bgd.bin section 13: handgun = parts #20 + #52) | S | Should come from the decoded default loadout |
| Start camera | p000_cape.py `HERO_CAM` (label only) | GUESS | the stage camera setup | M | |
| Sky dome | left out of the BYO build | missing | the sky container is not in the decoded room path | M | Only the capture build has it |
