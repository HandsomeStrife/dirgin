// z128's enemies: e027 soldiers, and any other type export_web_enemy.py exported (e048, e030,
// e036, e023, e024, e001). Separate module, hooked from play.js; ?enemies=0 turns it off. The AI
// (enemy_ai.js) drives the save's live records 44-49 / 66 (play.js `extra`); the push-out is enemy_ai.js pushOut.
// Start state = the sea-of-flames save: the stage-load soldiers 41 / 42 are dead there and have
// no body, so they are not drawn (placements.json `dead`, export_web_placements.py SAVE_STATE_DEAD).
//
// Data (gitignored, tools/byo/build_web.sh): data/enemies/placements.json
// (export_web_placements.py, from 06800001.bin sections 0/1), MODEL.glb / MODEL.json /
// MODEL_local.bin / MODEL_weapons.json (export_web_enemy.py). Decode notes: notes/enemy-placement.md,
// notes/enemy-model.md.
//
// Which placements are drawn stays the game's rule below: alive at the save (records 44 / 45 from
// play.js) + every record enemy_ai activates later (trigger scripts' cmd 0x108: onActivate -> add();
// 0x109 / 0x130: onDeactivate -> remove()). e027 / e048 / e030 are driven by their enemy_ai actor (its
// clip each frame; enemy_ai AI_TYPES); other types stand in their idle clip (GUESS, no AI actor). Viewer-only option ?enemies=all (off by default, not the
// game): every enemy placement of stage section 1 (ai.json records, e0xx models) stands in its
// idle clip, alive or not, to check the models.
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
import { boneRows } from './enemy_hit.js';
import { rootSpeed as clipRootSpeed } from './aim.js';
import { mpDrops, carryFrame } from './enemy_ai.js';

const DIR = 'data/enemies/';
const CLIP = 'r_gst';            // GUESS: the idle the brain requests at activation (FUN_0055b8e0 from the brain; not decoded). Save state: idle soldiers play r_gst / r_gst_lk
// GUESS: idle clip of the other types (not decoded: no actor of these types is active in the save
// state; notes/enemy-model.md "Other types"). The stand clip of the motion set the type would use:
// e048 carries a rifle like e027 and has the same m001 r_* set; the others' stand clip in m000 / m001.
const IDLE = { e027: CLIP, e048: 'r_gst', e030: 'st_a', e036: 'm_gst', e023: 'idle', e024: 'idle', e001: 'n_st' };
const ALL = typeof location !== 'undefined' && new URLSearchParams(location.search).get('enemies') === 'all';
const RATE = 0.5;                // MP+0x40c of every idle e027 in the save state (= FUN_003456c0 rate 1.0, as Vincent's)
// A request's blend length = C+0x591 + 1 at the request (enemy_ai C.blendLen, FUN_00344608; C+0x591 is
// loaded by FUN_00340bb8 per motion class, 10 = C+0x6c0's default from FUN_0033ff18 as player.js
// GAME.blendFrames). T: k_fire_emp15 layer +0x1d = 11. BLEND_DEFAULT: an AI without C.blendLen.
const BLEND_DEFAULT = 10;
const VARIANT = 0;               // GUESS: LOD byte obj+0x44b (who sets it not decoded; notes/enemy-model.md); 0 = MODEL.glb
// GUESS: MP+0x450 (set by script, FUN_00342240). Read from the save state's idle soldiers
// (records 44-49, 66: 122 for six, 128 for 66), not from 41 / 42 (shot dead before the save).
const SCALE_BYTE = 122;
// GUESS: model bbox centre (MP+0x90 + MP+0xa0)/2 for FUN_005a72a8's actor point, read from the
// save state's idle soldiers (object 154, e027 idle; not records 41 / 42); FUN_005a8850 ->
// FUN_005b0380 not decoded (as Vincent's). The other types use e027's value too (?enemies=all only today).
const ACTOR_CENTRE = [-0.2927, -8.2564, -0.7635];

// Weapon (decoded, notes/enemy-model.md "Rifle"): FUN_00315e90 -> FUN_00316058 slot 0 = the
// parameter set's +0x44 name (z128 soldiers: w003, some w004) -> FUN_00316270 attaches it at
// marker 15 (slot + 0xf); FUN_005ab8f0 draws it right after the body with world = owner world x
// palette[bone] x T(marker xyz) (FUN_005ad508), with the owner's light rig. e027 marker 15 =
// bone 26 (child of the right hand 16), offset 0. The glb holds the weapon in that socket space,
// so the mesh takes joint j26's world matrix, as gun.js does with Vincent's j56.
// Checked on hero dump 163235 (w003 low LOD, 290 drawn triangles, all UV-matched): shape fit
// scale 0.993-1.0005, RMS 0.15-0.32 u; placed through bone 26 of each soldier's best-fit idle
// pose: median 0.04-0.12 u from the game's rifle (4 soldiers). Winding cw 145 / ccw 14: BackSide.
// Other owners: e048 (w003 / w004, bone 28) and e036 (w004, bone 29) carry the same glbs; their
// marker 15 offset is 0 too (MODEL_weapons.json bone / marker_offset, export_web_enemy.py --map-only).
// Types with no weapon slot in z128 (e030, e023, e024, e001) have no map (export_web_enemy.weapon_names).
const ARMED = new Set(['e027', 'e048', 'e036']);
const weaponGlb = {};
async function loadWeapons(model) {
  let map;
  try { map = await json(DIR + model + '_weapons.json'); }
  catch (e) {
    console.warn(`${model} weapons not shown:`, e.message);
    return null;
  }
  return { map, bone: map.bone ?? 26 };                        // a map from before 'bone' was written: e027's 26
}
async function weaponGltf(name) {
  try { return await (weaponGlb[name] ??= new GLTFLoader().loadAsync(DIR + name + '.glb')); }
  catch (e) { console.warn(`enemy weapon ${name} not shown:`, e.message); return null; }
}
async function attachWeapon(weapons, rec, scn, body, U, litPatch) {
  const name = weapons?.map.records[rec];
  const g = name && await weaponGltf(name);
  if (!g) return null;
  const sock = 'j' + String(weapons.bone).padStart(2, '0');    // marker 15 -> its bone (marker.mkd; e027 26)
  let joint = null;
  scn.traverse(n => { if (!joint && n.name === sock) joint = n; });
  if (!joint) { console.warn(`enemy ${rec}: no joint ${sock}, weapon not shown`); return null; }
  const mesh = g.scene.clone(true);
  mesh.traverse(o => {
    if (!o.isMesh) return;
    o.frustumCulled = false;
    o.material = o.material.clone();
    const m = o.material;
    if (m.map) { m.map.colorSpace = THREE.NoColorSpace; m.map.needsUpdate = true; }
    m.side = THREE.BackSide;                                   // measured on the hero dump (see above)
    // the owner's rig (FUN_005ab8f0). Static mesh: the 'cloth' variant of program D reads the
    // normal attribute through modelMatrix (no skinning)
    if (U && litPatch) litPatch(m, U, true);
  });
  mesh.matrixAutoUpdate = false;
  mesh.traverse(o => { o.matrixAutoUpdate = false; o.matrix.identity(); });
  // joint world at draw time = owner world x palette[26]; after the body (added last: drawn after it)
  mesh.traverse(o => { if (o.isMesh) o.onBeforeRender = () => { o.matrixWorld.copy(joint.matrixWorld); }; });
  body.add(mesh);
  return { name, mesh };
}

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

// extra: section-1 records the AI drives at the start (enemy_ai.js; ai.json records, up = (0,-1,0)
// as every z128 placement). Later activations (cmd 0x108, enemy_ai onActivate) come through add().
export async function setupEnemies({ root, litPatch, light, extra = [] }) {
  const P = await json(DIR + 'placements.json');
  // dead in the save state (the page's start): no body, nothing drawn (FUN_00559748 builds none for
  // a dead status). deadRecs: the same records for enemy_ai (createEnemyAI deadRecs: it refuses them)
  const deadRecs = P.save_state_dead?.records ?? P.placements.filter(p => p.dead).map(p => p.rec);
  const fromRec = r => ({ rec: r.rec, tag: 'rec' + r.rec, model: r.model, pos: r.pos, face: r.face, up: [0, -1, 0] });   // GUESS: ai.json carries no up vector; (0,-1,0) as for 41/42 (the up check can't fire here)
  let list = P.placements.filter(p => IDLE[p.model] && !p.dead).concat(extra.map(fromRec));
  if (ALL) {
    // viewer only (?enemies=all): every enemy placement standing; the AI still drives its own records
    const A = await json(DIR + 'ai.json');
    list = A.records.filter(r => /^e\d{3}$/.test(r.model)).map(fromRec);
  }
  // one export per model, loaded on first use: MODEL.glb (variant), sidecar, local pose, weapon map
  const models = {};
  const loadModel = m => models[m] ??= (async () => {
    const clip = IDLE[m];
    if (!clip) throw new Error('no idle clip known');
    const stem = VARIANT === 0 ? m : `${m}_v${VARIANT}`;
    const [gltf, side, lpBuf] = await Promise.all([
      new GLTFLoader().loadAsync(DIR + stem + '.glb'),
      json(DIR + m + '.json'),
      fetch(DIR + m + '_local.bin').then(r => { if (!r.ok) throw new Error(`${m}_local.bin: HTTP ${r.status}`); return r.arrayBuffer(); }),
    ]);
    const lp = side.local_pose;
    if (!lp || !lp.clips[clip]) throw new Error(`${m}.json: no local pose for ${clip}`);
    // weapons (export_web_enemy.py weapon): {record: name}; one static glb per name (weaponGltf)
    const weapons = ARMED.has(m) ? await loadWeapons(m) : null;
    return { gltf, side, lpBuf, lp, clip, weapons };
  })().catch(e => {
    if (m === 'e027') throw e;                                 // the AI's soldiers: enemies off, as before
    console.warn(`enemies: ${m} not drawn:`, e.message);
    return null;
  });
  // e027 always (the AI's model; a failure turns the enemies off), then the start list's types
  await loadModel('e027');
  for (const m of new Set(list.map(p => p.model))) await loadModel(m);
  const s = SCALE_BYTE / 128;
  const Y = new THREE.Vector3(0, 1, 0);
  const actors = [];
  const pending = new Set();                                   // records whose body is loading
  async function spawn(pl) {
    const M = await loadModel(pl.model);
    if (!M) return null;
    const { gltf, side, lpBuf, lp, clip, weapons } = M;
    const scn = cloneSkinned(gltf.scene);
    const prims = [];
    scn.traverse(o => {
      if (!o.isMesh) return;
      o.frustumCulled = false;                                 // skinned: the bind-pose bounds lie
      o.material = o.material.clone();                         // own light-2 uniforms per soldier
      const m = o.material;
      if (m.map) { m.map.colorSpace = THREE.NoColorSpace; m.map.needsUpdate = true; }
      // measured on e027 and e048 (see Winding above).
      // GUESS: e030 / e036 / e023 / e024 / e001 use the same back-face winding (not measured; no dump shows them; notes/fidelity-audit.md 'Other enemy types winding')
      m.side = THREE.BackSide;
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
    // FUN_005a9db0 through SpeedAnimator's own code (setLocalPose / poseLocal use only this.lp)
    const pose = Object.create(SpeedAnimator.prototype);
    pose.setLocalPose(lp, lpBuf, scn);
    const a = { rec: pl.rec, tag: pl.tag, model: pl.model, side, lp, body, prims, pose, frame: 0, clip };   // GUESS: frame 0 at activation, all in phase
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
      a.U = U;
    }
    a.weapon = await attachWeapon(weapons, pl.rec, scn, body, a.U, litPatch);
    pose.poseLocal(clip, 0, null, 0, 1);
    return a;
  }
  // start list in order (play.js adds these after vin.body: the draw order above)
  for (const pl of list) { const a = await spawn(pl); if (a) { root.add(a.body); actors.push(a); } }

  // A record activated after the start (enemy_ai onActivate: cmd 0x108 -> FUN_00559748 builds its
  // body): drawn at its placement. e027 / e048 / e030: enemy_ai's actor drives it (tick). Other models:
  // GUESS: they stand in their idle clip at the placement (no AI actor: enemy_ai AI_TYPES;
  // notes/fidelity-audit.md 'e030 brain states 7 / 8 ...').
  // Draw order: FUN_005a7978 links a new MP right after the list head, so it draws before every
  // older actor: the body goes in front of the first drawn actor (Vincent's body at the start).
  async function add(record) {
    const k = record.rec;
    if (pending.has(k) || actors.some(a => a.rec === k)) return null;      // already drawn (?enemies=all, a re-send)
    pending.add(k);
    const a = await spawn(fromRec(record)).catch(e => { console.warn(`enemies: record ${k} not drawn:`, e.message); return null; });
    if (!pending.delete(k)) { if (a) dispose(a); return null; }   // deactivated while loading: never drawn
    if (!a) return null;
    const head = mpHead();
    root.add(a.body);
    if (head) { const c = root.children; c.splice(c.indexOf(a.body), 1); c.splice(c.indexOf(head), 0, a.body); }
    linked.unshift(a.body);                                    // right after the list head (newest first)
    actors.push(a);
    return a;
  }
  function dispose(a) {
    for (const o of a.prims) o.material.dispose();
    if (a.weapon) a.weapon.mesh.traverse(o => { if (o.isMesh) o.material.dispose(); });   // attachWeapon's cloned materials
  }
  // cmd 0x109 / 0x130 (enemy_ai onDeactivate). GUESS (enemy_ai 'Actor deactivation'): the body goes.
  function remove(record) {
    const k = record.rec;
    pending.delete(k);
    const i = actors.findIndex(a => a.rec === k);
    if (i < 0) return false;
    const [a] = actors.splice(i, 1);
    // FUN_005a7d88 (MP destructor): plain unlink, prev->next = next. A body linked after the head
    // leaves its place to its successor, so the next add still goes in front of the newest one left.
    const j = linked.indexOf(a.body); if (j >= 0) linked.splice(j, 1);
    root.remove(a.body);
    dispose(a);
    return true;
  }
  // Bodies activated after the start, in link order (newest first), and the first start body they
  // all draw before (play.js: Vincent's). A new body goes in front of linked[0], else of base.
  const linked = [];
  let base = null;
  const mpHead = () => { const h = linked[0] || base; return h && h.parent === root ? h : null; };
  // bone b's world rows (enemy_hit boneRows) of a drawn soldier's last pose (SpeedAnimator's composed
  // model-space R / S / P), or null when the record has no body or was not posed yet
  function partPose(rec) {
    const a = actors.find(x => x.rec === rec);
    if (!a || !a.posed || !a.pose.lp) return null;
    const { R, S, P } = a.pose.lp, h = a.posed.heading, face = [Math.sin(h), 0, Math.cos(h)], pos = a.posed.pos;
    return b => boneRows([R[b].x, R[b].y, R[b].z, R[b].w], [S[3 * b], S[3 * b + 1], S[3 * b + 2]],
                         [P[3 * b], P[3 * b + 1], P[3 * b + 2]], pos, face);
  }
  // FUN_005aa2a8 on one layer: frame += rate x weight/128 x step (weight byte 128); a looped layer
  // resets to 0 at n, any other holds n - 1 (player.js advLayer)
  function advance(a, L, rate = RATE) {
    L.frame = Math.fround(L.frame + Math.fround(Math.fround(rate * 1.0) * STEP));
    const n = a.side.clips[L.clip].frames;
    if (L.loop) { if (n <= L.frame) L.frame = 0; }
    else if (n - 1 <= L.frame) L.frame = n - 1;
  }
  return {
    actors, deadRecs, add, remove, partPose,
    // FUN_00345e58 -> FUN_00344340: a clip's root speed (aim.js rootSpeed, its GUESS frame unit) from a drawn
    // actor of that model; null when no such actor or clip is loaded
    rootSpeed(model, clip) {
      const a = actors.find(x => x.model === model && x.side.clips[clip]);
      return a ? clipRootSpeed(a.side.clips, clip) : null;
    },
    // the drawn actor that draws first (FUN_005a7978 list head side); play.js sets Vincent's body
    set mpHead(o) { base = o; },
    // one game frame: draw pose = this frame's clip frame(s), then FUN_005aa2a8's advance.
    // ai (enemy_ai.js world, optional): each motion request of an AI-driven soldier (C.req, C.clip,
    // C.loop) pushes a head layer and the old head becomes the blend-out layer (MP+0x448 head / +0x44a
    // blend-out index): the new head's weight MP+0x408 = (len - rem) / len, len = C+0x591 + 1 (11),
    // rem -= step per frame; the head and, while blending, the blend-out layer advance. When rem
    // reaches 0 the blend-out layer is dropped. T: k_fire_emp15 (#66 / #67 MP layers, blend_len
    // 9.8 -> 0 by 1.2, the lower layers frozen) and Vincent's SpeedAnimator (same code, player.js).
    // A looped layer (+0x1e bit 0, flags 0x83) wraps at n; the others hold n - 1.
    tick(ai) {
      // the part test (enemy_ai playerBulletHit -> FUN_005ad118) reads each soldier's MP skeleton:
      // the pose drawn last (FUN_005a87a0 from MP+0x180), placed as the body is drawn
      if (ai && !ai.boneWorld) ai.boneWorld = s => partPose(s.rec);
      for (const a of actors) {
        const s = ai && ai.actors.find(x => x.rec === a.rec);
        // body matrix: the AI's body C+0x20 facing (enemy_ai turnBody, FUN_0034de50) and C+0x30
        // position, drawn as at setup (play.js bodyYaw: rotation.y = atan2(face.x, face.z) + pi)
        if (s) { a.body.position.set(...s.pos); a.body.rotation.y = Math.atan2(s.face[0], s.face[2]) + Math.PI; }
        const has = c => !!(a.lp.clips[c] && a.side.clips[c]);
        if (!a.head) a.head = { clip: a.clip, frame: a.frame, loop: true };
        // first frame under the AI: its body's clip as it stands (no request yet, no blend)
        if (s && a.req === undefined) {
          a.req = s.C.req;
          if (has(s.C.clip) && s.C.clip !== a.head.clip) a.head = { clip: s.C.clip, frame: 0, loop: s.C.loop !== false };
        }
        if (s && s.C.req !== a.req) {
          a.req = s.C.req;
          // FUN_005a91c8 / FUN_005a9398 (enemy_ai mpDrops): a request with flags bit 0x80 (all of enemy_ai's: 0x82 /
          // 0x83 / 0x86 / 0x87) is dropped while the top layer (a.head: the page always has one) already plays
          // that clip with that partner (plain = none).
          // T: w159_walk95 MP+0x448 (test_path_walk).
          const drop = mpDrops(s.C.flags, a.head, s.C.clip, s.C.partner);
          if (!drop && has(s.C.clip)) {
            a.out = a.head;
            // a locomotion-table switch (FUN_00346860, enemy_ai C.reqCarry) starts the new clip at the old
            // head's frame (FUN_005a8fd0: min(max(2 frame, 0), 2 n - 1) / 2); a first choice / request at 0
            const n = a.side.clips[s.C.clip].frames;
            const fr = s.C.reqCarry ? carryFrame(a.head.frame, n) : 0;
            a.head = { clip: s.C.clip, frame: fr, loop: s.C.loop !== false, partner: s.C.partner ?? null };
            const len = (s.C.blendLen ?? BLEND_DEFAULT) + 1;
            a.blend = { len, rem: len };
          }
        }
        const b = a.blend, blending = a.out && b && b.rem > 0;
        if (!blending) a.out = null;
        const w = blending ? (b.len - b.rem) / b.len : 1;                    // MP+0x408
        a.weight = w;                                                          // drawn this frame (tests)
        a.pose.poseLocal(a.head.clip, a.head.frame, a.out ? a.out.clip : null, a.out ? a.out.frame : 0, w);
        a.clip = a.head.clip; a.frame = a.head.frame;
        a.posed = { pos: a.body.position.toArray(), heading: a.body.rotation.y - Math.PI };
        // MP+0x40c: the body's locomotion rate (enemy_ai C.rate, FUN_00346c10) while a table is installed,
        // else 0.5 (FUN_003456c0 rate 1.0). GUESS (other types): e027's RATE for every type
        const rate = s && s.C.rate != null ? s.C.rate : RATE;
        advance(a, a.head, rate);
        if (blending) { advance(a, a.out, rate); b.rem = Math.fround(b.rem - STEP); if (b.rem <= 0) b.rem = 0; }
        a.frame = a.head.frame;
      }
    },
  };
}
