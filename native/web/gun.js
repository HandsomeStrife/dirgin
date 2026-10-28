// Vincent's gun (Cerberus), drawn as a static mesh under joint 56: the gun attach is
// marker 15 -> bone 56 (FUN_00316270 / FUN_005ab8f0; notes/actions-research.md section 4).
// Mesh and atlas: native/assets/models/export_web_gun.py (data/vincent/cerberus_web.*); its
// parts list is a GUESS listed in notes/fidelity-audit.md (BYO build, "Gun parts shown").
// Visual only: no gun motion (the reload clip turns gun bone 3; not exported). ?gun=0 hides it.
import * as THREE from 'three';

async function loadGun() {
  const base = 'data/vincent/';
  const meta = await (await fetch(base + 'cerberus_web.json')).json();
  if (!Array.isArray(meta.runs) || meta.alpha_ref == null)
    throw new Error('cerberus_web.json is from an older export (no runs / alpha_ref): re-run export_web_gun.py');
  const r = await fetch(base + 'cerberus_web.bin');
  if (!r.ok) throw new Error('cerberus_web.bin: HTTP ' + r.status);
  const f = new Float32Array(await r.arrayBuffer());
  const ib = new THREE.InterleavedBuffer(f, meta.stride);
  const g = new THREE.BufferGeometry();
  g.setAttribute('position', new THREE.InterleavedBufferAttribute(ib, 3, 0));
  g.setAttribute('uv', new THREE.InterleavedBufferAttribute(ib, 2, 3));
  g.setAttribute('color', new THREE.InterleavedBufferAttribute(ib, 4, 5));
  const tex = await new THREE.TextureLoader().loadAsync(base + meta.atlas);
  tex.colorSpace = THREE.NoColorSpace; tex.flipY = false;
  tex.magFilter = THREE.LinearFilter;
  // Back-face cull, as for the body (play.js loadVincent): the game draws the gun with the
  // same ADC culling. GS dumps hero 163235 / walk 185126 / 185138: every drawn gun triangle
  // (78 / 80 / 69) is clockwise on screen (y down) in our index order (29 / 27 / 27 have
  // only that reading; the rest are two-sided cards whose twin is the other order), none
  // only counter-clockwise. Clockwise in index order = a WebGL back face. DoubleSide drew
  // both twins of every two-sided card on top of each other.
  //
  // Materials per run (meta.runs = [first tri, count, ate], triangles in the game's draw
  // order, export_web_gun.py). GS dumps hero / 185126 / 185138, every gun draw: TEX0 PSM T8
  // TCC 1 TFX MODULATE, TEST ZTE 1 ZTST GEQUAL, ZBUF ZMSK 0, drawn right after Vincent's
  // last (hair) draw. Descriptor flag bit 1 (FUN_005b19b8; 5 drawn triangles: 1 frame0,
  // 4 barrel1) adds ATE GEQUAL AREF 100, AFAIL RGB_ONLY and ABE (Cs-Cd)*As+Cd; the rest
  // are ATE 0 / ABE 0 (texture alpha unused). The ATE draws are the same GS mode as
  // Vincent's hair, so they get play.js's two passes (gsBlend / gsAlphaPasses, not exported;
  // translated here): pass 1 keeps As >= ref (blend + depth write), pass 2 As < ref
  // (blend, no depth write). Non-transparent materials (sortObjects = false: drawn in scene-graph
  // order), so attachGun puts the mesh after Vincent's mesh to draw after his hair.
  const opaque = new THREE.MeshBasicMaterial({ map: tex, vertexColors: true, side: THREE.BackSide });
  const ref = meta.alpha_ref;
  const pass1 = new THREE.MeshBasicMaterial({ map: tex, vertexColors: true, side: THREE.BackSide,
    alphaTest: ref, depthWrite: true });
  const pass2 = pass1.clone(); pass2.depthWrite = false;
  pass2.onBeforeCompile = sh => {
    sh.fragmentShader = sh.fragmentShader.replace('#include <alphatest_fragment>',
      'if ( diffuseColor.a >= alphaTest ) discard;   // GS AFAIL pass: keep a < ref');
  };
  pass2.customProgramCacheKey = () => 'gs-afail';
  for (const m of [pass1, pass2]) gsBlend(m);
  for (const [t0, n, ate] of meta.runs) {
    if (ate) { g.addGroup(3 * t0, 3 * n, 1); g.addGroup(3 * t0, 3 * n, 2); }
    else g.addGroup(3 * t0, 3 * n, 0);
  }
  const mesh = new THREE.Mesh(g, [opaque, pass1, pass2]);
  mesh.name = 'cerberus'; mesh.frustumCulled = false;
  return mesh;
}

// GS ABE (Cs-Cd)*As+Cd, framebuffer alpha kept: play.js gsBlend (same settings)
function gsBlend(m) {
  m.transparent = false;
  m.blending = THREE.CustomBlending;
  m.blendEquation = THREE.AddEquation;
  m.blendSrc = THREE.SrcAlphaFactor; m.blendDst = THREE.OneMinusSrcAlphaFactor;
  m.blendSrcAlpha = THREE.ZeroFactor; m.blendDstAlpha = THREE.OneFactor;
}

// load the mesh and draw it with joint j56's transform (the attach), as the last child of
// the rig (mixRoot), after vincent_mesh: the GS dumps draw the gun right after Vincent's last
// hair draw. Under j56 itself it would draw before all of Vincent (the joints come first).
export async function attachGun(mixRoot) {
  if (new URLSearchParams(location.search).get('gun') === '0') return null;
  let j56 = null;
  mixRoot.traverse(n => { if (!j56 && n.name === 'j56') j56 = n; });
  if (!j56) { console.info('gun not shown: no joint j56'); return null; }
  try {
    const mesh = await loadGun();
    mesh.matrixAutoUpdate = false;
    // j56's world matrix is current by render time; copied before this mesh's draw
    mesh.onBeforeRender = () => { mesh.matrixWorld.copy(j56.matrixWorld); };
    mixRoot.add(mesh);
    return mesh;
  } catch (e) {
    console.info('gun not shown:', e.message);
    return null;
  }
}
