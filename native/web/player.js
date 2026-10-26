// Reusable third-person pieces for the Dirge web build: a fixed-tick character
// controller, a speed-driven animation blender and a collision-aware follow camera.
//
// Game space throughout: -Y is up, so "down" is +y and the head sits at a smaller y
// than the feet. Headings use main.js's yaw convention: dir = (sin h, 0, cos h).
import * as THREE from 'three';

export const TICK = 1 / 60;                       // simulation step (s)

const wrapPi = a => Math.atan2(Math.sin(a), Math.cos(a));

// ---------------------------------------------------------------- controller
// Speeds are world units per tick (one 60 Hz game frame).
export class PlayerController {
  constructor(col, { pos, heading = 0, walkSpeed, runSpeed, radius = 3.5, height = 19,
                     accel = 14, turnRate = 11 } = {}) {
    this.col = col;
    this.opts = { radius, height };
    this.walkSpeed = walkSpeed; this.runSpeed = runSpeed;
    this.accel = accel;                           // 1/s: velocity eases toward the target
    this.turnRate = turnRate;                     // rad/s: max turn speed of the body
    this.spawn = { ...pos, heading };
    this.reset();
  }

  reset() {
    const s = this.spawn;
    this.pos = { x: s.x, y: s.y, z: s.z };
    this.prev = { ...this.pos };
    this.heading = this.prevHeading = s.heading;
    this.vel = { x: 0, z: 0 };                    // intended horizontal velocity (units/tick)
    this.speed = 0;                               // measured ground speed (units/tick), smoothed
    this.onGround = true; this.hitWall = false; this.stuck = false;
    this.ticks = 0;
  }

  // input: { x, z } desired world direction (any length, 0 = no input), run: bool
  step(input) {
    this.prev = { ...this.pos };
    this.prevHeading = this.heading;

    const len = Math.hypot(input.x, input.z);
    const target = len > 1e-6 ? (input.run ? this.runSpeed : this.walkSpeed) : 0;
    const tx = len > 1e-6 ? input.x / len * target : 0;
    const tz = len > 1e-6 ? input.z / len * target : 0;
    const k = 1 - Math.exp(-this.accel * TICK);
    this.vel.x += (tx - this.vel.x) * k;
    this.vel.z += (tz - this.vel.z) * k;
    if (target === 0 && Math.hypot(this.vel.x, this.vel.z) < 0.01) this.vel.x = this.vel.z = 0;

    let moved = 0;
    if (this.vel.x || this.vel.z) {
      const r = this.col.move(this.pos, { x: this.vel.x, z: this.vel.z }, this.opts);
      this.stuck = r.x === this.pos.x && r.z === this.pos.z;
      moved = Math.hypot(r.x - this.pos.x, r.z - this.pos.z);
      this.pos = { x: r.x, y: r.y, z: r.z };
      this.onGround = r.onGround; this.hitWall = r.hitWall;
      // pressing into a wall: don't keep building velocity into it
      if (r.hitWall && moved < 0.5 * Math.hypot(this.vel.x, this.vel.z)) {
        const f = moved / Math.max(1e-6, Math.hypot(this.vel.x, this.vel.z));
        this.vel.x *= Math.max(f, 0.5); this.vel.z *= Math.max(f, 0.5);
      }
    } else {
      this.hitWall = false; this.stuck = false;
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
    this.ticks++;
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
export class SpeedAnimator {
  constructor(object, clips, rootSpeed, { fade = 0.2 } = {}) {
    this.mixer = new THREE.AnimationMixer(object);
    this.rootSpeed = rootSpeed; this.fade = fade;
    this.actions = {};
    for (const [k, clip] of Object.entries(clips)) {
      const a = this.mixer.clipAction(clip);
      a.setLoop(THREE.LoopRepeat, Infinity);
      this.actions[k] = a;
    }
    this.current = 'idle';
    this.actions.idle.play();
    this.rate = 1;
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
export class FollowCamera {
  constructor(camera, col, { dist = 52, pitch = 0.32, targetHeight = 15, minDist = 5,
                             sensitivity = 0.0035 } = {}) {
    this.camera = camera; this.col = col;
    this.dist = dist; this.minDist = minDist; this.targetHeight = targetHeight;
    this.yaw = 0; this.pitch = pitch; this.sens = sensitivity;
    this.cur = dist;                              // current (possibly pulled-in) distance
    this.blocked = false;
    camera.up.set(0, -1, 0);
  }

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

  update(feet, dt) {
    const t = new THREE.Vector3(feet.x, feet.y - this.targetHeight, feet.z);
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
