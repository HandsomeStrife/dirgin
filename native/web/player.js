// Reusable third-person pieces for the Dirge web build: the player's locomotion translated
// from the game (z128, physics-body movement mode 0xc), its clip choice and clip clock,
// pluggable actions, and the game's follow camera (FUN_0032ace0, see the camera section).
//
// Game space throughout: -Y is up, so "down" is +y and the head sits at a smaller y
// than the feet. Headings use main.js's yaw convention: dir = (sin h, 0, cos h).
//
// ================================================================ TIMING (PAL)
// The game runs ONE logic step per 50 Hz frame, with step = vblanks x 1.2 ticks
// (FUN_00322f88 / FUN_0040ce28; DAT_004eeb24 = 1.1999999 in every trace record) and
// every mover scaling by `step`. PlayerController.step() is one such game frame
// (TICK = 1/50 s, STEP = 1.1999999 ticks). play.js's fixed-step loop uses TICK.
// Actions (actions/*.js) are still timed in 60 Hz ticks: step() runs them from a
// 60 Hz accumulator (ACTION_TICK), 1 or 2 times per game frame (see ACTIONS.md).
//
// ================================================================ ACTION INTERFACE
// (Also in native/web/ACTIONS.md; the one action on the page is actions/ladder.js.)
//
// PlayerController is a small state machine: LOCOMOTION (the game's ground/air body,
// translated below) plus at most one ACTIVE ACTION from a registry. Register actions
// with ctrl.addAction(mod); registry order is priority (first wins).
//
// An action module is a plain object:
//   name            string, unique ('ladder', ...)
//   canEnter(ctx)   -> bool. Asked every action tick, in registry order, while no action
//                      is active (locomotion running). Must not change anything.
//   enter(ctx)      -> optional ANIMATION REQUEST { clip, loop, rate, onEnd }
//                      (see Animator.play). Called once when canEnter said yes.
//   update(dt, ctx) -> a MOVE RESULT, once per 60 Hz action tick (dt = ACTION_TICK):
//        { locomotion: true }       locomotion moves him this game frame (action only animates)
//        { delta: {x,y,z}, heading?, onGround?, vy? }
//                                   the action OWNS movement: delta is applied as is
//                                   (game units, -Y up); the action does its own
//                                   collision through ctx.col. Omitted fields keep.
//        { done: true, ... }        leave after this tick (combine with either above)
//        null / undefined           same as { } = stand still, no movement
//   exit(ctx)       called once after done (or ctrl.cancelAction()). Release the
//                   animation (ctx.anim.release()) here.
//
// ctx (the same object every tick; read-only unless noted):
//   ctx.ctrl     the controller: pos {x,y,z} (feet), heading, vel {x,z} (units per 60 Hz
//                tick = the game's C+0xc0 x C+0xcc), vy, onGround, speed (C+0x1d8 / 60),
//                opts {radius,height}, gravity, spawn. Actions may write pos/vel/vy/onGround.
//   ctx.input    this frame's input: { x, z, run, keys: Set of held codes,
//                pressed: Set of codes pressed since the last frame (edge) }
//   ctx.col      collision (collision.js API: groundY, groundHit, move, navMove, segment)
//   ctx.anim     Animator: play(name, {loop, rate, onEnd}), release(),
//                setRate(r), has(name), time(), duration(name), current
//   ctx.motions  clip name -> sidecar entry (vincent.json / vincent_<pack>.json clips:
//                frames, loop, in_place_axes, root_motion_per_frame_glb, ...)
//   ctx.rootDelta(name, f0, f1)  the clip's root motion from frame f0 to f1 (fractional
//                frames OK), turned into game space for the current heading and scale
//   ctx.tick     60 Hz action ticks since reset;  ctx.TICK  the action step (s)
//
// With no actions registered every code path is the game's locomotion.
// ================================================================
import * as THREE from 'three';

// ---------------------------------------------------------------- timing
export const TICK = 1 / 50;                       // one PAL game frame (s): play.js's loop step
export const STEP = 1.1999999284744263;           // DAT_004eeb24: ticks per frame (f32 of 1 x 1.2)
export const DT = 0.019999999552965164;           // DAT_004eeb28 = STEP x 0.016666668 (s)
export const ACTION_TICK = 1 / 60;                // actions/*.js are timed in 60 Hz ticks

const f32 = Math.fround;
const wrapPi = a => Math.atan2(Math.sin(a), Math.cos(a));

// glb root motion (Y up, +Z = his forward, +X = his left) -> game space (-Y up) for
// heading h (forward = (sin h, cos h); right = (cos h, -sin h), main.js) at scale s.
export function glbToGame(v, h, s = 1) {
  const fx = Math.sin(h), fz = Math.cos(h), rx = Math.cos(h), rz = -Math.sin(h);
  return { x: s * (v[2] * fx - v[0] * rx), y: -s * v[1], z: s * (v[2] * fz - v[0] * rz) };
}

// ---------------------------------------------------------------- game data
// Runtime values of Vincent's records in z128, read from the sea-of-flames save state's
// EE memory and confirmed per frame in the PCSX2 traces (tools/trace; worker 0072).
export const GAME = {
  maxSpeed: 60.0,          // C+0x1d4 = rec+0x70 (FUN_00565bd0 mode 0 -> FUN_0034b978), u/s
  walkSpeed: 15.0,         // rec+0x74; walk fraction = rec+0x74 / rec+0x70 (FUN_00317618)
  rampUp: 25, rampDown: 25,   // P+0x29a / P+0x29b, stick byte ramp per tick (FUN_005748b0 init)
  gravity: 235.2,          // body+0xd0 = cfg+0xcc x 196 (FUN_00356358), u/s^2
  fallScale: 0.8,          // DAT_0046f4a0: gravity factor after the apex (FUN_003579a8)
  // locomotion clip table C+0x5f4 (FUN_00346860 / FUN_00346c10), as installed in z128:
  // entry 0 idle slot 1 (h_st), 1 walk slot 9 (h_wk), 2 run slot 16 (h_rn);
  // +0xc = upper speed threshold (u/s), +0x10 = root speed (u/s) -> C+0x65c
  loco: [{ key: 'idle', thr: 0.0, root: 0.0 },
         { key: 'walk', thr: 30.0, root: 15.551365852355957 },
         { key: 'run', thr: 0.0, root: 48.6978759765625 }],
  rateClamp: [0.1, 1.1],   // C+0x768 / +0x76c
  runRateClamp: [0.9, 1.0],// C+0x770 / +0x774 (run with C+0x564 bit 8)
  locoFlags: 0x19,         // C+0x564 while the table is installed (bits 1, 8, 0x10)
  blendFrames: 10,         // C+0x591 default (FUN_0033ff18); a layer blends over n + 1 clip-time units
  blendToIdle: 0x14,       // FUN_00346860: FUN_00340bb8(c, 1, 0x14) when the choice goes to idle
};

// FUN_00258218: one raw stick axis (pad byte - 0x80) -> -1..1. Dead zone 0x30 on both
// axes together, then (v -/+ 0x2f) / 81.
export function padAxis(raw, other) {
  if (Math.abs(raw) < 0x30 && Math.abs(other) < 0x30) return 0;
  const v = raw >= 0x2f ? raw - 0x2f : raw <= -0x2f ? raw + 0x2f : 0;
  return f32(v * f32(0.012345679));
}

// PS2 FPU / VU0 arithmetic rounds toward zero: the f32 next to x on the zero side.
const _cb = new DataView(new ArrayBuffer(4));
export function chop(x) {
  const r = f32(x);
  if (!(Math.abs(r) > Math.abs(x))) return r;
  _cb.setFloat32(0, r); _cb.setInt32(0, _cb.getInt32(0) - 1);
  return _cb.getFloat32(0);
}
const vlen = (x, z) => chop(Math.sqrt(chop(chop(x * x) + chop(z * z))));

// FUN_003295c0(cam, &sx, &sy, 1) + FUN_00333150's |v|: the processed stick (sx right,
// sy down; FUN_00258218) into world space through the camera matrix CAM+0x60 (rows 0
// and 2; translation dropped), flattened (y = 0) and rescaled to the stick's length.
// Returns { x, z, mag } with mag = the length FUN_00333150 turns into P+0x298.
export function stickToWorld(sx, sy, row0, row2) {
  const m = vlen(sx, sy);                         // fVar1 = |(sx, 0, -sy)|
  const vz = -sy;
  let x = chop(chop(sx * row0[0]) + chop(vz * row2[0]));
  let z = chop(chop(sx * row0[2]) + chop(vz * row2[2]));
  const s = chop(chop(x * x) + chop(z * z));
  if (s > 0) {
    const q = chop(1 / Math.sqrt(s));             // vrsqrt
    x = chop(chop(x * q) * m); z = chop(chop(z * q) * m);
  }
  return { x, z, mag: vlen(x, z) };
}

// Keyboard -> left stick (GUESS: keyboard mapping, not decoded; see notes/fidelity-audit.md).
// The game has no run button: tilt sets the stick byte, and >= 236 runs (FUN_00578748).
// Shift = full tilt (byte 255); walk = the b_walk trace's half tilt (pad 0x40 -> 0.2099,
// byte 53). Any byte 1..235 walks at the same 15 u/s; the byte only sets the turn rate.
export const KEY_WALK = 0.20987654;
export const KEY_RUN = 1.0;

// ---------------------------------------------------------------- controller
export class PlayerController {
  // opts.pos/heading: spawn feet and heading. walkSpeed/runSpeed/accel/turnRate/snapDown/
  // gravity/maxFall/killDepth from the old controller are ignored (the game's values above).
  constructor(col, { pos, heading = 0, radius = 3.5, height = 19, scale = 1 } = {}) {
    this.col = col;
    this.opts = { radius, height };
    this.gravity = GAME.gravity / 3600;           // mode-0xc gravity, u per 60 Hz tick^2 (actions read it)
    this.scale = scale;                           // character scale (root motion -> world)
    this.spawn = { ...pos, heading };
    this.actions = [];                            // registry, priority order
    this.active = null;                           // the running action, or null = locomotion
    this.ctx = { ctrl: this, col, input: null, anim: null, motions: {},
                 tick: 0, TICK: ACTION_TICK, rootDelta: (n, f0, f1) => this.rootDelta(n, f0, f1) };
    this.reset();
  }

  // ctx pieces the controller doesn't own (play.js wires them)
  attach({ anim, motions } = {}) {
    if (anim) { this.ctx.anim = anim; if (anim.bind) anim.bind(this); }
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
    // player half (P = the player actor)
    this.P = {
      state: 2,                                   // P+0x104: 2 = on foot (FUN_0055b8e0 states)
      b298: 0, b299: 0,                           // stick byte target / ramped (FUN_00578748)
      dir: { x: 0, y: 0, z: 0 },                  // P+0x2c0: last stick direction (world)
    };
    // character half (C) and its physics body (C+0x3b0)
    this.C = {
      dir: { x: 0, y: 0, z: 0 }, spd: 0,          // C+0xc0 xyz, C+0xcc (units per tick)
      turn: { x: 0, y: 0, z: 0 }, turnRate: 0,    // C+0x2c0 xyz, C+0x2cc
      face: { x: Math.sin(s.heading), y: 0, z: Math.cos(s.heading) },   // C+0x20 (matrix row 2)
      groundSpeed: 0,                             // C+0x1d8 (u/s)
      moveDir: { x: 0, y: 0, z: 0 },              // C+0x90
    };
    this.body = {
      state: 0,                                   // body+0xa8: 0 ground, 2 airborne (FUN_00357860)
      poly: null,                                 // body+0x110: walk-mesh polygon (kept across frames)
      gy: 0,                                      // body+0x10 y: the walker's ground point under him
      t: 0, y0: 0, dy: 0, v0: 0, rise: 0,         // body+0xd4, +0xd8, +0xdc, +0xe0, +0xe8 (FUN_00357168)
    };
    this.vel = { x: 0, z: 0 };                    // C+0xc0 x C+0xcc, units per 60 Hz tick
    this.speed = 0;                               // C+0x1d8 / 60 (units per tick)
    this.onGround = true; this.hitWall = false; this.stuck = false;
    this.vy = 0;                                  // airborne vertical speed (units per tick, +y down)
    this.ticks = 0;                               // 60 Hz action ticks
    this.frames = 0;                              // game frames
    this.actAcc = 0;
    this.locate();
  }

  // the walk polygon under the feet (FUN_0035cbc8 search, the body's polygon body+0x110)
  // GUESS: re-search after spawn and after an action moved him (nearest polygon within 40,
  // else the 0x02-crossing search window); the game keeps body+0x110, and how its ladder
  // states hand the body back isn't decoded. Locomotion itself keeps body.poly.
  locate() {
    const col = this.col, p = this.pos;
    if (typeof col.navMove !== 'function') { this.body.poly = null; return; }
    const f = col.nearestPoly(p.x, p.y, p.z, 40) ?? col.findPoly(p.x, p.z, p.y - 7.5, p.y + 200);
    this.body.poly = f ? f[0] : null;
    this.body.gy = f ? f[1] : p.y;
    if (f && this.body.state === 0) this.pos.y = f[1];
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
  // optional stick: magnitude 0..1 (overrides the keyboard mapping; the trace replay
  // feeds the pad's processed magnitude here), keys / pressed Sets for actions.
  // One call = one game frame.
  step(input) {
    this.prev = { ...this.pos };
    this.prevHeading = this.heading;
    const ctx = this.ctx;
    ctx.input = input;

    // actions: 60 Hz ticks from an accumulator (1 or 2 per frame)
    // GUESS: the 60 Hz action accumulator (port convenience, not the game's: the game runs
    // its states once per frame with `step`; see notes/fidelity-audit.md "Web frame loop and cape cloth").
    let loco = !this.active, owned = false;
    this.actAcc += STEP;
    while (this.actAcc >= 1) {
      this.actAcc -= 1;
      ctx.tick = this.ticks;
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
        const res = a.update(ACTION_TICK, ctx) || {};
        if (res.locomotion) loco = true;
        else { this.applyOwned(res); owned = true; loco = false; }
        if (res.done && this.active === a) { this.active = null; a.exit(ctx); }
      }
      this.ticks++;
      ctx.input = { ...input, pressed: new Set() };   // an edge is seen by one tick only
    }
    ctx.input = input;
    if (owned) this.locate();                     // an action moved him off the body's polygon
    this.stickInput(input);                       // FUN_00333150: runs in every state
    this.intent(owned);                           // FUN_00578748: the ramp runs in every state
    if (loco) this.moveTick();                    // FUN_0034ea18 -> FUN_0034c388 case 0xc
    if (loco) this.rotate();                      // FUN_0034ea18 -> FUN_0034de50 mode 4
    if (ctx.anim && ctx.anim.gameFrame) ctx.anim.gameFrame(this, STEP);   // FUN_00343160 / FUN_005aa5b8
    this.frames++;
  }

  // an action moved him: apply its result
  applyOwned(res) {
    const d = res.delta || { x: 0, y: 0, z: 0 };
    this.pos = { x: this.pos.x + (d.x || 0), y: this.pos.y + (d.y || 0), z: this.pos.z + (d.z || 0) };
    if (res.heading !== undefined) {
      this.heading = wrapPi(res.heading);
      this.C.face = { x: Math.sin(this.heading), y: 0, z: Math.cos(this.heading) };
    }
    if (res.teleport) {                           // test aid (?ladder=): no previous frame to lag behind
      this.prev = { ...this.pos }; this.prevHeading = this.heading;
      if (this.onTeleport) this.onTeleport();
    }
    if (res.onGround !== undefined) {
      this.onGround = res.onGround;
      this.body.state = res.onGround ? 0 : 2;
      if (!res.onGround) {
        this.startFall(0);                        // hand-over to the body's own fall
        // GUESS: an action that drops him (ladder jump-off) hands over in the fall state 9,
        // so the first fall frame is not an on-foot frame (same GUESS state switch as
        // moveTick's 2 <-> 9; see notes/fidelity-audit.md)
        if (this.P.state >= 2 && this.P.state <= 5) this.P.state = 9;
      } else if (this.P.state === 9) {
        this.P.state = 2;                         // back on his feet (same GUESS switch; a ladder exit)
      }
    }
    if (res.vy !== undefined) this.vy = res.vy;
    if (!res.keepVel) { this.vel.x = 0; this.vel.z = 0; }
    this.hitWall = false; this.stuck = false;
  }

  // ------------------------------------------------------------ player half
  // FUN_00333150 (input handler), the stick part for the on-foot states 2-5 (the tail
  // after LAB_00333b08): stick -> world through the camera (FUN_003295c0, magnitude
  // kept); |v| > 0.05 -> P+0x2c0 = v and P+0x298 = clamp(|v| x 255); else P+0x298 = 0
  // (P+0x2c0 keeps the last direction). Air states 8/9/0x1f-0x21 take LAB_00333760 instead.
  stickInput(input) {
    const P = this.P;
    const len = Math.hypot(input.x || 0, input.z || 0);
    let mag = 0, v = null;
    if (input.stick !== undefined) {              // a pad stick, already through stickToWorld
      mag = input.stick; v = { x: input.x || 0, y: 0, z: input.z || 0 };
    } else if (len > 1e-6) {
      mag = input.run ? KEY_RUN : KEY_WALK;       // GUESS: keyboard mapping
      v = { x: input.x / len * mag, y: 0, z: input.z / len * mag };
    }
    const st = P.state;
    if (st === 8 || st === 9 || (st >= 0x1f && st <= 0x21)) {
      // air states: LAB_00333760 -> FUN_00574938 / FUN_00574920 (and the same writes for
      // 0x20/0x21): P+0x2c0 = this frame's stick vector every frame, no 0.05 gate (zero when
      // released); P+0x298 = clamp(|v| x 255)
      P.dir = v ?? { x: 0, y: 0, z: 0 };
      P.b298 = Math.max(0, Math.min(255, Math.trunc(chop(mag * 255.0)))) & 0xff;
      return;
    }
    if (mag > 0.05) {
      P.dir = v;
      P.b298 = Math.max(0, Math.min(255, Math.trunc(chop(mag * 255.0)))) & 0xff;
    } else {
      P.b298 = 0;
    }
  }

  // FUN_00578748 (player move intent). Ground states 2-5 / 0x16 / 0x1a and the
  // jump-state air control (8, 9, 0x1f-0x21, physics-body mode 0xc with script flag 13).
  intent(owned = false) {
    const P = this.P, C = this.C;
    // stick byte ramp: target 0 / 235 / 255 (only a byte >= 236 runs), +-P+0x29a/b x step
    let cap = 0xff;
    if (P.b298 < 0xec) cap = P.b298 === 0 ? 0 : 0xeb;
    let b = P.b299;
    if (b < cap) {
      b = P.b299 + Math.trunc(f32(GAME.rampUp * STEP));
      if (cap < b) b = cap;
    } else if (cap < b) {
      // (P+0x100 bits 40-41 set: double rate; those are the weapon-mode bits, clear on foot)
      b = P.b299 - Math.trunc(f32(GAME.rampDown * STEP));
      if (b < cap) b = cap;
    }
    P.b299 = b < 0 ? 0 : b > 255 ? 255 : b;

    const mods = 1.0;                             // fVar18 + 1 + fVar19: no equipment mods (rec+0xc0 = 0)
    const walkFrac = f32(GAME.walkSpeed / GAME.maxSpeed);   // FUN_00317618
    // GUESS: while an action owns movement (the ladder) the player is in that action's own
    // state, not an on-foot state 2-5, so no walk move is written (the ladder's state
    // number isn't decoded; see notes/fidelity-audit.md). The stick ramp above still runs.
    const st = owned ? -1 : P.state;
    let spd = 0;
    if (st >= 2 && st <= 5) {
      const B = P.b299;
      if (B === 0) spd = 0;
      else if (B < 0xeb) spd = f32(f32(walkFrac * f32(0.004255319)) * B);
      else spd = f32(walkFrac + f32(f32((1.0 - walkFrac) * 0.05) * (B - 0xeb)));
      spd = f32(f32(spd * mods) * GAME.maxSpeed);
      if (spd <= 0.75) spd = 0;
    } else if (st === 8 || st === 9 || (st >= 0x1f && st <= 0x21)) {
      return this.airIntent(mods);
    }
    // normalised stick direction, w = speed per tick
    const d = norm3(P.dir);
    const w = f32(spd * f32(0.016666668));
    // P+0x29c = 2 (mode set by FUN_005747f8 in FUN_00333150); C+0x568 bit 0x1000 clear
    this.setMove(w, d);                           // FUN_0034af98
    // turn: FUN_0034b638(rate, C, dir); rate = (P+0x298 / 512 + 0.5) x 0.25, 0 with no stick
    const rate = P.b298 === 0 ? 0 : f32(f32(f32(P.b298 * f32(0.001953125)) + 0.5) * 0.25);
    this.setTurn(rate, d);
    // FUN_0034e960(C, 4): rotation mode 4 (already 4 on foot)
  }

  // FUN_00578748, states 8/9/0x1f-0x21 with mode 0xc: air control on the body's
  // horizontal velocity (FUN_0034a968 get / FUN_0034af98 set).
  airIntent(mods) {
    const P = this.P, C = this.C;
    const m = Math.min(1.0, mods < 0.01 ? 0 : mods);
    let cap = f32(f32(GAME.maxSpeed * f32(0.016666668)) * m);   // fVar19 (x DAT_0046ef24 = 0.3 firing; not here)
    // FUN_0034a968: v = body+0x40 (horizontal velocity, units per tick)
    let vx = C.dir.x * C.spd, vz = C.dir.z * C.spd;
    let L = Math.hypot(vx, vz);
    if (cap < L) { vx = vx / L * cap; vz = vz / L * cap; }
    // auStack_100.xyz = normalised P+0x2c0 (zero once the stick is released: stickInput air
    // path). The length test is xyz only (0x578eac: vmul.xyz, vaddy.x, vaddz.x; w not used).
    const d = norm3(P.dir);
    if (Math.hypot(d.x, d.y, d.z) > 0) {
      // 0x578ee0..0x578f1c: v += d.xyz * (DAT_004eeb24 * f20 * 0.075) (vmulx, scalar k)
      const k = f32(f32(STEP * cap) * f32(0.075));
      vx += d.x * k; vz += d.z * k;
      L = Math.hypot(vx, vz);
      if (cap < L) { vx = vx / L * cap; vz = vz / L * cap; }
    } else {
      L = Math.hypot(vx, vz);
      if (L > 0) {
        let s = L - cap * 0.025;
        if (s < 0) s = 0;
        vx *= s / L; vz *= s / L;
      }
    }
    L = Math.hypot(vx, vz);
    this.setMove(L, L > 0 ? { x: vx / L, y: 0, z: vz / L } : C.dir);
    this.setTurn(0.25, d);                        // FUN_0034b638(0.25, ...) in jump states
  }

  // FUN_0034af98(speed, C, dir): C+0xc0 = dir (if non-zero), C+0xcc = speed
  setMove(w, d) {
    const C = this.C;
    if (d.x !== 0 || d.y !== 0 || d.z !== 0) C.dir = { x: d.x, y: d.y, z: d.z };
    C.spd = w;
  }

  // FUN_0034b638(rate, C, dir): C+0x2c0 = dir, C+0x2cc = rate
  setTurn(rate, d) {
    const C = this.C;
    C.turn = { x: d.x, y: d.y, z: d.z };
    C.turnRate = rate;
  }

  // ------------------------------------------------------------ character half
  // FUN_0034c388, movement mode 0xc (physics body; z128 sets script flag 13).
  // C+0x1c0 flags in z128: 0x400000, 0x2000 and 0x4000 clear.
  moveTick() {
    const C = this.C, B = this.body;
    // velocity = C+0xc0 x C+0xcc -> FUN_00358fb8: body+0x30 (y = 0 while airborne)
    const v = { x: C.dir.x * C.spd, y: airborne(B) ? 0 : C.dir.y * C.spd, z: C.dir.z * C.spd };
    const prev = { ...this.pos };
    this.integrate(v);                            // FUN_003590c8
    // flag 0x2000 clear: C+0x1d8 = C+0xcc x 60 (the commanded speed, also against walls)
    C.groundSpeed = f32(C.spd * 60.0);
    // flag 0x4000 clear: C+0x90 = normalised C+0xc0
    C.moveDir = norm3(C.dir);
    // ground bit 0x200 = !FUN_003577c8(body)
    this.onGround = !airborne(B);
    // GUESS: player state on foot <-> fall. The state-2 class's event handler FUN_00537f38
    // returns state 9 when the ground bit is clear, but which event fires it, and the
    // fall state's landing, aren't decoded. 9 while airborne, 2 again once landed.
    if (!this.onGround && this.P.state >= 2 && this.P.state <= 5) this.P.state = 9;
    else if (this.onGround && this.P.state === 9) this.P.state = 2;
    this.vel = { x: C.dir.x * C.spd, z: C.dir.z * C.spd };
    this.speed = C.groundSpeed / 60;
    this.stuck = this.onGround && C.spd > 0 && prev.x === this.pos.x && prev.z === this.pos.z;
  }

  // FUN_003590c8 (physics-body step) for z128's body flags 0x236: bit 5 set, bits 19-21
  // clear (velocity body+0x40 = body+0x30 at once, no accel or turn limit), bit 12 clear,
  // bits 7 / 14 clear (position = the walker's result). States 0 (ground) and 2 (air).
  integrate(v) {
    const B = this.body, col = this.col;
    const p = this.pos;
    if (B.poly === null) {                        // no walk polygon: FUN_00357d48 skips the walk
      if (B.state === 2) this.fallY();
      return;
    }
    if (B.state === 0) {
      // move = body+0x40 x step -> FUN_00357d48 (collision.js navMove)
      const r = col.navMove(B.poly, p.x, p.y, p.z, v.x * STEP, v.z * STEP, false);
      this.hitWall = r[4];
      B.poly = r[3];
      // second switch case 0: pos = body+0x10, stored as the game's f32 (PS2 rounds toward zero)
      this.pos = { x: chop(r[0]), y: chop(r[1]), z: chop(r[2]) };
      B.gy = r[1];
      if (r[5]) this.startFall(0);                // drop edge / 0x02 crossing: FUN_00357148
      return;
    }
    // state 2: move = (body+0x40 + body+0x60) x step (body+0x60 = 0, FUN_00357168 clears it)
    this.gravityStep();                           // FUN_003579a8
    const y = f32(B.y0 + B.dy);                   // body.y = body+0xd8 + body+0xdc
    // FUN_00357d48 walks the ground point body+0x10 (collision.js air mode: horizontal slide)
    const r = col.navMove(B.poly, p.x, B.gy, p.z, v.x * STEP, v.z * STEP, true);
    this.hitWall = r[4];
    B.poly = r[3];
    const groundY = B.gy = r[1];                  // body+0x10.y
    this.pos = { x: chop(r[0]), y, z: chop(r[2]) };
    // second switch case 2 (FUN_00326500 true for the player): no landing while rising
    let land;
    if (B.rise > 0) land = !(B.t <= B.rise);
    else if (B.t <= DT) land = (col.polys[B.poly].flags & 0x10) === 0;
    else land = true;
    if (y > groundY && land) {
      this.pos.y = groundY;
      // GUESS: landing on a 0x10 (drop) polygon goes to body state 3 in the game
      // (FUN_00357860(body, 3)); state 3 isn't translated, he lands as state 0.
      B.state = 0;                                // FUN_00357860(body, 0)
      this.vy = 0;
    }
  }

  // FUN_00357168(h): take-off / fall start. h = 0 for a fall (FUN_00357148).
  startFall(h) {
    const B = this.body, g = GAME.gravity;
    B.t = 0;
    B.y0 = this.pos.y;                            // body+0xd8 = body.y
    B.dy = 0;
    if (h <= B.t) { B.rise = B.t; B.v0 = B.t; }
    else {
      B.v0 = f32(-Math.sqrt(f32(2 * g) * h));     // take-off speed (u/s, -y up)
      B.rise = f32(Math.abs(B.v0 / g));           // + DAT_0046f4a4 (= 0)
    }
    B.state = 2;                                  // FUN_00357860(body, 2)
  }

  // FUN_003579a8, FUN_00326500() true branch (player); DAT_0046f4a4 = DAT_0046f4a8 = 0:
  // rising: v0 t + g t^2 / 2; after the apex (t >= rise): the rise height plus
  // 0.8 g (DAT_0046f4a0) from the apex. Then +0.01 and t += dt.
  gravityStep() {
    const B = this.body, g = GAME.gravity;
    let t = B.t, dy;
    if (t < B.rise) {
      dy = f32(B.v0 * t + g * t * 0.5 * t);
    } else {
      const r = B.rise, u = f32(t - r);
      dy = f32(B.v0 * r + g * r * 0.5 * r + g * GAME.fallScale * 0.5 * u * u);
    }
    const old = B.dy;
    B.dy = f32(dy + 0.01);
    B.t = f32(B.t + DT);
    this.vy = (B.dy - old) / STEP;               // informational: units per 60 Hz tick
  }

  // airborne with no polygon: only the height changes
  fallY() {
    this.gravityStep();
    this.pos.y = f32(this.body.y0 + this.body.dy);
  }

  // FUN_0034de50, rotation mode 4 (C+0x214): turn the facing (matrix row 2) toward
  // C+0x2c0 by FUN_00323318(C+0x2cc). C+0x45c bit 21 clear (no angular limit).
  rotate() {
    const C = this.C;
    let tgt = C.turn;
    if (Math.hypot(tgt.x, tgt.y, tgt.z) === 0) tgt = C.face;
    const f = C.face;
    const dot = f.x * tgt.x + f.y * tgt.y + f.z * tgt.z;
    const t = fracStep(C.turnRate);
    let to = tgt;
    // nearly opposite: turn toward matrix row 0 (= (-fz, 0, fx)); C+0x1c0 bit 0x40 is clear
    if (!(-0.97 < dot)) to = { x: -f.z, y: 0, z: f.x };
    const n = norm3({ x: f.x + (to.x - f.x) * t, y: f.y + (to.y - f.y) * t, z: f.z + (to.z - f.z) * t });
    if (n.x !== 0 || n.z !== 0) C.face = n;
    this.heading = Math.atan2(C.face.x, C.face.z);
  }

  // render-time pose between the last two frames
  lerp(alpha) {
    this.alpha = alpha;
    const p = this.prev, c = this.pos;
    return {
      x: p.x + (c.x - p.x) * alpha, y: p.y + (c.y - p.y) * alpha, z: p.z + (c.z - p.z) * alpha,
      heading: this.prevHeading + wrapPi(this.heading - this.prevHeading) * alpha,
    };
  }
}

// FUN_00323318(r): the per-frame fraction for a per-tick rate r at the current step
// (*(DAT_0046e290+4) = 1.1999999): 1 - (1 - r)^n for whole ticks n while step > 1, else r x step.
export function fracStep(r, step = STEP) {
  if (!(1.0 < step)) return f32(r * step);
  let i = 1, k = 1.0;
  if (1.0 <= step) {
    do { i++; k = f32(k * f32(1.0 - r)); } while (i <= step);
  }
  return f32(1.0 - k);
}

// FUN_003577c8: body states 2, 3, 6, 7 are airborne
function airborne(B) { return B.state === 2 || B.state === 3 || B.state === 6 || B.state === 7; }

function norm3(v) {
  const l = Math.hypot(v.x, v.y, v.z);
  return l > 0 ? { x: v.x / l, y: v.y / l, z: v.z / l } : { x: 0, y: 0, z: 0 };
}

// ---------------------------------------------------------------- animation
// The game's locomotion clip choice and clip clock, run once per game frame by the
// controller (gameFrame), drawn by update().
//   FUN_00346860  clip choice from C+0x1d8 against the table C+0x5f4 (GAME.loco):
//                 <= 0.01 idle, <= 30 (29.9 while running) walk, else run. A switch
//                 between table entries keeps the clip frame (FUN_00345448 ->
//                 FUN_005a8fd0, clamped to the new clip's n - 0.5); the first one after
//                 the table is installed starts at frame 0.
//   FUN_00346c10  rate = clamp(speed / root, 0.1, 1.1), at least 0.5 (run: clamp to
//                 0.9..1.0, at least 1.0); FUN_003453e0: clip rate MP+0x40c = rate x 0.5
//   FUN_005aa2a8  frame += rate x weight/128 x step (FUN_005aa5b8, both blend layers);
//                 loops reset to exactly 0 at the clip length, one-shots hold n - 1
//   blend         the new clip's weight MP+0x408 rises (len - rem) / len, rem -= step
//                 per frame, len = C+0x591 + 1 = 11 (21 into idle: FUN_00340bb8(c,1,0x14))
// Clip frames are 30 Hz-authored game frames; the glb keys sit at t = frame / 60 with a
// closing key at nf = frame 0 (vincent.json), so time = frame / 60.
// Actions override it with play(name, ...) and release() hands control back; both
// crossfade over ACTION_FADE (three.js crossfades, not the game's blend).
// GUESS: action clip blend length (not decoded: FUN_00342360's length argument; see
// notes/fidelity-audit.md "Crossfade times"). Stand-in: the locomotion blend's
// C+0x591 default, 10 + 1 clip-time units at 60 per second.
export const ACTION_FADE = (GAME.blendFrames + 1) / 60;
export class SpeedAnimator {
  constructor(object, clips) {
    this.mixer = new THREE.AnimationMixer(object);
    this.actions = {};
    this.names = {};                              // locomotion key -> clip name
    this.frames = {};                             // locomotion key -> clip frame count
    for (const [k, clip] of Object.entries(clips)) {
      const a = this.mixer.clipAction(clip);
      a.setLoop(THREE.LoopRepeat, Infinity);
      a.timeScale = 0;                            // the clock is the game's (gameFrame)
      this.actions[k] = a;
      this.names[k] = clip.name;
      this.frames[k] = Math.round(clip.duration * 60);
    }
    this.library = new Map(Object.values(clips).map(c => [c.name, c]));
    this.current = 'idle';
    this.actions.idle.play();
    this.rate = 0.5;                              // MP+0x40c
    // game clip state: head layer (key, frame) and the blend-out layer
    this.head = { key: 'idle', frame: 0, prevFrame: 0 };
    this.out = null;                              // { key, frame, prevFrame }
    this.blend = { len: 0, rem: 0, w: 1 };        // layer +0x24 remaining, MP+0x408
    this.installed = false;                       // C+0x658 > 0 (table installed)
    this.cur = -1;                                // C+0x659: current table index
    this.override = null;                         // { name, action, loop, onEnd, ended }
    this.drawHook = null;                         // per-frame hook point (play.js: cape cloth), see gameFrame
    this.mixer.addEventListener('finished', e => {
      const o = this.override;
      if (!o || e.action !== o.action || o.ended) return;
      o.ended = true;
      if (o.onEnd) o.onEnd(o.name);
    });
  }

  bind(ctrl) { this.ctrl = ctrl; }

  addClips(clips) { for (const c of clips) if (!this.library.has(c.name)) this.library.set(c.name, c); }
  has(name) { return this.library.has(name); }
  duration(name) { const c = this.library.get(name); return c ? c.duration : 0; }
  clipName() { return this.override ? this.override.name : this.names[this.current]; }
  time() { return this.override ? this.override.action.time : this.head.frame / 60; }

  // Crossfade from whatever plays now to clip `name`. loop=false plays it once and
  // holds the last frame; onEnd(name) fires when it finishes (one-shots only).
  play(name, { loop = false, rate = 1, onEnd = null } = {}) {
    const fade = ACTION_FADE;
    const clip = this.library.get(name);
    if (!clip) throw new Error(`no clip ${name} (missing pack?)`);
    const from = this.override ? this.override.action : this.actions[this.current];
    if (!this.override) {                         // leave the game's clock: free-running clips
      for (const a of Object.values(this.actions)) a.timeScale = 1;
      if (this.out) this.actions[this.out.key].setEffectiveWeight(0);
      this.out = null;
    }
    const to = this.mixer.clipAction(clip);
    to.reset();
    to.setLoop(loop ? THREE.LoopRepeat : THREE.LoopOnce, loop ? Infinity : 1);
    to.clampWhenFinished = !loop;
    to.setEffectiveTimeScale(rate).setEffectiveWeight(1).play();
    // the clip that was showing fades out from full weight: mid-blend its weight is w < 1, and
    // three.js would fill the rest with the bind pose (port plumbing for the ACTION_FADE GUESS)
    if (from !== to) { from.setEffectiveWeight(1); from.crossFadeTo(to, fade, false); }
    this.override = { name, action: to, loop, onEnd, ended: false };
    this.rate = rate;
    return to;
  }

  setRate(r) { if (this.override) { this.override.action.setEffectiveTimeScale(r); this.rate = r; } }

  // back to the game's locomotion clips (the choice runs again on the next game frame)
  release() {
    const fade = ACTION_FADE, o = this.override;
    if (!o) return;
    this.override = null;
    const to = this.actions[this.current];
    to.reset().setEffectiveWeight(1).play();
    to.timeScale = 0;
    if (o.action !== to) o.action.crossFadeTo(to, fade, false);
    this._fadeOut = { action: o.action, t: fade };
  }

  // FUN_00346860: the table index for ground speed s (u/s)
  pick(s) {
    const L = GAME.loco;
    const thr0 = Math.max(L[0].thr, 0.01);        // 0.75 with C+0x564 bit 0x800 (clear)
    const hyst = this.cur === 2 ? -0.1 : 0.0;
    if (s <= thr0) return 0;
    if (s <= f32(L[1].thr + hyst)) return 1;
    return L.length - 1;
  }

  // one game frame (called by PlayerController.step after the character tick)
  gameFrame(ctrl, step) {
    if (this.override) {                          // an action's clip plays
      if (this.drawHook) this.drawHook();
      return;
    }
    const s = ctrl.C.groundSpeed;
    // GUESS: when the game installs the table (C+0x658 = 3) isn't decoded (its writer for
    // the player wasn't found in two attempts; FUN_00345b58 is only called from NPC state
    // classes). Traced: installed in the frame C+0x1d8 first reaches 5.55 (ramp byte 87,
    // the third frame of stick); 3.70 the frame before doesn't install it.
    if (!this.installed && s >= 5.0) this.installed = true;
    let rate = this.rate;
    if (this.installed) {
      const idx = this.pick(s);
      if (idx !== this.cur) this.switchTo(idx);
      // FUN_00346c10 (C+0x564 = 0x19: bits 1, 8, 0x10)
      const run = (GAME.locoFlags & 8) !== 0 && this.cur === 2;
      const root = GAME.loco[this.cur].root;      // C+0x65c
      let r = 1.0;
      if (root !== 0) {
        const [lo, hi] = run ? GAME.runRateClamp : GAME.rateClamp;
        r = f32(s / root);
        r = Math.min(Math.max(r, Math.min(lo, hi)), Math.max(lo, hi));
        const floor = (GAME.locoFlags & 1) ? (run ? 1.0 : 0.5) : 0.5;
        if (r < floor) r = floor;
      }
      rate = f32(r * 0.5);                        // FUN_003453e0: MP+0x40c
    }
    this.rate = rate;
    // MP+0x408 for this frame's draw: the traced bones blend at the weight logged in the same
    // record (worker 0083, c_run_pose_blend: median 0.000; last frame's weight: 0.13-0.3), so
    // the weight is set before the pose, from the remainder before this frame's countdown.
    const b = this.blend, blending = this.out && b.rem > 0;
    if (blending) b.w = (b.len - b.rem) / b.len;  // MP+0x408
    else { b.w = 1; this.out = null; }
    // Hook point: the frame's draw. FUN_005ab8f0 poses the model (FUN_005a87a0 / FUN_005a9db0,
    // its lines 87-113) and runs the cape cloth (FUN_005b2ef8, line 143) BEFORE it advances
    // the clips (FUN_005aa5b8, line 218): after this frame's clip choice, before the advance.
    if (this.drawHook) this.drawHook();
    // FUN_005aa5b8 -> FUN_005aa2a8 on the head layer and the blend-out layer
    const adv = L => {
      L.prevFrame = L.frame;
      L.frame = f32(L.frame + f32(f32(rate * 1.0) * step));   // weight byte 128 / 128
      if (L.frame >= this.frames[L.key]) { L.frame = 0; L.prevFrame = 0; }   // loop: reset to 0
    };
    adv(this.head);
    if (blending) {
      adv(this.out);
      b.rem = f32(b.rem - step);
      if (b.rem <= 0) b.rem = 0;
    }
  }

  // FUN_00346860: a new table index
  switchTo(idx) {
    const prevKey = this.head.key, first = this.cur === -1;
    const key = GAME.loco[idx].key;
    let frame = 0;
    if (!first) frame = Math.min(Math.max(this.head.frame * 2, 0), this.frames[key] * 2 - 1) * 0.5;  // FUN_005a8fd0
    const len = (idx === 0 && !first ? GAME.blendToIdle : GAME.blendFrames) + 1;
    this.out = { key: prevKey, frame: this.head.frame, prevFrame: this.head.frame };
    this.head = { key, frame, prevFrame: frame };
    this.blend = { len, rem: len, w: 0 };
    this.cur = idx;
    this.current = key;
  }

  // draw: pose the mixer at the game's clip frames (the controller's render alpha
  // interpolates between game frames, like the body position)
  update(speed, dt) {
    if (this.override) { this.mixer.update(dt); return; }
    if (this._fadeOut) {                           // an action's clip fading after release()
      const f = this._fadeOut; f.t -= dt;
      if (f.t <= 0) { f.action.stop(); this._fadeOut = null; }
      this.mixer.update(dt);
    }
    const alpha = this.ctrl && Number.isFinite(this.ctrl.alpha) ? this.ctrl.alpha : 1;
    const fr = L => L.frame >= L.prevFrame ? L.prevFrame + (L.frame - L.prevFrame) * alpha : L.frame;
    const at = L => fr(L) / 60;
    for (const [k, a] of Object.entries(this.actions)) {
      const isHead = k === this.head.key, isOut = this.out && k === this.out.key;
      // FUN_005a9db0 poses only the head layer (MP+0x448) and the blend-out layer
      // (MP+0x44a, while MP+0x408 < 1): every other clip has weight 0. Don't gate this
      // on isRunning(): three.js reports false whenever timeScale is 0, which is always
      // true here, so a finished blend's weights stayed live (run = 68% h_rn + 31% h_st).
      if (!isHead && !isOut) { if (!this._fadeOut) a.setEffectiveWeight(0); continue; }
      if (!a.isRunning()) a.play();
      a.timeScale = 0;
      a.time = at(isHead ? this.head : this.out);
      a.setEffectiveWeight(isHead ? (this.out ? this.blend.w : 1) : 1 - this.blend.w);
    }
    this.mixer.update(0);
    // The game's own pose build replaces the mixer's model-space mix for the locomotion
    // clips (skipped while an action's clip fades out: that crossfade is the mixer's GUESS).
    if (this.lp && !this._fadeOut)
      this.poseLocal(this.names[this.head.key], fr(this.head),
                     this.out ? this.names[this.out.key] : null, this.out ? fr(this.out) : 0, this.blend.w);
  }

  // data/vincent/vincent.json local_pose + vincent_local.bin (export_web_vincent.py
  // local_pose): each clip's parent-relative TRS per bone at frames 0..nf.
  setLocalPose(info, buffer, root) {
    const n = info.bones, joints = [];
    for (let b = 0; b < n; b++) {
      const j = root.getObjectByName('j' + String(b).padStart(2, '0'));
      if (!j) throw new Error('local pose: no joint node j' + b);
      joints.push(j);
    }
    const par = info.parent.map((p, i) => (p < 0 || p >= n || p === i) ? -1 : p);
    const order = [], seen = new Uint8Array(n);   // parents before children (anim_rot.compose recursion)
    const visit = i => { if (seen[i]) return; seen[i] = 1; if (par[i] >= 0) visit(par[i]); order.push(i); };
    for (let i = 0; i < n; i++) visit(i);
    this.lp = { n, clips: info.clips, data: new Float32Array(buffer), joints, par, order,
                a: new Float64Array(n * 10), b: new Float64Array(n * 10),
                R: Array.from({ length: n }, () => new THREE.Quaternion()),
                S: new Float64Array(n * 3), P: new Float64Array(n * 3), q: new THREE.Quaternion(), v: new THREE.Vector3() };
  }

  // FUN_005af6c0 at a fractional clip frame: t/s lerp, q nlerp with hemisphere flip between
  // the bracketing keys (the export samples every integer frame, so the keys are frames).
  sampleLocal(name, frame, out) {
    const L = this.lp, c = L.clips[name], d = L.data, n = L.n;
    let i = Math.floor(frame);
    i = Math.max(0, Math.min(i, c.frames - 2));
    const u = frame - i, o0 = (c.offset + i * n) * 10, o1 = o0 + n * 10;
    for (let b = 0; b < n; b++) {
      const k0 = o0 + b * 10, k1 = o1 + b * 10, k = b * 10;
      for (let j = 0; j < 6; j++) out[k + j] = d[k0 + j] * (1 - u) + d[k1 + j] * u;
      const sg = d[k0 + 6] * d[k1 + 6] + d[k0 + 7] * d[k1 + 7] + d[k0 + 8] * d[k1 + 8] + d[k0 + 9] * d[k1 + 9] < 0 ? -1 : 1;
      let m = 0;
      for (let j = 6; j < 10; j++) { out[k + j] = d[k0 + j] * (1 - u) + sg * d[k1 + j] * u; m += out[k + j] * out[k + j]; }
      m = Math.sqrt(m) || 1;
      for (let j = 6; j < 10; j++) out[k + j] /= m;
    }
  }

  // FUN_005a9db0: the blend-out layer at weight 1 (FUN_005afa30), the head layer lerped in at
  // MP+0x408 (FUN_005afa58: per bone, local t/s lerp, q nlerp with hemisphere flip), then
  // the compose (anim_rot.compose: R = Rp q, S = Sp s, P = Pp + Rp (Sp t)) written to the
  // flat joint nodes as model-space TRS. Checked against the game's bone matrices *(MP+0x3f8)
  // in the idle -> walk -> run blends (c_run_pose_blend): median 0.000-0.003, max 0.027.
  poseLocal(head, fh, out, fo, w) {
    const L = this.lp, n = L.n, A = L.a, B = L.b;
    this.sampleLocal(head, fh, B);
    if (out && w < 1) {
      this.sampleLocal(out, fo, A);
      for (let b = 0; b < n; b++) {
        const k = b * 10;
        for (let j = 0; j < 6; j++) A[k + j] = A[k + j] * (1 - w) + B[k + j] * w;
        const sg = A[k + 6] * B[k + 6] + A[k + 7] * B[k + 7] + A[k + 8] * B[k + 8] + A[k + 9] * B[k + 9] < 0 ? -1 : 1;
        let m = 0;
        for (let j = 6; j < 10; j++) { A[k + j] = A[k + j] * (1 - w) + sg * B[k + j] * w; m += A[k + j] * A[k + j]; }
        m = Math.sqrt(m) || 1;
        for (let j = 6; j < 10; j++) A[k + j] /= m;
      }
    } else A.set(B);
    const { R, S, P, par, joints, q, v } = L;
    for (const i of L.order) {
      const k = i * 10, p = par[i];
      q.set(A[k + 6], A[k + 7], A[k + 8], A[k + 9]);
      if (p < 0) {
        R[i].copy(q);
        S[3 * i] = A[k + 3]; S[3 * i + 1] = A[k + 4]; S[3 * i + 2] = A[k + 5];
        P[3 * i] = A[k]; P[3 * i + 1] = A[k + 1]; P[3 * i + 2] = A[k + 2];
      } else {
        R[i].multiplyQuaternions(R[p], q);
        for (let j = 0; j < 3; j++) S[3 * i + j] = S[3 * p + j] * A[k + 3 + j];
        v.set(S[3 * p] * A[k], S[3 * p + 1] * A[k + 1], S[3 * p + 2] * A[k + 2]).applyQuaternion(R[p]);
        P[3 * i] = P[3 * p] + v.x; P[3 * i + 1] = P[3 * p + 1] + v.y; P[3 * i + 2] = P[3 * p + 2] + v.z;
      }
      const J = joints[i];
      J.position.set(P[3 * i], P[3 * i + 1], P[3 * i + 2]);
      J.quaternion.copy(R[i]);
      J.scale.set(S[3 * i], S[3 * i + 1], S[3 * i + 2]);
    }
  }
}

// ---------------------------------------------------------------- camera
// The game's third-person follow camera: camera type 0xc ("normal", view mode 1),
// FUN_00330478 (the type's update) -> FUN_0032ace0 (the camera update), translated for
// the paths z128 takes on foot (worker 0089; notes/fidelity-audit.md "Camera").
// Checked per frame against six PCSX2 traces (idle, walk, run, right-stick turn,
// run in a circle, right-stick look): eye <= 0.007, look-at <= 0.001, yaw/pitch <= 2e-4 rad.
//
// CAM fields (camera object, DAT_0046e290+0x40) kept here:
//   +0x130 base (the smoothed player point), +0x90 eye, +0x80 forward, +0x60.. basis rows,
//   +0x174 pitch, +0x178 yaw, +0x1c0 eye distance, +0x1a8 auto-turn gain,
//   +0x1e2 stick-idle counter (s8), +0x1ec / +0x1f0 look-stick ramp and direction.
// Game space, -Y up. pitch > 0 = eye below the look-at point, looking up.
//
// One game frame = one tick() (50 Hz, step 1.2), run BEFORE the player's logic: the
// player's stick-to-world (FUN_003295c0) reads this frame's matrix, and the camera reads
// the player as the previous frame left him (both checked on the traces).
const CSTEP = 1.2;                                // DAT_004eeb24 (PAL)
const PI_ = 3.1415927, TWO_PI = 6.2831855;
const DEG = 0.017453294;
export const CAM_TYPE_C = {                       // type 0xc object (FUN_00330138), runtime values (z128 state)
  pivot: [0, -17, 0],                             // +0x10: look-at point above the base
  offset: [0, 1.25, -38],                         // +0x20: eye offset (camera space)
  rate: [0.05, 0.05],                             // +0x30 / +0x34: look-stick yaw / pitch rate
  fov: 73.5,                                      // +0x50
};
const LOOK_TABLE = 1.0;                           // DAT_004a0650[DAT_004e1058 = 8] (view mode 1; option value in the state)
const BASE_RATE_UP = [0.125, 0.5, 0.125];         // CAM+0x150 (base moving down, dy > 0)
const BASE_RATE_DN = [0.125, 0.15, 0.125];        // CAM+0x160
export const PITCH_MAX = 50 * DEG;                // DAT_0046e3f8 (50 in RAM; the ELF's initial value is 54)
export const PITCH_MIN = -54 * DEG;               // DAT_0046e3fc
const BASE_FOV = 73.5;                            // DAT_0046e3f0 in RAM (ELF: 65)
const BASE_RATE_MAX = [1.0, 1.0, 1.0];            // DAT_004a05f8..0x4a0600 (vmini cap on rate x step)
// FUN_003264e0() = (FUN_00241410() == 0) = (DAT_0046b1d8 == 0); DAT_0046b1d8 = 1 in the state
// (set by FUN_00241400), so the base rates are scaled by the step and capped
const DAT_0046B1D8 = 1;
// FUN_00253f50() = FUN_002419d8(0) (DAT_004b8ac8 bit 0) && option DAT_004e1070; both 0 in the
// state: auto turn-behind and the follow nudge are on
const DAT_004B8AC8 = 0, DAT_004E1070 = 0;
const fun253f50 = () => ((DAT_004B8AC8 >> 0) & 1) !== 0 && DAT_004E1070 !== 0;
const LOOK_DEADZONE = 0.5;                        // DAT_0046e464 (RAM)
const MOUSE_PAD_RELEASE = 0.3;                    // FUN_00254ac8: pad x|x| or y|y| past this ends mouse mode
// spawn: the sea-of-flames save state's camera angles (CAM+0x174 / +0x178, +0x1e2 = 30).
// The game's camera reset FUN_00329f68 -> FUN_003285d0 takes the angles from render view 1's
// forward (FUN_003ee5e8(1) + 0x20), sets the snap flag CAM+0x1e8 0x2000, distance 1000 and
// ramp 0 (reset() below). GUESS: the angles (what view 1 holds at stage entry, and whether a
// respawn calls FUN_00329f68, are not decoded; notes/fidelity-audit.md). dist is the state's
// (30.54) for reference only: reset() measures it as the game does.
export const CAM_SPAWN = { pitch: -0.0383, yaw: 0.9083, dist: 30.5358 };
// Mouse: the game's USB-mouse path (FUN_00254ac8, mode DAT_004e0ff8 = 1 in the state):
// right stick += mouse counts x DAT_004e1004 (0.1), clamped to +-1, then / step
// (FUN_003256d8 when DAT_004e17d0). GUESS: one browser movementX/Y pixel = one USB count.
export const MOUSE_SCALE = 0.1;

const wrapA = a => {                              // the inline wrap in FUN_0032ace0
  if (a > 314.15927 || a < -314.15927) return 0;
  while (a < -PI_) a += TWO_PI;
  while (a > PI_) a -= TWO_PI;
  return a;
};
const clampf = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
// FUN_00260bc8(a, pi): wrap to +-pi in steps of pi/4096 (13-bit signed)
const quantA = a => (((Math.trunc((a / PI_) * 4096.0) << 19) >> 19) * PI_ * 0.00024414062);
// FUN_00260f54: (pitch, yaw) of a direction
const dirAngles = (x, y, z) => [Math.atan2(-y, Math.hypot(x, z)), Math.atan2(x, z)];
// FUN_00323318(k): per-frame fraction for the step (whole ticks compounded)
function stepFrac(k) {
  if (!(1.0 < CSTEP)) return k * CSTEP;
  let f = 1.0, i = 1;
  do { i++; f *= 1.0 - k; } while (i <= CSTEP);
  return 1.0 - f;
}
// row-vector x 3x3 (FUN_0026138c); rotations FUN_00259928 (x) / FUN_00259990 (y)
const vm = (v, M) => [v[0] * M[0][0] + v[1] * M[1][0] + v[2] * M[2][0],
                      v[0] * M[0][1] + v[1] * M[1][1] + v[2] * M[2][1],
                      v[0] * M[0][2] + v[1] * M[1][2] + v[2] * M[2][2]];
const rotX = a => { const c = Math.cos(a), s = Math.sin(a); return [[1, 0, 0], [0, c, s], [0, -s, c]]; };
const rotY = a => { const c = Math.cos(a), s = Math.sin(a); return [[c, 0, -s], [0, 1, 0], [s, 0, c]]; };
const mm = (A, B) => A.map(r => [0, 1, 2].map(j => r[0] * B[0][j] + r[1] * B[1][j] + r[2] * B[2][j]));
const len3 = v => Math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

// Camera collision set: the packets the camera's ray tests. FUN_0054c848 casts through the
// map object (FUN_0053bb58) with probe index 6 = flags 0x18b, i.e. packets with class bit
// 0x04 (notes: native/assets/collision/FORMAT.md, "Packet-mask test").
// GUESS: which DCOL input files that map object holds (not decoded; FUN_0053bb58). The
// traces match with files 1-2 of the z128 DCOL; file 3's deck blocker (z -1120.1) must
// not be in it (the game's eye passes it at +-50 deg pitch).
export const CAM_COL_FILES = [1, 2];
export function cameraRays(buf) {
  const dv = new DataView(buf);
  const nv = dv.getUint32(8, true), nt = dv.getUint32(12, true);
  let o = 0x30;
  const V = new Float32Array(buf.slice(o, o + nv * 12)); o += nv * 12;
  const I = new Uint32Array(buf.slice(o, o + nt * 12)); o += nt * 12;
  o += nt * 4;                                    // surface words
  const info = new Uint32Array(buf.slice(o, o + nt * 4));
  const T = [];
  for (let t = 0; t < nt; t++) {
    const inf = info[t];
    if ((inf & 3) !== 1) continue;                // sub-tree 1 packets
    if (((inf >>> 16) & ~(0x18b | 0x100) & 0xffff) === 0) continue;
    if (!CAM_COL_FILES.includes((inf >>> 8) & 0xff)) continue;
    const a = 3 * I[3 * t], b = 3 * I[3 * t + 1], c = 3 * I[3 * t + 2];
    const tri = [V[a], V[a + 1], V[a + 2], V[b], V[b + 1], V[b + 2], V[c], V[c + 1], V[c + 2]];
    const ux = tri[3] - tri[0], uy = tri[4] - tri[1], uz = tri[5] - tri[2];
    const wx = tri[6] - tri[0], wy = tri[7] - tri[1], wz = tri[8] - tri[2];
    const nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
    const ln = Math.sqrt(nx * nx + ny * ny + nz * nz);
    if (ln < 1e-9) continue;
    tri.push(nx / ln, ny / ln, nz / ln,
             Math.min(tri[0], tri[3], tri[6]), Math.max(tri[0], tri[3], tri[6]),
             Math.min(tri[1], tri[4], tri[7]), Math.max(tri[1], tri[4], tri[7]),
             Math.min(tri[2], tri[5], tri[8]), Math.max(tri[2], tri[5], tri[8]));
    T.push(tri);
  }
  // segment a->b: nearest hit {point, normal} (double-sided, as col.segment)
  function segment(a, b) {
    const dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    const x0 = Math.min(a[0], b[0]), x1 = Math.max(a[0], b[0]);
    const y0 = Math.min(a[1], b[1]), y1 = Math.max(a[1], b[1]);
    const z0 = Math.min(a[2], b[2]), z1 = Math.max(a[2], b[2]);
    let bt = null, bn = null;
    for (const p of T) {
      if (p[13] < x0 || p[12] > x1 || p[15] < y0 || p[14] > y1 || p[17] < z0 || p[16] > z1) continue;
      const e1x = p[3] - p[0], e1y = p[4] - p[1], e1z = p[5] - p[2];
      const e2x = p[6] - p[0], e2y = p[7] - p[1], e2z = p[8] - p[2];
      const px = dy * e2z - dz * e2y, py = dz * e2x - dx * e2z, pz = dx * e2y - dy * e2x;
      const det = e1x * px + e1y * py + e1z * pz;
      if (Math.abs(det) < 1e-12) continue;
      const inv = 1 / det, sx = a[0] - p[0], sy = a[1] - p[1], sz = a[2] - p[2];
      const u = (sx * px + sy * py + sz * pz) * inv;
      if (u < 0 || u > 1) continue;
      const qx = sy * e1z - sz * e1y, qy = sz * e1x - sx * e1z, qz = sx * e1y - sy * e1x;
      const v = (dx * qx + dy * qy + dz * qz) * inv;
      if (v < 0 || u + v > 1) continue;
      const t = (e2x * qx + e2y * qy + e2z * qz) * inv;
      if (t >= 0 && t <= 1 && (bt === null || t < bt)) { bt = t; bn = p; }
    }
    return bt === null ? null : { point: [a[0] + bt * dx, a[1] + bt * dy, a[2] + bt * dz], normal: [bn[9], bn[10], bn[11]] };
  }
  return { count: T.length, segment };
}

export class FollowCamera {
  // camera: THREE camera (null in headless tests); rays: cameraRays(...) or null (no collision)
  constructor(camera, rays = null, type = CAM_TYPE_C) {
    this.camera = camera; this.rays = rays; this.type = type;
    this.mouseActive = false;                     // DAT_004e17d0
    this.look = [0, 0];                           // mouse counts gathered for the next frame
    this.reset({ x: 0, y: 0, z: 0 });
  }

  // put the camera on a player at pos (spawn, H, ladder warp): FUN_003285d0 (angles, snap
  // flag 0x2000, CAM+0x1c0 = 1000, ramp 0), then the eye placed as the next frame's update
  // places it (snapped base, collision; dist 1000 > any measured, so dist = the measured one)
  reset(pos, s = CAM_SPAWN) {
    this.base = [pos.x, pos.y, pos.z];            // CAM+0x130
    this.yaw = s.yaw; this.pitch = s.pitch;       // CAM+0x178 / +0x174 (GUESS: CAM_SPAWN)
    this.dist = 1000.0;                           // CAM+0x1c0 = 0x447a0000
    this.ramp = 0; this.rampDir = [0, 0];         // CAM+0x1ec (+0x1f0 left as is in the game)
    this.snap = true;                             // CAM+0x1e8 bit 0x2000
    this.idle = 30;                               // CAM+0x1e2 (GUESS: the state's value)
    this.gain = 0;                                // CAM+0x1a8 (GUESS: not reset by FUN_003285d0)
    this.look = [0, 0];
    this.mouseActive = false;                     // page aid: H / warp drop the mouse latch (DAT_004e17d0)
    this.flags14 = 0;                             // DAT_0046e290+0x14 (this frame's camera request bits)
    const g = this.place(this.base, this.base, { recede: 0, state: 0, L: 0 });
    this.eye = g.eye; this.at = g.at; this.M = g.M;
    this.prev = { eye: this.eye.slice(), at: this.at.slice(), f: this.M[2].slice() };
  }

  // mouse movement (pixels; GUESS 1 px = 1 USB count), summed until the next frame
  addMouse(dx, dy) { this.look[0] += dx; this.look[1] += dy; }

  // horizontal forward / right of this frame's matrix (CAM+0x60 rows 2 / 0), for the
  // camera-relative keys (FUN_003295c0 flattens row 2)
  basis() {
    const f = this.M[2], h = Math.hypot(f[0], f[2]) || 1;
    return { f: { x: f[0] / h, z: f[2] / h }, r: { x: this.M[0][0], z: this.M[0][2] } };
  }

  // one game frame. p: { pos: {x,y,z} (C+0x30, last frame's), want: {x,y,z} (C+0xc0),
  // state: P+0x104, stick: |left stick| (FUN_00325678), pad: [rx, ry] processed right stick or null }
  tick(p) {
    const T = this.type, S = CSTEP;
    this.prev = { eye: this.eye.slice(), at: this.at.slice(), f: this.M[2].slice() };
    // FUN_00330478: auto-turn gain from the left stick
    const L = p.stick || 0;
    this.gain = clampf(L > 0.5 ? (L - 0.5) * S : this.gain - S * 0.3, 0, 1);
    // base follow (CAM+0x130, FUN_0032ace0 head). Snap flag 0x2000: base = player, flag
    // cleared. Else d = player - base; only when |d.y| < 15 is d scaled per axis (by
    // min(rate x step, 1) when FUN_003264e0() == 0, else by the raw rate); at 15+ the base
    // lands on the player in one frame
    const tgt = [p.pos.x, p.pos.y, p.pos.z];
    let old = this.base, base;
    if (this.snap) {
      old = tgt.slice(); base = tgt.slice();      // CAM+0x140 and +0x130 both = the player
      this.snap = false;
    } else {
      const d = [tgt[0] - old[0], tgt[1] - old[1], tgt[2] - old[2]];
      if (Math.abs(d[1]) < 15.0) {
        const rr = 0.0 < d[1] ? BASE_RATE_UP : BASE_RATE_DN;
        const k = DAT_0046B1D8 !== 0              // FUN_003264e0() == 0
          ? rr.map((x, j) => Math.min(x * S, BASE_RATE_MAX[j])) : rr;
        for (let j = 0; j < 3; j++) d[j] *= k[j];
      }
      base = [old[0] + d[0], old[1] + d[1], old[2] + d[2]];
    }
    this.base = base;
    // P+8 == 0 path: how far the look point turned and receded, seen from last frame's eye
    // (FUN_00259d58 -> e2 = quantised (pitch, yaw) change; f32 = distance change)
    const e = this.eye, py = T.pivot[1];
    const v2d = [old[0] - e[0], old[1] - e[1] + py, old[2] - e[2]];
    const v2c = [base[0] - e[0], base[1] - e[1] + py, base[2] - e[2]];
    const ac = dirAngles(...v2c), ad = dirAngles(...v2d);
    let e2 = [quantA(ac[0] - ad[0]), quantA(ac[1] - ad[1])];
    const n2c = len3(v2c);
    if (n2c < 6.0) {                              // DAT_0046e444 / 0x46e448
      const s = n2c > 3.5 ? (n2c - 3.5) / (6.0 - 3.5) : 0;
      e2 = [e2[0] * s, e2[1] * s];
    }
    const recede = (n2c - len3(v2d)) * 1.0;       // x (CAM+0x1e1 = 60) / 60; CAM+0x1e9 < 0
    // look stick (FUN_003256d8: x|x|, y|y|, signs +1 / -1; mouse: counts x 0.1 / step)
    // FUN_00254ac8: mouse counts set the latch DAT_004e17d0; a pad stick past 0.3 (after
    // x|x|) clears it; the sum is clamped to +-1
    let sx = 0, sy = 0;
    const fx = this.look[0] * MOUSE_SCALE, fy = this.look[1] * MOUSE_SCALE;
    if (fx !== 0.0 || fy !== 0.0) this.mouseActive = true;
    const px = p.pad ? p.pad[0] * Math.abs(p.pad[0]) : 0, pyy = p.pad ? p.pad[1] * Math.abs(p.pad[1]) : 0;
    if (this.mouseActive && (MOUSE_PAD_RELEASE < Math.abs(px) || MOUSE_PAD_RELEASE < Math.abs(pyy))) this.mouseActive = false;
    const mx = clampf(fx + px, -1, 1);
    const my = clampf(fy + pyy, -1, 1);
    this.look = [0, 0];
    sx = mx; sy = -my;
    if (this.mouseActive) { sx /= S; sy /= S; }
    // ramp CAM+0x1ec toward |(sx, sy)| (FUN_0032ace0, LAB_0032b950)
    const m = Math.hypot(sx, sy);
    let c = this.ramp;
    if (c < m) {
      const st = (m < 0.5 ? 0.01 : 0.05) * S;
      c = Math.abs(c - m) < st ? m : c + st;
    } else if (m < c) {
      c = this.mouseActive ? m : (Math.abs(c - m) < S * 0.075 ? m : c - S * 0.075);
    }
    this.ramp = c;
    if (m > 0) this.rampDir = [sx, sy];
    const dl = Math.hypot(this.rampDir[0], this.rampDir[1]);
    if (dl > 0) { sx = this.rampDir[0] / dl * c; sy = this.rampDir[1] / dl * c; } else { sx = 0; sy = 0; }
    const smag = Math.hypot(sx, sy);
    // look-stick deadzone DAT_0046e464: past it on either axis the update calls
    // FUN_00324440(10) (request bit 10 of DAT_0046e290+0x14); sx / sy are not changed.
    // GUESS: its FUN_00326068(7) gate is taken as open (not decoded); nothing in the port
    // reads the bit yet (FUN_00324530 / FUN_00324598 do in the game)
    this.flags14 = 0;
    if (!(Math.abs(sx) <= LOOK_DEADZONE && Math.abs(sy) <= LOOK_DEADZONE)) this.flags14 |= 1 << 10;
    // stick-idle counter CAM+0x1e2: -20 while turning, then +1 per frame to 30
    if (sx === 0) this.idle = this.idle > 29 ? 30 : Math.min(30, this.idle + Math.trunc(S));
    else this.idle = -20;
    const f30 = this.idle * 0.033333335, f29 = f30 * 0.0025;
    // auto turn-behind: yaw toward the wanted move direction (C+0xc0) by the gain;
    // past 110 deg it slows to 0 at 140, beyond 140 it turns toward the front instead
    if (sx === 0 && this.gain > 0.1 && this.idle >= 1 && !fun253f50()) {
      const fw = this.M[2], camYaw = Math.atan2(fw[0], fw[2]);
      const w = p.want || { x: 0, z: 0 }, plYaw = Math.atan2(w.x, w.z);
      const raw = wrapA(-wrapA(camYaw - plYaw));
      const cl = wrapA(clampf(raw, -140 * DEG, 140 * DEG));
      const deg = cl * 57.295776, g = f29 * PI_ * this.gain * S;
      if (Math.abs(deg) >= 110.0) {               // DAT_0046e404 / DAT_0046e408
        if (Math.abs(raw * 57.295776) < 140.0) this.yaw += cl * g * (140.0 - Math.abs(deg)) * 0.033333335;
        else this.yaw += wrapA(raw + PI_) * g;
      } else {
        this.yaw += cl * g;
      }
    }
    // look stick -> yaw / pitch (type +0x30 / +0x34 x step x option table)
    const ry = T.rate[0] * S * LOOK_TABLE, rp = T.rate[1] * S * LOOK_TABLE;
    this.yaw = wrapA(this.yaw + sx * ry);
    this.pitch += sy * rp;
    // follow nudge (CAM+0x1e8 bit 0x8000): turn by the look point's angle change, capped
    if (this.idle > 0 && smag === 0 && !fun253f50()) {
      this.yaw += clampf((L * 0.2 + 0.8) * f30 * 1.0 * e2[1], -ry, ry);
      this.pitch += clampf(f30 * 0.25 * e2[0], -rp, rp);
    }
    this.pitch = clampf(this.pitch, PITCH_MIN, PITCH_MAX);
    // place the eye: offset, collision, distance ease
    const g = this.place(base, old, { recede, state: p.state, L });
    this.eye = g.eye; this.at = g.at; this.M = g.M;
  }

  // the eye for this frame (FUN_0032ace0 from the matrix build on); ctx = null: no
  // collision or easing (reset)
  place(base, old, ctx) {
    const T = this.type;
    // final pitch: + atan(k tan(fov x 0.875 / 2)) - atan(k tan(BASE_FOV x 0.875 / 2)), 0 while
    // the type FOV = DAT_0046e3f0 (both 73.5 in z128); CAM+0x188 x +0x180 = 0 (no blend)
    const g0 = this.pitch * -0.4774648, x = Math.abs(g0) + 0.3;
    const kk = g0 * (x > 1 ? 1.0 : 1 - Math.cos(x * 1.5707964)); // FUN_00260cf4
    const pf = this.pitch + Math.atan(kk * Math.tan(T.fov * 0.875 * DEG * 0.5))
                          - Math.atan(kk * Math.tan(BASE_FOV * 0.875 * DEG * 0.5));
    const RX = rotX(pf), RY = rotY(this.yaw), M = mm(RX, RY);
    const off = T.offset.slice();
    off[1] *= Math.abs(RX[1][1]) * 0.8 + 0.2;
    const eoff = vm(off, M);
    const piv = [T.pivot[0], T.pivot[1], T.pivot[2] + (pf > 0 ? -4.0 : -3.0) * pf];   // DAT_0046e4dc / 0x46e4d8
    const pr = vm(piv, RY);
    const at = [base[0] + pr[0], base[1] + pr[1], base[2] + pr[2]];                    // CAM+0xf0
    let eye = [at[0] + eoff[0], at[1] + eoff[1], at[2] + eoff[2]];
    if (!ctx) return { eye, at, M };
    // collision (FUN_00544f90 -> FUN_0054c848): 5 rays from the view-axis point S to the
    // eye and the corners of a 6 x 6 square around it; the nearest hit pulls the eye in
    // to the same fraction, then forward by 3 sin(angle to the wall) (DAT_0046e440 = 3)
    if (this.rays) {
      const offz = vm(off, RX)[2];
      const f23 = Math.abs(RX[2][2]) < 0.001 ? 1.0 : (offz + T.pivot[2]) / RX[2][2];
      const fw = M[2];
      const S0 = [eye[0] - fw[0] * f23, eye[1] - fw[1] * f23, eye[2] - fw[2] * f23];
      let best = null;
      for (const [a, b] of [[-1, -1], [-1, 1], [1, -1], [1, 1], [0, 0]]) {      // DAT_0058bb90
        const o = [0, 1, 2].map(j => M[0][j] * 3 * a + M[1][j] * 3 * b);
        const end = [eye[0] + o[0], eye[1] + o[1], eye[2] + o[2]];
        const h = this.rays.segment(S0, end);
        if (!h) continue;
        const dh = len3([S0[0] - h.point[0], S0[1] - h.point[1], S0[2] - h.point[2]]);
        if (best === null || dh < best.d) {
          const sc = dh / len3([S0[0] - end[0], S0[1] - end[1], S0[2] - end[2]]);
          best = { d: dh, p: [0, 1, 2].map(j => h.point[j] - o[j] * sc), n: h.normal };
        }
      }
      if (best) {
        const f = fw[0] * best.n[0] + fw[1] * best.n[1] + fw[2] * best.n[2];
        const push = Math.sqrt(9.0 * (1 - f * f));
        eye = [0, 1, 2].map(j => best.p[j] + fw[j] * push);
      }
    }
    // distance (CAM+0x1c0): in at once, out slowly. Out: 0.03 per frame (FUN_00323318),
    // x0.1 while the base moves away from the eye in the on-foot states (view mode 1),
    // else x (|left stick| x 0.9 + 0.1); plus the recede term x (dist - 25) / (60 - 25)
    // (DAT_0046e504 / DAT_0046e500 = 60 in RAM)
    const pv = vm(T.pivot, rotY(this.yaw));
    const ref = [base[0] + pv[0], base[1] + pv[1], base[2] + pv[2]];
    const u = [eye[0] - ref[0], eye[1] - ref[1], eye[2] - ref[2]];
    const dist = len3(u);
    const fz = dist >= 60.0 ? 1.0 : Math.max(0, (dist - 25.0) / (60.0 - 25.0));
    if (this.dist < dist) {
      let r = stepFrac(0.03);
      const st = ctx.state;
      const slow = (st >= 2 && st < 6) || st === 0x13 || st === 0x14 || st === 0x1d || st >= 0xc0;
      if (slow && 20.0 < dist) {
        const dOld = len3([eye[0] - old[0], eye[1] - old[1], eye[2] - old[2]]);
        const dNew = len3([eye[0] - base[0], eye[1] - base[1], eye[2] - base[2]]);
        r *= dOld < dNew ? 0.1 : ctx.L * 0.9 + 0.1;
      }
      this.dist += (dist - this.dist) * r + fz * ctx.recede;
      eye = [0, 1, 2].map(j => ref[j] + u[j] / dist * this.dist);
    } else {
      this.dist = dist + ctx.recede;
    }
    return { eye, at, M };
  }

  // render: the camera between the last two game frames (alpha as ctrl.lerp; a display-rate
  // convenience like the body's, not game logic)
  update(alpha = 1) {
    if (!this.camera) return;
    const a = clampf(alpha, 0, 1), P = this.prev;
    const L = (x, y) => x + (y - x) * a;
    const eye = [0, 1, 2].map(j => L(P.eye[j], this.eye[j]));
    const at = [0, 1, 2].map(j => L(P.at[j], this.at[j]));
    // forward = matrix row 2 (CAM+0x80); aim along it from the eye
    const f = [0, 1, 2].map(j => L(P.f[j], this.M[2][j]));
    this.camera.position.set(eye[0], eye[1], eye[2]);
    this.camera.up.set(0, -1, 0);
    this.camera.lookAt(eye[0] + f[0], eye[1] + f[1], eye[2] + f[2]);
    this._at = at;
  }
}
