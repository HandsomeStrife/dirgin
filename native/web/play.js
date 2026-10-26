// Dirge z128, playable: Vincent (data/vincent/vincent.glb) walking the room with
// third-person controls, a follow camera and collision (collision.js, or the stub).
//
// The room loading and shader are copied from main.js (kept separate on purpose).
// Game space, -Y up. URL params: hud=0, aa=1, dpr=N, col=stub (force the stub),
// cam=ex,ey,ez,yaw,pitch (detached camera at that view; H re-attaches it).
import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { TICK, PlayerController, SpeedAnimator, FollowCamera } from './player.js';

const params = new URLSearchParams(location.search);
const hud = document.getElementById('hud');
const msg = document.getElementById('msg');
if (params.get('hud') === '0') hud.style.display = 'none';

const SPAWN = { x: -1395.63, z: -1110.51 };
const ROOT_PER_FRAME = { walk: 0.525, run: 1.665 };   // clip root motion, glb units per 60 Hz frame

// ------------------------------------------------------------ room (from main.js)
const VS = `
precision highp float;
in vec3 position;
in vec2 uv;
in vec4 color;
uniform mat4 modelViewMatrix, projectionMatrix;
uniform bool uFog;
uniform float uFogA, uFogB, uFogMin;
out vec2 vUV;
out vec4 vCol;
out float vFogW, vW;
void main() {
  vec4 clip = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
  float f = uFog ? clamp(uFogA * clip.w + uFogB, uFogMin, 1.0) : 1.0;
  vUV = uv; vCol = color; vFogW = f * clip.w; vW = clip.w;
  gl_Position = clip;
}`;

const FS = `
precision highp float;
in vec2 vUV;
in vec4 vCol;
in float vFogW, vW;
uniform sampler2D uTex;
uniform bool uTexOn, uFog, uOutAlpha;
uniform vec3 uFogColor;
uniform float uARef;
uniform int uAMode;
out vec4 fragColor;
void main() {
  vec4 c = vCol;
  if (uTexOn) {
    vec4 t = texture(uTex, vUV);
    c.rgb = min(t.rgb * vCol.rgb * 2.0, 1.0);
    c.a   = min(t.a   * vCol.a   * 2.0, 1.0);
  }
  if (uAMode == 1 && c.a <  uARef) discard;
  if (uAMode == 2 && c.a >= uARef) discard;
  if (uFog) c.rgb = mix(uFogColor, c.rgb, vFogW / vW);
  fragColor = vec4(c.rgb, uOutAlpha ? c.a : 1.0);
}`;

function makeMaterial(scene, tex, draw, mode) {
  const fog = scene.fog;
  const m = new THREE.RawShaderMaterial({
    glslVersion: THREE.GLSL3, vertexShader: VS, fragmentShader: FS,
    side: THREE.DoubleSide, depthFunc: THREE.LessDepth,
    uniforms: {
      uTex: { value: tex }, uTexOn: { value: !!tex },
      uFog: { value: !!(fog && draw.fog) },
      uFogA: { value: fog ? -1 / (fog.far - fog.near) : 0 },
      uFogB: { value: fog ? fog.far / (fog.far - fog.near) : 0 },
      uFogMin: { value: fog ? fog.min : 0 },
      uFogColor: { value: new THREE.Vector3(...(fog ? fog.color : [0, 0, 0])) },
      uARef: { value: draw.aref }, uAMode: { value: mode }, uOutAlpha: { value: mode !== 0 },
    },
  });
  if (mode === 0) {
    m.blending = THREE.NoBlending;
  } else {
    m.blending = THREE.CustomBlending;
    m.blendEquation = THREE.AddEquation;
    m.blendSrc = THREE.SrcAlphaFactor; m.blendDst = THREE.OneMinusSrcAlphaFactor;
    m.blendSrcAlpha = THREE.ZeroFactor; m.blendDstAlpha = THREE.OneFactor;
    m.depthWrite = mode === 1;
  }
  return m;
}

async function fetchBuf(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  return r.arrayBuffer();
}

async function loadRoom() {
  const scene = await (await fetch('data/scene.json')).json();
  // the static posed Vincent in scene.json is replaced by the animated glb
  const draws = scene.draws.filter(d => d.kind !== 'char');
  const used = new Set(draws.map(d => d.mesh));
  const texUsed = new Set(draws.map(d => d.tex).filter(Boolean));
  const meshes = Object.entries(scene.meshes).filter(([id]) => used.has(id));
  const texs = Object.entries(scene.textures).filter(([id]) => texUsed.has(id));
  const files = [...meshes.map(e => e[1]), ...texs.map(e => e[1])];
  let done = 0;
  const bufs = {};
  await Promise.all(files.map(async f => {
    bufs[f.file] = await fetchBuf('data/' + f.file);
    msg.textContent = `loading room ${++done}/${files.length}`;
  }));

  const textures = {};
  for (const [id, t] of texs) {
    const tx = new THREE.DataTexture(new Uint8Array(bufs[t.file]), t.w, t.h, THREE.RGBAFormat);
    tx.wrapS = tx.wrapT = THREE.RepeatWrapping;
    tx.magFilter = THREE.LinearFilter; tx.minFilter = THREE.LinearMipmapLinearFilter;
    tx.generateMipmaps = true; tx.flipY = false; tx.colorSpace = THREE.NoColorSpace;
    tx.needsUpdate = true;
    textures[id] = tx;
  }
  const attrs = {};
  for (const [id, m] of meshes) {
    const ib = new THREE.InterleavedBuffer(new Float32Array(bufs[m.file]), 12);
    attrs[id] = {
      position: new THREE.InterleavedBufferAttribute(ib, 3, 0),
      uv: new THREE.InterleavedBufferAttribute(ib, 2, 6),
      color: new THREE.InterleavedBufferAttribute(ib, 4, 8),
    };
  }

  const root = new THREE.Scene();
  const lo = new THREE.Vector3(...scene.bbox.min), hi = new THREE.Vector3(...scene.bbox.max);
  for (const d of draws) {
    const a = attrs[d.mesh];
    const g = new THREE.BufferGeometry();
    for (const k of ['position', 'uv', 'color']) g.setAttribute(k, a[k]);
    g.setDrawRange(d.first, d.count);
    const tex = d.tex ? textures[d.tex] : null;
    for (const mode of (d.aref >= 0 && tex ? [1, 2] : [0])) {
      const mesh = new THREE.Mesh(g, makeMaterial(scene, tex, d, mode));
      mesh.frustumCulled = false; mesh.matrixAutoUpdate = false;
      mesh.name = d.name + (mode ? `#ate${mode}` : '');
      root.add(mesh);
    }
  }
  return { scene, root, lo, hi };
}

// ------------------------------------------------------------ Vincent
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
    if (m.name === 'alpha' || m.alphaTest > 0) {
      m.transparent = true; m.alphaTest = 0.77734; m.depthWrite = true;
    }
  });
  prims.forEach((o, i) => {
    const ex = o.userData || {};
    o.renderOrder = 1000 + (Number.isFinite(ex.drawOrder) ? ex.drawOrder : i);
  });
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
  return { body, mixRoot: gltf.scene, clips: { idle: clip('h_st'), walk: clip('h_wk'), run: clip('h_rn') }, prims };
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
  const [{ scene, root, lo, hi }, { col, name: colName }] = await Promise.all([loadRoom(), loadCol()]);
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
    walkSpeed: rootSpeed.walk, runSpeed: rootSpeed.run, radius: 3.5, height: 19,
  });
  const anim = new SpeedAnimator(vin.mixRoot, vin.clips, rootSpeed, { fade: 0.2 });
  const follow = new FollowCamera(camera, col, { dist: 52, pitch: 0.3, targetHeight: 15 });
  follow.yaw = heading0;

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
  addEventListener('keydown', e => {
    keys.add(e.code);
    if (e.code === 'KeyH') { ctrl.reset(); follow.yaw = heading0; follow.pitch = 0.3; detached = null; }
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
    follow.look(clampMv(e.movementX), clampMv(e.movementY));
  });
  let drag = null;
  canvas.addEventListener('pointerdown', e => { if (!document.pointerLockElement) drag = [e.clientX, e.clientY]; });
  addEventListener('pointerup', () => { drag = null; });
  addEventListener('pointermove', e => {
    if (!drag || document.pointerLockElement) return;
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
    return { x, z, run: keys.has('ShiftLeft') || keys.has('ShiftRight') };
  }

  // ---- loop: fixed 60 Hz sim, interpolated render
  const clock = new THREE.Clock();
  let acc = 0, frames = 0, fpsAcc = 0, fps = 0;
  const stats = { ticks: 0, groundErrMax: 0, colName };
  function frame() {
    requestAnimationFrame(frame);
    const dt = Math.min(clock.getDelta(), 0.25);
    acc += dt;
    let n = 0;
    while (acc >= TICK && n < 8) {
      ctrl.step(readInput());
      // feet vs ground check (for the HUD / tests)
      const g = col.groundY(ctrl.pos.x, ctrl.pos.z, ctrl.pos.y);
      if (g != null) stats.groundErrMax = Math.max(stats.groundErrMax, Math.abs(g - ctrl.pos.y));
      stats.lastGroundErr = g == null ? null : g - ctrl.pos.y;
      acc -= TICK; n++; stats.ticks++;
    }
    if (n === 8) acc = 0;                          // fell far behind (tab hidden): drop time
    const pose = ctrl.lerp(acc / TICK);
    vin.body.position.set(pose.x, pose.y, pose.z);
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
      `clip ${anim.current} (${{ idle: 'h_st', walk: 'h_wk', run: 'h_rn' }[anim.current]}) x${anim.rate.toFixed(2)}` +
      `   speed ${(ctrl.speed * 60).toFixed(1)} u/s\n` +
      `onGround ${ctrl.onGround}${ctrl.hitWall ? '  WALL' : ''}${follow.blocked ? '  cam pulled in' : ''}\n` +
      `click: mouse look (drag works too)   WASD move   Shift run   H respawn`;
  }
  window.__play = { ctrl, anim, follow, col, stats, camera, renderer, root, vin, spawnY: gy, rootSpeed };
  requestAnimationFrame(frame);
}

main().catch(e => { msg.textContent = 'error: ' + e.message; console.error(e); });
