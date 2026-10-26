// z128 room drawing shared by the web pages: loads data/scene.json (export_web_room.py)
// and builds one three.js mesh per draw (two for a GS alpha-test draw), drawn the way
// native/gpu/render_scene_textured does: prelit vertex colour, PS2 MODULATE
// (texel*vcol*2), per-vertex screen-linear GS fog, GS alpha test as two passes, no
// culling, draw order = scene.json order (the renderer needs sortObjects = false).
//
// Taken from main.js (which still has its own copy; it can switch to this module).
// play.js uses it, so the z-fight fixes live in one place:
//  zprio  - untextured room draws get polygon offset (1, 4): the unbound group shares
//           planes with textured faces (pale staircase on walls). ?zprio=0 turns it off.
//  zunder - draws with under: true (room.list col 5, export_z128_room.py UNDER_SUFFIX)
//           get the same offset: a later chunk's coarse copy of a surface an earlier
//           chunk already has on that plane loses the tie, as in the game.
//           ?zunder=0 turns it off.
import * as THREE from 'three';

export const VS = `
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

export const FS = `
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

// The two depth-tie offsets, read from the page URL (both on unless =0).
export function roomOptions(params = new URLSearchParams(location.search)) {
  return { zprio: params.get('zprio') !== '0', zunder: params.get('zunder') !== '0' };
}

// mode: 0 = plain draw; 1 / 2 = GS ATE pass keeping alpha >= ref / < ref.
export function makeMaterial(scene, tex, draw, mode, opts = roomOptions()) {
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
  // A few depth steps only: anything really in front still shows.
  if ((!tex && draw.kind === 'room' && opts.zprio) || (draw.under && opts.zunder)) {
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

// Load scene.json and build the room.
//   skip(draw)      - leave a draw out (play.js drops the static posed Vincent)
//   onProgress(done, total)
//   normals         - also bind the normal attribute (no shader reads it; main.js binds only
//                     position, uv and color)
//   opts            - roomOptions() unless given
// Returns { scene, root, lo, hi, floats }: floats[mesh id] = the raw 12-float vertex
// buffer (report.js ray casts read it); each mesh's userData.draw indexes scene.draws.
export async function loadRoom({ skip = null, onProgress = null, normals = false, opts = roomOptions(), base = 'data/' } = {}) {
  const r = await fetch(base + 'scene.json');
  if (!r.ok) throw new Error(`scene.json: HTTP ${r.status}`);
  const scene = await r.json();
  const draws = scene.draws.map((d, i) => [d, i]).filter(([d]) => !(skip && skip(d)));
  const used = new Set(draws.map(([d]) => d.mesh));
  const texUsed = new Set(draws.map(([d]) => d.tex).filter(Boolean));
  const meshes = Object.entries(scene.meshes).filter(([id]) => used.has(id));
  const texs = Object.entries(scene.textures).filter(([id]) => texUsed.has(id));
  const files = [...meshes.map(e => e[1]), ...texs.map(e => e[1])];
  let done = 0;
  const bufs = {};
  await Promise.all(files.map(async f => {
    bufs[f.file] = await fetchBuf(base + f.file);
    if (onProgress) onProgress(++done, files.length);
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
  const attrs = {}, floats = {};
  for (const [id, m] of meshes) {
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
  const sphere = new THREE.Sphere(lo.clone().add(hi).multiplyScalar(.5), lo.distanceTo(hi));
  for (const [d, di] of draws) {
    const a = attrs[d.mesh];
    const g = new THREE.BufferGeometry();
    for (const k of normals ? ['position', 'normal', 'uv', 'color'] : ['position', 'uv', 'color']) g.setAttribute(k, a[k]);
    g.setDrawRange(d.first, d.count);
    g.boundingSphere = sphere.clone();
    const tex = d.tex ? textures[d.tex] : null;
    for (const mode of (d.aref >= 0 && tex ? [1, 2] : [0])) {
      const mesh = new THREE.Mesh(g, makeMaterial(scene, tex, d, mode, opts));
      mesh.frustumCulled = false; mesh.matrixAutoUpdate = false;
      mesh.name = d.name + (mode ? `#ate${mode}` : '');
      mesh.userData = { draw: di, mode };
      root.add(mesh);
    }
  }
  return { scene, root, lo, hi, floats };
}
