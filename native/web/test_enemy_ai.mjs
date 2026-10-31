// Replays the enemy traces k_enemy_engage7 / k_enemy_engage8 (tools/trace/out, TRACE_FIELDSET=enemy,
// game-derived, gitignored) through enemy_ai.js and compares every frame for #66 / #67 (and the
// AI mode of all seven active soldiers): AI mode, brain state, think interval, target, actor state,
// clip, fire frames, rounds left, fire-timer presence, the AI clock and Vincent's HP (with the
// player damage option on). Also checks defence() (FUN_00318120) on fixed inputs.
//   node native/web/test_enemy_ai.mjs [enemy_ai.js]        (STRICT=1: known gaps fail too)
// Needs data/enemies/ai.json (export_web_triggers.py) and the traces (tools/trace/out, game-derived,
// gitignored). Either missing: SKIP after the fixed-input checks (defence(); GameRandom when the
// save state is present), exit code from those (public clones have neither).
//
// Inputs taken from the trace (they are outside this module): the player's position each frame
// (trigger volumes, AI distances), the soldiers' body flags C+0x564 (motion system: blend busy
// 0x4000), and the state of the seven AIs at the first record. Record f holds the state *before*
// frame f's logic, so simulated frame f-1 is compared with record f.
// Random numbers: the RNG state (DAT_004fbe00) is not in the trace, so the jitter draws are FITTED:
// the values implied by the traced next-fire times (#66 2360 -> 2626: 0.978..0.983; #67 2375 ->
// 2467: 0.006..0.011; #67 after 2467 does not fire again before the end: > 0.47). GameRandom's
// output sequence is NOT checked (seed and call history at stage start not decoded); only its
// twist step, against the save state's MT words (below).
//
// Known gaps (GUESS parts of enemy_ai.js, printed as 'gap', counted as failures only with STRICT=1):
// #67's f875 shot, which misses Vincent in the game but hits in the sim (the bullet hit test,
// BULLET_R, is a GUESS), and in k_enemy_engage7 the body flag C+0x564 (not in that trace's
// fields), which decides #67's f799 'round used, no shot' branch.
import { readFileSync, existsSync } from 'node:fs';
import { pathToFileURL, fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const AI = await import(pathToFileURL(path.resolve(process.argv[2] ?? path.join(here, 'enemy_ai.js'))).href);
const OUT = process.env.TRACE_OUT ?? path.join(root, 'tools/trace/out');
const dataPath = path.join(here, 'data/enemies/ai.json');
const TRACES = ['k_enemy_engage7', 'k_enemy_engage8'];

let fail = 0, ran = 0, gaps = 0;
const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) fail++; };
const gap = (c, m) => { if (c) return ok(c, m); console.log('gap  ' + m); gaps++; if (process.env.STRICT) fail++; };

const hex = s => Buffer.from(s, 'hex');
const s16 = (b, o) => b.readInt16LE(o);
const u32 = (b, o) => b.readUInt32LE(o);
const pos = r => r.matrix[3].slice(0, 3);

// GameRandom vs the game's Mersenne twister state in the save state's RAM (DAT_004fbe00, index
// DAT_004fc7c0): the word just before the index was twisted last (FUN_0040b500 twists the word it
// returns), from the next word (not twisted yet) and word k+397 / k-227 (unchanged since). Only its
// old top bit is lost, so one of the two choices must give the RAM word back. Checks twist() and
// the read-then-twist order; the tempering and the seed are not checked. Skipped without the save.
{
  const save = process.env.SAVE_STATE ?? '/srv/shared/dirge-port/save-states/sea-of-flames.p2s';
  if (!existsSync(save)) console.log(`skip GameRandom check: ${save} missing`);
  else {
    const { execFileSync } = await import('node:child_process');
    const py = `import sys,struct,json\nsys.path.insert(0,sys.argv[1])\nfrom p2s import read_p2s\nm=read_p2s(sys.argv[2])['eeMemory.bin']\n` +
      `print(json.dumps([list(struct.unpack_from('<624I',m,0x4fbe00)),struct.unpack_from('<I',m,0x4fc7c0)[0]]))`;
    const [words, idx] = JSON.parse(execFileSync('python3', ['-I', '-c', py, path.join(root, 'tools/trace'), save], { encoding: 'utf8' }));
    const k = (idx + 623) % 624;
    const fits = [0, 0x80000000].filter(top => {
      const g = new AI.GameRandom(1);
      g.s.set(words);
      g.s[k] = top;
      g.twist(k);
      return g.s[k] === words[k];
    });
    ok(fits.length === 1, `GameRandom.twist(${k}) reproduces the save's MT word (index ${idx})`);
  }
}

// defence FUN_00318120 on fixed inputs (hand-worked from the decomp; notes/enemy-research.md §10.2)
{
  const D = AI.defence, st = AI.PLAYER_STATUS, G = AI.GAME;
  // the save: level term int(0.33*4 + 0.4 + 0.5) = 2; +0x60 bit 2 -> cut index 0 -> +0xaa = 0
  ok(D(st, 45, 0) === 44, `defence: Vincent vs an e027 bullet 45 -> ${D(st, 45, 0)} (game 44, trace f789)`);
  ok(D(st, 45, 3) === 45, 'defence: kind 3 is raw');
  ok(D({ ...st, f60: 0x8231 }, 45, 0) === 44, 'defence: +0x60 bit 2 clear -> element factor 1.0');
  ok(D({ ...st, f60: 0x8231, elem9c: 1 }, 45, 0) === 0, 'defence: element 1 (factor 0) -> 0, no min-1');
  ok(D(st, 45, 0, { ...G, d46b328: 2 }) === 53, 'defence: difficulty index 1 -> +0xae 20 % more (int(44.5*1.2))');
  ok(D({ ...st, lvl: 400 }, 45, 0) === 0 + 1, 'defence: L capped at 99 -> int(45.5*0.01) = 0 -> min 1');
  ok(D({ ...st, f27: 1 }, 45, 0) === 8, 'defence: status +0x27 nibble 1 -> x0.2');
  ok(D(st, 45, 0, { ...G, progress: 5 }) === 22, 'defence: progress < 7 -> DAT_0046e220 {0, 50}: L 50 -> 22');
}

// fixed-input checks above run without the traces / ai.json; then SKIP keeps the exit code
const summary = () => { console.log((fail ? `${fail} FAILED` : 'all ok') + (gaps ? ` (${gaps} known gaps)` : '')); process.exit(fail ? 1 : 0); };
if (!TRACES.some(n => existsSync(path.join(OUT, n + '.jsonl')))) { console.log(`SKIP: no enemy traces in ${OUT}`); summary(); }
if (!existsSync(dataPath)) { console.log('SKIP: data/enemies/ai.json missing (run export_web_triggers.py)'); summary(); }
const data = JSON.parse(readFileSync(dataPath, 'utf8'));

function aiOf(r, k) {
  const b = hex(r[`e${k}_ai`]), t = hex(r[`e${k}_target`]);
  return {
    flags: u32(b, 0), due: u32(b, 4) | 0, last: u32(b, 8) | 0, interval: s16(b, 0xc), bstate: s16(b, 0x10),
    cstate: s16(b, 0x18), mode: r[`e${k}_ai_mode`], f118: u32(t, 0), tid: t.readUInt16LE(4),
    rounds: r[`e${k}_ai_ammo`] != null ? (r[`e${k}_ai_ammo`] << 16) >> 16 : null,
    timer: r[`e${k}_ai_timer`] != null ? r[`e${k}_ai_timer`] !== 0 : null,
    astate: r[`e${k}_astate`] != null ? r[`e${k}_astate`] & 0xff : null,
    cflags: r[`e${k}_cflags`] ?? 0, clip: r[`e${k}_motion`], hp: r[`e${k}_hp`],
    pos: [16, 20, 24].map(o => hex(r[`e${k}_face_pos`]).readFloatLE(o)),
    face: [0, 4, 8].map(o => hex(r[`e${k}_face_pos`]).readFloatLE(o)),
  };
}

function run(name) {
  const file = path.join(OUT, name + '.jsonl');
  if (!existsSync(file)) { console.log(`skip ${name}: ${file} missing`); return; }
  ran++;
  const lines = readFileSync(file, 'utf8').trim().split('\n').map(l => JSON.parse(l));
  const meta = lines[0].meta, R = lines.slice(1);
  const ids = [0, 1, 2, 3, 4, 5, 6].map(k => parseInt(meta.bases['EI' + k], 16));
  const hasHp = R[0].p_hp != null;

  // AI clock accumulator phase (scheduler +8) is not traced: take the one that reproduces the clock
  let acc0 = null;
  for (let c = 0; c < 1 && acc0 == null; c += 0.0125) {
    let now = R[0].ai_now, acc = Math.fround(c), good = true;
    for (let i = 1; i < R.length && good; i++) {
      const f = Math.fround(acc + R[i - 1].step);
      if (f >= 1) { now += Math.trunc(f); acc = Math.fround(f - Math.trunc(f)); } else acc = f;
      good = now === R[i].ai_now;
    }
    if (good) acc0 = c;
  }
  ok(acc0 != null, `${name}: AI clock reproduced (accumulator phase ${acc0})`);
  if (acc0 == null) return;

  // The game's AI queue also holds AIs the trace does not log (on some ticks none of the seven
  // runs: those are theirs; likely the stage-load soldiers 41-43, L). Their think times are not
  // traced, so fit them: count and start due that reproduce the seven AIs' think times (+0x18)
  // over the first 60 frames.
  function build(hidden) {
    const draws = [0.98, 0.008, 0.75, 0.75, 0.75, 0.75];
    const player = { pos: pos(R[0]), hp: hasHp ? R[0].p_hp : 780, id: 0x16, f118: 0, s64: 0 };
    const logs = [];
    const W = AI.createEnemyAI({ data, player, now: R[0].ai_now, acc: Math.fround(acc0), rand: () => draws.shift() ?? 0.5,
      log: m => logs.push(m), damage: true, quiet: true });
    const acts = ids.map((id, k) => {
      const s = aiOf(R[0], k);
      return W.addActor({ id, rec: id - 22, pos: s.pos, face: s.face, hp: s.hp, mode: s.mode, due: s.due, last: s.last,
        interval: s.interval, aiFlags: s.flags, bstate: s.bstate, rounds: s.rounds ?? undefined,
        astate: s.astate ?? 1, clip: s.clip, f564: s.cflags, f118: s.f118 & ~2 });
    });
    hidden.forEach((due, j) => W.addActor({ id: 63 + j, rec: 41 + j, due, last: due - 15, interval: 15, hp: 0, mode: 0 }));
    return { W, acts, player, logs };
  }
  function stepTo(B, i) {
    const r0 = R[i - 1], r = R[i];
    // body flags (input), except bit 30 (look at the target), which the sim sets (FUN_00342608)
    for (let k = 0; k < 7; k++) { const C = B.acts[k].C; C.f564 = (aiOf(r0, k).cflags & ~0x40000000) | (C.f564 & 0x40000000); }
    B.player.pos = pos(r);
    B.W.tick(r0.step, pos(r0), pos(r));
  }
  function dueErr(hidden, n) {
    const B = build(hidden);
    let e = 0;
    for (let i = 1; i < Math.min(n, R.length); i++) {
      stepTo(B, i);
      for (let k = 0; k < 7; k++) if (B.acts[k].ai.due !== aiOf(R[i], k).due) e++;
    }
    return e;
  }
  let hidden = null;
  const t0 = R[0].ai_now;
  for (let n = 0; n <= 3 && !hidden; n++) {
    const pick = (j, cur) => {
      if (hidden) return;
      if (j === n) { if (dueErr(cur, 60) === 0) hidden = cur.slice(); return; }
      for (let d = t0 + 1; d <= t0 + 16; d++) if (!cur.length || d >= cur[cur.length - 1]) pick(j + 1, [...cur, d]);
    };
    pick(0, []);
  }
  ok(hidden != null, `${name}: untraced AIs in the queue fitted: ${hidden ? hidden.length + ' (due ' + hidden + ')' : 'none fits'}`);
  if (!hidden) return;
  const B = build(hidden), { W, acts, player, logs } = B;
  // trigger 95's own var0 ('once') starts clear: the stage file state

  const fields = ['mode', 'bstate', 'interval', 'target', 'astate', 'clip', 'rounds', 'timer'];
  const mism = {}, first = {};
  const fire = { sim: [], game: [] }, hp = { sim: [], game: [] }, wake = { sim: null, game: null };
  let clockBad = 0, dueBad = 0, modeAllBad = 0, dueAll = 0;
  // motion requests (enemy_ai C.req: a new head layer, enemies.js blend) of #66 / #67: frame:clip:loop
  const pushes = [[], []], reqSeen = acts.slice(0, 2).map(a => a.C.req);
  // facing (body C+0x20): the sim's heading vs the game's, every frame, closed loop
  const yaw = v => Math.atan2(v[0], v[2]) * 180 / Math.PI;
  const face = [{ err: [], moved: 0, bit30: 0 }, { err: [], moved: 0, bit30: 0 }];
  for (let i = 1; i < R.length; i++) {
    const r0 = R[i - 1], r = R[i], f = r.frame;
    const hpBefore = player.hp;
    stepTo(B, i);
    if (W.S.now !== r.ai_now) clockBad++;
    if (hasHp) {
      if (player.hp !== hpBefore) hp.sim.push(`${f}:${player.hp}`);
      if (r.p_hp !== r0.p_hp) hp.game.push(`${f}:${r.p_hp}`);
    }
    for (let k = 0; k < 7; k++) {
      const g = aiOf(r, k), a = acts[k];
      if (g.mode !== a.ai.mode) modeAllBad++;
      if (g.due !== a.ai.due) dueAll++;
      if (k > 1) continue;
      if (a.C.req !== reqSeen[k]) { reqSeen[k] = a.C.req; pushes[k].push(`${f}:${a.C.clip}:${a.C.loop ? 1 : 0}`); }
      if (wake.game == null && g.mode === 2 && k === 0) wake.game = f;
      if (wake.sim == null && a.ai.mode === 2 && k === 0) wake.sim = f;
      const sim = { mode: a.ai.mode, bstate: a.ai.bstate, interval: a.ai.interval,
        target: !!(a.f118 & 2), astate: a.astate, clip: a.C.clip, rounds: a.ai.rounds, timer: !!a.ai.timer };
      const game = { mode: g.mode, bstate: g.bstate, interval: g.interval, target: !!(g.f118 & 2),
        astate: g.astate, clip: g.clip, rounds: g.rounds, timer: g.timer };
      if (g.due !== a.ai.due) dueBad++;
      if (r[`e${k}_cflags`] != null && ((g.cflags ^ a.C.f564) & 0x40000000)) face[k].bit30++;
      const fe = Math.abs(((yaw(a.face) - yaw(g.face)) % 360 + 540) % 360 - 180);
      face[k].err.push([f, fe]); if (process.env.FACEDBG && k === 0) console.log(f, yaw(a.face).toFixed(2), yaw(g.face).toFixed(2), fe.toFixed(2));
      if (Math.abs(((yaw(g.face) - yaw(aiOf(R[0], k).face)) % 360 + 540) % 360 - 180) > 30) face[k].moved++;
      for (const fl of fields) {
        if (game[fl] == null) continue;
        const key = `#${ids[k]} ${fl}`;
        mism[key] ??= 0;
        if (sim[fl] !== game[fl]) { mism[key]++; first[key] ??= `f${f} sim ${sim[fl]} game ${game[fl]}`; }
      }
      const rp = aiOf(r0, k);
      if (g.clip === 'r_st_f' && rp.clip !== 'r_st_f') fire.game.push(`#${ids[k]}@${f}`);
    }
  }
  // fire frames from the sim: replay clip changes is cheaper via the events, collected below
  console.log(`${name}: frames ${R[1].frame}..${R[R.length - 1].frame}, wake game f${wake.game} sim f${wake.sim}`);
  ok(clockBad === 0, `${name}: AI clock every frame (${clockBad} off)`);
  ok(wake.game === wake.sim && wake.game != null, `${name}: #66 wakes (mode 0 -> 2) at f${wake.game} (sim f${wake.sim})`);
  ok(modeAllBad === 0, `${name}: AI mode of all 7 soldiers every frame (${modeAllBad} off)`);
  ok(dueAll === 0, `${name}: next think time (AI +0x18) of all 7 every frame (${dueAll} off)`);
  // Facing: FUN_0034de50 case 6 (enemy_ai turnBody). The aim point is Vincent's feet - PLAYER_AIM_H
  // (GUESS for vt+0x5c), so a residual of a few tenths of a degree is that point's xz, and it grows
  // when Vincent is close (k_enemy_engage7: he passes within ~5 u of #66).
  for (let k = 0; k < 2; k++) {
    const e = face[k].err.map(x => x[1]).sort((a, b) => a - b);
    const med = e[e.length >> 1], max = e[e.length - 1], at = face[k].err.find(x => x[1] === max)[0];
    if (R[0].e0_cflags != null) ok(face[k].bit30 === 0, `${name}: #${ids[k]} C+0x564 bit 30 (target look, FUN_00342608) every frame (${face[k].bit30} off)`);
    ok(face[k].moved > 0 && med < 3, `${name}: #${ids[k]} facing every frame: median ${med.toFixed(2)} deg (game turns > 30 deg on ${face[k].moved} frames)`);
    // open loop, a fixed (0.4, -0.1) offset of Vincent's point in his frame takes #66's engage7 median
    // 0.93 -> 0.06 deg: the residual is the vt+0x5c GUESS (PLAYER_AIM_H), large when he is within ~5 u
    // (k_enemy_engage7 #66 only: there Vincent walks past it; elsewhere he stays >= 30 u away)
    if (name === 'k_enemy_engage7' && k === 0) gap(max < 2, `${name}: #${ids[k]} facing max ${max.toFixed(2)} deg (f${at}) < 2 [aim point vt+0x5c GUESS]`);
    else ok(max < 2, `${name}: #${ids[k]} facing max ${max.toFixed(2)} deg (f${at}) < 2`);
  }
  const noFlags = R[0].e0_cflags == null;                 // engage7: C+0x564 not traced
  for (const [key, n] of Object.entries(mism)) {
    const msg = `${key}: ${n} frames off${n ? ' (first ' + first[key] + ')' : ''}`;
    if (noFlags && key.startsWith('#67')) gap(n === 0, msg + ' [no C+0x564 in this trace]');
    else ok(n === 0, msg);
  }
  if (hasHp) {
    const drops = l => l.map((x, j, a) => (j ? +a[j - 1].split(':')[1] : R[0].p_hp) - +x.split(':')[1]);
    const dg = drops(hp.game), ds = drops(hp.sim);
    // every damage event: same per-hit damage in the game and the sim
    ok(dg.length > 0 && ds.length > 0 && [...dg, ...ds].every(d => d === dg[0]), `${name}: Vincent's damage per hit game [${dg}] sim [${ds}]`);
    // every game hit lands in the sim on the same frame (bullet countdown + flight, FUN_005549f0)
    const simF = new Set(hp.sim.map(x => x.split(':')[0]));
    const missing = hp.game.filter(x => !simF.has(x.split(':')[0]));
    ok(missing.length === 0, `${name}: game HP drop frames [${hp.game.map(x => x.split(':')[0])}] all hit in the sim (missing: [${missing}])`);
    const gameF = new Set(hp.game.map(x => x.split(':')[0]));
    const extra = hp.sim.filter(x => !gameF.has(x.split(':')[0]));
    gap(extra.length === 0, `${name}: sim hits the game does not have: [${extra}] (#67's f875 shot misses in the game; hit test BULLET_R GUESS)`);
  }
  console.log(`${name}: game fire frames (r_st_f starts) ${fire.game.join(' ')}`);
  // k_fire_emp15 replays this script's inputs to f830 with #66 / #67's motion-player layers
  // (TRACE_EMP=1): every head-layer push (MP+0x448 changes) with its clip and loop flag (+0x1e bit 0)
  if (logs.length) console.log(`${name}: log ${[...new Set(logs)].slice(0, 8).join(' | ')}`);
  if (name === 'k_enemy_engage8') checkPushes(pushes, 830);
}

// Head-layer pushes of the game (k_fire_emp15: e0 = #66, e1 = #67) vs the sim's motion requests,
// frames < `to` (frame = record index, the same convention as the fields above).
function checkPushes(pushes, to) {
  const file = path.join(OUT, 'k_fire_emp15.jsonl');
  if (!existsSync(file)) { console.log('skip motion-request pushes: k_fire_emp15.jsonl missing (TRACE_EMP=1 run)'); return; }
  const R = readFileSync(file, 'utf8').trim().split('\n').slice(1).map(l => JSON.parse(l));
  for (let k = 0; k < 2; k++) {
    const game = [];
    for (let i = 1; i < R.length; i++) {
      const h0 = parseInt(R[i - 1][`e${k}_mp_head`].slice(16, 18), 16), h = parseInt(R[i][`e${k}_mp_head`].slice(16, 18), 16);
      if (h === h0) continue;
      const L = R[i][`e${k}_mp_layers`][h];
      game.push(`${R[i].frame}:${L.clip}:${L.lflags & 1}`);
    }
    const lo = R[1].frame;
    const sim = pushes[k].filter(x => { const f = +x.split(':')[0]; return f >= lo && f < to; });
    ok(game.length > 0 && game.join(' ') === sim.join(' '), `k_fire_emp15 #${66 + k}: motion pushes (frame:clip:loop) game [${game.join(' ')}] sim [${sim.join(' ')}]`);
  }
}

for (const n of TRACES) run(n);

// Synthetic: the bullet hit test uses where Vincent is when the bullet flies (not where he was
// aimed at). #66 (record 44) awake and targeting him 30 u away; he either stands still or steps
// 40 u sideways on the frame the shot goes out. GUESS stand-in (BULLET_R), so only the
// clear-miss / clear-hit cases are checked.
for (const [side, damage, dist, flight] of [[0, true, 30, 3], [40, true, 30], [0, undefined, 30], [0, true, 100, 4]]) {
  const rec = data.records[44];
  const player = { pos: [rec.pos[0] - dist, rec.pos[1], rec.pos[2]], hp: 780, id: 0x16, f118: 0, s64: 0 };
  const W = AI.createEnemyAI({ data, player, rand: () => 0.5, damage, quiet: true });
  // #66's save-state actor state 1 (play.js AWAKE_STATE): 1 -> 3 = motion 6, the first shot spawns its bullet
  // (from the activation state 2, motion 5 holds C+0x564 0x4000 and the first shot has none)
  const a = W.addActor({ rec: 44, mode: 2, hp: 1, astate: 1 });
  let shotAt = null, hitAt = null;
  for (let i = 0; i < 40; i++) {
    const ev = W.tick(1.2, player.pos, player.pos);
    if (shotAt == null && ev.some(e => e === `shot ${a.id}`)) { shotAt = i; player.pos = [player.pos[0], player.pos[1], player.pos[2] + side]; }
    if (hitAt == null && player.hp !== 780) hitAt = i;
  }
  const hit = player.hp !== 780;
  ok(shotAt != null && hit === (side === 0 && !!damage), `synthetic: Vincent ${dist} u away ${side ? 'steps ' + side + ' u aside' : 'stands'} after the shot${damage ? '' : ' (damage option not given: off)'} -> ${hit ? 'hit' : 'no hit'}`);
  // 2 frames of countdown (+0x142), then 1.2 x 50 = 60 u per frame along the path
  if (flight) ok(hitAt - shotAt === flight, `synthetic: ${dist} u: hit ${hitAt - shotAt} frames after the shot (expected ${flight})`);
}

// turnBody (FUN_0034de50 case 6) on fixed inputs: a target straight behind (face . dir <= -0.97)
// turns toward row 0 = up x face, up (0,-1,0): face (0,0,1) -> row 0 (-1,0,0) -> half way = yaw -45;
// at step 1.2 the fraction is FUN_00323318(0.5) = 0.5, at step 2.4 it is 1 - 0.5^2 = 0.75.
{
  const y = v => Math.atan2(v[0], v[2]) * 180 / Math.PI;
  const behind = AI.turnBody([0, 0, 1], [0, 0, 0], [0, -10, -50], 1.2);
  ok(Math.abs(y(behind) + 45) < 1e-4 && behind[1] === 0, `turnBody: target behind -> toward row 0, yaw ${y(behind).toFixed(3)} (-45)`);
  const side = AI.turnBody([0, 0, 1], [0, 0, 0], [50, -12.4, 0], 1.2);
  const want = Math.atan2(50 / Math.hypot(50, 12.4), 1) * 180 / Math.PI;     // the 3D unit dir's x is < 1 (dy), so < 45
  ok(Math.abs(y(side) - want) < 1e-3 && side[1] === 0, `turnBody: target 90 deg right and 12.4 up, 3D lerp 0.5 then flattened: yaw ${y(side).toFixed(3)} (${want.toFixed(3)})`);
  ok(Math.abs(AI.fracStep(0.5, 2.4) - 0.75) < 1e-7 && Math.abs(AI.fracStep(0.5, 0.5) - 0.25) < 1e-7, 'fracStep: FUN_00323318(0.5) at step 2.4 = 0.75, at 0.5 = 0.25');
}

// A dead soldier (HP 0) doesn't think or shoot: the death path FUN_0055cac8 -> FUN_0035f728 puts
// its AI in mode 0 (notes/enemy-research.md §9.10, f843: mode 2 -> 0, fire timer 0), and
// FUN_003605d8 returns when the owner is dead. Added with mode 2, it must not wake or fire.
{
  const rec = data.records[44];
  const player = { pos: [rec.pos[0] - 30, rec.pos[1], rec.pos[2]], hp: 780, id: 0x16, f118: 0, s64: 0 };
  const W = AI.createEnemyAI({ data, player, rand: () => 0.5, damage: true, quiet: true });
  const a = W.addActor({ rec: 44, mode: 2, hp: 0 });
  let shots = 0;
  for (let i = 0; i < 200; i++) shots += W.tick(1.0, player.pos, player.pos).filter(e => e === `shot ${a.id}`).length;
  ok(a.ai.mode === 0 && shots === 0 && player.hp === 780, `dead soldier: AI mode ${a.ai.mode} (0), ${shots} shots in 200 frames (0), Vincent HP ${player.hp}`);
}

// Activation after the save (notes/enemy-activation.md §1 / §2): cmd 0x108 (FUN_0054cd70 ->
// FUN_00559748), the triggers' own "once" var0, and the bit-0x20000 triggers FUN_00550ad8 leaves off.
// The stage scripts are the game's (ai.json); the walks are synthetic.
{
  const trigPos = k => data.triggers[k].m[3].slice(0, 3);
  // walk through trigger k along world x (old -> new crosses the volume; FUN_005517a8 sweep)
  const cross = (W, k, back = false) => {
    const [x, y, z] = trigPos(k), a = [x - 40, y, z], b = [x + 40, y, z];
    W.tick(1.0, back ? b : a, back ? a : b);
    W.tick(1.0, back ? a : b, back ? a : b);       // next frame: queued 0x101 messages delivered
  };
  const mk = (extra = {}) => {
    const got = [];
    const player = { pos: [-1395, -99, -1110], hp: 780, id: 0x16, f118: 0, s64: 0 };
    const W = AI.createEnemyAI({ data, player, rand: () => 0.5, quiet: true, deadRecs: [41, 42, 43],
      onActivate: (rec, a) => got.push([rec.rec, !!a]), ...extra });
    return { W, got, player };
  };
  const recActors = (W, k) => W.actors.filter(a => a.rec === k).length;

  // trigger 56 (script 91, ev 0xd, once): 0x108 -> records 50 / 51, exactly once
  {
    const { W, got } = mk();
    cross(W, 56);
    ok(JSON.stringify(got) === '[[50,true],[51,true]]', `trigger 56: activates records ${JSON.stringify(got)} ([[50,true],[51,true]])`);
    ok(recActors(W, 50) === 1 && recActors(W, 51) === 1, `trigger 56: one AI actor each for 50 / 51 (${recActors(W, 50)}, ${recActors(W, 51)})`);
    const a50 = W.actors.find(a => a.rec === 50);
    ok(a50 && a50.ai.mode === data.records[50].mode && a50.flags28 & 1, `trigger 56: record 50 active, AI mode = section-1 +8 (${a50?.ai.mode})`);
    ok(W.objects.get(41 + 22).kind === 'record' && !(W.objects.get(41 + 22).flags28 & 1), 'trigger 56: dead record 41 woken (0x101) but not activated');
    // run-once: turn 50 off, go back through 56 both ways -> its script's var0 guard sends nothing
    W.setActive(W.objects.get(50 + 22), 0);
    got.length = 0;
    cross(W, 56, true); cross(W, 56);
    ok(got.length === 0 && recActors(W, 50) === 0, `trigger 56 once (own var0): re-entry activates ${JSON.stringify(got)} ([])`);
    // FUN_00559748: activating an active record is a no-op
    got.length = 0;
    W.setActive(W.objects.get(51 + 22), 1);
    ok(got.length === 0 && recActors(W, 51) === 1, `0x108 to an active record: no-op (${got.length} activations, ${recActors(W, 51)} actor)`);
    // FUN_00559748: a record whose status is dead (the save's 41) is not activated
    W.setActive(W.objects.get(41 + 22), 1);
    ok(got.length === 0 && recActors(W, 41) === 0, `0x108 to dead record 41: not activated (${got.length} activations)`);
  }

  // trigger 205 (flags 0x20001, script 129: wake n2 -> 50 / 51) starts off (FUN_00550ad8); on the
  // save's difficulty 1 the stage controller (script 0 event 0x22, flag0 = 0) leaves it off; on
  // difficulty 3 it arms it with 0x3d / 0x108.
  ok((data.triggers[205].flags >>> 17) & 1, 'trigger 205 has flag bit 0x20000');
  for (const diff of [1, 3]) {
    const { W } = mk({ difficulty: diff });
    const t205 = W.triggers[205];
    ok(!t205.active, `difficulty ${diff}: trigger 205 off at stage start`);
    W.runEvent({ kind: 'stage', id: 'stage', script: 0, vars: new Int16Array(8), flags28: 0 }, 0x22);
    ok(t205.active === (diff === 3), `difficulty ${diff}: after script 0 event 0x22 trigger 205 ${t205.active ? 'armed' : 'off'} (${diff === 3 ? 'armed' : 'off'})`);
    cross(W, 56);                                    // 50 / 51 active, mode 0
    const fired = [];
    const [x, y, z] = trigPos(205);
    fired.push(...W.tick(1.0, [x - 40, y, z], [x + 40, y, z]));
    fired.push(...W.tick(1.0, [x + 40, y, z], [x + 40, y, z]));
    const ran = fired.some(e => e.startsWith('event t205 '));
    const woke = fired.filter(e => / ev 2 r 2,/.test(e)).length;
    // (on difficulty 3 script 129 first runs commands not translated yet (0xa1, 0x106), so only the
    // run itself is checked there)
    ok(ran === (diff === 3) && (diff === 3 || woke === 0), `difficulty ${diff}: walking through trigger 205 ${ran ? 'runs' : 'does not run'} its script (${diff === 3 ? 'runs' : 'does not'}), ${woke} wake n2 messages`);
  }

  // Deactivation mid-burst: FUN_00559748 off -> FUN_00558bb8 -> FUN_003659d8 -> AI destructor
  // FUN_0035e6f8 -> FUN_00360820 (fire timer off). The switched-off soldier fires no more rounds;
  // reactivated (a new AI, FUN_00365c50) and woken again by trigger 95 he has one fire timer.
  {
    const { W, player } = mk();
    const rec = data.records[44];
    player.pos = [rec.pos[0] - 30, rec.pos[1], rec.pos[2]];
    const a = W.addActor({ rec: 44, mode: 2, hp: 1 });
    W.addActor({ rec: 45 });                        // idle, keeps the brain queue (and fireTimers) running
    const rounds = ev => ev.filter(e => e.startsWith(`round ${a.id} `)).length;
    let first = false;
    for (let i = 0; i < 200 && !first; i++) first = rounds(W.tick(1.0, player.pos, player.pos)) > 0;
    const midBurst = first && !!a.ai.timer;
    W.setActive(a, 0);
    let after = 0;
    for (let i = 0; i < 300; i++) after += rounds(W.tick(1.0, player.pos, player.pos));
    const stale = W.S.timers.filter(e => e.ai === a.ai).length;
    ok(midBurst && after === 0 && stale === 0 && !a.ai.timer, `deactivated mid-burst (timer running: ${midBurst}): ${after} rounds in 300 frames (0), ${stale} timers left (0)`);
    W.setActive(W.objects.get(a.id), 1);
    const b = W.actors.find(x => x.rec === 44);
    cross(W, 95);                                   // wake n0 -> mode 2 (script 108)
    let most = 0, fired = 0;
    for (let i = 0; i < 300; i++) {
      fired += W.tick(1.0, player.pos, player.pos).filter(e => e.startsWith(`round ${a.id} `)).length;
      most = Math.max(most, W.S.timers.filter(e => e.ai.owner.rec === 44).length);
    }
    ok(b && b !== a && b.ai.mode === 2 && fired > 0 && most === 1, `reactivated: new AI mode ${b?.ai.mode} (2), ${fired} rounds, at most ${most} fire timer (1)`);
  }

  // trigger 95 unchanged: armed from the start, wakes 44 / 45 to mode 2 once (traced: notes §8)
  {
    const { W, got } = mk();
    const a44 = W.addActor({ rec: 44 }), a45 = W.addActor({ rec: 45 });
    ok(W.triggers[95].active, 'trigger 95 armed at stage start (no bit 0x20000)');
    cross(W, 95);
    ok(a44.ai.mode === 2 && a45.ai.mode === 2 && got.length === 0, `trigger 95: 44 / 45 AI mode ${a44.ai.mode} / ${a45.ai.mode} (2 / 2), no activations (${got.length})`);
  }

  // FUN_005641c0 with act 0 (the brains' FUN_0055b8e0(a, st, 0, 0)): the same motion as now is not
  // restarted, any class. #66 in state 1 already in motion 6 (its row requested): 1 -> 3 = motion 6
  // (FUN_00537aa0) keeps that motion and asks for no clip.
  {
    const { W, player } = mk();
    const a44 = W.addActor({ rec: 44, astate: 1 });
    a44.C.motion = { cls: 6, phase: 0, timer: 0, done: 0, t: 0, next74: 0xe, started: true };
    const m0 = a44.C.motion, req0 = a44.C.req;
    cross(W, 95);
    let j = 0;
    while (a44.astate !== 3 && j++ < 40) W.tick(1.2, player.pos, player.pos);
    ok(a44.astate === 3 && a44.C.motion === m0 && a44.C.req === req0,
       `actor state 1 -> 3 in motion 6: motion kept (${a44.C.motion === m0}), no new clip request (${a44.C.req - req0})`);
  }

  // Activation without a given state (FUN_00559748 -> FUN_0055b8e0(a, 2, 1, 0)): actor state 2, and on
  // the wake 2 -> 3 = motion 5 (raise the gun): r_st_a without loop, then looped once its timer > 8.
  // T: k_fire_emp15 #67 (state 2 in the save) pushes r_st_a:0 at f796, r_st_a:1 at f804 (step 1.2).
  {
    const { W, player } = mk();
    const a45 = W.addActor({ rec: 45 });
    const st0 = a45.astate, gun0 = a45.C.gun, mot0 = a45.C.motion && a45.C.motion.cls;
    const push = [];
    let req = a45.C.req;
    cross(W, 95);
    for (let i = 0; i < 40; i++) {
      W.tick(1.2, player.pos, player.pos);
      if (a45.C.req !== req) { req = a45.C.req; push.push([W.S.frame, `${a45.C.clip}:${a45.C.loop ? 1 : 0}`]); }
    }
    let want = [[796, 'r_st_a:0'], [804, 'r_st_a:1']];
    const tf = path.join(OUT, 'k_fire_emp15.jsonl');
    if (existsSync(tf)) {
      const R = readFileSync(tf, 'utf8').trim().split('\n').slice(1).map(l => JSON.parse(l));
      want = [];
      for (let i = 1; i < R.length && want.length < 2; i++) {
        const h0 = R[i - 1].e1_mp_head.slice(16, 18), h = R[i].e1_mp_head.slice(16, 18);
        if (h !== h0) { const L = R[i].e1_mp_layers[parseInt(h, 16)]; want.push([R[i].frame, `${L.clip}:${L.lflags & 1}`]); }
      }
    }
    const got2 = push.slice(0, 2);
    ok(st0 === 2 && gun0 === 0 && mot0 === 3 && got2.length === 2 && got2.map(x => x[1]).join(' ') === want.map(x => x[1]).join(' ')
       && got2[1][0] - got2[0][0] === want[1][0] - want[0][0],
       `activated #67 (no astate): state ${st0} (2), gun ${gun0}, motion ${mot0} (3); wake pushes ${got2.map(x => x[1] + '@+' + (x[0] - got2[0][0])).join(' ')} ` +
       `(k_fire_emp15 #67: ${want.map(x => x[1] + '@+' + (x[0] - want[0][0])).join(' ')})`);
  }
}

// Other types (notes/enemy-ai-hook.md "Other types"). Every placement is the actor class 0x58e1f8; what
// differs is data. FUN_00564228 picks the stance by model name (e027 / e048 / e036 by weapon class W+0x14:
// 2 -> 0x1e, else 0x1f; e030 0x23; ...), its flags C+0x808 and clip rows come from the stage's 'bmd'
// table (06800009.bin; save state: 0x1e / 0x1f 0x872b, 0x23 0x8c44). Synthetic: each awake (mode 2) with
// Vincent 30 u away.
if (!data.stances) ok(false, 'ai.json has no stances (rebuild data/enemies: export_web_triggers.py)');
else {
  const st = k => data.stances[data.records[k].stance];
  ok(data.records[28].stance === 0x1f && data.records[24].stance === 0x1e && data.records[7].stance === 0x23 && data.records[44].stance === 0x1f,
     `stances: e048 #28 0x${data.records[28].stance.toString(16)} (w class 3: 0x1f), e048 #24 0x${data.records[24].stance.toString(16)} (class 2: 0x1e), e030 #7 0x${data.records[7].stance.toString(16)} (0x23), e027 #44 0x1f`);
  ok(st(44).flags === 0x872b && st(24).flags === 0x872b && st(7).flags === 0x8c44, `stance flags C+0x808: 0x1f 0x${st(44).flags.toString(16)}, 0x1e 0x${st(24).flags.toString(16)} (0x872b, trace), 0x23 0x${st(7).flags.toString(16)} (save 0x8c44)`);
  ok(st(44).motions[0x40] === 'r_st_a' && st(44).motions[0x57] === 'r_st_f' && st(24).motions[0x57] === 'm_st_f' && st(7).motions[0] === 'st_a',
     `stance rows: 0x1f 0x40 / 0x57 = ${st(44).motions[0x40]} / ${st(44).motions[0x57]} (trace r_st_a / r_st_f), 0x1e 0x57 ${st(24).motions[0x57]}, 0x23 0 ${st(7).motions[0]}`);
  for (const [k, model, aimClip, fireClip, shooter] of [[28, 'e048', 'r_st_a', 'r_st_f', true], [24, 'e048', 'm_st_a', 'm_st_f', true], [7, 'e030', null, null, false]]) {
    const rec = data.records[k];
    const player = { pos: [rec.pos[0] - 30, rec.pos[1], rec.pos[2]], hp: 780, id: 0x16, f118: 0, s64: 0 };
    const W = AI.createEnemyAI({ data, player, rand: () => 0.5, quiet: true });
    const a = W.addActor({ rec: k, mode: 2 });
    const clips = new Set(), states = new Set();
    let shots = 0;
    for (let i = 0; i < 400; i++) {
      shots += W.tick(1.2, player.pos, player.pos).filter(e => e === `shot ${a.id}`).length;
      clips.add(a.C.clip); states.add(a.ai.bstate);
    }
    if (shooter) {
      const Wt = data.weapons[rec.weapon];
      ok(a.ai.W === Wt && states.has(4) && a.ai.cmode === 2 && shots > 0 && clips.has(aimClip) && clips.has(fireClip),
         `${model} #${k} awake: weapon ${rec.weapon} (interval ${Wt.interval}), brain 3 -> 4, combat layer ${a.ai.cmode} (2), ${shots} shots in 400 frames, clips ${[...clips].join(' ')} (${aimClip} / ${fireClip})`);
    } else {
      ok(!a.ai.W && states.has(7) && !states.has(3) && shots === 0 && a.C.clip === 'st_a',
         `${model} #${k} awake: no weapon, brain state 7 (C+0x808 bit 0 clear; states ${[...states].join(',')}), ${shots} shots, clip ${a.C.clip} (st_a)`);
    }
  }
  // the dispatch at activation (cmd 0x108 -> FUN_00559748): an AI actor for AI_TYPES, none for the rest
  const got = [];
  const W = AI.createEnemyAI({ data, player: { pos: [0, 0, 0], hp: 780, id: 0x16, f118: 0, s64: 0 }, quiet: true, onActivate: (r, a) => got.push([r.model, !!a]) });
  const pick = m => data.records.find(r => r.model === m && r.hp > 0).rec;
  for (const m of ['e027', 'e048', 'e030', 'e036', 'e023', 'e024', 'e001']) W.setActive(W.objects.get(pick(m) + 22), 1);
  ok(JSON.stringify(got) === JSON.stringify([['e027', true], ['e048', true], ['e030', true], ['e036', false], ['e023', false], ['e024', false], ['e001', false]]),
     `activation per type: ${got.map(([m, a]) => m + (a ? ' actor' : ' -')).join(', ')} (actor for e027 / e048 / e030)`);
}

// Brain mode 1 (FUN_00362998, start FUN_003628f8): Dan's report 20261031T000852Z-9974, the alley guard
// record 51 (e027, stance 0x1e). Trigger 56 activates 50 / 51 in mode 0; trigger 90 (Vincent walking
// north up the alley) sends them message 2: script 141 sets #50 mode 2, script 142 sets #51 mode 1.
// Mode 1 = the mode-2 shooter + a posture change in state 4 (C+0x808 bit 3): >= 0x79 ticks since the
// last change and a draw < 0.2 -> actor state 5 (FUN_00565bd0(a, 1): crouched, height 0.6 x), body
// motion 0x18 (m_sq_a, C+0x564 0x4000 while it runs) -> 0x16, combat off for 0x1f ticks; the next
// shot is class 0xf (m_sq_f, row 0x7b); the next change stands him up (state 3, motion 0x19 -> 6).
{
  const r51 = data.records[51];
  const walk = (W, player, from, to, n) => {
    for (let i = 1; i <= n; i++) {
      const o = from.map((x, j) => x + (to[j] - x) * (i - 1) / n), p = from.map((x, j) => x + (to[j] - x) * i / n);
      player.pos = p; W.tick(1.0, o, p);
    }
  };
  const run = (draw, frames) => {
    const player = { pos: [-1227.6, -95, -1120], hp: 780, id: 0x16, f118: 0, s64: 0 };
    const W = AI.createEnemyAI({ data, player, rand: () => draw, quiet: true, deadRecs: [41, 42, 43] });
    walk(W, player, [-1227.6, -95, -1120], [-1227.6, -95, -1080], 10);              // trigger 56
    walk(W, player, [-786.6, -160, -760], [-786.6, -160, -630], 40);                 // trigger 90
    const a = W.actors.find(x => x.rec === 51);
    const seen = [];
    for (let i = 0; i < frames && a; i++) {
      const e = W.tick(1.0, player.pos, player.pos);
      seen.push({ f: W.S.frame, now: W.S.now, bst: a.ai.bstate, btime: a.ai.btime, e, astate: a.astate, stance: a.stance, h: a.size.h, f564: a.C.f564,
                  mot: a.C.motion && a.C.motion.cls, clip: a.C.clip, comb: a.ai.cmode });
    }
    return { W, a, seen };
  };
  if (!r51 || r51.stance !== 0x1e || !data.model_clips?.e027?.m_sq_a) ok(false, 'ai.json: record 51 / m_sq_a missing (rebuild data/enemies)');
  else {
    const H = data.sizes[r51.zones].h, shotOf = (R, s) => s.e.includes(`shot ${R.a.id}`);
    const A = run(0.5, 300);
    const shots = A.a ? A.seen.filter(s => shotOf(A, s)).length : 0;
    ok(A.a && A.a.ai.mode === 1 && A.a.astate === 3 && shots >= 15,
       `mode 1: record 51 wakes on trigger 90 (mode ${A.a?.ai.mode}, actor state ${A.a?.astate}) and fires (${shots} shots in 300 frames, weapon ${r51.weapon} interval ${data.weapons[r51.weapon].interval})`);
    ok(A.seen.length && A.seen.every(s => s.astate === 3 && s.stance === 0), 'mode 1, draws 0.5: never crouches (FUN_0040b5c0 < 0.2 never true)');
    const B = run(0.1, 400);
    const i5 = B.seen.findIndex(s => s.astate === 5), c = B.seen[i5];
    ok(i5 >= 0 && c.stance === 1 && Math.abs(c.h - Math.fround(H * 0.6)) < 1e-4 && c.comb === 0 && !!(c.f564 & 0x4000) && c.mot === 0x18 && c.clip === 'm_sq_a',
       `mode 1, draws 0.1: crouches at f${c?.f}: stance ${c?.stance}, height ${c?.h?.toFixed(2)} (0.6 x ${H}), combat layer ${c?.comb}, C+0x564 0x4000 ${!!(c?.f564 & 0x4000)}, motion 0x${c?.mot?.toString(16)}, clip ${c?.clip}`);
    const quiet = i5 >= 0 && B.seen.slice(i5, i5 + 0x1f - 5).every(s => !shotOf(B, s));
    const after16 = i5 >= 0 ? B.seen.slice(i5).find(s => s.mot === 0x16) : null;
    // FUN_005978e8: timer 0 on the first update, + step after each, ends when > 20: 21 frames at step 1.0
    ok(quiet && after16 && !(after16.f564 & 0x4000) && after16.f - c.f === 21, `mode 1: no shot while the combat layer is off; motion 0x18 -> 0x16 at f${after16?.f} (+${after16 ? after16.f - c.f : '?'}, want +21; 0x4000 cleared)`);
    const sq = i5 >= 0 ? B.seen.slice(i5).find(s => shotOf(B, s)) : null;
    ok(sq && sq.clip === 'm_sq_f' && sq.mot === 0xf, `mode 1: first crouched shot f${sq?.f}: class 0x${sq?.mot?.toString(16)} (0xf), clip ${sq?.clip} (m_sq_f)`);
    const u = i5 >= 0 ? B.seen.slice(i5).find(s => s.astate === 3) : null;
    ok(u && u.stance === 0 && u.mot === 0x19 && u.clip === 'm_st_a' && Math.abs(u.h - H) < 1e-4,
       `mode 1: stands up again at f${u?.f}: motion 0x${u?.mot?.toString(16)} (0x19), clip ${u?.clip}, height ${u?.h}`);
    // FUN_00362998 timing (no mode-1 trace: the tracer can't follow actors activated mid-run, so this
    // checks the decoded constants, not game frames). The brain runs every 5 ticks (interval, start
    // FUN_003628f8); a posture change happens on the first run with bTime >= 0x79 after state 4's
    // btime (case 3), state 5 ends on the first run with bTime >= 0x1f after the change.
    // Each wait = the constant rounded up to the next run: [k, k + 5).
    // Wait = ticks from the btime in force the frame before (state 4's entry, or the change) to the change.
    const at = (from, pred) => B.seen.findIndex((s, n) => n >= from && pred(s));
    const jc = at(1, s => s.astate === 5), j3 = at(jc, s => s.bst !== 5), ju = at(j3, s => s.astate === 3);
    const d = y => (y > 0 ? (B.seen[y].now - B.seen[y - 1].btime) << 16 >> 16 : NaN);
    const inRun = (v, k) => v >= k && v < k + 5;
    ok(inRun(d(jc), 0x79) && inRun(d(j3), 0x1f) && inRun(d(ju), 0x79),
       `mode 1 timing: crouch f${B.seen[jc]?.f} (+${d(jc)} ticks since state 4's btime, want [0x79, 0x79 + 5)); ` +
       `brain state 5 -> 3 at f${B.seen[j3]?.f} (+${d(j3)} since the crouch, want [0x1f, 0x1f + 5)); stand f${B.seen[ju]?.f} (+${d(ju)}, want [0x79, 0x79 + 5))`);
  }
}

// Brain mode 1 state 8 (FUN_00362998, the non-shooter e030 #7): with a target, gate FUN_00310c40 >= 0x18,
// bTime >= 0x3d, then a draw: (int)(r x 11) == 0 -> the attack pick FUN_0035ff98 -> FUN_00347bd8 over
// e030's seven entries (zone set 8). Vincent ~32 u in front: only entry 3 (30-65, yaw +-64, flags 0)
// passes (0 / 4 / 5 flag 1, 6 flag 8 with G+0x64 48 >= 0x2d, 1 / 2 out of range) -> actor state 0x83,
// body motion 0x1003 (FUN_00537aa0 0x80 + i -> 0xf80 + st), clip atk_3 (its frame-0 play-clip command),
// state 9 until the attack ends (entry length 95 frames), then 8 again.
{
  const rec = data.records[7];
  const pick = draw => {
    const fw = rec.face, d = 30;
    const player = { pos: [rec.pos[0] + fw[0] * d, rec.pos[1], rec.pos[2] + fw[2] * d], hp: 780, id: 0x16, f118: 0, s64: 0 };
    const W = AI.createEnemyAI({ data, player, rand: () => draw, quiet: true });
    const a = W.addActor({ rec: 7, mode: 1 });
    let wait = null, atk = null, end = null, clip = null, mot = null, b9 = false, hits = [];
    for (let i = 0; i < 400; i++) {
      const before = (W.S.now - a.ai.btime) << 16 >> 16;
      const ev = W.tick(1.0, player.pos, player.pos);
      const e = ev.find(x => x.startsWith(`attack ${a.id} `));
      if (e && atk == null) { atk = { f: i, i: +e.split(' ')[2], st: a.astate }; wait = before + 1; clip = a.C.clip; mot = a.C.motion?.cls; }
      if (atk && a.ai.bstate === 9) b9 = true;
      for (const x of ev) if (x.startsWith(`melee ${a.id} hit`) && end == null) hits.push(i - (atk ? atk.f : 0));
      if (atk && end == null && ev.includes(`attack end ${a.id}`)) end = { f: i, st: a.astate };
    }
    return { a, atk, wait, clip, mot, b9, end, hits };
  };
  const hi = pick(0.1), lo = pick(0.05);                                    // 0.1 x 11 = 1.1 -> 1: no pick
  ok(hi.a.ai.bstate === 8 && !hi.atk, `mode 1 state 8 (e030): draw 0.1 -> no attack (state ${hi.a.ai.bstate})`);
  ok(lo.atk && lo.atk.i === 3 && lo.atk.st === 0x83 && lo.mot === 0x1003 && lo.clip === 'atk_3' && lo.wait >= 0x3d && lo.wait < 0x3d + 5,
     `mode 1 state 8 (e030): draw 0.05 -> attack ${lo.atk?.i} (3) after ${lo.wait} ticks (want [0x3d, 0x3d + 5)), actor state 0x${lo.atk?.st?.toString(16)}, motion 0x${lo.mot?.toString(16)}, clip ${lo.clip} (atk_3)`);
  // entry 3: hit commands at keyframes 13 and 52, length 95. FUN_0059eb18 adds the step before it reads the
  // timer and its first update runs on the attack frame (the brain runs first in FUN_00323128), so
  // keyframe k runs k - 1 frames after the attack frame and the motion is done at +94. Mode 1's start set
  // the gun bit (state 3's enter), so the return state (0x539ea0) is 3.
  // state 9's return writes AI +0x20 (the scheduler interval) = 6
  ok(lo.a.ai.interval === 6, `mode 1 state 9 -> 8: AI +0x20 interval ${lo.a.ai.interval} (6)`);
  ok(lo.b9 && lo.end && lo.end.f - lo.atk.f === 94 && lo.end.st === 3 && lo.hits.join() === '12,51',
     `e030 attack 3: brain state 9 while attacking (${lo.b9}), hit windows at +${lo.hits.join(', +')} (+12, +51), ends +${lo.end ? lo.end.f - lo.atk.f : '?'} (+94) -> actor state ${lo.end?.st} (3; GUESS stand-in for the leave)`);
}

// Attack pick geometry (FUN_00347bd8 / FUN_00347af0) on e027's entry (zone set 7: r_atk_0, 1-30, yaw +-10)
{
  const rec = data.records[44];
  const T = data.attacks && data.attacks[rec.zones];
  ok(T && T.entries.length === 1 && T.entries[0].min === 1 && T.entries[0].max === 30 && T.entries[0].tol === 10 && T.names['0'] === 'r_atk_0',
     `attack table zone set ${rec.zones}: ${T?.entries.length} entry, ${T?.entries[0]?.min}-${T?.entries[0]?.max} yaw +-${T?.entries[0]?.tol}, clip ${T?.names?.['0']} (1, 1-30, 10, r_atk_0)`);
  const T8 = data.attacks && data.attacks[data.records[7].zones];
  ok(T8 && T8.entries.length === 7 && !T8.alt, `attack table e030 (zone set ${data.records[7].zones}): ${T8?.entries.length} entries (7)`);
  // FUN_00347bd8 directly (world.attackPick): Vincent at distance d, angle ang (rad) off the facing, the
  // last draw r picks among the passing entries (ok[(int)(n x r)]); AI flag 2 (visible) set.
  const pickAt = (k, d, ang, r, up = 0) => {
    const rc = data.records[k], fw = rc.face, c = Math.cos(ang), s = Math.sin(ang);
    const dir = [fw[0] * c - fw[2] * s, 0, fw[2] * c + fw[0] * s];
    // feet on his level; the aim point is PLAYER_AIM_H up, so the pick's distance is sqrt(d^2 + 12.4^2)
    const player = { pos: [rc.pos[0] + dir[0] * d, rc.pos[1] - up, rc.pos[2] + dir[2] * d], hp: 780, id: 0x16, f118: 0, s64: 0 };
    const W = AI.createEnemyAI({ data, player, rand: () => r, quiet: true });
    const a = W.addActor({ rec: k, mode: 0 });
    a.f118 |= 3; a.target = player; a.ai.flags |= 2;
    return W.attackPick(a);
  };
  // e027 (1-30, yaw +-10 = +-14 deg): ahead at 15 -> 0; 30 deg off -> none; 40 u -> none
  ok(pickAt(44, 15, 0, 0.5) === 0 && pickAt(44, 15, 0.5236, 0.5) === -1 && pickAt(44, 40, 0, 0.5) === -1 && pickAt(44, 15, -0.2, 0.5) === 0,
     `attack pick e027: ahead 15 u -> ${pickAt(44, 15, 0, 0.5)} (0), 30 deg off -> ${pickAt(44, 15, 0.5236, 0.5)} (-1), 40 u -> ${pickAt(44, 40, 0, 0.5)} (-1), -11 deg -> ${pickAt(44, 15, -0.2, 0.5)} (0)`);
  // e030 at 52 u ahead (dist 53): entry 0 (50-65) has flag 1 (never), 6 (1-120) flag 8 with G+0x64 48 >= 0x2d
  // (skipped), 3 (30-65) passes alone -> 3 for any draw; 1 (70-120, pitch -4..4) needs 70.
  ok(pickAt(7, 52, 0, 0.0) === 3 && pickAt(7, 52, 0, 0.99) === 3 && pickAt(7, 80, 0, 0.0) === 1 && pickAt(7, 80, 0, 0.99) === 1,
     `attack pick e030: 52 u -> ${pickAt(7, 52, 0, 0.0)} / ${pickAt(7, 52, 0, 0.99)} (3 / 3: 0 flag 1, 6 flag 8 skipped), 80 u -> ${pickAt(7, 80, 0, 0.0)} / ${pickAt(7, 80, 0, 0.99)} (1 / 1)`);
  // entry 1's pitch window [-4, 4) (x 16/pi): Vincent 47.6 above at 50 u (aim point 60 up: 50 deg, dist 78)
  // -> pitch 4: out (GUESS sign: up = positive, FUN_00260f54 not decoded); 10 above (22 deg) -> 1: in
  ok(pickAt(7, 50, 0, 0.0, 47.6) === -1 && pickAt(7, 70, 0, 0.0, 10) === 1,
     `attack pick e030 pitch: 50 deg up -> ${pickAt(7, 50, 0, 0.0, 47.6)} (-1), 18 deg up at 70 u -> ${pickAt(7, 70, 0, 0.0, 10)} (1)`);
}

// Brain mode 5 (FUN_003639d0): a soldier switched to mode 5 (most scripts do it when he's hit; here by
// cmd 0x143's path, setMode) keeps acting: start 0x363988 (interval 10, C+0x1c0 |= 0x5000000), state 0 ->
// 5 (actor state 3, combat layer 2, action layer 2) -> 6, fires; after 0x79 ticks with < 3 hits taken and
// < 3 misses, FUN_0035f7c0(ai, 0) with AI +0x98 = 100 (p 1.0) -> 0xc (+0x78 <= 3) -> action layer 6 ->
// 0xd -> 5 again. Close (d34 < 22): combat layer off, state 10 -> attack pick -> 0xb -> 0xe.
{
  const rec = data.records[69];
  const fw = rec.face;
  const mk = d => {
    const player = { pos: [rec.pos[0] + fw[0] * d, rec.pos[1], rec.pos[2] + fw[2] * d], hp: 780, id: 0x16, f118: 0, s64: 0 };
    const W = AI.createEnemyAI({ data, player, rand: () => 0.5, quiet: true });
    const a = W.addActor({ rec: 69, mode: 5 });
    return { W, a, player };
  };
  const { W, a, player } = mk(100);
  const states = new Set(), out = [];
  let shots = 0, f1c0 = a.C.f1c0, int0 = a.ai.interval, al = new Set(), to7 = null;
  for (let i = 0; i < 900; i++) {
    const was = a.ai.bstate, f74 = a.ai.f74;
    const ev = W.tick(1.0, player.pos, player.pos);
    states.add(a.ai.bstate); al.add(a.ai.amode);
    shots += ev.filter(x => x === `shot ${a.id}`).length;
    if (to7 == null && was === 6 && a.ai.bstate === 7) to7 = { f74, i };
    out.push(a.ai.bstate);
  }
  // the first brain run (frame 0 here: the AI is due at once): state 0 with a live target -> 5 -> 6 in one run
  ok(out[0] === 6, `mode 5 first run: state 0 -> 5 -> 6 (fall-through) = ${out[0]} (6)`);
  ok(int0 === 10 && (f1c0 & 0x5000000) === 0x5000000, `mode 5 start (0x363988): interval ${int0} (10), C+0x1c0 0x${f1c0.toString(16)} (bits 0x5000000)`);
  // Damage off: every bullet ends on nothing (+0x74). The third miss sends state 6 to 7 (FUN_00360940
  // clears +0x74, FUN_0035f7c0 p 1.0); 7 -> 8 (action layer 3, the approach): with no move (not
  // translated) and no hit made (+0x78 0) state 8 waits there, still firing (combat layer 2).
  ok([6, 7, 8].every(x => states.has(x)) && shots >= 10 && a.astate === 3 && al.has(2) && al.has(3) && a.ai.cmode === 2 && to7 && to7.f74 >= 3,
     `mode 5 at 100 u: brain states {${[...states].map(x => x.toString(16)).join(',')}} (6, 7, 8), 6 -> 7 at f${to7?.i} with +0x74 ${to7?.f74} (>= 3), ${shots} shots in 900 frames (>= 10), actor state ${a.astate} (3), combat layer ${a.ai.cmode}, action layers {${[...al].join(',')}} (2, 3; not translated)`);
  // close: melee
  const C2 = mk(15);
  let atk = null, hitw = null, endf = null;
  for (let i = 0; i < 200; i++) {
    const ev = C2.W.tick(1.0, C2.player.pos, C2.player.pos);
    if (!atk) { const e = ev.find(x => x.startsWith(`attack ${C2.a.id} `)); if (e) atk = { f: i, clip: C2.a.C.clip, st: C2.a.astate, comb: C2.a.ai.cmode }; }
    if (atk && hitw == null && ev.some(x => x.startsWith(`melee ${C2.a.id} hit`))) hitw = i - atk.f;
    if (atk && endf == null && ev.includes(`attack end ${C2.a.id}`)) endf = { f: i - atk.f, bst: C2.a.ai.bstate, st: C2.a.astate };
  }
  const T = data.attacks[rec.zones];
  ok(atk && atk.f <= 10 && atk.st === 0x80 && atk.clip === T.names['0'] && atk.comb === 0 && hitw === T.entries[0].kf.find(k => k.cmds.some(c => c[0] === 2)).frame - 1,
     `mode 5 at 15 u (d34 < 22): attack at f${atk?.f} (the second brain run: state 6 -> 10, then 10 picks), actor state 0x${atk?.st?.toString(16)} (0x80), clip ${atk?.clip} (${T.names['0']}), combat layer ${atk?.comb} (0), hit window +${hitw}`);
  ok(endf && endf.f === T.entries[0].dur - 1 && endf.st === 3,
     `mode 5 attack ends +${endf?.f} (entry length ${T.entries[0].dur}), actor state ${endf?.st} (3: gun bit set by state 3's enter)`);
  ok(C2.player.hp === 780, `melee does not damage Vincent (hit volume FUN_00541310 not decoded): HP ${C2.player.hp}`);
}

// Brain mode 6 (FUN_00364558): state 0 -> 2 (actor state 3, combat 2, action layer 5); close, the attack
// gate: bTime > 0x78 and (int)(r x 11) == 0 -> pick -> state 6 -> 0 after the attack (interval 6).
{
  const rec = data.records[69], fw = rec.face;
  const player = { pos: [rec.pos[0] + fw[0] * 15, rec.pos[1], rec.pos[2] + fw[2] * 15], hp: 780, id: 0x16, f118: 0, s64: 0 };
  const W = AI.createEnemyAI({ data, player, rand: () => 0.05, quiet: true });
  const a = W.addActor({ rec: 69, mode: 6 });
  let atk = null, back = null;
  const st = new Set();
  for (let i = 0; i < 400; i++) {
    const ev = W.tick(1.0, player.pos, player.pos);
    st.add(a.ai.bstate);
    if (!atk && ev.some(x => x.startsWith(`attack ${a.id} `))) atk = i;
    if (atk != null && back == null && a.ai.bstate === 0 && a.ai.interval === 6) back = i;
  }
  ok(st.has(2) && atk != null && back != null, `mode 6: states {${[...st].join(',')}}, attack at f${atk}, back to state 0 (interval 6) at f${back}`);
}

// Single-state checks on the translated functions (world.t), each paired with the mutant it kills.
{
  const rec = data.records[69], fw = rec.face;
  const mk = (mode, d = 100, rand = () => 0.5) => {
    const player = { pos: [rec.pos[0] + fw[0] * d, rec.pos[1], rec.pos[2] + fw[2] * d], hp: 780, id: 0x16, f118: 0, s64: 0 };
    const W = AI.createEnemyAI({ data, player, rand, quiet: true });
    const a = W.addActor({ rec: 69, mode });
    a.f118 |= 3; a.target = player;                                          // a live target (FUN_0035fc98's bits)
    return { W, a, ai: a.ai, player };
  };
  // mode 5 state 0xe: FUN_0035f890 (= FUN_0035f7c0 ^ 1) == 0 -> C+0x1c0 |= 0x10; then FUN_0035f7c0 -> 0x10, else 0xf.
  // AI +0x98 = 100: p 1 (both draws pass); -100: p 0 (both fail).
  const e = b98 => { const m = mk(5); m.ai.b98 = b98; m.ai.bstate = 0xe; m.a.C.f1c0 &= ~0x10; if (b98 < 0) m.a.C.f1c0 |= 0x10;
    m.W.t.brain5(m.ai); return { bit: !!(m.a.C.f1c0 & 0x10), st: m.ai.bstate }; };
  const ep = e(100), en = e(-100);
  ok(ep.bit && ep.st === 0x10 && !en.bit && en.st === 0xf,
     `mode 5 state 0xe: odds pass -> C+0x1c0 bit 0x10 ${ep.bit} (true), next 0x${ep.st.toString(16)} (0x10); odds fail -> ${en.bit} (false), 0x${en.st.toString(16)} (0xf)`);
  // state 8: 25 <= d34 with a hit made (+0x78) -> 9; closer -> combat layer off, 10
  const s8 = d34 => { const m = mk(5); m.ai.bstate = 8; m.ai.flags |= 2; m.ai.f78 = 1; m.ai.d34 = d34; m.a.C.f1c0 &= ~0x100000;
    m.W.t.brain5(m.ai); return m.ai.bstate; };
  ok(s8(25.0) === 9 && s8(24.99) === 10, `mode 5 state 8: d34 25 -> ${s8(25.0)} (9), 24.99 -> ${s8(24.99)} (10)`);
  // state 6, target not visible: FUN_00360518 (0 <= rounds <= magazine / 2) -> reload, stays; else after 0x79 -> 7
  const s6 = rounds => { const m = mk(5); m.ai.bstate = 6; m.ai.flags &= ~2; m.ai.rounds = rounds; m.ai.btime = (m.W.S.now - 0x79) << 16 >> 16;
    m.W.events.length = 0; m.W.t.brain5(m.ai); return { st: m.ai.bstate, rl: m.W.events.includes(`reload ${m.a.id}`), mag: m.ai.W.mag }; };
  const mag = mk(5).ai.W.mag, h = s6(mag >> 1), h1 = s6((mag >> 1) + 1);
  ok(h.rl && h.st === 6 && !h1.rl && h1.st === 7,
     `mode 5 state 6 unseen: rounds ${mag >> 1} of ${mag} -> reload ${h.rl}, state ${h.st} (true, 6); ${(mag >> 1) + 1} -> reload ${h1.rl}, state ${h1.st} (false, 7)`);
  // bullet ends: on an object -> +0x78 (FUN_00360900), on nothing -> +0x74 (FUN_00360920)
  {
    const m = mk(5);
    m.W.t.bulletEnd({ from: m.a }, true); const hit = [m.ai.f74, m.ai.f78];
    m.W.t.bulletEnd({ from: m.a }, false); const miss = [m.ai.f74, m.ai.f78];
    ok(hit.join() === '0,1' && miss.join() === '1,1', `bullet end counters: hit -> +0x74/+0x78 ${hit} (0,1), then miss -> ${miss} (1,1)`);
  }
  // attack scan: FUN_00347bd8's third argument (AI flag 2, visible) gates every entry
  {
    const r = data.records[44], f = r.face;
    const player = { pos: [r.pos[0] + f[0] * 15, r.pos[1], r.pos[2] + f[2] * 15], hp: 780, id: 0x16, f118: 0, s64: 0 };
    const W = AI.createEnemyAI({ data, player, rand: () => 0.5, quiet: true });
    const a = W.addActor({ rec: 44, mode: 0 });
    a.f118 |= 3; a.target = player;
    a.ai.flags |= 2; const vis = W.attackPick(a);
    a.ai.flags &= ~2; const hid = W.attackPick(a);
    ok(vis === 0 && hid === -1, `attack pick e027 ahead 15 u: visible -> ${vis} (0), not visible -> ${hid} (-1)`);
  }
  // mode 6 state 0: a dead target (FUN_00316d08) -> FUN_0035fe08 only, stays 0; a live one -> 2
  const m6 = hp => { const m = mk(6); m.player.hp = hp; m.ai.bstate = 0; m.W.t.brain6(m.ai); return m.ai.bstate; };
  ok(m6(0) === 0 && m6(780) === 2, `mode 6 state 0: dead target -> ${m6(0)} (0), live -> ${m6(780)} (2)`);
  // mode 6 states 2-5 re-pick every update (FUN_0035fe80 / FUN_0035fec0 / FUN_0035fc98): no target -> the candidate
  {
    const m = mk(6); m.ai.bstate = 2; m.a.f118 &= ~2; m.a.target = null;
    m.W.t.brain6(m.ai);
    ok(m.W.t.target(m.ai) === m.player, `mode 6 state 2 with no target: re-pick -> ${m.W.t.target(m.ai) === m.player ? 'the player' : 'none'} (the player; candidate GUESS)`);
  }
  // actor states 6 / 0x1b enters (FUN_00538218 / FUN_005382e0) clear C+0x590 bit 24
  {
    const m = mk(0), g = [];
    for (const st of [6, 0x1b]) { m.W.t.setActorState(m.a, 3); g.push(m.a.C.gun); m.W.t.setActorState(m.a, st); g.push(m.a.C.gun); }
    ok(g.join() === '1,0,1,0', `gun bit: 3 -> ${g[0]}, 6 -> ${g[1]}, 3 -> ${g[2]}, 0x1b -> ${g[3]} (1,0,1,0)`);
  }
  // attack state 0x80 + i entered from 5 / 6 / 0x1b: FUN_00538420 / FUN_00537d58 -> FUN_00537aa0 -> 0x1000 + i;
  // the attack motion then ends the attack (a kept motion would leave attacking() set for good)
  {
    const sm = mk(0).W.t.stateMotion;
    const ms = [5, 6, 0x1b].map(fr => sm(fr, 0x83));
    ok(ms.every(x => x === 0x1003), `state motion 5 / 6 / 0x1b -> 0x83: ${ms.map(x => '0x' + x.toString(16))} (0x1003)`);
    const m = mk(5, 15);
    m.ai.mode = 0; m.ai.brain = null;                                       // no brain: only the attack runs
    m.W.t.setActorState(m.a, 5);
    m.W.t.setActorState(m.a, 0x80);
    let end = null;
    for (let i = 0; i < 400 && end == null; i++) if (m.W.tick(1.0, m.player.pos, m.player.pos).includes(`attack end ${m.a.id}`)) end = i;
    ok(m.a.C.motion && end != null && !(m.a.astate >= 0x80 && m.a.astate <= 0xbf),
       `attack from crouch (state 5): ends at f${end}, actor state 0x${m.a.astate.toString(16)} (not 0x80-0xbf)`);
  }
}

console.log((fail ? `${fail} FAILED` : 'all ok') + (gaps ? ` (${gaps} known gaps)` : ''));
process.exit(fail ? 1 : 0);
