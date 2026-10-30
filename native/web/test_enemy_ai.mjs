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
  if (logs.length) console.log(`${name}: log ${[...new Set(logs)].slice(0, 8).join(' | ')}`);
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
  const a = W.addActor({ rec: 44, mode: 2, hp: 1 });
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

console.log((fail ? `${fail} FAILED` : 'all ok') + (gaps ? ` (${gaps} known gaps)` : ''));
process.exit(fail ? 1 : 0);
