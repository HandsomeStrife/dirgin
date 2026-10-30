// Vincent's bullets vs the soldiers (enemy_ai.js playerShot / playerBulletHit / hitEnemy, enemy_hit.js)
// against the PCSX2 trace k_hits12 (tools/trace/out, TRACE_FIELDSET=enemy + TRACE_CALLLOG=spread on
// 835:900; game-derived, gitignored; = k_enemy_engage12 cut to 900 frames, record for record):
//   R1 at 840: #66 (record 44, HP 1) is killed by the first pellet (record 843)
//   R1 at 866: #67 (record 45, HP 100) takes a limb pellet (870, flinch + push-back) and the next
//              pellet in the head (871, death + slide)
// Per frame for both soldiers: HP, part hit, actor state, AI mode, clip, AI hit count, position
// (push-back / death slide), and the corpse-settled bit (+0x118 0x100). Plus fixed-input checks of
// the geometry (enemy_hit.js) that run without data.
//   node native/web/test_enemy_hits.mjs           (STRICT=1: known gaps fail too)
// Inputs from the trace (outside the module): Vincent's position, his aim point P+0x2b0 at each shot
// and the shot frames (R1 edge with h_st_f next); the soldiers' position / facing / clip until their
// first hit (open loop; closed loop after it); the pellets' cone draws (two FUN_0040b5c0 per pellet)
// from the call log, since the RNG state is not in the trace. The muzzle / marker 1 are Vincent's j56
// / j05 in h_st_a at frame 0 (GUESS: his clip phase is not in the trace); each pellet's direction is
// compared with the logged FUN_0025a058 output.
// Known gaps (GUESS / open parts, 'gap', failures only with STRICT=1): #67's limb hit takes 25 in the
// game and 24 here (notes §10.2: the +1 is not decoded), so his HP reads 76 for one frame; #66's
// death slide meets a wall in the game (no enemy collision on the page); the pellets fly 0.7-1.0 deg
// off the game's (the muzzle pose), which also shows as <= 0.05 u in the slides (their direction is
// the bullet's).
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { createEnemyAI } from './enemy_ai.js';
import { cylinderHit, partTest, boxCorners, modelPose, boneRows, cone } from './enemy_hit.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const OUT = process.env.TRACE_OUT ?? path.join(root, 'tools/trace/out');
let fail = 0, gaps = 0;
const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) fail++; };
const gap = (c, m) => { if (c) return ok(c, m); console.log('gap  ' + m); gaps++; if (process.env.STRICT) fail++; };
const done = () => { console.log(fail ? `${fail} FAILED${gaps ? ` (${gaps} known gaps)` : ''}` : `all ok${gaps ? ` (${gaps} known gaps)` : ''}`); process.exit(fail ? 1 : 0); };

// ---------------------------------------------------------------- fixed inputs (no data)
{
  const r = 5, A = [0, 0, 0], B = [0, -18, 0];
  ok(!!cylinderHit(r, [-20, -9, 0], [20, -9, 0], A, B), 'cylinder: a segment through the side hits');
  ok(!cylinderHit(r, [-20, -9, 6], [20, -9, 6], A, B), 'cylinder: a segment 6 u off the axis misses (r 5)');
  ok(!cylinderHit(r, [-20, 2, 0], [20, 2, 0], A, B), 'cylinder: a segment below the feet misses (no caps beyond the ends)');
  const inside = cylinderHit(r, [1, -9, 0], [2, -9, 0], A, B);
  ok(!!inside && inside.point[0] === 1, 'cylinder: a segment inside hits at its start (FUN_0025c770)');
  ok(!!cylinderHit(r, [0, 10, 1], [0, -30, 1], A, B), 'cylinder: a segment along the axis hits the end disc');
  const I = [[1, 0, 0], [0, 1, 0], [0, 0, 1], [0, 0, 0]];
  const boxes = [{ offset: [0, 0, 0], bone: 0, scale: [1, 1, 1] }, { offset: [5, 0, 0], bone: 0, scale: [1, 1, 1] }];
  const bw = () => I;
  ok(partTest(boxes, bw, [-10, 0, 0], [10, 0, 0]) === 0, 'partTest: the first box along the segment wins');
  ok(partTest(boxes, bw, [10, 0, 0], [-10, 0, 0]) === 1, 'partTest: reversed segment -> the other box');
  ok(partTest(boxes, bw, [-10, 2, 0], [10, 2, 0]) === -1, 'partTest: passing above both boxes -> -1');
  ok(partTest(boxes, bw, [0.2, 0.2, 0.2], [0.3, 0.3, 0.3]) === -1, 'partTest: a segment inside a box crosses no face -> -1 (FUN_0030baa0)');
  // FUN_0025a058 on notes/enemy-research.md §11.2's logged e027 shots (muzzle, aim, r1, r2 -> dir)
  const vecs = [[[-963.7263, -173.8069, -1084.9316], [-1069.9387, -159.2386, -1105.2223], 0.09658, 0.79215, [-0.9340, 0.0872, -0.3464]],
                [[-963.8456, -173.0982, -1085.4814], [-1034.6635, -157.4682, -1103.1165], 0.429386, 0.731939, [-0.9405, 0.2127, -0.2650]],
                [[-1046.2216, -155.2970, -1098.2831], [-1074.4689, -158.2524, -1105.6091], 0.549953, 0.86268, [-0.9682, -0.0870, -0.2344]]];
  for (const [m, a, r1, r2, want] of vecs) {
    const q = [r1, r2], d = cone(24.390244, a.map((x, k) => x - m[k]), () => q.shift());
    const e = Math.max(...d.map((x, k) => Math.abs(x - want[k])));
    ok(e < 1e-4, `cone (FUN_0025a058): r ${r1} / ${r2} -> ${d.map(x => x.toFixed(4))} (log ${want}, max ${e.toExponential(1)})`);
  }
  const c = boxCorners({ offset: [1, 2, 3], bone: 0, scale: [2, 1, 1] }, [[0, 1, 0], [-1, 0, 0], [0, 0, 1], [10, 0, 0]]);
  ok(Math.abs(c[6][0] - 7) < 1e-9 && Math.abs(c[6][1] - 3) < 1e-9 && Math.abs(c[6][2] - 4) < 1e-9, `boxCorners: corner x local x bone rows (${c[6]})`);
}

// ---------------------------------------------------------------- data + traces
const D = p => path.join(here, 'data', p);
const need = [D('enemies/ai.json'), D('enemies/e027.json'), D('enemies/e027_local.bin'), D('aim/weapon.json'),
              D('vincent/vincent_shoot.json'), D('vincent/vincent_shoot_local.bin')];
const traces = [path.join(OUT, 'k_hits12.jsonl')];
const callsOf = f => f.replace(/\.jsonl$/, '.calls.jsonl');
if (![...need, ...traces, ...traces.map(callsOf)].every(existsSync)) { console.log('SKIP trace checks: data/ or traces missing (tools/byo/build_web.sh, tools/trace)'); done(); }
const json = p => JSON.parse(readFileSync(p, 'utf8'));
const f32 = p => { const b = readFileSync(p); return new Float32Array(b.buffer, b.byteOffset, b.byteLength / 4); };
const data = json(D('enemies/ai.json'));
if (!data.hitboxes || data.records[44].hp == null) { console.log('FAIL ai.json predates the hit export (rebuild data/enemies)'); fail++; done(); }
const W = json(D('aim/weapon.json'));
const e027 = json(D('enemies/e027.json')).local_pose, e027d = f32(D('enemies/e027_local.bin'));
const vin = json(D('vincent/vincent_shoot.json')).local_pose, vind = f32(D('vincent/vincent_shoot_local.bin'));
ok(data.records[44].hp === 1 && data.records[45].hp === 100, `max HP from the stage file: #66 ${data.records[44].hp}, #67 ${data.records[45].hp} (trace 1 / 100)`);
ok(data.hitboxes.e027.length === 13, `e027 part boxes: ${data.hitboxes.e027.length} (13 zone records)`);
ok(W.hit === 4 && W.pellets === 3 && W.damage === 30, `Vincent's W: hit ${W.hit}, pellets ${W.pellets}, damage ${W.damage} (save RAM: 4 / 3 / 30)`);
ok(Array.isArray(W.spread) && Math.abs(W.spread[0] / 1.028 - 4.66926) < 1e-4, `Vincent's spread W+0x1c ${W.spread?.[0]} / 1.028 = the logged cone ${(W.spread?.[0] / 1.028).toFixed(5)} (4.66926)`);

// near miss (FUN_0055bda8 -> FUN_0055df90): a straight pellet through #67's cylinder (radius 5) 4.5 u
// beside his axis at waist height crosses no part box: AI +0x70 + 1, no damage, no part
{
  const php = { pos: [0, 0, 0], hp: 780, id: 0x16, f118: 0, s64: 0 };
  const ai = createEnemyAI({ data, player: php, quiet: true, log: () => {} });
  const a = ai.addActor({ rec: 45, mode: 0, pos: [0, 0, 0], face: [0, 0, 1], clip: 'r_st_a' });
  const P0 = modelPose(e027, e027d, 'r_st_a', 0);
  ai.boneWorld = () => b => boneRows(P0.R[b], P0.S[b], P0.P[b], a.pos, a.face);
  const straight = { ...W, spread: null, pellets: 1 };
  ai.playerShot([4.5, -9, -60], [4.5, -9, 60], straight);
  for (let i = 0; i < 6; i++) ai.tick(1.2, [0, 0, 0], [0, 0, 0]);
  ok(a.ai.hits === 1 && a.hp === 100 && a.part === -1, `near miss: AI hits ${a.ai.hits} (1), HP ${a.hp} (100), part ${a.part} (-1)`);
  ai.playerShot([0, -9, -60], [0, -9, 60], straight);
  for (let i = 0; i < 6; i++) ai.tick(1.2, [0, 0, 0], [0, 0, 0]);
  ok(a.ai.hits === 2 && a.hp < 100 && a.part >= 0, `through the body: AI hits ${a.ai.hits} (2), HP ${a.hp} (< 100), part ${a.part}`);
}

// a soldier (record 45 = #67, HP 100) at pos facing +z in r_st_a frame 0, the pose for the part test
const P0 = modelPose(e027, e027d, 'r_st_a', 0);
function soldierAI(d = data, opts = {}) {
  const php = { pos: [0, 0, -200], hp: 780, id: 0x16, f118: 0, s64: 0 };
  const ai = createEnemyAI({ data: d, player: php, quiet: true, log: () => {}, ...opts });
  ai.boneWorld = a => b => boneRows(P0.R[b], P0.S[b], P0.P[b], a.pos, a.face);
  return ai;
}
const straight = { ...W, spread: null, pellets: 1 };
const shootThrough = (ai, z0 = -60, x = 0) => { ai.playerShot([x, -9, z0], [x, -9, z0 + 120], straight); for (let i = 0; i < 6; i++) ai.tick(1.2, [0, 0, -200], [0, 0, -200]); };

// zone records from the stage file (ai.json zones[record.zones], export_web_triggers.py zone_set):
// the values the save's RAM showed for #66 / #67 (notes §10.2)
{
  const z = data.zones && data.zones[data.records[45].zones];
  ok(!!z && z.length === 13 && z[0].scale === 0x50 && z[0].flags === 8 && z[9].scale === 0x0e && z[1].scale === 0x18 && z.slice(1).every(r => r.flags === 0),
     `e027 zones from the stage file (set ${data.records[45].zones}): ${z ? z.length : 'missing'} records; head / chest / part 9 as the save RAM showed (notes §10.2), only the head has flags (8)`);
}

// HP lives on the status: a deactivated and reactivated soldier keeps it (FUN_00314ea8 runs only at
// placement setup, FUN_00315050 / FUN_003151c8; FUN_00559748 does not write +0x20)
{
  const ai = soldierAI();
  ai.setActive(ai.objects.get(67), 1);
  const a = ai.actors.find(x => x.rec === 45);
  ok(!!a && a.hp === 100, `activation: #67 HP ${a && a.hp} (max HP 100)`);
  a.pos = [0, 0, 0]; a.face = [0, 0, 1];
  shootThrough(ai);
  const hp = a.hp;
  ai.setActive(a, 0);
  ok(ai.objects.get(67).kind === 'record' && ai.objects.get(67).hp === hp, `deactivated: the record keeps HP ${ai.objects.get(67).hp} (${hp})`);
  ai.setActive(ai.objects.get(67), 1);
  const b = ai.actors.find(x => x.rec === 45);
  ok(hp < 100 && !!b && b !== a && b.hp === hp, `reactivated: #67 HP ${b && b.hp} (kept ${hp}, not reset to 100)`);
}

// invulnerability gate (FUN_0055db50: actor +0x118 & 0x60 -> damage 0); the hit still counts
for (const bit of [0x20, 0x40]) {
  const ai = soldierAI();
  const a = ai.addActor({ rec: 45, mode: 0, pos: [0, 0, 0], face: [0, 0, 1], clip: 'r_st_a', f118: bit });
  shootThrough(ai);
  ok(a.hp === 100 && a.ai.hits === 1 && a.part >= 0, `+0x118 0x${bit.toString(16)}: HP ${a.hp} (100: no damage), AI hits ${a.ai.hits} (1), part ${a.part}`);
}

// zone flags (ushort +2): bit 1 = no reaction flags (no flinch), bit 2 = no stagger (no flinch, +0x148
// untouched), bit 3 = bullet +0x120 |= 0x60000. Control: flags 0 -> stagger 0x42 -> the flinch.
{
  const withFlags = fl => { const d = JSON.parse(JSON.stringify(data)); for (const r of d.zones[d.records[45].zones]) r.flags = fl; return d; };
  const run = fl => {
    const ai = soldierAI(withFlags(fl));
    const a = ai.addActor({ rec: 45, mode: 0, pos: [0, 0, 0], face: [0, 0, 1], clip: 'r_st_a' });
    ai.playerShot([0, -9, -60], [0, -9, 60], straight);
    const b = ai.bullets[ai.bullets.length - 1], ev = [];
    for (let i = 0; i < 6; i++) ev.push(...ai.tick(1.2, [0, 0, -200], [0, 0, -200]));
    return { a, b, flinch: ev.includes(`clip ${a.id} r_stlld0`) };
  };
  const c = run(0);
  ok(c.a.hp < 100 && c.flinch && c.a.s148 > 0 && !(c.b.f120 & 0x60000), `zone flags 0: hit (HP ${c.a.hp}), flinch ${c.flinch}, stagger ran, bullet +0x120 0x${c.b.f120.toString(16)}`);
  const z2 = run(2);
  ok(z2.a.hp < 100 && !z2.flinch && z2.a.s148 > 0, `zone flags 2: hit (HP ${z2.a.hp}), no flinch (${z2.flinch}), stagger still ran`);
  const z4 = run(4);
  ok(z4.a.hp < 100 && !z4.flinch && z4.a.s148 < 0, `zone flags 4: hit (HP ${z4.a.hp}), no stagger (+0x148 ${z4.a.s148}), no flinch (${z4.flinch})`);
  const z8 = run(8);
  ok((z8.b.f120 & 0x60000) === 0x60000, `zone flags 8: bullet +0x120 0x${z8.b.f120.toString(16)} (has 0x60000)`);
}

// two soldiers in the pellet's line: the closest along the segment is hit (FUN_005442d8), whichever
// was activated first; the other is not touched
for (const nearFirst of [false, true]) {
  const ai = soldierAI();
  const add = z => ai.addActor({ rec: z === 0 ? 45 : 44, mode: 0, pos: [0, 0, z], face: [0, 0, 1], clip: 'r_st_a', hp: 100 });
  const [near, far] = nearFirst ? [add(0), add(12)] : [add(12), add(0)].reverse();
  ai.playerShot([0, -9, -20], [0, -9, 100], straight);
  const segs = [];
  for (let i = 0; i < 6; i++) { const b = ai.bullets[0]; ai.tick(1.2, [0, 0, -200], [0, 0, -200]); if (b) segs.push([b.prev[2], b.pos[2]]); }
  const both = segs.some(([p, q]) => p < -5 && q > 17);
  ok(both && near.hp < 100 && near.ai.hits === 1 && far.hp === 100 && far.ai.hits === 0,
     `closest of two (${nearFirst ? 'near' : 'far'} soldier listed first; one segment crosses both: ${both}): near HP ${near.hp} hits ${near.ai.hits}, far HP ${far.hp} hits ${far.ai.hits}`);
}

// Other types (notes/enemy-ai-hook.md "Other types"): the same actor class, so the same hit path with
// the type's own data: part boxes (its 'mkd'), zone set, size (FUN_00563fc0: cylinder radius / height),
// HP, and its stance's clip rows (FUN_00564228). Shot dead in the head with one straight pellet each,
// posed in its own stand clip at frame 0. e048 record 28 (w003 class 3: stance 0x1f, r_*), record 24
// (class 2: stance 0x1e, m_*), e030 record 7 (stance 0x23, size 6 / 12).
for (const [k, model, stand, size, deathClip] of [[28, 'e048', 'r_st_a', [5, 18], 'r_stbf0'], [24, 'e048', 'm_st_a', [5, 18], 'm_stbf0'],
                                                  [7, 'e030', 'st_a', [6, 12], 'stbf0']]) {
  const rec = data.records[k];
  const sideP = D(`enemies/${model}.json`), binP = D(`enemies/${model}_local.bin`);
  if (!existsSync(sideP) || !existsSync(binP)) { console.log(`SKIP ${model}: data/enemies/${model}.* missing`); continue; }
  const lp = json(sideP).local_pose;
  if (!lp.clips[stand] || !data.stances) { ok(false, `${model} record ${k}: export has no ${stand} / ai.json no stances (rebuild data/enemies)`); continue; }
  const P = modelPose(lp, f32(binP), stand, 0);
  const php = { pos: [0, 0, -200], hp: 780, id: 0x16, f118: 0, s64: 0 };
  const ai = createEnemyAI({ data, player: php, quiet: true, log: () => {} });
  ai.boneWorld = a => b => boneRows(P.R[b], P.S[b], P.P[b], a.pos, a.face);
  const a = ai.addActor({ rec: k, mode: 0, pos: [0, 0, 0], face: [0, 0, 1] });
  ok(a.model === model && a.size.r === size[0] && a.size.h === size[1] && rec.hp > 1,
     `${model} #${k}: actor size ${a.size.r} / ${a.size.h} (save state ${size.join(' / ')}), HP ${a.hp}, stance 0x${a.C.st664.toString(16)}, idle ${a.C.clip}`);
  // head = part box 0 (posed, frame 0): soldiers: a pellet straight down from 30 u above onto the box
  // centre; e030: its head box (z 6.4) sits outside its 6 u cylinder, so a pellet that only crosses the
  // head fails the cylinder gate (FUN_0055bda8): shot from the front along its body axis instead
  const hb = data.hitboxes[model][0];
  const cs = boxCorners(hb, boneRows(P.R[hb.bone], P.S[hb.bone], P.P[hb.bone], a.pos, a.face));
  const head = [0, 1, 2].map(i => cs.reduce((t, c) => t + c[i], 0) / 8);
  const hp0 = a.hp;
  const [p0, p1] = model === 'e030' ? [[head[0], head[1], 60], [head[0], head[1], -60]] : [[head[0], head[1] - 30, head[2]], head];
  ai.playerShot(p0, p1, { ...straight, damage: 9999 });
  let died = null, settled = null;
  for (let i = 0; i < 200 && settled == null; i++) {
    ai.tick(1.2, [0, 0, -200], [0, 0, -200]);
    if (died == null && a.astate === 7) died = i;
    if (a.f118 & 0x100) settled = i;
  }
  ok(a.part === 0 && a.hp === 0 && died != null && a.ai.mode === 0 && a.C.clip === deathClip,
     `${model} #${k}: head shot (part ${a.part}, its own box 0 = bone ${hb.bone}) -> HP ${hp0} -> ${a.hp}, actor state 7 at tick ${died}, AI mode ${a.ai.mode}, death clip ${a.C.clip} (${deathClip})`);
  // settled bit when the death clip's clock (0.5 x 1.2 = 0.6 / frame) passes the clip's own length (model_clips)
  const n = data.model_clips[model][deathClip], want = Math.ceil(n / 0.6);
  ok(settled != null && Math.abs(settled - died - want) <= 1, `${model} #${k}: corpse settled ${settled - died} frames after the death (${deathClip} ${n} frames / 0.6 = ${want})`);
}
// e030 is shorter (height 12): a pellet 15 u up crosses the e027 / e048 cylinder (18) but not the e030's
{
  const php = { pos: [0, 0, -200], hp: 780, id: 0x16, f118: 0, s64: 0 };
  const ai = createEnemyAI({ data, player: php, quiet: true, log: () => {} });
  ai.boneWorld = a => b => boneRows(P0.R[b], P0.S[b], P0.P[b], a.pos, a.face);
  const dog = ai.addActor({ rec: 7, mode: 0, pos: [0, 0, 0], face: [0, 0, 1] });
  ai.playerShot([0, -15, -60], [0, -15, 60], straight);
  for (let i = 0; i < 6; i++) ai.tick(1.2, [0, 0, -200], [0, 0, -200]);
  ok(dog.ai.hits === 0, `e030 #7: a pellet 15 u up passes above its cylinder (height ${dog.size.h}): AI hits ${dog.ai.hits} (0)`);
}
// The flinch's end (class 0x1e update FUN_00598950): motion 0x40 with C+0x590 bit 24 (gun out) set,
// else motion 1. e030 (stance 0x23, no row 0x40, never in actor state 3: bit clear) goes back to row 1
// st_a, a real clip, and never asks for an empty row. Control: an e027 with the gun out -> r_st_a.
{
  const sideP = D('enemies/e030.json'), binP = D('enemies/e030_local.bin');
  if (!existsSync(sideP) || !existsSync(binP)) console.log('SKIP e030 flinch end: data/enemies/e030.* missing');
  else {
    const P = modelPose(json(sideP).local_pose, f32(binP), 'st_a', 0);
    const run = (k, gun, pose) => {
      const php = { pos: [0, 0, -200], hp: 780, id: 0x16, f118: 0, s64: 0 };
      const ai = createEnemyAI({ data, player: php, quiet: true, log: () => {} });
      ai.boneWorld = a => b => boneRows(pose.R[b], pose.S[b], pose.P[b], a.pos, a.face);
      const a = ai.addActor({ rec: k, mode: 0, pos: [0, 0, 0], face: [0, 0, 1] });
      if (gun === 1) a.C.gun = 1;
      ai.playerShot([0, -6, -60], [0, -6, 60], straight);
      const ev = [], clips = [];
      for (let i = 0; i < 300; i++) { if (gun === 0) a.C.gun = 0; if (gun === 1) a.C.gun = 1; ev.push(...ai.tick(1.2, [0, 0, -200], [0, 0, -200])); clips.push(a.C.clip); }
      return { a, ev, clips };
    };
    const r = run(7, 0, P), id = r.a.id, mine = r.ev.filter(e => e.startsWith(`clip ${id} `));
    const flinched = r.clips.indexOf('stld0');
    ok(r.a.hp > 0 && flinched >= 0 && r.a.C.clip === 'st_a' && r.clips.slice(flinched).includes('st_a') && !r.ev.includes(`clip ${id} null`),
       `e030 #7: non-lethal hit (HP ${r.a.hp}) -> flinch stld0 -> back to row 1 ${r.a.C.clip} (st_a); clip events ${mine.join(', ')}`);
    // unpinned: the hit wakes #67, actor state 3 sets the bit (GUESS), so the flinch's end asks row 0x40, never row 1
    const g = run(45, null, P0), ge = g.ev.filter(e => e.startsWith(`clip ${g.a.id} `)), gf = ge.indexOf(`clip ${g.a.id} r_stlld0`);
    ok(g.a.hp > 0 && gf >= 0 && g.a.C.gun === 1 && ge.indexOf(`clip ${g.a.id} r_gst`, gf) < 0 && g.a.C.clip === 'r_st_a',
       `e027 #67 woken (gun bit ${g.a.C.gun}): flinch -> row 0x40, no r_gst after it (events ${ge.join(', ')})`);
    // an empty row (e030 has no 0x40): with the bit forced on, no 'clip <id> null' event and the clip is kept
    const e = run(7, 1, P);
    ok(!e.ev.some(x => x.endsWith(' null')) && e.a.C.clip === 'stld0', `e030 #7 bit forced on: row 0x40 empty -> no null clip event, keeps ${e.a.C.clip}`);
    // the hit wakes #67 (actor state 3 sets the bit); held clear here: the clip after the flinch is row 1
    const n = run(45, 0, P0), ne = n.ev.filter(e => e.startsWith(`clip ${n.a.id} `)), nf = ne.lastIndexOf(`clip ${n.a.id} r_stlld0`);
    // (the brain's state-3 request r_st_a lands while the flinch still runs; the flinch's end then asks row 1)
    const after = nf >= 0 && ne.indexOf(`clip ${n.a.id} r_gst`, nf) > nf;
    ok(n.a.hp > 0 && after, `e027 #67 gun held clear: flinch end asks row 1 r_gst (events ${ne.join(', ')})`);
  }
}

// the shared RNG in the game's frame order (FUN_00323128): AI scheduler FUN_00365008 (the fire timer's
// jitter draw, FUN_003656a0) -> player tick FUN_005747a0 (Vincent's cone draws, FUN_00577480) ->
// object lists (bullets: the flinch draw, FUN_00561718). The page queues the shot (queueShot) and
// tick fires it after the scheduler.
{
  const order = [];
  const label = () => { const st = new Error().stack; return /at cone /.test(st) ? 'cone' : /at fireTimers /.test(st) ? 'jitter' : /at flinchClip /.test(st) ? 'flinch' : 'other'; };
  const ai = soldierAI(data, { rand: () => { order.push(label()); return 0.3; } });
  const a = ai.addActor({ rec: 45, mode: 0, pos: [0, 0, 0], face: [0, 0, 1], clip: 'r_st_a' });
  const timer = () => ai.S.timers.push({ due: ai.S.now, ai: a.ai, left: 5, period: 20 });   // due this tick: one jitter draw (period > 12)
  timer();
  ai.queueShot([0, -9, -60], [0, -9, 60], { ...W, pellets: 1 });
  ok(ai.bullets.length === 0, 'queueShot: no bullet before the tick');
  ai.tick(1.2, [0, 0, -200], [0, 0, -200]);
  const f0 = order.filter(x => x !== 'other');
  ok(f0.length === 3 && f0[0] === 'jitter' && f0[1] === 'cone' && f0[2] === 'cone',
     `shot frame: draws ${f0.join(', ')} (jitter, cone, cone: the scheduler before the player's shot)`);
  // the frame the pellet hits: the scheduler's jitter before the flinch's draw
  let hitFrame = null;
  for (let i = 0; i < 6 && hitFrame == null; i++) {
    order.length = 0;
    ai.S.timers.length = 0; a.ai.timer = null; timer();
    ai.tick(1.2, [0, 0, -200], [0, 0, -200]);
    if (order.includes('flinch')) hitFrame = order.filter(x => x !== 'other');
  }
  ok(!!hitFrame && hitFrame.indexOf('jitter') >= 0 && hitFrame.indexOf('jitter') < hitFrame.indexOf('flinch'),
     `hit frame: draws ${hitFrame ? hitFrame.join(', ') : 'none'} (jitter before flinch)`);
}

const fp = h => { const b = Buffer.from(h, 'hex'); return { face: [0, 1, 2].map(i => b.readFloatLE(4 * i)), pos: [4, 5, 6].map(i => b.readFloatLE(4 * i)) }; };
const u32h = h => Buffer.from(h, 'hex').readUInt32LE(0);
const posOf = r => r.matrix[3].slice(0, 3);

function replay(file) {
  const lines = readFileSync(file, 'utf8').trim().split('\n').map(JSON.parse);
  const recs = lines.slice(1), at = new Map(recs.map(r => [r.frame, r]));
  const name = path.basename(file, '.jsonl');
  const php = { pos: posOf(recs[0]), hp: recs[0].p_hp, id: 0x16, f118: 0, s64: 0 };
  // the call log: per frame, the FUN_0025a058 draws ('rand') and outputs ('cone'); its frame = record
  // frame + 1 (logged after the frame's logic)
  const calls = readFileSync(callsOf(file), 'utf8').trim().split('\n').map(JSON.parse).filter(c => c.hook);
  const draws = [];
  const ai = createEnemyAI({ data, player: php, quiet: true, log: () => {}, now: recs[0].ai_now,
                             rand: () => draws.length ? draws.shift() : 0.25 });   // GUESS for draws outside the shots: not compared
  const S = [{ k: 0, rec: 44 }, { k: 1, rec: 45 }].map(s => {
    const r0 = recs[0], b = fp(r0[`e${s.k}_face_pos`]);
    const a = ai.addActor({ rec: s.rec, mode: r0[`e${s.k}_ai_mode`], f118: 3, target: php, pos: b.pos, face: b.face,
                            astate: r0[`e${s.k}_astate`] & 0xff, clip: r0[`e${s.k}_motion`] });
    return { ...s, a, open: true, clip: null, frame: 0, start: r0.frame, log: [] };
  });
  // the drawn pose each soldier's part test reads (enemies.js tick: pose = this frame's clip frame,
  // then frame += 0.6; a new clip starts at 0; the death clip holds)
  const drawn = new Map();
  ai.boneWorld = a => { const d = drawn.get(a.rec); return d ? b => boneRows(d.R[b], d.S[b], d.P[b], d.pos, d.face) : null; };
  const pose = s => {
    const clip = s.open ? recClip(s) : s.a.C.clip;
    if (clip !== s.clip) { s.clip = clip; s.frame = s.open ? (curFrame - s.start) * 0.6 : 0; }
    const n = data.clips[clip] ?? e027.clips[clip].frames;
    s.frame = s.frame % n;
    const P = modelPose(e027, e027d, clip, s.frame);
    drawn.set(s.rec, { ...P, pos: s.a.pos.slice(), face: s.a.face.slice() });
    s.frame = Math.fround(s.frame + Math.fround(0.5 * 1.2));
    if (n <= s.frame) s.frame = s.a.C.hold ? n - 1 : 0;
  };
  let curFrame = recs[0].frame;
  const recClip = s => at.get(curFrame)[`e${s.k}_motion`];
  const out = [];
  for (let i = 0; i + 1 < recs.length; i++) {
    const r = recs[i], nx = recs[i + 1];
    curFrame = r.frame;
    for (const s of S) {
      if (!s.open) continue;
      const b = fp(r[`e${s.k}_face_pos`]);
      if (r[`e${s.k}_motion`] !== (i ? recs[i - 1][`e${s.k}_motion`] : null)) s.start = r.frame;
      s.a.pos = b.pos; s.a.face = b.face; s.a.C.clip = r[`e${s.k}_motion`];
    }
    // Vincent's shot this frame (R1 edge, h_st_f next): muzzle = j56 of his clip, aim = P+0x2b0 after the frame
    const r1 = (r.pad_in.buttons & 8) && !(i && (recs[i - 1].pad_in.buttons & 8));
    if (r1 && nx.motion === 'h_st_f' && r.motion !== 'h_st_f') {
      const vp = modelPose(vin, vind, 'h_st_a', 0);
      const bone = b => boneRows(vp.R[b], vp.S[b], vp.P[b], posOf(r), r.matrix[2].slice(0, 3))[3];
      const log = calls.filter(c => c.frame === r.frame + 1);
      draws.length = 0; draws.push(...log.filter(c => c.hook === 'rand').map(c => c.f0));
      const n0 = ai.bullets.length;
      ai.playerShot(bone(56), nx.p_aim.slice(0, 3), W, bone(5));
      const want = log.filter(c => c.hook === 'cone').map(c => c['a1+0x0'].f.slice(0, 3));
      const got = ai.bullets.slice(n0).map(b => b.dir);
      const ang = got.map((d, k) => want[k] ? Math.acos(Math.min(1, d[0] * want[k][0] + d[1] * want[k][1] + d[2] * want[k][2])) * 180 / Math.PI : NaN);
      out.push({ shot: r.frame, pellets: got.length, logged: want.length, ang, left: draws.length });
    }
    php.pos = posOf(nx);
    const hpBefore = S.map(s => s.a.hp);
    ai.tick(r.step, posOf(r), posOf(nx));
    S.forEach((s, j) => { if (s.open && (s.a.hp !== hpBefore[j] || s.a.part >= 0)) s.open = false; });
    for (const s of S) pose(s);
    // sim after frame r = record nx
    for (const s of S) {
      const a = s.a, g = fp(nx[`e${s.k}_face_pos`]);
      s.log.push({ frame: nx.frame,
        sim: { hp: a.hp, part: a.part, astate: a.astate, mode: a.ai.mode, clip: a.C.clip, hits: a.ai.hits, pos: a.pos.slice(), settled: !!(a.f118 & 0x100) },
        game: { hp: nx[`e${s.k}_hp`], part: (nx[`e${s.k}_part`] >>> 16) & 0xff, astate: nx[`e${s.k}_astate`] & 0xff, mode: nx[`e${s.k}_ai_mode`],
                clip: nx[`e${s.k}_motion`], hits: nx[`e${s.k}_ai_hits`] & 0xffff, pos: g.pos, settled: !!(u32h(nx[`e${s.k}_target`]) & 0x100) } });
    }
  }
  return { name, S, shots: out };
}

const first = (log, fn) => { const e = log.find(fn); return e ? e.frame : null; };
for (const file of traces) {
  const { name, S, shots } = replay(file);
  ok(shots.length > 0, `${name}: ${shots.length} shots replayed (${shots.map(s => s.shot).join(', ')})`);
  for (const sh of shots) {
    ok(sh.pellets === 3 && sh.logged === 3 && sh.left === 0, `${name} f${sh.shot}: ${sh.pellets} pellets, ${sh.logged} logged cones, all 6 draws used (${sh.left} left)`);
    ok(sh.ang.every(a => a < 1.5), `${name} f${sh.shot}: pellet directions vs the logged cones ${sh.ang.map(a => a.toFixed(3)).join(' / ')} deg (< 1.5)`);
    gap(sh.ang.every(a => a < 0.1), `${name} f${sh.shot}: pellet directions within 0.1 deg of the game's (the muzzle pose j56 / j05 in h_st_a frame 0 is a GUESS)`);
  }
  for (const s of S) {
    const L = s.log, who = `${name} #${s.rec + 22}`;
    const hitG = first(L, e => e.game.hits > (L[0].game.hits)), hitS = first(L, e => e.sim.hits > (L[0].sim.hits));
    if (hitG == null) { ok(hitS == null, `${who}: not hit in the game, not hit here (${hitS ?? 'none'})`); continue; }
    ok(hitS === hitG, `${who}: first hit at ${hitS} (game ${hitG})`);
    const hpBad = L.filter(e => e.sim.hp !== e.game.hp);
    const only25 = hpBad.every(e => e.game.hp === 75 && e.sim.hp === 76);
    if (hpBad.length && only25) gap(false, `${who}: HP per frame: ${hpBad.length} frame(s) 76 vs game 75 (limb hit 24 vs 25: the +1 of notes §10.2)`);
    else ok(hpBad.length === 0, `${who}: HP per frame equal${hpBad.length ? ` (first diff ${hpBad[0].frame}: ${hpBad[0].sim.hp} vs ${hpBad[0].game.hp})` : ''}`);
    const parts = L.filter(e => e.game.hits !== (L[L.indexOf(e) - 1] ?? L[0]).game.hits).map(e => [e.frame, e.game.part, e.sim.part]);
    ok(parts.every(([, g, x]) => g === x), `${who}: part per hit ${parts.map(p => `${p[0]}:${p[2]}/${p[1]}`).join(' ')} (sim/game)`);
    const dG = first(L, e => e.game.astate === 7), dS = first(L, e => e.sim.astate === 7);
    ok(dS === dG, `${who}: death (actor state 7) at ${dS} (game ${dG})`);
    const mG = first(L, e => e.frame >= hitG && e.game.mode === 0), mS = first(L, e => e.frame >= hitG && e.sim.mode === 0);
    ok(mS === mG, `${who}: AI mode 0 (FUN_0035f728) at ${mS} (game ${mG})`);
    const hitsEnd = L[L.length - 1];
    ok(hitsEnd.sim.hits === hitsEnd.game.hits, `${who}: AI +0x70 hits at the end ${hitsEnd.sim.hits} (game ${hitsEnd.game.hits}; no count after the death)`);
    const clipBad = L.filter(e => e.frame >= hitG && e.frame < hitG + 20 && e.sim.clip !== e.game.clip);
    ok(clipBad.length === 0, `${who}: clip after the hit (20 frames)${clipBad.length ? `: ${clipBad[0].frame} ${clipBad[0].sim.clip} vs ${clipBad[0].game.clip}` : ` ${L.find(e => e.frame === hitG).sim.clip}`}`);
    const sG = first(L, e => e.game.settled), sS = first(L, e => e.sim.settled);
    if (sG != null || sS != null) ok(sS === sG, `${who}: corpse settled (+0x118 0x100) at ${sS} (game ${sG})`);
    // positions: push-back / death slide, per frame from the hit
    const pe = L.filter(e => e.frame >= hitG).map(e => ({ f: e.frame, d: Math.hypot(e.sim.pos[0] - e.game.pos[0], e.sim.pos[2] - e.game.pos[2]) }));
    const n = s.rec === 44 ? 4 : pe.length, worst = pe.slice(0, n).reduce((m, e) => e.d > m.d ? e : m, { d: 0 });
    ok(worst.d < 0.05, `${who}: position per frame over ${n} frames from the hit, max ${worst.d.toFixed(4)} (f${worst.f ?? '-'})`);
    if (s.rec === 44) {
      const end = pe[pe.length - 1];
      gap(end.d < 0.05, `${who}: death slide end ${end.d.toFixed(2)} u off (the game's corpse meets a wall at ~f849; no enemy collision here)`);
    }
  }
}
done();
