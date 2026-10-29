// Aiming and shooting, translated from the game (PAL SLES_541.85). Pure logic, no three.js,
// so test_aim.mjs can replay the PCSX2 traces frame by frame. aim.js wires it to the page.
// Research and every offset: notes/aim-research.md. Names: P = player, C = body, CAM =
// camera, HUD = view object, RET = reticle sprite object, W = weapon stats.
import { weaponSwitch } from './jump_core.js';
//
// Per game frame the game runs (and so does WeaponSystem.frame):
//   1. camera update FUN_0032ace0: type transition, fade, recoil, reticle offset; the aim
//      ray FUN_00329778 (the caller supplies the hit); HUD reticle mode (type update slot +0x14)
//   2. input FUN_003320c0 (draw / holster / R3 / fire)
//   3. move intent FUN_00578748 (player.js; reads P+0x29c via isAiming())
//   4. weapon state machine FUN_00579570
//   5. reticle animation FUN_003b0ac0 (HUD draw)
// Traced order: a camera-type switch in the input (frame f) shows its HUD mode at f+1.

const f32 = Math.fround;
const DEG = Math.PI / 180;

// FUN_00323318 (also player.js fracStep): per-frame fraction for a per-tick rate.
export function fracStep(r, step) {
  if (!(1.0 < step)) return f32(r * step);
  let i = 1, k = 1.0;
  do { i++; k = f32(k * f32(1.0 - r)); } while (i <= step);
  return f32(1.0 - k);
}

// ---------------------------------------------------------------- camera types
// Runtime values of the camera type objects in z128 (DAT_004eed98[type], built by
// FUN_00330138 / FUN_00330638 / FUN_00330c10 / FUN_003311a8 via FUN_003314e0; the enter
// functions FUN_00330748 / FUN_00330d18 rewrite some fields). +0x10 pivot, +0x20 offset,
// +0x50 FOV, +0x58 fade target. reticle = the HUD mode the type's update sets
// (FUN_00330478 -> 0, FUN_00330980 -> 1, FUN_00330f08 -> 2).
export const CAM_TYPES = {
  0xc: { pivot: [0, -17, 0], offset: [0, 1.25, -38], fov: 73.5, fade: 0x80, reticle: 0 },
  0xd: { pivot: [5, -16.25, 0], offset: [0, 0, -35], fov: 73.5, fade: 0x80, reticle: 1 },   // pivot.x = CAM+0x1a0 (5)
  0xe: { pivot: [0, -16.25, -0.1], offset: [0, 0, -0.1], fov: 73.5, fade: 0, reticle: 2 },  // FUN_00330d18: FOV = DAT_0046e3f0
  0xf: { pivot: [0, -16.25, -0.1], offset: [0, 0, -0.1], fov: 25, fade: 0, reticle: 2 },   // FOV = W+0x34 (FUN_0032a708)
};
const VIEW_TO_TYPE = { 1: 0xc, 2: 0xd, 3: 0xe, 4: 0xf };      // FUN_0032a420 -> FUN_00337338
export const PITCH_UP = 50 * DEG, PITCH_DOWN = -54 * DEG;    // DAT_0046e3f8 / 0x46e3fc (RAM)

// ---------------------------------------------------------------- reticle offset
// FUN_00260cf4: 1 - cos(x pi/2), clamped to 0..1
const c1 = x => (x < 0 ? 0 : x > 1 ? 1 : f32(1 - Math.cos(x * Math.PI / 2)));
// FUN_00260d44: sin(x pi/2), clamped (jumps to FUN_0045b530)
export const ease = x => (x < 0 ? 0 : x > 1 ? 1 : f32(Math.sin(x * Math.PI / 2)));

// FUN_0032ace0 l.1687-1696: HUD+0x78 (reticle y as a fraction of the half height,
// + = down) from the camera pitch (CAM+0x174, rad, + = up) and the recoil CAM+0x198.
export function reticleOffset(pitch, k198) {
  const a = f32(pitch * -0.4774648);
  return f32(f32(a * c1(Math.abs(a) + 0.3)) - f32(k198 + k198));
}

// ---------------------------------------------------------------- aimed clip tables
// FUN_00595440 (standing aimed-move class): the move direction's angle from the facing
// (FUN_00353458 = FUN_00260e74(C+0x20, C+0x90, C+0x10)) picks a quadrant
// (FUN_0036a638: |a| < 30 -> 1, |a| >= 150 -> 3, else a > 0 -> 2, a <= 0 -> 4) and
// installs idle h_st_a + walk + run (FUN_00345b58), up/down partners (FUN_00345ca8 /
// FUN_00345d70), root speeds of entries 1/2 (FUN_00345e58 -> FUN_00344340) and the walk
// threshold 24 u/s (FUN_00345e38(0, 24)). Motion slots 0x40, 0x60/0x6c, 0x66/0x72,
// 0x63/0x6f, 0x69/0x75 (C+0x7fc in the z128 state).
export const AIM_SETS = {
  1: { walk: 'h_wk_a', run: 'h_rn_a' },
  2: { walk: 'h_wk_a', run: 'h_lrn_a' },
  3: { walk: 'h_wk_a', run: 'h_brn_a' },
  4: { walk: 'h_wk_a', run: 'h_rrn_a' },
};
export const AIM_IDLE = 'h_st_a', AIM_FIRE = 'h_st_f';        // slots 0x40 / 0x57
export const AIM_WALK_THR = 24.0;

export function quadrant(deg) {                               // FUN_0036a638
  let a = deg;
  if (a > 18000 || a < -18000) a = 0;
  while (a < -180) a += 360;
  while (a > 180) a -= 360;
  if (Math.abs(a) < 30) return 1;
  if (Math.abs(a) >= 150) return 3;
  return a > 0 ? 2 : 4;
}
// FUN_00260e74(face, move, up): signed angle from face to move about up = C+0x10 (row 1;
// -Y up, so up = (0,-1,0)): sign of (face x move) . up.
export function moveAngleDeg(face, move) {
  const fl = Math.hypot(face.x, face.z), ml = Math.hypot(move.x, move.z);
  if (!(fl > 0 && ml > 0)) return 0;
  const dot = (face.x * move.x + face.z * move.z) / (fl * ml);
  const cy = face.z * move.x - face.x * move.z;               // (face x move).y
  const ang = Math.acos(Math.max(-1, Math.min(1, dot))) / DEG;
  return -cy < 0 ? -ang : ang;                                // dot with up (0,-1,0)
}

// FUN_003b1048: the pieces of reticle kind k (reticle.json) at draw time t (ms) -> quads in
// HUD px relative to the reticle centre: { p: 4 corners, rgb, a }.
export function reticleQuads(data, k, variant, t) {
  const K = data.kinds[k]; if (!K) return [];
  const len = K.len, out = [];
  for (const pc of K.variants[variant]) {
    const fr = (data.sprite_frames[pc.sprite] || [])[pc.frame];
    const w = pc.w === -1 ? (fr ? Math.abs(fr[2]) : 0) : pc.w, h = pc.h === -1 ? (fr ? Math.abs(fr[3]) : 0) : pc.h;
    let a = pc.rgba[3];
    if (t !== len) {
      if (pc.flags & 1) a = Math.trunc(t * a / len);
      else if (pc.flags & 2) { const hl = len >> 1; a = t < hl ? Math.trunc((t << 5) / hl) : Math.trunc(((len - t) << 5) / hl); }
      else if (pc.flags & 4) a = 0x32 - Math.trunc(t * 0x32 / len);
      else if (pc.flags & 8) { const hl = len >> 1; a = t >= hl ? Math.trunc((t - hl) * a / hl) : 0; }
    }
    let ang = 0;
    if (pc.flags & 0x10) ang = t * (Math.PI / 2) / len;
    else if (pc.flags & 0x20) ang = -t * (Math.PI / 2) / len;
    else if (pc.flags & 0x40) ang = t * 2 * Math.PI / len;
    const corners = [[pc.x - 1 / 16, pc.y - 1 / 16], [pc.x + w + 1 / 16, pc.y - 1 / 16],
                     [pc.x + w + 1 / 16, pc.y + h + 1 / 16], [pc.x - 1 / 16, pc.y + h + 1 / 16]];
    const n = pc.copies;
    if (n > 0) {
      for (let i = 0; i < n; i++, ang += 2 * Math.PI / n) {
        const c = Math.cos(ang), s = Math.sin(ang);
        out.push({ p: corners.map(([x, y]) => [x * c - y * s, x * s + y * c]), rgb: pc.rgba, a });
      }
    } else if (n < 0) {
      out.push({ p: corners, rgb: pc.rgba, a });
      out.push({ p: corners.map(([x, y]) => [-x, y]), rgb: pc.rgba, a });
    }
  }
  return out;
}

// ---------------------------------------------------------------- the system
// the gun-out jump's eye offset (FUN_0032a180 / FUN_0032a1d8; RAM values in the save state)
const JUMP_EYE_OFFSET = [2.8, 4.0, -36.0];        // DAT_0046e320
const A220_UP = 0.06;                             // DAT_0046e42c
const A220_DN_MOVE = 0.0075;                      // DAT_0046e430
const A220_DN_NEAR = 0.03;                        // DAT_0046e434
const A220_DN_VIEW = 0.1;                         // DAT_0046e438
// FUN_00260c98: 0 below 0, 1 above 1, else (1 - cos(pi x)) / 2 (FUN_0045b428 = cosf)
const ease260c98 = x => (x < 0 ? 0 : x > 1 ? 1 : (1.0 - Math.cos(x * 3.1415927)) * 0.5);

export class WeaponSystem {
  // W: weapon.json (export_web_aim.py, FUN_0031c3f8); reticle: reticle.json (kinds, lengths)
  constructor(W, reticle = null, { step = 1.1999999284744263, lockOn = true } = {}) {
    this.W = W; this.ret = reticle; this.step = step;
    this.lockOn = lockOn;   // HUD+0x64 bit 0 / CAM+0x1e8 bit 2 (FUN_00575510 <- FUN_00321e20: the auto-lock option; on in the save)
    this.reset();
  }

  reset() {
    this.P = { st: 0, T: 0, acc: 0, drawn: false, m29c: 2, m2a0: 0, aim: null, facePt: null, state: 2 };
    this.cam = { view: 1, type: 0xc, phase: 1, ramp: 1, fade: 0x80, k198: 0, k19c: 0, kick: 0, flags: 0,
                 pitch: 0, fov: 73.5, e: 1, d1d8: 0, d1dc: 0, c1cc: 0,
                 a1a0: 5, a1a4: 0,                             // CAM+0x1a0 shoulder x (type 0xd pivot x), +0x1a4 look-at y
                 a220: 0,                                      // CAM+0x220: the gun-out jump's eye-offset blend (camJump)
                 e1e2: null };                                 // CAM+0x1e2 written by a type enter (FollowCamera owns the counter)
    this.eye = null; this.hit = null;                           // CAM+0x90 / +0xa0 this frame (the page sets them)
    this.hud = { mode: 0, offY: 0 };
    this.r = { kind: 0, phase: new Array(8).fill(0), end: new Array(8).fill(0), len: new Array(8).fill(0) };
    this.clock = 0;                                             // DAT_004f58ec (ms; FUN_003e2450)
    this.events = [];                                           // shots this frame (for the page)
    this.fire = null;                                           // stand-fire class FUN_00596460
  }

  // ---- the gun-out jump's camera (CAM+0x1e8 flag 0x1000000, CAM+0x220). FUN_00326500() is true
  // in the state, so all of these run.
  // FUN_0032a0f8(CAM, on, reset): flag 0x1000000 on / off; with reset, CAM+0x220 = 1 / 0.
  // Callers: FUN_00575768 (the take-off: on, no reset, when P+0x2a8 bit 0 = gun drawn), the enter
  // slots (vtable +0xc) of player states 2 / 3 (FUN_00537e70 / FUN_005380a0: off, no reset) and
  // FUN_00576548(P, 0 / 1) in the air (off, no reset / on, reset). The draw (FUN_005766c0, 0x5766dc)
  // passes its own a0 / a1, so a draw in the air sets the flag and CAM+0x220 = 1 (traced
  // cam_j_draw_r1 / _r3); on the ground it doesn't and the state 3 enter clears the flag (R1 traced 0).
  camJump(on, reset = false) {
    const c = this.cam;
    if (on) { c.flags |= 0x1000000; if (reset) c.a220 = 1.0; }
    else { c.flags &= ~0x1000000; if (reset) c.a220 = 0; }
  }
  // player state 2 / 3 entered (FUN_00537e70 / FUN_005380a0): FUN_0032a0f8(CAM, 0, 0)
  enterFoot() { this.camJump(false); }
  // FUN_0032a180 (FUN_0032ace0 l.1774): the type's eye offset (type object +0x20) moves toward
  // DAT_0046e320 = (2.8, 4, -36) by FUN_00260c98(CAM+0x220) = (1 - cos(pi x)) / 2; read before
  // this frame's camJumpFrame
  jumpOffset(off) {
    const k = ease260c98(this.cam.a220);
    return [0, 1, 2].map(j => off[j] + (JUMP_EYE_OFFSET[j] - off[j]) * k);
  }
  // FUN_0032a1d8(sticks, CAM, near), right after FUN_0032a180. sticks: |left stick| + |look stick|
  // (fVar31 + fVar28); near: CAM+0x1c0 (last frame's distance) < DAT_0046e4d0 = 25 and CAM+0x1e8
  // 0x80 (the camera collision hit last frame). View 2: with the flag CAM+0x220 rises by
  // DAT_0046e42c = 0.06 x step to 1; without it, it falls by 0.03 x step (near), 0.0075 x step
  // (either stick moving, sticks > 0.001) or holds. Other views: falls by 0.1 x step. Clamped 0..1.
  camJumpFrame(sticks, near, step) {
    const c = this.cam;
    let v = c.a220;
    if (c.view === 2) {
      if (c.flags & 0x1000000) { if (v < 1.0) v += A220_UP * step; }
      else if (near) { if (v > 0) v -= A220_DN_NEAR * step; }
      else if (sticks > 0.001) { if (v > 0) v -= A220_DN_MOVE * step; }
    } else if (v > 0) v -= A220_DN_VIEW * step;
    c.a220 = v < 0 ? 0 : v > 1 ? 1 : v;
  }

  isAiming() { return this.P.m29c === 3; }                    // FUN_00578748: P+0x29c == 3 -> mode 6
  drawn() { return this.P.drawn; }

  // ---- 1. camera (FUN_0032ace0 parts). pitch: CAM+0x174 (rad, + up), from the page's look.
  cameraFrame(pitch) {
    const c = this.cam, s = this.step, ty = CAM_TYPES[c.type];
    c.pitch = pitch;
    // HUD reticle mode from the type's update function (slot +0x14)
    this.setHudMode(ty.reticle);
    // transition: CAM+0x17c (l.2094-2112); DAT_0046e4ec 0.02, 0x46e4f0 0.1 with flag 0x100000
    // CAM+0x170 FOV target: the type's +0x50; type 0xf takes W+0x34 (FUN_0032a708 / FUN_0032a420)
    const tfov = c.type === 0xf ? this.W.scope_fov : ty.fov;
    if (c.phase === 0) {
      // l.2094: the eased fraction FUN_00260d44(CAM+0x17c) is taken before the ramp update;
      // the FOV (l.2132) and the eye lerp use it
      c.e = ease(c.ramp);
      c.fov = f32(f32(f32(tfov - c.fov) * c.e) + c.fov);
      // leaving first person ends in one frame: switchAngles() set the ramp to 1 (l.1509)
      const rate = (c.flags & 0x100000) ? 0.1 : 0.02;
      let r = f32(f32(f32(f32(1.02 - c.ramp) * rate) * s) + c.ramp);
      c.ramp = r < 0 ? 0 : r > 1 ? 1 : r;
      // fade C+0x1e0 toward type +0x58: +4 (to 0x80 from 0x7c) / -8 (to 0 below 9) per frame
      if (c.fade < ty.fade) c.fade = c.fade < 0x7c ? c.fade + 4 : 0x80;
      else if (ty.fade < c.fade) c.fade = c.fade < 9 ? 0 : c.fade - 8;
      if (c.ramp === 1.0) { c.phase = 1; c.fade = ty.fade; }
    } else {
      c.e = 1; c.fov = tfov;                                   // l.2180: settled, CAM+0x170 = type +0x50
    }
    // reticle offset first: l.1690 reads CAM+0x198 before the recoil update below (l.1751)
    this.hud.offY = reticleOffset(pitch, c.k198);
    // CAM+0x1cc = -atan(HUD+0x78 x tan(type FOV x DAT_0046e360 0.875 / 2)) (l.1692-1698): the
    // aim ray's pitch below the view axis, through the reticle; CAM+0x1d0 (yaw) = 0
    c.c1cc = f32(-Math.atan(this.hud.offY * Math.tan(ty.fov * 0.875 * 0.017453294 * 0.5)));
    // recoil (l.1747-1756): DAT_0046e4c8 0.3, DAT_0046e4c4 0.05 deg
    const t = c.k19c;
    c.k198 = f32(f32(f32(f32(t - c.k198) * 0.3) * s) + c.k198);
    c.k19c = t > 0 ? f32(t - f32(f32(0.05 * s) * 0.017453294)) : 0;
  }
  // FUN_0032a348: the eye kick CAM+0x224 decays by CAM+0x228 (0.01) x step; called after the
  // camera has put the kick on the eye (player.js FollowCamera.place: eye += forward x kick)
  kickDecay() {
    const c = this.cam;
    if (c.kick > 0) { c.kick = f32(c.kick - 0.01 * this.step); if (c.kick < 0) c.kick = 0; }
  }
  // FUN_0032ace0 l.1509-1520, before the pitch clamp: with CAM+0x1e8 0x800 (a switch between
  // views 2-4) clear it and return the angles FUN_0032aaf8 stored (pitch CAM+0x1d8, yaw +0x1dc);
  // with 0x100000 as well (first person -> weapon view) the ramp CAM+0x17c = 1 (ends this frame)
  switchAngles() {
    const c = this.cam;
    if (!(c.flags & 0x800)) return { pitch: 0, yaw: 0 };
    c.flags &= ~0x800;
    if (c.flags & 0x100000) c.ramp = 1;
    return { pitch: c.d1d8, yaw: c.d1dc };
  }

  // view pitch the render uses: + 0.8 x CAM+0x198 x (FOV / 73.5) (l.1762, DAT_0046e4cc)
  viewPitch() { const c = this.cam; return c.pitch + 0.8 * c.k198 * (c.fov / 73.5); }

  // FUN_0032a420(CAM, mode): new camera type; transition starts (phase 0, ramp 0)
  setView(mode) {
    const c = this.cam;
    const prev = c.view;
    if (mode === prev) return;                                // same mode, no restart (LAB_0032a4d0)
    // CAM+0x100: the old type's pivot (y x cos(pitch x 0.017453294): the game converts the
    // radian pitch again, so the cos is ~1) + offset, when the old view is 2-4; else 0 and
    // 0x800 cleared
    const old = prev > 1 && prev < 5 ? this.typeVec(c.type) : null;
    if (!old) c.flags &= ~0x800;
    const F = c.flags;
    if (mode === 1) c.flags = F & ~0x100800;
    else if (mode === 2) c.flags = prev === 4 ? ((F & ~0x100000) | 0x800) : prev === 3 ? (F | 0x100800) : (F & ~0x100800);
    else c.flags = prev === 1 ? (F & ~0x100800) : ((F & ~0x100000) | 0x800);   // 3 (4: not on the page)
    // FUN_00337338 runs the old type's leave slot: 0xe's FUN_00331160 clears flag 0x1000 and
    // sets CAM+0x1a0 = DAT_0046e348 (5; DAT_0046e34c = 2 with flag 0x4000, never set on the page)
    // and CAM+0x1a4 = PTR_DAT_0046e350 (-0.75)
    if (c.type === 0xe) { c.a1a0 = 5; c.a1a4 = -0.75; }
    c.view = mode; c.type = VIEW_TO_TYPE[mode];
    this.enterType(old);
  }
  // type object +0x10 pivot (y scaled as above) + +0x20 offset (FUN_0032a420 / FUN_0032aaf8)
  typeVec(type) {
    const t = CAM_TYPES[type], k = Math.cos(this.cam.pitch * 0.017453294);
    return [t.pivot[0] + t.offset[0], t.pivot[1] * k + t.offset[1], t.pivot[2] + t.offset[2]];
  }
  // FUN_00337338 -> the type's enter -> FUN_0032aaf8(type, CAM, 0): phase 0, ramp 0; with
  // 0x800 the angle that keeps the aim on CAM+0xa0 across the pivot change:
  // v = new - CAM+0x100, D = max(25, |CAM+0xa0 - CAM+0x90| - v.z); +0x1dc = -sign(v.x) atan(|v.x| / D),
  // +0x1d8 = -sign(v.y) atan(|v.y| / D) (v >= 0 takes the negative branch)
  enterType(old) {
    const c = this.cam;
    c.phase = 0; c.ramp = 0;
    if (c.type === 0xc) c.e1e2 = 0;                           // FUN_00330240 0x330420: sb zero, CAM+0x1e2 (stick-idle counter)
    if (!(c.flags & 0x800)) return;
    const n = this.typeVec(c.type), v = [n[0] - old[0], n[1] - old[1], n[2] - old[2]];
    const e = this.eye, h = this.hit;
    let D = (e && h ? Math.hypot(h.x - e.x, h.y - e.y, h.z - e.z) : 0) - v[2];
    if (D < 25) D = 25;
    const ang = x => (x < 0 ? Math.atan(Math.abs(x / D)) : -Math.atan(Math.abs(x / D)));   // FUN_0045b180
    c.d1dc = f32(ang(v[0])); c.d1d8 = f32(ang(v[1]));
  }
  // FUN_0032a390: first person (type 0xe), or the scope (0xf) when W+4 bit 2
  setFirstPerson() { this.setView(this.W.flags & 4 ? 4 : 3); }

  // camera message 0x3e9 (FUN_0032ffb0): pitch carry-over and the new kick target
  shotMessage() {
    const c = this.cam;
    let r = 1.0;
    if (this.W) { r = this.W.recoil_deg; if (!(this.W.flags & 0x10)) c.pitch += c.k198; }
    c.k198 = 0; c.k19c = f32(r * 0.017453294);
    return c.pitch;                                           // the page takes the carried pitch back
  }

  // ---- HUD reticle (FUN_0033ba88 -> FUN_0033c060 -> FUN_003b0440)
  setHudMode(m) {
    if (m === this.hud.mode) return;
    this.hud.mode = m;
    this.setKind(m === 0 ? 0 : (this.lockOn ? 2 : 1));
  }
  setKind(k) {                                                // FUN_003b0440
    const r = this.r;
    if (k === r.kind) return;
    this.hideKind(r.kind); this.showKind(k); r.kind = k;
  }
  lens(k) { const K = this.ret && this.ret.kinds[k]; return K ? K.len_show : 100; }
  showKind(k) {                                               // FUN_003b0680 -> FUN_003b0490(k, 0, 1, 3)
    if (!k) return;
    const r = this.r, now = this.clock, len = this.lens(k);
    if (r.phase[k] === 0) { r.len[k] = len; r.end[k] = now + len; }
    else if (r.phase[k] === 3) { r.len[k] = len; let e = len - (r.end[k] - now); if (len < e) e = len; r.end[k] = now + e; }
    else return;
    r.phase[k] = 1;
  }
  hideKind(k) {                                               // FUN_003b0738 -> FUN_003b0588(k, 2, 1, 3)
    if (!k) return;
    const r = this.r, now = this.clock, len = this.lens(k);
    if (r.phase[k] === 2) { r.len[k] = len; r.end[k] = now + len; }
    else if (r.phase[k] === 1) { r.len[k] = len; let e = now - (r.end[k] - len); if (len < e) e = len; r.end[k] = now + e; }
    else return;
    r.phase[k] = 3;
  }
  // FUN_003b0ac0 (normal phases only; the on-target / locked variants need actors)
  reticleFrame(ms) {
    this.clock += ms;
    const r = this.r, now = this.clock;
    for (let k = 1; k < 8; k++) {
      if (r.phase[k] === 1 && now >= r.end[k]) r.phase[k] = 2;
      else if (r.phase[k] === 3 && now >= r.end[k]) r.phase[k] = 0;
    }
  }
  // draw time t for kind k (FUN_003b0ac0): elapsed in 1, remaining in 3, else the length
  reticleT(k) {
    const r = this.r, p = r.phase[k];
    if (p === 1) return this.clock - (r.end[k] - r.len[k]);
    if (p === 3) return r.end[k] - this.clock;
    return r.len[k];
  }

  // ---- 2. input (FUN_003320c0, the parts the page has: no L1/materia, melee, reload).
  // inp: { r1Pressed, r1Held, r3Pressed, circlePressed }, onGround, far: CAM+0xb0 (FUN_003296e0(CAM, ., 0))
  // player: the PlayerController (jump_core weaponSwitch: the player side of FUN_005766c0 and the
  // P+0x2a8 bits; null in the weapon-only tests)
  input(inp, onGround, far = null, player = null) {
    this.far = far;
    const P = this.P, W = this.W;
    this.events.length = 0;
    if (!P.drawn) {
      if (inp.r3Pressed) {                                    // R3: draw + first person
        this.setFirstPerson(); this.draw(onGround, player); return;
      }
      // R1: draw + weapon camera. FUN_003320c0 l.288 (LAB_00332810): on the ground (C+0x1c0 bit
      // 0x200) or with P+0x2a8 bit 3 clear (set by a draw in the air, cleared at a take-off from the
      // ground: jump_core startJump). The l.288 state test (bVar1 / bVar3) is true in the on-foot
      // and air states; the page runs this only there (not during actions).
      if (inp.r1Pressed && (onGround || !(((player && player.P.flags2a8) ?? 0) & 8))) {
        this.setView(2); this.draw(onGround, player); return;
      }
    } else {
      if (inp.circlePressed) { this.holster(onGround, player); return; }
      if (inp.r3Pressed && !(P.st >= 10 && P.st <= 11)) {     // FUN_005773c8
        if (this.cam.view === 2) this.setFirstPerson(); else this.setView(2);
      }
    }
    // fire: (R1 held && auto) || (R1 pressed && !auto); FUN_00577180: state 0 and drawn
    const trig = (W.flags & 1) ? inp.r1Held : inp.r1Pressed;
    if (trig && P.st === 0 && P.drawn) {
      P.m2a0 = 0;                                             // FUN_00574800(P, 0)
      if (W.flags & 1) { P.st = 3; P.T = W.first_delay; }    // FUN_00574b00
      else { P.st = 1; P.T = W.first_delay; }                 // FUN_00574a80
      P.m29c = 3;                                             // FUN_005747f8(P, 3)
      P.facePt = this.far;                                    // FUN_003296e0(CAM, ., 0) -> FUN_00562ef8
    }
    if ((W.flags & 1) && !inp.r1Held) {                       // FUN_00574b98 on release
      if (P.st === 3) P.st = 0; else if (P.st === 4) P.st = 2;
    }
  }

  // FUN_005766c0(P, 1, 1) / (P, 0, 1) -> FUN_00576548(P, on) (same a0 / a1): weapon state 0xc / 0xe
  // and T = max(T, DAT_0046ee84 = 6) (DAT_0046ee80 = 15 when FUN_00326500() is false; true in the
  // save), holster: FUN_0032a420(CAM, 1). Also P+0x2a8 bit 0, P+0x29c 3 / 2 (traced). Then the
  // player state: on the ground 3 / 2 (whose enter clears the flag: enterFoot), in the air 9
  // (FUN_00538b50 doesn't touch the camera; jump_core weaponSwitch). The ladder mount calls the
  // holster too (FUN_00576c88 / FUN_00576de8).
  draw(onGround = true, player = null) {
    const P = this.P; P.drawn = true; P.st = 0xc; P.T = Math.max(P.T, 6); P.m29c = 3; P.facePt = this.far;
    if (player) player.P.flags2a8 = (player.P.flags2a8 ?? 0) | 1;   // FUN_00576548: FUN_00574808(P, 0, 1)
    if (!onGround) this.camJump(true, true);                  // FUN_00576548(P, 1): FUN_0032a0f8(CAM, 1, 1)
    const st = player ? weaponSwitch(player, true) : (onGround ? 3 : 9);
    P.state = st;
    if (st === 3) this.enterFoot();                           // state 3 enter FUN_005380a0 (not in the wind-up: 9)
  }
  // FUN_00576548(P, 0) itself: P+0x2a8 bit 0 clear (FUN_00574808(P, 0, 0)), state 0xe, T, CAM view
  // 1. It has no drawn-gun test, nor do its callers: the ladder mount sets 0xe with the gun away too.
  // In the air (C+0x1c0 bit 0x200 clear) it also calls FUN_0032a0f8(CAM, 0, 0).
  holsterCore(onGround = true, player = null) {
    const P = this.P; P.drawn = false; P.st = 0xe; P.T = Math.max(P.T, 6); this.setView(1);
    if (player) player.P.flags2a8 = (player.P.flags2a8 ?? 0) & ~1;  // FUN_00574808(P, 0, 0)
    if (!onGround) this.camJump(false);
  }
  // circle (FUN_005766c0(P, 0, 1)): P+0x29c 2 (traced); player state 2 on the ground
  // (FUN_0055b8e0(P, 2): its enter clears the flag), 9 in the air (jump_core weaponSwitch)
  holster(onGround = true, player = null) {
    this.holsterCore(onGround, player); this.P.m29c = 2;
    const st = player ? weaponSwitch(player, false) : (onGround ? 2 : 9);
    if (st != null) this.P.state = st;
    if (st === 2) this.enterFoot();                           // state 2 enter FUN_00537e70
  }
  // the ladder mount (FUN_00576c88 / FUN_00576de8): FUN_005747f8(P, 4) (P+0x29c = 4), the player
  // state 0xb / 0xc (FUN_0055b8e0; player.js's ladder owns it), FUN_00576548(P, 0), then
  // FUN_00574928(P, 0) (P+0x298 / 0x299 = 0, not on the page)
  // onGround: C+0x1c0 bit 0x200 at the mount (FUN_00576548 reads it: a ladder caught in the air
  // clears the flag)
  ladderMount(onGround = true, player = null) { this.P.m29c = 4; this.holsterCore(onGround, player); }

  // ---- 4. FUN_00579570 (aimHit: CAM+0xa0 from this frame's camera)
  stateMachine(aimHit) {
    const P = this.P, W = this.W;
    // FUN_005784d8(P, 1): P+0x2b0 = CAM+0xa0, and face it (FUN_00562ef8: P+0x120, C+0x670/0x2b0)
    const AIM = () => { if (P.drawn && P.m2a0 === 0 && aimHit) { P.aim = aimHit; P.facePt = aimHit; } };
    const interval = () => W.interval;                        // FUN_00316a20 true (traced 25 = W+0x44)
    P.acc = f32(P.acc + this.step);
    while (P.acc >= 1.0) {
      switch (P.st) {
        case 0: if (P.drawn) AIM(); break;
        case 1:
          if (P.T > 0) { P.T -= 1; break; }
          this.shoot(); P.st = 2; P.T = interval(); break;
        case 2: AIM(); if (P.T > 0) P.T -= 1; else P.st = 0; break;
        case 3:
          if (P.T > 0) { P.T -= 1; break; }
          this.shoot(); P.st = 4; P.T = interval(); break;
        case 4: AIM(); if (P.T > 0) P.T -= 1; else P.st = 3; break;
        case 0xc: P.st = 0xd; break;
        case 0xd: AIM(); if (P.T <= 0) P.st = 0; else P.T -= 1; P.st = 0xf; break;   // falls into 0xe
        case 0xe: P.st = 0xf; break;
        case 0xf: AIM(); if (P.T <= 0) P.st = 0; else P.T -= 1; break;
        default: P.st = 0;                                    // states 5-11 (materia, reload) not on the page
      }
      P.acc = f32(P.acc - 1.0);
    }
  }

  // FUN_00577480 (the parts on the page): one bullet event per pellet, camera message, kick
  shoot() {
    this.events.push({ type: 'shot', pellets: this.W.pellets });
    this.shotMessage();                                       // camera message 0x3e9 (FUN_0033a310), handled at once
    this.cam.kick = 0.75;                                     // FUN_0032a320(table 0x46eee0[class 1])
    this.fire = this.fire || { sub: 0, t: 0 };                // stand-fire class request
  }
}

// ---------------------------------------------------------------- the aim ray (FUN_00329778)
// F = row 2 of rotX(CAM+0x1cc + 1.0 x CAM+0x1fc) x the camera matrix (FUN_00259928; CAM+0x1fc
// = 0 on the page, yaw CAM+0x1d0 + 2 x CAM+0x200 = 0): -sin(a) row 1 + cos(a) row 2.
// M: the camera's rows (player.js FollowCamera.M = CAM+0x60..0x8f).
export function reticleRay(M, a) {
  const s = Math.sin(a), c = Math.cos(a);
  return { x: -s * M[1][0] + c * M[2][0], y: -s * M[1][1] + c * M[2][1], z: -s * M[1][2] + c * M[2][2] };
}
// O = eye, d = |O - CAM+0xf0|; S = O + F (d + CAM+0x1d4 = 10), E = S + F max(1050, CAM+0x1b8),
// CAM+0xb0 = S + F x 10000. hit = CAM+0xa0 = the first world hit on S..E, else CAM+0xb0.
// GUESS: the world probe FUN_005449f0(world, 1, ...) is the caller's segment() (collision.js,
// the character probe triangle set); the probe class of index 1 isn't decoded.
export function aimRayPoints(O, F, look, range10, segment) {
  const add = (p, k) => ({ x: p.x + F.x * k, y: p.y + F.y * k, z: p.z + F.z * k });
  const d = Math.hypot(O.x - look.x, O.y - look.y, O.z - look.z);
  const S = add(O, d + 10.0);
  const E = add(S, Math.max(1050, range10 * 10));
  const far = add(S, 10000);
  const h = segment ? segment(S, E) : null;
  const hit = h ? { x: h.point[0] ?? h.point.x, y: h.point[1] ?? h.point.y, z: h.point[2] ?? h.point.z } : null;
  return { S, E, far, hit: hit || far, hitWorld: !!hit };
}
