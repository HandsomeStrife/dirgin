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
import { Jump, stickMode, enterState } from './jump_core.js';

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
export const CROSS_KEY = 'Space';                 // the page's ✕ (logical id 0xd), shared with ladder.js KEY
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
      jumpBits: 0,                                // P+0x100 bits 46-47 (jump_core enterState)
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
      vel: { x: 0, y: 0, z: 0 },                  // body+0x40 (units per tick; FUN_003590c8)
    };
    this.vel = { x: 0, z: 0 };                    // C+0xc0 x C+0xcc, units per 60 Hz tick
    this.speed = 0;                               // C+0x1d8 / 60 (units per tick)
    this.onGround = true; this.hitWall = false; this.stuck = false;
    this.vy = 0;                                  // airborne vertical speed (units per tick, +y down)
    this.ticks = 0;                               // 60 Hz action ticks
    this.frames = 0;                              // game frames
    this.actAcc = 0;
    this.stickW = { x: 0, z: 0, mag: 0 };         // this frame's stick (world, magnitude), stickInput
    this.jump = new Jump();                       // the jump (jump_core.js): FUN_00575768 + motion states 0x20 / 0x21
    this.jumpStepped = false;
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
    let loco = !this.active, owned = false, ate = false;
    this.actAcc += STEP;
    while (this.actAcc >= 1) {
      this.actAcc -= 1;
      ctx.tick = this.ticks;
      if (!this.active) {
        for (const a of this.actions) {
          if (!a.canEnter(ctx)) { if (a.ate) ate = true; continue; }   // ✕ taken, no mount (FUN_00333150 bVar2)
          this.active = a;
          if (this.jump) this.jump.ladderMount(this);
          if (this.weaponHook && this.weaponHook.actionEnter) this.weaponHook.actionEnter(a);
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
        if (res.done && this.active === a) {
          // GUESS: the ladder's own exit (its fall landed) back to state 2. The game's ladder states
          // 0xb / 0xc answer a motion-done event with state 10 (0x17 in the other FUN_00316cf8 branch;
          // FUN_00538ec8 / FUN_00538fa0), whose chain isn't decoded; see notes/fidelity-audit.md
          if (res.locomotion && this.onGround && this.P.state === 9) enterState(this.P, 2);
          this.active = null; a.exit(ctx);
        }
      }
      this.ticks++;
      ctx.input = { ...input, pressed: new Set() };   // an edge is seen by one tick only
    }
    ctx.input = input;
    if (owned) this.locate();                     // an action moved him off the body's polygon
    // weapon (aim.js): camera update + input (FUN_0032ace0 / FUN_003320c0) before the intent
    // (the camera and FUN_00579570 run in every state, FUN_005786e8; actions pause the buttons and clips)
    if (this.weapon && this.weaponHook) this.weaponHook.pre(input, !!this.active);
    // FUN_00333150: ✕ (logical id 0xd, press edge) goes to the ladder first (the action loop
    // above: an action that took the press, or owns him this frame, keeps it), then the jump
    // gate FUN_00577230 and FUN_00575768 (jump_core.js takeOff), before the stick writes.
    // input.cross: the pad's ✕ edge (trace replays); the page's ✕ is Space (ladder.js KEY).
    const cross = input.cross ?? !!(input.pressed && input.pressed.has(CROSS_KEY));
    this.jump.takeOff(this, cross, { ladder: owned || ate || !!this.active });
    this.stickInput(input);                       // FUN_00333150: runs in every state
    stickMode(this, this.stickW);                 // FUN_00333150: P+0x29c (the back jump's turn-round bit)
    this.intent(owned);                           // FUN_00578748: the ramp runs in every state
    // FUN_00579570 (weapon state machine) runs right after FUN_00578748 in FUN_005786e8, before the
    // character's motion update: a shot fired in the air switches the motion to the air-shot class
    // 0x4e now (FUN_00577480 -> FUN_00560b58; jump_core airShot), whose phase 0 runs below.
    if (this.weapon && this.weaponHook && this.weaponHook.weapon) {
      if (this.weaponHook.weapon(input, !!this.active)) this.jump.airShot(this);
    }
    // the jump / landing / air-shot / fall motion state (FUN_00598f50 / FUN_00599aa0 / FUN_00596830 /
    // FUN_00599820), before FUN_0034ea18. armed: C+0x590 bit 24 (gun out) and slot 0xbc loaded (it
    // is in z128: aim_g plays h_jp0_a); FUN_00598f50 / FUN_00599aa0 / FUN_00599820 then play the
    // h_jp*_a sets.
    this.jump.motionUpdate(this, { armed: !!(this.weapon && this.weapon.drawn()), action: !!this.active });
    const frozen = this.jump.frozen;              // C+0x1c0 bit 0x20: FUN_0034ea18 returns at once
    if (loco && !frozen) this.moveTick();         // FUN_0034ea18 -> FUN_0034c388 case 0xc
    if (loco && !frozen) this.rotate();           // FUN_0034ea18 -> FUN_0034de50 mode 4 (6 with the weapon out)
    if (this.weapon && this.weaponHook) this.weaponHook.post(input, !!this.active);   // the state classes' clips
    this.jumpStepped = false;
    if (ctx.anim && ctx.anim.gameFrame) ctx.anim.gameFrame(this, STEP);   // FUN_00343160 / FUN_005aa5b8
    if (!this.jumpStepped) this.jumpAdvance();    // no animator (tests): the jump clips' clock still runs
    this.frames++;
  }

  // FUN_005aa5b8 on the jump's head-layer clock (the animator calls it between its draw and its advance)
  jumpAdvance() { this.jump.advance(STEP); this.jumpStepped = true; }

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
        // GUESS: an action that drops him (ladder jump-off) hands over in the fall state 9, so the
        // first fall frame is not an on-foot frame. The game's ladder states (0xb / 0xc) leave by
        // motion-done events to state 10 (FUN_00538ec8 / FUN_00538fa0), not decoded further; see
        // notes/fidelity-audit.md
        if (this.P.state >= 2 && this.P.state <= 5) enterState(this.P, 9);
      } else if (this.P.state === 9) {
        enterState(this.P, 2);                        // back on his feet (same ladder GUESS; a ladder exit)
      }
    }
    if (res.vy !== undefined) this.vy = res.vy;
    // (same ladder GUESS: the ladder states' body+0x40 isn't decoded; zeroed with the page's vel)
    if (!res.keepVel) { this.vel.x = 0; this.vel.z = 0; this.body.vel = { x: 0, y: 0, z: 0 }; }
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
    this.stickW = { x: v ? v.x : 0, z: v ? v.z : 0, mag };
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
    this.setMove(w, d);                           // FUN_0034af98
    // P+0x29c = 3 (weapon out; aim_core.js): no turn request, rotation mode 6 (FUN_0034e960(C, 6)):
    // FUN_0034de50 case 6 turns toward C+0x2b0 - C+0x30 at rate C+0x2cc = 0.5. C+0x2b0 = the face
    // point (FUN_003424c0 -> FUN_0034b5e0). GUESS: its sideways offset (C row 0 x 0.25..1.25 by
    // C+0x664, FUN_003424c0) is left out. The target is flattened: row 2 y stays 0 in every
    // aim trace record.
    if (this.aimTurn()) return;
    // P+0x29c = 2 (mode set by FUN_005747f8 in FUN_00333150); C+0x568 bit 0x1000 clear
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
    // FUN_0034a968: v = body+0x40 (horizontal velocity, units per tick). It is last frame's
    // C+0xc0 x C+0xcc except where FUN_003590c8 left it alone (body states 1 / 3, a frozen frame)
    // or the state-3 crossing zeroed it.
    let vx = this.body.vel.x, vz = this.body.vel.z;
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
    // LAB_00579288: with P+0x29c = 5 (the back jump, jump_core.js stickMode) the turn target is
    // negated, so he keeps facing away from the stick (jump_core.js backTurnFix, folded in here
    // so a mode-6 aim turn below still replaces it)
    const td = P.mode29c === 5 ? { x: -d.x, y: -d.y, z: -d.z } : d;
    this.setTurn(0.25, td);                       // FUN_0034b638(0.25, ...) in jump states
    // the air branch doesn't change the rotation mode: with the weapon out it stays 6, whose
    // case in FUN_0034de50 rewrites C+0x2c0 / C+0x2cc (traced: aim_g, the turn to the aim
    // point holds through the jump)
    this.aimTurn();
  }

  // rotation mode 6 (weapon out): turn toward the face point at 0.5 (see intent)
  aimTurn() {
    const wpn = this.weapon;
    if (!(wpn && wpn.isAiming())) return false;
    const fp = wpn.P.facePt;
    this.setTurn(0.5, fp ? norm3({ x: fp.x - this.pos.x, y: 0, z: fp.z - this.pos.z }) : this.C.face);
    return true;
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
    // (the player state follows in the next motion update: jump_core motionUpdate / stateEvent)
    this.vel = { x: C.dir.x * C.spd, z: C.dir.z * C.spd };
    this.speed = C.groundSpeed / 60;
    this.stuck = this.onGround && C.spd > 0 && prev.x === this.pos.x && prev.z === this.pos.z;
  }

  // FUN_003590c8 (physics-body step) for z128's body flags 0x236: bit 5 set, bits 19-21
  // clear (velocity body+0x40 = body+0x30 at once, no accel or turn limit), bit 12 clear,
  // bits 7 / 14 clear (position = the walker's result). States 0 (ground), 2 (air) and
  // 3 (the slide down a 0x10 polygon after landing on it).
  integrate(v) {
    const B = this.body, col = this.col;
    const p = this.pos;
    // body flag bit 5 set, bits 19-21 clear: body+0x40 = body+0x30 at once, but not in states 1 / 3
    if (B.state !== 1 && B.state !== 3) B.vel = { x: v.x, y: v.y, z: v.z };
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
    if (B.state === 3) {
      // first switch case 1 / 3 (0x3595fc): FUN_00357b70 = t += DAT_004eeb28, body+0xdc =
      // v0 t + g t^2 / 2 (no +0.01)
      const g = GAME.gravity;
      B.t = f32(B.t + DT);
      B.dy = f32(f32(B.v0 * B.t) + f32(f32(f32(g * B.t) * 0.5) * B.t));
      // move = (body+0x40 projected on the plane's horizontal line (-n.z, 0, n.x) by
      // FUN_0025a778(0, ..)  +  (0, h, 0) taken along (n.x, 0, n.z) onto the plane through 0 by
      // FUN_002606b8) x 1.01; h = (v0 + g t) DAT_004eeb28 + g (DAT_004eeb28 / 2) DAT_004eeb28.
      // body+0x40 is NOT scaled by the step here (vmove, then FUN_0025a778).
      const n = col.polys[B.poly].plane;          // body+0x120
      const ul = Math.hypot(n[2], n[0]);
      const ux = -n[2] / ul, uz = n[0] / ul;
      const bv = B.vel;                           // body+0x40, kept from the landing frame
      const s = bv.x * ux + bv.y * 0 + bv.z * uz;
      const h = f32(f32(f32(B.v0 + f32(g * B.t)) * DT) + f32(f32(g * f32(DT * 0.5)) * DT));
      const k = (n[1] * h) / (n[0] * n[0] + n[2] * n[2]);
      const mx = f32((ux * s - n[0] * k) * 1.01), my = f32(h * 1.01), mz = f32((uz * s - n[2] * k) * 1.01);
      const r = col.navMove(B.poly, p.x, B.gy, p.z, mx, mz, false, { state3: { dy: my } });
      this.hitWall = r[4];
      B.poly = r[3];
      if (r[6]) {                                 // the crossing (0x358afc): FUN_00357860(body, 0),
        B.state = 0;                              // body+0x40 = 0 (so next frame's air control
        B.vel = { x: 0, y: 0, z: 0 };             // FUN_00578748 starts from 0: trace f558, 0.09)
      }
      // second switch case 3 (or 0 after the crossing): pos = body+0x10
      this.pos = { x: chop(r[0]), y: chop(r[1]), z: chop(r[2]) };
      B.gy = r[1];
      return;
    }
    // state 2: move = (body+0x40 + body+0x60) x step (body+0x60 = 0, FUN_00357168 clears it)
    this.gravityStep();                           // FUN_003579a8
    const y = f32(B.y0 + B.dy);                   // body.y = body+0xd8 + body+0xdc
    // FUN_00357d48 walks the ground point body+0x10 (airborne: collision.js body.air, the
    // ground point's rise capped by the vertical speed (v0 + g t) x DAT_004eeb28)
    const vdt = f32(f32(B.v0 + f32(GAME.gravity * B.t)) * DT);
    const r = col.navMove(B.poly, p.x, B.gy, p.z, v.x * STEP, v.z * STEP, true, { air: { bodyY: y, vdt } });
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
      // polygon flags (0x359b64..): 0x10 -> FUN_00357860(body, 3); else 0 (0x04 / 0x100 have
      // their own paths in the game; absent in z128)
      B.state = (col.polys[B.poly].flags & 0x10) ? 3 : 0;
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
    // the installed clip table C+0x5f4 (FUN_00345b58): { key, thr, root } per entry. Default =
    // the locomotion table (GAME.loco, keys idle/walk/run); aim.js installs the aimed tables
    // (FUN_00595440) with clip names as keys.
    this.table = GAME.loco;
    this.request = null;                          // a one-shot motion request on the head layer
    this.current = 'idle';
    this.actions.idle.play();
    this.rate = 0.5;                              // MP+0x40c
    // game clip state: head layer (key, frame) and the blend-out layer
    // loop = the layer's play flag +0x1e bit 0 (FUN_005aa2a8: set = wrap to 0 at n, clear = hold n - 1);
    // the table clips play looped (traced lflags 0x87), the jump clips per request (h_jp4: 6)
    this.head = { key: 'idle', frame: 0, prevFrame: 0, loop: true };
    this.out = null;                              // { key, frame, prevFrame, loop }
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

  // an action for a clip key (lazily for table / request clips from the packs)
  ensure(key) {
    if (this.actions[key]) return true;
    const clip = this.library.get(key);
    if (!clip) return false;
    const a = this.mixer.clipAction(clip);
    a.setLoop(THREE.LoopRepeat, Infinity);
    a.timeScale = 0;
    // the clip's own frame count (pack metadata `frames`): a one-shot has keys 0..n-1 and no
    // closing key, so its glb duration is (n - 1)/60 (h_jp4: 14 vs the game's 15, which made the
    // blend-out layer wrap one frame early)
    this.actions[key] = a; this.names[key] = key;
    this.frames[key] = this.clipFrames?.get(key) ?? Math.round(clip.duration * 60);
    return true;
  }

  // FUN_00345b58: install a clip table (resets the current index C+0x659 = -1, so the next
  // choice starts its clip at frame 0). null = the default locomotion table.
  setTable(entries, speed = 0) {
    const t = entries || GAME.loco;
    if (t === (this.table || GAME.loco)) return;
    for (const e of t) if (!this.ensure(e.key)) return false;
    this.table = t; this.cur = -1;
    if (entries) {
      // an aimed table: FUN_00345b58 installs it (C+0x658 > 0); keep the locomotion table's own
      // install flag (the traced install-at-5-u/s rule, gameFrame) for when it comes back
      if (this.locoInstalled === undefined) this.locoInstalled = !!this.installed;
      this.installed = true;
    } else if (this.locoInstalled !== undefined) {
      this.installed = this.locoInstalled; this.locoInstalled = undefined;
      // holster: the locomotion choice runs at once at the current speed (traced: h_st standing,
      // aim_e f221; h_rn while running, aim_h_holster_run f91), even before the table's first
      // install (that rule stays: without it no further choice runs until 5 u/s).
      // GUESS: the game's install path on holster isn't decoded; this matches both traces.
      if (!this.installed) this.switchTo(this.pick(speed));
    }
    return true;
  }

  // a one-shot motion request on the head layer (FUN_00345818 from a state class, e.g. the
  // stand-fire class FUN_00596460: h_st_f). The table choice pauses until clearRequest().
  // Blends over C+0x591 + 1 like a table switch; ends (FUN_003460d8) on the last frame.
  playRequest(key) {
    if (!this.ensure(key)) return false;
    this.out = { key: this.head.key, frame: this.head.frame, prevFrame: this.head.frame, loop: this.head.loop !== false };
    this.head = { key, frame: 0, prevFrame: 0, loop: false };
    this.blend = { len: GAME.blendFrames + 1, rem: GAME.blendFrames + 1, w: 0 };
    this.request = { key, ended: false };
    this.current = key;
    return true;
  }
  clearRequest() { if (this.request) { this.request = null; this.cur = -1; } }

  // meta: the pack's clips table (vincent_<pack>.json `clips`, { name: { frames } })
  addClips(clips, meta = null) {
    this.clipFrames ??= new Map();
    if (meta === null && clips.length) console.warn('clip pack has no clips table (old export? re-run export_web_vincent.py --pack): one-shot clips will end one frame early');
    for (const c of clips) {
      if (this.library.has(c.name)) continue;
      this.library.set(c.name, c);
      const n = meta?.[c.name]?.frames;
      if (Number.isInteger(n) && n > 0) this.clipFrames.set(c.name, n);
    }
  }
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
    const L = this.table || GAME.loco;
    const thr0 = Math.max(L[0].thr, 0.01);        // 0.75 with C+0x564 bit 0x800 (clear)
    const hyst = this.cur === 2 ? -0.1 : 0.0;
    if (s <= thr0) return 0;
    // the index is clamped to C+0x658 - 1 (entry count): a 1-entry table always picks 0
    if (L.length > 1 && s <= f32(L[1].thr + hyst)) return 1;
    return L.length - 1;
  }

  // one game frame (called by PlayerController.step after the character tick)
  gameFrame(ctrl, step) {
    if (this.override) {                          // an action's clip plays
      if (this.drawHook) this.drawHook();
      return;
    }
    const J = ctrl.jump;
    if (J && J.head && J.clock) return this.jumpFrame(ctrl, J.clock, step);
    const s = ctrl.C.groundSpeed;
    if (this.jumpHeld) {
      // the landing motion ended last frame (FUN_00599aa0 -> motion 3 / 8): the locomotion clip
      // choice FUN_00346860 runs from a fresh index (C+0x659 = -1: frame 0, blend C+0x591 + 1).
      // Traced: j_stand f78 h_st 0.6 with blend 11 and h_jp4 (14.0) blending out.
      this.jumpHeld = false; this.cur = -1;
      this.switchTo(this.pick(s));
      // before the table's first install (the 5 u/s GUESS below) the index stays fresh: the first
      // choice after the install starts at frame 0 (e_jumps f123: h_wk 0.3 after the landing)
      if (!this.installed) this.cur = -1;
    }
    // GUESS: when the game installs the table (C+0x658 = 3) isn't decoded (its writer for
    // the player wasn't found in two attempts; FUN_00345b58 is only called from NPC state
    // classes). Traced: installed in the frame C+0x1d8 first reaches 5.55 (ramp byte 87,
    // the third frame of stick); 3.70 the frame before doesn't install it.
    if (!this.installed && s >= 5.0) this.installed = true;
    let rate = this.rate;
    if (this.request) {
      rate = 0.5;                                 // FUN_003456c0 / FUN_00345818 rate 1.0 -> MP+0x40c 0.5
    } else if (this.installed || (this.table && this.table !== GAME.loco)) {
      const idx = this.pick(s);
      if (idx !== this.cur) this.switchTo(idx);
      // FUN_00346c10 (C+0x564 = 0x19: bits 1, 8, 0x10)
      const run = (GAME.locoFlags & 8) !== 0 && this.cur === 2;
      const root = (this.table || GAME.loco)[this.cur].root;     // C+0x65c
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
    const req = this.request;
    const end = this.advLayer(this.head, rate, step);
    if (req && this.head.key === req.key && end) req.ended = true;   // one-shot: holds n - 1
    if (blending) {
      this.advLayer(this.out, rate, step);
      b.rem = f32(b.rem - step);
      if (b.rem <= 0) b.rem = 0;
    }
  }

  // FUN_005aa2a8 on one layer: frame += rate x weight/128 x step; a looped layer (+0x1e bit 0)
  // resets to 0 at n, any other holds n - 1 (traced: j_stand f78-89, the blend-out h_jp4 stays
  // at 14.0 while h_st blends in). Returns 1 at the end, like the game.
  advLayer(L, rate, step) {
    L.prevFrame = L.frame;
    L.frame = f32(L.frame + f32(f32(rate * 1.0) * step));   // weight byte 128 / 128
    const n = this.frames[L.key];
    if (L.loop === false) {
      if (n - 1 <= L.frame) { L.frame = n - 1; return 1; }
    } else if (n <= L.frame) { L.frame = 0; L.prevFrame = 0; return 1; }
    return 0;
  }

  // the jump's motion states own the head layer (jump_core.js FUN_00598f50 / FUN_00599aa0): its
  // ClipClock (requests: now = replace the head with blend req.blend, else queued; the hold /
  // queue rule is jump_core's GUESS) is the head; a blend > 0 keeps the old head as the
  // blend-out layer over blend + 1 (j_double: h_jp3 shows 5). Blend 0 = a cut (traced: every
  // h_jp request has blend_frames 0).
  jumpFrame(ctrl, clock, step) {
    this.jumpHeld = true;
    this.request = null;                          // the motion state replaced the head (a fire request's class is gone)
    this.rate = 0.5;                              // FUN_003456c0 rate 1.0 -> MP+0x40c 0.5 (jump_core clipRate)
    const take = () => {
      if (clock.serial !== this.jumpSerial || clock !== this.jumpClock) {   // a new Jump (respawn) restarts the count
        this.jumpSerial = clock.serial; this.jumpClock = clock;
        // GUESS: a requested clip the page doesn't have (the jump pack not loaded: every clip the
        // jump motions request is in it) keeps the current head pose; the clock still times it
        // (notes/fidelity-audit.md "Jump")
        if (!this.ensure(clock.clip)) return;
        const len = clock.blend > 0 ? clock.blend + 1 : 0;
        this.out = len ? { key: this.head.key, frame: this.head.frame, prevFrame: this.head.frame, loop: this.head.loop !== false } : null;
        this.blend = { len, rem: len, w: len ? 0 : 1 };
        this.head = { key: clock.clip, frame: clock.frame, prevFrame: clock.frame, loop: !!clock.loop };
        this.current = clock.clip;
      } else if (this.head.key === clock.clip) {
        this.head.prevFrame = this.head.frame; this.head.frame = clock.frame;
      }
    };
    take();                                       // this frame's requests (applied before the move)
    const b = this.blend, blending = this.out && b.rem > 0;
    if (blending) b.w = (b.len - b.rem) / b.len;  // MP+0x408
    else { b.w = 1; this.out = null; }
    if (this.drawHook) this.drawHook();
    if (blending) {
      this.advLayer(this.out, this.rate, step);
      b.rem = f32(b.rem - step);
      if (b.rem <= 0) b.rem = 0;
    }
    const before = this.head.frame, serial = clock.serial;
    ctrl.jumpAdvance();                           // FUN_005aa5b8 on the head layer
    // a queued clip took over: FUN_005aa2a8 returned 1 with the old (non-looped) layer held at
    // n - 1, then FUN_005aa5b8 stepped to the queued layer (traced: j_double f65, c_wjp2 8.0 blending out)
    if (clock.serial !== serial && this.head.loop === false && this.frames[this.head.key] !== undefined)
      this.head.frame = this.frames[this.head.key] - 1;
    take();                                       // a queued clip taking over (frame 0) or the next frame
    if (this.head.frame < before) this.head.prevFrame = this.head.frame;   // loop wrap: no interpolation back
  }

  // FUN_00346860: a new table index
  switchTo(idx) {
    const prevKey = this.head.key, first = this.cur === -1;
    const key = (this.table || GAME.loco)[idx].key;
    let frame = 0;
    if (!first) frame = Math.min(Math.max(this.head.frame * 2, 0), this.frames[key] * 2 - 1) * 0.5;  // FUN_005a8fd0
    const len = (idx === 0 && !first ? GAME.blendToIdle : GAME.blendFrames) + 1;
    this.out = { key: prevKey, frame: this.head.frame, prevFrame: this.head.frame, loop: this.head.loop !== false };
    this.head = { key, frame, prevFrame: frame, loop: true };
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
    // Clips without local-pose records (an old export, or a pack exported before packs carried
    // vincent_<pack>_local.bin) keep the mixer's model-space mix (notes/fidelity-audit.md)
    const hasLP = k => !!this.lp.clips[this.names[k]];
    if (this.lp && !this._fadeOut && hasLP(this.head.key) && (!this.out || hasLP(this.out.key)))
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

  // a clip pack's local TRS (vincent_<pack>.json local_pose + vincent_<pack>_local.bin,
  // export_web_vincent.py --pack; same record format): its clips join the table and read
  // their own file. A pack for another bone count is refused.
  addLocalPose(info, buffer) {
    const L = this.lp;
    if (!L) return false;
    if (info.bones !== L.n) throw new Error(`local pose: pack has ${info.bones} bones, rig ${L.n}`);
    const data = new Float32Array(buffer);
    for (const [name, c] of Object.entries(info.clips))
      if ((c.offset + c.frames * L.n) * 10 > data.length) throw new Error(`local pose: ${name} runs past the file (truncated?)`);
    for (const [name, c] of Object.entries(info.clips)) if (!L.clips[name]) L.clips[name] = { ...c, data };
    return true;
  }

  // FUN_005af6c0 at a fractional clip frame: t/s lerp, q nlerp with hemisphere flip between
  // the bracketing keys (the export samples every integer frame, so the keys are frames).
  sampleLocal(name, frame, out) {
    const L = this.lp, c = L.clips[name], d = c.data || L.data, n = L.n;
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
// The weapon views' type objects (DAT_004eed98[type]; runtime values traced in z128,
// TRACE_FIELDSET=aim camtype_d / camtype_e). FUN_0032a420 maps view mode CAM+0x21c to a type:
// 1 -> 0xc, 2 -> 0xd (weapon), 3 -> 0xe (first person), 4 -> 0xf (scope; not on the page).
// 0xd: its update FUN_00330980 writes pivot x = CAM+0x1a0 every frame; CAM+0x1a0 eases to
// DAT_0046e348 = 5 at FUN_00323318(0.25) (FUN_0032ace0) and is 5 in the state, so 5 here.
// 0xe: the values after its enter FUN_00330d18 (FOV = DAT_0046e3f0).
export const CAM_TYPE_D = { pivot: [5, -16.25, 0], offset: [0, 0, -35], rate: [0.05, 0.05], fov: 73.5 };
export const CAM_TYPE_E = { pivot: [0, -16.25, -0.1], offset: [0, 0, -0.1], rate: [0.05, 0.05], fov: 73.5 };
export const CAM_TYPES = { 0xc: CAM_TYPE_C, 0xd: CAM_TYPE_D, 0xe: CAM_TYPE_E };
const LOOK_TABLE = 1.0;                           // DAT_004a0650[DAT_004e1058 = 8] (view mode 1; option value in the state)
const LOOK_TABLE_W = 0.66;                        // DAT_004a0608[DAT_004e1060 = 8] (view modes 2-4)
const GAIN_DECAY_W = 0.06;                        // DAT_0046e354: FUN_00330980 / FUN_00330f08 auto-turn gain decay
const EASE_OUT_FAST = 0.2;                        // DAT_0046e508: distance ease-out with CAM+0x1e8 0x100000 / near
const KICK_SCALE = 1.0;                           // DAT_0046e43c (FUN_0032a330)
const BASE_RATE_UP = [0.125, 0.5, 0.125];         // CAM+0x150 (base moving down, dy > 0)
const BASE_RATE_DN = [0.125, 0.15, 0.125];        // CAM+0x160
// CAM+0x150 / +0x160 as each type's enter writes them (FUN_00330240 0xc: DAT_0046e330/334/338;
// FUN_00330748 0xd: DAT_0046e33c/340/344 = 0.3 / 0.5 / 0.15 when FUN_00326500() (true);
// FUN_00330d18 0xe: (1, 0.5, 1) both). The rates stay until the next enter, i.e. follow the type.
const BASE_RATES = { 0xc: [BASE_RATE_UP, BASE_RATE_DN], 0xd: [[0.3, 0.5, 0.3], [0.3, 0.15, 0.3]],
                     0xe: [[1.0, 0.5, 1.0], [1.0, 0.5, 1.0]] };
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
// Page input plumbing (not game logic): the browser delivers mousemove once per rendered
// frame, the game reads its USB counts every game frame (FUN_00254ac8, DAT_004e1230 /
// DAT_004e1234). The counts gathered since the last game frame are shared out evenly over
// the game frames the page runs for this rendered frame, so a frame rate under 50 does not
// leave game frames with 0 counts: in mouse mode a 0-count frame drops the ramp CAM+0x1ec
// straight to 0 (FUN_0032ace0, LAB_0032b950 with FUN_00254db0() != 0) and the turn crawled.
// GUESS: the even split (a real mouse's per-frame counts are not decoded; see the audit).
export class MouseFeed {
  constructor() { this.dx = 0; this.dy = 0; }
  add(dx, dy) { this.dx += dx; this.dy += dy; }
  // before each game frame; left = game frames still to run for this rendered frame
  feed(cam, left) {
    const k = 1 / Math.max(1, left), x = this.dx * k, y = this.dy * k;
    this.dx -= x; this.dy -= y;
    if (x !== 0 || y !== 0) cam.addMouse(x, y);
  }
}
// OPTION mouselook: play.html?mouselook=direct (off by default, not in the game). Mouse
// pixels turn yaw / pitch directly by `sens` rad per pixel (?mousesens= in degrees per pixel,
// default 0.15), skipping the look stick (clamp, / step, ramp, deadzone) for the mouse only;
// a frame with mouse movement counts as turning for the idle counter CAM+0x1e2, as the
// stick does. Pitch limits and everything else stay the game's.
export const MOUSE_SENS_DEFAULT = 0.15;

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
// rows from a unit forward with row 1 = (0, 1, 0) (FUN_0032ace0 l.2127-2140): right = up x f
const basisFromForward = f => {
  const h = Math.hypot(f[0], f[2]) || 1, r = [f[2] / h, 0, -f[0] / h];
  return [r, [f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]], f];
};
const easeSin = x => (x < 0 ? 0 : x > 1 ? 1 : Math.sin(x * 1.5707964));   // FUN_00260d44
const DAT_0046E4CC = 0.8;                         // recoil share of the view pitch (l.1762)
const NEAR_A220 = 25.0;                           // DAT_0046e4d0: FUN_0032a1d8's near distance (aim_core camJumpFrame)

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
    this.mouseDirect = null;                      // OPTION mouselook: { sens } (rad per pixel) or null = the game's path
    this.hit = false;                             // CAM+0x1e8 0x80: the collision hit last frame
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
    // reset: FUN_00329f68 / FUN_003285d0 don't run FUN_0032a1d8 and leave CAM+0x220 as it is
    const g = this.place(this.base, this.base, { recede: 0, state: 0, L: 0, reset: true });
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
  // this.weapon (aim.js): aim_core.js WeaponSystem; its cam holds the view mode CAM+0x21c, the
  // type CAM+0x28, the switch ramp CAM+0x17c, flags, recoil and kick. Without it: type 0xc, view 1.
  tick(p) {
    const W = this.weapon || null, wc = W ? W.cam : null, S = CSTEP;
    const T = wc ? CAM_TYPES[wc.type] : this.type;
    const view1 = !wc || wc.view === 1;           // uStack_15c = (FUN_0032a0f0() == 1)
    this.prev = { eye: this.eye.slice(), at: this.at.slice(), f: this.M[2].slice() };
    const L = p.stick || 0;
    if (wc && wc.e1e2 != null) { this.idle = wc.e1e2; wc.e1e2 = null; }   // a type enter's CAM+0x1e2 (aim_core enterType)
    if (!wc || wc.type === 0xc) {
      // FUN_00330478: auto-turn gain from the left stick
      this.gain = clampf(L > 0.5 ? (L - 0.5) * S : this.gain - S * 0.3, 0, 1);
    } else {
      // FUN_00330980 (0xd) / FUN_00330f08 (0xe): the gain only decays
      this.gain = clampf(this.gain - GAIN_DECAY_W * S, 0, 1);
    }
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
        const [rUp, rDn] = wc ? BASE_RATES[wc.type] : BASE_RATES[0xc];
        const rr = 0.0 < d[1] ? rUp : rDn;
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
    // CAM+0x1e9 < 0 = flag 0x8000 (set while the type is 0xc, see the nudge below); clear (the
    // weapon views): no recede, and the angle change is zeroed too (l.404-432), which only the
    // nudge reads, gated by the same bit
    const f8000 = !wc || wc.type === 0xc;
    const recede = f8000 ? (n2c - len3(v2d)) * 1.0 : 0;   // x (CAM+0x1e1 = 60) / 60
    // look stick (FUN_003256d8: x|x|, y|y|, signs +1 / -1; mouse: counts x 0.1 / step)
    // FUN_00254ac8: mouse counts set the latch DAT_004e17d0; a pad stick past 0.3 (after
    // x|x|) clears it; the sum is clamped to +-1
    let sx = 0, sy = 0;
    const MD = this.mouseDirect;                  // OPTION mouselook: the mouse bypasses the stick
    const dmx = MD ? this.look[0] : 0, dmy = MD ? this.look[1] : 0;
    if (MD) this.look = [0, 0];
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
    if (sx === 0 && dmx === 0 && dmy === 0) this.idle = this.idle > 29 ? 30 : Math.min(30, this.idle + Math.trunc(S));
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
    // (view modes 2-4 read DAT_004a0608[DAT_004e1060] instead, uStack_15c == 0)
    const lt = view1 ? LOOK_TABLE : LOOK_TABLE_W;
    const ry = T.rate[0] * S * lt, rp = T.rate[1] * S * lt;
    this.yaw = wrapA(this.yaw + sx * ry);
    this.pitch += sy * rp;
    // OPTION mouselook: direct mouse turn (x right = yaw +, y down = pitch -, as the stick's signs)
    if (MD && (dmx !== 0 || dmy !== 0)) { this.yaw = wrapA(this.yaw + dmx * MD.sens); this.pitch -= dmy * MD.sens; }
    // follow nudge (CAM+0x1e8 bit 0x8000): turn by the look point's angle change, capped.
    // The bit is set by type 0xc's enter FUN_00330240 (0x330368) and cleared by 0xd's
    // FUN_00330748 (0x330938) and 0xe's FUN_00330d18 (traced: set exactly while the type is 0xc)
    if (f8000 && this.idle > 0 && smag === 0 && !fun253f50()) {   // (OPTION mouselook: idle is -20 after a mouse turn)
      this.yaw += clampf((L * 0.2 + 0.8) * f30 * 1.0 * e2[1], -ry, ry);
      this.pitch += clampf(f30 * 0.25 * e2[0], -rp, rp);
    }
    // FUN_0032ace0 l.1509: a switch between views 2-4 (CAM+0x1e8 0x800, FUN_0032a420) adds the
    // angles FUN_0032aaf8 measured (CAM+0x1d8 / +0x1dc); with 0x100000 (first person -> weapon
    // view) the switch ramp CAM+0x17c is set to 1 here, so it ends this frame
    if (wc) { const a = W.switchAngles(); this.pitch += a.pitch; this.yaw += a.yaw; }
    this.pitch = clampf(this.pitch, PITCH_MIN, PITCH_MAX);
    // l.1687-1762: the reticle offset, switch ramp, fade, FOV and recoil (aim_core.js cameraFrame)
    if (W) W.cameraFrame(this.pitch);
    if (wc) FollowCamera.shoulderPre(wc);
    // place the eye: offset, collision, switch lerp, distance ease, eye kick
    const g = this.place(base, old, { recede, state: p.state, L, smag, view1,
                                      fast: !!wc && (wc.flags & 0x100000) !== 0,
                                      e: wc ? wc.e : 1,
                                      recoil: wc ? DAT_0046E4CC * wc.k198 * (wc.fov / BASE_FOV) : 0,
                                      kick: wc ? KICK_SCALE * wc.kick : 0 });
    this.eye = g.eye; this.at = g.at; this.M = g.M;
    if (wc) FollowCamera.shoulderPost(wc);
    if (W) W.kickDecay();                         // FUN_0032a348, after the eye kick
  }

  // FUN_00330478 (type 0xc) sets CAM+0x1a4 = 0 before FUN_0032ace0; FUN_00330980 (0xd) writes
  // its pivot x = CAM+0x1a0 (CAM_TYPE_D.pivot[0], 5 = DAT_0046e348 at rest)
  static shoulderPre(wc) { if (wc.type === 0xc) wc.a1a4 = 0; }
  // l.2256-2264: CAM+0x1a0 eases to 5 at FUN_00323318(0.25); t = (x - 5) / (2 - 5);
  // CAM+0x1a4 = -0.75 x FUN_00260d44(2t) below t = 0.5, else -0.75
  static shoulderPost(wc) {
    wc.a1a0 = (5.0 - wc.a1a0) * stepFrac(0.25) + wc.a1a0;
    const t = (wc.a1a0 - 5.0) / (2.0 - 5.0);
    wc.a1a4 = t < 0.5 ? -0.75 * easeSin(t + t) : -0.75;
  }

  // the eye for this frame (FUN_0032ace0 from the matrix build on); ctx = null: no
  // collision or easing (reset)
  place(base, old, ctx) {
    const T = this.weapon ? CAM_TYPES[this.weapon.cam.type] : this.type;
    // final pitch: + atan(k tan(fov x 0.875 / 2)) - atan(k tan(BASE_FOV x 0.875 / 2)), 0 while
    // the type FOV = DAT_0046e3f0 (both 73.5 in z128); CAM+0x188 x +0x180 = 0 (no blend)
    const g0 = this.pitch * -0.4774648, x = Math.abs(g0) + 0.3;
    const kk = g0 * (x > 1 ? 1.0 : 1 - Math.cos(x * 1.5707964)); // FUN_00260cf4
    // + 0.8 x CAM+0x198 x CAM+0x170 / DAT_0046e3f0 (recoil, l.1762; ctx.recoil, 0 without the weapon)
    const pf = this.pitch + Math.atan(kk * Math.tan(T.fov * 0.875 * DEG * 0.5))
                          - Math.atan(kk * Math.tan(BASE_FOV * 0.875 * DEG * 0.5));
    const RX = rotX(pf + (ctx ? ctx.recoil || 0 : 0)), RY = rotY(this.yaw), M = mm(RX, RY);
    let off = T.offset.slice();
    // l.1774 (FUN_00326500() true; on the page: the weapon translation is loaded): the offset moves
    // toward the gun-out jump's by CAM+0x220 (FUN_0032a180), then CAM+0x220 steps (FUN_0032a1d8,
    // fed |left stick| + |look stick| and "near" = CAM+0x1c0 < 25 with CAM+0x1e8 0x80, the last
    // frame's collision hit). Before the y scale (l.1795)
    if (ctx && this.weapon) {
      off = this.weapon.jumpOffset(off);
      if (!ctx.reset) this.weapon.camJumpFrame((ctx.L || 0) + (ctx.smag || 0), this.dist < NEAR_A220 && this.hit, CSTEP);
    }
    off[1] *= Math.abs(RX[1][1]) * 0.8 + 0.2;
    const eoff = vm(off, M);
    // l.1812: pivot y + CAM+0x1a4 (0 except the frame after leaving first person, FUN_00331160)
    const a4 = this.weapon ? this.weapon.cam.a1a4 : 0;
    const piv = [T.pivot[0], T.pivot[1] + a4, T.pivot[2] + (pf > 0 ? -4.0 : -3.0) * pf];   // DAT_0046e4dc / 0x46e4d8
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
      // CAM+0x1e8 0x80 at the end of FUN_0032ace0 (l.2488): a hit this frame (iStack_178, l.2029)
      this.hit = !!best;
      if (best) {
        const f = fw[0] * best.n[0] + fw[1] * best.n[1] + fw[2] * best.n[2];
        const push = Math.sqrt(9.0 * (1 - f * f));
        eye = [0, 1, 2].map(j => best.p[j] + fw[j] * push);
      }
    }
    // switch ramp (CAM+0x2c == 0, l.2088-2140): the eye (CAM+0x90) and the forward row
    // (CAM+0x80) move from last frame's toward this frame's by FUN_00260d44(CAM+0x17c) (taken
    // before the ramp update; FUN_002611f4 = lerp), the forward renormalised with row 1 = (0,1,0)
    let M2 = M;
    if (ctx.e !== undefined && ctx.e < 1) {
      const e = ctx.e, le = this.eye, lf = this.M[2], nf = M[2];
      eye = [0, 1, 2].map(j => le[j] + (eye[j] - le[j]) * e);
      const f = [0, 1, 2].map(j => lf[j] + (nf[j] - lf[j]) * e), fl = len3(f) || 1;
      M2 = basisFromForward(f.map(v => v / fl));
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
    // View mode 1 only (uStack_15c): when last frame's eye was within 17.5 of base + type
    // pivot (unrotated, param_1+0x10), the out rate is (0.2 - 0.03) x (17.5 - d) / 10 + 0.03,
    // as with CAM+0x1e8 0x100000 in any view (which gives 0.2); the state rule is view 1 only.
    let near = false, k23 = 1.0;
    if (ctx.view1 !== false && this.eye) {          // (constructor: no eye yet; reset: dist 1000, unused)
      const le = this.eye;
      const d17 = len3([base[0] + T.pivot[0] - le[0], base[1] + T.pivot[1] - le[1], base[2] + T.pivot[2] - le[2]]);
      if (d17 < 17.5) { near = true; k23 = (17.5 - d17) / 10.0; }
    }
    // for test_cam_traces.mjs: which branch ran, and the walk-away test's margin
    this.ease = { out: this.dist < dist, slowGap: null };
    if (this.dist < dist) {
      const r0 = (ctx.fast || near) ? (EASE_OUT_FAST - 0.03) * k23 + 0.03 : 0.03;
      let r = stepFrac(r0);
      const st = ctx.state;
      const slow = (st >= 2 && st < 6) || st === 0x13 || st === 0x14 || st === 0x1d || st >= 0xc0;
      if (ctx.view1 !== false && slow && 20.0 < dist) {
        const dOld = len3([eye[0] - old[0], eye[1] - old[1], eye[2] - old[2]]);
        const dNew = len3([eye[0] - base[0], eye[1] - base[1], eye[2] - base[2]]);
        r *= dOld < dNew ? 0.1 : ctx.L * 0.9 + 0.1;
        // pinned on a wall the base barely moves, dOld ~ dNew, and the game's f32 / VU rounding
        // decides the x0.1 (traced: it flips every other frame)
        this.ease.slowGap = dNew - dOld;
      }
      this.dist += (dist - this.dist) * r + fz * ctx.recede;
      eye = [0, 1, 2].map(j => ref[j] + u[j] / dist * this.dist);
    } else {
      this.dist = dist + ctx.recede;
    }
    // eye kick (l.2412, FUN_00326500() true in the state): eye += forward x FUN_0032a330()
    // (DAT_0046e43c x CAM+0x224); the kick then decays (FUN_0032a348, tick())
    if (ctx.kick) eye = [0, 1, 2].map(j => eye[j] + M2[2][j] * ctx.kick);
    return { eye, at, M: M2 };
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
