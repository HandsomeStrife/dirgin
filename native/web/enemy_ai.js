// Enemy AI for z128's e027 soldiers: wake by trigger volume + event script, and the awake
// combat AI (brain mode 2, combat layer 2, fire timer, shot, bullet, damage to Vincent), and
// Vincent's bullets hitting them (pellets, spread, part boxes, damage, flinch / death; §13).
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

import { cylinderHit, partTest, cone } from './enemy_hit.js';

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
export const PLAYER_STATUS = { lvl: 4, armour: null, f60: 0x8233, f27: 0, f64: 0, u20top: 0, w68: 0, b317630: 0, elem9c: 0,
                                b10: 2, f118: 0 };   // status +0x10 (FUN_005405b0's level term; 4.8 / 1.028 = the k_hits12 cone angle), actor +0x118
// GUESS (save RAM): G = *DAT_0046dd68: G+0x60 story progress 20, G+0x14 table +0x270 level factors,
// +0xaa / +0xae / +0xb2 difficulty cuts; DAT_004b8ac8 option bits 0, DAT_0046b328 1, DAT_0046b2fc 2.
// ELF data: DAT_0046e220 {0, 50} (level factors before progress 7), DAT_004a02b0 element factors.
export const GAME = { progress: 20, lvlK: [0.33, 0.4], lvlK0: [0, 50], cut: { aa: 0, ae: 20, b2: 100 },
                      bits4b8ac8: 0, d46b328: 1, d46b2fc: 2, elem: [1, 0, 0.25, 0.5, 2], armour: null,
                      // T = G+0x14: +0xd0 / +0xd4 spread distance classes, +0x268 the spread level factors (FUN_0031cf08)
                      spreadD: [120, 380], spreadK: [0.15, 2.5] };
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
// Vincent's bullets vs the soldiers (notes/enemy-research.md §13). Hit-zone records (status +0xa8
// -> +0x30 -> 16 B per part, FUN_0055bda8): byte 0 = damage scale (x 0.05, FUN_0055d2c0) and ushort
// +2 flags (bit 1 no reaction flags, bit 2 no stagger, bit 3 bullet +0x120 |= 0x60000; FUN_0055db50).
// From the stage file: ai.json zones[record.zones] (export_web_triggers.py zone_set: FUN_003153d0 /
// FUN_00310e40 / FUN_0030fc68).
// e027 status fields FUN_00318120 reads (§10.2, RAM of #67): armour +0xc0 none, +0x60 0, +0x27 0,
// +0x64 0, element 0; level +0x11 comes from ai.json (FUN_00314ea8).
export const E027_STATUS = { lvl: 0, armour: null, f60: 0, f27: 0, f64: 0, u20top: 0, w68: null, b317630: 0, elem9c: 0 };
// GUESS: C+0x1c0 of a standing soldier (trace k_enemy_engage12 #67: 0x10301; bit 0x200 = on the
// ground, read by FUN_00565480). The page has no enemy physics, so it stays.
const C1C0_STAND = 0x10301;
// GUESS: the motion classes' clips. Class 0x1e (flinch, FUN_00561718 stance 0) and the death motion
// (class 0x36 after FUN_0055b8e0(actor, 7)) pick a clip by hit side / part; that choice is not
// decoded. The traces show r_stlld0 (flinch, part 10), r_stlaf0 (death, part 6) and r_stbf0 (death,
// part 0 = head): head -> r_stbf0, any other part -> r_stlaf0.
const FLINCH_CLIP = 'r_stlld0';
const deathClip = part => part === 0 ? 'r_stbf0' : 'r_stlaf0';
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
  const S = { frame: 0, now: opts.now ?? 0, acc: opts.acc ?? 0, rays: 0, shots: 0, runs: 0, total: 0, q: [], timers: [] };
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
  // Status HP (+0x20) = max HP from the placement setup (FUN_00315050 / FUN_003151c8 -> FUN_00314ea8:
  // ai.json hp; GUESS 100 for an old ai.json). It lives on the status, so it survives deactivation
  // and reactivation (FUN_00559748 does not reset it).
  data.records.forEach((rec, k) => {
    const id = k + ID_BASE;
    if (!objects.has(id)) objects.set(id, { kind: 'record', id, rec: k, script: rec.script, vars: new Int16Array(8), flags28: 0, ai: null, hp: deadRecs.has(k) ? 0 : rec.hp ?? 100 });
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
      // status +0x20 HP: the record object's (set at placement setup, kept across deactivation)
      astate: init.astate ?? 0, hp: init.hp ?? (prev && prev.kind === 'record' ? prev.hp : rec.hp ?? 100), s64: 0, f19c: null,
      lvl: rec.lvl ?? 0,                         // status +0x11 (FUN_00314ea8)
      stance: 0, f110: 0,                        // +0x100 bits 40-41 stance / +0x110 reaction nibbles (0 for z128's soldiers, §10.3)
      part: -1, f144: -0x7fffffff,               // +0x142 last part hit, +0x144 frame of the last flinch clip
      s148: -0x7fffffff, s150: 0, s152: 0, sphase: 0,   // stagger FUN_0055d950: +0x148 time, +0x150 / +0x152 sums, phase (+0x150 bits 40-43)
      // body C (+0x300 mover mode and its slide; C+0x1c0 move flags)
      C: { f564: init.f564 ?? 0, speed: 0, s214: 0, aim: null, t2b0: null, motion: null, clip: init.clip || 'r_gst',
           f1c0: C1C0_STAND, mover: 0, slide: null, hold: false },
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
    // HP stays on the status (FUN_00559748 does not write +0x20; the record object keeps it).
    // GUESS: the rest (body / weapon release, FUN_00588330, flags28 kept) is not translated;
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
    if (st === 7) {                              // the death motion (class 0x36); its clip: GUESS deathClip
      a.C.motion = { cls: 0x36, t: 0 };
      a.C.clip = deathClip(a.part); a.C.hold = true;
      events.push(`clip ${a.id} ${a.C.clip}`);
    }
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
    moverUpdate(a, step);                                                     // FUN_0034f8c0 (C+0x300 slides)
    const m = a.C.motion;
    if (!m) return;
    if (m.cls === 0x1e || m.cls === 0x36) {
      // the clip clock as enemies.js plays it (MP+0x40c 0.5 x step, FUN_005aa2a8). Death (0x36): at the
      // clip's end the motion-done event FUN_00343160 -> FUN_00539570 sets actor +0x118 bit 0x100
      // (corpse settled, notes §10.5) and the last frame holds. GUESS: the flinch (0x1e) ends with its
      // clip and requests motion 0x40 (r_st_a); class 0x1e's own update is not read.
      const n = data.clips && data.clips[a.C.clip];
      m.t = f(m.t + f(f(0.5) * step));
      if (n != null && n <= m.t) {
        if (m.cls === 0x36) { if (!m.done) { m.done = 1; a.f118 |= 0x100; events.push(`settled ${a.id}`); } }
        else { a.C.motion = null; request(a, 0x40); }
      }
      return;
    }
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
    if (b.from === P) return playerBulletHit(b) || done;                      // Vincent's bullet: the soldiers
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

  // ------------------------------------------------ Vincent's bullets (notes/enemy-research.md §13)
  // FUN_00577480 -> FUN_005408e8(mgr, 0, muzzle, aim, W, status, i, 0) for i < W+0x16 pellets ->
  // FUN_00552dd8(b, i): every pellet flies from the muzzle toward the aim, pellet i with i + 2 frames
  // of countdown (+0x142), so the pellets follow each other one frame apart on one line.
  // W = data/aim/weapon.json (export_web_aim.py: speed10 = W+0x2c, range10 = W+0x18, hit = W+0x4a).
  // GUESS: the pellet count is not cut by the rounds left (FUN_00319ee0; the page counts no ammo).
  // aim = P+0x2b0 = CAM+0xa0: with a lock, the target's lock point (lock_on.js via aim.js). FUN_00577480
  // adds a lead (aim and direction += C+0x410 x |aim - muzzle| / (W+0x2c x 10) x 60) only when P+8 (a
  // parent object, FUN_0054a720) is set: never on the page (trace k_lock13: P+8 = 0). The bullets get
  // +0x12a = the FUN_00329740 target's id; with no W+4 bit 0x2000 (Vincent's W+4 = 0x100) there is no
  // homing (FUN_00552dd8: +0x154 = W+0x3c only with that bit), and +0x12a only feeds FUN_00567ef0's
  // HUD popup at the bullet's end (FUN_00554ea8; not on the page).
  // chest = marker 1 (FUN_00342000; Vincent: bone 5, offset 0): FUN_00563148 first pulls the muzzle
  // back along the shot by 0.8 x |muzzle - chest| (unless actor +0x118 bit 0x1000). Then per pellet
  // FUN_005405b0 (W+4 without 0x100000 / 0x200000): s = W+0x1c / +0x20 / +0x24 by |aim - muzzle|
  // < T+0xd0 (120) / < T+0xd4 (380) / beyond, / FUN_0031cf08(1.0, status +0x10), FUN_00318518 (no
  // armour record: unchanged), x 0.5 for FUN_00317b68 (not for Vincent here); FUN_0025a058 tilts the
  // direction (two draws from the shared RNG); aim' = muzzle + dir x W+0x18 x 10 -> FUN_00552dd8.
  // GUESS: FUN_003251f8 (with FUN_00326500 and status +0x60 bit 1: the x {1, 0.33, 0.75, 0.1} factor)
  // taken as false: not read; the k_hits12 log shows s = 4.8 / 1.028 with no factor.
  const pendingShots = [];
  // the page's hook (play.js / aim.js onShot): the player steps before tick, so his shot waits for
  // tick's slot after the scheduler (see tick 1b)
  function queueShot(muzzle, aim, W, chest = null) { pendingShots.push([muzzle, aim, W, chest]); }
  function playerShot(muzzle, aim, W, chest = null) {
    const PS = opts.playerStatus || PLAYER_STATUS;
    let m0 = muzzle.slice();
    if (chest && !(PS.f118 & 0x1000)) {
      const u = unit3(sub3(aim, m0)), k = f(len3(sub3(m0, chest)) * f(-0.8));
      m0 = m0.map((x, i) => f(x + f(u[i] * k)));
    }
    if (!W.spread) warnOnce('enemy_ai: weapon.json has no spread (W+0x1c..0x24): rebuild data/aim; pellets fly straight');
    const range = f(f(W.range10 * 10.0) + 15.0);                              // +0x15c (FUN_00321e20 false, as above)
    for (let i = 0; i < W.pellets; i++) {
      const start = m0.slice();
      let aimP = aim.slice();
      if (W.spread && !(W.flags & 0x300000)) {                                // FUN_005405b0
        const v0 = sub3(aim, start), d0 = len3(v0);
        let s = d0 < GAME.spreadD[0] ? W.spread[0] : d0 < GAME.spreadD[1] ? W.spread[1] : W.spread[2];
        const kk = GAME.progress < 7 ? GAME.lvlK0 : GAME.spreadK;             // FUN_0031cf08 (G+0x60 >= 7)
        s = f(s * f(1.0 / f(f(f(f(kk[0] * PS.b10) + kk[1]) * f(0.01)) + 1.0)));
        if ((PS.f64 & 0x20) && PS.u20top === 2) s = f(s * 0.5);              // FUN_00317b68
        const d2 = cone(s, unit3(v0), rng);
        aimP = start.map((x, k) => f(x + f(d2[k] * f(W.range10 * 10.0))));
      }
      const v = sub3(aimP, start), d = len3(v);
      const dir = d > 0 ? v.map(x => f(x / d)) : [0, 0, 0];
      const end = (W.flags & 0x400000) ? aimP.slice() : start.map((x, k) => f(x + f(dir[k] * range)));
      bullets.push({
        from: P, start, dir, end, endDist: f(len3(sub3(start, end))), range,
        speed: f(W.speed10 * f(0.16666667)),                                  // +0x12c = W+0x2c / 6
        age: 0, pos: start.slice(), prev: start.slice(), f120: 0, f148: 0,
        delay: i + 2,                                                         // +0x142 = pellet index + 2
        damage: W.damage, hit: W.hit ?? 0,                                    // +0x134 (short)W+0x40, +0x153 W+0x4a
        kind: (W.flags & 0x80000) && JITTER_ON ? 2 : 0,                      // FUN_00554738
        shooter: P.id ?? 0x16,                                                // +0x128 owner id
      });
    }
    if (W.hit == null) warnOnce('enemy_ai: weapon.json has no hit value (W+0x4a): rebuild data/aim; stagger uses 0');
    events.push(`player shot ${W.pellets}`);
  }

  // FUN_005442d8 for Vincent's bullet: the active soldiers along prev -> pos, each through its vt+0x17c
  // = FUN_0055bda8(actor, 0, ...): the cylinder (FUN_0054b2b0: feet .. feet + up x actor+0x30, radius
  // actor+0x2c; e027 has no actor +0x28 bit 0x20, so not the extended form), then the part test
  // FUN_005ad118 on the same segment. No part (or a masked part, C+0x77c) -> FUN_0055df90 -> FUN_00360870
  // (ai, 0, 0): counted, no hit. A part -> actor +0x142 = part, +0x19c = zone record, a hit.
  // The closest hit (distance from prev to the cylinder point) gets vt+0x1b4 -> FUN_0055db50 and the
  // bullet ends (FUN_00556870 returns its result). GUESS: soldiers are not see-through (actor +0x28
  // bit 8 / bullet +0x120 bit 0x40 would list every hit instead; bit 8 is not exported); W flags 0x100
  // has no 0x40. L: the object filter vt+0x1ac lets Vincent's bullets reach every soldier; the room
  // probe after the objects (probe 0) is not done (bullets pass walls, as the soldiers' do).
  function playerBulletHit(b) {
    let best = null;
    for (const a of actors) {                                                 // active only (+0x28 bit 0: setActive keeps the list)
      // L: a dead soldier is not tested: AI +0x70 (counted on every hit and near miss) stops at the
      // death for both soldiers while the later pellets pass him (k_enemy_engage11a / 12). The gate
      // (filter vt+0x1ac or object +0x38 bit 0) is not read.
      if (dead(a)) continue;
      const boxes = data.hitboxes && data.hitboxes[recModel(a)];
      // MISSING: part boxes are exported for e027 only (export_web_triggers.py); other types can't be hit yet
      // (notes/fidelity-audit.md 'Player shots vs non-e027 enemies')
      if (!boxes) { warnOnce(`enemy_ai: no part boxes for ${recModel(a)} (only e027's are exported); not hit`); continue; }
      const A = a.pos, B = [a.pos[0], f(a.pos[1] - EYE_H), a.pos[2]];        // up row (0,-1,0) x actor+0x30
      const cyl = cylinderHit(R_ACTOR, b.prev, b.pos, A, B);
      if (!cyl) continue;
      const bw = world.boneWorld ? world.boneWorld(a) : null;                 // the MP's posed skeleton
      if (!bw) { warnOnce('enemy_ai: no pose for the part test (boneWorld unset); soldiers not hit'); continue; }
      const part = partTest(boxes, bw, b.prev, b.pos);
      if (part < 0) { aiHit(a, 0, 0); events.push(`near miss ${a.id}`); continue; }   // FUN_0055df90 (no part mask on the page: C+0x77c 0, trace pmask)
      const dist = len3(sub3(b.prev, cyl.point));
      if (!best || dist < best.dist) best = { a, part, point: cyl.point, dist };
    }
    if (!best) return 0;
    best.a.part = best.part;                                                  // +0x142
    best.a.f19c = zoneRec(best.a, best.part);                                 // +0x19c = zone record (null: status +0xa8 = 0)
    hitEnemy(best.a, b, best.point);
    return 1;
  }
  const recModel = a => data.records[a.rec] ? data.records[a.rec].model : 'e027';

  // The soldiers as the lock-on (lock_on.js lockFrame, FUN_00328968) sees them: the active actors.
  // point = vt+0x5c FUN_00563720: bone C+0x765's world position (C+0x765 = the zone header's byte
  // +0x14, FUN_00340760 at activation: ai.json lock_bones, 3 for z128's soldiers, trace k_lock13), else
  // FUN_005634e0(actor, 3) = actor +0x30 - 2 up the body; that point is also the line-of-sight end.
  // alive = HP != 0 (FUN_00316cf8), lockable = +0x38 bit 2 and alive (vt+0x204 FUN_0055ef38).
  // GUESS (traced values, setters not read): +0x38 = 0x387 alive (probes 0-2) and 0x183 once dead (bits
  // 2 and 0x200 cleared on the death frame; k_lock14 f836), so a corpse leaves probe 2 and can't be locked.
  const PROBE_BITS = 0x387, PROBE_BITS_DEAD = 0x183;
  function lockTargets() {
    const out = [];
    for (const a of actors) {
      if (!(a.flags28 & 1)) continue;                                         // FUN_005449f0: +0x28 bit 0
      const rec = data.records[a.rec];
      const lb = data.lock_bones && rec && rec.zones >= 0 ? data.lock_bones[rec.zones] : -1;
      const top = [a.pos[0], f(a.pos[1] - f(EYE_H - 2.0)), a.pos[2]];
      let point = top;
      if (lb != null && lb >= 0) {
        const bw = world.boneWorld ? world.boneWorld(a) : null;
        if (bw) point = bw(lb)[3].slice(0, 3);
        else warnOnce('enemy_ai: no pose for the lock point (boneWorld unset); lock-on aims at the body top');
      }
      const alive = !dead(a), bits = alive ? PROBE_BITS : PROBE_BITS_DEAD;
      out.push({ id: a.id, alive, lockable: alive && !!(bits & 4), probeBits: bits, point,
                 feet: a.pos.slice(), radius: R_ACTOR, height: EYE_H, losPoint: top });
    }
    return out;
  }
  // FUN_0055bda8: actor +0x19c = *(status +0xa8 + 0x30) + part x 16, or 0 without a zone set
  function zoneRec(a, part) {
    const rec = data.records[a.rec];
    if (!data.zones || !rec || rec.zones == null) { warnOnce('enemy_ai: ai.json has no hit zones (old export: rebuild data/enemies); damage unscaled'); return null; }
    const zs = rec.zones >= 0 ? data.zones[rec.zones] : null;
    return zs ? zs[part] ?? null : null;
  }

  // FUN_00360870(ai, shooter, dmg): AI +0x70 hits + 1, +0x4c = now; with a shooter FUN_003608d8
  // (AI +0x90 = the shooter; T: 0x160016 after Vincent's hit, k_enemy_engage11a)
  function aiHit(a, shooter, dmg) {
    const ai = a.ai;
    if (!ai) return;
    ai.hits = short(ai.hits + 1); ai.f4c = S.now;
    if (shooter) ai.f90 = shooter;
  }

  // FUN_0055d2c0: FUN_00318120 (defence) of the bullet's damage, then x zone byte x 0.05 unless bullet
  // +0x120 bit 0x800; cap 9999
  function enemyDamage(a, b) {
    // e027's status: only e027 actors are hit today (part boxes; see playerBulletHit)
    let d = defence({ ...E027_STATUS, lvl: a.lvl }, b.damage, b.kind);
    if (a.f19c && !(b.f120 & 0x800)) d = Math.trunc(f(f(f(d + 0.5) * a.f19c.scale) * f(0.05)));
    return Math.min(d, 9999);
  }

  // FUN_0055d950(actor, hit value): the stagger flags 0x42 / 0x44 / 0x48. Thresholds 1 / 30 / 60
  // (FUN_00310c40 = G+0x64 < 27 in this save; else status +0x9c -> +0x94..+0x98).
  // The window is FUN_0054cbe8 = DAT_0046e938 +0x20 - actor +0x148 < 60 (the counter the flinch
  // throttle reads; L: it counts frames, S.frame here).
  function stagger(a, hv) {
    if (S.frame - a.s148 < 60) { a.s150 += hv; a.s152 += hv; }
    else { a.s150 = hv; a.s152 = hv; a.sphase = 0; }
    a.s148 = S.frame;
    let fl = 0;
    if (a.s152 >= 1) { fl = 0x42; a.s152 = 0; if (a.sphase === 0) a.sphase = 1; }
    if (a.sphase === 1 && a.s150 >= 30) { fl = 0x44; a.sphase = 2; }
    else if (a.sphase === 2 && a.s150 >= 60) { fl = 0x48; a.s150 = 0; a.s152 = 0; a.sphase = 0; }
    return fl;
  }

  // FUN_0055db50(victim, bullet, hit point): Vincent's bullet on a soldier
  function hitEnemy(a, b, hitpos) {
    const shooter = b.shooter;
    const gate = a.s64 & 3;                                                   // FUN_0055f0e0(a, 1) / (a, 0): no damage
    let dmg = 0;
    if (!gate) {
      const wasDead = dead(a);
      dmg = enemyDamage(a, b);
      if (a.f118 & 0x60) dmg = 0;
      let dir = [0, 0, 0];
      if (!dead(a)) dir = unit3([b.dir[0], 0, b.dir[2]]);                     // FUN_0055d390: the bullet's +0x80, y = 0
      // vt+0x2f4 event 0x12 (r0 = owner id, r2 = owner +0x20); scripts 108 / 109 have none
      runEvent(a, 0x12, [shooter, 0, shooter, 0]);
      const zf = a.f19c ? a.f19c.flags : 0;
      let fl = 0;
      if (!a.f19c || !(zf & 4)) fl = stagger(a, b.hit);
      if (zf & 8) b.f120 |= 0x60000;
      if (wasDead) {
        if (!(a.f118 & 0x10000)) react(a, b.f148, dir, hitpos, 0);
      } else {
        enemyHP(a, dmg);                                                      // vt+0x1ec FUN_0055c7f8
        // the shooter's vt+0x314 / +0x31c (Vincent's kill / damage callbacks): not translated
        let flags = b.f148 | fl;
        if (zf & 2) flags = 0;
        if (!(a.f118 & 0x10000)) react(a, flags, dir, hitpos, 1);
      }
    }
    aiHit(a, shooter, dmg);                                                   // FUN_00360870
    // FUN_003361b8 (hit effect by the zone's nibble) and FUN_00567f10 (actor +0x28 bit 0x20 clear):
    // not on the page
    events.push(`hit ${a.id} part ${a.part} dmg ${dmg} hp ${a.hp}`);
  }

  // FUN_0055c7f8: HP -= dmg (cap 9999; +0x118 bit 31 leaves 1); alive -> vt+0x324 FUN_0055c978(dmg, 0):
  // script event 8 (r0 = dmg; cues 0x44 / 0x4c are sound); killed -> FUN_0055c978(dmg, 1) (vt+0x344
  // is empty for e027) and vt+0x334 FUN_0055cac8
  function enemyHP(a, dmg) {
    const was = dead(a);
    if (dmg > 9999) dmg = 9999;
    if ((a.f118 | 0) < 0 && a.hp <= dmg) dmg = a.hp - 1;
    const before = a.hp;
    a.hp = Math.max(0, a.hp - dmg);                                           // FUN_003168c8 (clamp at 0: L)
    log(`enemy_ai: #${a.id} HP ${before} -> ${a.hp}`);
    if (was) return;
    if (!dead(a)) runEvent(a, 8, [dmg, 0, 0, 0]);
    else enemyDeath(a);
  }

  // FUN_0055cac8: cue (sound), script event 0x17, FUN_005435a8 (manager: not decoded), the AI's
  // modes 0 (FUN_0035f728 = FUN_0035f268 / FUN_0035f540 / FUN_0035f9a0 with 0: the fire timer stops)
  // and the item drop. GUESS: the item drop (FUN_00316d70 / FUN_00541688) is left out (no items on the page).
  function enemyDeath(a) {
    runEvent(a, 0x17);
    if (a.ai) { setMode(a.ai, 0); setActionLayer(a.ai, 0); setCombatLayer(a.ai, 0); }
    events.push(`dead ${a.id}`);
  }

  // FUN_00565480(actor, flags, hitdir, hitpos, wasAlive), the branches z128's soldiers take (notes §10.3)
  function react(a, flags, dir, hitpos, wasAlive) {
    const C = a.C;
    // 1. C+0x1c8 / FUN_00356740 and the flag 8 -> 4 swap for actor +0x28 bit 0x20 or C+0x45c bit 17:
    // GUESS: not translated (z128's soldiers have neither bit; FUN_00356740 not read)
    if (!wasAlive) {
      if (C.motion && C.motion.cls === 0x36) warnOnce('enemy_ai: hit on a corpse in its death motion (FUN_005612a0) not translated');
      return;
    }
    if (a.f110 & 0xff00) warnOnce('enemy_ai: actor +0x110 reaction nibbles (FUN_00562670 / FUN_00562778) not translated');
    const isDead = dead(a);
    const grounded = C.f1c0 & 0x200;
    if (a.astate === 0xf) return;
    if ((flags & 8 && !(a.f118 & 4)) || (flags & 4 && !(a.f118 & 0x10))) {
      // GUESS: the knock-down / big reactions (FUN_005618d0, FUN_00562898, FUN_00561270, FUN_00561138)
      // are not translated: alive -> no reaction; dead -> the small-hit death below
      warnOnce('enemy_ai: big / knock-down hit reaction (flags 4 / 8) not translated');
      if (isDead) die(a, dir);
      return;
    }
    if (flags & 2) {
      if (a.f118 & 8) return;
      if (!grounded) { warnOnce('enemy_ai: airborne hit reaction (FUN_00561270 / FUN_005618d0) not translated'); if (isDead) die(a, dir); return; }
      if (isDead) { die(a, dir); return; }
      if ((a.f118 & 0x100000) && !(flags & 0x40)) return;
      const away = unit3(sub3(a.pos, hitpos));
      flinch(a, unit3([0, 1, 2].map(k => f(dir[k] + f(0.15 * away[k])))));
      return;
    }
    if (isDead) die(a, dir);                                                  // 8. no flag 2/4/8
  }

  // FUN_00561570: the flinch. FUN_0034b638(0.15, C, -dir) is left out (GUESS: not read; it runs
  // before the push-back), push-back FUN_003531e0(4, 4, C, dir) + mover 9 (FUN_0034edf0) for an
  // actor without +0x28 bit 0x20 (e027), C+0x214 = 4 (FUN_0034e960: no turn), then FUN_00561718.
  function flinch(a, dir) {
    const C = a.C;
    const locked = (a.f118 & 0x1800) !== 0;
    const mv = C.mover === 1 || C.mover === 0xe;
    if (!mv && !locked) {
      slide(C, 9, 4.0, 4.0, dir);
      C.s214 = 4;
    }
    const started = flinchClip(a);
    if (mv && started) C.f1c0 |= 0x20;
  }
  // FUN_00561718: no new clip within 12 frames of the last (6 on a coin flip: one FUN_0040b5c0 draw,
  // int(r x 2) != 0); stance (actor +0x100 bits 40-41) 0 -> motion state 0x1e (FUN_00341780)
  function flinchClip(a) {
    const el = S.frame - a.f144;                                              // FUN_0054cbe8: DAT_0046e938 +0x20 (L: frames)
    const r = rng();
    if (Math.trunc(r * 2) === 0 ? el < 12 : el < 6) { a.f144 = S.frame - 6; return 0; }
    const st = (a.stance ?? 0) & 3;
    if (st !== 0) { warnOnce(`enemy_ai: flinch stance ${st} (motion 0x1f / actor state 0x11) not translated`); return 0; }
    a.C.motion = { cls: 0x1e, t: 0 };
    a.C.clip = FLINCH_CLIP; a.C.hold = false;
    events.push(`clip ${a.id} ${FLINCH_CLIP}`);
    a.f144 = S.frame;
    return 1;
  }
  // FUN_00560fc8: stop (FUN_003533e0), the death slide FUN_00353340(12, 16, C, dir) + mover 0xb for an
  // actor without +0x28 bit 0x20 (C+0x7e4 +0x50 / +0x54 override: none on the page), status bit 6
  // (FUN_0055f0e0(a, 6) -> FUN_0056b620): not set for z128's soldiers; actor state 7 unless +0x118
  // has 0x8000 / 0x10000
  function die(a, dir) {
    const C = a.C;
    C.mover = 0; C.slide = null;                                              // FUN_003533e0
    slide(C, 0xb, 12.0, 16.0, dir);
    if (a.s64 & 0x40) warnOnce('enemy_ai: status bit 6 death (FUN_0056b620) not translated');
    if (!(a.f118 & 0x18000)) setActorState(a, 7);
  }
  // FUN_003531e0 / FUN_00353340 + FUN_0034edf0(C, mode): direction (unit), speed 2d / T per tick,
  // deceleration speed / T per tick
  function slide(C, mode, d, T, dir) {
    const v = f(f(d + d) / T);
    C.slide = { dir: unit3(dir), v, dv: f(v / T) };
    C.mover = mode;
  }
  // FUN_0034f8c0 (body mover): mode 9 = FUN_00351550, mode 0xb = FUN_003516a0. Both hand the speed to
  // FUN_0034af98 (C+0xc0 dir, C+0xcc speed), which the body move FUN_0034c388 applies as dir x speed x
  // step; then speed -= deceleration x step. Mode 9 ends below 0 (FUN_00352a60 -> mover 0); mode 0xb
  // stops at 0 and stays. GUESS: mode 0xb's second term (a root-motion velocity from the death clip,
  // FUN_003516a0's matrix part) is left out; no enemy collision or ground (the slide is horizontal).
  function moverUpdate(a, step) {
    const C = a.C, s = C.slide;
    if (!s || (C.mover !== 9 && C.mover !== 0xb)) return;
    if (C.mover === 0xb && !(s.v >= 0.01)) { s.v = Math.max(0, f(s.v - f(s.dv * step))); return; }   // FUN_0034b110: no move
    const k = f(s.v * step);
    a.pos = [f(a.pos[0] + f(s.dir[0] * k)), a.pos[1], f(a.pos[2] + f(s.dir[2] * k))];
    s.v = f(s.v - f(s.dv * step));
    if (C.mover === 9) { if (s.v < 0) { s.v = 0; C.mover = 0; C.slide = null; } }
    else if (s.v < 0) s.v = 0;
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
    S.frame++;                                                                // DAT_0046e938 +0x20 (frames)
    activated.length = 0;
    // 1. AI
    scheduler(step);
    // 1b. Vincent's shot (queueShot). FUN_00323128 runs the AI scheduler FUN_00365008 first, then the
    // object update FUN_0053b500, which ticks the player (vt+0x214 of vtable 0x58f128 = FUN_005747a0
    // -> FUN_005786e8 -> FUN_00579570 -> FUN_00577480: the cone draws) before its object lists
    // (FUN_005810e8: bullets, so the flinch draw). The shared RNG is drawn in that order.
    for (const sh of pendingShots.splice(0)) playerShot(...sh);
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

  // boneWorld: (actor) => (bone) => world rows of that bone (the MP's last posed skeleton), set by the
  // page (enemies.js partPose) or a test; the part test needs it. playerShot(muzzle, aim, W): Vincent
  // fired, now; queueShot: the same at this frame's player slot inside the next tick (the page).
  const world = { S, actors, triggers, bullets, objects, addActor, tick, runEvent, recById, events, activated, setActive,
                  playerShot, queueShot, lockTargets, boneWorld: opts.boneWorld || null };
  return world;
}
