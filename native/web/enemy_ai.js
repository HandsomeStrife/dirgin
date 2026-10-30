// Enemy AI for z128's e027 soldiers: wake by trigger volume + event script, and the awake
// combat AI (brain mode 2, combat layer 2, fire timer, shot, bullet, damage to Vincent).
// A direct translation of the game's functions (addresses cited per function); decode notes
// notes/enemy-research.md §8 / §9, hook patch for play.js / enemies.js notes/enemy-ai-hook.md.
// Pure logic: no three.js. Tested against PCSX2 traces by test_enemy_ai.mjs.
//
// Data (gitignored, tools/byo/build_web.sh): data/enemies/ai.json from
// native/assets/models/export_web_triggers.py (stage file sections 1, 2/3, 8; e027 clip lengths).
//
// Frame order inside one game frame (GUESS: the game's top-level object/AI/body update order
// is not decoded; this order reproduces k_enemy_engage7/8, see notes/enemy-ai-hook.md):
//   1. AI scheduler FUN_00365008 (AIs FUN_00365110, then the fire timer FUN_003656a0)
//   2. bullets FUN_005549f0 (a bullet spawned in step 3 is first updated next frame)
//   3. bodies: motion state update (class 0xe FUN_00596460; spawns the bullet)
//   4. object messages queued last frame delivered (FUN_0053e9e0's consumer, not decoded; the
//      trace: #67 re-queued with interval 15 on the wake frame, so after the AI step)
//   5. player moved -> trigger volumes FUN_005517a8(old, new) (they queue the wake messages)

// ---------------------------------------------------------------- constants
// Enemy weapon W (status +0xac) = stage section 24 record (FUN_00318610 / FUN_00311600), index by
// FUN_00318718: from ai.json (export_web_triggers.py: data.weapons, data.records[k].weapon).
export const R_ACTOR = 5.0;       // actor +0x2c radius: AI+0x34 - AI+0x38 = 10 = both radii (trace k_enemy_engage8). GUESS: not read from data
export const EYE_H = 18.0;        // GUESS: actor +0x30 height for FUN_005634e0 (not decoded; 18.0 is its own no-body default)
export const PLAYER_AIM_H = 12.4; // GUESS: player vt+0x5c aim point = feet - 12.4 up (trace AI+0x40 at f784; vt+0x5c not decoded)
// Vincent's status fields read by FUN_00318120 (defence), and the game globals it reads. GUESS:
// values read from the sea-of-flames save state's RAM (P status = P+0x17c); their source on the
// disc / in the save file is not decoded. lvl +0x11 4, +0xc0 armour record 0, +0x60 0x8233,
// +0x27 0, +0x64 0, his weapon W+0x68 0 (export_web_aim.py compose() of the save's parts),
// FUN_00317630 0, param set +0x9c 0. With them a 45 shot does 44 (trace f789 / f1011).
export const PLAYER_STATUS = { lvl: 4, armour: null, f60: 0x8233, f27: 0, f64: 0, u20top: 0, w68: 0, b317630: 0, elem9c: 0 };
// GUESS (save RAM): G = *DAT_0046dd68: G+0x60 story progress 20, G+0x14 table +0x270 level factors,
// +0xaa / +0xae / +0xb2 difficulty cuts; DAT_004b8ac8 option bits 0, DAT_0046b328 1, DAT_0046b2fc 2.
// ELF data: DAT_0046e220 {0, 50} (level factors before progress 7), DAT_004a02b0 element factors.
export const GAME = { progress: 20, lvlK: [0.33, 0.4], lvlK0: [0, 50], cut: { aa: 0, ae: 20, b2: 100 },
                      bits4b8ac8: 0, d46b328: 1, d46b2fc: 2, elem: [1, 0, 0.25, 0.5, 2], armour: null };
export const MANAGER_SHORT_18 = 1; // GUESS: manager short 18 (cmd 0x30); trace: script 108 event 2 took the mode-2 branch, so != 0
export const JITTER_ON = GAME.d46b2fc !== 0;   // FUN_00326500 = FUN_002424a0 = DAT_0046b2fc != 0 (notes §9.6)
const BULLET_R = 5.0;             // GUESS: hit radius around the player's aim point (part test vt+0x17c not decoded; = R_ACTOR)
// FUN_005a5xxx motion ids -> clip names (notes §9.7, trace: 0x57 = r_st_f, 0x40 = r_st_a). GUESS for
// actor state 3 -> 0x40: the state table behind FUN_0055b8c0 vt+0x24 is not decoded.
export const MOTION = { 0x40: 'r_st_a', 0x57: 'r_st_f' };
// The page's save state: FUN_00242c28() = 1 (worker 0125, notes §9.11); event flags = evt_flags XOR
// evt_key in k_enemy_engage7: only flag 2 = 7 and flag 46 = 1 (notes/enemy-activation.md §1)
export const SAVE_DIFFICULTY = 1;
export const SAVE_EVENT_FLAGS = Object.freeze({ 2: 7, 46: 1 });
const ID_BASE = 22;               // GUESS: manager +0x3b2 table (cmd 0x3a): object id = record + 22 (trace meta EI0..6: rec 44 -> 66, 66 -> 88)

// ---------------------------------------------------------------- RNG FUN_0040b398 / 0040b500 / 0040b5c0
// The game's Mersenne twister (624 words at DAT_004fbe00, index DAT_004fc7c0). It returns the
// tempered *current* word, then twists that word in place.
export class GameRandom {
  constructor(seed = 0) { this.s = new Uint32Array(624); this.seed(seed); }   // GUESS: seed and call history at stage start unknown
  seed(p) {                                                                     // FUN_0040b398
    const s = this.s;
    s[0] = p !== 0 ? p : p + 1;
    for (let i = 1; i < 624; i++) s[i] = (Math.imul(s[i - 1] ^ (s[i - 1] >>> 30), 0x6c078965) + i) >>> 0;
    this.i = 0;
    for (let k = 0; k < 624; k++) this.twist(k);
    this.i = 0;
  }
  twist(i) {
    const s = this.s, n = i < 623 ? s[i + 1] : s[0];
    let y = ((s[i] & 0x80000000) | (n & 0x7fffffff)) >>> 1;
    if (n & 1) y ^= 0x9908b0df;
    s[i] = (y ^ (i < 0xe3 ? s[i + 397] : s[i - 227])) >>> 0;
  }
  next() {                                                                      // FUN_0040b500
    const i = this.i;
    let y = this.s[i];
    this.twist(i);
    this.i = i < 0x26f ? i + 1 : 0;
    y ^= y >>> 11; y ^= (y & 0x13a58ad) << 7; y ^= (y & 0x1df8c) << 15; y ^= y >>> 18;
    return y >>> 0;
  }
  rand() {                                                                      // FUN_0040b5c0
    const b = new DataView(new ArrayBuffer(4));
    b.setUint32(0, (this.next() & 0x7fffff) | 0x3f800000);
    return Math.fround(b.getFloat32(0) - 1.0);
  }
}

// ---------------------------------------------------------------- defence FUN_00318120
// Damage after the victim's defence. st: the victim's status fields (PLAYER_STATUS layout),
// raw: bullet +0x134, kind: FUN_00554738(bullet) (0 for an e027 bullet: flag 2 clear, W+4 bit
// 0x80000 clear). Float maths in single precision as the game's FPU; (uint) casts truncate.
const f = Math.fround;
export function defence(st, raw, kind, G = GAME) {
  if (kind === 3) return raw;
  let L = 0;
  const armour = st.armour;                                   // status +0xc0
  if (st.w68 != null && st.b317630 !== 2 && kind !== 2) L = st.w68;   // FUN_00317c70 W+0x68 (FUN_00317630 != 2)
  let noArmour = false, f60 = st.f60;
  if (!armour) {
    if (kind !== 2) L += Math.trunc(f(f(G.progress < 7 ? G.lvlK0[0] : G.lvlK[0]) * st.lvl + (G.progress < 7 ? G.lvlK0[1] : G.lvlK[1])) + 0.5);   // FUN_0031ce90
    noArmour = (f60 & 2) === 0;
  } else {
    const i = kind === 1 ? armour.u10 >>> 28 : kind === 2 ? armour.b14 >> 4 : armour.b14 & 0xf;
    L += G.armour[i];                                         // G+0x14 table +0x46c (s16)
  }
  if (L > 99) L = 99;
  let d = Math.trunc(f(f(f(raw) + 0.5) * f(100 - L) * f(0.01)));
  if ((f60 & 2) && !(f60 & 0x100)) {                          // difficulty cut
    let c = 0, ok = true;
    if (G.progress > 0x11) {                                  // FUN_00310c60
      const d242c28 = (G.d46b2fc !== 0 && G.d46b328 === 0) ? 1 : G.d46b328;   // FUN_00242c28
      if (G.d46b2fc === 0) c = d242c28;                       // FUN_00312698
      else c = ((G.bits4b8ac8 >>> 0x11) & 1) ? 0 : d242c28 - 1;   // FUN_002419d8(0x11)
      c = c === 1 ? G.cut.ae : (c < 2 || c > 3) ? G.cut.aa : G.cut.b2;
      c = -c;
      ok = c < 100;
    }
    if (!ok) c = 99;
    d = Math.trunc(f(f(f(d) + 0.5) * f(100 - c) * f(0.01)));
  }
  let immune = false;
  if (noArmour) {                                             // element FUN_0031d388 by param set +0x9c
    let u = st.elem9c;
    if (kind === 1) u >>= 3; else if (kind >= 2) u = (u >> 6) & 7;
    if (kind >= 0 && kind <= 3) {                          // FUN_00318120: negative kind skips the element step
      if (G.elem[u & 7] == null) throw new Error(`defence: element ${u & 7} past the 5-entry table DAT_004a02b0`);
      d = Math.trunc(f(f(d) * f(G.elem[u & 7]) + 0.5)); immune = (u & 7) === 1;
    }
  }
  if ((st.f27 & 0xf) === 1) d = Math.trunc(f(f(d) * f(0.2)));                    // FUN_003177a8
  else if ((st.f64 & 0x20) && st.u20top === 2) d = Math.trunc(f(f(d) * f(G.d46b2fc === 0 ? 0.33 : 0.2)));   // FUN_00317b68
  if (raw > 0 && d === 0) d = immune ? 0 : 1;
  return d < 10000 ? d : 9999;
}

// ---------------------------------------------------------------- small helpers
const short = v => (v << 16) >> 16;
const sub3 = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const len3 = v => Math.hypot(v[0], v[1], v[2]);
const dot3 = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const unit3 = v => { const l = len3(v); return l ? [f(v[0] / l), f(v[1] / l), f(v[2] / l)] : v.slice(); };   // VU vrsqrt x vmulq

// FUN_00323318(r): per-frame fraction for a per-tick rate at step *(DAT_0046e290+4) (as player.js fracStep)
export function fracStep(r, step) {
  if (!(1.0 < step)) return f(r * step);
  let i = 1, k = 1.0;
  if (1.0 <= step) do { i++; k = f(k * f(1.0 - r)); } while (i <= step);
  return f(1.0 - k);
}

// Body yaw toward the aim target, one frame: FUN_0034de50 (from FUN_0034ea18) while C+0x214 = 6.
// Case 6: C+0x2c0 = unit(C+0x2b0 - C+0x30) (3D), C+0x2cc = 0.5; then LAB_0034e2f0: if the target
// is behind (face . dir <= -0.97) dir = row 0 (C+0x1c0 bit 0x40 clear: e027 0x10301), else dir as is;
// face = face + (dir - face) x FUN_00323318(0.5) (FUN_002611f4), unit; FUN_0034a5a8(C, 1) rebuilds
// the rows from the up row (0,-1,0): face loses its y and is renormalised (row 0 = up x face,
// row 2 = row 0 x up). Not taken: the rate-limited branch for C+0x45c bit 21 (FUN_0025a560 by
// C+0x478 x DAT_004eeb28); the lerp branch fits #66 / #67 per frame (test_enemy_ai 'facing').
// Not translated: C+0x224 (turn rate written for the motion code).
export function turnBody(face, pos, t2b0, step) {
  let dir = unit3(sub3(t2b0, pos));
  if (len3(dir) === 0) dir = face.slice();
  if (!(-0.97 < dot3(face, dir))) dir = [f(-face[2]), 0, f(face[0])];      // row 0 = up x face, up (0,-1,0)
  const k = fracStep(0.5, step);
  let v = unit3([0, 1, 2].map(i => f(face[i] + f(f(dir[i] - face[i]) * k))));
  v = unit3([v[0], 0, v[2]]);                                                 // FUN_0034a5a8 with up = (0,-1,0)
  return v;
}
// distance from point c to segment a-b (for the BULLET_R stand-in)
function segPointDist(a, b, c) {
  const ab = sub3(b, a), ac = sub3(c, a), l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
  const t = l2 > 0 ? Math.max(0, Math.min(1, (ac[0] * ab[0] + ac[1] * ab[1] + ac[2] * ab[2]) / l2)) : 0;
  return len3([ac[0] - ab[0] * t, ac[1] - ab[1] * t, ac[2] - ab[2] * t]);
}
// row-vector transform (PS2 / VU convention: p' = [x y z 1] . M, row 3 = translation)
const xform = (m, p) => [0, 1, 2].map(c => p[0] * m[0][c] + p[1] * m[1][c] + p[2] * m[2][c] + m[3][c]);

// FUN_00261218: point inside a box
function inBox(p, lo, hi) {
  return p[0] >= lo[0] && p[0] <= hi[0] && p[1] >= lo[1] && p[1] <= hi[1] && p[2] >= lo[2] && p[2] <= hi[2];
}
// FUN_00550918 stand-in: segment a->b crosses the box (slab test). GUESS: the game's returned
// face index (passed as event arg r0) is not decoded; this returns 0 on a hit, -1 on none.
function segBox(a, b, lo, hi) {
  let t0 = 0, t1 = 1;
  for (let k = 0; k < 3; k++) {
    const d = b[k] - a[k];
    if (Math.abs(d) < 1e-12) { if (a[k] < lo[k] || a[k] > hi[k]) return -1; continue; }
    let u = (lo[k] - a[k]) / d, v = (hi[k] - a[k]) / d;
    if (u > v) [u, v] = [v, u];
    t0 = Math.max(t0, u); t1 = Math.min(t1, v);
    if (t0 > t1) return -1;
  }
  return 0;
}

// ---------------------------------------------------------------- the world
// createEnemyAI(opts):
//   data    ai.json (export_web_triggers.py)
//   player  { pos:[x,y,z], hp, id: 0x16, f118: 0, s64: 0 }   (P status +0x20 HP; logged on change)
//   rand    () => [0,1): FUN_0040b5c0 (default: GameRandom(0))
//   los     (a, b) => true when the probe-3 ray a->b is blocked (FUN_005449f0). Default: never
//           blocked (GUESS until the page passes its room collision ray).
//   damage  true: bullets test and damage the player (default false: the hit test is a GUESS,
//           see BULLET_R; the page turns it on with ?enemydmg=1)
//   log     (msg) => void
//   difficulty  FUN_00242c28() (default SAVE_DIFFICULTY = 1: manager 0xa6 / 0xa7 are false)
//   eventFlags  {N: value} event flags (FUN_003ff738, manager cmd 0xc); default SAVE_EVENT_FLAGS
//   deadRecs    section-1 records whose status is dead at the start (the save: 41-43); FUN_00559748
//               refuses to activate them
//   onActivate  (record, actor|null) => void: a section-1 record was activated (cmd 0x108 ->
//               FUN_00559748). actor = the AI actor (e027 only); null for other models (their
//               bodies / AIs are not exported yet). Also listed per frame in `activated`.
//   onDeactivate (record) => void: cmd 0x109 / 0x130 turned an active record off.
export function createEnemyAI(opts) {
  const data = opts.data;
  if (!data || !Array.isArray(data.weapons) || !Array.isArray(data.records)) throw new Error('ai.json has no weapons / records (old export: rebuild data/enemies)');
  const P = opts.player;
  const rng = opts.rand || (() => (opts._rng || (opts._rng = new GameRandom(0))).rand());
  const los = opts.los || (() => false);
  const log = opts.log || (() => {});
  const warned = new Set();
  const warnOnce = m => { if (warned.has(m)) return; warned.add(m); log(m); if (typeof console !== 'undefined' && !opts.quiet) console.warn(m); };
  const DAMAGE = !!opts.damage;
  const PST = opts.playerStatus || PLAYER_STATUS;
  const scripts = data.scripts;
  // manager shorts (+0x28, cmd 0x2f writes, 0x30 reads). GUESS: their values at stage start are not
  // decoded; short 18 = MANAGER_SHORT_18, the rest 0.
  const mshort = new Int16Array(0x100);
  mshort[18] = MANAGER_SHORT_18;

  // AI scheduler (*DAT_0046f4e4): +0 clock, +8 step accumulator, +0x10 rays / +0x11 shots /
  // +0x12 runs / +0x13 total this tick, +0x20 AI queue (sorted by due), +0x2c fire timers.
  const S = { now: opts.now ?? 0, acc: opts.acc ?? 0, rays: 0, shots: 0, runs: 0, total: 0, q: [], timers: [] };
  const objects = new Map();    // object id -> actor or trigger object
  const actors = [];
  const bullets = [];
  const mailbox = [];           // 0x101 messages posted this frame, delivered next frame (GUESS: FUN_0053e9e0 consumer not decoded)
  const events = [];            // per-frame record of what happened (tests / page debug)

  // ------------------------------------------------ objects
  // Trigger volumes = section-8 objects; vars at +0xa0 (cmd 0x104 / 0x105, FUN_0054cd70).
  const triggers = data.triggers.map(t => {
    const o = { kind: 'trigger', id: 't' + t.idx, t, vars: new Int16Array(8), flags28: 0, script: t.script };
    objects.set(o.id, o);
    return o;
  });
  const recById = id => data.records[id - ID_BASE];
  // FUN_00550ad8 (stage start, L: its caller is a vtable slot, not traced): each trigger object is
  // activated (+0x144(1)) unless its flags +0xa4 have bit 17 (0x20000); those wait for a script's
  // cmd 0x108 (manager 0x3d lookup). L: an inactive trigger runs no volume test (FUN_005517a8).
  for (const o of triggers) o.active = !((o.t.flags >>> 17) & 1);
  // GUESS: trigger object id = 't' + index (manager +0x7b0 table, cmd 0x3d, not read; scripts only pass it on)
  const trigObj = k => triggers[k] ? triggers[k].id : -1;
  // Section-1 records exist as (inactive) objects from stage load; cmd 0x3a gives their id and
  // 0x101 / 0x108 reach them by it. addActor replaces the record object with the active actor.
  const DIFF = opts.difficulty ?? SAVE_DIFFICULTY;
  const EVFLAGS = opts.eventFlags ?? SAVE_EVENT_FLAGS;
  const deadRecs = new Set(opts.deadRecs ?? []);
  const onActivate = opts.onActivate || (() => {});
  const onDeactivate = opts.onDeactivate || (() => {});
  const activated = [];         // records activated this frame (tick clears it)
  data.records.forEach((rec, k) => {
    const id = k + ID_BASE;
    if (!objects.has(id)) objects.set(id, { kind: 'record', id, rec: k, script: rec.script, vars: new Int16Array(8), flags28: 0, ai: null, hp: deadRecs.has(k) ? 0 : 1 });
  });

  // addActor: an activated e027 (FUN_00559748 gave it a body and an AI). init may carry the
  // state of a traced actor (test) or nothing (page).
  function addActor(init) {
    const rec = data.records[init.rec];
    if (!rec) throw new Error(`ai.json: no section-1 record ${init.rec}`);
    // FUN_00318718: weapon index (section-1 +0x54 or parameter set +0x88), -1 = none
    const W = rec.weapon >= 0 ? data.weapons[rec.weapon] ?? null : null;
    const id = init.id ?? init.rec + ID_BASE;
    const prev = objects.get(id);                // the record object: its vars (0x104) carry over
    const a = {
      kind: 'actor', id, rec: init.rec, script: init.script ?? rec.script,
      pos: init.pos || rec.pos.slice(), face: init.face || rec.face.slice(),
      vars: prev && prev.kind === 'record' ? prev.vars : new Int16Array(8),
      flags28: 1,                                // +0x28 bit 0 = active (FUN_00559748 tests it)
      f118: init.f118 ?? 0, target: init.target ?? null, aim120: null,
      astate: init.astate ?? 0, hp: init.hp ?? 100, s64: 0, f19c: null,
      // body C
      C: { f564: init.f564 ?? 0, speed: 0, s214: 0, aim: null, t2b0: null, motion: null, clip: init.clip || 'r_gst' },
      ai: null,
    };
    a.ai = {
      owner: a, flags: init.aiFlags ?? 0x100, due: init.due ?? S.now, last: init.last ?? S.now,
      interval: init.interval ?? 15, bstate: init.bstate ?? 0, btime: 0, atime: 0, cstate: 0, ctime: 0,
      d34: 10, d38: 10, d3c: 10, d40: 0, seen: 0, lost: 0, perc: init.perc ?? -1000,
      rounds: init.rounds ?? (W ? W.mag : 0), mode: init.mode ?? 0, amode: 0, cmode: 0, brain: null, timer: null,
      W, hits: 0,
    };
    a.C.t2b0 = a.pos.map((x, i) => f(x + a.face[i]));                           // FUN_0034b4c8: C+0x2b0 = facing + position (no turn)
    objects.set(a.id, a);
    actors.push(a);
    queueInsert(a.ai);
    // a dead actor's AI is in mode 0: the death path FUN_0055cac8 -> FUN_0035f728 (notes §9.10 f843)
    if (dead(a)) { if (a.ai.mode) setMode(a.ai, 0); }
    else if (a.ai.mode) setMode(a.ai, a.ai.mode);
    return a;
  }

  // ------------------------------------------------ script VM FUN_00371318 (ops: tools/stage_events.py)
  // Runs event `ev` of object o's script (vtable +0x2f4, FUN_003715e0) with args r0..r3.
  function runEvent(o, ev, args = []) {
    const sc = scripts[String(o.script)];
    const fn = sc && sc[String(ev)];
    if (!fn) return;
    const code = fn.code, st = [], loc = new Array(fn.locals + 8).fill(0);
    let r = [args[0] ?? 0, args[1] ?? 0, args[2] ?? 0, args[3] ?? 0];
    events.push(`event ${o.id} script ${o.script} ev ${ev} r ${r}`);
    const pop = () => st.length ? st.pop() : 0;
    for (let pc = 0, n = 0; pc < code.length && n < 10000; n++) {
      const [op, imm] = code[pc++];
      switch (op) {
        case 0: case 31: case 32: break;                       // nop, nop31, enter
        case 1: case 17: return;                               // halt, ret
        case 2: pop(); break;                                  // drop
        case 3: st.push(st[st.length - 1]); break;             // dup
        case 4: case 5: st.push(imm); break;                   // push int / float
        case 6: { const b = pop(), a = pop(); st.push(a + b); break; }
        case 8: { const b = pop(), a = pop(); st.push(a - b); break; }
        case 14: pc = imm; break;                              // jmp
        case 18: case 19: case 20: case 21: case 22: case 23: { // compares (a op b)
          const b = pop(), a = pop();
          st.push(+[a > b, a < b, a >= b, a <= b, a === b, a !== b][op - 18]); break;
        }
        case 24: st.push(r[imm]); break;                       // push reg
        case 25: st.push(loc[imm]); break;                     // push local
        case 26: r[imm] = pop(); break;                        // pop reg
        case 27: loc[imm] = pop(); break;                      // pop local
        case 28: st.push(+!pop()); break;                      // not
        case 29: if (!pop()) pc = imm; break;                  // jz
        case 30: if (pop()) pc = imm; break;                   // jnz
        case 33: case 34: case 35: loc[imm] = 0; break;        // clear local (int / float / string)
        case 38: { const tgt = pop(); st.push(send(o, tgt, imm, r)); break; }   // FUN_00370b40
        case 52: r = [0, 0, 0, 0]; break;                      // clear regs
        default:
          warnOnce(`enemy_ai: script ${o.script} ev ${ev}: op ${op} not translated; event stopped`);
          return;
      }
    }
  }

  // FUN_00370b40: command imm to target (-2 = own object); < 0x100 -> object manager FUN_00545b20,
  // else the object's +0xc handler (generic FUN_0054cd70, e027 FUN_005661e8).
  function send(o, tgt, cmd, r) {
    if (cmd < 0x100) {
      if (cmd === 0x3a || cmd === 0x3f) return r[0] + ID_BASE; // object id of section-1 record r0 (manager +0x3b2)
      if (cmd === 0x3d) return trigObj(r[0]);                  // object id of section-8 trigger r0 (manager +0x7b0)
      if (cmd === 0xc) return EVFLAGS[r[0]] ?? 0;              // event flag r0 (FUN_003ff738)
      if (cmd === 0xa6) return +(DIFF === 2);                  // FUN_00242c28() == 2
      if (cmd === 0xa7) return +(DIFF === 3);                  // FUN_00242c28() == 3
      if (cmd === 0x2f) { if ((r[0] >>> 0) < 0x100) mshort[r[0]] = r[1]; return 0; }   // FUN_00545b20 case 0x2f
      if (cmd === 0x30) return (r[0] >>> 0) < 0x100 ? mshort[r[0]] : 0;                 // case 0x30
      // GUESS: other manager commands are skipped (return 0, the script goes on)
      warnOnce(`enemy_ai: manager cmd 0x${cmd.toString(16)} not translated (skipped)`);
      return 0;
    }
    const t = tgt === -2 ? o : objects.get(tgt);
    if (!t) return 0;
    switch (cmd) {
      case 0x101:                                              // FUN_0054cd70: queue message r1 to object r0
        if (r[1] >= 0 && objects.get(r[0]) && !(objects.get(r[0]).flags28 & 0x80000))
          mailbox.push({ to: r[0], n: r[1], from: t.id });
        return 0;
      case 0x108:                                              // FUN_0054cd70: +0x144(1, 0) (actors FUN_00559748)
        // then manager event 7 (FUN_0053fbe8) if active: not translated (its consumer is not decoded)
        setActive(t, 1);
        return 0;
      case 0x109: case 0x130:                                  // +0x144(0, 0); manager event 8 not translated
        setActive(t, 0);
        return 0;
      case 0x104: if (r[0] < 8) { t.vars[r[0]] = r[1]; t.flags28 |= 0x4000; } return 0;
      case 0x105: return t.vars[r[0]];
      case 0x143: if (t.kind === 'actor') cmd143(t, r); return 0;
      case 0x193: if (t.kind === 'actor' && t.ai) setTarget(t.ai, P); return 0;   // FUN_005661e8 0x193 (FUN_0053bb08 = the player)
      // 0x113 (FUN_0054cd70): object +0x36 = r0 and a delayed message to itself through
      // FUN_0053e9e0, whose delivery is not decoded. GUESS: it and other commands are skipped
      // (return 0, the script goes on).
      default:
        warnOnce(`enemy_ai: object cmd 0x${cmd.toString(16)} not translated (skipped)`);
        return 0;
    }
  }

  // vtable +0x144(on, 0). Triggers: generic activate (L: the trigger class's +0x144 is not read;
  // it gates the volume test). Records / actors: FUN_00559748 with param_3 = 0 (no event 3).
  function setActive(t, on) {
    if (t.kind === 'trigger') { t.active = !!on; events.push(`${on ? 'arm' : 'disarm'} ${t.id}`); return; }
    if (t.kind !== 'record' && t.kind !== 'actor') return;
    if (on === (t.flags28 & 1)) return;                      // FUN_00559748: already in that state
    const rec = data.records[t.rec];
    if (on) {
      if (dead(t)) return;                                   // FUN_00318bf8 status dead (FUN_00316cf8): stays off
      // AI mode: status +0xa0 (the placement) +8 (FUN_00365c50). L: the status +0x61 bit-0 path
      // (mode from the parameter set) is not taken. Mode 8 -> 0, then FUN_0035f2e0 (not translated).
      let mode = rec.mode ?? 0;
      if (mode === 8) { mode = 0; warnOnce('enemy_ai: activation mode 8 (FUN_0035f2e0) not translated'); }
      let a = null;
      if (rec.model === 'e027') {
        // L: FUN_0055b8e0(a, 2, 1, 0) = actor state 2 for a plain placement (+0x28 bits 2 / 5 / 1 and
        // +0x118 bit 16 clear, C+0x664 <= 0x29; those bits are not exported)
        a = addActor({ rec: t.rec, mode, astate: 2 });
      } else {
        // other models: no body / AI export yet (notes/enemy-activation.md §4.3); marked active only
        t.flags28 |= 1;
        warnOnce(`enemy_ai: record ${t.rec} (${rec.model}) activated without an AI (only e027 is translated)`);
      }
      events.push(`activate rec ${t.rec}`);
      activated.push(t.rec);
      onActivate(rec, a);
      return;
    }
    // Off: FUN_00559748 (param_2 = 0) -> FUN_00558bb8: when the AI index (+0x140) is >= 0,
    // FUN_003659d8(DAT_0046f4ec, +0x140) unlinks the AI from the brain list (S.q) and calls its
    // destructor (vtable 0x4a2358 +0xc = FUN_0035e6f8), whose first call is FUN_00360820 = stopFire
    // (the fire timer leaves the list DAT_0046f4e4 +0x2c); then +0x184 = 0, +0x140 = 0xff.
    // Reactivation builds a new AI (FUN_00365c50 in the on path), so nothing of the old one stays.
    // GUESS: the rest (body / weapon release, FUN_00588330, flags28 and HP kept) is not translated;
    // notes/fidelity-audit.md 'Actor deactivation'.
    t.flags28 &= ~1;
    if (t.kind === 'actor') {
      const i = actors.indexOf(t); if (i >= 0) actors.splice(i, 1);
      if (t.ai) {
        const j = S.q.indexOf(t.ai); if (j >= 0) S.q.splice(j, 1);           // FUN_003659d8 unlink
        stopFire(t.ai);                                                     // FUN_0035e6f8 -> FUN_00360820
      }
      objects.set(t.id, { kind: 'record', id: t.id, rec: t.rec, script: t.script, vars: t.vars, flags28: 0, ai: null, hp: t.hp });
    }
    events.push(`deactivate rec ${t.rec}`);
    onDeactivate(rec);
  }

  // FUN_005661e8 case 0x143: AI mode r0
  function cmd143(a, r) {
    if (!a.ai) return;
    const deadNoFlag = a.hp === 0;          // FUN_00566198 (status dead, FUN_00315738(s,1) GUESS false)
    if (deadNoFlag && r[1] === 0) return;
    if (!target(a.ai)) {
      // FUN_005632f0 / FUN_00563318 not decoded (GUESS false) -> FUN_0035fe08: candidate
      // FUN_005608e8 = the player (GUESS: the candidate search is not translated; Vincent is the only hostile)
      setTarget(a.ai, P);
    }
    if (r[0] === 8) warnOnce('enemy_ai: mode 8 (FUN_0035f2e0) not translated');
    else setMode(a.ai, r[0]);
  }

  // ------------------------------------------------ AI helpers
  const now = () => S.now;
  const target = ai => (ai.owner.f118 & 2) ? ai.owner.target : null;          // FUN_0035eda0 / FUN_00563200
  const dead = o => o.hp === 0;                                               // FUN_00316cf8
  const deadFinal = o => o.hp === 0 && !(o.s64 & 8) && !(o.s64 & 4);          // FUN_00316d08
  const busy = a => (a.C.f564 >>> 15) & 1;                                    // FUN_0055ab08: C+0x565 bit 7
  const attacking = a => a.astate >= 0x80 && a.astate <= 0xbf;                // FUN_0055bb40

  function setTarget(ai, t) {                                                 // FUN_0035fc98
    const prev = target(ai);
    // FUN_00562f50 -> FUN_00342608(C, target): C+0x6c2 = target id and C+0x564 bit 30 (the body
    // then aims at the target every frame, FUN_00342650 in bodyTrack); none: 0xffff, bit 30 clear
    if (t) { ai.owner.f118 |= 3; ai.owner.target = t; ai.owner.C.f564 |= 0x40000000; }
    else { ai.owner.f118 &= ~2; ai.owner.target = null; ai.owner.C.f564 &= ~0x40000000; }
    distances(ai, t !== prev);
    return !!t;
  }
  function eye(o) {                                                           // FUN_005634e0(o, 3): (0, h-2, 0) in o's frame, up = -y
    return [o.pos[0], o.pos[1] - (EYE_H - 2.0), o.pos[2]];
  }
  const targetPoint = t => [t.pos[0], t.pos[1] - PLAYER_AIM_H, t.pos[2]];   // player vt+0x5c (GUESS, see PLAYER_AIM_H)
  function aimPoint(a) {                                                      // FUN_00360558 -> FUN_00563438 / FUN_00562ff8
    const t = target(a.ai);
    if (t) a.aim120 = targetPoint(t);                                         // target vt+0x5c
    else if (!(a.f118 & 1)) a.aim120 = [a.pos[0] + a.face[0] * 1000, a.pos[1] + a.face[1] * 1000, a.pos[2] + a.face[2] * 1000];
    if (t) setAim(a, a.aim120);                                               // FUN_00562ff8 -> FUN_003424c0(C, +0x120)
    return a.aim120;
  }
  function distances(ai, retarget) {                                          // FUN_0035e830
    const t = target(ai);
    if (!t) { ai.d34 = ai.d38 = ai.d3c = 10.0; ai.d40 = 0; return; }
    const v = sub3(aimPoint(ai.owner), ai.owner.pos);
    ai.d34 = Math.fround(len3(v));
    const rr = R_ACTOR + R_ACTOR;                                             // actor +0x2c + target +0x2c
    ai.d38 = ai.d34 - rr;
    ai.d40 = v[1];
    ai.d3c = Math.hypot(v[0], v[2]) - rr;
    // GUESS: +0x7e / +0x94 / +0x80 last-seen bookkeeping (FUN_00360bb8) not translated
  }
  function rayBlocked(a, b) {                                                 // FUN_00360378
    S.rays++; S.total++;
    return len3(sub3(a, b)) <= 1000.0 ? !!los(a, b) : false;
  }
  function visible(ai, t) {                                                   // FUN_003600e0(ai, t, 3)
    if (!t) return false;
    return !rayBlocked(eye(ai.owner), eye(t));
  }
  function inRange(ai) {                                                      // FUN_003602e8(ai, 3, 0)
    return !!target(ai) && !!ai.W && ai.d34 < ai.W.range * 10.0 * 1.0;   // no W: not decoded (GUESS: out of range)
  }
  function perception(ai) {                                                   // FUN_0035ec10
    const skip = S.rays !== 0 && (S.rays > 2 || S.now - ai.perc < 0x3d) && S.now - ai.perc < 0x79;
    if (!skip) {
      const t = target(ai);
      if (!t) {
        if (ai.mode !== 0) {
          const c = P;                                                        // FUN_0035fd78 -> FUN_005608e8 (GUESS: the player)
          if (c && visible(ai, c)) { ai.seen = S.now; setTarget(ai, c); ai.flags |= 2; }
          else { ai.lost = S.now; ai.flags &= ~2; }
        }
      } else {
        const was = ai.flags & 2;
        ai.flags = visible(ai, t) ? ai.flags | 2 : ai.flags & ~2;            // FUN_00360290
        ai.flags = inRange(ai) ? ai.flags | 4 : ai.flags & ~4;
        if (!!was !== !!(ai.flags & 2)) { if (ai.flags & 2) ai.seen = S.now; else ai.lost = S.now; }
      }
      ai.perc = S.now;
    }
    distances(ai, false);
  }

  // ------------------------------------------------ actor state FUN_0055b8e0 + motion requests
  function setActorState(a, st) {
    if (st >= 0x100) st = 2;
    if (a.astate === st) return 0;
    a.astate = st;
    if (st === 3) request(a, 0x40);              // GUESS: state 3 -> motion 0x40 (r_st_a), see MOTION
    return 1;
  }
  function request(a, m) {                       // FUN_00345818 (clip change only; blend flags 0x82 / 0x83 not modelled)
    const name = MOTION[m];
    if (name && a.C.clip !== name) a.C.clip = name;
    events.push(`clip ${a.id} ${name}`);
  }

  // ------------------------------------------------ brain / layers
  // FUN_0035f268 -> FUN_0035f300(mode): new brain object. Mode 2 = class 0x4a2608 (FUN_00362e58);
  // mode 0 / other = class 0x4a2430 (update FUN_003614b0 is empty).
  function setMode(ai, mode) {
    ai.brain = { mode };
    if (mode !== 0 && mode !== 2) warnOnce(`enemy_ai: brain mode ${mode} not translated (empty update)`);
    ai.mode = mode;
    ai.flags &= ~0x100;
    if (ai.owner) {
      if (mode === 2) ai.interval = 5;           // start FUN_00362ee8
      else if (mode === 0) { setActionLayer(ai, 0); setCombatLayer(ai, 0); }   // start FUN_00361440
      ai.flags |= 0x100;
    }
    ai.bstate = 0;
    ai.btime = short(S.now);                     // FUN_0035eb50
    events.push(`mode ${ai.owner.id} ${mode}`);
  }
  function setActionLayer(ai, m) { ai.amode = m; ai.atime = short(S.now); }   // FUN_0035f540 (mode 0: no layer)
  function setCombatLayer(ai, m) {                                            // FUN_0035f9a0 -> FUN_0035fa10
    if (m === ai.cmode && ai.combat) return;
    if (m === 0) stopFire(ai);                                                // FUN_0035fa10 case 0
    ai.combat = m === 2 ? { mode: 2 } : null;                                 // GUESS: layer 1 (vt 0x4a23a0) not translated
    if (m === 1) warnOnce('enemy_ai: combat layer 1 not translated');
    ai.cmode = ai.combat ? m : 0;
    ai.cstate = 0;
    ai.ctime = short(S.now);                                                  // FUN_0035ebc0
    if (ai.combat) stopFire(ai);                                              // start FUN_00360e30
  }
  const bTime = ai => short(S.now - (ai.btime & 0xffff));                     // FUN_0035eb30
  const cTime = ai => short(S.now - (ai.ctime & 0xffff));                     // FUN_0035eba0

  function brain2(ai) {                                                       // FUN_00362ef8
    const a = ai.owner;
    if (ai.bstate !== 1 && busy(a)) ai.bstate = 1;
    if (attacking(a)) return;
    if (!target(ai)) ai.bstate = 5;
    switch (ai.bstate) {
      case 0:
        setActorState(a, 3);
        // FUN_0055a828: stop (FUN_003533e0); standing, nothing to stop on the page
        setActionLayer(ai, 0);
        a.C.s214 = 6;                                                         // FUN_0034e960(C, 6)
        if (!target(ai)) { ai.btime = short(S.now); a.C.s214 = 6; ai.bstate = 2; return; }
        ai.bstate = 3;                                                        // C+0x808 bit 0 set for e027 (shooter, trace 0x872b)
        return;
      case 1: if (busy(a)) return; break;
      case 2: if (bTime(ai) < 0x1f) return; ai.btime = short(S.now); break;
      case 3: setCombatLayer(ai, 2); ai.bstate = 4; ai.btime = short(S.now); return;
      case 5:
        if (!(ai.flags & 0x40)) setActorState(a, 3);
        else warnOnce('enemy_ai: brain 5 with AI bit 0x40 (motions 0xb6 / 2) not translated');
        a.C.s214 = 6; ai.bstate = 6;
      // fall through
      case 6: if (!target(ai)) return; break;
      case 7: case 8:
        warnOnce('enemy_ai: brain states 7/8 (non-shooter) not translated');      // not e027
        return;
      default: return;
    }
    ai.bstate = 0;
  }

  function combat2(ai) {                                                      // FUN_00360e50
    const a = ai.owner;
    if (busy(a)) return;
    const t = target(ai);
    const ts = t;                                                             // target status (FUN_0055a370)
    switch (ai.cstate) {
      case 0: {
        if (magEmpty(ai)) { reload(ai); return; }
        if (!t) { stopFire(ai); ai.cstate = 1; return; }
        if (!ts || !dead(ts)) {
          if ((ai.flags & 2) && (ai.flags & 4)) {                             // FUN_003601d8
            if (!ai.timer) startFire(ai, -1);
            ai.ctime = short(S.now);
            return;
          }
          stopFire(ai);
          // FUN_00360bb8(ai, 1) not decoded (GUESS: false)
          if (cTime(ai) < 0x12d) return;
          ai.cstate = 1; return;
        }
        stopFire(ai);
        ai.cstate = deadFinal(ts) ? 1 : 2;
        return;
      }
      case 1:
        if (cTime(ai) < 0x3d) return;
        setTarget(ai, P);                                                     // FUN_0035fe08 (GUESS candidate = player)
        break;
      case 2:
        if (ts && !dead(ts)) { ai.ctime = short(S.now); ai.cstate = 3; return; }
        if (t) return;
        ai.ctime = short(S.now); ai.cstate = 1; return;
      case 3:
        if (cTime(ai) < 0x1f) return;
        break;
      default: return;
    }
    ai.ctime = short(S.now);
    ai.cstate = 0;
  }

  const magEmpty = ai => ai.W && ai.rounds === 0;                             // FUN_003604f0
  function refill(ai, extra) {                                                // FUN_00360418
    if (!ai.W) return;
    ai.rounds = ai.W.mag;
    if (extra) ai.rounds += Math.trunc(Math.fround(((ai.W.mag >> 1) + 1) * rng()));
  }
  function reload(ai) {                                                       // FUN_00360718
    // FUN_005612e8 reload motion (C motion 0x1c / 0x1d): GUESS not translated (no reload clip exported)
    events.push(`reload ${ai.owner.id}`);
    refill(ai, 0);
  }

  // ------------------------------------------------ fire timer list (scheduler +0x2c)
  function timerInsert(e) {                                                   // FUN_00365520: before the first entry with due >= e.due
    let i = 0;
    while (i < S.timers.length && e.due - S.timers[i].due > 0) i++;
    S.timers.splice(i, 0, e);
  }
  function startFire(ai, count) {                                             // FUN_003607a8 -> FUN_00365308
    if (ai.timer || !ai.W) return;
    const e = { due: S.now, ai, left: count, period: ai.W.interval };
    ai.timer = e;
    timerInsert(e);
    // period < 15: FUN_00565ff0(actor, 1) aim pose (not for e027: 180)
    events.push(`timer ${ai.owner.id}`);
  }
  function stopFire(ai) {                                                     // FUN_00360820
    if (!ai.timer) return;
    S.timers.splice(S.timers.indexOf(ai.timer), 1);                           // FUN_00365478
    ai.timer = null;
  }
  function fireTimers() {                                                     // FUN_003656a0
    let n = 0;
    for (const e of S.timers.slice()) {
      if (!(e.due - S.now < 1)) continue;
      if (!S.timers.includes(e)) continue;
      const ai = e.ai;
      let rel = 0;
      if (!magEmpty(ai)) {
        if (ai.flags & 0x10) rel = ai.W.reload;
        else shot(ai);
      } else rel = ai.W.reload;
      S.timers.splice(S.timers.indexOf(e), 1);
      if (e.left > 0) e.left--;
      if (e.left === 0) ai.timer = null;
      else {
        if (rel < 1) {
          e.due = S.now + e.period + n;
          if (JITTER_ON) {
            if (e.period > 12) e.due += Math.trunc(Math.fround(e.period * Math.fround(rng() - 0.5)));
            else {
              const r24 = Math.fround(rng() * 24.0);
              if (r24 < 1.0) e.due += e.period * 2;
              else if (r24 < 2.0) e.due += e.period;
            }
          }
        } else e.due = S.now + rel + n;
        timerInsert(e);
      }
      n++;
      if (n > 1) return;
    }
  }

  // FUN_003605d8: one shot request
  function shot(ai) {
    const a = ai.owner;
    if (ai.rounds === 0) return;
    if (dead(a)) return;                                                      // owner status (AI +0xf4)
    const aim = aimPoint(a).slice();
    const C = a.C;
    if (!(a.f118 & 0x800)) {
      if (C.f564 & 0x4000) {
        if (C.f564 & 0x200000) spawnShot(a, aim);
      } else if (C.speed === 0) {
        if (C.s214 !== 6) C.s214 = 6;
        standShot(a, aim);                                                    // FUN_00341b78
      } else spawnShot(a, aim);
    } else warnOnce('enemy_ai: actor +0x118 bit 0x800 shot (FUN_0055b878) not translated');
    S.shots++; S.total++;
    if (ai.rounds >= 0) ai.rounds = short(ai.rounds - 1);
    events.push(`round ${a.id} ${ai.rounds}`);
  }

  // FUN_003424c0: C+0x670 = aim; while C+0x214 = 6 also the turn target C+0x2b0 = aim + row 0 x
  // stance offset (only with C+0x564 bit 0; e027 0x40000400 / 0x204000 in k_enemy_engage8: offset 0),
  // through FUN_0034b5e0 = the inverse of the body's parent frame (vt+0x2c). The soldiers stand on
  // the static room, so that frame is taken as the world (L: the turn fits the traces with it).
  function setAim(a, aim) {
    const C = a.C;
    C.aim = aim.slice();
    if (C.s214 !== 6) return;
    if (C.f564 & 1) warnOnce('enemy_ai: C+0x564 bit 0 stance offset (C+0x664, DAT_0046f0e8..f4) not translated');
    C.t2b0 = aim.slice();
  }

  // FUN_00341b78: aim point to C+0x670, then motion state (current +0x74) or 0xe
  function standShot(a, aim) {
    setAim(a, aim);
    const m = a.C.motion;
    if (!m) a.C.motion = { cls: 0xe, phase: 0, timer: 0, done: 0 };
    else if (m.next74) a.C.motion = { cls: m.next74, phase: 0, timer: 0, done: 0 };
  }

  // body update: motion state class 0xe, FUN_00596460 (called with the frame step)
  function bodyUpdate(a, step) {
    const m = a.C.motion;
    if (!m) return;
    if (m.cls === 6) {
      // GUESS: motion state 6 (what class 0xe returns into) is not decoded: on its first update it
      // requests r_st_a and stays; its +0x74 = 0xe so the next shot starts class 0xe again.
      // This request alone puts r_st_a at f788 (two frames after r_st_f, trace k_enemy_engage8).
      if (!m.started) { m.started = true; request(a, 0x40); }
      return;
    }
    if (m.cls !== 0xe) return;
    if (m.phase === 2) {
      // The game: when the motion ends (FUN_003460d8) or timer > 30, request motions 0x40-0x42
      // (r_st_a), set the done bit, return 6. GUESS: FUN_003460d8 (FUN_005a9860 -> FUN_005aa200)
      // is not decoded (two attempts) and no clip frame is kept here, so phase 2 ends on its
      // first update without the request (a request here would put r_st_a at f787, one frame early).
      m.done |= 1;
      a.C.motion = { cls: 6, next74: 0xe };                                   // returns 6 (GUESS: return value = next motion state)
      return;
    }
    if (m.phase === 0) {
      // C+0x300 / FUN_00342308 -> FUN_003533e0 stop: standing, nothing to stop
      m.phase = 1;
    }
    if (m.phase === 1) {
      // muzzle FUN_00341ec8 (marker 15): GUESS = eye point (no skeleton markers in this module)
      spawnShot(a, a.C.aim, eye(a));                                          // FUN_00341988 -> slot 0x56a11c -> FUN_00569340
      request(a, 0x57);
      m.timer = 0;
      m.phase = 2;
    }
    m.timer = f(m.timer + step);
  }

  // FUN_00569340 -> FUN_005694f8 -> FUN_005409a8(mgr, muzzle, aim, W, status, 0, 0) -> FUN_00554568
  // -> FUN_00552dd8(bullet, 0, ...): one bullet from the muzzle along (aim - muzzle).
  function spawnShot(a, aim, muzzle) {
    const W = a.ai.W;
    const start = (muzzle || eye(a)).slice();                                 // +0xc0, +0xe0
    const v = sub3(aim, start), d = len3(v);
    const dir = d > 0 ? v.map(x => f(x / d)) : [0, 0, 0];                     // +0x80 (vrsqrt; d = 0 not handled by the game)
    // +0x15c range = W+0x18 x 10 + DAT_0046ed6c (15). GUESS: FUN_00321e20(W) (then +20) taken as false.
    const range = f(f(W.range * 10.0) + 15.0);
    // +0xd0 end: W+4 bit 0x400000 -> the aim point, else start + dir x range; +0x130 = |end - start|
    const end = (W.flags & 0x400000) ? aim.slice() : start.map((x, k) => f(x + f(dir[k] * range)));
    const b = {
      from: a, start, dir, end, endDist: f(len3(sub3(start, end))), range,
      speed: f(W.speed * f(0.16666667)),                                      // +0x12c (bullet flag 2 clear)
      age: 0, pos: start.slice(), prev: start.slice(), f120: 0,
      delay: 0 + 2,                                                           // +0x142 = (FUN_005409a8 arg 7 = 0) + 2
      damage: W.damage,                                                       // +0x134 = (short)W+0x40
      kind: (W.flags & 0x80000) && JITTER_ON ? 2 : 0,                        // FUN_00554738 (flag 2 clear; FUN_00326500)
      target: target(a.ai),
    };
    bullets.push(b);
    events.push(`shot ${a.id}`);
  }
  // FUN_005549f0: one frame of a bullet. Returns 1 when it ends this frame (hit or out of range).
  function bulletUpdate(b, step) {
    if (b.delay > 0) {                                                        // +0x142 countdown
      const was = b.delay;
      b.delay--;
      if (was === 1) events.push('bullet start');                             // FUN_00554970: script event 0x4e, FUN_00553aa8 (no state here)
      return 0;
    }
    b.age = f(b.age + step);                                                  // +0x124 += DAT_004eeb24 (the frame step)
    // FUN_00556068 (motion type 0): prev = pos; pos = start + dir x (age x speed)
    b.prev = b.pos.slice();
    const t = f(b.age * b.speed);
    const p = b.start.map((x, k) => f(x + f(b.dir[k] * t)));
    const dist = f(len3(sub3(p, b.start)));                                   // +0x158
    let done = 0;
    if (b.endDist < dist) { b.pos = b.end.slice(); b.f120 |= 0x800000; done = 1; }
    else if (b.range < dist) { b.f120 |= 0x800000; done = 1; }                // +0x90 not moved in this branch
    else b.pos = p;
    if (!done && !(b.f120 & 0x2000000) && !(b.speed > 0 && b.age <= 1800.0 && dist <= 3000.0)) { b.f120 |= 0x800000; done = 1; }
    // FUN_00556870 -> FUN_005442d8: objects along prev -> pos (closest hit, vt+0x1b4), then the
    // room (probe 0). GUESS: only the player is tested: hit when the segment passes within
    // BULLET_R of his aim point (his part test vt+0x17c, the other actors' filter vt+0x1ac and the
    // room probe are not decoded; #66 shows no hit or near miss from #67's f875 bullet, so the
    // filter skips soldiers: L). Off unless opts.damage.
    if (DAMAGE && b.target === P && segPointDist(b.prev, b.pos, playerAim()) <= BULLET_R) {
      damagePlayer(b);
      return 1;
    }
    return done;
  }
  const playerAim = () => [P.pos[0], P.pos[1] - PLAYER_AIM_H, P.pos[2]];       // vt+0x5c (GUESS, see PLAYER_AIM_H)

  // FUN_0055db50 -> FUN_0055d2c0 -> vt+0x1ec FUN_0055c7f8 on the player
  function damagePlayer(b) {
    let dmg = defence(PST, b.damage, b.kind);                                 // FUN_0055d2c0: FUN_00318120
    dmg = Math.min(dmg, 9999);                                                // (no zone record: +0x19c 0)
    if ((P.f118 & 0x20) || (P.f118 & 0x40)) dmg = 0;                          // FUN_0055db50
    if ((P.f118 | 0) < 0 && P.hp <= dmg) dmg = P.hp - 1;                      // FUN_0055c7f8
    const before = P.hp;
    P.hp = Math.max(0, P.hp - dmg);                                           // FUN_003168c8 (clamp at 0: L)
    events.push(`hit player ${before}->${P.hp}`);
    log(`enemy_ai: Vincent HP ${before} -> ${P.hp} (shot by #${b.from.id})`);
    // Not translated: the hit reaction vt+0x324 FUN_00576ac8, and at HP 0 the death vt+0x334
    // FUN_00576b48: HP stays at 0 and nothing else happens (the page does not handle it).
  }

  // ------------------------------------------------ AI run FUN_00365110 and scheduler FUN_00365008
  function queueInsert(ai) {                                                  // FUN_00365ad0
    let i = 0;
    while (i < S.q.length && ai.due - S.q[i].due > 0) i++;
    S.q.splice(i, 0, ai);
  }
  function runAI(ai) {
    perception(ai);
    if (ai.brain && !(ai.flags & 0x10)) {                                     // owner vt+0x26c (actor+0x180) assumed set
      if (ai.brain.mode === 2) brain2(ai);                                    // vt+0x1c
      if (ai.combat) combat2(ai);                                             // action layer (+0xe4): none in mode 2
    }
    ai.last = S.now;
    ai.due = S.now + ai.interval;
    if (!(ai.flags & 1)) ai.due += S.runs;
    if (S.q.length > 1) { S.q.splice(S.q.indexOf(ai), 1); queueInsert(ai); }
    S.runs++;
  }
  function scheduler(step) {
    // FUN_00325b40 (AI paused) / manager +0x2b0 bit 12: GUESS not paused on this page
    const f = Math.fround(S.acc + step);
    S.acc = f;
    if (!(1.0 <= f)) return;
    S.now += Math.trunc(f);
    S.rays = S.shots = S.runs = S.total = 0;
    S.acc = Math.fround(f - Math.trunc(f));
    if (S.q.length > 0) {
      while (S.q[0].due - S.now < 1) runAI(S.q[0]);
      if (S.runs === 0 && S.q[0]) runAI(S.q[0]);
      fireTimers();
    }
  }

  // ------------------------------------------------ trigger volumes FUN_005517a8 (shapes 0 / 3 / 8)
  function triggerCheck(o, pNew, pOld) {
    const t = o.t;
    if (!o.active) return;                                                    // FUN_00550ad8 / cmd 0x108 (see setActive)
    if (t.shape !== 0 && t.shape !== 3 && t.shape !== 8) return;              // shape 4 (FUN_003114d0) not translated
    if ((t.flags >> 19) & 1) return;                                          // GUESS: bit-19 swept branch not translated
    const lNew = xform(t.inv, pNew), lOld = xform(t.inv, pOld);               // FUN_005507a0
    const inNew = inBox(lNew, t.lo, t.hi), inOld = inBox(lOld, t.lo, t.hi);
    if (inNew) {
      if (!inOld && (t.flags & 1)) runEvent(o, 0xd, [segBox(lOld, lNew, t.lo, t.hi), 0]);
      return;
    }
    if (inOld && (t.flags & 2)) runEvent(o, 0xe, [segBox(lNew, lOld, t.lo, t.hi), 0]);
    if (inOld) return;
    const h = segBox(lOld, lNew, t.lo, t.hi);
    if (h !== -1) {
      runEvent(o, 0xd, [h, 0]);
      runEvent(o, 0xe, [segBox(lNew, lOld, t.lo, t.hi), 0]);
    }
  }

  // ------------------------------------------------ one game frame
  function tick(step, playerOld, playerNew) {
    events.length = 0;
    activated.length = 0;
    // 1. AI
    scheduler(step);
    // 2. bullets (oldest first)
    for (const b of bullets.slice()) if (bulletUpdate(b, step)) bullets.splice(bullets.indexOf(b), 1);
    // 3. bodies: turn (FUN_0034ea18 -> FUN_0034de50), then the motion state (may spawn bullets).
    // GUESS: turn before the motion state (their order inside the body update is not traced; the turn
    // reads only C+0x2b0, which neither changes)
    for (const a of actors) {
      // GUESS: a dead soldier stops turning. In the game the turn runs in FUN_0034ea18 until C+0x214 is 0;
      // the death path (FUN_0055cac8 -> FUN_0035f728) isn't seen to clear it, so he may keep turning
      // toward his last aim point (notes/fidelity-audit.md 'Dead soldier stops turning').
      if (dead(a)) { bodyUpdate(a, step); continue; }
      // FUN_00343160 (motion update): C+0x564 bit 30 -> FUN_00342650: the C+0x6c2 object's vt+0x5c
      // point -> FUN_003424c0 (no parent-frame transform: vt+0x34 = no parent on the static room, L)
      // T: the point is where Vincent was before this frame's move (record f's position fits the
      // game's turn, f+1's does not: k_enemy_engage7 #66 open loop median 0.9 vs 1.0 deg, max 5.5 vs 11)
      if ((a.C.f564 & 0x40000000) && a.target)
        setAim(a, targetPoint(a.target === P && playerOld ? { pos: playerOld } : a.target));
      if (a.C.s214 === 6) a.face = turnBody(a.face, a.pos, a.C.t2b0, step);
      bodyUpdate(a, step);
    }
    // 4. messages posted last frame -> event 2 of the receiver (r0 = n, r2 = sender)
    const mail = mailbox.splice(0);
    for (const m of mail) { const o = objects.get(m.to); if (o) runEvent(o, 2, [m.n, 0, m.from, 0]); }
    // 5. trigger volumes (the player's move this frame)
    if (playerOld && playerNew) for (const o of triggers) triggerCheck(o, playerNew, playerOld);
    return events.slice();
  }

  return { S, actors, triggers, bullets, objects, addActor, tick, runEvent, recById, events, activated, setActive };
}
