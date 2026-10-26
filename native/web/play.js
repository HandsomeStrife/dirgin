// Dirge z128, playable: Vincent (data/vincent/vincent.glb) walking the room with
// third-person controls, a follow camera and collision (collision.js, or the stub).
//
// The room comes from room.js (main.js's loader and shader, with its z-fight fixes).
// Game space, -Y up. URL params: hud=0, aa=1, dpr=N, col=stub (force the stub),
// cam=ex,ey,ez,yaw,pitch (detached camera at that view; H re-attaches it),
// actions=0 (no action modules), packs=a,b (clip packs to load; default jump,ladder,shoot,
// packs= loads none), zprio=0 / zunder=0 (room.js depth-tie offsets off). Packs load in
// the background after start (vincent_<pack>.glb).
import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { TICK, PlayerController, SpeedAnimator, FollowCamera } from './player.js';
import { loadActions } from './actions/index.js';
import { initReport } from './report.js';
import { loadRoom } from './room.js';

const params = new URLSearchParams(location.search);
const hud = document.getElementById('hud');
const msg = document.getElementById('msg');
if (params.get('hud') === '0') hud.style.display = 'none';

const SPAWN = { x: -1395.63, z: -1110.51 };
const PACKS = params.has('packs') ? params.get('packs').split(',').filter(Boolean) : ['jump', 'ladder', 'shoot'];
const ROOT_PER_FRAME = { walk: 0.525, run: 1.665 };   // clip root motion, glb units per 60 Hz frame
// The glb's origin is not at his soles: export_web_vincent.py centres the root on the
// bind-pose body (feet at model y = 12.13), but the in-place clips put the feet near
// model y = 0, so every clip draws him ~11.67 world units above the origin (worker 0053).
// measureSole() finds the lowest skinned vertex in h_st frame 0 at load and the body is
// shifted by that much, so his soles sit on the controller's feet. Measured: h_st
// -11.670 (constant), h_wk -11.58..-11.68 (planted foot), h_rn -11.56 lowest.
let FEET_OFFSET = 0;

// Lowest point (largest y, -Y up) of the skinned body relative to body.position, in
// world units, for the pose the mixer currently holds.
function measureSole(vin, root) {
  root.updateMatrixWorld(true);
  const v = new THREE.Vector3();
  let maxY = -Infinity;
  for (const m of vin.prims) {
    if (!m.isSkinnedMesh) continue;
    const n = m.geometry.attributes.position.count;
    for (let i = 0; i < n; i++) {
      m.getVertexPosition(i, v); v.applyMatrix4(m.matrixWorld);
      if (v.y > maxY) maxY = v.y;
    }
  }
  return maxY - vin.body.position.y;
}

// ------------------------------------------------------------ room (room.js)
// the static posed Vincent in scene.json is replaced by the animated glb
const loadRoomPlay = () => loadRoom({
  skip: d => d.kind === 'char',
  onProgress: (n, t) => { msg.textContent = `loading room ${n}/${t}`; },
});

// ------------------------------------------------------------ Vincent
// The game draws Vincent's flag-bit-1 groups (hair cards, some cloth edges; material
// 'alpha') with ABE (Cs-Cd)*As+Cd and ATE GEQUAL ref, AFAIL = RGB_ONLY: EVERY fragment
// blends by its alpha, and only those with As >= ref write depth (HANDOFF "Uncaptured-model
// render"; render_scene_textured -charatest does the same). Two passes per primitive, as
// room.js does for the room: pass 1 keeps a >= ref (blend + depth write), pass 2 keeps
// a < ref (blend, no depth write). The game draws the groups in one fixed order (glb
// primitive extras.drawOrder, opaque and alpha runs interleaved). The renderer runs with
// sortObjects = false, which ignores renderOrder and draws every `transparent` material
// after all opaque ones, so these stay non-transparent and the meshes are put in
// drawOrder in the scene graph instead.
const CHAR_AREF = 0.77734;                         // placement.json char_alpha_ref (glb alphaCutoff)
function gsBlend(m) {
  m.transparent = false;
  m.blending = THREE.CustomBlending;
  m.blendEquation = THREE.AddEquation;
  m.blendSrc = THREE.SrcAlphaFactor; m.blendDst = THREE.OneMinusSrcAlphaFactor;
  m.blendSrcAlpha = THREE.ZeroFactor; m.blendDstAlpha = THREE.OneFactor;
}
function gsAlphaPasses(prims) {
  // glb primitive extras.drawOrder: GLTFLoader puts primitive extras on the geometry
  const order = (o, i) => {
    const d = o.geometry.userData?.drawOrder ?? o.userData?.drawOrder;
    return Number.isFinite(d) ? d : i;
  };
  const below = new Map();                         // pass-1 material -> pass-2 material
  const afail = [];
  for (const o of prims) {
    const m = o.material;
    if (!(m.name === 'alpha' || m.alphaTest > 0)) continue;
    const ref = m.alphaTest > 0 ? m.alphaTest : CHAR_AREF;
    m.alphaTest = ref; m.depthWrite = true; gsBlend(m);
    let m2 = below.get(m);
    if (!m2) {
      m2 = m.clone(); m2.alphaTest = ref; m2.depthWrite = false; gsBlend(m2);
      m2.onBeforeCompile = sh => {
        sh.fragmentShader = sh.fragmentShader.replace('#include <alphatest_fragment>',
          'if ( diffuseColor.a >= alphaTest ) discard;   // GS AFAIL pass: keep a < ref');
      };
      m2.customProgramCacheKey = () => 'gs-afail';
      below.set(m, m2);
    }
    const o2 = new THREE.SkinnedMesh(o.geometry, m2);
    o2.bind(o.skeleton, o.bindMatrix);
    o2.frustumCulled = false; o2.name = (o.name || 'prim') + '#afail';
    o2.userData = { ...o.userData, afail: true };
    afail.push([o, o2]);
  }
  // scene-graph order = draw order: each parent's primitives by drawOrder, pass 2
  // right after its pass 1 (other children, e.g. joint nodes, keep their places)
  const idx = new Map(prims.map((o, i) => [o, order(o, i)]));
  for (const parent of new Set(prims.map(o => o.parent))) {
    const kids = parent.children.filter(c => idx.has(c)).sort((a, b) => idx.get(a) - idx.get(b));
    const seq = kids.flatMap(c => [c, ...afail.filter(([o]) => o === c).map(([, o2]) => o2)]);
    let k = 0;
    parent.children = parent.children.flatMap(c => (idx.has(c) ? (k++ === 0 ? seq : []) : [c]));
    for (const c of seq) c.parent = parent;
  }
  return afail.map(([, o2]) => o2);
}

async function loadVincent(scale) {
  msg.textContent = 'loading vincent…';
  const gltf = await new GLTFLoader().loadAsync('data/vincent/vincent.glb');
  const prims = [];
  gltf.scene.traverse(o => {
    if (!o.isMesh) return;
    o.frustumCulled = false;                       // skinned: the bind-pose bounds lie
    prims.push(o);
    const m = o.material;
    if (m.map) { m.map.colorSpace = THREE.NoColorSpace; m.map.needsUpdate = true; }  // raw texel, like the room
    m.side = THREE.DoubleSide;
  });
  const afail = gsAlphaPasses(prims);
  // gltf.scene is Y-up (root node 0 flips the game's -Y-up model). Flip back with
  // Rx(pi) so it sits in game space; glb +Z forward becomes game -Z.
  const flip = new THREE.Group();
  flip.rotation.x = Math.PI;
  flip.add(gltf.scene);
  const body = new THREE.Group();                  // position = feet, rotation.y from heading
  body.scale.setScalar(scale);
  body.add(flip);
  const clip = n => {
    const c = gltf.animations.find(a => a.name === n);
    if (!c) throw new Error(`vincent.glb has no clip ${n}`);
    return c;
  };
  return { body, mixRoot: gltf.scene, clips: { idle: clip('h_st'), walk: clip('h_wk'), run: clip('h_rn') }, prims, afail };
}

// clip sidecars: vincent.json + each pack's json -> { clip name: entry }
async function loadMotions(name) {
  const r = await fetch(`data/vincent/${name}.json`);
  if (!r.ok) throw new Error(`${name}.json: HTTP ${r.status}`);
  return (await r.json()).clips || {};
}

// SHA-1 hex of an ASCII string (crypto.subtle needs a secure context; the page is plain http).
function sha1Hex(str) {
  const b = Array.from(str, c => c.charCodeAt(0) & 0xff), ml = b.length * 8;
  b.push(0x80); while (b.length % 64 !== 56) b.push(0);
  for (let i = 7; i >= 0; i--) b.push(i >= 4 ? 0 : (ml >>> (i * 8)) & 0xff);
  let h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476, h4 = 0xc3d2e1f0;
  const w = new Array(80), rol = (x, n) => (x << n) | (x >>> (32 - n));
  for (let o = 0; o < b.length; o += 64) {
    for (let i = 0; i < 16; i++) w[i] = (b[o + 4 * i] << 24) | (b[o + 4 * i + 1] << 16) | (b[o + 4 * i + 2] << 8) | b[o + 4 * i + 3];
    for (let i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    let a = h0, bb = h1, c = h2, d = h3, e = h4;
    for (let i = 0; i < 80; i++) {
      const [f, k] = i < 20 ? [(bb & c) | (~bb & d), 0x5a827999] : i < 40 ? [bb ^ c ^ d, 0x6ed9eba1]
        : i < 60 ? [(bb & c) | (bb & d) | (c & d), 0x8f1bbcdc] : [bb ^ c ^ d, 0xca62c1d6];
      const t = (rol(a, 5) + f + e + k + w[i]) | 0;
      e = d; d = c; c = rol(bb, 30); bb = a; a = t;
    }
    h0 = (h0 + a) | 0; h1 = (h1 + bb) | 0; h2 = (h2 + c) | 0; h3 = (h3 + d) | 0; h4 = (h4 + e) | 0;
  }
  return [h0, h1, h2, h3, h4].map(x => (x >>> 0).toString(16).padStart(8, '0')).join('');
}

// The main rig's skeleton signature, in export_web_vincent.py's parent_sha1 form:
// sha1 of the parent list as compact JSON ([-1,0,1,...]). null if vincent.json has none.
async function rigSignature() {
  const r = await fetch('data/vincent/vincent.json');
  if (!r.ok) throw new Error(`vincent.json: HTTP ${r.status}`);
  const parent = (await r.json()).joints?.parent;
  if (!Array.isArray(parent)) return null;
  return { bone_count: parent.length, parent_sha1: sha1Hex(JSON.stringify(parent.map(x => Math.trunc(x)))) };
}

// Why a pack can't be used with this rig, or null. A pack built for another skeleton
// would bind its tracks by joint name onto the wrong bones, so a mismatch skips it.
// A pack without the fields (exported before they existed) or a rig we couldn't read
// can't be checked: those load with a warning, as before the check.
function packMismatch(name, side, rig) {
  if (!rig) { console.warn(`clip pack ${name}: main rig signature unavailable, loaded unchecked`); return null; }
  if (side.bone_count == null || side.parent_sha1 == null) {
    console.warn(`clip pack ${name}: no bone_count/parent_sha1 (old export), loaded unchecked`); return null;
  }
  if (side.bone_count !== rig.bone_count) return `bone_count ${side.bone_count} != rig ${rig.bone_count}`;
  if (side.parent_sha1 !== rig.parent_sha1) return `parent_sha1 ${side.parent_sha1} != rig ${rig.parent_sha1}`;
  return null;
}

// Animation-only clip packs (export_web_vincent.py --pack). Their tracks use the same
// joint node names as vincent.glb, so the main mixer binds them directly; each pack's
// bone_count / parent_sha1 must match vincent.json's skeleton (packMismatch).
async function loadPacks(names, anim, motions) {
  const loader = new GLTFLoader();
  const got = [];
  const rig = await rigSignature().catch(e => { console.warn('rig signature:', e.message); return null; });
  await Promise.all(names.map(async n => {
    try {
      const r = await fetch(`data/vincent/vincent_${n}.json`);
      if (!r.ok) throw new Error(`vincent_${n}.json: HTTP ${r.status}`);
      const side = await r.json();
      const bad = packMismatch(n, side, rig);
      if (bad) { console.warn(`clip pack ${n} skipped (built for another skeleton): ${bad}`); return; }
      const g = await loader.loadAsync(`data/vincent/vincent_${n}.glb`);
      anim.addClips(g.animations);
      Object.assign(motions, side.clips || {});
      got.push(`${n} (${g.animations.length})`);
    } catch (e) {
      console.warn(`clip pack ${n} not loaded:`, e.message);
    }
  }));
  return got;
}

// body.rotation.y for a heading h (dir = (sin h, cos h)): forward in body space is
// (0,0,-1); Ry(t) sends it to (-sin t, 0, -cos t), so t = h + pi.
const bodyYaw = h => h + Math.PI;

async function loadCol() {
  if (params.get('col') !== 'stub') {
    try {
      const m = await import('./collision.js');
      const col = await m.loadCollision('data/z128_collision.bin');
      return { col, name: 'collision.js' };
    } catch (e) {
      console.warn('collision.js unavailable, using the stub:', e.message);
    }
  }
  const m = await import('./collision_stub.js');
  return { col: await m.loadCollision(), name: 'stub (flat y=-98.44)' };
}

// ------------------------------------------------------------ main
async function main() {
  const [{ scene, root, lo, hi, floats }, { col, name: colName }] = await Promise.all([loadRoomPlay(), loadCol()]);
  const vin = await loadVincent(scene.char.s || 1);
  msg.style.display = 'none';
  root.add(vin.body);

  const renderer = new THREE.WebGLRenderer({ antialias: params.get('aa') === '1', alpha: false });
  renderer.setPixelRatio(params.has('dpr') ? +params.get('dpr') : Math.min(devicePixelRatio, 2));
  renderer.setSize(innerWidth, innerHeight);
  renderer.sortObjects = false;                    // keep the game's draw order (Vincent added last)
  renderer.outputColorSpace = THREE.LinearSRGBColorSpace;
  renderer.setClearColor(new THREE.Color().setRGB(...scene.clear, THREE.LinearSRGBColorSpace), 1);
  document.body.appendChild(renderer.domElement);
  const canvas = renderer.domElement;

  const c0 = new THREE.Vector3(...scene.camera.eye);
  const far = Math.max(c0.distanceTo(lo.clone().add(hi).multiplyScalar(.5)) + lo.distanceTo(hi) + 1, 1);
  const camera = new THREE.PerspectiveCamera(scene.camera.fov, innerWidth / innerHeight, 1, far);

  // spawn: feet on the first floor at or below the scene's placement height
  const s = scene.char.s || 1;
  let gy = col.groundY(SPAWN.x, SPAWN.z, scene.char.y);
  if (gy == null) gy = col.groundY(SPAWN.x, SPAWN.z, scene.char.y - 40);
  if (gy == null) { console.warn('no ground at spawn'); gy = scene.char.y; }
  // start facing along the hero camera's view, with the camera behind him
  const heroDir = new THREE.Vector3(...scene.camera.at).sub(c0);
  const heading0 = Math.atan2(heroDir.x, heroDir.z);

  // feet speeds in world units/tick: the clip's root motion at the body scale, so
  // full walk/run plays at rate 1 with no foot slide
  const rootSpeed = { walk: ROOT_PER_FRAME.walk * s, run: ROOT_PER_FRAME.run * s };
  const ctrl = new PlayerController(col, {
    pos: { x: SPAWN.x, y: gy, z: SPAWN.z }, heading: heading0,
    walkSpeed: rootSpeed.walk, runSpeed: rootSpeed.run, radius: 3.5, height: 19, scale: s,
  });
  const anim = new SpeedAnimator(vin.mixRoot, vin.clips, rootSpeed, { fade: 0.2 });
  anim.mixer.update(0);                            // h_st frame 0
  const sole = measureSole(vin, root);
  FEET_OFFSET = Number.isFinite(sole) && Math.abs(sole) < 40 ? -sole : 0;
  if (!Number.isFinite(sole) || Math.abs(sole) >= 40) console.warn('sole measurement failed:', sole);
  const follow = new FollowCamera(camera, col, { dist: 52, pitch: 0.3, targetHeight: 15 });
  follow.yaw = heading0;

  // ---- actions (player.js header / ACTIONS.md)
  const motions = {};
  loadMotions('vincent').then(m => Object.assign(motions, m)).catch(e => console.warn(e.message));
  ctrl.attach({ anim, camera: follow, motions });
  let actionHelp = [];
  if (params.get('actions') !== '0') {
    for (const a of await loadActions()) { ctrl.addAction(a.action); if (a.help) actionHelp.push(a.help); }
  }
  let packsLoaded = [];
  const packsReady = loadPacks(PACKS, anim, motions).then(g => { packsLoaded = g; return g; });

  // detached debug camera (?cam=), same format as main.js
  let detached = null;
  function setCam(str) {
    const v = String(str).split(',').map(Number);
    if (v.length !== 5 || !v.every(Number.isFinite)) return false;
    const y = v[3] * Math.PI / 180, p = v[4] * Math.PI / 180;
    const dir = new THREE.Vector3(Math.sin(y) * Math.cos(p), -Math.sin(p), Math.cos(y) * Math.cos(p));
    detached = { pos: new THREE.Vector3(v[0], v[1], v[2]), dir };
    return true;
  }
  if (params.has('cam')) setCam(params.get('cam'));

  // ---- input
  const keys = new Set();
  const pressed = new Set();                       // key-down edges not yet seen by a tick
  let frozen = false;                              // report mode: no input, no simulation
  const typing = e => /^(INPUT|TEXTAREA)$/.test(e.target.tagName);
  addEventListener('keydown', e => {
    if (typing(e) || frozen) return;
    keys.add(e.code);
    if (!e.repeat) pressed.add(e.code);
    if (e.code === 'KeyH') { ctrl.reset(); follow.yaw = heading0; follow.pitch = 0.3; detached = null; }
    if (e.code === 'KeyR') { e.preventDefault(); report.open(); }
    if (e.code === 'Space' || e.code.startsWith('Arrow')) e.preventDefault();
  });
  addEventListener('keyup', e => keys.delete(e.code));
  addEventListener('blur', () => keys.clear());
  canvas.addEventListener('click', () => { try { canvas.requestPointerLock?.(); } catch (_) { /* drag fallback */ } });
  // Chrome can report one huge movementX/Y right after locking: skip that first
  // event and clamp spikes so the camera doesn't spin.
  let skipMove = false;
  document.addEventListener('pointerlockchange', () => { skipMove = true; });
  const clampMv = v => Math.max(-150, Math.min(150, v));
  addEventListener('mousemove', e => {
    if (document.pointerLockElement !== canvas) return;
    if (skipMove) { skipMove = false; return; }
    if (!frozen) follow.look(clampMv(e.movementX), clampMv(e.movementY));
  });
  let drag = null;
  canvas.addEventListener('pointerdown', e => { if (!document.pointerLockElement) drag = [e.clientX, e.clientY]; });
  addEventListener('pointerup', () => { drag = null; });
  addEventListener('pointermove', e => {
    if (!drag || document.pointerLockElement || frozen) return;
    follow.look(e.clientX - drag[0], e.clientY - drag[1]); drag = [e.clientX, e.clientY];
  });
  addEventListener('resize', () => {
    camera.aspect = innerWidth / innerHeight; camera.updateProjectionMatrix();
    renderer.setSize(innerWidth, innerHeight);
  });

  function readInput() {
    const { f, r } = follow.basis();
    let x = 0, z = 0;
    if (keys.has('KeyW') || keys.has('ArrowUp')) { x += f.x; z += f.z; }
    if (keys.has('KeyS') || keys.has('ArrowDown')) { x -= f.x; z -= f.z; }
    if (keys.has('KeyD') || keys.has('ArrowRight')) { x += r.x; z += r.z; }
    if (keys.has('KeyA') || keys.has('ArrowLeft')) { x -= r.x; z -= r.z; }
    return { x, z, run: keys.has('ShiftLeft') || keys.has('ShiftRight'), keys, pressed };
  }

  // ---- Report button (report.js, shared with the main viewer)
  const toastEl = document.getElementById('toast');
  let toastT = 0;
  function toast(text, ms = 3000) {
    toastEl.textContent = text; toastEl.style.display = 'block';
    clearTimeout(toastT); toastT = setTimeout(() => { toastEl.style.display = 'none'; }, ms);
  }
  const r4 = v => +v.toFixed(4);
  // camera in main.js's ?cam= convention: dir = (sin y cos p, -sin p, cos y cos p), degrees
  function camView() {
    const d = camera.getWorldDirection(new THREE.Vector3());
    const yaw = Math.atan2(d.x, d.z) * 180 / Math.PI, pitch = Math.asin(Math.max(-1, Math.min(1, -d.y))) * 180 / Math.PI;
    const cam = [...camera.position.toArray().map(v => v.toFixed(2)), yaw.toFixed(3), pitch.toFixed(3)].join(',');
    const link = new URL(location.href);
    link.search = ''; link.searchParams.set('cam', cam);
    return { eye: camera.position.toArray().map(r4), dir: d.toArray().map(r4), up: camera.up.toArray().map(r4),
             yaw: +yaw.toFixed(3), pitch: +pitch.toFixed(3), fov: camera.fov, aspect: +camera.aspect.toFixed(5),
             near: r4(camera.near), far: +camera.far.toFixed(2), cam,
             link: link.toString().replace(/%2C/gi, ','), detached: !!detached };
  }
  function playerState() {
    const p = ctrl.pos;
    const g = col.groundHit ? col.groundHit(p.x, p.z, p.y) : null;
    return {
      feet: [p.x, p.y, p.z].map(r4), heading_deg: +(ctrl.heading * 180 / Math.PI).toFixed(2),
      onGround: ctrl.onGround, vy: r4(ctrl.vy || 0), hitWall: ctrl.hitWall,
      speed_per_tick: r4(ctrl.speed), clip: anim.override ? 'action' : anim.current,
      clipName: anim.clipName(), rate: r4(anim.rate),
      clipTime: r4(anim.time()), feetOffset: FEET_OFFSET,
      action: ctrl.active ? ctrl.active.name : null, packs: packsLoaded,
      ground: g && { y: r4(g.y), feetError: r4(g.y - p.y), tri: g.tri, subtree: g.subtree,
                     mask: g.mask === undefined ? null : '0x' + g.mask.toString(16).padStart(4, '0'),
                     normal: g.normal.map(r4), surface: g.surface },
      collision: colName, spawn: [ctrl.spawn.x, ctrl.spawn.y, ctrl.spawn.z].map(r4),
    };
  }
  const report = initReport({
    renderer, camera, root, scene, attrs: floats, toast, view: camView,
    setFrozen(f) { frozen = f; keys.clear(); pressed.clear(); drag = null; if (!f) { clock.getDelta(); acc = 0; } },
    extraMeta: () => ({ page: 'play', player: playerState() }),
  });
  document.getElementById('btn-report').addEventListener('click', e => { e.stopPropagation(); report.open(); });

  // ---- loop: fixed 60 Hz sim, interpolated render
  const clock = new THREE.Clock();
  let acc = 0, frames = 0, fpsAcc = 0, fps = 0;
  const stats = { ticks: 0, groundErrMax: 0, colName };
  function frame() {
    requestAnimationFrame(frame);
    const dt = frozen ? 0 : Math.min(clock.getDelta(), 0.25);
    acc += dt;
    let n = 0;
    while (acc >= TICK && n < 8) {
      ctrl.step(readInput());
      pressed.clear();                             // an edge is seen by one tick only
      // feet vs ground check (for the HUD / tests)
      const g = col.groundY(ctrl.pos.x, ctrl.pos.z, ctrl.pos.y);
      if (g != null) stats.groundErrMax = Math.max(stats.groundErrMax, Math.abs(g - ctrl.pos.y));
      stats.lastGroundErr = g == null ? null : g - ctrl.pos.y;
      acc -= TICK; n++; stats.ticks++;
    }
    if (n === 8) acc = 0;                          // fell far behind (tab hidden): drop time
    const pose = ctrl.lerp(acc / TICK);
    vin.body.position.set(pose.x, pose.y + FEET_OFFSET, pose.z);
    vin.body.rotation.y = bodyYaw(pose.heading);
    anim.update(ctrl.speed, dt);

    if (detached) {
      camera.position.copy(detached.pos); camera.up.set(0, -1, 0);
      camera.lookAt(detached.pos.clone().add(detached.dir));
    } else {
      follow.update(pose, dt);
    }
    renderer.render(root, camera);

    frames++; fpsAcc += dt;
    if (fpsAcc >= 0.5) { fps = frames / fpsAcc; frames = 0; fpsAcc = 0; }
    const p = ctrl.pos;
    hud.textContent = `${fps.toFixed(0)} fps   collision: ${colName}\n` +
      `pos  ${p.x.toFixed(2)}, ${p.y.toFixed(2)}, ${p.z.toFixed(2)}\n` +
      `clip ${anim.override ? 'action' : anim.current} (${anim.clipName()}) x${anim.rate.toFixed(2)}` +
      `${ctrl.active ? '   action ' + ctrl.active.name : ''}` +
      `   speed ${(ctrl.speed * 60).toFixed(1)} u/s\n` +
      `onGround ${ctrl.onGround}${ctrl.hitWall ? '  WALL' : ''}${follow.blocked ? '  cam pulled in' : ''}\n` +
      `click: mouse look (drag works too)   WASD move   Shift run   ${actionHelp.join('   ')}   H respawn   R report`;
  }
  window.__play = { ctrl, anim, follow, col, stats, camera, renderer, root, vin, spawnY: gy, rootSpeed,
                    playerState, camView, report, keys, pressed, motions, packsReady, actionHelp };
  requestAnimationFrame(frame);
}

main().catch(e => { msg.textContent = 'error: ' + e.message; console.error(e); });
