// Path walking (script cmd 0x131, path_mover.js) against the PCSX2 trace w159_walk95 (worker 0159;
// tools/trace/scripts/w159_walk95.json with TRACE_FIELDSET=path TRACE_ERECS=46,47 TRACE_ENEMIES=2 and
// TRACE_POKE=9540a4=6e,10a2140=6e: k_lock14's route to the ladder top, where trigger 95 runs trigger 89's
// walk-in script 110, so records 46 / 47 wake at f783 and walk stage paths 5 / 8; game-derived, not committed).
//   1. path_mover alone, from the game's state at f783: position, facing, segment clock, node and the
//      stop frame, every frame of both walks (record f + 1 = after frame f).
//   2. the AI world (enemy_ai.js): the wake routine (script 127 / 128 event 2) runs cmd 0x131; the body,
//      the motion states 3 -> 8 -> 3 (r_gst / r_grn), and the path end -> manager event 0xf -> script
//      event 0x13 (AI mode 1, target the player), against the same records.
//   node native/web/test_path_walk.mjs [enemy_ai.js]
import { readFileSync, existsSync } from 'node:fs';
import { pathToFileURL, fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const AI = await import(pathToFileURL(path.resolve(process.argv[2] ?? path.join(here, 'enemy_ai.js'))).href);
const PM = await import(pathToFileURL(path.join(here, 'path_mover.js')).href);
const tracePath = path.join(process.env.TRACE_OUT ?? path.join(root, 'tools/trace/out'), 'w159_walk95.jsonl');
const dataPath = path.join(here, 'data/enemies/ai.json');
let fail = 0;
const skipped = [];
const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) fail++; };
const skip = m => { console.log('SKIP ' + m); skipped.push(m); };
// missing inputs show in the summary line: 'all ok (2 SKIPPED: ...)'
const done = () => {
  const sk = skipped.length ? ` (${skipped.length} SKIPPED: ${skipped.join('; ')})` : '';
  console.log(fail ? `${fail} FAILED${sk}` : `all ok${sk}`); process.exit(fail ? 1 : 0);
};

// fixed inputs (no data needed): the spline weights sum to 1 and their slopes to 0 (cardinal spline,
// FUN_0025fcc8 / FUN_0025fd58), the step factor FUN_00323318 at the PAL step
{
  const sums = [0, 0.25, 0.5, 1].map(u => [PM.splineBasis(u, 0.4).reduce((a, b) => a + b), PM.splineSlope(u, 0.4).reduce((a, b) => a + b)]);
  ok(sums.every(([b, d]) => Math.abs(b - 1) < 1e-6 && Math.abs(d) < 1e-5) && PM.splineBasis(0, 0.4)[1] === 1 && PM.splineBasis(1, 0.4)[2] === 1,
     `spline weights: sum 1, slopes sum 0, P1 at u 0 / P2 at u 1 (${sums.map(x => x.map(v => v.toFixed(6)).join('/')).join(' ')})`);
  ok(PM.stepFactor(1.0, 1.2) === 1 && PM.stepFactor(0.25, 1.2) === 0.25 && PM.stepFactor(0.5, 0.5) === 0.25,
     `FUN_00323318: w 1 / 0.25 at step 1.2 -> ${PM.stepFactor(1.0, 1.2)} / ${PM.stepFactor(0.25, 1.2)}; 0.5 at step 0.5 -> ${PM.stepFactor(0.5, 0.5)}`);
}
// FUN_0034de50 case 5 on hand-built states (the traced walks never turn round or stand with turn 5):
// moving straight back (dot <= -0.97) blends to row 0 = up x face instead; standing (C+0x1d8 == 0) doesn't turn
{
  const mk = (face, dir, speed) => {
    const a = { pos: [0, 0, 0], face, C: { f1c0: 0x10301, f1c4: 0x104, mover: 0, speed: 0, s214: 0, f45c: 0x36 } };
    PM.bodyInit(a.C, 50); PM.setTurn(a, 5);
    a.C.mm = 0; a.C.dir = dir;
    a.C.speed = speed;
    return a;
  };
  const back = mk([1, 0, 0], [-1, 0, 0], 50);
  PM.bodyFrame(back, [], 1.2);                          // mm 0: sets C+0x1d8 = 0 first (FUN_0034ea18)
  const b2 = mk([1, 0, 0], [-1, 0, 0], 50);
  ok(back.face.join() === '1,0,0', `standing (C+0x1d8 0 after the frame): no turn (${back.face})`);
  PM.turnTest(b2, 1.2);
  ok(Math.abs(b2.face[0]) < 1e-6 && Math.abs(b2.face[2] - 1) < 1e-6, `straight back (dot -1): facing -> row 0 = up x face (${b2.face.map(x => x.toFixed(3))}, want 0,0,1)`);
  const b3 = mk([1, 0, 0], [0.6, -0.5, 0.8], 50);
  PM.turnTest(b3, 1.2);
  ok(Math.abs(b3.face[0] - 0.6) < 1e-6 && b3.face[1] === 0 && Math.abs(b3.face[2] - 0.8) < 1e-6, `moving: facing = the level move direction (${b3.face.map(x => x.toFixed(3))})`);
}
// FUN_00346860 / FUN_00346c10 / FUN_005a8fd0 on hand cases (the traced run clamps to 0.5 whatever the clamp,
// so the formula is checked here): e027's table rows 1 / 9 / 0x10, thresholds 0 / 30, roots 0 / 17.67 / 49.35
{
  const thr = [0, 30], ch = (v, cur, f564 = 8 | 0x10) => AI.locoChoice(Math.fround(v), thr, 3, cur, f564);
  const got = [ch(0, -1), ch(0.005, 0), ch(0.02, 0), ch(30, 1), ch(30.01, 1), ch(29.95, 2), ch(29.95, 1), ch(29.85, 2), ch(0.5, 0, 0x800 | 8), ch(0.8, 0, 0x800 | 8)];
  const want = [0, 0, 1, 1, 2, 2, 1, 1, 0, 1];
  ok(got.join() === want.join(), `FUN_00346860 choice: speed 0 / 0.005 / 0.02 / 30 / 30.01 -> ${got.slice(0, 5)}; 29.95 running -> ${got[5]} (hysteresis -0.1), walking -> ${got[6]}; 29.85 running -> ${got[7]}; ` +
     `bit 0x800 (0.75): 0.5 / 0.8 -> ${got.slice(8)} (want ${want})`);
  const R = (v, root, cur, f564 = 8 | 0x10) => AI.locoRate(Math.fround(v), Math.fround(root), cur, f564);
  const rs = [R(50, 49.354838, 2), R(40, 49.354838, 2), R(47, 49.354838, 2), R(5, 17.671875, 1), R(15, 17.671875, 1), R(25, 17.671875, 1),
              R(15, 17.671875, 1, 8 | 0x10 | 0x800), R(1, 17.671875, 1, 8 | 0x10 | 0x800), R(40, 49.354838, 2, 8 | 0x10 | 1), R(20, 0, 1)];
  const exp = [0.5, 0.45, 0.4761, 0.25, 0.4244, 0.55, 0.4244, 0.05, 0.5, 0.5];
  ok(rs.every((r, i) => Math.abs(r - exp[i]) < 1e-3),
     `FUN_00346c10 MP+0x40c: run 50 / 40 / 47 u/s -> ${rs.slice(0, 3).map(x => x.toFixed(4))} (clamp 0.9..1.0 x 0.5); walk 5 / 15 / 25 -> ${rs.slice(3, 6).map(x => x.toFixed(4))} ` +
     `(floor 0.5, clamp 1.1); bit 0x800 (no floor) 15 / 1 -> ${rs.slice(6, 8).map(x => x.toFixed(4))}; bit 0 run floor 1.0 -> ${rs[8]}; root 0 -> ${rs[9]} (want ${exp})`);
  const cf = [AI.carryFrame(3.6, 24), AI.carryFrame(30, 24), AI.carryFrame(-1, 24), AI.carryFrame(0, 24)];
  ok(cf.join() === '3.6,23.5,0,0', `FUN_005a8fd0 carried frame: 3.6 / 30 / -1 / 0 in a 24-frame clip -> ${cf} (3.6, 23.5, 0, 0)`);
}
// the MP's same-layer test (FUN_005a91c8 plain / FUN_005a9398 aimed): only with flags bit 0x80, the same clip
// and the same partner (plain = none) on the top layer; no layer -> pushed
{
  const top = { clip: 'r_gst', partner: null }, aim = { clip: 'r_st_a', partner: 'r_st_au' };
  const got = [AI.mpDrops(0x83, top, 'r_gst'), AI.mpDrops(0x03, top, 'r_gst'), AI.mpDrops(0x83, top, 'r_grn'), AI.mpDrops(0x83, null, 'r_gst'),
               AI.mpDrops(0x83, aim, 'r_st_a', 'r_st_au'), AI.mpDrops(0x83, aim, 'r_st_a'), AI.mpDrops(0x83, { clip: 'r_st_a', partner: null }, 'r_st_a', 'r_st_au')];
  ok(got.join() === 'true,false,false,false,true,false,false',
     `mpDrops: same plain clip 0x83 ${got[0]}, flags 0x03 ${got[1]}, other clip ${got[2]}, no layer ${got[3]}; aimed same partner ${got[4]}, plain on aimed ${got[5]}, aimed on plain ${got[6]}`);
}
if (!existsSync(dataPath)) { skip('data/enemies/ai.json missing (build_web.sh)'); done(); }
const data = JSON.parse(readFileSync(dataPath, 'utf8'));
if (!data.paths) { skip('ai.json has no stage paths (re-export: export_web_triggers.py)'); done(); }
// cmd 0x131's turn mode (FUN_0055a6d8): 6 when the AI has a target and C+0x590 bit 24 (gun out), else 5
{
  const player = { pos: [-1070.03, -146.07, -1105.58], hp: 780, id: 0x16, f118: 0, s64: 0 };
  const turn = (target, gun) => {
    const W = AI.createEnemyAI({ data, player, rand: () => 0.5, quiet: true, log: () => {} });
    const a = W.addActor({ rec: 46, astate: 1, f118: 0 });
    if (target) a.target = player;
    a.C.gun = gun;
    W.t.send(a, -2, 0x131, [5, 0]);                                         // the command alone, on the state set here
    return [a.C.mover, a.C.s214];
  };
  // FUN_00565bd0 from the state enters 5 (gait 1) / 3 (gait 0): C+0x1d4 = status +0x70 x 0.4 / +0x70, C+0x45c bit 3
  {
    const W = AI.createEnemyAI({ data, player, rand: () => 0.5, quiet: true, log: () => {} });
    const a = W.addActor({ rec: 46, astate: 1, f118: 0 });
    W.t.setActorState(a, 5); const s5 = [a.C.spd, a.C.f45c & 8];
    W.t.setActorState(a, 3); const s3 = [a.C.spd, a.C.f45c & 8];
    ok(s5[0] === 20 && s5[1] === 8 && s3[0] === 50 && s3[1] === 0,
       `gait speed (FUN_00565bd0 -> FUN_0034b978): crouched (state 5) C+0x1d4 ${s5[0]} (50 x 0.4), C+0x45c bit 3 ${s5[1]}; standing (state 3) ${s3[0]}, bit ${s3[1]}`);
  }
  const t = [turn(true, 1), turn(true, 0), turn(false, 1)];
  ok(t.every(x => x[0] === 1) && t.map(x => x[1]).join() === '6,5,5',
     `cmd 0x131 turn mode: target + gun out ${t[0][1]} (6), target + gun in ${t[1][1]} (5), no target + gun out ${t[2][1]} (5); mover ${t.map(x => x[0])}`);
}
{
  const p5 = data.paths[5], p8 = data.paths[8];
  ok(p5.n === 9 && p8.n === 8 && p5.flags === 4 && p5.tension === 0.4 && p5.nodes.length === 9,
     `section 9: path 5 ${p5.n} nodes flags ${p5.flags} tension ${p5.tension}, path 8 ${p8.n} nodes`);
  const r46 = data.records.find(r => r.rec === 46);
  ok(r46.speed === 50, `record 46 move speed (parameter set +0x38 x 10) ${r46.speed} (save C+0x1d4 50)`);
}
if (!existsSync(tracePath)) { skip('trace w159_walk95 missing (tools/trace/scripts/w159_walk95.json): parts 1 and 2'); done(); }

const R = readFileSync(tracePath, 'utf8').trim().split('\n').slice(1).map(l => JSON.parse(l));
const byF = new Map(R.map(r => [r.frame, r]));
const f32s = h => { const b = Buffer.from(h, 'hex'); return Array.from({ length: b.length / 4 }, (_, i) => b.readFloatLE(i * 4)); };
const i32 = (h, o) => Buffer.from(h, 'hex').readInt32LE(o);
const game = (r, k) => {
  const m = f32s(r[`e${k}_cmat`]), mv = f32s(r[`e${k}_mv`]);
  return { pos: m.slice(12, 15), face: m.slice(8, 11), t: mv[3], dur: mv[4], speed: mv[6], mm: i32(r[`e${k}_mv`], 8),
           mover: i32(r[`e${k}_path`], 0), node: i32(r[`e${k}_path`], 0x14), turn: i32(r[`e${k}_mv`], 0x54),
           state: Buffer.from(r[`e${k}_a100`], 'hex')[4], mode: r[`e${k}_ai_mode`], motion: r[`e${k}_motion`],
           mpHead: Buffer.from(r[`e${k}_mp_head`], 'hex')[8] };
};
const d3 = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
const WAKE = 783;                                                            // frame of the wake (record 784 = after it)
const WALK = [{ k: 0, rec: 46, path: 5, end: 897 }, { k: 1, rec: 47, path: 8, end: 930 }];
{
  const g0 = game(byF.get(WAKE), 0), g1 = game(byF.get(WAKE + 1), 0);
  ok(g0.mover === 0 && g1.mover === 1 && g1.mm === 9 && g1.turn === 5, `trace: 46 starts its walk in frame ${WAKE} (mover ${g0.mover} -> ${g1.mover}, move mode ${g1.mm}, turn ${g1.turn})`);
}

// 1. path_mover alone
for (const w of WALK) {
  const g = game(byF.get(WAKE), w.k);
  const a = { pos: g.pos.slice(), face: g.face.slice(), C: { f1c0: 0x10301, f1c4: 0x104, mover: 0, speed: 0, s214: 0, f45c: 0x36 } };
  PM.bodyInit(a.C, 50);
  PM.pathStart(a, data.paths, w.path, 5);
  let maxP = 0, maxF = 0, maxT = 0, nodeOff = 0, endAt = null, n = 0;
  for (let f = WAKE; f <= w.end; f++) {
    if (PM.bodyFrame(a, data.paths, 1.2) === 'done' && endAt == null) endAt = f;
    const r = game(byF.get(f + 1), w.k);
    maxP = Math.max(maxP, d3(a.pos, r.pos)); maxF = Math.max(maxF, d3(a.face, r.face));
    maxT = Math.max(maxT, Math.abs(a.C.t - r.t)); if (a.C.node !== r.node && r.mover) nodeOff++;
    n++;
  }
  const last = game(byF.get(w.end + 1), w.k), node = data.paths[w.path].nodes.at(-1).pos;
  ok(maxP < 1e-3 && maxF < 5e-4 && maxT < 1e-4 && nodeOff === 0,
     `#${w.rec} path ${w.path}, ${n} frames: position max ${maxP.toExponential(2)} u, facing ${maxF.toExponential(2)}, clock ${maxT.toExponential(2)} s, node off ${nodeOff}`);
  ok(endAt === w.end && d3(a.pos, last.pos) < 1e-3 && d3(last.pos, node) > 0.1,
     `#${w.rec} stops in frame ${endAt} (game ${w.end}) at the last evaluated point, ${d3(last.pos, node).toFixed(3)} u short of the last node (page ${d3(a.pos, node).toFixed(3)})`);
}

// 2. the AI world
{
  const player = { pos: [-1070.03, -146.07, -1105.58], hp: 780, id: 0x16, f118: 0, s64: 0 };   // ladder top (trace matrix)
  const logs = [];
  const roots = { r_gwk: 17.671875, r_grn: 49.354838 };                     // e027.json r_gwk / r_grn (aim.js rootSpeed)
  const W = AI.createEnemyAI({ data, player, rand: () => 0.5, quiet: true, log: m => logs.push(m), rootSpeed: (_m, c) => roots[c] ?? null });
  // the save's clip (record 783: r_gst_lk), so the stand state's own row-1 request shows
  const acts = WALK.map(w => W.addActor({ rec: w.rec, astate: 1, f118: 0, clip: game(byF.get(WAKE), w.k).motion }));
  // GUESS-free order: the game's trigger (frame 783) wakes them before their body update; the page's tick runs
  // triggers after the bodies, so here the wake routine runs just before that frame's tick (see notes)
  for (const a of acts) W.runEvent(a, 2, [1, 0]);
  const res = WALK.map((w, i) => ({ maxP: 0, maxF: 0, motions: [], gm: [], ends: null, modes: [],
    // MP layer pushes: the page's (each request through mpDrops, as enemies.js) and the game's (MP+0x448 moves)
    carry: [], req: acts[i].C.req, top: { clip: acts[i].C.clip, partner: null }, push: [], gpush: [], gh: game(byF.get(WAKE), w.k).mpHead }));
  const evs = [];
  for (let f = WAKE; f < 1019; f++) {
    evs.push(...W.tick(1.2, player.pos, player.pos).filter(e => /^path end/.test(e)).map(e => `${f} ${e}`));
    WALK.forEach((w, i) => {
      const a = acts[i], r = game(byF.get(f + 1), w.k), q = res[i];
      if (f <= w.end) { q.maxP = Math.max(q.maxP, d3(a.pos, r.pos)); q.maxF = Math.max(q.maxF, d3(a.face, r.face)); }
      if (q.motions.at(-1)?.[1] !== a.C.clip) q.motions.push([f + 1, a.C.clip]);
      if (q.gm.at(-1)?.[1] !== r.motion) q.gm.push([f + 1, r.motion]);
      if (a.C.req !== q.req) { q.req = a.C.req; if (!AI.mpDrops(a.C.flags, q.top, a.C.clip, a.C.partner)) { q.top = { clip: a.C.clip, partner: a.C.partner }; q.push.push(`${f + 1}:${a.C.clip}`); q.carry.push(`${a.C.clip}:${a.C.reqCarry ? 1 : 0}`); } }
      if (r.mpHead !== q.gh) { q.gh = r.mpHead; q.gpush.push(`${f + 1}:${r.motion}`); }
      if (f === w.end) q.modes = [a.ai.mode, r.mode, a.target === player ? 'P' : a.target, a.astate, a.C.motion && a.C.motion.cls, a.C.loco ? 1 : 0];
    });
  }
  WALK.forEach((w, i) => {
    const q = res[i];
    ok(q.maxP < 1e-3 && q.maxF < 5e-4, `world #${w.rec}: walk position max ${q.maxP.toExponential(2)} u, facing ${q.maxF.toExponential(2)} (wake to f${w.end})`);
    const pg = q.motions.filter(([f]) => f <= w.end + 1).map(x => x.join(':')).join(' '), gg = q.gm.filter(([f]) => f <= w.end + 1).map(x => x.join(':')).join(' ');
    ok(pg === gg, `world #${w.rec}: clips page ${pg} | game ${gg}`);
    const pp = q.push.filter(x => parseInt(x) <= w.end + 1).join(' '), gp = q.gpush.filter(x => parseInt(x) <= w.end + 1).join(' ');
    ok(pp === gp, `world #${w.rec}: MP layer pushes (FUN_005a91c8) page ${pp} | game ${gp} (MP+0x448)`);
    // FUN_00346860 -> FUN_00345448: the table's first choice starts at frame 0, a switch carries the phase
    const cs = q.carry.slice(0, 3).join(' ');
    ok(cs === 'r_gst:0 r_grn:0 r_gst:1', `world #${w.rec}: carried phase per push ${cs} (r_gst:0 r_grn:0 r_gst:1: motion 3's request, first choice, switch)`);
    ok(q.modes[0] === 1 && q.modes[1] === 1 && q.modes[2] === 'P',
       `world #${w.rec}: path end f${w.end} -> event 0x13: AI mode ${q.modes[0]} (game ${q.modes[1]}), target ${q.modes[2]}`);
  });
  ok(evs.join() === WALK.map(w => `${w.end} path end ${w.rec + 22}`).join(), `path end events: ${evs.join(', ')}`);
  // without scripts 127 / 128 event 19 (which set state 3 in the same frame) the stand state shows: motion 8
  // returns 3 when the speed drops (FUN_00594f98), and 3's row-1 request uninstalls the table (FUN_00344608)
  {
    const d2 = JSON.parse(JSON.stringify(data));
    for (const k of ['127', '128']) delete d2.scripts[k]['19'];
    const W2 = AI.createEnemyAI({ data: d2, player, rand: () => 0.5, quiet: true, log: () => {}, rootSpeed: (_m, c) => roots[c] ?? null });
    const a2 = W2.addActor({ rec: 46, astate: 1, f118: 0 });
    W2.runEvent(a2, 2, [1, 0]);
    let at = null, top = { clip: a2.C.clip, partner: null }, req = a2.C.req;
    const cls = [], push = [];
    for (let f = WAKE; f < 905; f++) {
      W2.tick(1.2, player.pos, player.pos);
      if (f >= 896 && f <= 900) cls.push(`${f}:${a2.C.motion && a2.C.motion.cls}`);
      if (a2.C.req !== req) { req = a2.C.req; push.push(`${f}:${a2.C.clip}${AI.mpDrops(a2.C.flags, top, a2.C.clip, a2.C.partner) ? '(dropped)' : ''}`); if (!push.at(-1).includes('drop')) top = { clip: a2.C.clip, partner: a2.C.partner }; }
      if (f === 899) at = [a2.C.motion && a2.C.motion.cls, a2.C.loco ? 1 : 0, a2.C.clip, a2.C.rate];
    }
    ok(at && at[0] === 3 && at[1] === 0 && at[2] === 'r_gst' && at[3] == null,
       `standing at the end (no event 19): motion state ${at && at[0]} (3), table ${at && at[1]} (0), clip ${at && at[2]}, rate ${at && at[3]} (0.5 default)`);
    // FUN_00343160's order: f897 the body stops the walk (FUN_0034ea18) after motion 8's update, so the table step
    // picks r_gst (pushed, as the trace's MP+0x448 at f897); f898 motion 8 returns 3 (FUN_00341780 after the body);
    // f899 motion 3's row-1 request (0x83) finds r_gst on the top layer and is dropped (FUN_005a91c8)
    const want = ['896:8', '897:8', '898:3', '899:3', '900:3'].join(' ');
    const pushes = push.filter(x => parseInt(x) >= 890).join(' ');
    ok(cls.join(' ') === want && pushes === '897:r_gst 899:r_gst(dropped)',
       `path end order (no event 19): motion class ${cls.join(' ')} (want ${want}); requests ${pushes} (want 897:r_gst 899:r_gst(dropped))`);
  }
  const warns = logs.filter(m => /path_mover|0x131|motion state|aimed walk/.test(m));
  ok(warns.length === 0, `no path / locomotion warnings (${warns.join(' | ')})`);
}
done();
