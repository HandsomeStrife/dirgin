// Shoot action (prototype, no enemies): aim, fire, bullets, sparks, muzzle flash, gun.
//
// Keys: hold the right mouse button (or Q, for trackpads) to aim; left click / hold fires.
// While aiming he faces the camera, walks with h_wk_a (Shift: h_rn_a) and blends the
// level / up / down aim loops (h_*_a / _au / _ad) by the camera pitch, clamped to ±54°
// (research §4, the normal camera's limit). Fire plays h_st_f / _fu / _fd blended the
// same way (standing only: the pack has no moving fire clips), at most one shot per
// fire-clip length (no game rate found).
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
  };
  let view = null;                          // scene bits, found lazily
  let gunState = 'none';                    // none | loading | ok | failed
  let gun = null;
  let actsBySet = null;                     // { st: {a,au,ad}, wk: ..., rn: ..., fire: {f,fu,fd} }
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
    for (const s of ['st', 'wk', 'rn'])
      actsBySet[s] = { a: mk(`h_${s}_a`, true), au: mk(`h_${s}_au`, true), ad: mk(`h_${s}_ad`, true) };
    actsBySet.fire = { a: mk('h_st_f', false), au: mk('h_st_fu', false), ad: mk('h_st_fd', false) };
    const fm = ctx.motions?.h_st_f;
    S.cooldown = fm ? fm.frames - 1 : 14;   // one shot per fire-clip length (duration = (frames-1)/60)
  }

  const allActs = () => Object.values(actsBySet).flatMap(o => Object.values(o)).filter(Boolean);

  function applyWeights() {
    const b = S.blend, pw = { a: 1 - Math.abs(b), au: Math.max(b, 0), ad: Math.max(-b, 0) };
    for (const s of ['st', 'wk', 'rn']) {
      const set = actsBySet[s], ws = w.in * w.set[s] * (s === 'st' ? 1 - w.fire : 1);
      for (const k of ['a', 'au', 'ad']) if (set[k]) set[k].setEffectiveWeight(ws * pw[k]);
      // keep the three pitch variants in phase
      if (set.a) for (const k of ['au', 'ad']) if (set[k]) set[k].time = set.a.time;
    }
    const f = actsBySet.fire, wf = w.in * w.set.st * w.fire;
    for (const k of ['a', 'au', 'ad']) if (f[k]) { f[k].setEffectiveWeight(wf * pw[k]); if (f.a) f[k].time = f.a.time; }
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
    // fire clip (standing only: there are no h_wk_f / h_rn_f clips)
    if (S.set === 'st') {
      for (const a of Object.values(actsBySet.fire)) if (a) { a.reset(); a.play(); }
      S.firing = S.cooldown;
    }
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
      for (const a of Object.values(actsBySet.fire)) if (a) a.stop();
      w = { in: 0, set: { st: 1, wk: 0, rn: 0 }, fire: 0 };
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
      const target = len > 1e-6 ? (run ? sp('h_rn_a', 1.665) : sp('h_wk_a', 0.7)) : 0;
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

      // animation set by speed; walk/run play backwards when moving backwards
      const moved = Math.hypot(delta.x, delta.z);
      const fwd = delta.x * Math.sin(heading) + delta.z * Math.cos(heading);
      const want = moved < 0.12 * sp('h_wk_a', 0.7) ? 'st' : (run ? 'rn' : 'wk');
      S.set = want;
      const kw = 1 - Math.exp(-dt / 0.1);
      for (const s of ['st', 'wk', 'rn']) w.set[s] += ((s === want ? 1 : 0) - w.set[s]) * kw;
      w.in = Math.min(1, w.in + dt / 0.15);
      if (want !== 'st') {
        const set = actsBySet[want], base = sp(`h_${want}_a`, 0.7);
        const rate = Math.max(0.3, Math.min(1.6, moved / base)) * (fwd < -0.2 * moved ? -1 : 1);
        for (const a of Object.values(set)) if (a) a.setEffectiveTimeScale(rate);
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
