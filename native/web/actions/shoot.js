// Shoot action (prototype, no enemies): aim, fire, bullets, sparks, muzzle flash, gun.
//
// Keys: hold the right mouse button (or Q, for trackpads) to aim; left click / hold fires.
// While aiming he faces the camera and blends the level / up / down aim loops
// (h_*_a / _au / _ad) by the camera pitch, clamped to ±54° (research §4, the normal
// camera's limit). Moving, he blends the aimed movement sets by the move direction
// relative to his heading: forward h_wk_a (Shift: h_rn_a), back h_brn_a, left h_lrn_a,
// right h_rrn_a (the game only has strafe RUNS; walking sideways plays them slower).
// Each set's direction and speed come from its own root motion; the playback rate is
// ground speed / clip root speed, so the planted foot stays put. Backwards he moves at
// most at h_brn's own speed (0.83 u/frame, half the run). Fire plays h_st_f / _fu / _fd
// standing; moving, the same clips play as an ADDITIVE upper-body overlay on the gait
// (no game has moving fire clips for any weapon; whether the game overlays is undecoded),
// at most one shot per fire-clip length (no game rate found).
//
// Bullets are moving projectiles (research §4: type-6 actors sweeping previous ->
// current position each tick). Each 60 Hz tick a bullet moves SPEED units and tests
// col.segment(prev, next); a hit leaves a spark + mark for ~1 s. Speed and range are
// GUESSES (not decoded), tunable via ?bulletSpeed= / ?bulletRange= or window.__shoot.P.
//
// Workarounds for hooks the interface lacks (all inside this module):
//  * mouse buttons are read with this module's own listeners (ctx.input has keys only);
//  * the scene comes from ctx.anim.mixer.getRoot()'s top ancestor (ctx has no scene);
//  * the aim pitch range (±54°) is wider than FollowCamera.look's clamp (-0.35..1.25),
//    so ctx.camera.look is swapped for a wider one while aiming and restored on exit;
//  * the aim blend drives mixer actions directly (Animator.play is one clip at a time);
//  * bullets, sparks and the flash run on their own 60 Hz rAF clock, so shots in flight
//    finish after the aim is released (update() only runs while aiming);
//  * the gun mesh (export_web_gun.py) is loaded on the first canEnter (visual only).
import * as THREE from 'three';

export const HELP = 'RMB/Q aim  LMB fire';

const DEG = Math.PI / 180;
const qp = new URLSearchParams(location.search);
const num = (k, d) => (qp.has(k) && Number.isFinite(+qp.get(k)) ? +qp.get(k) : d);

// tunables (window.__shoot.P)
const P = {
  pitchLimit: 54 * DEG,                 // research §4 (normal camera); assumed for aim
  bulletSpeed: num('bulletSpeed', 15),  // units/tick (900 u/s): GUESS
  bulletRange: num('bulletRange', 1500),// units: GUESS
  markLife: 60,                         // ticks a hit spark/mark stays (~1 s)
  flashTicks: 3,
  cam: { dist: 20, targetHeight: 16.5, shoulder: 6 },  // over-the-shoulder: GUESS
  turnRate: 16,                         // rad/s toward the camera yaw while aiming
  accel: 14,
};

// aimed movement sets (clip prefix), each with _a / _au / _ad pitch variants
const CLIP = { st: 'h_st', wk: 'h_wk', rn: 'h_rn', b: 'h_brn', l: 'h_lrn', r: 'h_rrn' };
const MOVE = ['wk', 'rn', 'b', 'l', 'r'];
const SETS = ['st', ...MOVE];
// phase offsets for syncing the move sets: h_wk_a plants each foot a quarter cycle later
// than the run-type clips (j85 / j89 lowest at 0.80 / 0.30 vs 0.55 / 0.05 in h_rn_a,
// h_lrn_a, h_rrn_a; h_brn_a 0.40 / 0.05)
const PHASE = { wk: 0.25 };
// fire overlay mask: the upper body = bone 2's subtree (j02..j80, vincent.json
// joints.parent; legs are j81..j92) minus the cape-only bones (no body triangle uses them),
// so the legs and cape keep the gait
const CAPE_ONLY = new Set([39, 40, 41, 42, 44, 45, 46, 48, 49, 50, 72, 73, 74, 76, 77, 78, 79]);
const upperBody = j => j >= 2 && j <= 80 && !CAPE_ONLY.has(j);

// additive clip: `fire` minus `ref`'s frame 0 (the aim pose the fire clip settles on),
// upper-body tracks only. Quaternions as ref0^-1 * q, so three.js's current * delta gives
// the fire pose exactly when the current pose is the standing aim pose.
function overlayClip(fire, ref) {
  const tracks = [];
  for (const t of fire.tracks) {
    const m = /^j(\d+)\.(\w+)$/.exec(t.name);
    if (!m || !upperBody(+m[1])) continue;
    const r = ref.tracks.find(u => u.name === t.name);
    if (!r) continue;
    const n = t.getValueSize(), v = t.values.slice(), r0 = Array.from(r.values.slice(0, n));
    if (m[2] === 'quaternion') {
      const q0 = new THREE.Quaternion().fromArray(r0).invert(), q = new THREE.Quaternion();
      for (let i = 0; i < v.length; i += 4) q.fromArray(v, i).premultiply(q0).toArray(v, i);
    } else for (let i = 0; i < v.length; i++) v[i] -= r0[i % n];
    const c = t.clone(); c.values = v; tracks.push(c);
  }
  return new THREE.AnimationClip(fire.name + '_ovl', fire.duration, tracks, THREE.AdditiveAnimationBlendMode);
}

// ------------------------------------------------------------ mouse (own listeners)
const mouse = { right: false, left: false, leftEdge: false };
if (typeof window !== 'undefined') {
  addEventListener('mousedown', e => {
    // Only presses on the game canvas count, so clicks in the Report dialog or HUD never
    // aim or fire (and a context menu there can't swallow the mouseup).
    if (!(e.target && e.target.tagName === 'CANVAS')) return;
    if (e.button === 2) mouse.right = true;
    if (e.button === 0) { mouse.left = true; mouse.leftEdge = true; }
  });
  addEventListener('mouseup', e => {
    if (e.button === 2) mouse.right = false;
    if (e.button === 0) mouse.left = false;
  });
  addEventListener('contextmenu', e => { if (e.target && e.target.tagName === 'CANVAS') e.preventDefault(); });
  addEventListener('blur', () => { mouse.right = mouse.left = mouse.leftEdge = false; });
}

// ------------------------------------------------------------ small visuals
function glowTexture() {
  const c = document.createElement('canvas'); c.width = c.height = 64;
  const g = c.getContext('2d'), r = g.createRadialGradient(32, 32, 0, 32, 32, 32);
  r.addColorStop(0, 'rgba(255,255,230,1)'); r.addColorStop(0.25, 'rgba(255,210,120,0.9)');
  r.addColorStop(0.6, 'rgba(255,120,30,0.3)'); r.addColorStop(1, 'rgba(255,80,0,0)');
  g.fillStyle = r; g.fillRect(0, 0, 64, 64);
  const t = new THREE.CanvasTexture(c); t.colorSpace = THREE.NoColorSpace; return t;
}

function crosshair() {
  const d = document.createElement('div');
  d.id = 'shoot-crosshair';
  d.style.cssText = 'position:fixed;left:50%;top:50%;width:18px;height:18px;margin:-9px 0 0 -9px;' +
    'pointer-events:none;display:none;z-index:5;';
  d.innerHTML = '<svg width="18" height="18" viewBox="0 0 18 18" fill="none" stroke="#fff" stroke-width="1.5" ' +
    'style="filter:drop-shadow(0 0 1px #000)"><path d="M9 1v5M9 12v5M1 9h5M12 9h5"/><circle cx="9" cy="9" r="0.8" fill="#fff"/></svg>';
  document.body.appendChild(d);
  return d;
}

// ------------------------------------------------------------ gun (static, under j56)
async function loadGun() {
  const base = 'data/vincent/';
  const meta = await (await fetch(base + 'cerberus_web.json')).json();
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
  const m = new THREE.MeshBasicMaterial({ map: tex, vertexColors: true, side: THREE.DoubleSide });
  const mesh = new THREE.Mesh(g, m);
  mesh.name = 'cerberus'; mesh.frustumCulled = false; mesh.renderOrder = 1100;
  // muzzle: centre of the vertices at the barrel's far end (gun forward = -z)
  let minZ = Infinity; for (let i = 2; i < f.length; i += meta.stride) minZ = Math.min(minZ, f[i]);
  const mz = new THREE.Vector3(); let n = 0;
  for (let i = 0; i < f.length; i += meta.stride) if (f[i + 2] < minZ + 0.3) { mz.x += f[i]; mz.y += f[i + 1]; mz.z += f[i + 2]; n++; }
  return { mesh, muzzle: mz.multiplyScalar(1 / Math.max(1, n)) };
}

// ------------------------------------------------------------ the action
export function createAction() {
  const S = {                               // state, exposed for tests (window.__shoot)
    aiming: false, pitch: 0, blend: 0, set: 'st', shots: 0, lastShotTick: -1e9,
    cooldown: 14, firing: 0, bullets: [], marks: [], log: [], fx: 0,
    dirW: {}, rate: 1, moved: 0,            // direction weights, lead set's rate, ground step
  };
  let view = null;                          // scene bits, found lazily
  let gunState = 'none';                    // none | loading | ok | failed
  let gun = null;
  let actsBySet = null;                     // { st|wk|rn|b|l|r: {a,au,ad}, fire, ovl: {a,au,ad} }
  let setInfo = null;                       // per move set: { clip, frames, speed, dur }
  let w = null;                             // eased weights
  let savedLook = null;
  const cross = typeof document !== 'undefined' ? crosshair() : null;
  const tmpV = new THREE.Vector3();

  function scene(ctx) {
    if (view) return view;
    let o = ctx.anim?.mixer?.getRoot?.();
    if (!o || !o.parent) return null;       // not in the scene yet
    const mixRoot = o;
    while (o.parent) o = o.parent;
    let j56 = null; mixRoot.traverse(n => { if (!j56 && n.name === 'j56') j56 = n; });
    const tex = glowTexture();
    const add = obj => { o.add(obj); return obj; };
    const flash = add(new THREE.Sprite(new THREE.SpriteMaterial({ map: tex, blending: THREE.AdditiveBlending,
      transparent: true, depthWrite: false, color: 0xffd9a0 })));
    flash.visible = false; flash.renderOrder = 2000;
    view = { root: o, mixRoot, j56, tex, flash, camera: ctx.camera?.camera };
    startFx();
    return view;
  }

  function ensureGun(ctx) {
    if (gunState !== 'none') return;
    const v = scene(ctx); if (!v || !v.j56) return;
    if (qp.get('gun') === '0') { gunState = 'off'; return; }
    gunState = 'loading';
    loadGun().then(g => { gun = g; v.j56.add(g.mesh); gunState = 'ok'; })
      .catch(e => { gunState = 'failed'; console.info('shoot: gun not shown:', e.message); });
  }

  // muzzle in world space (falls back to the hand joint when the gun isn't loaded)
  function muzzleWorld(out) {
    const v = view;
    if (!v?.j56) return null;
    v.j56.updateWorldMatrix(true, false);
    out.copy(gun ? gun.muzzle : tmpV.set(0, -1.5, -4.6)).applyMatrix4(v.j56.matrixWorld);
    return out;
  }

  // ---- animation: three aim sets + fire, weights driven here
  function initActs(ctx) {
    const mixer = ctx.anim.mixer, lib = ctx.anim.library;
    const mk = (name, loop) => {
      const c = lib.get(name); if (!c) return null;
      const a = mixer.clipAction(c);
      a.setLoop(loop ? THREE.LoopRepeat : THREE.LoopOnce, loop ? Infinity : 1);
      a.clampWhenFinished = !loop;
      return a;
    };
    actsBySet = {};
    for (const s of SETS)
      actsBySet[s] = { a: mk(`${CLIP[s]}_a`, true), au: mk(`${CLIP[s]}_au`, true), ad: mk(`${CLIP[s]}_ad`, true) };
    actsBySet.fire = { a: mk('h_st_f', false), au: mk('h_st_fu', false), ad: mk('h_st_fd', false) };
    actsBySet.ovl = {};
    for (const [k, f, r] of [['a', 'h_st_f', 'h_st_a'], ['au', 'h_st_fu', 'h_st_au'], ['ad', 'h_st_fd', 'h_st_ad']]) {
      const fc = lib.get(f), rc = lib.get(r);
      if (!fc || !rc) { actsBySet.ovl[k] = null; continue; }
      const a = mixer.clipAction(overlayClip(fc, rc));
      a.setLoop(THREE.LoopOnce, 1); a.clampWhenFinished = true;
      actsBySet.ovl[k] = a;
    }
    // direction + speed of each move set from its own root motion. h_lrn_a has no root
    // track (its _au/_ad and the mirror h_rrn_a move 1.665 u/frame), so take the first
    // variant that moves.
    setInfo = {};
    for (const s of MOVE) {
      const n = ['_a', '_ad', '_au'].map(x => CLIP[s] + x)
        .find(x => actsBySet[s][x.slice(CLIP[s].length + 1)] && ctx.motions?.[x]?.speed_units_per_frame > 1e-3);
      if (!n) continue;
      const dur = actsBySet[s][n.slice(CLIP[s].length + 1)].getClip().duration;
      const speed = ctx.motions[n].speed_units_per_frame;
      setInfo[s] = { clip: n, frames: ctx.motions[n].frames, speed, dur, stride: speed * dur * 60 };  // units/cycle
    }
    const fm = ctx.motions?.h_st_f;
    S.cooldown = fm ? fm.frames - 1 : 14;   // one shot per fire-clip length (duration = (frames-1)/60)
  }

  const allActs = () => Object.values(actsBySet).flatMap(o => Object.values(o)).filter(Boolean);

  function applyWeights() {
    const b = S.blend, pw = { a: 1 - Math.abs(b), au: Math.max(b, 0), ad: Math.max(-b, 0) };
    // every move loop follows the heaviest set's normalised phase (same foot, same time)
    let lead = null, lw = 0;
    for (const s of MOVE) if (actsBySet[s].a && w.set[s] > lw) { lw = w.set[s]; lead = actsBySet[s].a; }
    let ph = 0;
    if (lead) for (const s of MOVE) if (actsBySet[s].a === lead) ph = lead.time / lead.getClip().duration - (PHASE[s] || 0);
    for (const s of SETS) {
      const set = actsBySet[s], ws = w.in * w.set[s] * (s === 'st' ? 1 - w.fire : 1);
      for (const k of ['a', 'au', 'ad']) if (set[k]) set[k].setEffectiveWeight(ws * pw[k]);
      // keep the three pitch variants (and the move sets) in phase
      if (s === 'st') { if (set.a) for (const k of ['au', 'ad']) if (set[k]) set[k].time = set.a.time; }
      else if (lead) {
        const p = ph + (PHASE[s] || 0), f = p - Math.floor(p);
        for (const k of ['a', 'au', 'ad']) if (set[k] && set[k] !== lead) set[k].time = f * set[k].getClip().duration;
      }
    }
    // fire: full-body clips standing, the additive upper-body overlay on the move sets
    const f = actsBySet.fire, o = actsBySet.ovl, wf = w.in * w.fire;
    for (const k of ['a', 'au', 'ad']) {
      if (f[k]) { f[k].setEffectiveWeight(wf * w.set.st * pw[k]); if (f.a) f[k].time = f.a.time; }
      if (o[k]) { o[k].setEffectiveWeight(wf * (1 - w.set.st) * pw[k]); if (f.a) o[k].time = f.a.time; }
    }
  }

  // ---- effects clock (bullets, marks, flash): own fixed 60 Hz
  let fxRunning = false, fxLast = 0, fxAcc = 0;
  function startFx() {
    if (fxRunning) return; fxRunning = true; fxLast = performance.now();
    const loop = now => {
      requestAnimationFrame(loop);
      fxAcc += Math.min(0.25, (now - fxLast) / 1000); fxLast = now;
      let n = 0;
      while (fxAcc >= 1 / 60 && n < 8) { fxTick(); fxAcc -= 1 / 60; n++; }
      if (n === 8) fxAcc = 0;
    };
    requestAnimationFrame(loop);
  }

  let colRef = null;
  function fxTick() {
    S.fx++;
    const v = view;
    if (v.flashT > 0 && --v.flashT === 0) v.flash.visible = false;
    for (let i = S.bullets.length - 1; i >= 0; i--) {
      const b = S.bullets[i];
      const next = b.pos.clone().addScaledVector(b.dir, P.bulletSpeed);
      const hit = colRef.segment(b.pos, next);
      b.age++;
      if (hit) {
        const p = new THREE.Vector3(...(hit.point.toArray ? hit.point.toArray() : [hit.point.x, hit.point.y, hit.point.z]));
        addMark(p, hit.normal, b);
        S.log.push({ ev: 'hit', id: b.id, fx: S.fx, point: p.toArray(), travelled: b.travelled + hit.t * P.bulletSpeed });
        killBullet(i); continue;
      }
      b.travelled += P.bulletSpeed;
      b.prev = b.pos; b.pos = next;
      b.line.geometry.attributes.position.setXYZ(0, ...next.clone().addScaledVector(b.dir, -Math.min(8, b.travelled)).toArray());
      b.line.geometry.attributes.position.setXYZ(1, next.x, next.y, next.z);
      b.line.geometry.attributes.position.needsUpdate = true;
      if (b.travelled >= P.bulletRange) { S.log.push({ ev: 'expire', id: b.id, fx: S.fx }); killBullet(i); }
    }
    for (let i = S.marks.length - 1; i >= 0; i--) {
      const m = S.marks[i]; m.age++;
      const k = 1 - m.age / P.markLife;
      m.spark.material.opacity = Math.max(0, 1 - m.age / 12);
      m.spark.scale.setScalar(2.5 + m.age * 0.15);
      m.mark.material.opacity = Math.max(0, Math.min(1, k * 2)) * 0.85;
      if (m.age >= P.markLife) {
        v.root.remove(m.spark); v.root.remove(m.mark);
        m.spark.material.dispose(); m.mark.material.dispose(); m.mark.geometry.dispose();
        S.marks.splice(i, 1);
      }
    }
  }

  function killBullet(i) {
    const b = S.bullets[i];
    view.root.remove(b.line); b.line.geometry.dispose(); b.line.material.dispose();
    S.bullets.splice(i, 1);
  }

  function addMark(p, normal, b) {
    const n = new THREE.Vector3(...(Array.isArray(normal) ? normal : [normal.x, normal.y, normal.z])).normalize();
    if (n.dot(b.dir) > 0) n.negate();          // two-sided faces: face the shooter
    const spark = new THREE.Sprite(new THREE.SpriteMaterial({ map: view.tex, blending: THREE.AdditiveBlending,
      transparent: true, depthWrite: false, color: 0xffe0a0 }));
    spark.position.copy(p).addScaledVector(n, 0.3); spark.scale.setScalar(2.5); spark.renderOrder = 2000;
    const mark = new THREE.Mesh(new THREE.CircleGeometry(0.6, 10), new THREE.MeshBasicMaterial({
      color: 0x0a0806, transparent: true, opacity: 0.85, depthWrite: false, side: THREE.DoubleSide,
      polygonOffset: true, polygonOffsetFactor: -2, polygonOffsetUnits: -2 }));
    mark.position.copy(p).addScaledVector(n, 0.05);
    mark.lookAt(p.clone().add(n)); mark.renderOrder = 1999;
    view.root.add(spark); view.root.add(mark);
    S.marks.push({ spark, mark, age: 0, pos: p.clone(), bullet: b.id });
  }

  // aim target: what the crosshair (camera centre ray) points at
  function aimTarget(cam) {
    cam.updateMatrixWorld();
    const o = cam.getWorldPosition(new THREE.Vector3()), d = cam.getWorldDirection(new THREE.Vector3());
    const step = 100;
    for (let s = 0; s < P.bulletRange + 60; s += step) {
      const a = o.clone().addScaledVector(d, s), b = o.clone().addScaledVector(d, s + step);
      const h = colRef.segment(a, b);
      if (h) return o.clone().addScaledVector(d, s + h.t * step);
    }
    return o.clone().addScaledVector(d, P.bulletRange + 60);
  }

  function fire(ctx) {
    const v = view, m = muzzleWorld(new THREE.Vector3());
    if (!m) return;
    const target = aimTarget(v.camera);
    const dir = target.clone().sub(m);
    if (dir.length() < 1e-3) return;
    dir.normalize();
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.Float32BufferAttribute([m.x, m.y, m.z, m.x, m.y, m.z], 3));
    const line = new THREE.Line(geo, new THREE.LineBasicMaterial({ color: 0xffd080, transparent: true,
      blending: THREE.AdditiveBlending, depthWrite: false }));
    line.frustumCulled = false; line.renderOrder = 2000;
    v.root.add(line);
    const id = ++S.shots;
    S.bullets.push({ id, pos: m.clone(), prev: m.clone(), dir, travelled: 0, age: 0, line });
    S.log.push({ ev: 'fire', id, tick: ctx.tick, muzzle: m.toArray(), dir: dir.toArray(), target: target.toArray() });
    v.flash.position.copy(m).addScaledVector(dir, 0.6);
    v.flash.scale.setScalar(3 + Math.random() * 1.5);
    v.flash.material.rotation = Math.random() * Math.PI;
    v.flash.visible = true; v.flashT = P.flashTicks;
    S.lastShotTick = ctx.tick;
    // fire clip: full body standing, upper-body overlay while moving (weights in applyWeights)
    for (const a of [...Object.values(actsBySet.fire), ...Object.values(actsBySet.ovl)]) if (a) { a.reset(); a.play(); }
    S.firing = S.cooldown;
  }

  const held = ctx => mouse.right || ctx.input?.keys?.has('KeyQ');

  const action = {
    name: 'shoot',
    canEnter(ctx) {
      ensureGun(ctx);                         // visual only (see the header)
      if (!held(ctx)) { mouse.leftEdge = false; return false; }
      return ctx.ctrl.onGround && !!ctx.anim && ['h_st_a', 'h_st_au', 'h_st_ad', 'h_st_f'].every(n => ctx.anim.has(n));
    },
    enter(ctx) {
      if (S.lastShotTick > ctx.tick) S.lastShotTick = -1e9;   // tick counter was reset (H respawn)
      const v = scene(ctx);
      colRef = ctx.col;
      if (!actsBySet) initActs(ctx);
      for (const a of allActs()) { a.reset(); a.setEffectiveWeight(0); a.play(); }
      for (const a of [...Object.values(actsBySet.fire), ...Object.values(actsBySet.ovl)]) if (a) a.stop();
      w = { in: 0, set: Object.fromEntries(SETS.map(s => [s, s === 'st' ? 1 : 0])), fire: 0 };
      S.aiming = true; S.set = 'st'; S.firing = 0;
      mouse.leftEdge = false;
      // aim camera + the wider pitch range
      const cam = ctx.camera;
      if (cam) {
        cam.setMode('aim', P.cam);
        savedLook = Object.hasOwn(cam, 'look') ? cam.look : null;
        cam.look = function (dx, dy) {
          this.yaw += dx * this.sens;
          this.pitch = Math.max(-P.pitchLimit, Math.min(P.pitchLimit, this.pitch + dy * this.sens));
        };
        cam.pitch = Math.max(-P.pitchLimit, Math.min(P.pitchLimit, cam.pitch));
      }
      if (cross) cross.style.display = 'block';
      if (v) v.flash.visible = false;
      // h_st_a as the Animator override (fades the gait out); weights are ours from here
      return { clip: 'h_st_a', loop: true, rate: 1, fade: 0.15 };
    },
    update(dt, ctx) {
      const c = ctx.ctrl, cam = ctx.camera, inp = ctx.input;
      if (!held(ctx) || !c.onGround) return { done: true, keepVel: true };

      // pitch: camera pitch is "above horizontal" for the eye, so aim up = -pitch
      S.pitch = cam ? Math.max(-P.pitchLimit, Math.min(P.pitchLimit, -cam.pitch)) : 0;
      S.blend = S.pitch / P.pitchLimit;

      // movement: camera-relative input, body faces the view
      const len = Math.hypot(inp.x, inp.z);
      const run = inp.run && !!actsBySet.rn.a;
      const sp = (n, d) => (ctx.motions[n]?.speed_units_per_frame ?? d) * c.scale;
      const fwdSet = run ? 'rn' : 'wk';
      // direction weights, velocity-matched: each set's own root-motion direction u (game
      // space, current heading) and stride S (units per cycle); w ~ cos(angle) / S, so with
      // the sets sharing one phase the blended foot velocity sum(w S u) points along the
      // move. Pure W/A/S/D is one clip; a walk/strafe diagonal is ~62/38. out.stride =
      // |sum(w S u)|, the blend's stride along the move (world units per cycle).
      const dirWeights = (dx, dz) => {
        const out = {}, l = Math.hypot(dx, dz); let tot = 0;
        if (l < 1e-6) return out;
        const u = {};
        for (const s of [fwdSet, 'b', 'l', 'r']) {
          const si = setInfo[s]; if (!si) continue;
          const d = ctx.rootDelta(si.clip, 0, si.frames - 1), dl = Math.hypot(d.x, d.z);
          if (dl < 1e-6) continue;
          const cs = (dx * d.x + dz * d.z) / (l * dl);
          if (cs > 1e-3) { out[s] = cs / si.stride; tot += out[s]; u[s] = [d.x / dl, d.z / dl]; }
        }
        let vx = 0, vz = 0;
        for (const s in out) { out[s] /= tot; vx += out[s] * setInfo[s].stride * u[s][0]; vz += out[s] * setInfo[s].stride * u[s][1]; }
        if (tot) Object.defineProperty(out, 'stride', { value: Math.hypot(vx, vz) * c.scale });
        return out;
      };
      const base = run ? sp('h_rn_a', 1.665) : sp('h_wk_a', 0.7);
      const wIn = dirWeights(inp.x, inp.z);
      // backwards: no faster than h_brn's own speed (the game's backpedal is half a run)
      const backCap = setInfo.b ? Math.min(base, setInfo.b.speed * c.scale) : base;
      const target = len > 1e-6 ? base * (1 - (wIn.b || 0)) + backCap * (wIn.b || 0) : 0;
      const k = 1 - Math.exp(-P.accel * dt);
      c.vel.x += ((len > 1e-6 ? inp.x / len * target : 0) - c.vel.x) * k;
      c.vel.z += ((len > 1e-6 ? inp.z / len * target : 0) - c.vel.z) * k;
      if (target === 0 && Math.hypot(c.vel.x, c.vel.z) < 0.01) c.vel.x = c.vel.z = 0;
      let delta = { x: 0, y: 0, z: 0 }, onGround = true;
      if (c.vel.x || c.vel.z) {
        const r = ctx.col.move(c.pos, { x: c.vel.x, z: c.vel.z }, c.opts);
        if (!r.onGround) onGround = false;
        else delta = { x: r.x - c.pos.x, y: r.y - c.pos.y, z: r.z - c.pos.z };
      }
      const yaw = cam ? cam.yaw : c.heading;
      const dh = Math.atan2(Math.sin(yaw - c.heading), Math.cos(yaw - c.heading));
      const mx = P.turnRate * dt, heading = c.heading + Math.max(-mx, Math.min(mx, dh));

      // animation: blend the move sets by the direction he actually moved (walls included)
      const moved = Math.hypot(delta.x, delta.z);
      const moving = moved >= 0.12 * sp('h_wk_a', 0.7);
      let dw = moving ? dirWeights(delta.x, delta.z) : {};
      let reverse = false;
      if (moving && !Object.keys(dw).length) { dw = { [fwdSet]: 1 }; reverse = true; }  // no b/l/r clips loaded
      let lead = 'st', lw = 0;
      for (const s in dw) if (dw[s] > lw) { lw = dw[s]; lead = s; }
      S.set = lead; S.dirW = dw; S.moved = moved;
      const kw = 1 - Math.exp(-dt / 0.1);
      for (const s of SETS) w.set[s] += ((s === 'st' ? (moving ? 0 : 1) : (dw[s] || 0)) - w.set[s]) * kw;
      w.in = Math.min(1, w.in + dt / 0.15);
      if (moving) {
        // cycles per second = ground speed / the blend's stride (rate 1 = the clip's own speed)
        const fb = setInfo[lead] || { stride: (lead === 'rn' ? 1.665 : 0.7) * actsBySet[lead].a.getClip().duration * 60,
                                      dur: actsBySet[lead].a.getClip().duration };
        const stride = dw.stride || fb.stride * c.scale;
        const leadDur = setInfo[lead]?.dur ?? fb.dur;
        const r = Math.max(0.25, Math.min(2, moved * 60 * leadDur / stride));   // lead set's rate
        const cyc = r / leadDur;
        for (const s of MOVE) for (const a of Object.values(actsBySet[s])) if (a) a.setEffectiveTimeScale(cyc * a.getClip().duration * (reverse ? -1 : 1));
        S.rate = cyc * (setInfo[lead]?.dur ?? 1);
      }

      // fire
      if (S.firing > 0) S.firing--;
      const wantFire = mouse.leftEdge || mouse.left;
      mouse.leftEdge = false;
      if (S.lastShotTick > ctx.tick) S.lastShotTick = -1e9;   // reset while aiming
      if (wantFire && ctx.tick - S.lastShotTick >= S.cooldown && w.in > 0.5) fire(ctx);
      w.fire += ((S.firing > 2 ? 1 : 0) - w.fire) * (1 - Math.exp(-dt / 0.04));
      applyWeights();

      if (!onGround) return { done: true, onGround: false, vy: 0, keepVel: true, heading };
      return { delta, heading, keepVel: true };
    },
    exit(ctx) {
      S.aiming = false;
      if (cross) cross.style.display = 'none';
      const cam = ctx.camera;
      if (cam) {
        if (savedLook) cam.look = savedLook; else delete cam.look;
        savedLook = null;
        cam.pitch = Math.max(-0.35, Math.min(1.25, cam.pitch));   // FollowCamera.look's range
        cam.clearMode();
      }
      // hand back to the speed blend; every aim/fire action fades from ITS weight
      const before = new Map(allActs().map(a => [a, a.getEffectiveWeight()]));
      ctx.anim.release(0.2);
      for (const [a, wt] of before) {
        if (a === ctx.anim.actions[ctx.anim.current]) continue;
        if (wt > 1e-4) a._scheduleFading(0.2, wt, 0); else a.stop();
      }
    },
  };
  if (typeof window !== 'undefined') window.__shoot = { P, S, mouse, action, get gun() { return gunState; } };
  return action;
}
