// z128's e027 soldiers. Separate module, hooked from play.js; ?enemies=0 turns it off. The AI
// (enemy_ai.js) drives records 44 / 45 (play.js `extra`); no collision or damage taken yet.
// Start state = the sea-of-flames save: the stage-load soldiers 41 / 42 are dead there and have
// no body, so they are not drawn (placements.json `dead`, export_web_placements.py SAVE_STATE_DEAD).
//
// Data (gitignored, tools/byo/build_web.sh): data/enemies/placements.json
// (export_web_placements.py, from 06800001.bin sections 0/1), e027.glb / e027.json /
// e027_local.bin (export_web_enemy.py). Decode notes: notes/enemy-placement.md.
//
// Which placements: FUN_00319190 at stage load loads a status whose placement has flag bit 3
// and whose tag is an event actor (FUN_00376938); FUN_005453b0 -> FUN_00558f68 activates it.
// The event-actor tags are a GUESS stand-in in the exporter (EVENT_ACTORS). Of those, the e027s
// are records 41 / 42 (e_Sol_11 / 12); record 43 is e048 (no model export yet). All three are dead
// and bodiless in the save (HP 0, actor +0x28 bit 0 clear, +0xe8 = 0): skipped below.
//
// Matrix: FUN_00315050 copies +0xc / +0x1c / +0x2c into the status, FUN_0055a168 builds
// rows (up x face, up, face, pos). The page's body convention (play.js bodyYaw): heading
// h = atan2(face.x, face.z), rotation.y = h + pi, position = feet = row 3.
//
// Pose: the game's per-bone local path (FUN_005a9db0, player.js SpeedAnimator.poseLocal) on
// e027_local.bin; the clip advances as FUN_005aa2a8: frame += MP+0x40c x step, looped.
// Lighting: VU1 program D (play.js litPatch), the same rig as Vincent (FUN_005ab0b0): env
// slot 0 light 0 + ambient x MP+0x450/128, light 2 = the room light picked at the actor point.
// Hero dump 163235: e027 vertex colours (45,49,61) = ambient x 122/128, (48,52,64) = x 128/128.
//
// Draw order: FUN_005a8e28 draws the motion-player list from DAT_005b3788 in link order (then
// FUN_005a8ec0 the MP+0x428 bit-8 ones), and FUN_005a7978 links each new MP in right after the
// list head, so newer actors draw before older ones. Hero dump 163235: the event soldiers
// (records 44-47, 66; seq 56322-59261) -> Vincent (59465-62977) -> record 42 (64378-66678), i.e.
// record 42's MP is older than Vincent's. play.js adds these bodies after vin.body, which is that
// order (the renderer has sortObjects = false). GUESS: 41 before 42 (their MP creation order at
// stage load is not traced; record 41 is not drawn in the dumps).
//
// Winding: enemy_dump_check.py (glb index order vs the dump's drawn / ADC=1 triangles, mirror side
// by position): drawn are clockwise on screen = WebGL back faces, ADC ones counter-clockwise.
// var 0 hero: drawn cw 756 / ccw 2, ADC ccw 639 / cw 5; var 2 hero: 664 / 11, 635 / 2; var 2 walk
// 185126: 646 / 5, 648 / 8. So THREE.BackSide, as Vincent's.
import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { clone as cloneSkinned } from 'three/addons/utils/SkeletonUtils.js';
import { SpeedAnimator, STEP } from './player.js';

const DIR = 'data/enemies/';
const CLIP = 'r_gst';            // GUESS: the idle the brain requests at activation (FUN_0055b8e0 from the brain; not decoded). Save state: idle soldiers play r_gst / r_gst_lk
const RATE = 0.5;                // MP+0x40c of every idle e027 in the save state (= FUN_003456c0 rate 1.0, as Vincent's)
const VARIANT = 0;               // GUESS: LOD byte obj+0x44b (who sets it not decoded; notes/enemy-model.md); 0 = e027.glb
// GUESS: MP+0x450 (set by script, FUN_00342240). Read from the save state's idle soldiers
// (records 44-49, 66: 122 for six, 128 for 66), not from 41 / 42 (shot dead before the save).
const SCALE_BYTE = 122;
// GUESS: model bbox centre (MP+0x90 + MP+0xa0)/2 for FUN_005a72a8's actor point, read from the
// save state's idle soldiers (object 154, e027 idle; not records 41 / 42); FUN_005a8850 ->
// FUN_005b0380 not decoded (as Vincent's)
const ACTOR_CENTRE = [-0.2927, -8.2564, -0.7635];

async function json(path) {
  const r = await fetch(path);
  if (!r.ok) throw new Error(`${path}: HTTP ${r.status}`);
  return r.json();
}

// light 2 for a fixed actor point: FUN_0026a710 pick (play.js light.pick), colour rgb*k*100/d^2.
// The enemies don't move, so FUN_005ab608's smoothing stays settled on the first light
// (GUESS: start settled, as Vincent's; notes/fidelity-audit.md "Character lighting").
function light2(light, p, dir, col) {
  dir.set(0, 0, 0); col.set(0, 0, 0);
  const r = light.pick(p);
  if (!r) return;
  dir.set(r.pos[0] - p.x, r.pos[1] - p.y, r.pos[2] - p.z);
  const d = dir.length();
  if (d !== 0) { dir.multiplyScalar(1 / d); col.set(...r.rgb).multiplyScalar(r.k * 100 / (d * d)); }
  else dir.set(0, 0, 0);
}

export async function setupEnemies({ root, litPatch, light, extra = [] }) {
  const P = await json(DIR + 'placements.json');
  // extra: section-1 records the AI drives (enemy_ai.js; ai.json records, up = (0,-1,0) as every z128 placement)
  // dead in the save state (the page's start): no body, nothing drawn (FUN_00559748 builds none for a dead status)
  const list = P.placements.filter(p => p.model === 'e027' && !p.dead)
    .concat(extra.map(r => ({ rec: r.rec, tag: 'rec' + r.rec, model: r.model, pos: r.pos, face: r.face, up: [0, -1, 0] })));   // GUESS: ai.json carries no up vector; (0,-1,0) as for 41/42 (the up check can't fire here)
  if (!list.length) return null;
  const stem = VARIANT === 0 ? 'e027' : `e027_v${VARIANT}`;
  const [gltf, side, lpBuf] = await Promise.all([
    new GLTFLoader().loadAsync(DIR + stem + '.glb'),
    json(DIR + 'e027.json'),
    fetch(DIR + 'e027_local.bin').then(r => { if (!r.ok) throw new Error(`e027_local.bin: HTTP ${r.status}`); return r.arrayBuffer(); }),
  ]);
  const lp = side.local_pose;
  if (!lp || !lp.clips[CLIP]) throw new Error(`e027.json: no local pose for ${CLIP}`);
  const s = SCALE_BYTE / 128;
  const Y = new THREE.Vector3(0, 1, 0);
  const actors = [];
  for (const pl of list) {
    const scn = cloneSkinned(gltf.scene);
    const prims = [];
    scn.traverse(o => {
      if (!o.isMesh) return;
      o.frustumCulled = false;                                 // skinned: the bind-pose bounds lie
      o.material = o.material.clone();                         // own light-2 uniforms per soldier
      const m = o.material;
      if (m.map) { m.map.colorSpace = THREE.NoColorSpace; m.map.needsUpdate = true; }
      m.side = THREE.BackSide;                                 // measured on e027 (see Winding above)
      prims.push(o);
    });
    const flip = new THREE.Group(); flip.rotation.x = Math.PI; flip.add(scn);   // glb Y-up -> game -Y-up
    const body = new THREE.Group(); body.add(flip);
    // FUN_0055a168's matrix is (up x face, up, face, pos); the page body takes only the face as a
    // yaw, which is the whole matrix only while up = (0,-1,0) (every z128 placement).
    const [ux, uy, uz] = pl.up;
    if (Math.abs(ux) > 1e-6 || Math.abs(uy + 1) > 1e-6 || Math.abs(uz) > 1e-6)
      console.warn(`enemies: record ${pl.rec} up ${pl.up} != (0,-1,0); drawn upright (face yaw only)`);
    const [fx, , fz] = pl.face;
    const heading = Math.atan2(fx, fz);
    body.position.set(...pl.pos);
    body.rotation.y = heading + Math.PI;                       // play.js bodyYaw
    root.add(body);
    // FUN_005a9db0 through SpeedAnimator's own code (setLocalPose / poseLocal use only this.lp)
    const pose = Object.create(SpeedAnimator.prototype);
    pose.setLocalPose(lp, lpBuf, scn);
    const a = { rec: pl.rec, tag: pl.tag, body, prims, pose, frame: 0, clip: CLIP };   // GUESS: frame 0 at activation, all in phase
    if (light && litPatch) {
      const p = new THREE.Vector3(...ACTOR_CENTRE).applyAxisAngle(Y, body.rotation.y).add(body.position);
      const dir = new THREE.Vector3(), col = new THREE.Vector3();
      light2(light, p, dir, col);
      const E = light.env;                                     // unscaled env slot 0 (Vincent's U carries his MP+0x450)
      const U = {
        uL0: { value: new THREE.Vector3(...E.dir0) }, uC0: { value: new THREE.Vector3(...E.col0).multiplyScalar(s) },
        uL1: { value: new THREE.Vector3() }, uC1: { value: new THREE.Vector3() },   // no effect lights on this page
        uL2: { value: dir }, uC2: { value: col },
        uAmb: { value: new THREE.Vector3(...E.ambient).multiplyScalar(s) },
      };
      for (const o of prims) litPatch(o.material, U);
      a.light = { point: p.toArray(), L2: dir.toArray(), C2: col.toArray() };
    }
    pose.poseLocal(CLIP, 0, null, 0, 1);
    actors.push(a);
  }
  return {
    actors,
    // one game frame: draw pose = this frame's clip frame, then FUN_005aa2a8's advance.
    // ai (enemy_ai.js world, optional): the clip each AI-driven soldier's body asked for
    // (C+0x570); a new clip starts at frame 0. GUESS: no blend between clips (flags 0x82 / 0x83
    // of FUN_00345818 not translated), every clip loops at its frame count.
    tick(ai) {
      for (const a of actors) {
        const s = ai && ai.actors.find(x => x.rec === a.rec);
        const want = s && lp.clips[s.C.clip] && side.clips[s.C.clip] ? s.C.clip : a.clip;
        if (want !== a.clip) { a.clip = want; a.frame = 0; }
        a.pose.poseLocal(a.clip, a.frame, null, 0, 1);
        a.frame = Math.fround(a.frame + Math.fround(Math.fround(RATE * 1.0) * STEP));
        if (side.clips[a.clip].frames <= a.frame) a.frame = 0;
      }
    },
  };
}
