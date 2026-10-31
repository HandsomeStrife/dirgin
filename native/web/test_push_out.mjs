// Actor push-out (enemy_ai.js pushOut = FUN_00542ab8) against the PCSX2 trace k_push16
// (tools/trace/out, TRACE_FIELDSET=enemy TRACE_ENEMIES=2 TRACE_PUSH=1; game-derived, not committed):
// Vincent runs east along the ladder-top railing into #66 (record 44). For every logged frame f the
// push the game computed in frame f (record f+1's p_push / e0_push) must equal pushOut over the
// positions record f starts with (Vincent's matrix row 3, #66's body C+0x30).
// Also checks the step: player.js integrate with ctrl.push = FUN_00357d48 -> FUN_0035a440 (move = body+0x40 x
// step + C+0x430, then the walk mesh, 0x80 edges walls for Vincent's body flag 0x200). With k_push17
// (worker 0158: k_push16's route to f960 plus FUN_0035a440's in / out / push from the call log,
// TRACE_CALLLOG=pushstep) the translation is checked frame by frame against the game's own numbers.
//   node native/web/test_push_out.mjs [enemy_ai.js]
import { readFileSync, existsSync } from 'node:fs';
import { pathToFileURL, fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const AI = await import(pathToFileURL(path.resolve(process.argv[2] ?? path.join(here, 'enemy_ai.js'))).href);
const tracePath = path.join(process.env.TRACE_OUT ?? path.join(root, 'tools/trace/out'), 'k_push16.jsonl');
const dataPath = path.join(here, 'data/enemies/ai.json');
let fail = 0;
const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) fail++; };
const done = () => { console.log(fail ? `${fail} FAILED` : 'all ok'); process.exit(fail ? 1 : 0); };
if (!existsSync(dataPath)) { console.log('SKIP: data/enemies/ai.json missing'); done(); }
if (!existsSync(tracePath)) { console.log('SKIP: trace k_push16 missing (tools/trace/scripts/k_push16.json)'); done(); }

const data = JSON.parse(readFileSync(dataPath, 'utf8'));
const R = readFileSync(tracePath, 'utf8').trim().split('\n').slice(1).map(l => JSON.parse(l));
const byF = new Map(R.map(r => [r.frame, r]));
const f32s = h => { const b = Buffer.from(h, 'hex'); return Array.from({ length: b.length / 4 }, (_, i) => b.readFloatLE(i * 4)); };
const e0pos = r => f32s(r.e0_face_pos).slice(4, 7);

// fixed input, no trace needed in principle: two equal cylinders 6 u apart push each other apart by
// (10 - 6)^2 / 5 x 0.25 = 0.8 along the line between them
{
  const rec = data.records[44];
  const player = { pos: [rec.pos[0] - 6, rec.pos[1], rec.pos[2]], hp: 780, id: 0x16, f118: 0, s64: 0 };
  const W = AI.createEnemyAI({ data, player, rand: () => 0.5, quiet: true });
  const a = W.addActor({ rec: 44, astate: 1 });
  const p = W.pushOut(player.pos);
  ok(Math.abs(p[0] + 0.8) < 1e-5 && Math.abs(p[2]) < 1e-5 && Math.abs(a.push[0] - 0.8) < 1e-5,
     `pair 6 u apart: Vincent pushed ${p.map(v => v.toFixed(4))}, #66 ${a.push.map(v => v.toFixed(4))} (want -0.8 / +0.8 in x)`);
  const far = W.pushOut([rec.pos[0] - 10.01, rec.pos[1], rec.pos[2]]);
  ok(far.every(v => v === 0), 'pair 10.01 u apart (more than 5 + 5): no push');
  a.hp = 0;
  ok(W.pushOut(player.pos).every(v => v === 0), 'a dead soldier (HP 0: +0x38 bit 9 cleared at death) pushes nothing');
}

// FUN_00542ab8 / FUN_0025d508 cases, worked by hand (Vincent and #66's record: radius 5, heights 18 / 18;
// "up" = the soldier's feet that far above Vincent's, as on the platform north-east of the ladder, 6-20 u up).
// [horizontal gap, up, Vincent's push (x, y), the soldier's push x, what it checks]
{
  const rec = data.records[44];
  const run = (dx, up, set = () => {}) => {
    const player = { pos: [rec.pos[0] - dx, rec.pos[1], rec.pos[2]], hp: 780, id: 0x16, f118: 0, s64: 0 };
    const W = AI.createEnemyAI({ data, player, rand: () => 0.5, quiet: true });
    const a = W.addActor({ rec: 44, astate: 1 });
    a.pos = [rec.pos[0], rec.pos[1] - up, rec.pos[2]];
    set(a);
    const p = W.pushOut(player.pos);
    return { p, e: a.push };
  };
  const near = (x, y) => Math.abs(x - y) < 2e-5;
  const C = [
    // 8 apart, 7 up: 3-D distance 10.6 > 5 + 5, horizontal 8 <= 10 (the pair test is .xz only). Vincent's
    // low point (5 up) is 2 under the soldier's flat end and outside its radius: the rim point (3, 2) away,
    // overlap 5 - sqrt(13) = 1.394449 -> ^2 / 5 x 0.25 = 0.0972244 along (-3, 2 up)/sqrt(13). The soldier's low
    // point (12 up) is on Vincent's side (t 12 < 18 - 5): overlap 5 - 3 = 2 -> 0.2.
    [8, 7, -0.080895, 0.053930, 0.2, 'horizontal pair test, flat-end rim'],
    // 4 apart, 20 up: both overlaps negative (15 to the flat end; 7.65 from the round end): no push
    [4, 20, 0, 0, 0, 'negative overlap (0 <= ov)'],
    // 2 apart, 6 up: Vincent's low point under the soldier's flat end, inside its radius: 1 u away along
    // -up, overlap 4 -> 0.8 straight down (y); the soldier: side, 5 - 2 = 3 -> 0.2
    [2, 6, 0, 0.8, 0.2, 'flat end (t < 0, inside the radius)'],
    // 6 apart, 10 up: Vincent's half negative (rim 5.1 away); the soldier's low point (15 up) is past
    // 18 - 5 on Vincent's cylinder: the round end, centre 13 up, |(6, 2)| = 6.325, overlap 3.675 -> 0.6754
    // along (6, 2)/6.325: x 0.64078 (this needs Vincent's height 18)
    [6, 10, 0, 0, 0.64078, 'round end (t >= height - r), H_PLAYER'],
    // 7 apart, 3 up: both on the side, overlap 3 -> 0.45 each
    [7, 3, -0.45, 0, 0.45, 'side, raised feet'],
  ];
  for (const [dx, up, px, py, ex, what] of C) {
    const r = run(dx, up);
    ok(near(r.p[0], px) && near(r.p[1], py) && near(r.e[0], ex) && near(r.p[2], 0),
       `${dx} u apart, soldier ${up} u up: Vincent ${r.p.map(v => v.toFixed(4))}, soldier x ${r.e[0].toFixed(4)} (want ${px} / ${py} / ${ex}: ${what})`);
  }
  // flags, 6 u apart level (0.8 each)
  let r = run(6, 0, a => { a.f118 = 0x4000000; });
  ok(near(r.p[0], -3.2) && near(r.e[0], 0.8), `soldier +0x118 bit 0x4000000: Vincent's push x1 (${r.p[0].toFixed(3)}, want -3.2), the soldier's still x0.25 (${r.e[0].toFixed(3)})`);
  r = run(6, 0, a => { a.f118 = 0x10000200; });                             // #67's traced +0x118 (k_push16 e1_a118)
  ok(near(r.p[0], -0.8) && r.e.every(v => v === 0), `soldier +0x118 0x10000200 (#67's in k_push16): not pushed (${r.e.map(v => v.toFixed(3))}), Vincent still is (${r.p[0].toFixed(3)})`);
  const e1 = R.filter(x => x.e1_a118 & 0x10000000);
  ok(e1.length === R.length && e1.every(x => x.e1_push.slice(0, 3).every(v => v === 0)), `k_push16: #67 has +0x118 bit 0x10000000 on all ${e1.length} frames, push 0 on all`);
  // two soldiers 6 u apart, Vincent far off: pushed (0.8 each), unless both have +0x28 bit 0x20 and
  // +0x118 bit 0x20000 (the pair skip); three of the four bits are not enough
  for (const [bits, want] of [[0, 0.8], [4, 0], [3, 0.8]]) {
    const player = { pos: [rec.pos[0] - 100, rec.pos[1], rec.pos[2]], hp: 780, id: 0x16, f118: 0, s64: 0 };
    const W = AI.createEnemyAI({ data, player, rand: () => 0.5, quiet: true });
    const a = W.addActor({ rec: 44, astate: 1 }), b = W.addActor({ rec: 45, astate: 1 });
    b.pos = [rec.pos[0] + 6, rec.pos[1], rec.pos[2]]; a.pos = rec.pos.slice();
    if (bits >= 3) { a.flags28 |= 0x20; b.flags28 |= 0x20; a.f118 = 0x20000; }
    if (bits >= 4) b.f118 = 0x20000;
    W.pushOut(player.pos);
    ok(near(b.push[0], want) && near(a.push[0], -want), `two soldiers 6 u apart, ${bits} of the 4 pair-skip bits: pushed ${b.push[0].toFixed(3)} (want ${want})`);
  }
  r = run(6, 0, a => { a.astate = 0x80; });
  ok(near(r.p[0], -0.8) && r.e.every(v => v === 0), `attacking soldier (actor state 0x80, FUN_0055bb40): not pushed (${r.e[0].toFixed(3)})`);
  r = run(6, 0, a => { a.astate = 0x80; a.flags28 |= 4; });
  ok(near(r.e[0], 0.8), `attacking soldier with +0x28 bit 2: pushed (${r.e[0].toFixed(3)})`);
}

// the trace: frames 800-900 (contact from f809)
{
  const r0 = byF.get(800);
  const player = { pos: r0.matrix[3].slice(0, 3), hp: 780, id: 0x16, f118: 0, s64: 0 };
  const W = AI.createEnemyAI({ data, player, rand: () => 0.5, quiet: true });
  const a = W.addActor({ rec: 44, astate: 3 });
  let n = 0, nz = 0, worst = 0, worstF = -1;
  for (let fr = 800; fr < 900; fr++) {
    const r = byF.get(fr), nx = byF.get(fr + 1);
    if (!r || !nx) continue;
    a.pos = e0pos(r);
    const p = W.pushOut(r.matrix[3].slice(0, 3));
    const want = nx.p_push, wantE = nx.e0_push;
    const err = Math.max(...[0, 1, 2].map(k => Math.abs(p[k] - want[k])), ...[0, 1, 2].map(k => Math.abs(a.push[k] - wantE[k])));
    if (err > worst) { worst = err; worstF = fr; }
    n++; if (Math.hypot(want[0], want[2]) > 0) nz++;
  }
  ok(n >= 95 && nz >= 80 && worst < 2e-4, `k_push16 f800-f899: Vincent's and #66's push vs the game, ${n} frames (${nz} in contact), worst |d| ${worst.toExponential(2)} at f${worstF}`);
}

// the step: player.js's own integrate (state 0), per frame from the game's position at the frame start,
// with play.js's order (pushOut from the frame-start positions, then the step): ctrl.push = this frame's
// pushOut, the walk = f807-f808's step (no push yet; the stick is held; the slid step, k_push16 has no
// body+0x40). The trace's step f809-f826 runs along the railing; the page's step must match it, along the
// game's path and in full. Standing next to #66 (f909-f1019) the push (0.58 u) still enters the move
// (FUN_0035a440: out = 0 + push) and the 0x80 wall east of him holds him (+-0.03 u jitter, page and game).
{
  let PC = null, Col = null;
  try { ({ PlayerController: PC } = await import(pathToFileURL(path.join(here, 'player.js')).href));
        ({ Collision: Col } = await import(pathToFileURL(path.join(here, 'collision.js')).href)); }
  catch (e) { ok(false, `player.js needs the three.js stub: node --import ./native/web/test_hooks.mjs (${e.message})`); }
  const colPath = path.join(here, 'data/z128_collision.bin');
  if (PC && existsSync(colPath)) {
    const buf = readFileSync(colPath), col = new Col(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
    const { STEP } = await import(pathToFileURL(path.join(here, 'player.js')).href);
    const P = f => byF.get(f).matrix[3];
    const walk = [P(808)[0] - P(807)[0], P(808)[2] - P(807)[2]];
    const W = AI.createEnemyAI({ data, player: { pos: P(808).slice(0, 3), hp: 780, id: 0x16, f118: 0, s64: 0 }, rand: () => 0.5, quiet: true });
    const a = W.addActor({ rec: 44, astate: 3 });
    let worst = 0, worstF = -1, without = 0, side = 0, sideF = -1;
    for (let k = 809; k <= 826; k++) {
      const s0 = P(k - 1), g = P(k);
      const ctrl = new PC(col, { pos: { x: s0[0], y: s0[1], z: s0[2] }, heading: 0 });
      ctrl.locate(); ctrl.pos = { x: s0[0], y: s0[1], z: s0[2] };
      a.pos = e0pos(byF.get(k - 1));
      ctrl.push = W.pushOut([s0[0], s0[1], s0[2]]);
      ctrl.integrate({ x: walk[0] / STEP, y: 0, z: walk[1] / STEP });
      const d = [g[0] - s0[0], g[2] - s0[2]], L = Math.hypot(d[0], d[1]), u = [d[0] / L, d[1] / L];
      const m = [ctrl.pos.x - s0[0], ctrl.pos.z - s0[2]];
      const e = Math.abs(m[0] * u[0] + m[1] * u[1] - L);
      if (e > worst) { worst = e; worstF = k; }
      without = Math.max(without, Math.abs(walk[0] * u[0] + walk[1] * u[1] - L));
      const sd = Math.hypot(ctrl.pos.x - g[0], ctrl.pos.z - g[2]);
      if (sd > side) { side = sd; sideF = k; }
    }
    ok(worst < 0.02 && without > 0.5, `k_push16 f809-f826, player.js integrate + pushOut: step along the game's path, worst |d| ${worst.toFixed(4)} u at f${worstF} (walk alone: ${without.toFixed(3)})`);
    // the full step, sideways part included (was 0.81 u off with the push added as a displacement)
    ok(side < 0.05, `k_push16 f809-f826, player.js integrate: full position ${side.toFixed(3)} u from the game at f${sideF} (sideways part included)`);
    // standing still next to #66 (stick released, f909-f1019; the game's push on Vincent ~0.58 u per frame):
    // the game holds him in place (+-0.03 u jitter); the page must not slide him
    {
      let moved = 0, movedF = -1, gameJit = 0, pmax = 0;
      for (let k = 910; k <= 1019; k++) {
        const s0 = P(k - 1), g = P(k);
        const ctrl = new PC(col, { pos: { x: s0[0], y: s0[1], z: s0[2] }, heading: 0 });
        ctrl.locate(); ctrl.pos = { x: s0[0], y: s0[1], z: s0[2] };
        a.pos = e0pos(byF.get(k - 1));
        ctrl.push = W.pushOut([s0[0], s0[1], s0[2]]);
        pmax = Math.max(pmax, Math.hypot(ctrl.push[0], ctrl.push[2]));
        ctrl.integrate({ x: 0, y: 0, z: 0 });
        const d = Math.hypot(ctrl.pos.x - s0[0], ctrl.pos.z - s0[2]);
        if (d > moved) { moved = d; movedF = k; }
        gameJit = Math.max(gameJit, Math.hypot(g[0] - s0[0], g[2] - s0[2]));
      }
      ok(moved < 0.05 && pmax > 0.4 && gameJit < 0.05, `k_push16 f910-f1019, idle beside #66 (push up to ${pmax.toFixed(2)} u): page moves him ${moved.toFixed(3)} u (f${movedF}), game ${gameJit.toFixed(3)} u`);
    }
    // closed loop: from f808, page integrate + pushOut each frame with #66 where the game has him, the
    // walk held to f900 (k_push16's stick), then released; closest approach and rest vs the game's
    {
      const s0 = P(808);
      const ctrl = new PC(col, { pos: { x: s0[0], y: s0[1], z: s0[2] }, heading: 0 });
      ctrl.locate(); ctrl.pos = { x: s0[0], y: s0[1], z: s0[2] };
      const dist = (x, z, k) => { const e = e0pos(byF.get(k)); return Math.hypot(x - e[0], z - e[2]); };
      let pMin = 1e9, gMin = 1e9;
      for (let k = 809; k <= 1019; k++) {
        a.pos = e0pos(byF.get(k - 1));
        ctrl.push = W.pushOut([ctrl.pos.x, ctrl.pos.y, ctrl.pos.z]);
        const w = k <= 900 ? walk : [0, 0];
        ctrl.integrate({ x: w[0] / STEP, y: 0, z: w[1] / STEP });
        pMin = Math.min(pMin, dist(ctrl.pos.x, ctrl.pos.z, k));
        gMin = Math.min(gMin, dist(P(k)[0], P(k)[2], k));
      }
      const pRest = dist(ctrl.pos.x, ctrl.pos.z, 1019), gRest = dist(P(1019)[0], P(1019)[2], 1019);
      ok(Math.abs(pMin - gMin) < 0.05 && gMin > 3.9 && gMin < 4.1,
         `k_push16 f809-f1019 closed loop: closest approach page ${pMin.toFixed(3)} / game ${gMin.toFixed(3)} u from #66`);
      // at f827 the game stops him 6.55 u past #66 with the stick held and holds him there (+-0.1 u jitter)
      // to f900: a 0x80 walk-mesh edge, a wall for body flag 0x200 (FUN_00357d48; worker 0158: the same stop
      // with the push scale DAT_0046e90c poked to 0, and with FUN_0035a440's object pass off)
      ok(Math.abs(pRest - gRest) < 0.05, `k_push16 closed loop: rest page ${pRest.toFixed(3)} / game ${gRest.toFixed(3)} u from #66 (the f827 stop: the 0x80 wall)`);
    }

    // k_push17 (worker 0158): the call log has, per frame, FUN_0035a440's input (body+0x40 x step), the push
    // body+0x80 and its output, and FUN_003590c8's body+0x40 after the move. The page from the game's
    // position at the frame start, fed the game's step and push: FUN_0035a440's output, body+0x40 and the
    // position after the walk mesh must be the game's (f801-f960: walking, contact, the f827 stop, idle).
    const p17 = path.join(process.env.TRACE_OUT ?? path.join(root, 'tools/trace/out'), 'k_push17');
    if (existsSync(p17 + '.jsonl') && existsSync(p17 + '.calls.jsonl')) {
      const rd = f => readFileSync(f, 'utf8').trim().split('\n').slice(1).map(l => JSON.parse(l.replace(/NaN/g, 'null')));
      const B17 = new Map(rd(p17 + '.jsonl').map(r => [r.frame, r])), C17 = rd(p17 + '.calls.jsonl');
      const h2f = h => { const b = Buffer.alloc(4); b.writeUInt32LE(parseInt(h, 16)); return b.readFloatLE(0); };
      const W3 = c => c.u.slice(0, 3).map(h2f);
      const po = new Map(C17.filter(c => c.hook === 'push_out').map(c => [c.frame, c]));
      const bs = new Map(C17.filter(c => c.hook === 'body_step').map(c => [c.frame, c]));
      let n = 0, np = 0, eOut = 0, eVel = 0, ePos = 0, fPos = -1;
      for (let k = 801; k <= 960; k++) {                // call frame k moves record k-1 to record k
        const c = po.get(k), b = bs.get(k);
        if (!c || !b || !B17.has(k - 1) || !B17.has(k)) continue;
        const vin = W3(c['a2+0x0']), q = W3(c['a0+0x80']), gout = W3(c['a1+0x0']), gvel = W3(b['a0+0x40']);
        const s0 = B17.get(k - 1).matrix[3], g = B17.get(k).matrix[3];
        const ctrl = new PC(col, { pos: { x: s0[0], y: s0[1], z: s0[2] }, heading: 0 });
        ctrl.locate(); ctrl.pos = { x: s0[0], y: s0[1], z: s0[2] };
        ctrl.push = q;
        ctrl.body.vel = { x: vin[0] / STEP, y: vin[1] / STEP, z: vin[2] / STEP };
        const out = ctrl.pushIn(vin[0], vin[1], vin[2]);
        eOut = Math.max(eOut, Math.abs(out[0] - gout[0]), Math.abs(out[1] - gout[2]));
        eVel = Math.max(eVel, Math.abs(ctrl.body.vel.x - gvel[0]), Math.abs(ctrl.body.vel.z - gvel[2]));
        ctrl.pos = { x: s0[0], y: s0[1], z: s0[2] };
        ctrl.integrate({ x: vin[0] / STEP, y: vin[1] / STEP, z: vin[2] / STEP });
        const e = Math.hypot(ctrl.pos.x - g[0], ctrl.pos.z - g[2]);
        if (e > ePos) { ePos = e; fPos = k; }
        n++; if (q[0] || q[2]) np++;
      }
      ok(n >= 155 && np >= 140 && eOut < 1e-5, `k_push17 f801-f960: pushIn = FUN_0035a440's output (in + C+0x430), ${n} frames (${np} pushed), worst ${eOut.toExponential(2)}`);
      ok(eVel < 1e-5, `k_push17: body+0x40 after the move (FUN_00357d48's rewrite, scaled to |body+0x40|), worst ${eVel.toExponential(2)}`);
      ok(ePos < 2e-3, `k_push17: page step from the game's position with the game's step and push, worst ${ePos.toFixed(4)} u at f${fPos} (walk, contact, the f827 stop, idle)`);
    } else console.log('skip k_push17 checks (tools/trace/scripts/k_push17.json; TRACE_PUSHSTEP=1 TRACE_CALLLOG=pushstep)');
    // hand cases (FUN_00357d48 after FUN_0035a440 returns 1): |out| capped at 10 (body flag 0x200, no
    // platform); out shorter than body+0x40 replaces it; no push: untouched
    {
      const s0 = P(808);
      const mk = () => { const c = new PC(col, { pos: { x: s0[0], y: s0[1], z: s0[2] }, heading: 0 }); c.locate(); return c; };
      let c = mk(); c.push = [30, 0, 0]; c.body.vel = { x: 1, y: 0, z: 0 };
      let o = c.pushIn(1.2, 0, 0);
      ok(Math.abs(o[0] - 10) < 1e-5 && Math.abs(c.body.vel.x - 1) < 1e-6, `push 30 u: out ${o[0].toFixed(3)} (cap 10), body+0x40 ${c.body.vel.x.toFixed(3)} (scaled to |1|)`);
      c = mk(); c.push = [-0.9, 0, 0]; c.body.vel = { x: 1, y: 0, z: 0 };
      o = c.pushIn(1.2, 0, 0);
      ok(Math.abs(o[0] - 0.3) < 1e-6 && Math.abs(c.body.vel.x - 0.3) < 1e-6, `push against the walk: out ${o[0].toFixed(3)} (0.3), body+0x40 = out ${c.body.vel.x.toFixed(3)}`);
      c = mk(); c.push = [0, 0, 0]; c.body.vel = { x: 1, y: 0, z: 0 };
      o = c.pushIn(1.2, 0, 0);
      ok(o[0] === 1.2 && c.body.vel.x === 1, `no push: out ${o[0]} (in), body+0x40 ${c.body.vel.x} (kept)`);
      ok(mk().col.block80 === true && col.block80 === false, `Vincent walks the mesh as a block80 body (body flag 0x200); the shared mesh keeps block80 ${col.block80}`);
    }
  } else if (PC) ok(false, 'data/z128_collision.bin missing: integrate step unchecked');
}
// Dan: "There was no melee attack". The game in k_push16: #66 (woken by trigger 95 at f784, AI mode 2,
// a shooter: stance flags C+0x808 bit 0) with Vincent 4-7 u from him for ~200 frames never enters an
// attack state (actor +0x104 0x80-0xbf). The page's AI on the same path does the same: modes 1 / 2 reach
// the attack pick (FUN_0035ff98) only in their non-shooter states (FUN_00362ef8 state 7, FUN_00362998 8).
{
  const gameAtk = R.filter(r => (r.e0_astate & 0xff) >= 0x80 && (r.e0_astate & 0xff) <= 0xbf).length;
  const close = R.filter(r => { const e = e0pos(r), m = r.matrix[3]; return Math.hypot(m[0] - e[0], m[2] - e[2]) < 8; }).length;
  const r0 = R[0];
  const player = { pos: r0.matrix[3].slice(0, 3), hp: 780, id: 0x16, f118: 0, s64: 0 };
  let seed = 1;                                     // varying draws, so the 1-in-11 attack draws can pass
  const rand = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296;
  const W = AI.createEnemyAI({ data, player, now: r0.ai_now, rand, quiet: true });
  const a = W.addActor({ rec: 44, astate: 1 });
  let pageAtk = 0, mode2 = 0;
  for (let i = 1; i < R.length; i++) {
    const old = R[i - 1].matrix[3].slice(0, 3), nw = R[i].matrix[3].slice(0, 3);
    player.pos = nw;
    W.tick(1.2, old, nw);
    if (a.astate >= 0x80 && a.astate <= 0xbf) pageAtk++;
    if (a.ai && a.ai.mode === 2) mode2++;
  }
  ok(gameAtk === 0 && close > 150, `k_push16: the game's #66 never attacks (attack-state frames ${gameAtk}) with Vincent < 8 u on ${close} frames`);
  ok(pageAtk === 0 && mode2 > 150, `page AI on the same path: #66 in mode 2 on ${mode2} frames, attack-state frames ${pageAtk} (game 0)`);
}
done();
