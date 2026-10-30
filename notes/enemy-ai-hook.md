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
