// Actor push-out (enemy_ai.js pushOut = FUN_00542ab8) against the PCSX2 trace k_push16
// (tools/trace/out, TRACE_FIELDSET=enemy TRACE_ENEMIES=2 TRACE_PUSH=1; game-derived, not committed):
// Vincent runs east along the ladder-top railing into #66 (record 44). For every logged frame f the
// push the game computed in frame f (record f+1's p_push / e0_push) must equal pushOut over the
// positions record f starts with (Vincent's matrix row 3, #66's body C+0x30).
// Also checks the step: Vincent's move in frame f = (walk + the push along the walk) slid by the walk mesh,
// which is what player.js integrate does with ctrl.push (the GUESS for where the push enters; audit row).
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
// pushOut, the walk = f807-f808's step (no push yet; the stick is held). The trace's step f809-f826 runs
// along the railing; the page's step projected on the game's step direction must match it (the GUESS in
// player.js: walk + (push . w_hat) w_hat, worker 0155's fit) and so must the full position (the old
// stand-in, push added as a displacement, was 0.81 u off sideways). Standing next to #66 (f909-f1019)
// the game keeps him in place (+-0.03 u jitter) under a 0.58 u push. Where the push enters the move is
// not decoded (notes/fidelity-audit.md).
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
      // Known gap (not the push): at f827 the game stops him 6.55 u past #66 with the stick held and the push
      // pointing along his walk (+0.5 u), then holds him there (+-0.1 u jitter) to f900; the page's walk
      // mesh has nothing there and he walks ~3 u on. Something not on the page stops him (an object
      // primitive in FUN_0035a440, not translated? not decoded).
      const msg = `k_push16 closed loop: rest page ${pRest.toFixed(3)} / game ${gRest.toFixed(3)} u from #66 (game stops him at f827; cause not decoded, not the push)`;
      if (Math.abs(pRest - gRest) < 0.1) ok(true, msg); else { console.log('gap  ' + msg); if (process.env.STRICT) fail++; }
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
