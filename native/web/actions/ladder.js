// Ladder action (play.html): Space (the game's ✕, id 0xd) at a ladder's bottom or top
// trigger gets on, W / S climb in 4.5-unit rungs, climbing past the top or bottom gets
// off, Space jumps off. In the game ✕ is also jump, with the ladder first (FUN_00333150):
// player.js step runs the jump (jump_core.js) only when no action took the press.
// Data: data/z128_ladders.json (native/assets/models/export_web_ladders.py, stage
// section 8 types 1/2 + the class-0x01 ladder faces). Values: notes/actions-research.md
// section 3 (FUN_0059a5d0, FUN_00599f10, FUN_00346fc0, FUN_003470d8).
//
// Positions follow the clips: each phase moves from a start to an end point, and the
// clip's own root-motion curve (sidecar root_motion_per_frame_glb, read via ctx.motions)
// says how far along he is at each frame. The end points come from the game rules
// (5.6 out from the face, 4.5 per rung, feet on col.groundY after getting off), so the
// curve's shape is the clip's and its length is the game's. Game space, -Y up.
export const KEY = 'Space';
export const HELP = 'Space ladder (W/S climb, Space jump off)';

const DATA_URL = new URL('../data/z128_ladders.json', import.meta.url);
const SNAP = 5.6;         // game: character origin this far out from the face (Vincent, FUN_0059a5d0)
const BODY_FWD = 5.5;     // c_ld_u's constant bone-1 offset toward the ladder (glb units). The
                          // in-place export dropped it, so the feet go SNAP - BODY_FWD * scale
                          // (0.1 at the game's unit scale) from the face; that puts the hands and feet on the rungs.
const RUNG = 4.5;         // units per climb step (FUN_00599f10); z128's ladder mesh has its
                          // rungs exactly 4.5 apart, so this is world units, not clip * scale
const MOUNT_RISE = 6.1;   // first rung above the bottom trigger floor: c_ld_u's 6 + the game's 0.1
const STEP_TICKS = 14;    // (16 - 2) ticks per step; the 16-frame clip plays at 15/14
const TOP_PROBE = 20.25;  // top exit: no face this far above the feet (FUN_00346fc0)
const BOTTOM_EXIT = 6;    // c_ld_del/der drop (glb units); less than a rung + this left: get off
// The ✕ check and the per-frame gimmick choice both cast from FUN_005635e0's point = the
// feet + (0, -7, 0) (7 units up; FUN_00333150 x7 / FUN_0053b500 x10 the facing, row 2 of the
// matrix, FUN_00563678). For a ladder trigger only the ray's origin and direction matter.
const RAY_UP = 7;

// Test aid: play.html?ladder=N puts him at ladder N's bottom once, on load (z128's start
// deck doesn't connect to the ladder floor on foot in the current collision).
let warpTo = (() => { try { const v = new URLSearchParams(location.search).get('ladder'); return v === null ? null : +v; } catch { return null; } })();

const wrapPi = a => Math.atan2(Math.sin(a), Math.cos(a));
const lerp = (a, b, t) => a + (b - a) * t;

let ladders = null;       // loaded once, shared by every createAction()
let loading = null;
function load() {
  if (!loading) loading = fetch(DATA_URL).then(r => r.ok ? r.json() : Promise.reject(new Error(r.status)))
    .then(d => { ladders = d.ladders.map(prep); return ladders; })
    .catch(e => { console.warn('ladder data not loaded:', e.message); ladders = []; });
  return loading;
}

// world-space helpers for one ladder
function prep(L) {
  const F = { x: L.facing[0], z: L.facing[2] };
  const fl = Math.hypot(F.x, F.z); F.x /= fl; F.z /= fl;
  const fp = L.face.point;
  const n = { x: -F.x, z: -F.z };               // face normal, toward the climber
  const d = n.x * fp[0] + n.z * fp[2];          // plane n.p = d (horizontal part)
  // climbing spot (set per action from the scale, see setClimb): the face centre line, out along n
  const climb = { x: fp[0], z: fp[2] };
  const base = L.bottomY - MOUNT_RISE;          // rung 0 feet height; rung k = base - RUNG * k
  const faceTop = L.face.y ? L.face.y[0] : L.topY;   // smallest y = highest point
  const faceBottom = L.face.y ? L.face.y[1] : L.bottomY;
  return { ...L, F, n, d, climb, base, faceTop, faceBottom };
}

// is p inside trigger box b (local coordinates via its axes)
function inBox(b, p) {
  const o = b.origin, ax = b.axes;
  const v = [p.x - o[0], p.y - o[1], p.z - o[2]];
  const l = ax.map(a => (v[0] * a[0] + v[1] * a[1] + v[2] * a[2]) / (a[0] * a[0] + a[1] * a[1] + a[2] * a[2]));
  return l[0] >= b.min[0] && l[0] <= b.max[0] && l[2] >= b.min[2] && l[2] <= b.max[2] &&
         l[1] >= b.min[1] && l[1] <= b.max[1];
}

// FUN_005513c8 (gimmick vtable +0x154; FUN_00589978 -> FUN_005494a0) for the ladder types (the
// player-state term is applied by choose(), which only selects in states 2..5):
// type 1 (bottom): the ray origin inside the box (FUN_005507a0(g, origin, 1): the record's
// inverse matrix +0x40, then min +0x80 / max +0x90), player state 2..5, and the ray direction
// along the record's local +z (row 2, +0x20): dot > 0.
// type 2 (top): the origin inside the box, state 2..5, and |angle(+0x20, ray) about +0x10 (row 1,
// up)| < pi/2 (FUN_00260fe8), i.e. the same horizontal dot > 0.
// GUESS: type 2's FUN_003177a8(FUN_0055a370(P)) != 1 term is not decoded and taken as true.
const origin = pos => ({ x: pos.x, y: pos.y - RAY_UP, z: pos.z });
function selects(b, pos, heading) {
  const z = b.axes[2];
  return inBox(b, origin(pos)) && Math.sin(heading) * z[0] + Math.cos(heading) * z[2] > 0;
}

// clip root-motion curve, raw glb values at fractional frame f
function curve(ctx, clip, f) {
  const m = ctx.motions[clip];
  const r = m && m.root_motion_per_frame_glb;
  if (!r || !r.length) return [0, 0, 0];
  const n = r.length, c = Math.max(0, Math.min(n - 1, f)), i = Math.min(Math.floor(c), Math.max(0, n - 2)), t = c - i;
  if (n === 1) return r[0];
  return [0, 1, 2].map(k => r[i][k] + (r[i + 1][k] - r[i][k]) * t);
}
// how far along axis k (0..1) the clip is at frame f; linear in time when it doesn't move on k
function frac(ctx, clip, f, k) {
  const m = ctx.motions[clip], n = m ? m.root_motion_per_frame_glb.length : 2;
  const a = curve(ctx, clip, 0)[k], b = curve(ctx, clip, n - 1)[k];
  if (Math.abs(b - a) < 1e-3) return Math.max(0, Math.min(1, f / (n - 1)));
  return (curve(ctx, clip, f)[k] - a) / (b - a);
}
const frames = (ctx, clip) => (ctx.motions[clip] ? ctx.motions[clip].frames : 16);

export function ladderAction() {
  load();
  let L = null;           // the ladder in use
  let ph = null;          // phase: { kind, clip, t, len, rate, from, to, hFrac, vFrac, ... }
  let hand = 'l';         // which hand the idle is on ('l' -> c_ld_lid)
  let done = false;
  let log = [];           // per-rung positions, for tests (window.__play.ctrl.active.log)

  function play(ctx, clip, { loop = false, rate = 1 } = {}) {
    if (ctx.anim && ctx.anim.has(clip)) ctx.anim.play(clip, { loop, rate });
  }

  // start a move phase from the current feet to `to` over clip `clip`
  function move(ctx, kind, clip, to, { len, rate = 1, hAxis = 1, vAxis = 1, hHalf = false, heading, from } = {}) {
    const p = from || ctx.ctrl.pos;
    const n = frames(ctx, clip);
    ph = { kind, clip, t: 0, len: len ?? n - 1, rate, from: { ...p }, to,
           hAxis, vAxis, hHalf, h0: ctx.ctrl.heading, h1: heading ?? ctx.ctrl.heading };
    play(ctx, clip, { rate });
  }

  function idle(ctx) {
    ph = { kind: 'idle', t: 0 };
    play(ctx, hand === 'l' ? 'c_ld_lid' : 'c_ld_rid', { loop: true });
  }

  function climbPos(y) { return { x: L.climb.x, y, z: L.climb.z }; }
  // nearest rung-grid feet height to y
  function rungY(y) { return L.base - RUNG * Math.round((L.base - y) / RUNG); }
  function setClimb(ctx) {
    const s = ctx.ctrl.scale || 1, fwd = ctx.motions.c_ld_u ? curve(ctx, 'c_ld_u', 0)[2] : BODY_FWD;
    const off = SNAP - fwd * s, fp = L.face.point;
    L.offset = off;
    L.climb = { x: fp[0] + L.n.x * off, z: fp[2] + L.n.z * off };
  }

  // decide what W / S / Space do from the rung he is on
  // at = where he is (the end of the step just finished, or the current feet)
  function next(ctx, at = ctx.ctrl.pos) {
    const keys = ctx.input.keys || new Set(), pressed = ctx.input.pressed || new Set();
    const y = at.y, from = { ...at };
    if (pressed.has(KEY) && ctx.anim?.has('c_ld_jp')) return jumpOff(ctx, from);
    const up = keys.has('KeyW') || keys.has('ArrowUp');
    const down = keys.has('KeyS') || keys.has('ArrowDown');
    if (up && !down) {
      if (y - TOP_PROBE < L.faceTop) {          // no face 20.25 above the feet: over the top
        const clip = hand === 'l' ? 'c_ld_uel' : 'c_ld_uer';
        const land = topLanding(ctx);
        move(ctx, 'exitTop', clip, land, { hAxis: 2, vAxis: 1, from });
        return;
      }
      const clip = hand === 'l' ? 'c_ld_umr' : 'c_ld_uml';
      move(ctx, 'up', clip, climbPos(y - RUNG), { len: STEP_TICKS, rate: (frames(ctx, clip) - 1) / STEP_TICKS, from });
      return;
    }
    if (down && !up) {
      const floor = bottomFloor(ctx);
      // GUESS: bottom exit test from the c_ld_der/del drop (BOTTOM_EXIT) with 0.5 margin;
      // the game's probe (FUN_003470d8: 5.5 behind, down 8.25, class 0x01) isn't ported
      if (floor - y - RUNG < BOTTOM_EXIT - 0.5) {   // the next rung would be below the last one
        const clip = hand === 'l' ? 'c_ld_der' : 'c_ld_del';
        // c_ld_del/der step back 5.5 (glb) from the ladder and drop to the floor
        const back = -curve(ctx, clip, frames(ctx, clip) - 1)[2] * (ctx.ctrl.scale || 1);
        const x = L.climb.x + L.n.x * back, z = L.climb.z + L.n.z * back;
        const g = ctx.col.groundY(x, z, L.bottomY - 8);
        move(ctx, 'exitBottom', clip, { x, y: g ?? floor, z }, { hAxis: 2, vAxis: 1, from });
        return;
      }
      const clip = hand === 'l' ? 'c_ld_dmr' : 'c_ld_dml';
      move(ctx, 'down', clip, climbPos(y + RUNG), { len: STEP_TICKS, rate: (frames(ctx, clip) - 1) / STEP_TICKS, from });
      return;
    }
    // GUESS: idle clip at once; the game settles 32 ticks after a step (FUN_00599f10), not ported
    if (ph?.kind !== 'idle') idle(ctx);
  }

  function bottomFloor(ctx) {
    const g = ctx.col.groundY(L.climb.x, L.climb.z, L.bottomY - 8);
    return g ?? L.bottomY;
  }

  // top floor: c_ld_uel/uer's own forward step (3.5 glb) from the climb spot; the trigger
  // origin only if there is no floor near the top there
  function topLanding(ctx) {
    const clip = 'c_ld_uel', s = ctx.ctrl.scale || 1;
    const fwd = curve(ctx, clip, frames(ctx, clip) - 1)[2] * s;
    const lx = L.climb.x + L.F.x * fwd, lz = L.climb.z + L.F.z * fwd;
    const lg = ctx.col.groundY(lx, lz, L.topY - 8);
    // GUESS: "floor near the top" = within 3 of the top trigger (not decoded)
    if (lg !== null && Math.abs(lg - L.topY) < 3) return { x: lx, y: lg, z: lz };
    const o = L.top.origin;
    const along = (o[0] - L.climb.x) * L.F.x + (o[2] - L.climb.z) * L.F.z;
    const x = L.climb.x + L.F.x * along, z = L.climb.z + L.F.z * along;
    const g = ctx.col.groundY(x, z, L.topY - 8);
    return { x, y: g ?? L.topY, z };
  }

  function jumpOff(ctx, p) {
    const clip = 'c_ld_jp';
    const end = curve(ctx, clip, frames(ctx, clip) - 1), s = ctx.ctrl.scale || 1;
    // jp: up 3.5 and back 10 (glb), straight off the ladder
    const to = { x: p.x + L.n.x * -end[2] * s, y: p.y - end[1] * s, z: p.z + L.n.z * -end[2] * s };
    move(ctx, 'jump', clip, to, { hAxis: 2, vAxis: 1, from: p });
  }

  // one tick of the current move phase -> target feet position
  function advance(ctx) {
    ph.t++;
    const f = Math.min(ph.t * ph.rate, frames(ctx, ph.clip) - 1);
    const u = Math.min(1, ph.t / ph.len);
    let hf = ph.hHalf ? Math.min(1, ph.t / (ph.len / 2)) : frac(ctx, ph.clip, f, ph.hAxis);
    let vf = frac(ctx, ph.clip, f, ph.vAxis);
    if (u >= 1) { hf = 1; vf = 1; }
    const a = ph.from, b = ph.to;
    return {
      pos: { x: lerp(a.x, b.x, hf), y: lerp(a.y, b.y, vf), z: lerp(a.z, b.z, hf) },
      heading: ph.h0 + wrapPi(ph.h1 - ph.h0) * Math.min(1, ph.hHalf ? hf : u * 2),
      end: u >= 1,
    };
  }

  function result(ctx, pos, extra = {}) {
    const c = ctx.ctrl.pos;
    return { delta: { x: pos.x - c.x, y: pos.y - c.y, z: pos.z - c.z }, ...extra };
  }

  // M+0x10ac, the current gimmick (FUN_0053be88): chosen once per frame by the map update
  // FUN_0053b500 -> FUN_0053bf30, which runs AFTER the player's ✕ check (FUN_00333150), so a ✕
  // acts on the trigger chosen from last frame's pose (traced: l_ladder_jump_a ✕ f520 jumps,
  // l_ladder_jump_e ✕ f521 mounts). States 2..5 (FUN_0055abd8): choose from the pose; other
  // states: cleared while moving (C+0x1d8 > 0), else kept. The mount clears it (FUN_00576c88).
  // GUESS: with two boxes hit, the first in data order wins (FUN_00544960's pick is not decoded;
  // no z128 ladder boxes overlap).
  // MISSING: only ladder boxes are chosen. In FUN_00333150 any chosen gimmick with +0xd4 set eats ✕
  // (e.g. type 8, FUN_00576428), so beside a non-ladder gimmick the game may not jump where the page does
  // (notes/fidelity-audit.md "Non-ladder gimmicks taking ✕").
  let gimmick = null;
  function choose(ctx) {
    const c = ctx.ctrl, st = c.P ? c.P.state : (c.onGround ? 2 : 8);
    if (!ladders) { gimmick = null; return; }
    if (st < 2 || st > 5) { if (c.speed > 0) gimmick = null; return; }
    gimmick = null;
    for (const l of ladders) {
      if (selects(l.bottom, c.pos, c.heading)) { gimmick = { l, from: 'bottom', b: l.bottom }; break; }
      if (selects(l.top, c.pos, c.heading)) { gimmick = { l, from: 'top', b: l.top }; break; }
    }
  }

  let pending = null;
  return {
    name: 'ladder',
    get log() { return log; },
    get phase() { return ph && ph.kind; },
    get ladder() { return L && L.id; },
    get gimmick() { return gimmick && gimmick.from; },
    ate: false,         // ✕ taken by a chosen trigger that didn't mount (player.js: no jump)
    canEnter(ctx) {
      if (warpTo !== null && ladders) {
        const l = ladders.find(x => x.id === warpTo); warpTo = null;
        if (l) { pending = { l, from: 'warp' }; return true; }
      }
      this.ate = false;
      // FUN_00333150 ✕ (press edge), gate FUN_005771c8: P+0x118 bit 0x200 (also C+0x564 bits).
      // The chosen trigger takes ✕ (no jump); it mounts when the origin is in its box now
      // (FUN_005507a0) and the clip is there (FUN_003455e8 0x92 / 0x9b).
      // GUESS: bit 0x200 as player state 2..5. Traced, it follows state 2 <-> 8 frame for frame
      // (also on the ground take-off / landing frames); its setters FUN_005382e0 / FUN_00538218 /
      // FUN_005383a8 are not tied to state classes (see notes/fidelity-audit.md "Ladder trigger").
      // GUESS: the other gate terms are taken as passing (not ported; notes/fidelity-audit.md
      // "Ladder ✕ gate terms not ported"): FUN_003242e8 bit 4 (FUN_00333150), FUN_003422f0(C) == 0 and
      // P+0x118 bit 0x40000 (FUN_005771c8), FUN_00326068(5) on the per-frame choice (FUN_0053b500).
      let take = false;
      const st = ctx.ctrl.P ? ctx.ctrl.P.state : (ctx.ctrl.onGround ? 2 : 8);
      if (ctx.input.pressed?.has(KEY) && st >= 2 && st <= 5 && gimmick) {
        this.ate = true;
        if (inBox(gimmick.b, origin(ctx.ctrl.pos)) && ctx.anim?.has('c_ld_u') && ctx.anim.has('c_ld_d')) {
          pending = { l: gimmick.l, from: gimmick.from };
          take = true;
        }
      }
      // the map update's choice for the next frame (a frame's ticks share its pose: same result)
      if (take) gimmick = null; else choose(ctx);
      return take;
    },
    enter(ctx) {
      L = pending.l; done = false; log = []; hand = 'l';
      setClimb(ctx);
      const p = ctx.ctrl.pos;
      if (pending.from === 'warp') {
        const o = L.bottom.origin, x = o[0] - L.F.x * 2, z = o[2] - L.F.z * 2;
        const g = ctx.col.groundY(x, z, L.bottomY - 8);
        ph = { kind: 'warp', to: { x, y: g ?? L.bottomY, z } };
        return null;
      }
      if (pending.from === 'bottom') {
        // c_ld_u: slide onto the climb spot over the first half, rise 6 (glb) on the clip curve
        // onto rung 0 of the ladder's own grid, whatever floor bump he stood on
        move(ctx, 'mountBottom', 'c_ld_u', climbPos(L.base), { hHalf: true, heading: L.heading });
      } else {
        // c_ld_d: turn to face the ladder, back over the edge and drop 18 (glb)
        const drop = curve(ctx, 'c_ld_d', 0)[1] - curve(ctx, 'c_ld_d', frames(ctx, 'c_ld_d') - 1)[1];
        move(ctx, 'mountTop', 'c_ld_d', climbPos(rungY(p.y + drop)), { hAxis: 1, vAxis: 1, heading: L.heading });
      }
      log.push({ phase: ph.kind, tick: ctx.tick, feet: [p.x, p.y, p.z] });
      return null;                                  // move() already started the clip
    },
    update(dt, ctx) {
      if (!L || !ph) return { done: true };
      if (ph.kind === 'warp') return result(ctx, ph.to, { heading: L.heading, onGround: true, vy: 0, done: true, teleport: true });
      if (ph.kind === 'fall') {                   // after the jump-off: the body's own fall
        if (!ctx.ctrl.onGround) return { locomotion: true };
        log.push({ phase: 'landed', tick: ctx.tick, feet: [ctx.ctrl.pos.x, ctx.ctrl.pos.y, ctx.ctrl.pos.z] });
        return { locomotion: true, done: true };
      }
      if (ph.kind === 'idle') {
        next(ctx);
        if (ph.kind === 'idle') return result(ctx, climbPos(ctx.ctrl.pos.y), { onGround: false });
      }
      const s = advance(ctx);
      const res = result(ctx, s.pos, { heading: s.heading, onGround: false });
      if (!s.end) return res;
      const k = ph.kind;
      log.push({ phase: k, tick: ctx.tick, clip: ph.clip, feet: [s.pos.x, s.pos.y, s.pos.z] });
      if (k === 'exitTop' || k === 'exitBottom') {
        res.onGround = true; res.vy = 0; res.done = true;
        return res;
      }
      if (k === 'jump') {
        // c_ld_jp, then c_ld_fa0 while he falls (research section 3, conf. L). The fall
        // is the physics body's (player.js startFall / FUN_00357168 with h = 0, mode-0xc
        // gravity); FUN_00562d30's jump-off speeds aren't decoded, so no push off the ladder.
        ph = { kind: 'fall' };
        play(ctx, 'c_ld_fa0');
        res.onGround = false;
        return res;
      }
      if (k === 'up' || k === 'down') hand = ph.clip.endsWith('l') ? 'l' : 'r';
      ph = null;
      // chain the next step at once when the key is still held (FUN_00599f10), else idle
      next(ctx, s.pos);
      return res;
    },
    exit(ctx) {
      ph = null; L = null; pending = null;
      ctx.anim?.release();
    },
  };
}

export function createAction() { return ladderAction(); }
