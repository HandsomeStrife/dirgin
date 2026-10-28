// Vincent's cape cloth: a direct translation of the game's cloth step (PAL SLES_541.85).
//
// The game simulates the cape's 82 cloth VERTICES (not bones). Each frame it skins every
// vertex to a keyed target from the clip's bones, then runs one cloth step that moves the
// free vertices and pulls them back toward that target. Data: data/vincent/cloth.json
// (cape block 0x19f70 of 07700001.bin via p000_cloth.Cape, plus each clip's window bytes).
//
// Per drawn frame (50/s on PAL), in this order (FUN_005ab8f0 draws: FUN_005b2ef8 -> cloth at
// its line 143, FUN_005aa5b8 motion advance + window decrement at its line 218):
//   1. clip start        FUN_005a95f0  window armed from the clip (flag +0x19, u16 +0x1a)
//   2. targets           FUN_005b06d0  weighted skin of the 82 vertices
//   3. k                 FUN_005b1128  k = (1 - rem/total)^2, or 1.0
//   4. cloth step        FUN_005b0d30  -> FUN_005b0b38, FUN_005b09f8 x4, FUN_005b0c50
//   5. decrement         FUN_005aa5b8  rem -= 1 when rem > 0 and the frame advance != 0
//   6. facing check      FUN_005a6f40  rem = total when the facing turned (dot < 0.992)
// Steps 5 -> 6 order: measured from the trace (tools/trace: c_run records 10-12, d_turn
// 104-107 show a turn re-arm the decrement did not touch, and a turn re-arm the same
// frame's cloth did not see). The caller chain that puts FUN_005a6f40 there is not traced.
//
// The window does NOT re-arm when a clip loops: FUN_005aa2a8's loop branch just resets the
// frame; FUN_005a95f0 runs only on a clip request (FUN_005a91c8 / FUN_005a9398) and on a
// queued layer taking over (FUN_005aa5b8). There is no separate "turn window": a turn copies
// the current total (whatever the last flag-2 clip set) into the remaining count.
//
// Space: the game skins into the character matrix's space, i.e. world. The web passes
// three.js's skin matrices (bone.matrixWorld x boneInverse x bindMatrix), which map the
// bind-model point q of each influence to world, so this module works in game world space.

const CONSTRAINT_PASSES = 4;            // FUN_005b0d30: iVar4 = 3 .. 0 (do-while, 4 calls)
const CONSTRAINT_GAIN = 0.8;            // FUN_005b09f8: 0x3f4ccccd
const TURN_DOT = 0.992;                 // FUN_005a6f40
const PINNED = 0x8000;                  // vertex list bit 15

export class CapeCloth {
  constructor(data) {
    this.data = data;
    this.N = data.N;
    const g = data.group;
    this.gravity = g.gravity;           // group +0x00 (0.2)
    this.blend = g.blend;               // group +0x04 (0.1)
    this.damping = g.damping;           // group +0x08 (0.1)
    this.list = Int32Array.from(g.list);                  // group +0x20 + 8 x count
    this.ca = Int32Array.from(g.constraints, c => c[0]);  // u8 a (vertex-list index)
    this.cb = Int32Array.from(g.constraints, c => c[1]);  // u8 b
    this.rest = Float32Array.from(g.constraints, c => c[2]);
    // the cloth buffer, *(block+0x1c): vel at +0, pos at +N*0x10, target at +N*0x20
    this.vel = new Float32Array(this.N * 3);
    this.pos = new Float32Array(this.N * 3);
    this.target = new Float32Array(this.N * 3);
    // skin records, FUN_005b06d0: [vertex, bone, weight, local xyz, bind-model xyz]
    this.skinRec = data.skin;
    this.windows = data.clip_windows || {};
    // motion-player window state: MP+0x434 total, MP+0x438 remaining, MP+0xf0 last facing
    this.total = 0;
    this.remaining = 0;
    this.lastFacing = null;
    // GUESS: MP+0x440 bit 0x100000 held clear (meaning not decoded; clear in every trace
    // record, mp_head; see notes/fidelity-audit.md "Web frame loop and cape cloth"). Set, it blocks the turn re-arm.
    this.mpFlag100000 = false;
    this.cloth_on = true;               // DAT_005b37ac (global cloth switch; 1 in game)
    this.started = false;
  }

  // ---- FUN_005a95f0: a clip starts on the head layer
  clipStart(name) {
    const w = this.windows[name];
    if (!w) return;                     // clip not in the data: leave the window alone
    const [flag, n16] = w;
    if (flag === 0) { this.remaining = 0; this.total = 0; }
    else if (flag === 2) { this.total = n16 << 1; this.remaining = n16 << 1; }
  }

  // ---- FUN_005b1128
  k() {
    if (this.cloth_on && this.total !== 0) {
      const f = 1.0 - this.remaining / this.total;
      return Math.fround(f * f);
    }
    return 1.0;
  }

  // ---- FUN_005b06d0: keyed targets. skinMat(bone) -> 16 floats, column-major, that map the
  // influence's bind-model point (q) to world.
  skin(skinMat) {
    const T = this.target;
    T.fill(0);
    const acc = new Float64Array(this.N * 3);
    for (const r of this.skinRec) {
      const v = r[0], w = r[2], m = skinMat(r[1]);
      const x = r[6], y = r[7], z = r[8];
      acc[3 * v]     += w * (m[0] * x + m[4] * y + m[8] * z + m[12]);
      acc[3 * v + 1] += w * (m[1] * x + m[5] * y + m[9] * z + m[13]);
      acc[3 * v + 2] += w * (m[2] * x + m[6] * y + m[10] * z + m[14]);
    }
    T.set(acc);
  }

  // Set the targets directly (tests): t = Float32Array(N*3), world.
  setTargets(t) { this.target.set(t); }

  // GUESS: cloth buffer start state (not decoded; see notes/fidelity-audit.md "Web frame loop and cape cloth").
  // pos = target, vel = 0. With k = 1 the game's own step reaches its fixed point
  // (vel.y = g*d/(1-d) = 0.0222, x = z = 0) within ~5 frames, and pos == target exactly.
  reset() {
    this.pos.set(this.target);
    this.vel.fill(0);
    this.started = true;
  }

  // ---- FUN_005b0d30(k, ...): one cloth step for the group.
  // vblanks = DAT_0047e1e0 (vblanks this frame; 1 at a steady 50 fps). advance = the
  // frame advance DAT_004fc7e8 (0 when paused, DAT_004fc7e4 == 1).
  step(k, vblanks = 1, advance = 1) {
    if (!this.started) this.reset();
    if (!(vblanks < 2 || k === 1.0)) return;
    this._integrate(advance);
    for (let it = 0; it < CONSTRAINT_PASSES; it++) this._constrain();
    this._pull(k);
  }

  // ---- FUN_005b0b38: integrate. Pinned: pos = target. Free: v.y += gravity (when the
  // frame advance is not 0; NOT scaled by the step), pos += v, v = (pos - old) * damping.
  _integrate(advance) {
    const { list, pos, vel, target: T } = this;
    for (let i = 0; i < list.length; i++) {
      const e = list[i], v = (e & 0x7fff) * 3;
      if ((e & PINNED) === 0) {
        let vy = vel[v + 1];
        if (advance !== 0) vy = Math.fround(vy + this.gravity);
        const ox = pos[v], oy = pos[v + 1], oz = pos[v + 2];
        pos[v] = ox + vel[v]; pos[v + 1] = oy + vy; pos[v + 2] = oz + vel[v + 2];
        vel[v] = (pos[v] - ox) * this.damping;
        vel[v + 1] = (pos[v + 1] - oy) * this.damping;
        vel[v + 2] = (pos[v + 2] - oz) * this.damping;
      } else {
        pos[v] = T[v]; pos[v + 1] = T[v + 1]; pos[v + 2] = T[v + 2];
      }
    }
  }

  // ---- FUN_005b09f8: one pass over the constraints in file order, stretch-only.
  _constrain() {
    const { list, ca, cb, rest, pos } = this;
    for (let c = 0; c < ca.length; c++) {
      const ea = list[ca[c]], eb = list[cb[c]];
      const a = (ea & 0x7fff) * 3, b = (eb & 0x7fff) * 3;
      const dx = pos[a] - pos[b], dy = pos[a + 1] - pos[b + 1], dz = pos[a + 2] - pos[b + 2];
      const L = Math.fround(Math.sqrt(dx * dx + dy * dy + dz * dz));
      if (rest[c] < L) {
        const s = Math.fround((1.0 / L) * (L - rest[c]) * CONSTRAINT_GAIN);
        const cx = dx * s, cy = dy * s, cz = dz * s;
        if ((ea & PINNED) === 0) { pos[a] -= cx; pos[a + 1] -= cy; pos[a + 2] -= cz; }
        if ((eb & PINNED) === 0) { pos[b] += cx; pos[b + 1] += cy; pos[b + 2] += cz; }
      }
    }
  }

  // ---- FUN_005b0c50: f = max(group blend, k). Free and f != 1: pos += (target - pos) * f;
  // otherwise pos = target.
  _pull(k) {
    const { list, pos, target: T } = this;
    let f = this.blend;
    if (f <= k) f = k;
    for (let i = 0; i < list.length; i++) {
      const e = list[i], v = (e & 0x7fff) * 3;
      if ((e & PINNED) === 0 && f !== 1.0) {
        pos[v] += (T[v] - pos[v]) * f;
        pos[v + 1] += (T[v + 1] - pos[v + 1]) * f;
        pos[v + 2] += (T[v + 2] - pos[v + 2]) * f;
      } else {
        pos[v] = T[v]; pos[v + 1] = T[v + 1]; pos[v + 2] = T[v + 2];
      }
    }
  }

  // ---- FUN_005aa5b8 tail (LAB_005aa7dc): rem -= 1 when rem > 0 and the advance != 0.
  // A queued layer taking over calls FUN_005a95f0 and returns before this (pass
  // queuedTakeover = true for that frame).
  decrement(advance = 1, queuedTakeover = false) {
    if (queuedTakeover) return;
    if (this.remaining > 0 && advance !== 0) this.remaining -= 1;
  }

  // ---- FUN_005a6f40: fx = the character's x axis (MP+0x10 matrix x (1,0,0,0)), unit.
  facing(fx) {
    if (this.lastFacing && !this.mpFlag100000 && this.total !== 0) {
      const p = this.lastFacing;
      const d = Math.fround(fx[0] * p[0] + fx[1] * p[1] + fx[2] * p[2]);
      if (d < TURN_DOT) this.remaining = this.total;
    }
    this.lastFacing = [fx[0], fx[1], fx[2]];
  }

  // One drawn frame in the game's order (header). clipStarted = head clip name when a clip
  // request (FUN_005a91c8 / FUN_005a9398) started it this frame, before the draw;
  // queuedStart = clip name when a queued layer took over inside FUN_005aa5b8 (armed AFTER
  // this frame's cloth, and that call returns before the decrement). skinMat / targets: this
  // frame's keyed pose; fx: the facing x axis after this frame's logic.
  frame({ clipStarted = null, queuedStart = null, skinMat = null, targets = null, fx = null, vblanks = 1, advance = 1 }) {
    if (clipStarted) this.clipStart(clipStarted);
    if (skinMat) this.skin(skinMat); else if (targets) this.setTargets(targets);
    const k = this.k();
    this.step(k, vblanks, advance);
    if (queuedStart) this.clipStart(queuedStart);
    this.decrement(advance, !!queuedStart);
    if (fx) this.facing(fx);
    return k;
  }
}
