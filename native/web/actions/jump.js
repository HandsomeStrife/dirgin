// Jump action: Space (the game's button is X / cross, logical id 0xd).
//
// Values from notes/actions-research.md section 1 (FUN_00575768, FUN_003543b8, FUN_00598f50):
//   take-off 80 units/s up = 1.333 units/tick (-y), gravity 0.07 units/tick^2, no speed cap,
//   no air control: the horizontal ground velocity at take-off is kept for the whole jump.
//   Apex 1.333 / 0.07 = 19 ticks after take-off, 12.7 units up.
// Each tick moves him along the exact parabola (y += vy + g/2, then vy += g), so the
// heights sampled at whole ticks are the continuous ones (12.70 at tick 19).
// The game's order of "add gravity" and "move" inside a tick isn't decoded; the two plain
// orders give 13.36 / 12.03 at the apex, the exact parabola sits between them.
//
// Clips (h_jp*, vincent_jump.glb), timed in ticks, one clip frame per tick:
//   t 0..2   h_jp0 take-off crouch (3 f)
//   t 3..12  h_jp1 rise (10 f)
//   t 13..22 h_jp2 descend (10 f; its slow first half spans the apex at t 19)
//   t 23..   h_jp3 air loop (16 f) until the feet reach a floor
//   landing  h_jp4 (15 f): he stands for 14 ticks, or 6 when a move key is held
// A head bump cuts the rise short and starts h_jp2 at once.
//
// Collision while airborne (the same pieces player.js fall() uses):
//   walls    with the navmesh collision (z128 runs the physics-body mode), col.move(..,
//            {air: true}) walks the mesh, so boundary edges stop him. With an older
//            collision.js: col.capsulePush, and the velocity part into the wall is
//            reflected at -0.25 (walk-mode air probe, research: L)
//   ceiling  col.segment from the head now to the head after this tick, while rising;
//            a hit stops him just under it and he falls from there
//   floors   col.groundY at the new x,z from just above the feet before this tick; landing
//            when the feet reach it. The search starts at the old feet, so no floor between
//            the old and new height is skipped however fast he falls (no tunnelling).
// The mid-air second jump (c_wjp0-2, state 0x1f/0x21) isn't done: its clips aren't in any
// exported pack yet, and the gate (FUN_00575698) isn't decoded.
export const KEY = 'Space';
export const HELP = 'Space jump';

export const TAKEOFF = 80 / 60;                   // units/tick, upward (-y)
export const GRAVITY = 0.07;                      // units/tick^2, +y
const CLIPS = ['h_jp0', 'h_jp1', 'h_jp2', 'h_jp3', 'h_jp4'];
const T_RISE = 3, T_DESC = 13, DESC_LEN = 10;     // clip start ticks (see header)
const LAND_TICKS = 14, LAND_MIN = 6;              // h_jp4: (15 - 1) frames; early exit when moving
const BOUNCE = -0.25;                             // velocity into a wall after an air hit

export function jumpAction() {
  // per-jump state (one jump at a time); `state` is read by the tests and the HUD
  const s = { phase: 'idle', t: 0, vx: 0, vz: 0, vy: 0, y0: 0, apex: 0, apexTick: 0,
              descAt: T_DESC, landT: 0, bumped: false, landedY: null };
  const play = (ctx, name, loop = false, fade = 0.08) => {
    if (ctx.anim && ctx.anim.has(name)) ctx.anim.play(name, { loop, rate: 1, fade });
  };
  const moving = ctx => Math.hypot(ctx.input.x || 0, ctx.input.z || 0) > 1e-6;

  // one airborne tick: returns the move result
  function air(ctx) {
    const c = ctx.ctrl, col = ctx.col, p = c.pos;
    const { radius, height } = c.opts;
    let nx = p.x + s.vx, nz = p.z + s.vz;
    if (typeof col.navMove === 'function') {   // navmesh collision (FORMAT.md §3d): the airborne
      const r = col.move(p, { x: s.vx, z: s.vz }, { air: true });   // body keeps walking the mesh
      nx = r.x; nz = r.z;
    } else if (col.capsulePush) {         // older collision.js: capsule push + air wall bounce
      let hit;
      const bx = nx, bz = nz;
      [nx, nz, hit] = col.capsulePush(nx, p.y, nz, radius, height);
      const px = nx - bx, pz = nz - bz, pl = Math.hypot(px, pz);
      if (hit && pl > 1e-6) {                     // reflect the part of the velocity into the wall
        const ux = px / pl, uz = pz / pl, vn = s.vx * ux + s.vz * uz;
        if (vn < 0) { s.vx += (BOUNCE - 1) * vn * ux; s.vz += (BOUNCE - 1) * vn * uz; }
      }
    }
    let ny = p.y + s.vy + GRAVITY / 2;
    s.vy += GRAVITY;

    // head bump while rising: the head moves from p.y - height to ny - height
    if (ny < p.y && col.segment) {
      const h = col.segment({ x: nx, y: p.y - height, z: nz }, { x: nx, y: ny - height, z: nz });
      if (h) {
        ny = Math.min(p.y, h.point.y + height + 0.01);
        s.vy = 0; s.bumped = true;
        if (s.t < s.descAt) { s.descAt = s.t; play(ctx, 'h_jp2'); }
      }
    }

    // landing: the first floor at or below the old feet (a little above them while falling,
    // so a slope rising under him is met instead of passed through)
    let landed = false;
    if (s.vy > 0) {
      const g = col.groundY(nx, nz, p.y, Math.min(4, 1 + Math.hypot(s.vx, s.vz)));
      if (g !== null && ny >= g) { ny = g; landed = true; }
    }
    if (s.y0 - ny > s.apex) { s.apex = s.y0 - ny; s.apexTick = s.t + 1; }

    const delta = { x: nx - p.x, y: ny - p.y, z: nz - p.z };
    if (landed) {
      s.phase = 'land'; s.landT = 0; s.landedY = ny; s.vy = 0;
      c.vel.x = s.vx; c.vel.z = s.vz;             // after any wall bounce
      play(ctx, 'h_jp4', false, 0.06);
      return { delta, onGround: true, vy: 0, keepVel: true };
    }
    // fell out of the world: hand him to locomotion, whose fall respawns him
    if (ny > c.spawn.y + (c.killDepth ?? 500)) {
      c.vel.x = s.vx; c.vel.z = s.vz;
      return { delta, onGround: false, vy: s.vy, keepVel: true, done: true };
    }
    // clip timeline (s.t = ticks since take-off; this tick's clip shows from this tick)
    if (s.t === T_RISE && s.descAt > T_RISE) play(ctx, 'h_jp1');
    else if (s.t === s.descAt && !s.bumped) play(ctx, 'h_jp2');
    else if (s.t === s.descAt + DESC_LEN) play(ctx, 'h_jp3', true, 0.1);
    return { delta, onGround: false, vy: s.vy, keepVel: true };
  }

  return {
    name: 'jump',
    state: s,
    canEnter(ctx) {
      return !!(ctx.input.pressed?.has(KEY) && ctx.ctrl.onGround && ctx.anim
                && CLIPS.every(n => ctx.anim.has(n)));
    },
    enter(ctx) {
      const c = ctx.ctrl;
      Object.assign(s, { phase: 'air', t: 0, vx: c.vel.x, vz: c.vel.z, vy: -TAKEOFF, y0: c.pos.y,
                         apex: 0, apexTick: 0, descAt: T_DESC, landT: 0, bumped: false, landedY: null });
      return { clip: 'h_jp0', loop: false, rate: 1, fade: 0.06 };
    },
    update(dt, ctx) {
      let res;
      if (s.phase === 'air') {
        res = air(ctx);
      } else {                                    // landing: stand while h_jp4 plays
        s.landT++;
        const go = moving(ctx);
        const done = s.landT >= LAND_TICKS || (go && s.landT >= LAND_MIN);
        // keep the take-off velocity only if he walks on out of the landing
        res = { delta: { x: 0, y: 0, z: 0 }, onGround: true, vy: 0, keepVel: go, done };
      }
      s.t++;
      if (res.done) s.phase = 'idle';
      return res;
    },
    exit(ctx) {
      s.phase = 'idle';
      if (ctx.anim) ctx.anim.release(0.15);
    },
  };
}

export function createAction() { return jumpAction(); }
