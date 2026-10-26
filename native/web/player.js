// Reusable third-person pieces for the Dirge web build: a fixed-tick character
// controller with pluggable actions, a speed-driven animation blender that actions
// can override, and a collision-aware follow camera with mode hooks.
//
// Game space throughout: -Y is up, so "down" is +y and the head sits at a smaller y
// than the feet. Headings use main.js's yaw convention: dir = (sin h, 0, cos h).
//
// ================================================================ ACTION INTERFACE
// (Also in native/web/ACTIONS.md, with a worked example: actions/reload.js.)
//
// PlayerController is a small state machine: LOCOMOTION (idle / walk / run / fall,
// the original behaviour) plus at most one ACTIVE ACTION from a registry. Register
// actions with ctrl.addAction(mod); registry order is priority (first wins).
//
// An action module is a plain object:
//   name            string, unique ('jump', 'ladder', 'shoot', 'reload', ...)
//   canEnter(ctx)   -> bool. Asked every tick, in registry order, while no action is
//                      active (locomotion running). Must not change anything.
//   enter(ctx)      -> optional ANIMATION REQUEST { clip, loop, rate, fade, onEnd }
//                      (see Animator.play). Called once when canEnter said yes.
//   update(dt, ctx) -> a MOVE RESULT, once per 60 Hz tick (dt = TICK) while active:
//        { locomotion: true }       locomotion moves him this tick (action only animates)
//        { delta: {x,y,z}, heading?, onGround?, vy? }
//                                   the action OWNS movement: delta is applied as is
//                                   (game units, -Y up); the action does its own
//                                   collision through ctx.col. Omitted fields keep.
//        { done: true, ... }        leave after this tick (combine with either above)
//        null / undefined           same as { } = stand still, no movement
//   exit(ctx)       called once after done (or ctrl.cancelAction()). Release the
//                   animation (ctx.anim.release()) and camera mode here.
//
// ctx (the same object every tick; read-only unless noted):
//   ctx.ctrl     the controller: pos {x,y,z} (feet), heading, vel {x,z}, vy, onGround,
//                speed (measured units/tick), opts {radius,height,maxDrop}, walkSpeed,
//                runSpeed, gravity, spawn. Actions may write pos/vel/vy/onGround.
//   ctx.input    this tick's input: { x, z, run, keys: Set of held codes,
//                pressed: Set of codes pressed since the last tick (edge) }
//   ctx.col      collision (collision.js API: groundY, groundHit, move, capsulePush, segment)
//   ctx.anim     Animator: play(name, {loop, rate, fade, onEnd}), release(fade),
//                setRate(r), has(name), time(), duration(name), current
//   ctx.camera   FollowCamera: setMode(name, {dist, targetHeight, shoulder}), clearMode()
//   ctx.motions  clip name -> sidecar entry (vincent.json / vincent_<pack>.json clips:
//                frames, loop, in_place_axes, root_motion_per_frame_glb, ...)
//   ctx.rootDelta(name, f0, f1)  the clip's root motion from frame f0 to f1 (fractional
//                frames OK), turned into game space for the current heading and scale
//   ctx.tick     ticks since reset;  ctx.TICK  the step (s)
//
// With no actions registered every code path is the original one.
// ================================================================
import * as THREE from 'three';

export const TICK = 1 / 60;                       // simulation step (s)

const wrapPi = a => Math.atan2(Math.sin(a), Math.cos(a));

// glb root motion (Y up, +Z = his forward, +X = his left) -> game space (-Y up) for
// heading h (forward = (sin h, cos h); right = (cos h, -sin h), main.js) at scale s.
export function glbToGame(v, h, s = 1) {
  const fx = Math.sin(h), fz = Math.cos(h), rx = Math.cos(h), rz = -Math.sin(h);
  return { x: s * (v[2] * fx - v[0] * rx), y: -s * v[1], z: s * (v[2] * fz - v[0] * rz) };
}

// ---------------------------------------------------------------- controller
// Speeds are world units per tick (one 60 Hz game frame).
export class PlayerController {
  // Falling (not decoded from the game; a plain stand-in): when the floor is more
  // than snapDown below the feet, or missing, he leaves the ground and falls with
  // `gravity` (units/tick^2, +y is down) capped at maxFall, landing on the first
  // floor his feet pass. killDepth below the spawn (+y) respawns him.
  constructor(col, { pos, heading = 0, walkSpeed, runSpeed, radius = 3.5, height = 19,
                     accel = 14, turnRate = 11, snapDown = 4, gravity = 0.05, maxFall = 6,
                     killDepth = 500, scale = 1 } = {}) {
    this.col = col;
    this.opts = { radius, height, maxDrop: snapDown };
    this.gravity = gravity; this.maxFall = maxFall; this.killDepth = killDepth;
    this.walkSpeed = walkSpeed; this.runSpeed = runSpeed;
    this.accel = accel;                           // 1/s: velocity eases toward the target
    this.turnRate = turnRate;                     // rad/s: max turn speed of the body
    this.scale = scale;                           // character scale (root motion -> world)
    this.spawn = { ...pos, heading };
    this.actions = [];                            // registry, priority order
    this.active = null;                           // the running action, or null = locomotion
    this.ctx = { ctrl: this, col, input: null, anim: null, camera: null, motions: {},
                 tick: 0, TICK, rootDelta: (n, f0, f1) => this.rootDelta(n, f0, f1) };
    this.reset();
  }

  // ctx pieces the controller doesn't own (play.js wires them)
  attach({ anim, camera, motions } = {}) {
    if (anim) this.ctx.anim = anim;
    if (camera) this.ctx.camera = camera;
    if (motions) this.ctx.motions = motions;
  }

  addAction(mod) {
    for (const k of ['name', 'canEnter', 'enter', 'update', 'exit'])
      if (!(k in mod)) throw new Error(`action ${mod.name || '?'}: missing ${k}`);
    this.actions.push(mod);
    return mod;
  }

  cancelAction() {
    if (!this.active) return;
    const a = this.active; this.active = null;
    a.exit(this.ctx);
  }

  reset() {
    if (this.active) this.cancelAction();
    const s = this.spawn;
    this.pos = { x: s.x, y: s.y, z: s.z };
    this.prev = { ...this.pos };
    this.heading = this.prevHeading = s.heading;
    this.vel = { x: 0, z: 0 };                    // intended horizontal velocity (units/tick)
    this.speed = 0;                               // measured ground speed (units/tick), smoothed
    this.onGround = true; this.hitWall = false; this.stuck = false;
    this.vy = 0;                                  // vertical speed while falling (units/tick, +y down)
    this.ticks = 0;
  }

  // root motion of clip `name` between clip frames f0 and f1, game space
  rootDelta(name, f0, f1) {
    const m = this.ctx.motions[name];
    if (!m) return { x: 0, y: 0, z: 0 };
    const r = m.root_motion_per_frame_glb, n = r.length;
    const at = f => {
      const c = Math.max(0, Math.min(n - 1, f)), i = Math.min(Math.floor(c), n - 2), t = c - i;
      if (n === 1) return r[0];
      return [0, 1, 2].map(k => r[i][k] + (r[i + 1][k] - r[i][k]) * t);
    };
    const a = at(f0), b = at(f1);
    return glbToGame([b[0] - a[0], b[1] - a[1], b[2] - a[2]], this.heading, this.scale);
  }

  // input: { x, z } desired world direction (any length, 0 = no input), run: bool,
  // optional keys / pressed Sets for actions
  step(input) {
    this.prev = { ...this.pos };
    this.prevHeading = this.heading;
    const ctx = this.ctx;
    ctx.input = input; ctx.tick = this.ticks;

    if (!this.active) {
      for (const a of this.actions) {
        if (!a.canEnter(ctx)) continue;
        this.active = a;
        const req = a.enter(ctx);
        if (req && req.clip && ctx.anim) ctx.anim.play(req.clip, req);
        break;
      }
    }
    if (this.active) {
      const a = this.active;
      const res = a.update(TICK, ctx) || {};
      if (res.locomotion) this.locomotion(input);
      else this.applyOwned(res);
      if (res.done && this.active === a) { this.active = null; a.exit(ctx); }
    } else {
      this.locomotion(input);
    }
    this.ticks++;
  }

  // an action moved him: apply its result, keep `speed` meaningful for the animator
  applyOwned(res) {
    const d = res.delta || { x: 0, y: 0, z: 0 };
    const nx = this.pos.x + (d.x || 0), ny = this.pos.y + (d.y || 0), nz = this.pos.z + (d.z || 0);
    const moved = Math.hypot(nx - this.pos.x, nz - this.pos.z);
    this.pos = { x: nx, y: ny, z: nz };
    if (res.heading !== undefined) this.heading = wrapPi(res.heading);
    if (res.onGround !== undefined) this.onGround = res.onGround;
    if (res.vy !== undefined) this.vy = res.vy;
    if (!res.keepVel) { this.vel.x = 0; this.vel.z = 0; }
    this.hitWall = false; this.stuck = false;
    this.speed += (moved - this.speed) * (1 - Math.exp(-20 * TICK));
  }

  // the original controller tick: idle / walk / run / fall
  locomotion(input) {
    const len = Math.hypot(input.x, input.z);
    const target = len > 1e-6 ? (input.run ? this.runSpeed : this.walkSpeed) : 0;
    const tx = len > 1e-6 ? input.x / len * target : 0;
    const tz = len > 1e-6 ? input.z / len * target : 0;
    const k = 1 - Math.exp(-this.accel * TICK);
    this.vel.x += (tx - this.vel.x) * k;
    this.vel.z += (tz - this.vel.z) * k;
    if (target === 0 && Math.hypot(this.vel.x, this.vel.z) < 0.01) this.vel.x = this.vel.z = 0;

    let moved = 0;
    if (!this.onGround) {
      moved = this.fall();
    } else if (this.vel.x || this.vel.z) {
      const r = this.col.move(this.pos, { x: this.vel.x, z: this.vel.z }, this.opts);
      if (!r.onGround) {                          // walked off an edge: start falling
        this.onGround = false; this.vy = 0;
        moved = this.fall();
      } else {
        this.stuck = r.x === this.pos.x && r.z === this.pos.z;
        moved = Math.hypot(r.x - this.pos.x, r.z - this.pos.z);
        this.pos = { x: r.x, y: r.y, z: r.z };
        this.onGround = true; this.hitWall = r.hitWall;
      }
      // pressing into a wall: don't keep building velocity into it
      if (r.hitWall && moved < 0.5 * Math.hypot(this.vel.x, this.vel.z)) {
        const f = moved / Math.max(1e-6, Math.hypot(this.vel.x, this.vel.z));
        this.vel.x *= Math.max(f, 0.5); this.vel.z *= Math.max(f, 0.5);
      }
    } else {
      this.hitWall = false; this.stuck = false;
      // standing still: the floor can still be gone (spawned in the air)
      const g = this.col.groundY(this.pos.x, this.pos.z, this.pos.y);
      if (g === null || g - this.pos.y > this.opts.maxDrop) { this.onGround = false; this.vy = 0; moved = this.fall(); }
    }
    // measured ground speed, lightly smoothed so wall scrapes don't flicker the clip
    this.speed += (moved - this.speed) * (1 - Math.exp(-20 * TICK));

    // turn toward the movement direction (input direction when blocked)
    const fx = len > 1e-6 ? input.x : this.vel.x, fz = len > 1e-6 ? input.z : this.vel.z;
    if (Math.hypot(fx, fz) > 1e-4) {
      const want = Math.atan2(fx, fz);
      const d = wrapPi(want - this.heading);
      const max = this.turnRate * TICK;
      this.heading = wrapPi(this.heading + Math.max(-max, Math.min(max, d)));
    }
  }

  // One airborne tick: horizontal move pushed out of walls, then gravity; land on
  // the first floor at or below the feet before this tick. Returns the horizontal distance.
  fall() {
    const { radius, height } = this.opts, p = this.pos;
    let nx = p.x + this.vel.x, nz = p.z + this.vel.z, hit = false;
    if (this.col.capsulePush) [nx, nz, hit] = this.col.capsulePush(nx, p.y, nz, radius, height);
    this.vy = Math.min(this.vy + this.gravity, this.maxFall);
    const g = this.col.groundY(nx, nz, p.y, 0);   // no step up while falling
    let y = p.y + this.vy;
    if (g !== null && y >= g) { y = g; this.onGround = true; this.vy = 0; }
    const moved = Math.hypot(nx - p.x, nz - p.z);
    this.pos = { x: nx, y, z: nz };
    this.hitWall = hit; this.stuck = false;
    if (y > this.spawn.y + this.killDepth) this.reset();
    return moved;
  }

  // render-time pose between the last two ticks
  lerp(alpha) {
    const p = this.prev, c = this.pos;
    return {
      x: p.x + (c.x - p.x) * alpha, y: p.y + (c.y - p.y) * alpha, z: p.z + (c.z - p.z) * alpha,
      heading: this.prevHeading + wrapPi(this.heading - this.prevHeading) * alpha,
    };
  }
}

// ---------------------------------------------------------------- animation
// clips: { idle, walk, run } AnimationClips; rootSpeed: { walk, run } in world units/tick
// at timeScale 1. Clip choice follows the measured ground speed; the playback rate is
// speed / rootSpeed so the feet don't slide.
// Actions override it with play(name, ...) on any clip added via addClips(); release()
// hands control back to the speed blend.
export class SpeedAnimator {
  constructor(object, clips, rootSpeed, { fade = 0.2 } = {}) {
    this.mixer = new THREE.AnimationMixer(object);
    this.rootSpeed = rootSpeed; this.fade = fade;
    this.actions = {};
    this.names = {};                              // locomotion key -> clip name
    for (const [k, clip] of Object.entries(clips)) {
      const a = this.mixer.clipAction(clip);
      a.setLoop(THREE.LoopRepeat, Infinity);
      this.actions[k] = a;
      this.names[k] = clip.name;
    }
    this.library = new Map(Object.values(clips).map(c => [c.name, c]));
    this.current = 'idle';
    this.actions.idle.play();
    this.rate = 1;
    this.override = null;                         // { name, action, loop, onEnd, ended }
    this.mixer.addEventListener('finished', e => {
      const o = this.override;
      if (!o || e.action !== o.action || o.ended) return;
      o.ended = true;
      if (o.onEnd) o.onEnd(o.name);
    });
  }

  addClips(clips) { for (const c of clips) if (!this.library.has(c.name)) this.library.set(c.name, c); }
  has(name) { return this.library.has(name); }
  duration(name) { const c = this.library.get(name); return c ? c.duration : 0; }
  clipName() { return this.override ? this.override.name : this.names[this.current]; }
  time() { return this.override ? this.override.action.time : this.actions[this.current].time; }

  // Crossfade from whatever plays now to clip `name`. loop=false plays it once and
  // holds the last frame; onEnd(name) fires when it finishes (one-shots only).
  play(name, { loop = false, rate = 1, fade = 0.15, onEnd = null } = {}) {
    const clip = this.library.get(name);
    if (!clip) throw new Error(`no clip ${name} (missing pack?)`);
    const from = this.override ? this.override.action : this.actions[this.current];
    const to = this.mixer.clipAction(clip);
    to.reset();
    to.setLoop(loop ? THREE.LoopRepeat : THREE.LoopOnce, loop ? Infinity : 1);
    to.clampWhenFinished = !loop;
    to.setEffectiveTimeScale(rate).setEffectiveWeight(1).play();
    if (from !== to) from.crossFadeTo(to, fade, false);
    this.override = { name, action: to, loop, onEnd, ended: false };
    this.rate = rate;
    return to;
  }

  setRate(r) { if (this.override) { this.override.action.setEffectiveTimeScale(r); this.rate = r; } }

  // back to the speed blend (picks the gait again on the next update)
  release(fade = 0.2) {
    const o = this.override;
    if (!o) return;
    this.override = null;
    const to = this.actions[this.current];
    to.reset().setEffectiveTimeScale(1).setEffectiveWeight(1).play();
    if (o.action !== to) o.action.crossFadeTo(to, fade, false);
  }

  pick(speed) {
    const { walk, run } = this.rootSpeed;
    const mid = (walk + run) / 2;
    const cur = this.current;
    // hysteresis so the clip doesn't chatter at the boundaries
    if (speed < (cur === 'idle' ? 0.12 : 0.06) * walk) return 'idle';
    if (cur === 'run' ? speed > mid * 0.9 : speed > mid * 1.1) return 'run';
    return 'walk';
  }

  update(speed, dt) {
    if (this.override) { this.mixer.update(dt); return; }
    const want = this.pick(speed);
    if (want !== this.current) {
      const from = this.actions[this.current], to = this.actions[want];
      to.reset().setEffectiveWeight(1).play();
      // start the new gait in step: keep the cycle phase (both loops start on the same foot)
      if (this.current !== 'idle' && want !== 'idle') {
        to.time = (from.time / from.getClip().duration) * to.getClip().duration;
      }
      from.crossFadeTo(to, this.fade, false);
      this.current = want;
    }
    const rate = want === 'idle' ? 1
      : Math.max(0.25, Math.min(1.6, speed / this.rootSpeed[want]));
    this.rate = rate;
    this.actions[want].setEffectiveTimeScale(rate);
    this.mixer.update(dt);
  }
}

// ---------------------------------------------------------------- camera
// Orbit camera behind and above the player. yaw: view direction heading (main.js
// convention), pitch: radians above horizontal. Pulled in when col.segment hits.
// Mode hook (aim camera etc.): setMode(name, { dist, targetHeight, shoulder }) eases
// toward those values (shoulder = sideways offset to the right, world units);
// clearMode() eases back to the defaults. No mode = the original camera.
export class FollowCamera {
  constructor(camera, col, { dist = 52, pitch = 0.32, targetHeight = 15, minDist = 5,
                             sensitivity = 0.0035 } = {}) {
    this.camera = camera; this.col = col;
    this.dist = dist; this.minDist = minDist; this.targetHeight = targetHeight;
    this.yaw = 0; this.pitch = pitch; this.sens = sensitivity;
    this.cur = dist;                              // current (possibly pulled-in) distance
    this.blocked = false;
    this.base = { dist, targetHeight, shoulder: 0 };
    this.shoulder = 0;
    this.mode = null; this.modeTarget = null;     // modeTarget: values being eased toward
    camera.up.set(0, -1, 0);
  }

  setMode(name, params = {}) {
    this.mode = name;
    this.modeTarget = { ...this.base, ...params };
  }

  clearMode() { if (this.mode) { this.mode = null; this.modeTarget = { ...this.base }; } }

  look(dx, dy) {
    this.yaw += dx * this.sens;
    this.pitch = Math.max(-0.35, Math.min(1.25, this.pitch + dy * this.sens));
  }

  // horizontal forward / right of the view, for camera-relative WASD
  basis() {
    const f = { x: Math.sin(this.yaw), z: Math.cos(this.yaw) };
    // right = fwd x up with up = (0,-1,0), as in main.js
    return { f, r: { x: f.z, z: -f.x } };
  }

  easeMode(dt) {
    const m = this.modeTarget; if (!m) return;
    const k = 1 - Math.exp(-8 * dt);
    let done = true;
    for (const key of ['dist', 'targetHeight', 'shoulder']) {
      const v = this[key] + (m[key] - this[key]) * k;
      this[key] = Math.abs(m[key] - v) < 1e-3 ? m[key] : v;
      if (this[key] !== m[key]) done = false;
    }
    if (done && !this.mode) this.modeTarget = null;   // back on the defaults: original path
  }

  update(feet, dt) {
    this.easeMode(dt);
    const t = new THREE.Vector3(feet.x, feet.y - this.targetHeight, feet.z);
    if (this.shoulder) { const { r } = this.basis(); t.x += r.x * this.shoulder; t.z += r.z * this.shoulder; }
    const cp = Math.cos(this.pitch), sp = Math.sin(this.pitch);
    const back = new THREE.Vector3(-Math.sin(this.yaw) * cp, -sp, -Math.cos(this.yaw) * cp); // -Y up: above = -y
    let want = this.dist;
    const end = t.clone().addScaledVector(back, this.dist);
    const hit = this.col.segment ? this.col.segment(t, end) : null;
    this.blocked = !!hit;
    if (hit) want = Math.max(this.minDist, hit.t * this.dist - 2.5);
    // pull in at once, ease back out
    this.cur = want < this.cur ? want : this.cur + (want - this.cur) * (1 - Math.exp(-4 * dt));
    this.camera.position.copy(t).addScaledVector(back, this.cur);
    this.camera.up.set(0, -1, 0);
    this.camera.lookAt(t);
  }
}
