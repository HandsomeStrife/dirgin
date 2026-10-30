// Dirge z128, playable: Vincent (data/vincent/vincent.glb) walking the room with
// third-person controls, the game's follow camera (player.js, FUN_0032ace0) and collision (collision.js,
// or the stub). Gameplay is the game's locomotion (player.js) and the ladder action;
// invented behaviour was removed by worker 0078 (notes/fidelity-audit.md).
//
// The room comes from room.js (main.js's loader and shader, with its z-fight fixes).
// Game space, -Y up. URL params: hud=0, aa=1, dpr=N, col=stub (force the stub),
// cam=ex,ey,ez,yaw,pitch (detached camera at that view; H re-attaches it),
// actions=0 (no action modules), packs=a,b (clip packs to load; default ladder, jump, shoot,
// packs= loads none), zprio=0 / zunder=0 (room.js depth-tie offsets off), cloth=0 (cape
// cloth off: the keyed glb cape), gun=0 (no gun mesh), ladder=N (test aid: start at
// ladder N's bottom), outblur=1/0 (PS2 output merge on/off, output.js; default: on only with res=native), res=native (the game's
// 432x512 framebuffer shown 4:3; default = canvas-resolution render filling the canvas, Dan's choice), aim=0 (no
// weapon: aim.js / aim_core.js, the camera's weapon views and the reticle). Packs load in
// the background after start (vincent_<pack>.glb).
import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { TICK, STEP, PlayerController, SpeedAnimator, FollowCamera, cameraRays, KEY_WALK, KEY_RUN, MouseFeed, MOUSE_SENS_DEFAULT } from './player.js';
import { attachGun } from './gun.js';
import { loadActions } from './actions/index.js';
import { initReport } from './report.js';
import { loadRoom } from './room.js';
import { CapeCloth } from './cloth.js';
import { createOutputStage } from './output.js';
import { setupAim } from './aim.js';
import { setupEnemies } from './enemies.js';
import { createEnemyAI } from './enemy_ai.js';

const params = new URLSearchParams(location.search);
const hud = document.getElementById('hud');
const msg = document.getElementById('msg');
if (params.get('hud') === '0') hud.style.display = 'none';

const SPAWN = { x: -1395.63, z: -1110.51 };
const ENEMIES = params.get('enemies') !== '0';       // enemies.js: z128's e027 soldiers (the AI's, and stage-load ones alive in the save)
const ENEMY_AI = params.get('enemyai') !== '0';      // enemy_ai.js: wake trigger + awake soldier AI (?enemyai=0 = off)
// OPTION mouselook: the camera turns by mouse pixels (not in the game). ON by default at Dan's request
// (review board 2026-10-30); ?mouselook=game gives the game's USB-mouse path (FUN_00254ac8)
const MOUSE_DIRECT = params.get('mouselook') !== 'game';
const MOUSE_SENS = params.has('mousesens') && +params.get('mousesens') > 0 ? +params.get('mousesens') : MOUSE_SENS_DEFAULT;   // degrees per pixel
const ENEMY_DMG = params.get('enemydmg') === '1';    // enemy bullets hit Vincent (off by default: the hit test is a GUESS)
const AIM = params.get('aim') !== '0';               // aim.js (aim_core.js): the weapon, its camera views, the reticle
const PACKS = params.has('packs') ? params.get('packs').split(',').filter(Boolean) : (AIM ? ['ladder', 'jump', 'shoot'] : ['ladder', 'jump']);
// Placement: the game draws the model with the motion player's matrix MP+0x10 (FUN_005ab8f0
// -> view x MP+0x10), a copy of the character matrix (FUN_00343de8 -> FUN_005a6f40 ->
// FUN_005a6ea8). Its rows are unit length (save state) and its origin is the character
// position, which is at his feet: the game's cape_target is that matrix x the skinned bones
// at ratio 1.000 (worker 0083, tools/trace TRACE_FIELDSET=pose). So no scale, no pivot and
// no sole offset: the glb's model origin goes on ctrl.pos (export_web_vincent.py rootT = 0).

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

async function loadVincent() {
  msg.textContent = 'loading vincent…';
  const gltf = await new GLTFLoader().loadAsync('data/vincent/vincent.glb');
  const prims = [];
  gltf.scene.traverse(o => {
    if (!o.isMesh) return;
    o.frustumCulled = false;                       // skinned: the bind-pose bounds lie
    prims.push(o);
    const m = o.material;
    if (m.map) { m.map.colorSpace = THREE.NoColorSpace; m.map.needsUpdate = true; }  // raw texel, like the room
    // Back-face cull. The game sends every Vincent triangle whose screen winding is
    // counter-clockwise (y down, strip parity applied) as XYZ3 with ADC=1, so it is
    // not drawn. Hero dump 163235: 1299 of 1303 ADC triangles are ccw, 1195 of 1198
    // drawn ones are cw (body, hair, cape). The glb keeps both sides of the two-sided
    // cards as separate triangles. The ones the game draws are clockwise on screen
    // in glb index order: WebGL back faces. Drawn sets match the dump: hair alpha
    // 135/135, hair opaque 84/81, cape 56/56, body 932/926.
    m.side = THREE.BackSide;
  });
  // The glb shares one material per kind; the prims that stay prelit (the cape, extras
  // lit = false) get their own copy so the program-D patch (setupCharLighting) skips them.
  for (const o of prims) if (o.geometry.userData?.lit === false) o.material = o.material.clone();
  const afail = gsAlphaPasses(prims);
  // gltf.scene is Y-up (root node 0 flips the game's -Y-up model). Flip back with
  // Rx(pi) so it sits in game space; glb +Z forward becomes game -Z.
  const flip = new THREE.Group();
  flip.rotation.x = Math.PI;
  flip.add(gltf.scene);
  const body = new THREE.Group();                  // MP+0x10: position = feet, rotation.y from heading, unit scale
  body.add(flip);
  const clip = n => {
    const c = gltf.animations.find(a => a.name === n);
    if (!c) throw new Error(`vincent.glb has no clip ${n}`);
    return c;
  };
  return { body, mixRoot: gltf.scene, clips: { idle: clip('h_st'), walk: clip('h_wk'), run: clip('h_rn') }, prims, afail };
}

// ------------------------------------------------------------ character lighting
// VU1 program D (port/vu/RENDER-MATH.md §1) per vertex, from the skinned normal:
//   rgb = min(vcol * (C0*max(L0.n,0) + C1*max(L1.n,0) + C2*max(L2.n,0) + A), 255), FTOI0
// then GS MODULATE (COLOR_0 = vcol/128, so the shader keeps /128). The rig is rebuilt
// once per drawn frame as FUN_005ab0b0 does (data/vincent/light.json, export_web_light.py):
//   L0/C0  env slot 0 light (toward the light), colour x MP+0x450/128
//   L1/C1  effect point lights (DAT_004eea50 / MP+0x160, FUN_0030e208): none on this page
//   L2/C2  room light from the BMD light grids (FUN_0026a710 -> FUN_00270d08), colour
//          rgb*k*100/d^2, smoothed by FUN_005ab608 while the chosen light changes
//   A      env slot 0 ambient x MP+0x450/128
// Not ported (inactive in z128: save state): the fixed rig behind DAT_004fcaf8 bit 36,
// the MP+0x41c / +0x420 ambient tints, the env-slot blend FUN_005ab738.
// Hero dump 163235 vs this model at h_st f91: vertices whose triangle matches the dump
// pose (<2 deg) are within 1 level at the median (body, opaque and alpha hair).
// The cape (cloth meshes, not skinned) takes the same program D with the cloth buffer's
// normals (cloth.js skinNormals, FUN_005b06d0), written per render in its parent's space.
const LIGHT_VS = `
  #if ( defined( USE_SKINNING ) || defined( PROGRAM_D_CLOTH ) ) && defined( USE_COLOR_ALPHA )
  {
    #ifdef PROGRAM_D_CLOTH
    vec3 n = mat3( modelMatrix ) * normal;                // cape: cloth normal (MeshBasic has no objectNormal unskinned)
    #else
    vec3 n = mat3( modelMatrix ) * objectNormal;          // world (= game world) normal, unit-scale MP+0x10
    #endif
    vec3 lit = uC0 * max( dot( uL0, n ), 0.0 ) + uC1 * max( dot( uL1, n ), 0.0 )
             + uC2 * max( dot( uL2, n ), 0.0 ) + uAmb;     // MULA/MADDA L, MAX 0, MULA/MADD C + A
    vColor.rgb = floor( min( vColor.rgb * 128.0 * lit, 255.0 ) ) / 128.0;   // MUL vcol, MINIi 255, FTOI0
  }
  #endif`;
function litPatch(m, U, cloth = false) {
  const prev = m.onBeforeCompile, prevKey = m.customProgramCacheKey;
  m.onBeforeCompile = (sh, r) => {
    prev.call(m, sh, r);
    Object.assign(sh.uniforms, U);
    sh.vertexShader = (cloth ? '#define PROGRAM_D_CLOTH\n' : '') + 'uniform vec3 uL0, uC0, uL1, uC1, uL2, uC2, uAmb;\n' +
      sh.vertexShader.replace('#include <fog_vertex>', '#include <fog_vertex>' + LIGHT_VS);
  };
  m.customProgramCacheKey = () => prevKey.call(m) + (cloth ? '|programD-cloth' : '|programD');
  m.needsUpdate = true;
}

async function setupCharLighting(vin, cape) {
  let L;
  try {
    const r = await fetch('data/vincent/light.json');
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    L = await r.json();
  } catch (e) {
    console.warn('character lighting off (data/vincent/light.json):', e.message);
    return null;
  }
  const lit = [...vin.prims, ...vin.afail].filter(o => o.geometry.userData?.lit === true);
  if (!lit.length) { console.warn('character lighting off: vincent.glb has no extras.lit (old export)'); return null; }
  const V = (...a) => new THREE.Vector3(...a);
  const s = L.scale_byte / 128;                                // MP+0x450 * 0.0078125
  const U = {
    uL0: { value: V(...L.env.dir0) }, uC0: { value: V(...L.env.col0).multiplyScalar(s) },
    uL1: { value: V() }, uC1: { value: V() },
    uL2: { value: V() }, uC2: { value: V() },
    uAmb: { value: V(...L.env.ambient).multiplyScalar(s) },
  };
  for (const m of new Set(lit.map(o => o.material))) litPatch(m, U);
  // the cape: lit only on the cloth meshes, which carry the game's normals (with ?cloth=0 the
  // glb cape keeps its geometric normals, not the game's, and stays prelit)
  if (cape) for (const m of cape.materials) litPatch(m, U, true);

  // FUN_0026a710: DAT_004e88a4 = 1e6, then every loaded BMD's grid (FUN_00270d08); the
  // threshold carries over, so the result is the global least d^2/k.
  function pick(p) {
    let best = L.pick_start, hit = null;
    for (const g of L.grids) {
      let ix = Math.trunc((p.x - g.x0) / g.size), iz = Math.trunc((p.z - g.z0) / g.size);   // FUN_0026fb90
      ix = Math.min(Math.max(ix, 0), g.nx - 1); iz = Math.min(Math.max(iz, 0), g.nz - 1);
      for (const li of g.cells[iz * g.nx + ix]) {
        const r = g.lights[li];
        const dx = p.x - r.pos[0], dy = p.y - r.pos[1], dz = p.z - r.pos[2];
        const q = (dx * dx + dy * dy + dz * dz) / r.k;
        if (!(q < best)) continue;
        if (r.type === 2) {                                    // spot: outside the cone -> skip
          const d = Math.hypot(dx, dy, dz);
          if ((dx * r.spot[0] + dy * r.spot[1] + dz * r.spot[2]) / d < r.spot[3]) continue;
        }
        best = q; hit = r;
      }
    }
    return hit;
  }
  // FUN_005ab608 state: MP+0x110 dir, MP+0x140 colour, MP+0x430 the settled light
  const s110 = V(), s140 = V();
  let s430 = undefined;                                        // undefined: not run yet
  const dir = V(), col = V(), dd = V(), dc = V();
  // p = the actor point, MP+0x10 x (MP+0x90 + MP+0xa0)/2 (FUN_005a72a8, FUN_0026138c)
  function frame(p) {
    const r = pick(p);
    dir.set(0, 0, 0); col.set(0, 0, 0);
    if (r) {
      dir.set(r.pos[0] - p.x, r.pos[1] - p.y, r.pos[2] - p.z);
      const d = dir.length();
      if (d !== 0) { dir.multiplyScalar(1 / d); col.set(...r.rgb).multiplyScalar(r.k * 100 / (d * d)); }
      else dir.set(0, 0, 0);
    }
    const id = r || 0;
    if (s430 === undefined) {
      // GUESS: start settled on the first light (the actor's initial MP+0x110/0x140/0x430
      // are not decoded; see notes/fidelity-audit.md "Character lighting")
      s430 = id; s110.copy(dir); s140.copy(col);
    } else if (id !== s430) {
      dc.subVectors(col, s140); dd.subVectors(dir, s110);
      if (dc.lengthSq() > 0.010000001 || dd.lengthSq() > 0.010000001) {
        s140.addScaledVector(dc, 0.1); col.copy(s140);
        s110.addScaledVector(dd, 0.2);
        const l = s110.length(); if (l > 0) s110.multiplyScalar(1 / l);
        dir.copy(s110);
      } else s430 = id;
    }
    U.uL2.value.copy(dir); U.uC2.value.copy(col);              // light 2: not x MP+0x450
  }
  const centre = V(...L.actor_center);
  return { U, frame, centre, pick, env: L.env };               // env: unscaled, for enemies.js
}

// ------------------------------------------------------------ cape cloth (cloth.js)
// The game simulates the cape's 82 cloth vertices (not bones): one cloth step per drawn
// frame, 50/s on PAL (FUN_005ab8f0 -> FUN_005b2ef8 -> FUN_005b2550 -> FUN_005b0d30), on
// targets skinned with all of each vertex's influences (FUN_005b06d0; the glb keeps 4).
// The cape primitive (and its GS alpha-fail twin) is swapped for plain meshes whose
// corners are written from the cloth buffer. Outside a settle window the cloth output IS
// the keyed target, so the cape looks as before except for the 5th influences.
//
// Timing: one cloth.frame() per game frame, from the SpeedAnimator's draw hook (player.js
// gameFrame: after the clip choice, before the clip advance FUN_005aa5b8), in the order
// FUN_005ab8f0 runs it: a clip request arms the window (FUN_005a95f0), the cloth step
// (FUN_005b2ef8, line 143), the countdown (FUN_005aa5b8, line 218), the turn re-arm
// (FUN_005a6f40). It is skinned from the game's draw pose: the character matrix after this
// frame's movement and the clip frames before this frame's advance (pose(), from main).
async function setupCapeCloth(vin, anim, pose) {
  let data;
  try {
    const r = await fetch('data/vincent/cloth.json');
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    data = await r.json();
  } catch (e) {
    console.warn('cape cloth off (data/vincent/cloth.json):', e.message);
    return null;
  }
  const src = vin.prims.find(o => o.isSkinnedMesh && o.geometry.userData?.firstTri === data.cape_first_tri);
  if (!src || !src.geometry.index || src.geometry.index.count !== data.corner_vertex.length) {
    console.warn('cape cloth off: cape primitive not found or corner count differs');
    return null;
  }
  const twin = vin.afail.find(o => o.geometry === src.geometry);
  // unindexed copy of the cape corners; position is written from the cloth each render
  const idx = src.geometry.index.array, n = idx.length;
  const geom = new THREE.BufferGeometry();
  for (const [name, a] of Object.entries(src.geometry.attributes)) {
    if (name === 'position' || name === 'skinIndex' || name === 'skinWeight') continue;
    const out = new Float32Array(n * a.itemSize);
    for (let i = 0; i < n; i++) for (let k = 0; k < a.itemSize; k++) out[i * a.itemSize + k] = a.getComponent(idx[i], k);
    geom.setAttribute(name, new THREE.BufferAttribute(out, a.itemSize, a.normalized));
  }
  const posAttr = new THREE.BufferAttribute(new Float32Array(n * 3), 3);
  posAttr.setUsage(THREE.DynamicDrawUsage);
  geom.setAttribute('position', posAttr);
  // lighting normals: the cloth buffer's (FUN_005b06d0 -> FUN_005b1260), replacing the glb's
  // geometric ones; written with the positions each render
  const nrmAttr = data.skin[0]?.length >= 12 ? new THREE.BufferAttribute(new Float32Array(n * 3), 3) : null;
  if (nrmAttr) { nrmAttr.setUsage(THREE.DynamicDrawUsage); geom.setAttribute('normal', nrmAttr); }
  else console.warn('cape lighting off: cloth.json has no skin normals (old export)');
  const parent = src.parent;
  const materials = [];
  for (const o of [src, twin].filter(Boolean)) {      // same scene-graph slot = same draw order
    const m = new THREE.Mesh(geom, o.material);
    materials.push(o.material);
    m.name = o.name + '#cloth'; m.frustumCulled = false; m.userData = { ...o.userData, cloth: true };
    const p = o.parent, i = p.children.indexOf(o);
    p.children[i] = m; m.parent = p; o.parent = null;
  }
  const cloth = new CapeCloth(data);
  const sk = src.skeleton, bind = src.bindMatrix;
  const mats = new Map(), tmp = new THREE.Matrix4(), inv = new THREE.Matrix4();
  const bones = [...new Set(data.skin.map(r => r[1]))];
  const skinMat = b => mats.get(b);
  const fx = new THREE.Vector3();
  const N3 = cloth.N * 3;
  const off = new Float32Array(N3);                   // cloth pos - target at the last game frame, parent space
  const tgt = new Float32Array(N3);
  const rskin = { N: cloth.N, skinRec: cloth.skinRec, target: new Float32Array(N3) };   // render targets (world)
  const rnrm = new Float32Array(N3), nrm = new Float32Array(N3);                        // render normals (world, parent)
  let lastKey = undefined;
  function skinNow() {                                // targets at the pose the scene graph holds now
    vin.body.updateMatrixWorld(true);
    for (const b of bones) mats.set(b, tmp.multiplyMatrices(sk.bones[b].matrixWorld, sk.boneInverses[b]).multiply(bind).toArray());
  }
  // world -> the cape meshes' parent space (o += M x)
  function toParent(src3, dst, sub = null) {
    inv.copy(parent.matrixWorld).invert();
    const e = inv.elements;
    for (let j = 0; j < N3; j += 3) {
      const x = src3[j], y = src3[j + 1], z = src3[j + 2];
      dst[j] = e[0] * x + e[4] * y + e[8] * z + e[12];
      dst[j + 1] = e[1] * x + e[5] * y + e[9] * z + e[13];
      dst[j + 2] = e[2] * x + e[6] * y + e[10] * z + e[14];
      if (sub) { dst[j] -= sub[j]; dst[j + 1] -= sub[j + 1]; dst[j + 2] -= sub[j + 2]; }
    }
  }
  // the clip that is playing now, as an identity: a new play() / gait change is a new clip
  // request (FUN_005a91c8 -> FUN_005a95f0); a loop keeps the key (no re-arm)
  const clipKey = () => anim.override || anim.current;
  // one game frame (anim.drawHook)
  function gameFrame() {
    pose();
    skinNow();
    const key = clipKey();
    const started = key !== lastKey ? anim.clipName() : null;
    lastKey = key;
    // GUESS: (traced, not decoded; see notes/fidelity-audit.md) character x axis for the turn re-arm (FUN_005a6f40): THIS frame's facing. Traced: the
    // game's window re-arms on the frame our current facing turns (cw_turn / cw_run arm
    // frames equal), although its skin uses the previous matrix (logicPose). Not decoded why.
    pose.facing(fx);
    cloth.frame({ clipStarted: started, skinMat, fx: [fx.x, fx.y, fx.z] });
    toParent(cloth.target, tgt);
    toParent(cloth.pos, off, tgt);
  }
  anim.drawHook = gameFrame;
  return {
    cloth,
    // Draw: the cloth's offset from its target at the last game frame, added to the target
    // at the pose being rendered (the render interpolates between game frames, ctrl.lerp),
    // so the pinned edge stays on the body. Rendered at a game frame's own pose this is
    // cloth.pos exactly. GUESS: display interpolation, a port convenience (see
    // notes/fidelity-audit.md "Web frame loop and cape cloth").
    render() {                                      // before the first game frame: off = 0, keyed
      skinNow();
      cloth.skin.call(rskin, skinMat);                // FUN_005b06d0 into a render-only buffer
      const out = posAttr.array, cv = data.corner_vertex;
      toParent(rskin.target, tgt);
      for (let i = 0; i < n; i++) {
        const j = 3 * cv[i];
        out[3 * i] = tgt[j] + off[j]; out[3 * i + 1] = tgt[j + 1] + off[j + 1]; out[3 * i + 2] = tgt[j + 2] + off[j + 2];
      }
      posAttr.needsUpdate = true;
      // Normals at the rendered (interpolated) pose, not the game frame's pose: part of the
      // display-interpolation GUESS above (notes/fidelity-audit.md "Web frame loop and cape
      // cloth", row "Cape drawn between game frames"). At a game frame's own pose they are the game's.
      if (nrmAttr) {                                  // world -> parent (rotation)
        cloth.skinNormals.call(rskin, skinMat, rnrm);
        const e = inv.copy(parent.matrixWorld).invert().elements, o = nrmAttr.array;
        for (let j = 0; j < N3; j += 3) {
          const x = rnrm[j], y = rnrm[j + 1], z = rnrm[j + 2];
          nrm[j] = e[0] * x + e[4] * y + e[8] * z; nrm[j + 1] = e[1] * x + e[5] * y + e[9] * z; nrm[j + 2] = e[2] * x + e[6] * y + e[10] * z;
        }
        for (let i = 0; i < n; i++) {
          const j = 3 * cv[i];
          o[3 * i] = nrm[j]; o[3 * i + 1] = nrm[j + 1]; o[3 * i + 2] = nrm[j + 2];
        }
        nrmAttr.needsUpdate = true;
      }
    },
    materials: nrmAttr ? materials : [],
    state: () => ({ total: cloth.total, remaining: cloth.remaining, k: +cloth.k().toFixed(4) }),
  };
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

// The locomotion clips' local TRS (export_web_vincent.py local_pose): SpeedAnimator then
// builds the pose the game's way (FUN_005a9db0). Without it (an old export) the three.js
// mixer's model-space mix is used, with a warning.
async function loadLocalPose(anim, root) {
  let side;
  try {
    const r0 = await fetch('data/vincent/vincent.json');
    if (!r0.ok) throw new Error(`HTTP ${r0.status}`);
    side = await r0.json();
  } catch (e) { console.warn(`vincent.json: ${e.message}: mixer pose`); return; }
  const lp = side.local_pose;
  if (!lp) { console.warn('vincent.json has no local_pose (old export): mixer pose'); return; }
  const r = await fetch('data/vincent/' + lp.file);
  if (!r.ok) { console.warn(`${lp.file}: HTTP ${r.status}: mixer pose`); return; }
  anim.setLocalPose(lp, await r.arrayBuffer(), root);
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
      // the pack clips' local TRS (player.js addLocalPose), registered before the clips so a
      // pack clip never starts on the mixer pose; without it they use the mixer pose
      if (side.local_pose && anim.lp) {
        const rl = await fetch('data/vincent/' + side.local_pose.file);
        if (!rl.ok) console.warn(`${side.local_pose.file}: HTTP ${rl.status}: pack ${n} uses the mixer pose`);
        else {
          try { anim.addLocalPose(side.local_pose, await rl.arrayBuffer()); }
          catch (e) { console.warn(`clip pack ${n}: ${e.message}: mixer pose`); }
        }
      } else if (!side.local_pose) console.warn(`clip pack ${n}: no local_pose (old export): mixer pose`);
      anim.addClips(g.animations, side.clips);
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
      const r = await fetch('data/z128_collision.bin');
      if (!r.ok) throw new Error(`collision: HTTP ${r.status}`);
      const buf = await r.arrayBuffer();
      const col = await m.loadCollision(buf);
      return { col, name: 'collision.js', camRays: cameraRays(buf) };
    } catch (e) {
      console.warn('collision.js unavailable, using the stub:', e.message);
    }
  }
  const m = await import('./collision_stub.js');
  return { col: await m.loadCollision(), name: 'stub (flat y=-98.44)' };
}

// ------------------------------------------------------------ main
async function main() {
  const [{ scene, root, lo, hi, floats }, { col, name: colName, camRays }] = await Promise.all([loadRoomPlay(), loadCol()]);
  const vin = await loadVincent();
  msg.style.display = 'none';
  root.add(vin.body);

  const renderer = new THREE.WebGLRenderer({ antialias: params.get('aa') === '1', alpha: false });
  renderer.setPixelRatio(params.has('dpr') ? +params.get('dpr') : Math.min(devicePixelRatio, 2));
  renderer.setSize(innerWidth, innerHeight);
  renderer.sortObjects = false;                    // keep the game's draw order: scene-graph order (enemies.js: actors after Vincent)
  renderer.outputColorSpace = THREE.LinearSRGBColorSpace;
  renderer.setClearColor(new THREE.Color().setRGB(...scene.clear, THREE.LinearSRGBColorSpace), 1);
  document.body.appendChild(renderer.domElement);
  const canvas = renderer.domElement;
  // PS2 output (output.js): the game's 432x512 framebuffer, its projection (FUN_0040d478),
  // the PCRTC merge (FUN_0040d228) and the 4:3 display window, with ?res=native.
  // Default: the sharp canvas-resolution render filling the canvas (Dan's choice, 2026-10-28);
  // ?outblur=1 / 0 = merge on / off
  // Output merge: on in native mode (the game's look), off in the full-res default (sharp, Dan's
  // choice 2026-10-28); ?outblur=1 / 0 forces it.
  const nativeRes = params.get('res') === 'native';
  const output = createOutputStage(renderer, { enabled: params.has('outblur') ? params.get('outblur') !== '0' : nativeRes,
                                               msaa: params.get('aa') === '1', native: nativeRes });

  const c0 = new THREE.Vector3(...scene.camera.eye);
  const far = Math.max(c0.distanceTo(lo.clone().add(hi).multiplyScalar(.5)) + lo.distanceTo(hi) + 1, 1);
  const camera = new THREE.PerspectiveCamera(scene.camera.fov, innerWidth / innerHeight, 1, far);
  output.fitCamera(camera);                        // the game's projection (native) or the canvas aspect (full)

  // spawn: feet on the first floor at or below the scene's placement height
  let gy = col.groundY(SPAWN.x, SPAWN.z, scene.char.y);
  if (gy == null) gy = col.groundY(SPAWN.x, SPAWN.z, scene.char.y - 40);
  if (gy == null) { console.warn('no ground at spawn'); gy = scene.char.y; }
  // start facing along the hero camera's view, with the camera behind him
  const heroDir = new THREE.Vector3(...scene.camera.at).sub(c0);
  const heading0 = Math.atan2(heroDir.x, heroDir.z);

  const ctrl = new PlayerController(col, {
    pos: { x: SPAWN.x, y: gy, z: SPAWN.z }, heading: heading0, radius: 3.5, height: 19, scale: 1,   // MP+0x10 unit scale
  });
  const anim = new SpeedAnimator(vin.mixRoot, vin.clips);
  await loadLocalPose(anim, vin.mixRoot);
  anim.mixer.update(0);                            // h_st frame 0
  // the game's draw pose for the cloth (anim.drawHook, inside ctrl.step): MP+0x10 as the
  // draw sees it, i.e. the character matrix from BEFORE this frame's move (ctrl.prev /
  // prevHeading), and the clip frames before this frame's advance (alpha 1 = head.frame).
  // Traced, not decoded: per record the game's cape_target is the previous record's
  // character matrix x the skinned bones (0.000; this record's: 0.94 in run). Where
  // FUN_00343de8 runs relative to the move (FUN_0034c388) isn't decoded.
  const logicPose = () => {
    vin.body.position.set(ctrl.prev.x, ctrl.prev.y, ctrl.prev.z);
    vin.body.rotation.y = bodyYaw(ctrl.prevHeading);
    const a = ctrl.alpha; ctrl.alpha = 1; anim.update(ctrl.speed, 0); ctrl.alpha = a;
  };
  logicPose.facing = v => {                        // x axis of the body at this frame's heading
    const r = vin.body.rotation.y;
    vin.body.rotation.y = bodyYaw(ctrl.heading); vin.body.updateMatrixWorld(true);
    v.setFromMatrixColumn(vin.body.matrixWorld, 0).normalize();
    vin.body.rotation.y = r; vin.body.updateMatrixWorld(true);
  };
  const cape = params.get('cloth') === '0' ? null : await setupCapeCloth(vin, anim, logicPose);
  // character lighting (setupCharLighting): FUN_005ab0b0 runs in the draw, once per game
  // frame, with the draw's MP+0x10 (the logic pose: ctrl.prev, prevHeading), after the cloth
  const light = await setupCharLighting(vin, cape);
  if (light) {
    const ap = new THREE.Vector3(), Y = new THREE.Vector3(0, 1, 0);
    const actorPoint = () => ap.copy(light.centre).applyAxisAngle(Y, bodyYaw(ctrl.prevHeading)).add(ctrl.prev);
    light.frame(actorPoint());
    const prevHook = anim.drawHook;
    anim.drawHook = () => { if (prevHook) prevHook(); light.frame(actorPoint()); };
  }
  // enemy AI data (export_web_triggers.py). GUESS: records 44 / 45 (#66 / #67) are active from
  // the start, as in the sea-of-flames save (their activation by event cmd 0x108 is not on this page).
  const AWAKE = [44, 45];
  // ai.json also places them (section-1 records), so it loads with ?enemyai=0 too: the soldiers stay, the AI is off
  let aiData = ENEMIES ? await fetch('data/enemies/ai.json').then(r => r.ok ? r.json() : null).catch(() => null) : null;
  if (ENEMIES && !aiData) console.warn('enemy AI off: data/enemies/ai.json missing');
  // an old ai.json (no records 44 / 45 or no weapons) turns off the AI only, not the soldiers
  if (aiData && !(AWAKE.every(k => aiData.records?.[k]) && Array.isArray(aiData.weapons))) {
    console.warn('enemy AI off: data/enemies/ai.json is out of date (rebuild data/enemies)');
    aiData = null;
  }
  const enemies = ENEMIES ? await setupEnemies({ root, litPatch, light, extra: aiData ? AWAKE.map(k => aiData.records[k]) : [] })
    .catch(e => { console.warn('enemies off:', e.message); return null; }) : null;
  // Vincent's status for the enemy AI: HP 780 = P status +0x20 in the save state (k_enemy_engage8 p_hp)
  const php = { pos: [ctrl.pos.x, ctrl.pos.y, ctrl.pos.z], hp: 780, id: 0x16, f118: 0, s64: 0 };
  let enemyAI = null;
  try {
    if (enemies) enemies.mpHead = vin.body;            // newer actors draw before older ones (enemies.js add)
    enemyAI = ENEMY_AI && aiData && enemies ? createEnemyAI({ data: aiData, player: php, log: m => console.log(m), damage: ENEMY_DMG,
      // the save's dead records (41-43): FUN_00559748 refuses to activate them
      deadRecs: enemies.deadRecs,
      // cmd 0x108 / 0x109 / 0x130 on a section-1 record: its body appears / goes (enemies.js add / remove)
      onActivate: rec => { enemies.add(rec); },
      onDeactivate: rec => { enemies.remove(rec); },
      // GUESS: FUN_005449f0 probe 3 = the page's collision triangle set (probe sets not decoded)
      los: (a, b) => !!(col.segment && col.segment({ x: a[0], y: a[1], z: a[2] }, { x: b[0], y: b[1], z: b[2] })) }) : null;
    if (enemyAI) for (const k of AWAKE) enemyAI.addActor({ rec: k });
  } catch (e) { console.warn('enemy AI off:', e.message); enemyAI = null; }
  // test aids that move him (H, ?ladder=): the cloth restarts from the new pose (cloth.js reset)
  // and the camera resets as at spawn (player.js reset(): FUN_003285d0's snap; GUESS: the
  // angles are CAM_SPAWN, and whether the game resets the camera on a warp is not decoded)
  // Warp (ladder test aid): FUN_003285d0 takes the angles from render view 1's forward. GUESS:
  // that is the current view (not decoded; notes/fidelity-audit.md), so the warp keeps yaw/pitch.
  ctrl.onTeleport = () => { if (cape) cape.cloth.started = false; follow.reset(ctrl.pos, { yaw: follow.yaw, pitch: follow.pitch }); };
  attachGun(vin.mixRoot);                          // static gun under j56 (gun.js), loads in the background
  // the game's follow camera (player.js, FUN_0032ace0); its ray tests the camera collision
  // class (null with the stub: no pull-in). Starts at the save state's camera values (GUESS
  // for the page's spawn, player.js CAM_SPAWN).
  const follow = new FollowCamera(camera, camRays || null);
  if (MOUSE_DIRECT) follow.mouseDirect = { sens: MOUSE_SENS * Math.PI / 180 };   // OPTION mouselook
  follow.reset(ctrl.pos);

  // ---- actions (player.js header / ACTIONS.md)
  const motions = {};
  loadMotions('vincent').then(m => Object.assign(motions, m)).catch(e => console.warn(e.message));
  ctrl.attach({ anim, motions });
  let actionHelp = [];
  if (params.get('actions') !== '0') {
    for (const a of await loadActions()) { ctrl.addAction(a.action); if (a.help) actionHelp.push(a.help); }
  }
  let packsLoaded = [];
  // the game frame waits for the packs: the game never runs a level before its motion data is in
  // memory. Without this a ✕ pressed while the jump pack was still downloading played the whole
  // jump on a frozen idle pose (jumpFrame keeps the head when the clip is missing).
  let packsDone = false;
  const packsReady = loadPacks(PACKS, anim, motions).then(g => { packsLoaded = g; packsDone = true; return g; });
  // the weapon (aim.js): FUN_00579570 in ctrl.step, the camera's weapon views (follow.weapon), the
  // reticle in the frame (output.hud). isFrozen: the report freeze (declared below; false until then)
  const weapon = AIM ? await setupAim({ ctrl, anim, motions, col, follow, output, vin, canvas,
                                     isFrozen: () => { try { return frozen; } catch (_) { return false; } } }) : null;
  actionHelp.push('Space jump (✕; the ladder takes it first)');
  if (weapon) actionHelp.push(weapon.help);

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
    // H: back to the spawn (debug aid, not a game feature)
    if (e.code === 'KeyH') { ctrl.reset(); if (cape) cape.cloth.started = false; if (weapon) weapon.reset(); follow.reset(ctrl.pos); detached = null; }   // respawn: spawn angles
    if (e.code === 'KeyR') { e.preventDefault(); report.open(); }
    if (e.code === 'Space' || e.code.startsWith('Arrow')) e.preventDefault();
  });
  addEventListener('keyup', e => keys.delete(e.code));
  addEventListener('blur', () => keys.clear());
  canvas.addEventListener('click', () => { try { canvas.requestPointerLock?.(); } catch (_) { /* drag fallback */ } });
  // Chrome can report one huge movementX/Y right after locking: skip that first
  // event and clamp spikes so the camera doesn't spin.
  let skipMove = false;
  const mouse = new MouseFeed();                   // mouse counts until the next game frame(s)
  // a camera reset (H respawn, teleport, ladder warp) also drops mouse counts banked for later game frames
  { const reset = follow.reset.bind(follow); follow.reset = (...r) => { mouse.dx = mouse.dy = 0; return reset(...r); }; }
  document.addEventListener('pointerlockchange', () => { skipMove = true; });
  const clampMv = v => Math.max(-150, Math.min(150, v));   // GUESS: browser input spike clamp (port convenience, not game logic)
  addEventListener('mousemove', e => {
    if (document.pointerLockElement !== canvas) return;
    if (skipMove) { skipMove = false; return; }
    if (!frozen && !detached) mouse.add(clampMv(e.movementX), clampMv(e.movementY));   // the game's USB-mouse look (player.js MOUSE_SCALE, MouseFeed)
  });
  let drag = null;
  canvas.addEventListener('pointerdown', e => { if (!document.pointerLockElement) drag = [e.clientX, e.clientY]; });
  addEventListener('pointerup', () => { drag = null; });
  addEventListener('pointermove', e => {
    if (!drag || document.pointerLockElement || frozen || detached) return;
    mouse.add(e.clientX - drag[0], e.clientY - drag[1]); drag = [e.clientX, e.clientY];
  });
  addEventListener('resize', () => {
    renderer.setSize(innerWidth, innerHeight);
    output.fitCamera(camera);
  });

  // keys -> camera-relative world direction through the camera matrix (rows 2 / 0)
  function aim(inp, { f, r }) {
    const x = inp.fwd * f.x + inp.side * r.x, z = inp.fwd * f.z + inp.side * r.z;
    return { ...inp, x, z };
  }
  function readInput() {
    let fwd = 0, side = 0;
    if (keys.has('KeyW') || keys.has('ArrowUp')) fwd += 1;
    if (keys.has('KeyS') || keys.has('ArrowDown')) fwd -= 1;
    if (keys.has('KeyD') || keys.has('ArrowRight')) side += 1;
    if (keys.has('KeyA') || keys.has('ArrowLeft')) side -= 1;
    return aim({ fwd, side, run: keys.has('ShiftLeft') || keys.has('ShiftRight'), keys, pressed }, follow.basis());
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
      clipTime: r4(anim.time()),
      action: ctrl.active ? ctrl.active.name : null, packs: packsLoaded,
      ground: g && { y: r4(g.y), feetError: r4(g.y - p.y), tri: g.tri, subtree: g.subtree,
                     mask: g.mask === undefined ? null : '0x' + g.mask.toString(16).padStart(4, '0'),
                     normal: g.normal.map(r4), surface: g.surface },
      collision: colName, spawn: [ctrl.spawn.x, ctrl.spawn.y, ctrl.spawn.z].map(r4),
    };
  }
  const report = initReport({
    renderer, camera, root, scene, attrs: floats, toast, view: camView, output,
    setFrozen(f) { frozen = f; keys.clear(); pressed.clear(); drag = null; if (!f) { clock.getDelta(); acc = 0; } },
    extraMeta: () => ({ page: 'play', player: playerState() }),
  });
  document.getElementById('btn-report').addEventListener('click', e => { e.stopPropagation(); report.open(); });

  // ---- loop: one ctrl.step per PAL game frame (TICK = 1/50 s), interpolated render.
  // GUESS: the catch-up cap (8 frames, then drop the time) is a port convenience; the game
  // caps its step at 5 ticks (FUN_00322f88).
  const clock = new THREE.Clock();
  let acc = 0, frames = 0, fpsAcc = 0, fps = 0;
  const stats = { ticks: 0, groundErrMax: 0, colName };
  function frame() {
    requestAnimationFrame(frame);
    const dt = frozen ? 0 : Math.min(clock.getDelta(), 8 * TICK);   // GUESS: same cap as below (port convenience)
    acc += dt;
    if (!packsDone) { acc = 0; pressed.clear(); mouse.dx = mouse.dy = 0; }   // clip packs still loading: draw only, no game frame (no mouse banked)
    let n = 0;
    const due = Math.min(Math.floor(acc / TICK), 8);   // game frames this rendered frame runs (MouseFeed)
    while (acc >= TICK && n < 8) {
      mouse.feed(follow, due - n);                 // this game frame's share of the mouse counts
      // the camera runs first in the game frame: it follows the player as the last frame
      // left him, and this frame's stick-to-world uses its new matrix (player.js camera notes)
      const inp = readInput();
      const moving = Math.hypot(inp.x, inp.z) > 1e-6;
      follow.tick({ pos: ctrl.pos, want: ctrl.C.dir, state: ctrl.P.state,
                    stick: moving ? (inp.run ? KEY_RUN : KEY_WALK) : 0, pad: null });
      const b = follow.basis();                    // re-aim the keys with this frame's matrix
      const pOld = [ctrl.pos.x, ctrl.pos.y, ctrl.pos.z];
      ctrl.step(aim(inp, b));
      pressed.clear();                             // an edge is seen by one tick only
      // feet vs ground check (for the HUD / tests)
      const g = col.groundY(ctrl.pos.x, ctrl.pos.z, ctrl.pos.y);
      if (g != null) stats.groundErrMax = Math.max(stats.groundErrMax, Math.abs(g - ctrl.pos.y));
      stats.lastGroundErr = g == null ? null : g - ctrl.pos.y;
      if (enemyAI) { php.pos = [ctrl.pos.x, ctrl.pos.y, ctrl.pos.z]; enemyAI.tick(STEP, pOld, php.pos); }
      if (enemies) enemies.tick(enemyAI);
      acc -= TICK; n++; stats.ticks++;
    }
    if (n === 8) acc = 0;                          // fell far behind (tab hidden): drop time
    const pose = ctrl.lerp(acc / TICK);
    vin.body.position.set(pose.x, pose.y, pose.z);
    vin.body.rotation.y = bodyYaw(pose.heading);
    anim.update(ctrl.speed, dt);
    if (cape) cape.render();

    if (detached) {
      camera.position.copy(detached.pos); camera.up.set(0, -1, 0);
      camera.lookAt(detached.pos.clone().add(detached.dir));
    } else {
      follow.update(acc / TICK);                   // between the last two game frames, as the body
    }
    if (weapon) weapon.render();                   // Vincent's first-person fade; the reticle draws in output.render
    output.render(root, camera);

    frames++; fpsAcc += dt;
    if (fpsAcc >= 0.5) { fps = frames / fpsAcc; frames = 0; fpsAcc = 0; }
    const p = ctrl.pos;
    hud.textContent = `${fps.toFixed(0)} fps   collision: ${colName}${packsDone ? '' : '   loading clips…'}\n` +
      `pos  ${p.x.toFixed(2)}, ${p.y.toFixed(2)}, ${p.z.toFixed(2)}\n` +
      `clip ${anim.override ? 'action' : anim.current} (${anim.clipName()}) x${anim.rate.toFixed(2)}` +
      `${ctrl.active ? '   action ' + ctrl.active.name : ''}` +
      `   speed ${(ctrl.speed * 60).toFixed(1)} u/s\n` +
      `onGround ${ctrl.onGround}${ctrl.hitWall ? '  WALL' : ''}` + (enemyAI ? `   HP ${php.hp}${ENEMY_DMG ? '' : ' (enemy damage off)'}` : '') +
      (weapon ? `   weapon ${weapon.core.P.drawn ? 'out' : 'away'} st ${weapon.core.P.st} T ${weapon.core.P.T}  cam 0x${weapon.core.cam.type.toString(16)}` : '') + '\n' +
      `click: mouse look (drag works too)   WASD move (walk)   Shift run   ${actionHelp.join('   ')}   H respawn   R report`;
  }
  window.__play = { ctrl, anim, follow, aim: weapon, col, stats, camera, renderer, output, root, vin, light, spawnY: gy,
                    playerState, camView, report, keys, pressed, motions, packsReady, actionHelp, cape, enemies, enemyAI, php };
  requestAnimationFrame(frame);
}

main().catch(e => { msg.textContent = 'error: ' + e.message; console.error(e); });
