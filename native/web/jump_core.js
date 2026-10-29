// Vincent's on-foot jump in z128 (sea of flames), translated from the game. Pure: no three.js.
// Wired into player.js's PlayerController.step (notes/jump-research.md, "Wiring").
//
// The game's jump has three parts:
//   1. FUN_00333150 (input): ✕ (logical id 0xd) -> ladder first, then the jump gate
//      FUN_00577230 -> FUN_00575768(P, 0).                                    -> canJump()
//   2. FUN_00575768, physics-body branch (script flag 13, mode 0xc): player state 8
//      (0x1f for the second, mid-air jump), body take-off FUN_00357168(h).      -> startJump()
//   3. The motion-state classes: 0x20 jump (FUN_00598f50: freeze while h_jp0 plays, then
//      h_jp1 / h_jp2 / h_jp3 queued, done on ground) and 0x21 land (FUN_00599aa0: h_jp4).
//      The done flag sends player state 8 -> 2 (FUN_00538aa0) with motion 0x21 (FUN_00538a30).
// The flight itself (gravity, air steering, wall slide, landing) is the physics body that
// player.js already translates (startFall / gravityStep / airIntent / integrate).
//
// Controller interface (PlayerController in player.js; duck-typed here):
//   ctrl.P { state, b298, b299, dir, airShots?, flags2a8? }, ctrl.C { spd, dir, face, moveDir,
//   groundSpeed, flags79c? }, ctrl.body { state, ... }, ctrl.onGround, ctrl.startFall(h),
//   ctrl.stickInput(input), ctrl.intent(), ctrl.moveTick(), ctrl.rotate().

const f32 = Math.fround;

// ---------------------------------------------------------------- game data
// Runtime values from the sea-of-flames save state (EE RAM), checked against the traces.
export const JUMP = {
  STEP: 1.1999999284744263,  // DAT_004eeb24: ticks per PAL frame (player.js STEP)
  cfgC4: 12.100000381469727, // cfg+0xc4, cfg = *(DAT_0046dd68+0x14) = 0x1faaf24: jump height - 1
  cfgC0: 24.200000762939453, // cfg+0xc0: the alternative height (story-progress branch, not taken in z128)
  gravCfg: 1.2000000476837158, // DAT_0046ee2c -> cfg+0xcc (FUN_00326500 true); gravity = cfg+0xcc x 196
  freezeMax: 10.0,           // FUN_00598f50 phase 1/4/8: leave the wind-up when h_jp0 ends or t >= 10
  aimPhaseMax: 20.0,         // phases 5 / 6 (armed set): next part after 20
  phase2Mark: 30.0,          // phase 2 -> 3 after 30 (FUN_005ae618(MP, 2))
  landMax: 30.0,             // FUN_00599aa0: landing ends after 30 ticks at most
  landMoveAfter: 2.0,        // ... or after 2 ticks once C+0x1d8 > 0 (he is moving)
  backAngle: 90.0,           // FUN_0036b008: |angle(face, move)| <= 90 deg = normal jump
  clipRate: 0.5,             // MP+0x40c for the h_jp clips (trace), x weight 128/128
  blend591: 10,              // C+0x591 = C+0x6c0 = 10 (save state): the blend frames FUN_00340bb8(C, 1, -1) restores
  airFireBlend: 1,           // FUN_00596830: FUN_00340bb8(C, 1, 1) before the h_jp3_a set (aim_g f49: layer 2 = 1 + 1)
  airFireMax: 30.0,          // FUN_00596830 phase 2: the h_jp_f set ends after 30 ticks
};

// clip slots (C+0x7fc table, names read from the save state)
export const SLOT = {
  0x16: 'h_jp0', 0x17: 'h_jp1', 0x18: 'h_jp2', 0x19: 'h_jp3', 0x1a: 'h_jp4',
  0xd5: 'c_wjp0', 0xd6: 'c_wjp1', 0xd7: 'c_wjp2', 0xd8: 'h_jp3', 0xd9: 'h_jp4',
  0xda: 'c_bj0', 0xdb: 'c_bj1', 0xdc: 'c_bj2', 0xdd: 'c_bj0', 0xde: 'c_bj1', 0xdf: 'c_bj2',
  0xe0: 'c_bwj0', 0xe1: 'c_bwj1', 0xe2: 'c_bwj2',
  0xbc: 'h_jp0_a', 0xbd: 'h_jp1_a', 0xbe: 'h_jp2_a', 0xbf: 'h_jp3_a', 0xc0: 'h_jp4_a', 0xc1: 'h_jp_f',
};
// slots that are empty in z128's slot table (C+0x7fc in the save state): FUN_003455e8 says no.
// 0xc1 (h_jp_f, the air-shot clip) is empty, so FUN_00596830 plays the h_jp3_a set (aim_g f49).
export const UNLOADED = new Set([0xc1]);
// clip lengths (frames): h_jp* / h_jp*_a in 0770320b, c_wjp* / c_bj* / c_bwj* in 07703200, the
// containers z128 searches (FUN_005a7390; export_web_vincent.py CLIP_SETS; the `jump` pack,
// data/vincent/vincent_jump.json; the c_ lengths equal the traces' hold frame n - 1)
export const CLIP_FRAMES = { h_jp0: 3, h_jp1: 10, h_jp2: 10, h_jp3: 16, h_jp4: 15,
  h_jp0_a: 3, h_jp1_a: 10, h_jp2_a: 10, h_jp3_a: 16, h_jp4_a: 15,
  c_wjp0: 3, c_wjp1: 10, c_wjp2: 9, c_bj0: 2, c_bj1: 11, c_bj2: 10, c_bwj0: 2, c_bwj1: 11, c_bwj2: 10 };

// player states (P+0x104) this file deals with
export const ST = { FOOT: 2, JUMP: 8, FALL: 9, JUMP2: 0x1f, BACK: 0x20, BACK2: 0x21 };
const onFoot = s => s >= 2 && s <= 5;
const inAir = s => s === 8 || s === 9 || s === 0x20;

// FUN_0055a850 (FUN_0055aa88(P) on P+0x104): the air states 8, 9, 0xf, 0x10, 0x1f, 0x20, 0x21
export const airState = s => s === 8 || s === 9 || s === 0xf || s === 0x10 || (s >= 0x1f && s <= 0x21);

// P+0x100 bits 46-47 (P.jumpBits; "took off"): FUN_00565df8(P, v) writes them. The take-off sets
// them (FUN_00575768: 2 for kind 2, else 1; startJump below). FUN_0055b8e0 (every player state
// change) ends with FUN_00565df8(P, 0) when the new state is not an air state (FUN_0055aa88), so
// landing or a ladder clears them and a walk-off or ladder drop (no take-off) leaves them 0. The
// other clearers, the +0x14 slots of state 1 / 0x80 (FUN_00537ce8 / FUN_00539dd8, vtables 0x58ae90 /
// 0x58a860), and the script setters (FUN_005660b0, FUN_005661e8 case 0x145) are in states the page
// doesn't have. Readers: FUN_005766c0 (weaponSwitch below); FUN_00565480 isn't on the page.
// enterState: P+0x104 = st as FUN_0055b8e0 writes it, with that tail.
export function enterState(P, st) {
  P.state = st;
  if (!airState(st)) P.jumpBits = 0;                      // FUN_0055b8e0: FUN_00565df8(P, 0)
}

// ---------------------------------------------------------------- 1. the gate
// FUN_00575698: may a jump start from the current motion state? (also the second jump)
//   motionId = *(C+2000)+0x70: 0x20 jump, 0x22 fall, 0x59 back jump; standing/walking is 3/8.
export function secondJumpGate(ctrl, motionId) {
  const C = ctrl.C, P = ctrl.P;
  if ((C.flags79c ?? 0) & 0x20) return false;            // second jump already used
  if (!ctrl.onGround && ((P.flags2a8 ?? 0) & 8) && ((P.flags2a8 ?? 0) & 0x10)) return false;
  const s = C.s664 ?? 0;                                  // C+0x664 (0 in z128)
  if (!(s === 0 || s === 1 || s === 2)) return false;
  return motionId === 0x20 || motionId === 0x22 || motionId === 0x59;
}

// FUN_00333150's ✕ branch + FUN_00577230. `env` carries the flags the port doesn't model;
// the defaults are the save state's values (P+0x118 = 0x20209, C+0x564 bit 0x4000 clear,
// no input lock, no weapon block), for which the gate is open on foot.
//   pressed: ✕ went down this frame (FUN_00254810(0xd, 1, 0)); ladder: a ladder took ✕.
export function canJump(ctrl, motionId, { pressed, ladder = false, p118 = 0x20209, c564 = 0,
                                         inputLock = 0, weaponBlock = false } = {}) {
  if (!pressed || ladder) return false;
  // FUN_00577230
  let ok;
  if (p118 & 0x40000) ok = false;
  else if ((p118 & 0x200) && !(c564 & 0x4000)) ok = true;
  else ok = secondJumpGate(ctrl, motionId) || ctrl.P.state === 0x12;
  if (!ok) return false;
  const st = ctrl.P.state;
  if (st === 0x1a || (st >= 0x80 && st < 0xc0)) return false;      // FUN_0055bb40
  if (st >= 0xc0 || st === 0x13 || st === 0x14 || st === 0x1d) return false;   // FUN_0055bb78(P, 1)
  if (inputLock & 2) return false;                        // FUN_003242e8 bit 1
  if (weaponBlock) return false;                          // FUN_00574ca0
  return true;
}

// ---------------------------------------------------------------- 2. take-off
// FUN_00353458: signed angle (rad) from the facing C+0x20 to the move direction C+0x90
// about the up axis C+0x10 (FUN_00260e74). Up is -y in game space.
export function moveAngle(face, move) {
  const cx = face.z * move.x - face.x * move.z;           // (face x move) . (0, -1, 0)
  const dot = face.x * move.x + face.z * move.z;
  return Math.atan2(cx, dot);
}

// FUN_00575768(P, kind) for the physics body (DAT_0046e298 bit 13 set; z128).
// kind 0 = normal (✕), 1 = high jump (FUN_00326068(3) or FUN_00321460() == 1; not z128).
// opts.altHeight: the cfg+0xc0 branch (FUN_00310c60() >= 0x12, FUN_00310c40() >= 0x2e and the
// weapon byte 3..4: story progress; false in z128). opts.backLoaded: slot 0xdb (c_bj1) usable
// (FUN_003455e8; true in z128: the slot holds c_bj1 and j_back plays it).
// Returns the new player state, or null when the state doesn't allow a jump.
export function startJump(ctrl, motionId, { kind = 0, altHeight = false, backLoaded = true } = {}) {
  const P = ctrl.P, C = ctrl.C, J = JUMP;
  if (ctrl.onGround) {                                    // C+0x1c0 bit 0x200
    P.airShots = 0;                                       // P+0x270 = 0
    P.flags2a8 = (P.flags2a8 ?? 0) & ~0x18;               // FUN_00574808(P, 3 / 4, 0)
  }
  P.flags2a8 = (P.flags2a8 ?? 0) & ~0x60;                 // FUN_00574808(P, 5 / 6, 0)
  let h;
  if (kind === 1) h = f32(f32(J.cfgC4 + J.cfgC4) + 1.0);
  else h = f32((altHeight ? J.cfgC0 : J.cfgC4) + 1.0);

  // back jump: FUN_0036b058(angle) and slot 0xdb
  let back = false;
  const a = moveAngle(C.face, C.moveDir ?? { x: 0, y: 0, z: 0 });
  const deg = f32(a * 57.295776);
  if (!(deg >= -J.backAngle && deg <= J.backAngle) && backLoaded) back = true;
  // (back: FUN_0034b638(C, C+0x2d0) sets a turn target here, but FUN_00578748 overwrites
  // C+0x2c0 / C+0x2cc later in the same frame, so it has no effect; not ported)

  const st = P.state;
  let next;
  if (onFoot(st)) {
    C.flags79c = (C.flags79c ?? 0) & ~0x20;
    next = back ? ST.BACK : ST.JUMP;
  } else if (inAir(st)) {
    if (!secondJumpGate(ctrl, motionId)) return null;
    C.flags79c = (C.flags79c ?? 0) | 0x20;
    next = back ? ST.BACK2 : ST.JUMP2;
  } else {
    return null;                                          // other states: no jump (0x11/0x12/0x15/0x17 not ported)
  }
  enterState(P, next);                                    // FUN_0055b8e0(P, next, ...)
  P.flags2a8 &= ~4;                                       // bVar3: FUN_00574808(P, 2, 0)
  P.jumpBits = kind === 2 ? 2 : 1;                        // FUN_00565df8(P, 2 / 1) (l.236-245)

  // physics-body branch: cfg+0xcc = 1.2, gravity = cfg+0xcc x 196 (FUN_00356358)
  const bs = ctrl.body.state, airborne = bs === 2 || bs === 3 || bs === 6 || bs === 7;   // FUN_003577c8
  if (!airborne) {
    ctrl.startFall(h);                                    // FUN_0034a8e0 -> FUN_00357168(h)
  } else if (C.flags79c & 0x20) {
    if ((P.flags2a8 ?? 0) & 1) h = f32(h - 0.1);
    if ((P.airShots ?? 0) > 0) {
      const above = f32(ctrl.body.gy - ctrl.pos.y);        // FUN_00359050: body+0x14 - body+4
      if (h <= above) h = f32(h - Math.min(f32(above - h), 5.0));
    }
    ctrl.startFall(h);                                    // FUN_00357168(h)
  }
  return next;
}

// ---------------------------------------------------------------- 3. motion states
// A clip request for the motion player (FUN_003456c0 / FUN_003449a0): slot, name, blend
// frames (0 while C+0x564 bit 2 is set by FUN_00340bb8(C, 0, -1)), and the play flags:
// now = flag 2 (replace the head layer), else queued after it; loop = flag 1.
const req = (slot, { now = false, loop = false, blend = 0 } = {}) =>
  ({ slot, clip: SLOT[slot], now, loop, blend });

// FUN_00345818(C, level, up, down, flags, param_6, 0): an aimed whole-body set. The level clip
// plays with the up or down partner by the aim zone (C+0x590 bits 25-26); the blend is C+0x591
// when the head layer is still blending in (FUN_00346150 = FUN_005a9768(MP): the head layer's
// +0x24 blend length > 0; aim_g f45 / f48 yes -> 11 / 2, f82 no -> 0) or when param_6 is set
// and the zone is unchanged, else 0 (FUN_00340bb8(C, 0, -1)). Flags 2 = now, 1 = loop; with
// 0x80, a request for the level clip and partner already on the head is dropped (FUN_00344608:
// C+0x570 / C+0x578), so the fall class's re-issue of h_jp3_a doesn't restart it (aim_g f50).
// GUESS: the zone never changes: the page has no aim zones (it plays the level clip, no
// partner; see notes/fidelity-audit.md "Jump"), so "zone unchanged" holds and the partner
// matches whenever the level clip does.
function aimedReq(slot, flags, param6, io, b591 = JUMP.blend591) {
  const clip = SLOT[slot];
  if ((flags & 0x80) && io.headClip === clip) return [];
  const blend = (io.headBlending || param6) ? b591 : 0;
  return [{ ...req(slot, { now: !!(flags & 2), loop: !!(flags & 1), blend }), aimed: true }];
}

// FUN_00598f50: the jump motion state (class 0x20). One update per game frame, before the
// character's move (FUN_0034ea18). `io` = { ground, clipEnded, armed, aimChanged }:
//   ground: C+0x1c0 bit 0x200; clipEnded: FUN_003460d8 (head clip no longer playing);
//   armed: C+0x590 bit 24 and slot 0xbc usable; aimChanged: the aim zone changed (C+0x590
//   bits 25-26 vs 27-28) or FUN_00349db8(C, 3) && FUN_00349db8(C, 4).
// Returns the clip requests of this frame. this.freeze mirrors C+0x1c0 bit 0x20 (FUN_0034ea18
// skips the character's move and turn while it is set).
export class JumpMotion {
  constructor(second = false) {
    this.id = 0x20;
    this.second = second;                                 // C+0x79c bit 0x20 at entry
    this.phase = 0; this.t = 0; this.done = false; this.freeze = false;
  }
  update(io) {
    const J = JUMP, out = [];
    switch (this.phase) {
      case 0: {
        this.freeze = true;
        const armed = !this.second && io.armed;
        if (this.second) out.push(req(0xd5, { now: true }));
        else if (armed) out.push(...aimedReq(0xbc, 0x82, 1, io));   // FUN_00345818(C, 0xbc, 0xc2, 0xc8, 0x82, 1, 0)
        else out.push(req(0x16, { now: true }));
        // FUN_003412d0(C, 0x50, 1): sound 0x50 (not ported)
        this.phase = this.second ? 8 : armed ? 4 : 1;
        this.t = 0;
        break;
      }
      case 1:
      case 8:
        if (io.clipEnded || this.t >= J.freezeMax) {
          this.freeze = false;
          if (this.phase === 1) {
            out.push(req(0x17), req(0x18), req(0x19, { loop: true }));
          } else {
            out.push(req(0xd6), req(0xd7), req(0xd8, { loop: true, blend: 4 }));
          }
          this.phase = 2;                                 // t keeps counting (no reset)
        }
        break;
      case 2:
      case 3:
        if (this.t > J.phase2Mark && this.phase === 2) this.phase = 3;   // FUN_005ae618(MP, 2)
        if (io.ground) this.done = true;                 // FUN_005ae618(MP, 3); +8 |= 1
        break;
      case 4:                                             // armed wind-up (h_jp0_a)
        if (io.clipEnded || this.t >= J.freezeMax) {
          this.freeze = false;
          out.push(...aimedReq(0xbd, 2, 0, io));          // FUN_00345818(C, 0xbd, 0xc3, 0xc9, 2, 0, 0)
          this.phase = 5; this.t = 0; return this.tick(out);
        }
        break;
      case 5:
      case 6: {
        // the next part after its clip or 20 ticks (phase 6 -> h_jp3_a, flags 0x83: loop);
        // on the ground -> 7; an aim-zone change re-issues the current set (flags 2: restarts)
        const cur = this.phase === 5 ? 0xbd : 0xbe;
        if (!io.clipEnded && this.t < J.aimPhaseMax) {
          if (io.ground) { this.phase = 7; break; }
          if (io.aimChanged) out.push(...aimedReq(cur, 2, 0, io));
          break;
        }
        if (this.phase === 5) out.push(...aimedReq(0xbe, 2, 0, io));     // h_jp2_a
        else out.push(...aimedReq(0xbf, 0x83, 0, io));                     // h_jp3_a, looped
        this.phase = this.phase === 5 ? 6 : 7; this.t = 0; return this.tick(out);
      }
      case 7:
        if (!io.ground) { if (io.aimChanged) out.push(...aimedReq(0xbf, 0x83, 0, io)); }
        else this.done = true;                            // FUN_005ae618(MP, 3); +8 |= 1
        break;
    }
    return this.tick(out);
  }
  tick(out) { this.t = f32(this.t + JUMP.STEP); return out; }
}

// FUN_005a1510: the back-jump motion state (player states 0x20 / 0x21; its id 0x59 is the one
// FUN_00575698 and FUN_00538a30 name). Same shape as JumpMotion with the c_bj clips; h_jp3
// is queued with blend 4. Armed (phases 4-7): c_bj0-2 played now (FUN_003456c0, flags 2), then
// the h_jp3_a set through FUN_00345818 (flags 0x83, the aimedReq blend rule), as FUN_00598f50.
// io as JumpMotion, plus io.usable(slot) (FUN_003455e8; default: yes).
export class BackJumpMotion extends JumpMotion {
  constructor(second = false) { super(second); this.id = 0x59; }
  update(io) {
    const J = JUMP, out = [];
    const usable = io.usable ?? (slot => !UNLOADED.has(slot));
    switch (this.phase) {
      case 0: {
        this.freeze = true;
        const second = this.second && usable(0xe0);
        const armed = !second && io.armed;
        if (second) out.push(req(0xe0, { now: true }));
        else if (armed) out.push(req(0xdd, { now: true }));   // FUN_00345530(C, 1) first
        else out.push(req(0xda, { now: true }));
        this.phase = second ? 8 : armed ? 4 : 1;
        this.t = 0;
        break;
      }
      case 1:
      case 8:
        if (io.clipEnded || this.t >= J.freezeMax) {
          this.freeze = false;
          if (this.phase === 1) out.push(req(0xdb), req(0xdc), req(0x19, { loop: true, blend: 4 }));
          else out.push(req(0xe1), req(0xe2), req(0xd8, { loop: true, blend: 4 }));
          this.phase = 2;
        }
        break;
      case 2:
      case 3:
        if (this.t > J.phase2Mark && this.phase === 2) this.phase = 3;
        if (io.ground) this.done = true;
        break;
      case 4:
        if (io.clipEnded || this.t >= J.freezeMax) {
          this.freeze = false;
          out.push(req(0xde, { now: true }));
          this.phase = 5; this.t = 0; return this.tick(out);
        }
        break;
      case 5:
      case 6:
        if (!io.clipEnded && this.t < J.aimPhaseMax) {
          if (io.ground) this.phase = 7;                // (the aim-zone checks here play nothing)
          break;
        }
        if (this.phase === 5) out.push(req(0xdf, { now: true }));
        else out.push(...aimedReq(0xbf, 0x83, 0, io));   // FUN_00345818(C, 0xbf, 0xc5, 0xcb, 0x83, 0, 0): now, loop
        this.phase = this.phase === 5 ? 6 : 7; this.t = 0; return this.tick(out);
      case 7:
        // the zone-change re-issue, as phase 6 (FUN_00345818(C, 0xbf, 0xc5, 0xcb, 0x83, 0, 0))
        if (!io.ground) { if (io.aimChanged) out.push(...aimedReq(0xbf, 0x83, 0, io)); }
        else this.done = true;
        break;
    }
    return this.tick(out);
  }
}

// FUN_005766c0(P, on, 1): the R1 / R3 draw (on) and the circle holster, the player side (the weapon
// side, FUN_00576548, is aim_core draw / holsterCore; P+0x2a8 bit 0 is set / cleared there).
// The draw restarts the fall only when P+0x100 bits 46-47 != 0 (a take-off) and C+0x1c0 bit 0x200
// is clear: a walk-off or a ladder drop keeps falling. Both paths then pick the state: the ground
// state when the bits are 0 and the ground bit is set, else 9. So in the jump's wind-up (bits 1,
// still on the ground) a draw goes to state 9 without a restart. FUN_0055bb40 (states 0x80-0xbf)
// is 0 in the states the page draws from. Returns the new player state, or null (holster in
// states 0x1f / 0x20 / 0x21: no change); the ground states are returned, not written (the caller's).
// Not ported: FUN_00565d78 (P+0x100 bit 44, C+0x590 bit 24, no reader on the page), the state-4/5
// choice by P+0x100 bits 40-41 (0 traced).
export function weaponSwitch(ctrl, on) {
  const P = ctrl.P;
  const took = (P.jumpBits ?? 0) !== 0;                   // (P+0x100 >> 0x2e & 3) != 0
  if (on && took && !ctrl.onGround) {
    // FUN_00326500() true: FUN_00357168(DAT_0046ee88 = 1.0, C+0x3b0), a fresh take-off of height 1;
    // 0 after a second jump (C+0x79c bit 0x20) near the apex (|C+0x498 - C+0x484| = |rise - t| < 0.1)
    const B = ctrl.body;
    let h = 1.0;
    if (((ctrl.C.flags79c ?? 0) & 0x20) && Math.abs(f32(B.rise - B.t)) < 0.1) h = 0;
    ctrl.startFall(h);
  }
  if (!took && ctrl.onGround) return on ? 3 : 2;          // FUN_0055b8e0(P, 3 / 2, 0, 0) (LAB_00576930)
  if (!on) {
    const st = P.state;
    if (st === ST.JUMP2 || st === ST.BACK || st === ST.BACK2) return null;   // ' ', '!', 0x1f
  }
  enterState(P, ST.FALL);                                 // FUN_0055b8e0(P, 9, 1, 0) (enter FUN_00538b50)
  if (ctrl.jump) ctrl.jump.forceFall(ctrl);               // ... its param_3 = 1: the motion becomes 0x22
  // LAB_005769c8: a draw in an air state (FUN_0055aa88) sets P+0x2a8 bit 3 (FUN_00574808(P, 3, 1)):
  // no second R1 draw before the next take-off from the ground (FUN_003320c0 l.288)
  if (on) P.flags2a8 = (P.flags2a8 ?? 0) | 8;
  return ST.FALL;
}

// FUN_00333150's P+0x29c (turn mode) writes. States 0x20 / 0x21 (the back jump), after the stick writes (player.js
// stickInput's air path): P+0x29c = 5 unless P+0x2a8 bit 0 (armed: 3) or bit 2 is set (2),
// and with the stick past DAT_0046e518 = 0.5, P+0x2a8 bit 2 is set once the body's fall clock
// passes the apex: t (body+0xd4) > rise (body+0xe8) + margin, margin = DAT_0046e51c = 0.06 when
// the stick points more than 90 deg away from P+0x80 (the player's facing), else DAT_0046e520 = 0.
// FUN_00578748 then negates the turn target in mode 5 (LAB_00579288): he faces away from the
// stick until bit 2 turns him round. stick = { x, z, mag } in world space.
export function stickMode(ctrl, stick) {
  const P = ctrl.P, st = P.state;
  const f2a8 = P.flags2a8 ?? 0;
  if (st !== ST.BACK && st !== ST.BACK2) { P.mode29c = (f2a8 & 1) ? 3 : 2; return; }   // the other tails
  if (f2a8 & 1) { P.mode29c = 3; return; }               // LAB_00333e08
  if (f2a8 & 4) { P.mode29c = 2; return; }               // LAB_00333e28
  P.mode29c = 5;
  if (!(stick.mag > 0.5)) return;
  // FUN_00261038(P+0x80, stick): the angle between them.
  // GUESS: P+0x80 = the facing C+0x20 (equal in the save state; the writer isn't found;
  // see notes/jump-research.md)
  const a = moveAngle(ctrl.C.face, { x: stick.x, y: 0, z: stick.z });
  const deg = f32(a * 57.295776);
  const margin = (deg >= -90 && deg <= 90) ? 0.0 : 0.05999999865889549;
  if (ctrl.body.t <= f32(ctrl.body.rise + margin)) return;
  P.flags2a8 = f2a8 | 4;                                  // FUN_00574808(P, 2, 1)
}

// FUN_00578748 LAB_00579288: in the air states with P+0x29c = 5 the turn target is negated
// before FUN_0034b638(0.25, C, dir). player.js airIntent doesn't know mode 5; apply after it.
export function backTurnFix(ctrl) {
  const P = ctrl.P, st = P.state;
  if (!(st === 8 || st === 9 || (st >= 0x1f && st <= 0x21))) return;
  if (P.mode29c !== 5) return;
  const t = ctrl.C.turn;
  ctrl.C.turn = { x: -t.x, y: -t.y, z: -t.z };
}

// FUN_00599aa0: the landing motion state (class 0x21). `io` = { clipEnded, groundSpeed, armed }.
// When done, this.next = the motion class the game goes to: 3 idle / 8 moving (6 / 9 armed),
// i.e. back to the locomotion clips.
export class LandMotion {
  constructor() { this.id = 0x21; this.phase = 0; this.t = 0; this.done = false; this.next = null; }
  update(io) {
    const out = [];
    if (this.phase === 0) {
      if (io.armed) out.push(...aimedReq(0xc0, 0x82, 0, io));   // FUN_00345818(C, 0xc0, 0xc6, 0xcc, 0x82, 0, 0)
      else out.push(req(0x1a, { now: true }));
      // FUN_00341410(C, 3), FUN_003412d0(C, 0x51, 0): landing sound (not ported)
      this.t = 0; this.phase = 1;
    } else if (this.phase === 1) {
      if (io.clipEnded || this.t >= JUMP.landMax) {
        this.done = true; this.next = io.armed ? 6 : 3;
      } else if (this.t >= JUMP.landMoveAfter && io.groundSpeed > 0) {
        this.done = true; this.next = io.armed ? 9 : 8;
      }
      if (this.done) return out;                          // returns before t += step
    }
    this.t = f32(this.t + JUMP.STEP);
    return out;
  }
}

// FUN_00596830: the air-shot motion state (class 0x4e, vtable 0x5a6380; FUN_00592918 builds it
// for id 0x4e). A shot in the air enters it (FUN_00577480 -> FUN_00560b58, Jump.airShot). With
// slot 0xc1 (h_jp_f) empty, as in z128, phase 0 sets the blend to 1, plays the h_jp3_a set
// (flags 0x83: now, loop; dropped if h_jp3_a already plays), and is done with next motion 0x22.
// Traced aim_g: R1 f48 -> h_jp3_a 0.6 with blend 2 in the record of f49 (h_jp2_a never plays).
// The h_jp_f branch (phases 1-2: FUN_00345818(C, 0xc1, 199, 0xcd, 0x82, 0, 0), 30 ticks) can't
// run in z128 and is not ported.
export class AirFireMotion {
  constructor() { this.id = 0x4e; this.phase = 0; this.t = 0; this.done = false; this.next = null; this.freeze = false; }
  update(io) {
    const out = [];
    const usable = io.usable ?? (slot => !UNLOADED.has(slot));
    if (this.phase === 0) {
      // FUN_00345530(C, 1); C+0x664 == 2 -> FUN_00340de0(C, 1) (C+0x664 = 0 in z128)
      // the h_jp_f branch is not ported (slot 0xc1 is empty in z128): if a slot table ever has it,
      // leave the feature out (the h_jp3_a set below) instead of breaking the frame loop
      if (usable(0xc1) && !AirFireMotion.warned) {
        AirFireMotion.warned = true;
        console.warn('FUN_00596830: slot 0xc1 (h_jp_f) is loaded but its branch is not ported: playing the h_jp3_a set');
      }
      out.push(...aimedReq(0xbf, 0x83, 0, io, JUMP.airFireBlend));   // after FUN_00340bb8(C, 1, 1)
      this.done = true; this.next = 0x22; this.phase = 2;             // returns 0x22 (before t += step)
      return out;
    }
    this.t = f32(this.t + JUMP.STEP);
    return out;
  }
}

// FUN_00599820: the fall motion state (class 0x22, vtable 0x5a5a70; built by FUN_00599780).
// Phase 0 picks the blend by the previous motion id (FUN_00341780 stores it at +0): 0x16, 6-0xa,
// 0x30, 3, 0x12, 0x1c-0x1d -> FUN_00340bb8(C, 1, -1) and param_6 1; 0x1000-0x107f -> blend 6,
// param_6 1; any other (0x4e, the air shot) -> FUN_00340bb8(C, 0, -1) and param_6 0. Then
// FUN_005ae618(MP, 2) (not ported; the trace shows it at aim_g f49), and armed: the h_jp3_a set
// (flags 0x83, dropped when h_jp3_a already plays: aim_g f50 no restart), else h_jp3 (flags 3:
// now, loop). Phase 2: done on the ground (FUN_005ae618(MP, 3); +8 |= 1).
export class FallMotion {
  constructor(prev = 3) { this.id = 0x22; this.prev = prev; this.phase = 0; this.t = 0; this.done = false; this.freeze = false; }
  update(io) {
    const out = [];
    if (this.phase === 0) {
      const p = this.prev;
      let param6 = 1, b591 = JUMP.blend591, cut = false;
      if (p >= 0x1000 && p < 0x1080) b591 = 6;
      else if (!(p === 0x16 || (p >= 6 && p <= 0xa) || p === 0x30 || p === 3 || p === 0x12 || p === 0x1c || p === 0x1d)) {
        param6 = 0; cut = true;                           // FUN_00340bb8(C, 0, -1): C+0x591 unchanged
      }
      if (io.armed) out.push(...aimedReq(0xbf, 0x83, param6, io, b591));
      else out.push(req(0x19, { now: true, loop: true, blend: cut ? 0 : b591 }));
      this.phase = 2;
    } else if (this.phase === 2) {
      if (io.ground) this.done = true;
    }
    this.t = f32(this.t + JUMP.STEP);
    return out;
  }
}

// ---------------------------------------------------------------- head-layer clock
// The motion player's head layer for the jump clips, enough to answer FUN_003460d8:
// frame += rate x weight/128 x step (FUN_005aa2a8); a loop resets to 0 at the clip length;
// a one-shot holds at n - 1 and counts as ended there (trace: h_jp0 2.0, h_jp4 14.0); a
// queued clip takes over at frame 0 on the advance that would pass n - 1, the remainder
// dropped (trace: h_jp1 8.4 -> 9.0 -> h_jp2 0.0; c_wjp2 7.8 -> h_jp3 0.0).
// GUESS: the hold / queue take-over rule (not decoded; inferred from 7 traces, exact on all;
// see notes/jump-research.md). Only FUN_005aa2a8's frame step and loop reset are decoded.
export class ClipClock {
  constructor(clip = null, frame = 0, frames = CLIP_FRAMES) {
    this.frames = frames; this.clip = clip; this.frame = frame; this.loop = false; this.queue = [];
    this.serial = 0; this.blend = 0;                    // each clip switch: serial + 1, the request's blend frames
    this.blendLen = 0;                                  // the head layer's +0x24 blend length (aim_g: 11 -> 9.8 -> ... -> 0)
  }
  apply(reqs) {
    for (const r of reqs) {
      if (r.now) { this.clip = r.clip; this.frame = 0; this.loop = r.loop; this.queue = []; this.serial++; this.take(r.blend); }
      else this.queue.push(r);
    }
  }
  take(blend) { this.blend = blend; this.blendLen = blend > 0 ? blend + 1 : 0; }
  // FUN_00346150 -> FUN_005a9768(MP): the head layer is still blending in (+0x24 > 0)
  blending() { return this.blendLen > 0; }
  ended() {
    const n = this.frames[this.clip];
    return !this.loop && n !== undefined && this.frame >= n - 1;
  }
  advance(step = JUMP.STEP) {
    if (this.blendLen > 0) { this.blendLen = f32(this.blendLen - step); if (this.blendLen <= 0) this.blendLen = 0; }   // FUN_005aa5b8
    const n = this.frames[this.clip];
    if (n === undefined) return;
    const f = f32(this.frame + f32(f32(JUMP.clipRate * step)));
    if (this.loop) { this.frame = f >= n ? 0 : f; return; }
    if (f > n - 1 && this.queue.length) {               // passes the last frame: the queued clip
      const r = this.queue.shift(); this.clip = r.clip; this.frame = 0; this.loop = r.loop;
      this.serial++; this.take(r.blend); return;
    }
    this.frame = Math.min(f, n - 1);
  }
}

// ---------------------------------------------------------------- the per-frame driver
// The jump's hooks in the game's frame order, around PlayerController's own steps:
//   FUN_00333150 (✕ -> jump, then the stick)  ->  FUN_00578748 (intent)  ->
//   motion-state update (FUN_00598f50 / FUN_00599aa0)  ->  FUN_0034ea18 (move + turn,
//   skipped while frozen)  ->  state 8 -> 2 when the jump motion is done  ->  clip advance.
// The motion id (*(C+2000)+0x70) is tracked here: 0x20 jump, 0x21 land, 0x22 fall, else 3.
// frame() runs the whole order (test_jump.mjs); player.js's PlayerController.step calls the
// pieces (takeOff, stickMode, motionUpdate, advance) between its own steps.
export class Jump {
  constructor() { this.motion = null; this.clock = null; this.events = []; this.head = false; }
  motionId(ctrl) {
    if (this.motion) return this.motion.id;
    return ctrl.P.state === ST.FALL ? 0x22 : 3;
  }
  get active() { return !!this.motion; }
  get frozen() { return !!(this.motion && this.motion.freeze); }

  // FUN_00333150's ✕ branch, before the stick writes (the state may become 8 here).
  // opts: canJump's env ({ ladder, ... }) and startJump's options.
  takeOff(ctrl, cross, opts = {}) {
    if (!canJump(ctrl, this.motionId(ctrl), { pressed: cross, ...opts })) return null;
    const prevMotion = this.motionId(ctrl);
    const st = startJump(ctrl, prevMotion, opts);
    if (st === null) return null;
    // FUN_00537d58 (from 2) / FUN_00538a30 (from 8): 8, 0x1f -> motion 0x20; 0x20, 0x21 -> 0x59
    // GUESS: from states 9 and 0x20 the same mapping (their vt+0x24, 0x538bd8 / 0x53a478,
    // not read; see notes/jump-research.md)
    // FUN_00575768's end, every take-off: CAM+0x1e8 &= ~2 and FUN_00329ff0(CAM, 0) (flag 2 and the
    // target-lock bit 0x10000 / CAM+0x1e3: neither is set on the page), then FUN_0032a0f8(CAM, 1, 0)
    // when P+0x2a8 bit 0 (the gun is drawn): the eye offset starts its blend (aim_core camJump)
    if (ctrl.weapon && ctrl.weapon.drawn()) ctrl.weapon.camJump(true);
    const second = st === ST.JUMP2 || st === ST.BACK2;
    this.motion = (st === ST.BACK || st === ST.BACK2) ? new BackJumpMotion(second) : new JumpMotion(second);
    this.events.push({ frame: ctrl.frames, ev: 'jump', state: st });
    return st;
  }

  // the motion-state update (before FUN_0034ea18). Returns this frame's clip requests, already
  // applied to the head-layer clock. this.head: the jump's motion states own the head layer
  // this frame (from the take-off until the frame the landing motion ends; the locomotion
  // choice takes over on the next frame).
  motionUpdate(ctrl, opts = {}) {
    const reqs = [];
    this.head = !!this.motion;
    if (this.motion) {
      const io = { ground: ctrl.onGround, clipEnded: this.clock ? this.clock.ended() : true,
                   armed: !!opts.armed, aimChanged: !!opts.aimChanged, groundSpeed: ctrl.C.groundSpeed,
                   headClip: this.clock ? this.clock.clip : null, headBlending: this.clock ? this.clock.blending() : false };
      if (opts.usable) io.usable = opts.usable;
      const out = this.motion.update(io);
      reqs.push(...out);
      if (this.motion instanceof AirFireMotion && this.motion.done) {
        // the class returns 0x22 with its done flag: the fall class runs from the next frame (aim_g:
        // FUN_00599820's FUN_005ae618(MP, 2) shows at f49). FUN_00538aa0 (state 8) answers the done
        // event with 0 (no state change) while airborne.
        // GUESS: the dispatcher that reads the return value isn't decoded (the trace agrees); in
        // state 9 the same (its handler 0x538bd8 isn't read; see notes/jump-research.md)
        this.motion = new FallMotion(0x4e);
      } else if ((this.motion instanceof JumpMotion || this.motion instanceof FallMotion) && this.motion.done) {
        // done -> event -> FUN_00538aa0: state 2 (3 with P+0x100 bit 44); FUN_00538a30: motion 0x21
        // GUESS: that the done flag is what raises the event, on this frame (not decoded; the
        // traces agree to the frame; see notes/jump-research.md)
        enterState(ctrl.P, ST.FOOT);
        // entering state 2 / 3 runs its vtable +0xc slot, FUN_00537e70 / FUN_005380a0 (state table
        // FUN_00537628: vtables 0x58ae60 / 0x58ae30): FUN_0032a0f8(CAM, 0, 0) (traced: cam_j_gun f82)
        if (ctrl.weapon) ctrl.weapon.enterFoot();
        this.motion = new LandMotion();
        this.events.push({ frame: ctrl.frames, ev: 'land' });
      } else if (this.motion instanceof LandMotion && this.motion.done) {
        this.events.push({ frame: ctrl.frames, ev: 'loco', next: this.motion.next });
        this.motion = null;                               // back to the locomotion clips
      }
    }
    if (reqs.length) { if (!this.clock) this.clock = new ClipClock(); this.clock.apply(reqs); }
    return reqs;
  }

  // FUN_00577480 (a shot fired, in FUN_00579570) -> FUN_00560b58(P, 1), its airborne branch: in
  // states 8, 9, 0xf, 0x10, 0x1f-0x21 (FUN_0055aa88) the motion becomes 0x4e (FUN_00341780: the
  // old class's exit, e.g. FUN_00598ee8, clears the wind-up freeze; C+0x79c &= ~0xc0, so the
  // second jump's bit 0x20 stays); from the back jump (0x20 / 0x21, FUN_0055aaa8) the state
  // becomes 9 first. Call it after the weapon step, before motionUpdate (the game's order:
  // FUN_005786e8 runs FUN_00579570 before the character's motion update), so the new class's
  // phase 0 runs this frame (aim_g f48). Not ported: the P+0x100 bits 40-41 = 1 branch (motion
  // 0x11; the bits are 0 in the save state) and the ground branch (aim.js's stand-fire class).
  airShot(ctrl) {
    const st = ctrl.P.state;
    if (!(st === 8 || st === 9 || st === 0xf || st === 0x10 || (st >= 0x1f && st <= 0x21))) return false;
    if (st === ST.BACK || st === ST.BACK2) enterState(ctrl.P, ST.FALL);                 // FUN_0055b8e0(P, 9, 0, 0)
    this.motion = new AirFireMotion();
    this.events.push({ frame: ctrl.frames, ev: 'airShot' });
    return true;
  }

  // FUN_0055b8e0(P, 9, 1, 0), param_3 = 1 (weaponSwitch): the handler is FUN_00537a58(0) (the state
  // table's entry 0, vtable 0x58aec0), whose +0x24 slot FUN_00537cc0 -> FUN_00537aa0 gives state 9's
  // motion from the jumptable 0x589e70[9] (0x537b34: li v0, 0x22). FUN_005641c0(P, 0x22, 1) then sets
  // it even when it is already the motion (FUN_00341780(C, 0x22, 0)): a fresh fall class whose
  // previous motion is the old one. From the wind-up the jump class's exit clears the h_jp0 freeze
  // (cam_j_draw_windup: h_jp3_a and the rise start 2 frames early); mid-air it swaps h_jp3 for the
  // fall class's h_jp3_a.
  forceFall(ctrl) {
    this.motion = new FallMotion(this.motionId(ctrl));
    this.events.push({ frame: ctrl.frames, ev: 'forceFall' });
  }

  // FUN_005aa5b8's advance of the head layer (held after landing until the locomotion clips take over)
  advance(step = JUMP.STEP) { if (this.clock) this.clock.advance(step); }

  // one game frame. input: PlayerController.step's input; cross: ✕ pressed this frame.
  // opts: canJump env plus { armed, aimChanged }. Returns the clip requests of the frame.
  frame(ctrl, input, cross, opts = {}) {
    // FUN_00333150: ✕ first, then the stick (the state may have just become 8)
    this.takeOff(ctrl, cross, opts);
    ctrl.stickInput(input);
    stickMode(ctrl, { x: input.x || 0, z: input.z || 0, mag: input.stick ?? Math.hypot(input.x || 0, input.z || 0) });
    ctrl.intent(false);                                   // player.js airIntent applies backTurnFix's negation itself
    const reqs = this.motionUpdate(ctrl, opts);
    if (!this.frozen) { ctrl.moveTick(); ctrl.rotate(); }   // FUN_0034ea18 (C+0x1c0 bit 0x20)
    this.advance();
    ctrl.frames++;
    return reqs;
  }
}
