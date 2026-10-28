// Dirge z128 browser viewer: draws data/scene.json (export_web_room.py) the way
// native/gpu/render_scene_textured does: prelit vertex colour, PS2 MODULATE
// (texel*vcol*2), per-vertex GS fog, GS alpha test (two passes), no culling,
// draw order = scene.json order. WASD + mouse free-fly.
//
// URL params: hud=0 hides the overlay, aa=1 turns on MSAA, dpr=N pins pixel ratio,
// cam=ex,ey,ez,yaw,pitch starts at that view ("copy view" writes it). yaw/pitch are
// degrees about world up (-Y): yaw = atan2(dir.x, dir.z), pitch = asin(dir . up).
// zprio=0 / zunder=0 turn off the polygon offsets on untextured / underlay room draws.
// outblur=1/0 forces the PS2 output merge on/off (output.js; default: on only with res=native).
// res=native renders the game's 432x512 framebuffer shown as a 4:3 picture (output.js); the
// default renders at canvas resolution and fills the canvas (Dan's choice, 2026-10-28).
import * as THREE from 'three';
import { initReport } from './report.js';
import { createOutputStage } from './output.js';

const params = new URLSearchParams(location.search);
const hud = document.getElementById('hud');
const msg = document.getElementById('msg');
if (params.get('hud') === '0') { hud.style.display = 'none'; document.getElementById('tools').style.display = 'none'; }

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
out float vFogW, vW;   // fog*w and w: their ratio interpolates screen-linearly (= native noperspective)
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
uniform int uAMode;      // 0 none, 1 keep a>=ref, 2 keep a<ref
out vec4 fragColor;
void main() {
  vec4 c = vCol;
  if (uTexOn) {
    vec4 t = texture(uTex, vUV);
    c.rgb = min(t.rgb * vCol.rgb * 2.0, 1.0);   // PS2 MODULATE: 128 == 1.0
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
  if (!tex && draw.kind === 'room' && params.get('zprio') !== '0') {
    // The unbound group (no texture, flat vertex colour) shares planes with textured
    // faces; at equal depth they z-fight (pale staircase on walls). Push it back a
    // little so the textured face always wins a tie. zprio=0 turns this off.
    m.polygonOffset = true; m.polygonOffsetFactor = 1; m.polygonOffsetUnits = 4;
  }
  if (draw.under && params.get('zunder') !== '0') {
    // Underlay group (room.list col 5, export_z128_room.py UNDER_SUFFIX): a later chunk's
    // copy of a surface an earlier chunk already has on the same plane (e.g. 06800c12's
    // coarse floor over 06800112's tiles). The game draws the earlier chunk there, so this
    // copy loses the tie. A few depth steps only: anything really in front still shows.
    // zunder=0 turns this off.
    m.polygonOffset = true; m.polygonOffsetFactor = 1; m.polygonOffsetUnits = 4;
  }
  if (mode === 0) {
    m.blending = THREE.NoBlending;
  } else {
    // GS ATE, AFAIL=RGB_ONLY: every fragment blends by As; only passing ones write depth.
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

async function load() {
  const scene = await (await fetch('data/scene.json')).json();
  const files = [...Object.values(scene.meshes), ...Object.values(scene.textures)];
  let done = 0;
  const bufs = {};
  await Promise.all(files.map(async f => {
    bufs[f.file] = await fetchBuf('data/' + f.file);
    msg.textContent = `loading ${++done}/${files.length}`;
  }));

  const textures = {};
  for (const [id, t] of Object.entries(scene.textures)) {
    const tx = new THREE.DataTexture(new Uint8Array(bufs[t.file]), t.w, t.h, THREE.RGBAFormat);
    tx.wrapS = tx.wrapT = THREE.RepeatWrapping;
    tx.magFilter = THREE.LinearFilter; tx.minFilter = THREE.LinearMipmapLinearFilter;
    tx.generateMipmaps = true; tx.flipY = false; tx.colorSpace = THREE.NoColorSpace;
    tx.needsUpdate = true;
    textures[id] = tx;
  }
  const attrs = {}, floats = {};
  for (const [id, m] of Object.entries(scene.meshes)) {
    floats[id] = new Float32Array(bufs[m.file]);
    const ib = new THREE.InterleavedBuffer(floats[id], 12);
    attrs[id] = {
      position: new THREE.InterleavedBufferAttribute(ib, 3, 0),
      normal: new THREE.InterleavedBufferAttribute(ib, 3, 3),
      uv: new THREE.InterleavedBufferAttribute(ib, 2, 6),
      color: new THREE.InterleavedBufferAttribute(ib, 4, 8),
    };
  }

  const root = new THREE.Scene();
  const lo = new THREE.Vector3(...scene.bbox.min), hi = new THREE.Vector3(...scene.bbox.max);
  scene.draws.forEach((d, di) => {
    const a = attrs[d.mesh];
    const g = new THREE.BufferGeometry();
    for (const k of ['position', 'uv', 'color']) g.setAttribute(k, a[k]);
    g.setDrawRange(d.first, d.count);
    g.boundingSphere = new THREE.Sphere(lo.clone().add(hi).multiplyScalar(.5), lo.distanceTo(hi));
    const tex = d.tex ? textures[d.tex] : null;
    const modes = d.aref >= 0 && tex ? [1, 2] : [0];
    for (const mode of modes) {
      const mesh = new THREE.Mesh(g, makeMaterial(scene, tex, d, mode));
      mesh.frustumCulled = false; mesh.matrixAutoUpdate = false;
      mesh.name = d.name + (mode ? `#ate${mode}` : '');
      mesh.userData = { draw: di, mode };
      root.add(mesh);
    }
  });
  return { scene, root, lo, hi, floats };
}

function nearFar(eye, lo, hi) {
  // native explicit_camera(): far reaches the box from the eye plus one diagonal.
  const c = lo.clone().add(hi).multiplyScalar(.5);
  const far = Math.max(eye.distanceTo(c) + lo.distanceTo(hi) + 1, 1);
  return [Math.max(far * 0.0005, 0.5), far];
}

async function main() {
  const { scene, root, lo, hi, floats } = await load();
  msg.style.display = 'none';

  const renderer = new THREE.WebGLRenderer({ antialias: params.get('aa') === '1', alpha: false });
  renderer.setPixelRatio(params.has('dpr') ? +params.get('dpr') : Math.min(devicePixelRatio, 2));
  renderer.setSize(innerWidth, innerHeight);
  renderer.sortObjects = false;                    // keep the game's draw order
  renderer.outputColorSpace = THREE.LinearSRGBColorSpace;
  renderer.setClearColor(new THREE.Color().setRGB(...scene.clear, THREE.LinearSRGBColorSpace), 1);
  document.body.appendChild(renderer.domElement);
  // PS2 output (output.js): the game's 432x512 framebuffer, its projection (FUN_0040d478),
  // the PCRTC merge (FUN_0040d228) and the 4:3 display window, with ?res=native.
  // Default: the sharp canvas-resolution render filling the canvas (Dan's choice, 2026-10-28);
  // ?outblur=1 / 0 = merge on / off
  // Output merge: on in native mode (the game's look), off in the full-res default (sharp, Dan's
  // choice 2026-10-28); ?outblur=1 / 0 forces it.
  const nativeRes = params.get('res') === 'native';
  const output = createOutputStage(renderer, { enabled: params.has('outblur') ? params.get('outblur') !== '0' : nativeRes,
                                               msaa: params.get('aa') === '1', native: nativeRes });

  const cam0 = scene.camera;
  const eye0 = new THREE.Vector3(...cam0.eye);
  const [near, far] = nearFar(eye0, lo, hi);
  const camera = new THREE.PerspectiveCamera(cam0.fov, innerWidth / innerHeight, near, far);
  output.fitCamera(camera);                        // the game's projection (native) or the canvas aspect (full)
  const worldUp = new THREE.Vector3(...scene.up).normalize();
  const fwd = new THREE.Vector3();

  function hero() {
    camera.position.copy(eye0);
    camera.up.set(...cam0.up);
    camera.lookAt(...cam0.at);
    fwd.set(...cam0.at).sub(eye0).normalize();
  }
  // yaw/pitch about world up; horizontal basis X/Z (z128 up is -Y)
  if (Math.abs(Math.abs(worldUp.y) - 1) > 1e-6) console.warn('yaw/pitch assume a +-Y world up');
  const yawPitch = d => [Math.atan2(d.x, d.z) * 180 / Math.PI,
                         Math.asin(Math.max(-1, Math.min(1, d.dot(worldUp)))) * 180 / Math.PI];
  function dirFrom(yaw, pitch) {
    const y = yaw * Math.PI / 180, p = pitch * Math.PI / 180;
    return new THREE.Vector3(Math.sin(y) * Math.cos(p), 0, Math.cos(y) * Math.cos(p))
      .addScaledVector(worldUp, Math.sin(p)).normalize();
  }
  const camString = () => {
    const p = camera.position, [yw, pt] = yawPitch(fwd);
    return [p.x.toFixed(2), p.y.toFixed(2), p.z.toFixed(2), yw.toFixed(3), pt.toFixed(3)].join(',');
  };
  function setCam(str) {
    const v = String(str).split(',').map(Number);
    if (v.length !== 5 || !v.every(Number.isFinite)) return false;
    camera.position.set(v[0], v[1], v[2]);
    camera.up.copy(worldUp);
    fwd.copy(dirFrom(v[3], v[4]));
    camera.lookAt(camera.position.clone().add(fwd));
    camera.updateMatrixWorld();
    return true;
  }
  if (!(params.has('cam') && setCam(params.get('cam')))) hero();
  const viewURL = () => {
    const u = new URL(location.href);
    u.searchParams.set('cam', camString());
    return u.toString().replace(/%2C/gi, ',');
  };

  const cb = scene.char.bbox;
  const charH = Math.abs(cb.max[1] - cb.min[1]) || 100;
  const speed = charH * 1.5;                       // world units per second

  const keys = new Set();
  let frozen = false;                              // report mode: no movement, no redraw
  const typing = e => /^(INPUT|TEXTAREA)$/.test(e.target.tagName);
  addEventListener('keydown', e => {
    if (typing(e) || frozen) return;
    keys.add(e.code);
    if (e.code === 'KeyH') hero();
    if (e.code === 'KeyR') { e.preventDefault(); report.open(); }
  });
  addEventListener('keyup', e => keys.delete(e.code));
  addEventListener('blur', () => keys.clear());
  const canvas = renderer.domElement;
  canvas.addEventListener('click', () => canvas.requestPointerLock?.());

  function look(dx, dy) {
    camera.up.copy(worldUp);                       // drop the hero roll once the user steers
    const right = new THREE.Vector3().crossVectors(fwd, worldUp).normalize();
    fwd.applyAxisAngle(worldUp, -dx * 0.0025);
    const pitched = fwd.clone().applyAxisAngle(right, -dy * 0.0025);
    if (Math.abs(pitched.dot(worldUp)) < 0.98) fwd.copy(pitched);
    fwd.normalize();
  }
  addEventListener('mousemove', e => {
    if (document.pointerLockElement === canvas && !frozen) look(e.movementX, e.movementY);
  });
  // drag-to-look fallback (no pointer lock)
  let drag = null;
  canvas.addEventListener('pointerdown', e => { if (!document.pointerLockElement) drag = [e.clientX, e.clientY]; });
  addEventListener('pointerup', () => { drag = null; });
  addEventListener('pointermove', e => {
    if (!drag || document.pointerLockElement || frozen) return;
    look(e.clientX - drag[0], e.clientY - drag[1]); drag = [e.clientX, e.clientY];
  });

  addEventListener('resize', () => {
    renderer.setSize(innerWidth, innerHeight);
    output.fitCamera(camera);
  });

  // toast + buttons (the HUD itself ignores the pointer)
  const toastEl = document.getElementById('toast');
  let toastT = 0;
  function toast(text, ms = 3000) {
    toastEl.textContent = text; toastEl.style.display = 'block';
    clearTimeout(toastT); toastT = setTimeout(() => { toastEl.style.display = 'none'; }, ms);
  }
  async function copyText(t) {
    try { await navigator.clipboard.writeText(t); return true; } catch (_) { /* http: no clipboard API */ }
    const ta = Object.assign(document.createElement('textarea'), { value: t, readOnly: true });
    ta.style.cssText = 'position:fixed;left:-9999px;top:0';
    document.body.append(ta); ta.select(); ta.setSelectionRange(0, t.length);
    let ok = false; try { ok = document.execCommand('copy'); } catch (_) { ok = false; }
    ta.remove();
    return ok;
  }
  document.getElementById('btn-copy').addEventListener('click', async e => {
    e.stopPropagation();
    const u = viewURL();
    history.replaceState(null, '', u);             // the address bar holds it either way
    toast(await copyText(u) ? 'view link copied' : 'copy blocked: the address bar now has the view link');
  });
  document.getElementById('btn-report').addEventListener('click', e => { e.stopPropagation(); report.open(); });
  const report = initReport({
    renderer, camera, root, scene, attrs: floats, toast, output,
    setFrozen(f) { frozen = f; keys.clear(); drag = null; if (!f) clock.getDelta(); },
    view() {
      const [yw, pt] = yawPitch(fwd);
      const r = v => v.toArray().map(x => +x.toFixed(4));
      return { eye: r(camera.position), dir: r(fwd), up: r(camera.up), yaw: +yw.toFixed(3), pitch: +pt.toFixed(3),
               fov: camera.fov, aspect: +camera.aspect.toFixed(5), near: +camera.near.toFixed(4),
               far: +camera.far.toFixed(2), cam: camString(), link: viewURL() };
    },
  });

  const clock = new THREE.Clock();
  let frames = 0, acc = 0, fps = 0;
  const tris = scene.draws.reduce((n, d) => n + d.count / 3, 0);
  function frame() {
    requestAnimationFrame(frame);
    if (frozen) return;
    const dt = Math.min(clock.getDelta(), 0.1);
    const right = new THREE.Vector3().crossVectors(fwd, worldUp).normalize();
    const mv = new THREE.Vector3();
    if (keys.has('KeyW')) mv.add(fwd);
    if (keys.has('KeyS')) mv.sub(fwd);
    if (keys.has('KeyD')) mv.add(right);
    if (keys.has('KeyA')) mv.sub(right);
    if (keys.has('Space') || keys.has('KeyE')) mv.add(worldUp);
    if (keys.has('KeyC') || keys.has('KeyQ')) mv.sub(worldUp);
    if (mv.lengthSq() > 0) {
      const boost = keys.has('ShiftLeft') || keys.has('ShiftRight') ? 4 : 1;
      camera.position.addScaledVector(mv.normalize(), speed * boost * dt);
    }
    camera.lookAt(camera.position.clone().add(fwd));
    output.render(root, camera);

    frames++; acc += dt;
    if (acc >= 0.5) { fps = frames / acc; frames = 0; acc = 0; }
    const p = camera.position, [yw, pt] = yawPitch(fwd);
    hud.textContent = `${fps.toFixed(0)} fps  ${tris.toLocaleString()} tris\n` +
      `eye  ${p.x.toFixed(1)}, ${p.y.toFixed(1)}, ${p.z.toFixed(1)}\n` +
      `dir  ${fwd.x.toFixed(3)}, ${fwd.y.toFixed(3)}, ${fwd.z.toFixed(3)}\n` +
      `yaw ${yw.toFixed(1)}  pitch ${pt.toFixed(1)}  fov ${camera.fov.toFixed(1)}\n` +
      `cam=${camString()}\n` +
      `click: mouse look   WASD move   Space/E up  C/Q down\nShift fast   H hero camera   R report`;
    window.__frames = (window.__frames || 0) + 1;
  }
  window.__view = { setCam, camString, hero, render: () => output.render(root, camera), camera, renderer, output, root, scene };
  requestAnimationFrame(frame);
}

main().catch(e => { msg.textContent = 'error: ' + e.message; console.error(e); });
