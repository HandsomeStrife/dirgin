# Jump research: Vincent's on-foot jump in z128 (worker 0095, 2026-10-28)

Code: `native/web/jump_core.js` (pure, not wired). Test: `native/web/test_jump.mjs` (replays
PCSX2 traces). Traces: `tools/trace/scripts/j_*.json` with `TRACE_FIELDSET=jump` (new field
set in `tools/trace/fields.py`: the whole physics body, C+0x79c, C+0x664, P+0x118, P+0x2a8),
plus the older `e_jumps` and `i_air_release`.

Confidence: D = decoded (code read, meaning clear), L = likely (code read, meaning
inferred, trace agrees), T = trace only.

## Summary

| item | game | where | conf |
|---|---|---|---|
| button | ✕ = logical id 0xd, on the press edge (FUN_00254810(0xd, 1, 0)); a ladder trigger in front takes it first (FUN_005771c8 → FUN_00544200) | FUN_00333150 | D |
| when | gate FUN_00577230 (P+0x118 bit 0x40000 clear; bit 0x200 set and C+0x564 bit 0x4000 clear → open, as on foot in the save state); not state 0x1a / 0x80-0xbf / 0x13 / 0x14 / 0x1d / >= 0xc0, no input lock (FUN_003242e8 bit 1), no weapon block (FUN_00574ca0) | FUN_00333150, FUN_0055bb40, FUN_0055bb78 | D |
| which jump | FUN_00575768(P, 0); (P, 1) = high jump when FUN_00326068(3) or FUN_00321460() == 1 (DAT_0046e248+0xc = 0 in the save; the traces show the normal height) | FUN_00333150 | D |
| state | on foot (2-5) → **8**; the back jump → 0x20 (see below). C+0x79c bit 0x20 cleared | FUN_00575768 → FUN_0055b8e0 | D |
| height | h = cfg+0xc4 + 1 = **13.1** (cfg = *(DAT_0046dd68+0x14), +0xc4 = 12.1 in RAM). High jump: 2 × 12.1 + 1 = 25.2. A story-progress branch (FUN_00310c60() >= 0x12, FUN_00310c40() >= 0x2e, weapon byte 3..4) uses cfg+0xc0 = 24.2 + 1 instead | FUN_00575768 | D (values: save state) |
| take-off | physics-body branch (script flag 13, z128): cfg+0xcc = DAT_0046ee2c = 1.2; gravity body+0xd0 = 1.2 × 196 = 235.2 u/s²; body on the ground → FUN_0034a8e0 → **FUN_00357168(h)**: v0 = −sqrt(2 g h), rise time = \|v0/g\| (= player.js `startFall(h)`) | FUN_00575768, FUN_00356358, FUN_0034a8e0, FUN_00357168 | D |
| the -80/-112 u/s values | DAT_0046ee38 / DAT_0046ee3c are used only by the non-physics branch (mode 0xb): NOT z128 | FUN_00575768 | D |
| wind-up | the jump motion state sets C+0x1c0 bit **0x20**; FUN_0034ea18 returns at once while it is set, so the body neither moves nor turns (and its fall clock doesn't run) while **h_jp0** plays. Cleared when h_jp0 ends (FUN_003460d8) or after t >= 10 ticks. Trace: ✕ on frame 10, lift-off on frame 14 (4 frames = h_jp0's 3 frames at 0.6/frame) | FUN_00598f50 phase 0/1, FUN_0034ea18 | D |
| gravity / arc | player.js `gravityStep()` (FUN_003579a8): rising v0 t + g t²/2; after the apex 0.8 g; +0.01. Apex 13.09 at +17 frames after lift-off, 36 frames airborne | FUN_003579a8 | D, T |
| terminal speed | none (no cap in FUN_003579a8) | — | D |
| horizontal carry | nothing is set at take-off in this branch. The body keeps C+0xc0 × C+0xcc from the ground (running: 1.0 u/tick = 60 u/s) | FUN_00575768 | D |
| air steering | FUN_00578748 states 8/9/0x1f-0x21 (player.js `airIntent`): v += normalised stick × step × cap × 0.075, capped at cap = 60/60 u/tick; stick released: speed −= cap × 0.025 per frame | FUN_00578748 | D (worker 0079) |
| walls in the air | the body walks its ground point on the walk mesh and slides (collision.js air mode) | FUN_00357d48 | D (worker 0061) |
| landing | body lands when y passes the ground point and t > rise (player.js `integrate`); ground bit set that frame | FUN_003590c8 | D |
| state 8 → 2 | the frame AFTER the ground bit: the jump motion state (phase 2/3) sees the ground bit and sets its done flag; the player state's event handler FUN_00538aa0 returns 2 (3 if P+0x100 bit 44) and FUN_00538a30 maps 8 → 2 to motion state 0x21 (landing) | FUN_00598f50, FUN_00538aa0, FUN_00538a30 | D (handlers) / L (which event carries the done flag) |
| landing recovery | motion state 0x21 (FUN_00599aa0): plays **h_jp4** at once (blend 0); ends when h_jp4 ends, at t >= 30 ticks, or at t >= 2 once C+0x1d8 > 0 (he moves); next motion 3 (idle) or 8 (moving); 6 / 9 when armed. The player is in state 2 the whole time, so he can walk off it | FUN_00599aa0 | D |
| second (mid-air) jump | **yes**, once per jump: ✕ again in the air → state 0x1f, a full new take-off (h 13.1) from where he is, with its own 4-frame freeze; c_wjp0 → c_wjp1 → c_wjp2 → h_jp3. A third press does nothing | FUN_00575698, FUN_00575768 | D, T (j_double) |
| back jump | **yes**: ✕ while the move direction is more than 90° from the facing (e.g. stick pulled back while running) → state 0x20, c_bj0 → c_bj1 → c_bj2 → h_jp3; he keeps facing forward while flying back, then turns to the stick after the apex | FUN_00575768, FUN_005a1510, FUN_00333150 | D, T (j_back) |
| jump from a run | the same arc and timing; only the carried 60 u/s differs (no extra height or speed) | FUN_00575768 | D, T |

### Clips and timing (unarmed)

Clip slots (C+0x7fc, names read from the save state): 0x16 h_jp0, 0x17 h_jp1, 0x18 h_jp2,
0x19 h_jp3, 0x1a h_jp4; second jump 0xd5 c_wjp0, 0xd6 c_wjp1, 0xd7 c_wjp2, 0xd8 h_jp3; back
jump 0xdb c_bj1; armed 0xbc.. h_jp0_a.. (with partner slots 0xc2.., 0xc8..).

FUN_00598f50 (motion state 0x20), once per frame before the character's move; t += step (1.2)
at the end of every call:

| phase | does | next |
|---|---|---|
| 0 | C+0x1c0 \|= 0x20 (freeze); blend 0 (FUN_00340bb8(C, 0, −1)); play h_jp0 now (flag 2); sound 0x50; t = 0 | 1 (8 second jump, 4 armed) |
| 1 | when h_jp0 ended or t >= 10: unfreeze; queue h_jp1, h_jp2 (flag 0), h_jp3 (flag 1 = loop) | 2 (t not reset) |
| 2/3 | t > 30 → 3 (FUN_005ae618(MP, 2)); ground bit → done (FUN_005ae618(MP, 3)) | — |
| 4-7 | armed: the h_jp*_a sets (FUN_00345818), each part after its clip or 20 ticks, re-issued when the aim zone changes; 5/6 go straight to 7 on the ground | — |
| 8 | second jump: like 1 with c_wjp1, c_wjp2, then blend 4 and h_jp3 (loop) | 2 |

Motion player (head layer): frame += 0.5 × 1.2 = 0.6 per frame (FUN_005aa2a8, decoded); a
one-shot with nothing queued holds at n − 1 (h_jp0 2.0, h_jp4 14.0) and counts as ended
there (FUN_003460d8); a queued clip takes over at frame 0 on the advance that would pass
n − 1 (h_jp1 8.4 → 9.0 → h_jp2 0.0; c_wjp2 7.8 → h_jp3 0.0). This hold/queue rule is
inferred from the traces (GUESS in the code; exact on all 7 traces). In the standing trace: ✕ frame 10 → h_jp0 (11-14) → h_jp1 (15-30)
→ h_jp2 (31-46) → h_jp3 (47-52, looping) → landing bit on 51 → state 2 on 52 → h_jp4 from 53 →
h_st (idle) from 78. `ClipClock` in jump_core.js reproduces exactly this.

### Second jump

FUN_00575768 in states 8, 9, 0x20: the gate FUN_00575698 must pass: C+0x79c bit 0x20 clear
(not used yet), C+0x664 in {0, 1, 2}, not (airborne and P+0x2a8 bits 8 and 0x10), and the
current motion state id (*(C+2000)+0x70) is 0x20 (jump), 0x22 (fall) or 0x59 (back jump).
Then C+0x79c |= 0x20, state 0x1f (0x21 for the back variant), and the body re-takes off in
the air with FUN_00357168(h): h −0.1 if P+0x2a8 bit 0, and minus min(height above ground − h, 5)
when air shots were fired (P+0x270 > 0). Motion: phase 0 → c_wjp0 then phase 8.
Trace j_double (✕ at 10, 30, 45): state 0x1f on frame 30, C+0x79c = 0x30, body t reset to 0
and frozen for frames 31-34 (c_wjp0), new take-off v0 −78.5 (same h), c_wjp1 (to 9.0), c_wjp2,
h_jp3 with blend 4 (layer shows 5 = n + 1), lands 78, state 2 on 79, h_jp4 from 80. The third
✕ (45, in state 0x1f) does nothing (FUN_00575768's switch has no 0x1f case).
Second jump from a fall (state 9, motion 0x22) is allowed by the gate too; not traced.

### Back jump (state 0x20)

FUN_00575768: FUN_0036b058(angle) with angle = FUN_00353458(C) = the signed angle from the
facing (C+0x20) to the move direction (C+0x90) about the up axis (FUN_00260e74). Outside ±90°
and with slot 0xdb (c_bj1) usable, the state is 0x20 (0x21 for the second jump). (The
FUN_0034b638(C, C+0x2d0) it also calls is overwritten by FUN_00578748 the same frame.)
Motion state FUN_005a1510 (id 0x59): like the jump with c_bj0 (0xda) in the wind-up, then
c_bj1, c_bj2, blend 4, h_jp3; second jump c_bwj0-2 (0xe0-0xe2); armed c_bj0-2 at 0xdd-0xdf.
Turning: in states 0x20/0x21 FUN_00333150 sets P+0x29c = 5, and FUN_00578748 (LAB_00579288)
negates the turn target in mode 5, so he turns AWAY from the stick (keeps facing forward).
With the stick past DAT_0046e518 = 0.5, once the body clock t > rise + margin (margin
DAT_0046e51c = 0.06 if the stick is more than 90° from P+0x80, else DAT_0046e520 = 0) it sets
P+0x2a8 bit 2, after which P+0x29c = 2 and he turns to the stick. Every new jump clears bit 2.
Trace j_back (run, stick back on 30, ✕ on 31): state 0x20, freeze 2 frames (c_bj0 holds at
1.0), back at 60 u/s, bit 2 set on frame 53 (t 0.40 > 0.334 + 0.06), turned to the stick by 64.

## Traces and test

`node --import ./native/web/test_hooks.mjs native/web/test_jump.mjs` replays each trace's
pad input through jump_core's `Jump.frame` + player.js's PlayerController (real collision),
camera rows from the trace (record i+1: the camera updates before FUN_00333150), position
re-synced on each ✕ frame (removes player.js's ground-run drift, 2.2e-3). Per frame it checks
position, per-frame displacement, facing, player state, ground bit, C+0xcc, the jump clip and
clip frame on the head layer, the body's take-off record (t, v0, rise, dy) and the frame the
landing hands back to locomotion. 64 checks, all pass:

| trace | what | result |
|---|---|---|
| e_jumps | standing jump 10, running jump 140 (hits the railing in the air) | pos 1.7e-4, facing 8e-8, apex 13.086 (game 13.09), all frames exact on state/ground/clips |
| i_air_release | running jump, stick released in the air (decay) | pos 1.7e-4, speed 3e-6 |
| j_stand | standing jump | exact (body record < 1e-4) |
| j_run | jump while running, stick held through the landing | landing exits after 2 ticks (moving) on the game's frame |
| j_steer | standing jump steered right then left; running jump pulled back in the air | pos 2.1e-3 (railing slide) |
| j_double | second jump in the air, third press ignored | exact |
| j_back | back jump | pos 2.7e-3 (railing slide), facing exact incl. the turn-round |

Residuals (outside jump_core): sliding along the z ≈ −1113.35 railing in the air loses
~1e-4 per frame against the game (collision.js air walker); the first airborne step of a
running jump is 2.5e-4 longer in the game (player.js integrate, state 0 → 2 handover).
Tolerances: 3e-3 absolute, 3e-4 per frame.

Mutation test (scratchpad `mutate.py` of worker 0095; 24 mutants of jump_core.js): 22 killed.
Survivors: h_jp3 not looping (no trace stays in the air long enough to wrap h_jp3's 16
frames), and dropping the C+0x79c bit-0x20 test in the gate (it can't change the result from
state 0x1f, which FUN_00575768 rejects anyway). Killed include: height, take-off, the
wind-up freeze and its clip-end exit, every clip queue, the 1-frame landing event delay,
the motion-update order, the landing's clip-end and moving exits, the second-jump gate and
re-take-off, the back-jump angle test, turn negation and apex margin.

## Not ported / open

- Sounds 0x50 (take-off) and 0x51 (landing): FUN_003412d0 / FUN_00341330.
- Camera hooks in FUN_00575768: CAM+0x1e8 &= ~2, FUN_00329ff0(CAM, 0), and FUN_0032a0f8(CAM, 1, 0)
  when P+0x2a8 bit 0. For the camera translation (player.js FollowCamera).
- FUN_00574b98 (weapon state P+0x2a4 3 → 0, 4 → 2) and FUN_00565df8(P, 1) (P+0x100 bits 46-47).
- FUN_005ae618(MP, 1/2/3): a motion-player mode switch during the jump; meaning not decoded
  (the cloth? see cloth.json's h_jp windows).
- The high jump (kind 1) and the cfg+0xc0 height: translated, never taken in z128.
- The armed phases 4-7 and the landing's armed set: translated, not trace-checked.
- GUESS: state 9 (fall) / state 0x20 → second jump: their vt+0x24 (0x538bd8 / 0x53a478)
  mapping to the motion id is not read; jump_core assumes FUN_00538a30's (0x1f → 0x20,
  0x21 → 0x59).
- GUESS: the jump motion's done flag is the event that sends state 8 → 2 (handlers decoded,
  the event plumbing not; the frame agrees on all traces).
- GUESS: P+0x80 (the back jump's turn-round angle test) = the facing C+0x20 (equal in the
  save state; its writer isn't found). j_back passes with it.
- GUESS: the motion player's hold / queue take-over rule (ClipClock), see Clips above.
- c_wjp / c_bj clip lengths come from the traces (the c_ clips are not exported to
  data/vincent); c_bwj (second back jump) lengths unknown, so ClipClock can't end them.
- h_jp3's loop never wraps in these traces (6 frames of h_jp3 before landing); the loop flag
  is from the play flags (lflags 5) but untested by a long fall.
- The first airborne step of a running jump is 2.5e-4 longer in the game than in player.js
  (1.2003 vs 1.2000; later steps agree). That is player.js's air walker (integrate state 0 → 2
  handover), not the jump. test_jump allows 2e-4 per frame.

## On the page (worker 0102, 2026-10-28)

Done as below: player.js `step` calls `jump.takeOff` (✕ edge = `input.cross` or Space in
`input.pressed`; `ladder` = an action took the press or owns him), `stickMode`, `intent`
(airIntent negates the turn in mode 5: backTurnFix folded in, so Jump.frame no longer calls
it), `jump.motionUpdate`, then `moveTick` / `rotate` unless frozen. `SpeedAnimator.jumpFrame`
makes the jump's ClipClock the head layer (draw before the advance, like the locomotion
clock); the frame after the landing ends, the locomotion choice runs from a fresh index
(j_stand f78: h_st 0.6, blend 11, h_jp4 blending out), and the index stays fresh until the
table's first install (e_jumps f123: h_wk 0.3). aim.js `clips()` returns while `jump.head`.
`play.js` loads the `jump` pack by default.
Check: `node --import ./native/web/test_hooks.mjs native/web/test_jump_page.mjs` (7 traces
through the real step + the animator's clock, no re-sync; plus aim_g's armed jump for flight /
ground; ✕ priority; the gate branches no trace reaches). Mutation (scratchpad mutate.py of
worker 0102): 18/18 killed (ladder priority, Space edge, frozen bit in player.js and
jump_core, move / turn gating, freeze exit, the gate's flag / state / lock branches, hand-back).
Open: the armed h_jp*_a set (GUESS: plain set; see fidelity-audit "Jump"), the c_wjp / c_bj
clips (not exported: the head keeps its pose), the jump's camera hooks (the landing frame in
review/jump-page.png is framed differently), sounds.

## Wiring it into player.js (for Dan / the next worker)

jump_core's `Jump.frame(ctrl, input, cross, opts)` is the reference order. In
`PlayerController.step()`:

1. Before `this.stickInput(input)`: run the ✕ test + take-off:
   `if (canJump(this, jump.motionId(this), { pressed: cross, ladder })) startJump(...)` and
   create `new JumpMotion(second)` (what `Jump.frame` does). The ladder action must get ✕
   first (FUN_00333150 order): pass `ladder: true` when the ladder action took the press.
2. `stickInput` / `intent` stay as they are (they already take the air path for 8 / 0x1f-0x21),
   plus two small hooks from jump_core: `stickMode(ctrl, stick)` right after `stickInput`
   (P+0x29c, the back jump's turn-round bit) and `backTurnFix(ctrl)` right after `intent`
   (mode 5 negates the turn target). Alternatively fold both into player.js's
   stickInput / airIntent.
3. After `intent`, before `moveTick`: the motion-state update (`jump.motion.update(io)`), with
   io.ground = `this.onGround` (last frame's ground bit), io.clipEnded from the head layer.
   On done: `P.state = 2`, motion → `LandMotion`.
4. `moveTick()` and `rotate()` only when `!jump.frozen` (FUN_0034ea18's C+0x1c0 bit 0x20).
5. The clip requests go to the animator instead of the locomotion choice while a jump or land
   motion is active (`req.now` = replace the head with blend `req.blend`, else queue;
   `req.loop`). After `LandMotion` is done (next 3 / 8) the locomotion clip choice takes over
   (FUN_00346860, blend 0x14 to idle).
6. player.js's GUESS state switch in `moveTick` (2 ↔ 9) leaves 8 / 0x1f alone, which is right;
   keep it.
7. Keyboard: Space is already ✕ for the ladder (ladder.js `KEY`); the jump uses the same key.

## Landing hand-back (worker 0103)
FUN_005aa2a8: `frame += rate x weight/128 x step`; if layer +0x1e bit 0 (loop): reset to 0 at n,
return 1; else clamp to n - 1, return 1. FUN_005aa5b8 advances the blend-out layer (MP+0x44a =
head - 1) with the same function, so a non-looped clip blending out stays on its last frame.
Traced j_stand f78-88: out h_jp4 14.0 (lflags 6) while h_st blends in over 11. Page fixed in
player.js advLayer; one-shot pack clips use the pack's `frames` (glb duration is (n - 1)/60).
