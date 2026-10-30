// Aiming and shooting on the play page: wires aim_core.js (the translated game logic) to the
// controller, the animator, the game's camera (player.js FollowCamera, which runs the weapon
// views through FollowCamera.weapon), collision and the HUD (drawn into the frame, output.js).
// notes/aim-research.md has the decode; every GUESS here is listed in notes/fidelity-audit.md.
//
// Keys (GUESS mapping, like the other keys; the game's buttons in brackets):
//   left mouse [R1] draw the gun (first press), then fire;  Z / middle mouse [R3] first person;
//   C [circle] put the gun away.  ?aim=0 turns all of this off.
import * as THREE from 'three';
import { GAME, STEP } from './player.js';
import { WeaponSystem, AIM_SETS, AIM_IDLE, AIM_FIRE, AIM_WALK_THR, quadrant, moveAngleDeg,
         reticleQuads, reticleRay, aimRayPoints } from './aim_core.js';
export { aimRayPoints };

export const HELP = 'LMB draw/fire   Z first person   C holster';

async function json(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  return r.json();
}

// ---------------------------------------------------------------- reticle (in the frame)
// FUN_003b1048 per piece: n copies rotated 2pi/n about the reticle centre (copies < 0: a
// mirrored pair), alpha by the phase time (flags 1 / 2 / 4 / 8), spin (0x10 / 0x20 / 0x40).
// The game draws it into the framebuffer after the scene (GS dump reticle_f200: draws 11200-11220,
// before the PCRTC merge), so it goes through output.js's hud hook and the report captures it.
// Centre: HUD (256 + 0x7c x 256, 224 + 0x78 x 224) on the 512x448 grid (FUN_0033c868); HUD -> frame
// x 432/512, y 512/448, HUD centre = frame (216, 256) (the GS dump).
// Texture: 'menu2' (data/etc/menudata.bin; export_web_aim.py menu_texture), PSMT4 128x128, 16-colour
// CLUT = white with alpha 0x02..0x80. UVs per vertex (FUN_003b1048 via FUN_003cb1f0 / FUN_003cb268):
// u0 = frame u, u1 = u + w, v0 = frame v, v1 = v + h; strip vertices (x0,y0,u0,v0) (x1,y0,u1,v0)
// (x0,y1,u0,v1) (x1,y1,u1,v1), rotated / mirrored with their positions. GS state of the draws (dump):
// TEX1_1 = 0 (nearest), CLAMP_1 = 0 (repeat), TEX0 TCC 1 TFX 0 (modulate: Cv = Cf Ct >> 7, Av = Af At >> 7),
// ABE (Cs - Cd) As + Cd with As = Av / 128, no alpha test.
// Full-HD mode samples the same texels nearest at the target resolution (each texel a sharp block);
// the GS's 8-bit rounding of the blend is not reproduced.
// Position slides (flags 0x80-0x40000) and the 0x10000 scale are left out (they end at the record
// position / scale 1 in steady state).
class ReticleLayer {
  constructor(data) {
    this.data = data;
    this.tex = {};
    for (const [name, T] of Object.entries(data.textures || {})) {
      const px = new Uint8Array(T.w * T.h * 4);
      for (let i = 0; i < T.w * T.h; i++) px.set(T.clut[parseInt(T.idx[i], 16)], i * 4);
      const t = new THREE.DataTexture(px, T.w, T.h, THREE.RGBAFormat, THREE.UnsignedByteType);
      t.magFilter = t.minFilter = THREE.NearestFilter;            // TEX1_1 = 0
      t.wrapS = t.wrapT = THREE.RepeatWrapping;                   // CLAMP_1 = 0
      t.generateMipmaps = false; t.flipY = false; t.colorSpace = THREE.NoColorSpace;
      t.needsUpdate = true;
      this.tex[name] = { t, w: T.w, h: T.h };
    }
    this.max = 64 * 6;                                // vertices (21 quads x 6 = 126 used)
    this.pos = new Float32Array(this.max * 3); this.uv = new Float32Array(this.max * 2);
    this.col = new Float32Array(this.max * 4);
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.BufferAttribute(this.pos, 3));
    g.setAttribute('uv', new THREE.BufferAttribute(this.uv, 2));
    g.setAttribute('color', new THREE.BufferAttribute(this.col, 4));
    this.geo = g;
    this.mat = new THREE.RawShaderMaterial({
      glslVersion: THREE.GLSL3,
      uniforms: { tex: { value: null } },
      vertexShader: `precision highp float;
        uniform mat4 projectionMatrix, modelViewMatrix;
        in vec3 position; in vec2 uv; in vec4 color; out vec2 vUv; out vec4 vCol;
        void main() { vUv = uv; vCol = color; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`,
      fragmentShader: `precision highp float;
        uniform sampler2D tex; in vec2 vUv; in vec4 vCol; out vec4 o;
        void main() {
          vec4 t = floor(texture(tex, vUv) * 255.0 + 0.5);        // CLUT entry (alpha 0..0x80)
          vec3 c = min(floor(vCol.rgb * t.rgb / 128.0), 255.0);   // TFX 0 modulate
          float a = floor(vCol.a * t.a / 128.0);                  // TCC 1
          o = vec4(c / 255.0, a / 128.0);                         // As = A / 128
        }`,
      transparent: true, depthTest: false, depthWrite: false, side: THREE.DoubleSide,   // y-down camera flips the winding
      blending: THREE.CustomBlending, blendEquation: THREE.AddEquation,
      blendSrc: THREE.SrcAlphaFactor, blendDst: THREE.OneMinusSrcAlphaFactor,   // (Cs - Cd) As + Cd
      blendSrcAlpha: THREE.ZeroFactor, blendDstAlpha: THREE.OneFactor,          // frame alpha kept
    });
    this.mesh = new THREE.Mesh(g, this.mat);
    this.mesh.frustumCulled = false;
    this.scene = new THREE.Scene(); this.scene.add(this.mesh);
    this.cam = new THREE.OrthographicCamera(0, 1, 0, 1, -1, 1);
  }
  // the sprite frame (u, v, w, h) of each quad reticleQuads emits, in its order
  frames(k, variant) {
    const out = [];
    for (const pc of this.data.kinds[k].variants[variant]) {
      const fr = (this.data.sprite_frames[pc.sprite] || [])[pc.frame] || null;
      const n = pc.copies > 0 ? pc.copies : pc.copies < 0 ? 2 : 0;
      for (let i = 0; i < n; i++) out.push(fr);
    }
    return out;
  }
  // o: output.js hud() mapping; core: aim_core WeaponSystem
  draw(renderer, o, core) {
    let n = 0, tex = null;
    const fx = hx => 216 + hx * 432 / 512, fy = hy => 256 + (hy + core.hud.offY * 224) * 512 / 448;
    const tx = x => o.cx + (x - o.x0) * o.kx, ty = y => o.cy + (y - o.y0) * o.ky;
    for (let k = 1; k < 8; k++) {
      if (!core.r.phase[k] || !this.data.kinds[k]) continue;
      const T = this.tex[this.data.kinds[k].texture];
      if (!T) continue;
      if (tex && tex !== T) break;                    // one texture per draw call (all kinds use 'menu2')
      tex = T;
      const frs = this.frames(k, 0);
      reticleQuads(this.data, k, 0, core.reticleT(k)).forEach((q, j) => {
        const fr = frs[j];
        if (q.a <= 0 || !fr || n + 6 > this.max) return;
        const u0 = fr[0] / T.w, u1 = (fr[0] + fr[2]) / T.w, v0 = fr[1] / T.h, v1 = (fr[1] + fr[3]) / T.h;
        const uv = [[u0, v0], [u1, v0], [u1, v1], [u0, v1]];
        for (const i of [0, 1, 2, 0, 2, 3]) {
          this.pos.set([tx(fx(q.p[i][0])), ty(fy(q.p[i][1])), 0], n * 3);
          this.uv.set(uv[i], n * 2);
          this.col.set([q.rgb[0], q.rgb[1], q.rgb[2], q.a], n * 4); n++;
        }
      });
    }
    if (!n) return;
    this.mat.uniforms.tex.value = tex.t;
    this.geo.setDrawRange(0, n);
    for (const a of ['position', 'uv', 'color']) this.geo.attributes[a].needsUpdate = true;
    const c = this.cam;
    c.left = 0; c.right = o.w; c.top = 0; c.bottom = o.h; c.updateProjectionMatrix();
    renderer.render(this.scene, c);
  }
}

// ---------------------------------------------------------------- aimed clip tables
// FUN_00345e58 -> FUN_00344340: root speed = |root(n-1) - root(0)| (xz) x 60 / n.
// GUESS: n = 2 x clip frames - 1 (FUN_005ad6e8's unit isn't decoded; this reproduces the traced
// h_wk / h_rn table roots 15.551 / 48.698 to 0.1%).
export function rootSpeed(motions, name) {
  const m = motions[name]; if (!m) return null;
  const r = m.root_motion_per_frame_glb, n = m.frames;
  const a = r[0], b = r[Math.min(n - 1, r.length - 1)];
  return Math.hypot(b[0] - a[0], b[2] - a[2]) * 60 / (2 * n - 1);
}
// FUN_00595440's table for quadrant q, or null while its clips / sidecars aren't loaded
export function aimTable(motions, anim, q) {
  const s = AIM_SETS[q];
  const keys = [AIM_IDLE, s.walk, s.run];
  if (!keys.every(k => anim.has(k) && motions[k])) return null;
  return [{ key: AIM_IDLE, thr: 0.0, root: 0.0 },
          { key: s.walk, thr: AIM_WALK_THR, root: rootSpeed(motions, s.walk) },
          { key: s.run, thr: 0.0, root: rootSpeed(motions, s.run) }];
}

// ---------------------------------------------------------------- clip requests of the classes
// Pure (no three.js): test_aim.mjs drives it with the real SpeedAnimator logic.
//   clips()      once per game frame after FUN_00579570 (not during actions)
//   actionEnter  the ladder mount (FUN_00576c88 / FUN_00576de8) calls FUN_00576548(P, 0)
export function makeClipControl({ core, anim, ctrl, motions, step = STEP }) {
  const tables = {};
  const IDLE = [{ key: AIM_IDLE, thr: 0.0, root: 0.0 }];      // the aim idle class: h_st_a (slot 0x40)
  let quad = 0, fire = null, inMove = false;
  function clips() {
    // the jump's motion states (FUN_00598f50 / FUN_00599aa0, jump_core.js) own the head layer
    // from the take-off until the landing ends; no other class runs (aim_g: a shot in the air
    // changes no clip). The classes below pick up again on the next frame.
    if (ctrl.jump && ctrl.jump.head) { core.events.length = 0; return; }
    if (!core.drawn()) {
      if (anim.request) anim.clearRequest();
      if (anim.table && anim.table !== GAME.loco) { anim.setTable(null, ctrl.C.groundSpeed); quad = 0; }
      fire = null; inMove = false;
      core.events.length = 0;
      return;
    }
    // idle class <-> aimed move class FUN_00595440: it returns to the idle class when C+0x1d8 == 0.
    // GUESS: it is entered when C+0x1d8 reaches 5 u/s, the same rule player.js uses for the
    // locomotion table install (traced: h_st_a at 1.9 / 3.7, h_wk_a at 5.6; aim_c f31-33, f161-163).
    if (!inMove && ctrl.C.groundSpeed >= 5.0) inMove = true;
    else if (inMove && ctrl.C.groundSpeed === 0) inMove = false;
    const moving = ctrl.C.groundSpeed > 0;
    // stand-fire class FUN_00596460: a shot while standing requests h_st_f; back to h_st_a when
    // the clip ends (FUN_003460d8) or after 30 ticks; moving leaves it (FUN_00596678 -> 9).
    // GUESS: a shot while the class runs doesn't restart it (traced aim_a f64; the class
    // dispatcher isn't decoded).
    if (core.events.length && !moving && !fire && anim.has(AIM_FIRE)) {
      if (anim.playRequest(AIM_FIRE)) fire = { t: 0 };
    }
    core.events.length = 0;
    if (fire) {
      fire.t += step;
      if (moving || !anim.request || anim.request.ended || fire.t > 30) { anim.clearRequest(); fire = null; }
      else return;
    }
    if (!inMove) {                                            // aim idle class: h_st_a
      if (anim.table !== IDLE && anim.has(AIM_IDLE)) { anim.setTable(IDLE); quad = 0; }
      return;
    }
    // aimed move class FUN_00595440: (re)install when the quadrant changes while moving.
    // The table is built (and cached) only once its clips and sidecar entries are loaded.
    const q = quadrant(moveAngleDeg(ctrl.C.face, ctrl.C.moveDir));
    if (q !== quad || anim.table === GAME.loco || anim.table === IDLE) {
      if (!tables[q]) tables[q] = aimTable(motions, anim, q);
      if (tables[q] && anim.setTable(tables[q])) quad = q;
    }
  }
  return {
    clips,
    actionEnter(a) {
      if (a.name !== 'ladder') return;
      core.ladderMount(ctrl.onGround, ctrl);             // FUN_00576548(P, 0) reads the ground bit
      if (anim.request) anim.clearRequest();
      fire = null;
    },
    reset() { quad = 0; fire = null; inMove = false; if (anim.request) anim.clearRequest(); if (anim.table && anim.table !== GAME.loco) anim.setTable(null); },
    state: () => ({ quad, fire: !!fire }),
  };
}

// ---------------------------------------------------------------- LMB -> R1 gate (pure)
// GUESS mapping (LMB = R1), but it must not steal the page's own mouse uses:
//  - pointer locked: a press is R1 (held while down);
//  - not locked and the browser can lock: the click is the one that takes the lock -> ignored;
//  - the browser can't lock (no requestPointerLock, or pointerlockerror): the left button is the
//    drag-look, so only a click that doesn't move (under 4 px) is R1, on release.
export function mouseR1Gate() {
  let pending = null;
  return {
    down({ locked, refused, x, y }) {
      pending = null;
      if (locked) return 'press';
      if (refused) { pending = { x, y }; return 'pending'; }
      return 'ignore';
    },
    up({ refused, x, y, onCanvas }) {
      const p = pending; pending = null;
      if (!p || !refused || !onCanvas) return null;
      return Math.hypot(x - p.x, y - p.y) < 4 ? 'tap' : null;
    },
  };
}

// ---------------------------------------------------------------- setup
// follow: player.js FollowCamera (it runs FUN_0032ace0 for the weapon views through .weapon);
// output: output.js stage (the reticle goes into its hud hook). isFrozen: the report's freeze.
export async function setupAim({ ctrl, anim, motions, col, follow, output, vin, canvas, isFrozen = () => false }) {
  let W, ret;
  try {
    [W, ret] = await Promise.all([json('data/aim/weapon.json'), json('data/aim/reticle.json')]);
  } catch (e) {
    console.warn('aim off (run native/assets/models/export_web_aim.py):', e.message);
    return null;
  }
  const core = new WeaponSystem(W, ret, { step: STEP });
  if (!ret.textures) console.warn('reticle texture missing: re-run native/assets/models/export_web_aim.py');
  const layer = new ReticleLayer(ret);
  const ctl = makeClipControl({ core, anim, ctrl, motions });
  follow.weapon = core;                              // the camera's weapon views, switch ramp, recoil, kick

  // ---- buttons (GUESS mapping; touch input isn't mapped). LMB on the game canvas is R1 with or without pointer lock
  // (the drag fallback); clicks on the report UI never reach the canvas.
  const btn = { r1: false, r1Edge: false, r3Edge: false, circleEdge: false };
  const clearEdges = () => { btn.r1 = false; btn.r1Edge = btn.r3Edge = btn.circleEdge = false; };
  const gate = mouseR1Gate();
  let lockRefused = typeof canvas.requestPointerLock !== 'function';
  document.addEventListener('pointerlockerror', () => { lockRefused = true; });
  // a refusal can be transient (a re-lock just after Esc): gaining the lock clears it
  document.addEventListener('pointerlockchange', () => { if (document.pointerLockElement === canvas) lockRefused = false; });
  const lockState = () => ({ locked: document.pointerLockElement === canvas, refused: lockRefused });
  addEventListener('mousedown', e => {
    if (e.target !== canvas || isFrozen()) return;
    if (e.button === 0 && gate.down({ ...lockState(), x: e.clientX, y: e.clientY }) === 'press') { btn.r1 = true; btn.r1Edge = true; }
    if (e.button === 1) { btn.r3Edge = true; e.preventDefault(); }
  });
  addEventListener('mouseup', e => {
    if (e.button !== 0) return;
    btn.r1 = false;
    if (gate.up({ ...lockState(), x: e.clientX, y: e.clientY, onCanvas: e.target === canvas }) === 'tap' && !isFrozen()) btn.r1Edge = true;
  });
  addEventListener('keydown', e => {
    if (e.repeat || isFrozen() || /^(INPUT|TEXTAREA)$/.test(e.target.tagName)) return;
    if (e.code === 'KeyZ') btn.r3Edge = true;
    if (e.code === 'KeyC') btn.circleEdge = true;
  });
  addEventListener('blur', () => { btn.r1 = false; });

  // FUN_00329778 from this frame's camera (follow.tick ran first in the frame): the ray through the
  // reticle, CAM+0x1cc below the view axis (the reticle lead CAM+0x1fc / +0x200 needs the pad's
  // right stick: 0 on the page, as with the game's USB mouse)
  function aimRay() {
    const O = { x: follow.eye[0], y: follow.eye[1], z: follow.eye[2] };
    const look = { x: follow.at[0], y: follow.at[1], z: follow.at[2] };
    return aimRayPoints(O, reticleRay(follow.M, core.cam.c1cc), look, W.range10,
                        col.segment ? (a, b) => col.segment(a, b) : null);
  }

  // FUN_00577480's bullets start at the muzzle FUN_00341ec8 = marker 15 (bone 56, offset 0:
  // cerberus_web.json) and fly toward P+0x2b0, which FUN_005784d8(P, 1) has just set to this frame's
  // CAM+0xa0; FUN_00563148 pulls the muzzle back using marker 1 (FUN_00342000: bone 5, offset 0;
  // 07703201 marker.mkd). L: the joints' world matrices are the last drawn pose (the render's
  // interpolated one), not the logic frame's bone matrices.
  const joint = name => { let j = null; vin.mixRoot.traverse(n => { if (!j && n.name === name) j = n; }); return j; };
  const j56 = joint('j56'), j05 = joint('j05');
  const v3 = new THREE.Vector3();
  const worldPos = j => { j.updateWorldMatrix(true, false); v3.setFromMatrixPosition(j.matrixWorld); return [v3.x, v3.y, v3.z]; };
  const muzzle = () => j56 ? worldPos(j56) : null;
  const chest = () => j05 ? worldPos(j05) : null;

  let lastRay = null;
  const hook = {
    // the aim ray (after the camera), then the buttons (FUN_003320c0; not during actions)
    pre(input, inAction) {
      lastRay = aimRay();
      core.eye = { x: follow.eye[0], y: follow.eye[1], z: follow.eye[2] }; core.hit = lastRay.hit;
      if (inAction || isFrozen()) { clearEdges(); core.events.length = 0; return; }
      core.input({ r1Pressed: btn.r1Edge, r1Held: btn.r1, r3Pressed: btn.r3Edge, circlePressed: btn.circleEdge },
                 ctrl.onGround, lastRay.far, ctrl);
      btn.r1Edge = btn.r3Edge = btn.circleEdge = false;
    },
    actionEnter: a => ctl.actionEnter(a),
    // FUN_00579570 (every frame, FUN_005786e8: right after FUN_00578748, before the character's
    // motion update). Returns true when a shot was fired this frame (FUN_00577480 -> FUN_00560b58).
    weapon(input, inAction) {
      const before = core.cam.pitch;
      core.stateMachine(lastRay ? lastRay.hit : null);
      if (core.cam.pitch !== before) follow.pitch = core.cam.pitch;   // shot: pitch carry-over (message 0x3e9)
      core.reticleFrame(1000 / 50);                  // GUESS: DAT_004f58ec counts ms (phase 1 = 100 -> 5 frames, traced)
      const shot = core.events.some(e => e.type === 'shot');
      if (shot && api.onShot && lastRay) {          // the bullets (enemy_ai playerShot, hooked by play.js)
        const m = muzzle(), h = lastRay.hit;
        if (m) api.onShot(m, [h.x, h.y, h.z], W, chest());
      }
      return shot;
    },
    // the clip requests of the state classes (after the move)
    post(input, inAction) {
      if (!inAction) ctl.clips();
    },
  };
  ctrl.weapon = core; ctrl.weaponHook = hook;
  if (output) output.hud = o => layer.draw(o.renderer, o, core);

  const api = {
    core, help: HELP,
    onShot: null,                                   // (muzzle, aim, W, chest) per shot: set by play.js (enemy_ai playerShot)
    // H (respawn): the weapon, the latched buttons and the clip classes (the camera resets itself)
    reset() { core.reset(); clearEdges(); ctl.reset(); },
    // test aid (Playwright): press a game button for one frame ('r1', 'r3', 'circle'); 'r1' also holds
    press(b) { if (b === 'r1') { btn.r1 = true; btn.r1Edge = true; setTimeout(() => { btn.r1 = false; }, 60); }
               if (b === 'r3') btn.r3Edge = true; if (b === 'circle') btn.circleEdge = true; },
    // per drawn frame: Vincent's fade (CAM+0x1e0 -> FUN_00342318)
    // GUESS: the fade is drawn as visible / hidden (the 16-frame partial alpha is left out).
    render() { vin.body.visible = core.cam.fade > 0; },
    state: () => ({ st: core.P.st, T: core.P.T, drawn: core.P.drawn, view: core.cam.view, type: core.cam.type,
                    ramp: core.cam.ramp, fade: core.cam.fade, fov: core.cam.fov, offY: core.hud.offY, kind: core.r.kind,
                    phase: core.r.phase.slice(1, 4), aim: core.P.aim, quad: ctl.state().quad,
                    table: anim.table && anim.table.map ? anim.table.map(e => e.key) : null }),
  };
  return api;
}
