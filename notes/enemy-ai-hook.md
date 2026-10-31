# Enemy AI hook (worker 0122, 2026-10-29; review fixes worker 0125)

`native/web/enemy_ai.js` translates the wake path and the awake e027's combat AI (notes/enemy-research.md
§8 / §9). Data: `native/assets/models/export_web_triggers.py` -> `data/enemies/ai.json` (build step
`enemy_ai` in tools/byo/build_web.sh; format enemy-ai-2: + section-24 weapons and each record's
weapon index). Test: `node native/web/test_enemy_ai.mjs [enemy_ai.js]` (STRICT=1: known gaps fail).
**Applied by worker 0126 (2026-10-30)**, with: the HUD says `HP 780 (enemy damage off)` without
?enemydmg=1; ai.json loads whenever enemies are on, so records 44 / 45 are drawn with ?enemyai=0 too
(AI off); dead records (HP 0) get AI mode 0 in addActor (FUN_0055cac8 -> FUN_0035f728); 41 / 42 are
no longer drawn (dead in the save, notes/enemy-placement.md). The patches below are the original text.
The patches below hook it into play.js / enemies.js. The
first version was checked in the browser on a scratch copy (trigger 95 fires, #66 / #67 go to mode 2
and fire, the drawn soldiers switch to r_st_a / r_st_f); the 0125 changes were checked with
`git apply --check` and `node --check` only.

## Steps for the manager

1. Apply: `git apply play.patch enemies.patch` (both blocks below; written against the working tree of
   2026-10-29).
2. build_web.sh already exports the AI and reaction clips (r_gst, r_gst_lk, r_st_a, r_st_f, r_stlld0,
   r_stlaf0, r_stbf0) and checks ai.json. Rebuild data/enemies: an old ai.json (no weapons) turns
   off the AI only, with a console warning; the soldiers are still drawn.
3. Audit rows: notes/fidelity-audit.md, "Enemy AI".

## What the hook does

- play.js loads ai.json, adds records 44 / 45 (#66 / #67) to the drawn soldiers and to the AI.
  GUESS: they are active from the start, as in the save (their activation by cmd 0x108 is not on the page).
- Each game frame: the player's position before and after `ctrl.step`, then `enemyAI.tick(STEP, old, new)`,
  then `enemies.tick(enemyAI)` (the clip each AI body asked for).
- **Player damage is off by default**: bullets fly (decoded countdown + flight) but don't test Vincent.
  `?enemydmg=1` turns on the GUESS hit test (BULLET_R) and the decoded damage: `php.hp` (780 from the
  save state, P status +0x20) on the HUD, logged on change. No hit reaction or death for Vincent; at
  HP 0 nothing happens.
- LOS ray: `col.segment` (GUESS: probe set 3 of FUN_005449f0 not decoded).
- `?enemyai=0` turns the AI off. A stale ai.json or a createEnemyAI error turns off the AI only.

## Decoded in the 0125 fixes

- **Weapon W**: status +0xac = stage section 24 record (FUN_00318610 / FUN_00311600), index by
  FUN_00318718: status +0x0c (= section-1 record +0x40, FUN_00315050) bit 0x16 ? record +0x54 :
  parameter set +0x88. Records 44 / 45 -> W 2 (damage 45, interval 180, reload 100, magazine 6, hit
  value 12): the save's RAM values. Records 41 -> 2, 42 -> 0, 43 (e048) -> 3, 46 / 47 -> 0, 48 / 49 -> 2.
- **Bullet** (FUN_005409a8(mgr, muzzle, aim, W, status, 0, 0) -> FUN_00554568 -> FUN_00552dd8(b, 0)):
  dir = normalize(aim - muzzle); speed +0x12c = W+0x2c / 6 (50 u per 1.0 step); range +0x15c = W+0x18 x 10
  + 15; end +0xd0 = start + dir x range (the aim point only with W+4 bit 0x400000); **+0x142 = 0 + 2
  frames of countdown** before it moves (FUN_005549f0; FUN_00554970 when it reaches 0). Then each frame
  age += step, pos = start + dir x age x speed (FUN_00556068), test prev -> pos (FUN_00556870). So a
  shot 33 u away lands 3 frames after the shot frame: f786 -> f789, f1008 -> f1011 (trace).
- **Defence FUN_00318120** for Vincent, from the save state: no armour (+0xc0 0), his W+0x68 0, level 4
  -> FUN_0031ce90 = int(0.33 x 4 + 0.4 + 0.5) = 2 (G+0x60 = 20 >= 7: factors at G+0x14 table +0x270);
  +0x60 = 0x8233 (bit 2, no 0x100) -> difficulty cut: FUN_00312698 true, FUN_002419d8(0x11) 0,
  FUN_00242c28 = DAT_0046b328 = 1 -> index 0 -> table +0xaa = 0 -> no change. 45 -> int(45.5 x 0.98) = 44.
- Manager cmd 0x2f(n, v): manager short n = v (FUN_00545b20; 0x30 reads it).

## Not decoded (two attempts): the bullet hit test against Vincent

FUN_005442d8 walks the objects along the bullet's segment, filters them (vt+0x1ac), and asks each
one's part test (vt+0x17c: for e027 FUN_0055bda8 -> FUN_005ad118, the model's per-part volumes against
the posed skeleton); the closest hit gets vt+0x1b4 (FUN_0055db50). With no object hit, FUN_00556870
probes the room (probe 0). Vincent's part test needs his per-part volumes and his animated pose,
which this module doesn't have, and the room probe set isn't decoded. #67's f875 bullet passes 2.6 u
from #66's axis at chest height, but #66 shows no hit or near miss (AI +0x70) and FUN_0055db50 has no
friendly-fire exit, so the filter skips soldiers (L); why it then misses Vincent (part test, or room
geometry between #67 on the higher deck and Vincent) is open. Stand-in: BULLET_R, off by default.

## GUESS stand-ins

All listed with what depends on them in notes/fidelity-audit.md, "Enemy AI".

## Trace match (test_enemy_ai.mjs)

Both traces, every frame, for #66 / #67: AI mode, brain state, interval, target, think time of all
seven AIs, the AI clock. engage8 also matches actor state, clip, rounds and the fire timer. Wake at f784,
#66 aims at f785 and fires at f786 (r_st_a at f788), #67 uses a round with no shot at f799 and
fires at f875, #66 fires again at f1008. With damage on: 44 per hit, HP drops at f789 and f1011 as
in the game. Plus defence() on fixed inputs and a synthetic sidestep / distance check of the bullet.
The test fits the queue's two untraced AIs and the clock phase, and takes the jitter random
numbers from the traced fire times (GameRandom's sequence is not checked; its twist step is, against
the save state's MT words, when the save is present).
Known gaps (fail with STRICT=1): #67's f875 shot hits in the sim at f879 (the game misses: hit test
GUESS); engage7 has no C+0x564 field, so #67's f799 'round used, no shot' isn't reproduced from it.

## play.patch
```diff
--- a/native/web/play.js
+++ b/native/web/play.js
@@ -15,7 +15,7 @@
 // the background after start (vincent_<pack>.glb).
 import * as THREE from 'three';
 import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
-import { TICK, PlayerController, SpeedAnimator, FollowCamera, cameraRays, KEY_WALK, KEY_RUN } from './player.js';
+import { TICK, STEP, PlayerController, SpeedAnimator, FollowCamera, cameraRays, KEY_WALK, KEY_RUN } from './player.js';
 import { attachGun } from './gun.js';
 import { loadActions } from './actions/index.js';
 import { initReport } from './report.js';
@@ -24,6 +24,7 @@
 import { createOutputStage } from './output.js';
 import { setupAim } from './aim.js';
 import { setupEnemies } from './enemies.js';
+import { createEnemyAI } from './enemy_ai.js';
 
 const params = new URLSearchParams(location.search);
 const hud = document.getElementById('hud');
@@ -32,6 +33,8 @@
 
 const SPAWN = { x: -1395.63, z: -1110.51 };
 const ENEMIES = params.get('enemies') !== '0';       // enemies.js: z128's stage-load soldiers, idle
+const ENEMY_AI = params.get('enemyai') !== '0';      // enemy_ai.js: wake trigger + awake soldier AI (?enemyai=0 = off)
+const ENEMY_DMG = params.get('enemydmg') === '1';    // enemy bullets hit Vincent (off by default: the hit test is a GUESS)
 const AIM = params.get('aim') !== '0';               // aim.js (aim_core.js): the weapon, its camera views, the reticle
 const PACKS = params.has('packs') ? params.get('packs').split(',').filter(Boolean) : (AIM ? ['ladder', 'jump', 'shoot'] : ['ladder', 'jump']);
 // Placement: the game draws the model with the motion player's matrix MP+0x10 (FUN_005ab8f0
@@ -608,8 +611,27 @@
     const prevHook = anim.drawHook;
     anim.drawHook = () => { if (prevHook) prevHook(); light.frame(actorPoint()); };
   }
-  const enemies = ENEMIES ? await setupEnemies({ root, litPatch, light })
+  // enemy AI data (export_web_triggers.py). GUESS: records 44 / 45 (#66 / #67) are active from
+  // the start, as in the sea-of-flames save (their activation by event cmd 0x108 is not on this page).
+  const AWAKE = [44, 45];
+  let aiData = ENEMIES && ENEMY_AI ? await fetch('data/enemies/ai.json').then(r => r.ok ? r.json() : null).catch(() => null) : null;
+  if (ENEMIES && ENEMY_AI && !aiData) console.warn('enemy AI off: data/enemies/ai.json missing');
+  // an old ai.json (no records 44 / 45 or no weapons) turns off the AI only, not the soldiers
+  if (aiData && !(AWAKE.every(k => aiData.records?.[k]) && Array.isArray(aiData.weapons))) {
+    console.warn('enemy AI off: data/enemies/ai.json is out of date (rebuild data/enemies)');
+    aiData = null;
+  }
+  const enemies = ENEMIES ? await setupEnemies({ root, litPatch, light, extra: aiData ? AWAKE.map(k => aiData.records[k]) : [] })
     .catch(e => { console.warn('enemies off:', e.message); return null; }) : null;
+  // Vincent's status for the enemy AI: HP 780 = P status +0x20 in the save state (k_enemy_engage8 p_hp)
+  const php = { pos: [ctrl.pos.x, ctrl.pos.y, ctrl.pos.z], hp: 780, id: 0x16, f118: 0, s64: 0 };
+  let enemyAI = null;
+  try {
+    enemyAI = aiData && enemies ? createEnemyAI({ data: aiData, player: php, log: m => console.log(m), damage: ENEMY_DMG,
+      // GUESS: FUN_005449f0 probe 3 = the page's collision triangle set (probe sets not decoded)
+      los: (a, b) => !!(col.segment && col.segment({ x: a[0], y: a[1], z: a[2] }, { x: b[0], y: b[1], z: b[2] })) }) : null;
+    if (enemyAI) for (const k of AWAKE) enemyAI.addActor({ rec: k });
+  } catch (e) { console.warn('enemy AI off:', e.message); enemyAI = null; }
   // test aids that move him (H, ?ladder=): the cloth restarts from the new pose (cloth.js reset)
   // and the camera resets as at spawn (player.js reset(): FUN_003285d0's snap; GUESS: the
   // angles are CAM_SPAWN, and whether the game resets the camera on a warp is not decoded)
@@ -772,13 +794,15 @@
       follow.tick({ pos: ctrl.pos, want: ctrl.C.dir, state: ctrl.P.state,
                     stick: moving ? (inp.run ? KEY_RUN : KEY_WALK) : 0, pad: null });
       const b = follow.basis();                    // re-aim the keys with this frame's matrix
+      const pOld = [ctrl.pos.x, ctrl.pos.y, ctrl.pos.z];
       ctrl.step(aim(inp, b));
       pressed.clear();                             // an edge is seen by one tick only
       // feet vs ground check (for the HUD / tests)
       const g = col.groundY(ctrl.pos.x, ctrl.pos.z, ctrl.pos.y);
       if (g != null) stats.groundErrMax = Math.max(stats.groundErrMax, Math.abs(g - ctrl.pos.y));
       stats.lastGroundErr = g == null ? null : g - ctrl.pos.y;
-      if (enemies) enemies.tick();
+      if (enemyAI) { php.pos = [ctrl.pos.x, ctrl.pos.y, ctrl.pos.z]; enemyAI.tick(STEP, pOld, php.pos); }
+      if (enemies) enemies.tick(enemyAI);
       acc -= TICK; n++; stats.ticks++;
     }
     if (n === 8) acc = 0;                          // fell far behind (tab hidden): drop time
@@ -805,12 +829,12 @@
       `clip ${anim.override ? 'action' : anim.current} (${anim.clipName()}) x${anim.rate.toFixed(2)}` +
       `${ctrl.active ? '   action ' + ctrl.active.name : ''}` +
       `   speed ${(ctrl.speed * 60).toFixed(1)} u/s\n` +
-      `onGround ${ctrl.onGround}${ctrl.hitWall ? '  WALL' : ''}` +
+      `onGround ${ctrl.onGround}${ctrl.hitWall ? '  WALL' : ''}` + (enemyAI ? `   HP ${php.hp}` : '') +
       (weapon ? `   weapon ${weapon.core.P.drawn ? 'out' : 'away'} st ${weapon.core.P.st} T ${weapon.core.P.T}  cam 0x${weapon.core.cam.type.toString(16)}` : '') + '\n' +
       `click: mouse look (drag works too)   WASD move (walk)   Shift run   ${actionHelp.join('   ')}   H respawn   R report`;
   }
   window.__play = { ctrl, anim, follow, aim: weapon, col, stats, camera, renderer, output, root, vin, light, spawnY: gy,
-                    playerState, camView, report, keys, pressed, motions, packsReady, actionHelp, cape, enemies };
+                    playerState, camView, report, keys, pressed, motions, packsReady, actionHelp, cape, enemies, enemyAI, php };
   requestAnimationFrame(frame);
 }
 
```

## enemies.patch
```diff
--- a/native/web/enemies.js
+++ b/native/web/enemies.js
@@ -69,9 +69,11 @@
   else dir.set(0, 0, 0);
 }
 
-export async function setupEnemies({ root, litPatch, light }) {
+export async function setupEnemies({ root, litPatch, light, extra = [] }) {
   const P = await json(DIR + 'placements.json');
-  const list = P.placements.filter(p => p.model === 'e027');
+  // extra: section-1 records the AI drives (enemy_ai.js; ai.json records, up = (0,-1,0) as every z128 placement)
+  const list = P.placements.filter(p => p.model === 'e027')
+    .concat(extra.map(r => ({ rec: r.rec, tag: 'rec' + r.rec, model: r.model, pos: r.pos, face: r.face, up: [0, -1, 0] })));
   if (!list.length) return null;
   const stem = VARIANT === 0 ? 'e027' : `e027_v${VARIANT}`;
   const [gltf, side, lpBuf] = await Promise.all([
@@ -81,7 +83,6 @@
   ]);
   const lp = side.local_pose;
   if (!lp || !lp.clips[CLIP]) throw new Error(`e027.json: no local pose for ${CLIP}`);
-  const frames = side.clips[CLIP].frames;                      // 200, looped (FUN_005aa2a8 resets at n)
   const s = SCALE_BYTE / 128;
   const Y = new THREE.Vector3(0, 1, 0);
   const actors = [];
@@ -112,7 +113,7 @@
     // FUN_005a9db0 through SpeedAnimator's own code (setLocalPose / poseLocal use only this.lp)
     const pose = Object.create(SpeedAnimator.prototype);
     pose.setLocalPose(lp, lpBuf, scn);
-    const a = { rec: pl.rec, tag: pl.tag, body, prims, pose, frame: 0 };   // GUESS: frame 0 at activation, all in phase
+    const a = { rec: pl.rec, tag: pl.tag, body, prims, pose, frame: 0, clip: CLIP };   // GUESS: frame 0 at activation, all in phase
     if (light && litPatch) {
       const p = new THREE.Vector3(...ACTOR_CENTRE).applyAxisAngle(Y, body.rotation.y).add(body.position);
       const dir = new THREE.Vector3(), col = new THREE.Vector3();
@@ -132,12 +133,18 @@
   }
   return {
     actors,
-    // one game frame: draw pose = this frame's clip frame, then FUN_005aa2a8's advance
-    tick() {
+    // one game frame: draw pose = this frame's clip frame, then FUN_005aa2a8's advance.
+    // ai (enemy_ai.js world, optional): the clip each AI-driven soldier's body asked for
+    // (C+0x570); a new clip starts at frame 0. GUESS: no blend between clips (flags 0x82 / 0x83
+    // of FUN_00345818 not translated), every clip loops at its frame count.
+    tick(ai) {
       for (const a of actors) {
-        a.pose.poseLocal(CLIP, a.frame, null, 0, 1);
+        const s = ai && ai.actors.find(x => x.rec === a.rec);
+        const want = s && lp.clips[s.C.clip] && side.clips[s.C.clip] ? s.C.clip : a.clip;
+        if (want !== a.clip) { a.clip = want; a.frame = 0; }
+        a.pose.poseLocal(a.clip, a.frame, null, 0, 1);
         a.frame = Math.fround(a.frame + Math.fround(Math.fround(RATE * 1.0) * STEP));
-        if (frames <= a.frame) a.frame = 0;
+        if (side.clips[a.clip].frames <= a.frame) a.frame = 0;
       }
     },
   };
```

## Other types (worker 0144, 2026-10-30)

**One class for every type (D).** The save state's object table (`*(DAT_0046e880+0x1164)`) holds 215
objects of vtable **0x58e1f8** (FUN_00558790): every e027, e048, e030, e036, e023, e024, e001, g001 and
n0xx placement (object +0x17c = status, +0x9c parameter set -> model name). So there is no per-type AI
class: the brain modes (FUN_0035f300), the layers, the part test vt+0x17c FUN_0055bda8 -> FUN_005ad118
and the damage path vt+0x1b4 FUN_0055db50 are shared code. What differs per type is data:

| what | where (decoded) | e027 | e048 | e030 |
|---|---|---|---|---|
| stance C+0x664 | FUN_00559748 -> **FUN_00564228** (switch on the model name) -> FUN_00340ac0 | no status / status +0x9c == 0 -> 0x1f; weapon class W+0x14 >> 2 & 7: 2 -> 0x1e, 0 / 1 / 3 / no weapon -> 0x1f, 4-7 stance kept | same rule as e027 (no case of its own) | 0x23 (+ C+0x568 \|= 0x200) |
| stance flags C+0x808 | FUN_0030fc38(n) = 'bmd' table flags[n] (06800009.bin, FUN_0030f688 -> FUN_0030fb78 -> G+0x84; file = save RAM byte for byte) | 0x872b (bit 0: brain 2 state 0 -> 3, shooter) | 0x872b | 0x8c44 (bit 0 clear -> states 7 / 8) |
| motion rows C+0x7fc | FUN_0030fc08(n): 229 x 8-byte clip names per stance | 0x1f: r_gst, 0x40 r_st_a, 0x57 r_st_f, ...; 0x1e: m_gst, m_st_a, m_st_f, ... | as e027 | st_a, look, stld0, stbf0, bl_0, jp0..4, rev_0 (no 0x40 / 0x57) |
| radius +0x2c / height +0x30 | **FUN_00563fc0**: zone header (status +0xa8) +0x20 x 0.5 (bit 0: max with +0x28) / +0x24 | 5 / 18 | 5 / 18 (sets 3 / 4) | 6 / 12 (set 8) |
| part boxes | model 'mkd' (FUN_0030baa0) | 13 | 13 (bones 6, 2, 3, ...) | 13 (head box at z 6.4: outside the 6 u cylinder) |
| zones / lock bone | section 19/20 (zone_set, lock_bone) | sets 6 / 7, bone 3 | sets 3 / 4 (head x60), bone 3 | set 8 (head x24), bone 3 |
| weapon | section 24 (FUN_00318718) | 0 / 2 / 4 / 5 / 7 | 1, 3 (class 3), 8 (class 2: interval 16) | none |

Save-state checks: +0x2c / +0x30 of every inactive actor equal FUN_00563fc0's values (e001 7.5 / 28, e023
90 / 40 with +0x28 bit 0x10000, e024 90 / 120); status +0x60 (less 0x41) / +0xc0 / +0x27 / +0x64 are 0 on
every e027 / e048 / e030, so ENEMY_STATUS (was E027_STATUS) holds for all three.

**Ported.** export_web_triggers.py: records `stance` (stance_of = FUN_00564228 for z128's names), weapons
`cls`, `stances` (flags + rows of the used stances), `sizes`, part boxes of all 7 types, `model_clips`
(frames of the requested rows per model). enemy_ai.js: AI_TYPES {e027, e048, e030} get an actor at
activation; clip names from the actor's stance rows; brain 2 state 0 -> 3 / 7 by C+0x808 bit 0; cylinder,
eye, lock point from the actor's size; clip lengths per model. Inputs: 06800009, 07650c00 / 076506c0 (m000:
the m_* rows) in disc.py. e048: the whole e027 path (wake, aim, fire with its own W, bullets, hits, flinch,
death). e030: activated, stands in st_a, brain state 7 (warn), hit / flinch (stld0) / death (stbf0).

**Not done.** e030's brain states 7 / 8 (FUN_0035ff98 attack pick -> FUN_0055baa8(actor, 0x80 + i)) and
its attack motion classes; e036 / e023 / e024 / e001 have no actor (their stances 0x1e / 0x19 / 0x1a / 0x10
are exported). **No PCSX2 trace of an e048**: none is active in the save, the tracer logs only actors active
at load (fixed addresses, fields.find_enemies), and the e048 activations near the spawn don't help: trigger 56
(168 u away) names e048 records 28 / 29 only in its `c105(0,0) == 0` branch (on the save's settings it activates
e027 50 / 51, test_enemy_ai), and 28 / 29 stand ~1250 u off, beyond the 500 u range; the others are >= 478 u away. A trace needs the tracer to follow an actor activated
mid-run (pointer-chained fields) first.

**Flinch end (worker 0145, D).** Motion state 0x1e = factory 0x5a6c28 (FUN_00592918) case 0x1e ->
FUN_00598880, vt **0x5a5c70**, update vt+0x24 **FUN_00598950** (phase +4, done +8, timer +0xc):
- 0: FUN_00340bb8; motion by C+0x590 & 0xf (hit side): 2 -> 0x24, 3 -> 0x26, 4 -> 0x27, 5 -> 0x28, 6 -> 0x29, else 0x25; timer 0 -> 1.
- 1: when the motion is done (FUN_003460d8) or timer > 120 (timer += DAT_004eeb24, BSS): done bit, then
  motion **0x40** if C+0x590 bit 24, else motion **1** (flags 0x83) -> 2. (Off the ground, C+0x1c0 0x200
  clear: done bit only.)
- 2: C+0x1c0 &= ~0x20; returns FUN_00341b20: next state 6 (bit 24) or 3 (C+0x590 bits 16-19 == 1: 0x16 / 0x12).
C+0x590 bit 24 = 'gun out', written only by FUN_00565d78 (with actor +0x100 bit 44). **Writer DECODED (worker 0146):**
FUN_0055b8e0 runs the new state's enter (actor vt+0x2fc FUN_0055bbf8 -> state class vt+0xc, classes FUN_00537628);
state 3 FUN_005380a0 / 5 FUN_00538598 set it, 2 FUN_00537e70 / 7 FUN_005386f0 clear it (0 / 4 clear with +0x118 bit 0x10000
clear; also 6, 0xa-0xe, 0x12, 0x13, 0x15-0x17, 0x1a, 0x1b, 0x1d); no leave (vt+0x14) writes it. So activation (state 2) starts
it clear, the brain's state 3 sets it, death (7) clears it. FUN_0055f9d0's clear needs status bit 7 (FUN_0055f128 /
FUN_0055f338 kind 7: FUN_0056b4e0 from overlay code 0xbeb5d0.., event script FUN_005661e8, FUN_00570c10): left out.
e030 (no row 0x40, never in state 3) goes back to row 1 = st_a. e030's C+0x568 0x200 (FUN_00564228) is read only by FUN_00341a48:
C+0x1c0 |= 0x20000 when C+0x590 bits 16-19 < 2 (a body flag, not the flinch).

## Mode 1, crouch, fire blend (worker 0149, 2026-10-31)

**Dan's report 20261031T000852Z-9974** ("guard doesn't move or acknowledge Vincent"): the alley guard is
**record 51** (e027, stance 0x1e m_*, weapon 7, script 142), 40 u north of Vincent. Trigger 56 activated
50 / 51 in mode 0; trigger 90 (Vincent walking north up the alley) sends them message 2, and script 142
sets #51 **AI mode 1** (#50: mode 2; the corpse at his feet is likely #50, shot). Mode 1 had an empty
update on the page: a **port gap**, not the game. Trigger 76 (flags 0x20, event 0x12, condition not
decoded) would also wake them; the page still never runs event 0x12.

- **Brain mode 1** (class 0x4a2588: start FUN_003628f8, update FUN_00362998) translated: mode 2's
  shooter plus a posture change in state 4 (stance flag C+0x808 bit 3; e027 / e048 0x872b): once 0x79
  ticks have passed since the last change, each run draws FUN_0040b5c0; < 0.2 -> actor state 5 (crouch)
  or 3 (stand), combat layer off, 0x1f ticks later combat on again. No target -> state 6 / 7 (mode 2's 5 / 6).
  States 8 / 9 = the non-shooter attack (e030): warn, not translated.
- **Actor state -> body motion** (FUN_0055b8e0: old state's class vt+0x24(new) -> FUN_005641c0): 3 -> 5
  motion 0x18, 5 -> 3 motion 0x19, 2 -> 3 motion 5 (FUN_00538120 / FUN_005385f8 / FUN_00538340 ->
  FUN_00537d58). Classes from the factory 0x5a6c28's compare tree (FUN_00592918 is not a Ghidra
  function; walked from the ELF): 5 FUN_005945e8 (update FUN_005946a0), 6 FUN_00594898 (FUN_00594978),
  0xf vt 0x5a6300 (FUN_00597c60), 0x16 FUN_00597108 (FUN_005971d8), 0x18 FUN_005977e8 (FUN_005978e8),
  0x19 vt 0x5a5df0 (FUN_00597ae8). Update = vt+0x24, enter vt+0x14, leave vt+0x1c.
- **Crouch** (actor state 5 enter FUN_00538598 -> FUN_00565bd0(a, 1)): stance 1, height +0x174 = 0.6 x
  +0x170 (FUN_00563fc0). Clips m_sq_a / m_sq_f (rows 0x78 / 0x7b) now exported for e027 / e048.
- **Fire clip** (Dan's point 2): the clips were right (row 0x57 = r_st_f / m_st_f) but the page cut
  clips hard. New trace **k_fire_emp15** (engage8's inputs to f830, `TRACE_FIELDSET=enemy TRACE_ENEMIES=2
  TRACE_EMP=1`: each enemy's MP head + layers): every request pushes a head layer, the old head blends
  out over 11 clip ticks (+0x24: 9.8 -> 0 by 1.2) and only the head and blend-out layer advance. #66
  pushes r_st_a (loop) f785, r_st_f (no loop, partner r_st_fd) f786, r_st_a (loop) f788: so r_st_f
  plays as the blend-out layer at full weight, fading over ~9 frames (frame 1.8 -> 7.2). enemies.js now
  does exactly that (head + blend-out, as Vincent's SpeedAnimator). test_enemy_ai checks both soldiers'
  pushes (frame, clip, loop) against the trace; test_enemies_page step 7 checks #51's fade on the page.
  **Missing:** the up / down partner rows (r_st_fu / r_st_fd, picked by the aim pitch C+0x680 in
  FUN_00345818): not exported; the middle row plays.
- **Melee** (Dan's point 3), decoded, **not ported**: the attack pick FUN_0035ff98 -> FUN_00347bd8 scans
  the attack table C+0x7e8 (= zone header +0x34, FUN_00340760; +2 count, entries FUN_00347978, flags
  +6, conditions FUN_00347af0 from FUN_003479c0) and starts actor state 0x80 + i (FUN_0055baa8). Gate
  FUN_00310c40 = G+0x64 = 48 in the save (>= 0x18). Save RAM: e027 / e048 have 1 attack, e030 7.
  Shooters (C+0x808 bit 0) never reach the pick in brain modes 1 / 2; modes 3 (FUN_00361690), 5
  (FUN_003639d0 state 10) and 6 (FUN_00364558) call it without that check. Next: the 0x80+ actor state
  classes (FUN_00537628) -> attack motions, hit window and damage to Vincent; mode 5 (most soldiers'
  hit reaction, e.g. records 69 / 70 on wake) and mode 3 (e030s) are also untranslated, so those still
  stand. Survey (scripts' 0x143 per wake n / hit): 13 records activate in mode 1, ~30 branches set
  mode 1, ~40 mode 2, ~35 mode 5, ~32 mode 3 (mostly e030), 1 mode 6.

## Review fixes (worker 0150, 2026-10-31)

- **State -> motion tables** read from the ELF (FUN_00537628 classes' vt+0x24): states 0 / 1 use
  FUN_00537aa0 (jump table 0x589e70: new 2 -> motion 3, 3 -> 6, 5 -> 0x16); 2 FUN_00537d58; 3 FUN_00538120;
  5 FUN_005385f8. #66's 1 -> 3 = motion 6 (the old r_st_a GUESS is gone; k_fire_emp15 pushes still match).
  FUN_005641c0's same-motion no-restart holds for every class when FUN_0055b8e0's param_3 is 0 (the brains);
  activation passes 1 (old class = state 0's, always restarts).
- **Activation** (FUN_00559748 plain placement): FUN_0055b8e0(a, 2, 1, 0) -> state 2, motion 3 (class 3 not
  ported). The page's 44 / 45 start in the save's states 1 / 2 (play.js AWAKE_STATE, T engage8 f760), so #67's
  wake is 2 -> 3 = motion 5 (r_st_a:0, then r_st_a:1 8 frames later, as the trace).
- **Blend length** per request: FUN_00344608 uses C+0x591 (0 with C+0x564 bit 2); FUN_00340bb8 loads it
  (motions 0x18 / 0x19: 10; 6 / 0x16 phase 0: C+0x6c0, default 10 from FUN_0033ff18). Flinch's 5 / none is a GUESS.
- **Brain 1 states 8 / 9** translated around the attack pick (FUN_0035ff98 stand-in: none).
- **Found:** the game's flinch sets C+0x564 0x8000 (busy; k_hits12 f870), which the page lacks. G+0x64: 48
- **Ported (worker 0151):** flinch class 0x1e (vtable 0x5a5c70) enter 0x5988b8 = C+0x564 |= 0x8000|0x4000|0x10000; leave 0x5988f0 clears those and C+0x1c0 bit 0x20 (disassembled from the ELF). Effect on the page: a soldier woken from state 2 by a hit finishes the flinch (row 1, gun bit still clear) before the brain enters state 3. Death frame (k_hits12 f871) shows 0x4000c000: the death motion's enter sets 0x8000|0x4000 again (not read).
  here vs '< 27' in stagger()'s comment.
- No mode-1 trace exists (the tracer can't follow actors activated mid-run): the mode-1 timing test checks
  the decoded waits (0x79 / 0x1f rounded up to the 5-tick brain run), not game frames.

## Modes 5 / 6, melee (worker 0152, 2026-10-31)

**Brain mode 5** (class 0x4a2770, FUN_003638f8): start 0x363988 (ELF; not a Ghidra function): AI +0x20
interval 10, C+0x1c0 |= 0x1000000 | 0x4000000 (mode 3's 0x361648: the same with 5; mode 6's 0x364548:
interval 5). Update FUN_003639d0, translated in `brain5` (states 0-0x12). Summary: 0 -> 5 (actor state 3,
combat layer 2, action layer 2, +0x74 / +0x70 cleared) -> 6: visible and d34 < 22 -> 10 (attack pick ->
0xb -> 0xe); < 3 hits taken: < 3 misses -> after 0x79 ticks FUN_0035f7c0(ai, 0) -> 0xc (action 6) / 7
(+0x78 > 3) / 0xe; >= 3 misses -> 7 / 0xe; >= 3 hits -> 0xc. 7 -> 8 (action 3 = approach) waits for
C+0x1c0 0x100000 (move ended) or d34 < 25. FUN_0035f7c0 / FUN_0035f8b0 = odds from AI +0x98 / +0x99 =
section-1 record +0x4c / +0x4d (now exported: records[].b98 / b99; z128 soldiers 100 / 0 -> p 1.0).
AI +0x74 / +0x78 = the AI's bullets that ended on nothing / on something (FUN_00554ea8 -> FUN_00568e20 /
FUN_00568df8). **Mode 6** FUN_00364558 in `brain6`. Records 69 / 70 (scripts 24 / 25) set mode 5 in events
2, 3, 8 (hit) and 0x13.

**Action layers** (FUN_0035f540 -> FUN_0035f590, vtables 0x4a2a28 (1) .. 0x4a2908 (7), 0x4a28d8 (0xb);
update vt+0x1c: 2 FUN_00366390, 3 FUN_003667b0, 5 FUN_003670e8, 6 FUN_003672d8, 7 FUN_00367508) are
**not ported**: they ask for paths on the stage's node graph (FUN_00563970 / FUN_00563d20 node of an
object, FUN_0033e118 node record (+0xe area byte), FUN_00368120, request FUN_0055a628 with a 16-byte
struct / FUN_0055a590) or approach directly (FUN_00353110(20, 1000, C, target) + mover 3). That graph and
the mover are the next decode for enemy movement. Mode 3 (FUN_00361690) is mostly these calls.

**Melee.** Attack table T = zone header + u32 header +0x18 (FUN_0030fc68, version >= 0x16), relocated by
FUN_0030fd08: +2 count, +0x10 entries (16 B: +0 length, +2 / +4 keyframes, +6 flags, +8 / +0xa distance,
+0xc yaw, +0xd tolerance, +0xe pitch nibbles), +0x14 keyframes (frame, first command, count), +0x18
commands (4 x s16), +0x1c names (8 B: clips and sounds), +0x24 hit volumes (64 B), +0x28 projectile
records (16 B). Pick FUN_00347bd8 (see the audit row). Start FUN_0055baa8 -> actor state 0x80 + i (class
0x80 for 0x80-0xbf, FUN_00537490) -> body motion 0x1000 + i (FUN_00537aa0 table 0x589e70: 0x537bf4 =
st + 0xf80) = factory 0x5a6c28's vtable 0x5a4270: enter FUN_0059e830, update FUN_0059eb18 (timer += step;
keyframes whose frame <= (int)timer run their commands FUN_00348138 -> FUN_003481e0; done once all ran
and timer >= length), leave FUN_0059e938. Commands go on to the owner through FUN_00341988(C, 4, cmd) ->
the actor's handler (jump table 0x58de60 [4] -> 0x56a1c8, per-op table 0x58de90): op 0 / 1 -> actor
state 1 when not attacking; op 2 -> FUN_00569dc0 (melee hit volume: FUN_00541310 builds an attack object
from the 64-byte record, placed on a bone FUN_0054a768 / FUN_0054a7d0, damage +0xc2 scaled by status
+0x56); op 3 -> FUN_00311600 (section-24 record: e030's projectile); 6 -> 0x56a2c4, 7 -> 0x56a2b0, 0xc ->
0x56a320 (not read). e027 zone sets 6 / 7: m_atk / r_atk_0 (1-40 / 1-30, hit at keyframe 9 / 13, length
66); e048 3 / 4: r_atk / m_atk; e030 set 8: 7 entries (atk_0..3, jp0..2, b_stp, st_btl). **Not found:**
who leaves the attack state (class vt+0x1c = 0x539ea0 gives the state; its caller is not in the decomp:
GUESS stand-in). **Next:** the attack object class of FUN_00541278 / FUN_00541310 (its update and overlap
test against Vincent) for melee damage behind ?enemydmg=1.

**Worker 0153 fixes (review of 0152).** Mode 5 state 0xe: C+0x1c0 |= 0x10 when FUN_0035f890 (=
FUN_0035f7c0 ^ 1) returns 0, i.e. when the odds pass (was inverted). AI +0x20 is the scheduler interval
in every write (mode 1 state 9, mode 6 state 6, the starts). Actor states 6 / 0x1b (vtables 0x58ae00 /
0x58add0): enters FUN_00538218 / FUN_005382e0 = FUN_00565bd0(a, 0), FUN_00565d78(a, 0) (gun bit clear),
+0x118 |= 0x200; leaves 0x538260 / 0x538328 clear +0x118 bit 0x200; vt+0x24 FUN_00538278 / FUN_00538340
(same state 0, 8 / 0x1f 0x20, 9 0x22, 0x20 / 0x21 0x59, else FUN_00537d58). State 5's other rows go
FUN_00538420 -> FUN_00537aa0, so 0x80 + i from 5 / 6 / 0x1b is motion 0x1000 + i. Mode 6 states 2-5
re-pick unconditionally (FUN_0035fe80 -> FUN_005608e8, FUN_0035fec0, FUN_0035fc98): translated with the
candidate GUESS (the player); FUN_0035fec0's two-target switch (FUN_0054aa50 -> FUN_0054a9d0) warns.

## Dan's play report: melee, push-out, platform soldiers (worker 0154, 2026-10-31)

New trace **k_push16** (`TRACE_FIELDSET=enemy TRACE_ENEMIES=2 TRACE_PUSH=1`; engage8's route, stick held to
f900 so Vincent runs on into #66 along the ladder-top railing, no shots). New opt-in fields (fields.py
TRACE_PUSH): actor +0x28, +0x2c..+0x3b (radius, height, +0x38), +0x118, +0x130 for Vincent and the soldiers,
Vincent's C+0x430 / C+0x1c0.

- **No melee (point 1): the game doesn't either.** e027 / e048 are shooters (C+0x808 bit 0), and modes 1 / 2
  reach the attack pick FUN_0035ff98 only in their non-shooter states (FUN_00362ef8 state 7, FUN_00362998
  state 8). k_push16: #66 woken (mode 2, f784) with Vincent 4-7 u from him for ~200 frames stays in actor
  state 3; no 0x80+ state. Melee happens from modes 3 / 5 / 6 only (most soldiers' scripts switch to 5 when
  hit; worker 0152's page test step 8 sets that up by the hit event). test_push_out checks the trace and the
  page AI on the same path (0 attack frames; an invented mode-2 swing is caught: 216).
- **Push-out (point 2): FUN_00542ab8**, from the manager frame FUN_0053b500 before the actors move. Actors
  on list +0x1274 with +0x38 bit 9 (Vincent 0x381, live soldiers 0x387; a corpse's 0x183 leaves it). Clears
  each +0x130; for each pair with horizontal distance <= r_i + r_j: i's low point (feet + r_i up) against
  j's cylinder (FUN_0025d508: flat end at the feet, round end at the top), overlap = r_i - |surface - low|,
  push_i += normal x overlap^2 / r_i x 0.25 (DAT_0046e90c; x1 with j's +0x118 bit 0x4000000), skipped
  while i attacks (FUN_0055bb40) unless +0x28 bit 2, not added with i's +0x118 bit 0x10000000 (#67 has it);
  then the same for j. Matches k_push16 to 7e-5. FUN_00343160 copies +0x130 to C+0x430; the code that
  adds it to the move was **not found** (three attempts). The trace fits "ground move = walk x step +
  push, then the walk-mesh slide" to 0.002 u (player.js, GUESS row). #66's push is never applied (he
  stands still), so the page applies none to soldiers.
- **Platform soldiers (point 3):** the save has seven live soldiers, records 44-49 and 66 (k_enemy_engage8
  f760: active, HP > 0, actor states 1 2 1 1 2 2 1); the page only spawned 44 / 45. play.js AWAKE now has
  all seven (46-49 on the raised floor north-east of the ladder top, 66 by the west walkway). Triggers
  89 / 77 / 76 / 78 already name them; 77 / 76 / 78 are event-0x12 volumes the page still doesn't run.

## Platform soldiers' wake, push-out tests (worker 0155, 2026-10-31)

- **Event 0x12 = a bullet crossing the volume (D).** Trigger vtable 0x58cdf8 +0x1b4 = FUN_005511e8, called
  only from the bullet ray test FUN_005442d8 (FUN_00556870 <- the bullet update). FUN_005503e8 gives shape
  0 / 8 triggers with flags +0xa4 bit 5 (0x20): +0x40 |= 0x10 (FUN_005511e8 runs the event), +0x38 |= 1
  (bullets test it) and +0x28 |= 0x100 (see-through: listed, never the closest hit). Filters for a trigger:
  vt+0x1ac 0x552838 returns 1 (every bullet, the soldiers' too), vt+0x8c / +0xb4 / +0xbc return 0. Ray test
  vt+0x17c FUN_00550da0(o, 0, ..): segment to the box frame, FUN_0025e400(end, start) (clip; returns the face
  where its 2nd point was clipped), and if -1, FUN_0025e400(start, end): entry point, else exit point when
  the start is inside, nothing when both ends are inside. A listed volume nearer the segment start than the
  closest solid hit gets event 0x12 with r0 = bullet +0x128 (owner id), r1 = that object's +0x20 (or -1).
  Scripts 98-101 don't read r0 / r1. Walking into 75-78 fires nothing (bits 0 / 1 clear); trigger 89
  (ev 0xd, on the lower floor east, x -788 z -966) is the walk-in wake of 46-49.
- enemy_ai.js `shotVolumes` (both Vincent's and the soldiers' bullets), `clipSeg` = FUN_0025e400,
  `inBoxBut` = FUN_0025f638. test_shot_volumes.mjs; test_enemies_page step 9 (fresh page: R1 over the
  platform from the deck -> 77 -> 46-49 woken: 48 / 49 mode 1; 46 / 47 mode 0 + a script sub-routine the
  VM stops at, op 37 / 39 / 40 + cmds 0x142 / 0x131: not translated).
- Consequence: #51 stands inside trigger 76 (script 99, not once): each of his shots wakes 46-50. In the
  page test this brought his next shot forward through the shared RNG, so step 7 disarms 76 for its
  blend comparison (test aid).
- **Push-out step (test gap, not decoded).** player.js integrate per frame with pushOut (test_push_out):
  the step along the game's path matches (0.005 u) but the position is 0.81 u off sideways. The game's
  steps fit walk + (push . walk direction) x walk direction to ~0.005 u, and standing next to #66
  (f909-f1020, push 0.58) he only jitters +-0.03 u. So the push does not enter as a displacement the way
  the page's stand-in adds it. Not ported (a fit, not a decode); the reader of C+0x430 is the next decode.
- MISSING / GUESS labels in pushOut: FUN_0054b3a0 +0x28 bit 0x10000 offset (warns), +0x38 bit 9 stand-in.
  Pair skip (both +0x28 0x20 and both +0x118 0x20000) translated. #67's +0x118 0x10000000 (never pushed in
  the game) has no setter on the page.

## Push into the step, shot-volume gaps, #51's re-wakes (worker 0156, 2026-10-31)

- **Where C+0x430 enters the move: still not decoded** (attempts 4-5). REA's rizin backend isn't installed
  (`rizin` ENOENT), so the same scan was done by hand: every lw / lwc1 / lq / lqc2 / ld with offset
  0x430-0x438 in the ELF hits only stack slots (FUN_0035a440's r29 0x430 / 0x438 are locals) and
  FUN_0034c388's zero test (via `addiu r2, 0x430(r21)`); FUN_00358fb8 / FUN_003590c8 / FUN_00357d48 /
  FUN_0035a440 never read body (C+0x3b0) + 0x80. player.js now uses worker 0155's fit (GUESS row):
  move = w + (push . w_hat) w_hat, nothing while idle. k_push16: full position 0.005 u (f809-f826), idle
  0.000 u, closest approach 3.996 vs 3.999 u.
- **The f827 stop is not the push.** The stick is held to f900 (lx 204, ly 0; facing 113.5 deg, travel 87.9 deg
  along the railing from f796, before any contact). At f827 the game cuts his step to 0.35 u and from f828
  holds him 6.55 u past #66 (+-0.1 u alternating), with the push (0.49, -0.34) pointing along his walk.
  C+0x1c0 stays 0xd0f04 (0x10f04 only at f825 / f826). The page's walk mesh has nothing there; he walks
  ~3 u on. Unknown blocker (object primitive in FUN_0035a440?): test_push_out `gap` line.
- shotVolumes: MISSING label for +0x120 bit 0x800 bullets (FUN_005442d8's radius path, vt+0x184 /
  vt+0x19c); skipped with a warning.
- #51 / trigger 76: script 99 re-runs on every one of his shots, but the targets' event-2 scripts (126 / 127
  / 128 / 141 / 142) open with `cmd 0x105 (var 0) == 0`, then `cmd 0x104 var 0 = 1`: the wake is run-once per
  soldier, and later shots rebuild no brain (test_shot_volumes 7; mutant 0x105 -> 0 caught). No PCSX2 trace
  (#51 must first be woken by trigger 90; not in the save). Open: 46-49 were active at the save, so their
  var 0 may already be 1 there, while the page starts it at 0.
