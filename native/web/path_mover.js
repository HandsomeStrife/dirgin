// Stage path walking: script cmd 0x131 puts a soldier on a stage path (worker 0159).
// A direct translation of the game's body mover 1 and move mode 9; notes/enemy-ai-hook.md 'Path walking'.
//
//   cmd 0x131 -> FUN_0055a6d8(a, path, 0, -1): FUN_00352cb8 (path, first / last node), the turn mode
//   (FUN_0034e960(C, 5 / 6)), FUN_0034edf0(C, 1) (mover 1) -> FUN_0034f178 -> FUN_0034f4a0 (the segment's
//   four spline points) -> FUN_0034db80(C, 9) -> FUN_0034bd10 (polyline + duration, FUN_00353480).
//   Every frame the body update FUN_0034ea18: the mover FUN_0034f8c0 -> FUN_0034fad0 (next node on
//   arrival), the move FUN_0034c388 (mode 9: position along the polyline by time, direction = spline
//   slope), time += dt, FUN_0034d870 at the segment's end, then the turn FUN_0034de50 (mode 5: face the
//   move direction).
//
// State lives on the actor's body object a.C under the game's field offsets (named below); the
// position C+0x30 and the facing row C+0x20 are a.pos / a.face (the page's actor fields).
//   C.f1c0 / C.f1c4  move flags        C.mm   +0x1c8 move mode       C.t    +0x1cc segment time
//   C.dur  +0x1d0 segment duration     C.spd  +0x1d4 constant speed  C.speed +0x1d8 ground speed
//   C.seg  +0x1dc chord length         C.cp   +0x100..+0x13f spline points
//   C.i200 / C.n204 polyline index / count   C.arc +0x208   C.arcLen +0x20c   C.tension +0x210
//   C.poly / C.arcs  the polyline C+0x3a0 and its cumulative lengths C+0x3a4
//   C.s214 turn mode, C.t2c0 +0x2c0 turn target   C.dir +0x90 move direction
//   C.mover +0x300   C.path +0x310   C.node +0x314   C.last +0x318   C.p31c..C.p32c +0x31c..+0x32c
const f = Math.fround;

// FUN_00310c40 = G+0x64: 48 in the save (the gate in FUN_0034f4a0: > 8 sets the spline's points C+0x100..)
export const G64 = 48;
// DAT_0046e290 +0xc: the time a body's move clock C+0x1cc gains per frame (FUN_0034ea18), seconds.
// T: w159_walk95 (every frame of 46 / 47's walk: C+0x1cc gains 0.02 = 1.2 ticks / 60)
export const MOVE_DT = f(0.02);

// FUN_00311120: section 9 entry i (null when i >= the count; the exporter checks the layout)
export function pathOf(paths, i) {
  return paths && i >= 0 && i < paths.length ? paths[i] : null;
}
// FUN_003217e0: node i of path p. Not looped: i < 0 -> node 1 (0 for a 1-node path), i >= n -> n - 2.
// Looped (flags bit 0): i mod n. FUN_00311180 = section 10 [first + i].
export function nodeOf(p, i) {
  const n = p.n;
  if (n < 1) return null;
  if (p.flags & 1) { i = i % n; if (i < 0) i += n; }
  else if (i < 0) i = n > 1 ? 1 : 0;
  else if (i >= n) i = n > 1 ? n - 2 : 0;
  return p.nodes[i] || null;
}
// FUN_0025fcc8: cardinal spline weights of the four points at u (tension k)
export function splineBasis(u, k) {
  const u2 = f(u * u), u3 = f(u2 * u);
  return [
    f(f(f(f(-k) * u3) + f(f(k + k) * u2)) - f(k * u)),
    f(f(f(f(2.0 - k) * u3) + f(f(k - 3.0) * u2)) + 1.0),
    f(f(f(f(k - 2.0) * u3) + f(f(3.0 - f(k + k)) * u2)) + f(k * u)),
    f(f(k * u3) - f(k * u2)),
  ];
}
// FUN_0025fd58: their slopes (d/du)
export function splineSlope(u, k) {
  const u2 = f(u * u);
  return [
    f(f(f(k * u2) * -3.0) + f(f(k * u) * 4.0)) - k,
    f(f(f(6.0 - f(k * 3.0)) * u2) + f(f(f(k + k) - 6.0) * u)),
    f(f(f(f(k * 3.0) - 6.0) * u2) + f(f(6.0 - f(k * 4.0)) * u)) + k,
    f(f(f(k * u2) * 3.0) - f(f(k + k) * u)),
  ].map(f);
}
// FUN_0026138c: p0 b.x + p1 b.y + p2 b.z + p3 b.w (VU ACC chain)
function comb(cp, b) {
  const o = [0, 0, 0, 0];
  for (let c = 0; c < 4; c++) {
    let s = f(cp[0][c] * b[0]);
    s = f(s + f(cp[1][c] * b[1]));
    s = f(s + f(cp[2][c] * b[2]));
    o[c] = f(s + f(cp[3][c] * b[3]));
  }
  return o;
}
const len3 = v => f(Math.sqrt(f(f(f(v[0] * v[0]) + f(v[1] * v[1])) + f(v[2] * v[2]))));
const dist3 = (a, b) => len3([f(a[0] - b[0]), f(a[1] - b[1]), f(a[2] - b[2])]);
const unit3 = v => { const l = len3(v); return l ? [f(v[0] / l), f(v[1] / l), f(v[2] / l)] : v.slice(0, 3); };   // vrsqrt x vmulq
const lerp4 = (a, b, t) => [0, 1, 2, 3].map(c => f(a[c] + f(f(b[c] - a[c]) * t)));                     // FUN_002611f4

// the move state a body starts with (save state: 46 / 47 idle, trace w159_walk95 f770)
export function bodyInit(C, spd) {
  Object.assign(C, {
    f1c4: C.f1c4 ?? 0x104, mm: 0, t: 0, dur: 1.0, spd: f(spd), seg: 0, cp: [[0, 0, 0, 1], [0, 0, 0, 1], [0, 0, 0, 1], [0, 0, 0, 1]],
    poly: [], arcs: [], i200: 0, n204: 0, arc: 0, arcLen: 0, tension: f(0.4), dir: [0, 0, 0],
    path: 0, node: 0, last: 0, p31c: 0, p320: 0, p324: 0, p328: 0, p32c: 0, s218: 0, s21c: 0, t2c0: null,
  });
  return C;
}

// FUN_0034e960(C, mode): turn mode C+0x214, C+0x218 = 0, C+0x1c4 bit 1 set / bit 3 clear, then FUN_0034dc50:
// mode 5 -> C+0x2c0 = (facing row, w 1.0) and C+0x21c = -1 (FUN_0034bcf8); mode 6 -> C+0x21c = -1.
export function setTurn(a, mode) {
  const C = a.C;
  C.s214 = mode; C.s218 = 0;
  C.f1c4 = ((C.f1c4 & ~8) | 2) >>> 0;
  if (mode === 5) C.t2c0 = [a.face[0], a.face[1], a.face[2], 1.0];
  if (mode === 4 || mode === 5 || mode === 6 || mode === 7 || mode === 8) C.s21c = -1.0;
}

// FUN_0055a6d8(a, path, 0, -1) after its body check; turnMode = 5 / 6 as the caller found it (FUN_0035eda0
// target and C+0x590 bit 24), or null for 'unchanged' (no AI and actor +0xe4 bit 0). Returns false when
// nothing started (no path, or the path's mover 0xe branch).
export function pathStart(a, paths, idx, turnMode, warn = () => {}) {
  const C = a.C, p = pathOf(paths, idx);
  if (!p) return false;
  if ((p.flags >> 3) & 1) {
    // FUN_0034e960(C, 0), FUN_0034db80(C, 0, 0), FUN_00352d70, FUN_0034edf0(C, 0xe): not translated
    warn(`path_mover: path ${idx} flags bit 3 (mover 0xe, FUN_00352d70) not translated: the soldier stands`);
    return false;
  }
  // FUN_00352cb8(C, idx, 0, -1)
  C.path = idx;
  C.node = 0 < p.n ? 0 : p.n - 1;
  C.last = (p.flags & 1) ? -1 : p.n - 1;
  if (turnMode != null) setTurn(a, turnMode);
  return moverStart(a, paths, warn);
}

// FUN_0034edf0(C, 1)
export function moverStart(a, paths, warn = () => {}) {
  const C = a.C;
  if (C.mover !== 0) moverStop(a, warn);
  C.f1c4 = (C.f1c4 & ~0x80) >>> 0;
  const p = pathOf(paths, C.path);
  if (!p) return false;
  if (C.node < 0 || p.n <= C.node) C.node = 0;
  C.tension = f(p.tension);                                                  // C+0x210 = path +0xc
  let lo = C.node, hi = C.last, len = 0;
  if (hi === lo) { lo = 0; hi = p.n; }
  for (let i = lo; i < hi; i++) { const nd = nodeOf(p, i); if (nd) len = f(len + f(nd.seg)); }
  if (C.f1c0 & 2) warn('path_mover: C+0x1c0 bit 1 speed profile (FUN_00349e98) not translated');
  C.p328 = len; C.p32c = C.dur; C.p320 = 0; C.p31c = 0; C.t = 0;
  C.mover = 1;
  C.f1c4 = ((C.f1c4 & ~0x100) | 0x20) >>> 0;
  // FUN_0034f178 case 1
  C.f1c4 = (C.f1c4 | 0x10) >>> 0;
  segStart(a, p, C.node, warn);
  return true;
}

// FUN_00352a60, mover 1: C+0x1d0 = C+0x32c; mover 0; C+0x1c4 bit 5 clear / bit 8 set; move mode 0
// (C+0x1c8 0xc -> FUN_0034b110 instead: not a path walker's)
export function moverStop(a, warn = () => {}) {
  const C = a.C;
  if (C.mover === 1) C.dur = C.p32c;
  else if (C.mover) warn(`path_mover: stop of mover ${C.mover} (FUN_00352a60) not translated`);
  C.mover = 0;
  C.f1c4 = ((C.f1c4 & ~0x20) | 0x100) >>> 0;
  C.mm = 0;
}

// FUN_0034f4a0(C, p, i): the segment from node i to node i + 1
function segStart(a, p, i, warn) {
  const C = a.C, fl = p.flags;
  let straight;
  if (!((fl >> 1) & 1) || !((fl >> 2) & 1)) straight = ((fl >> 2) & 1) ^ 1;
  else straight = (nodeOf(p, i).flags & 1) !== 0 ? 1 : 0;
  if (!straight) {
    const n0 = nodeOf(p, i - 1), n1 = nodeOf(p, i), n2 = nodeOf(p, i + 1), n3 = nodeOf(p, i + 2);
    // FUN_0034aea8: C+0x100..0x13f = the four nodes' +0x50 (positions). FUN_0034aed0 / FUN_0034aef8 copy
    // their +0x40 / +0x30 rows to C+0x140.. / C+0x180..: move mode 9 doesn't read them (not exported).
    if (G64 > 8) C.cp = [n0.pos, n1.pos, n2.pos, n3.pos].map(v => v.map(f));
    C.p31c = f(n1.seg);
    moveMode(a, 9, warn);
  } else {
    // FUN_0034ad30 / FUN_0034ae00 + FUN_0034db80(C, 8): a straight segment (move mode 8, FUN_002611f4 and
    // the ground drop FUN_00356d98). No z128 path takes it (flags 4 / 6 with node flags 0).
    warn('path_mover: straight path segment (move mode 8) not translated: the soldier stops');
    C.p31c = f(nodeOf(p, i).seg);
    C.mm = 0;
  }
}

// FUN_0034db80(C, mode, 0)
function moveMode(a, mode, warn) {
  const C = a.C;
  // FUN_00326298(0xd) (script flag bit 13) only changes modes 10 / 11
  const old = C.f1c4;
  C.mm = mode;
  C.f1c4 = ((old & ~4) | 1) >>> 0;
  if (!(old & 0x10)) C.t = 0;
  if (!segSetup(a, warn)) { C.t = 0; segEnd(a, 0); }
  C.f1c0 = (C.f1c0 & ~0x800) >>> 0;
}

// FUN_0034bd10, move mode 9 (constant speed: C+0x1c0 bit 0)
function segSetup(a, warn) {
  const C = a.C;
  C.poly = [C.cp[1].slice()]; C.arcs = [0];
  const d = dist3(C.cp[1], C.cp[2]);
  C.seg = d;
  if (!(0.001 <= d)) return false;
  C.n204 = d < 3.0 ? Math.trunc(f(d * 10.0)) + 2 : 0x20;
  const arc = polyline(C);
  C.p31c = arc;                                                              // mode 9
  if (C.f1c0 & 2) warn('path_mover: C+0x1c0 bit 1 speed profile (FUN_0034a0c8) not translated');
  else if (C.f1c0 & 1) C.dur = f(arc / C.spd);
  else C.dur = f(C.p32c * f(C.p31c / C.p328));
  C.arcLen = arc; C.arc = 0; C.i200 = 0;
  return true;
}

// FUN_00353480: C+0x204 points of the spline from C+0x110 to C+0x120, cumulative lengths in C+0x3a4
function polyline(C) {
  let L = 0;
  const n = C.n204;
  for (let i = 1; i < n; i++) {
    const u = f(f(i) / f(n - 1));
    const P = comb(C.cp, splineBasis(u, C.tension));
    if (C.f1c0 & 0x80) P[1] = f(C.cp[1][1] + f(u * f(C.cp[2][1] - C.cp[1][1])));   // byte C+0x1c0 < 0
    C.poly[i] = P;
    L = f(L + dist3(P, C.poly[i - 1]));
    C.arcs[i] = L;
  }
  return L;
}

// FUN_0034d870(C, flag), move mode 9 (the default case): the segment is done -> mode 0, C+0x1c4 bit 0
// clear / bit 2 set (the mover's arrival bit). Returns 1.
function segEnd(a, _flag) {
  const C = a.C;
  C.mm = 0;
  C.f1c4 = ((C.f1c4 & ~1) | 4) >>> 0;
  return 1;
}

// FUN_0034fad0 (mover 1, per frame): on arrival the next node; 1 = the path is done
function moverTick(a, paths, warn) {
  const C = a.C;
  let done = 0;
  if (C.f1c4 & 4) {
    const p = pathOf(paths, C.path);
    C.node++;
    C.p320 = f(C.p320 + C.p31c);
    // the body listener C+0x540 gets event 3 (node index): the motion side, nothing on the page reads it
    if (C.node === C.last) done = 1;
    else if ((p.flags & 1) && p.n <= C.node) C.node = 0;
    if (!done) segStart(a, p, C.node, warn);
  }
  return done;
}

// FUN_0034c388, move mode 9 (C+0x1c0 bit 1 clear): position by time along the polyline, direction =
// the spline's slope there, ground speed = C+0x1d4 (bit 0) or chord / duration
function moveEval(a) {
  const C = a.C;
  C.speed = (C.f1c0 & 1) ? C.spd : f(C.seg / C.dur);
  C.arc = f(C.arcLen * f(C.t / C.dur));
  for (let i = C.i200; i < C.n204 && C.arcs[i] < C.arc; i++) C.i200 = i;
  const i = C.i200, a0 = C.arcs[i];
  let u = f(f(C.arc - a0) / f(C.arcs[i + 1] - a0));
  if (u < 0) u = 0; else if (u > 1.0) u = 1.0;
  const P = lerp4(C.poly[i], C.poly[i + 1], u);
  a.pos = [P[0], P[1], P[2]];
  // FUN_0025fdf0: slope at (i + u) / (n - 1); zero -> again 0.001 further along (or back)
  const U = f(f(i + u) / f(C.n204 - 1));
  let d = comb(C.cp, splineSlope(U, C.tension));
  if (f(f(f(d[0] * d[0]) + f(d[1] * d[1])) + f(d[2] * d[2])) === 0)
    d = comb(C.cp, splineSlope(U <= 0.001 ? f(U + 0.001) : f(U - 0.001), C.tension));
  C.dir = len3(d) === 0 ? a.face.slice(0, 3) : unit3(d);
}

// FUN_00323318: a per-tick blend factor over the frame's step (DAT_0046e290 +4)
export function stepFactor(w, step) {
  if (!(1.0 < step)) return f(w * step);
  let r = 1.0, i = 1;
  do { i++; r = f(r * f(1.0 - w)); } while (i <= step);
  return f(1.0 - r);
}

// FUN_0034de50, turn mode 5: with ground speed, the facing takes the move direction (blend factor
// FUN_00323318(C+0x2cc): 1.0, or 0.25 with C+0x1c0 bit 15), then FUN_0034a5a8(C, 1) keeps it upright
// (C+0x1c0 bit 8: face = (up x face) x up, up = (0, -1, 0))
function turnMode5(a, step, warn) {
  const C = a.C;
  if (C.speed === 0) return;
  C.t2c0 = [C.dir[0], C.dir[1], C.dir[2], (C.f1c0 & 0x8000) ? 0.25 : 1.0];
  let tg = C.t2c0;
  if (len3(tg) === 0) tg = [...a.face.slice(0, 3), C.t2c0[3]];
  const face = a.face;
  const dot = f(f(f(face[0] * tg[0]) + f(face[1] * tg[1])) + f(face[2] * tg[2]));
  if (C.f45c & 0x200000) warn('path_mover: C+0x45c bit 21 rate-limited turn not translated');
  const k = stepFactor(C.t2c0[3], step);
  if (!(-0.97 < dot)) {
    // row 0 (up x face; C+0x1c0 bit 6 picks its sign by the camera: not set on the soldiers)
    if (C.f1c0 & 0x40) warn('path_mover: C+0x1c0 bit 6 turn-around by the camera not translated');
    tg = [f(-face[2]), 0, f(face[0])];
  }
  let v = unit3([0, 1, 2].map(c => f(face[c] + f(f(tg[c] - face[c]) * k))));
  if (C.f1c0 & 0x100) v = unit3([v[0], 0, v[2]]);
  else warn('path_mover: FUN_0034a5a8 without C+0x1c0 bit 8 not translated');
  a.face = v;
}

// the turn alone (tests: hand-built facing / direction / ground speed)
export const turnTest = (a, step, warn = () => {}) => turnMode5(a, step, warn);

// FUN_0034ea18 for a path walker: mover, move, clock, segment end, turn. step = the frame's step
// (DAT_0046e290 +4, 1.2), dt = DAT_0046e290 +0xc. Returns 'done' on the frame the path ends.
export function bodyFrame(a, paths, step, dt = MOVE_DT, warn = () => {}) {
  const C = a.C;
  if (C.f1c0 & 0x20) return null;
  if (!(step > 0)) return null;
  let ev = null;
  if (C.mover === 1) {
    if (moverTick(a, paths, warn)) {
      // FUN_0034f8c0: FUN_00352a60, then the listener's events 2 and 0, C+0x1c4 bit 6. Event 2 = the path end
      // (enemy_ai moverUpdate). Event 0 (FUN_005690c8 case 0): its ground drop FUN_00356d98 runs only with
      // C+0x300 == 1 or motion class 0x42; the mover is already 0 here and the class is 8, so it doesn't.
      // Not read: its FUN_00588550 case 0 and FUN_00563970 (actor +0x108 != -1).
      moverStop(a, warn);
      C.f1c4 = (C.f1c4 | 0x40) >>> 0;
      ev = 'done';
    }
  }
  if (C.mm === 0) {
    C.speed = 0;
  } else {
    moveEval(a);
    C.t = f(C.t + dt);
    if (!(C.t < C.dur) && !(C.dur < 0.0)) {
      if (segEnd(a, 1) && C.mm === 0) C.t = f(C.t - C.dur);
    }
  }
  // MISSING: FUN_0034ea18 sends the body listener event 1 on each frame of a turn (FUN_005690c8 case 1 ->
  // FUN_00588550, jump table 0x591ef0 case 1: not read; nothing on the page depends on it).
  if (C.s214 === 5) turnMode5(a, step, warn);
  return ev;
}
