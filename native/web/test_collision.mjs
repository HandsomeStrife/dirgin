// test_collision.mjs -- parity test: collision.js vs native/assets/collision/cid_query.py.
//
//   node native/web/test_collision.mjs [path/to/collision.js] [path/to/z128_collision.bin]
//
// Runs cid_query.py (unchanged, imported by an inline helper) on the same
// queries and compares: groundY (random XZ x several feetY, the 3 known spots,
// points near the start), capsule pushes, move() over the self-test's 8-direction
// walk plus random walks, and segments. Exit code 1 on any mismatch.
import { readFileSync, writeFileSync, unlinkSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const modPath = path.resolve(process.argv[2] ?? path.join(here, 'collision.js'));
const binPath = path.resolve(process.argv[3] ?? path.join(here, 'data/z128_collision.bin'));
const pyDir = process.env.COLLISION_PY_DIR ?? path.join(root, 'native/assets/collision');   // override: mutation runs
const TOL = 1e-4;

const { Collision } = await import(pathToFileURL(modPath).href);
const buf = readFileSync(binPath);
let t0 = performance.now();
const col = new Collision(buf);
const loadMs = performance.now() - t0;

// ---------------------------------------------------------------- queries
let seed = 0x0049;
const rnd = () => ((seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0) / 4294967296);
const dv = new DataView(buf.buffer, buf.byteOffset);
const bmin = [0, 1, 2].map(i => dv.getFloat32(0x10 + 4 * i, true));
const bmax = [0, 1, 2].map(i => dv.getFloat32(0x1c + 4 * i, true));
const lerp = (a, b, t) => a + (b - a) * t;

const SPOTS = [['hero 163235', -1395.64, -1110.46, -98.77],
               ['walk 185138 h_st_94', -1395.70, -1110.27, -98.66],
               ['walk 185126 h_st_160', -1364.20, -1097.06, -98.67]];
const [, X0, Z0, FY0] = SPOTS[0];

const ground = [];                                   // [x, z, feetY|null]
const FEET = [null, -98.77, -40, -160, -300, 60];
for (let i = 0; i < 400; i++) {
  const x = lerp(bmin[0], bmax[0], rnd()), z = lerp(bmin[2], bmax[2], rnd());
  for (const fy of FEET) ground.push([x, z, fy]);
}
for (let i = 0; i < 300; i++)                        // dense near the start
  ground.push([X0 + (rnd() - 0.5) * 400, Z0 + (rnd() - 0.5) * 400, lerp(-130, -60, rnd())]);
for (const [, x, z, fy] of SPOTS) { ground.push([x, z, fy]); ground.push([x, z, null]); }
// Slopes near the floor threshold (0.45 <= |n.y| < 0.75, both windings): centroids,
// feet just above. Picked from the file itself with an inline copy of the sub-tree-1
// player-set rule (the game's packet test over the three probe flag words,
// mask & ~(f|0x100) != 0, i.e. mask & 0xfe7e), not from the module under test.
{
  const nv = dv.getUint32(8, true), nt = dv.getUint32(12, true);
  const V = new Float32Array(buf.buffer.slice(buf.byteOffset + 0x30, buf.byteOffset + 0x30 + nv * 12));
  const I = new Uint32Array(buf.buffer.slice(buf.byteOffset + 0x30 + nv * 12, buf.byteOffset + 0x30 + nv * 12 + nt * 12));
  const F = new Uint32Array(buf.buffer.slice(buf.byteOffset + 0x30 + nv * 12 + nt * 16, buf.byteOffset + 0x30 + nv * 12 + nt * 20));
  const picks = [];
  for (let t = 0; t < nt; t++) {
    if ((F[t] & 3) !== 1 || !((F[t] >>> 16) & 0xfe7e)) continue;
    const a = I[3 * t] * 3, b = I[3 * t + 1] * 3, c = I[3 * t + 2] * 3;
    const ux = V[b] - V[a], uy = V[b + 1] - V[a + 1], uz = V[b + 2] - V[a + 2];
    const wx = V[c] - V[a], wy = V[c + 1] - V[a + 1], wz = V[c + 2] - V[a + 2];
    const nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
    const l = Math.hypot(nx, ny, nz);
    if (l > 1e-9 && Math.abs(ny / l) >= 0.45 && Math.abs(ny / l) < 0.75)
      picks.push([(V[a] + V[b] + V[c]) / 3, (V[a + 2] + V[b + 2] + V[c + 2]) / 3, (V[a + 1] + V[b + 1] + V[c + 1]) / 3 - 2]);
  }
  for (let i = 0; i < 300 && picks.length; i++) ground.push(picks[Math.floor(rnd() * picks.length)]);
}

const pushes = [];                                   // [x, feetY, z]
for (let i = 0; i < 300; i++)
  pushes.push([X0 + (rnd() - 0.5) * 300, lerp(-110, -90, rnd()), Z0 + (rnd() - 0.5) * 300]);

const walks = [];                                    // {x,y,z, deltas:[[dx,dz]...]}
// Starts: centroids of walk polygons within 400 of the spawn (the walker only moves on them).
const nearPolys = [];
for (let i = 0; i < col.npolys; i++) {
  const vs = col.polys[i].v, c = [0, 1, 2].map(k => vs.reduce((a, v) => a + v[k], 0) / vs.length);
  if (Math.hypot(c[0] - X0, c[2] - Z0) < 400) nearPolys.push(c);
}
for (let w = 0; w < 30; w++) {
  const [x, cy, z] = nearPolys[Math.floor(rnd() * nearPolys.length)];
  const y = col.groundY(x, z, cy);                  // start on its polygon (same answer checked above)
  if (y === null) continue;
  let a = rnd() * 2 * Math.PI;
  const deltas = [];
  for (let s = 0; s < 120; s++) {
    a += (rnd() - 0.5) * 0.6;
    const sp = lerp(0.5, 2.0, rnd());
    deltas.push([Math.cos(a) * sp, Math.sin(a) * sp]);
  }
  walks.push({ x, y, z, deltas, air: w % 3 === 2 });   // every third walk: the airborne ground-point walk
}

// Airborne off the mesh (outside the file's bbox): the body must stay put.
walks.push({ x: bmin[0] - 50, y: -100, z: bmin[2] - 50, deltas: [[1, 0], [0.5, 0.5], [0, -1.6]], air: true });

// Raw walk polygons, read from the file (not the module under test).
const RAWP = (() => {
  const nv = dv.getUint32(8, true), nt = dv.getUint32(12, true);
  const po = 0x30 + nv * 12 + nt * 20, np = dv.getUint32(po, true), out = [];
  const V = i => [0, 1, 2].map(k => dv.getFloat32(0x30 + i * 12 + k * 4, true));
  for (let q = 0; q < np; q++) {
    const w = po + 8 + q * 40, w0 = dv.getUint32(w, true), vs = [];
    for (let a = 0; a < (w0 & 0xffff); a++) vs.push(V(dv.getUint32(w + 8 + a * 4, true)));
    out.push({ q, flags: w0 >>> 16, file: dv.getUint32(w + 4, true) >>> 16,
               c: [0, 1, 2].map(k => vs.reduce((s, v) => s + v[k], 0) / vs.length) });
  }
  return out;
})();
// block80 (bodies with flag 0x200): the 0x80 polygons are walls, so the polygon
// searches must not stand the body on one. Ground queries at their centroids.
const b80 = RAWP.filter(P => P.flags & 0x80).map(P => [P.c[0], P.c[2], P.c[1]]);
// Parity across the 0x02 seams: from each 0x02 polygon straight toward the nearest
// 0x02 polygon of another file, 0.5 a step, ground and airborne.
{
  const seam = RAWP.filter(P => P.flags & 0x02);
  for (const P of seam) {
    let T = null, bd = Infinity;
    for (const Q of seam) if (Q.file !== P.file) { const d = Math.hypot(Q.c[0] - P.c[0], Q.c[2] - P.c[2]); if (d < bd) { bd = d; T = Q; } }
    if (!T) continue;
    const n = Math.ceil(bd / 0.5) + 2, dx = (T.c[0] - P.c[0]) / bd * 0.5, dz = (T.c[2] - P.c[2]) / bd * 0.5;
    for (const air of [false, true])
      walks.push({ x: P.c[0], y: P.c[1], z: P.c[2], deltas: Array.from({ length: n }, () => [dx, dz]), air });
  }
}

const segs = [];                                     // [[ax,ay,az],[bx,by,bz]]
for (let i = 0; i < 300; i++) {
  const a = [X0 + (rnd() - 0.5) * 300, lerp(-140, -60, rnd()), Z0 + (rnd() - 0.5) * 300];
  const b = [a[0] + (rnd() - 0.5) * 60, a[1] + (rnd() - 0.5) * 60, a[2] + (rnd() - 0.5) * 60];
  segs.push([a, b]);
}
segs.push([[X0, FY0 - 10, Z0], [X0, FY0 + 10, Z0]]);

// ---------------------------------------------------------------- python
const PY = String.raw`
import json, math, sys
sys.path.insert(0, sys.argv[1])
import cid_query
col = cid_query.Collision(sys.argv[2])
q = json.load(sys.stdin)
def move(x, y, z, dx, dz, air=False):
    return list(col.move(x, y, z, dx, dz, air=air))
out = {'ntris': len(col.tris), 'ncells': len(col.grid), 'npolys': len(col.polys), 'npcells': len(col.pgrid)}
out['ground'] = [(lambda g: None if g is None else g[0])(col.ground_height(x, z, fy)) for x, z, fy in q['ground']]
out['push'] = [list(col.capsule_push(x, fy, z)) for x, fy, z in q['pushes']]
# the self-test's 8-direction walk, exactly as _selftest runs it
sw = []
x0, z0, fy0 = q['spot']
for k in range(8):
    a = k * math.pi / 4
    c, s = math.cos(a), math.sin(a)
    x, z, y = x0, z0, fy0
    steps = []
    for _ in range(200):
        r = move(x, y, z, c, s)
        steps.append(r)
        if not r[3]:
            break
        if math.hypot(r[0] - x, r[2] - z) < 0.05:
            break
        x, y, z = r[0], r[1], r[2]
    sw.append({'c': c, 's': s, 'steps': steps})
out['selfwalk'] = sw
rw = []
for w in q['walks']:
    x, y, z = w['x'], w['y'], w['z']
    steps = []
    for dx, dz in w['deltas']:
        r = move(x, y, z, dx, dz, w['air'])
        steps.append(r)
        x, y, z = r[0], r[1], r[2]
    rw.append(steps)
out['walks'] = rw
def seg(a, b):
    h = col.segment_hit(a, b)
    return None if h is None else [h[0], list(h[1]), list(h[2])]
out['segs'] = [seg(a, b) for a, b in q['segs']]
col80 = cid_query.Collision(sys.argv[2], block80=True)
out['b80'] = [(lambda g: None if g is None else [g[0], g[3]])(col80.ground_height(x, z, fy)) for x, z, fy in q['b80']]
out['b80move'] = [list(col80.move(x, fy, z, 0.5, 0.0, max_drop=4.0)) for x, z, fy in q['b80']]
json.dump(out, sys.stdout)
`;
t0 = performance.now();
const pr = spawnSync('python3', ['-I', '-c', PY, pyDir, binPath], {
  input: JSON.stringify({ ground, pushes, walks, segs, b80, spot: [X0, Z0, FY0] }),
  maxBuffer: 1 << 28, encoding: 'utf8',
});
if (pr.status !== 0) { console.error(pr.stderr); process.exit(2); }
const py = JSON.parse(pr.stdout);
const pyMs = performance.now() - t0;

// ---------------------------------------------------------------- compare
const bad = { ground: 0, push: 0, selfwalk: 0, walks: 0, segs: 0, b80: 0, abs: 0 };
let maxErr = 0;
const close = (a, b) => {
  if (a === null || b === null) return a === b;
  const e = Math.abs(a - b); if (e > maxErr) maxErr = e; return e <= TOL;
};
const sameMove = (j, p) => close(j.x, p[0]) && close(j.y, p[1]) && close(j.z, p[2])
  && j.onGround === p[3] && j.hitWall === p[4];
const examples = [];
const note = (kind, msg) => { bad[kind]++; if (examples.length < 8) examples.push(`${kind}: ${msg}`); };

ground.forEach(([x, z, fy], i) => {
  const j = col.groundY(x, z, fy ?? undefined);
  if (!close(j, py.ground[i])) note('ground', `(${x}, ${z}, ${fy}) js ${j} py ${py.ground[i]}`);
});
pushes.forEach(([x, fy, z], i) => {
  const [jx, jz, jh] = col.capsulePush(x, fy, z), p = py.push[i];
  if (!(close(jx, p[0]) && close(jz, p[1]) && jh === p[2])) note('push', `(${x}, ${fy}, ${z}) js ${[jx, jz, jh]} py ${p}`);
});
let moveCalls = 0;
const selfTravel = [];
py.selfwalk.forEach((w, k) => {                      // drive JS with the same rule, compare every step
  let pos = { x: X0, y: FY0, z: Z0 }, travelled = 0, n = 0;
  for (let s = 0; s < 200; s++) {
    const r = col.move(pos, { x: w.c, z: w.s }); moveCalls++; n++;
    const p = w.steps[s];
    if (!p || !sameMove(r, p)) { note('selfwalk', `dir ${k * 45} step ${s}: js ${JSON.stringify(r)} py ${JSON.stringify(p)}`); break; }
    if (!r.onGround) break;
    const step = Math.hypot(r.x - pos.x, r.z - pos.z);
    if (step < 0.05) break;
    travelled += step; pos = r;
  }
  if (n !== w.steps.length) note('selfwalk', `dir ${k * 45}: js ${n} steps, py ${w.steps.length}`);
  selfTravel.push(`${k * 45}:${travelled.toFixed(1)}`);
});
walks.forEach((w, i) => {
  let pos = { x: w.x, y: w.y, z: w.z };
  for (let s = 0; s < w.deltas.length; s++) {
    const r = col.move(pos, { x: w.deltas[s][0], z: w.deltas[s][1] }, { air: w.air }); moveCalls++;
    if (!sameMove(r, py.walks[i][s])) { note('walks', `walk ${i} step ${s}: js ${JSON.stringify(r)} py ${JSON.stringify(py.walks[i][s])}`); break; }
    pos = r;
  }
});
segs.forEach(([a, b], i) => {
  const j = col.segment({ x: a[0], y: a[1], z: a[2] }, { x: b[0], y: b[1], z: b[2] }), p = py.segs[i];
  const ok = (j === null || p === null) ? j === p
    : close(j.t, p[0]) && close(j.point.x, p[1][0]) && close(j.point.y, p[1][1]) && close(j.point.z, p[1][2])
      && close(j.normal[0], p[2][0]) && close(j.normal[1], p[2][1]) && close(j.normal[2], p[2][2]);
  if (!ok) note('segs', `${JSON.stringify([a, b])} js ${JSON.stringify(j)} py ${JSON.stringify(p)}`);
});
const col80 = new Collision(buf, { block80: true });
b80.forEach(([x, z, fy], i) => {
  const g = col80.groundHit(x, z, fy), p = py.b80[i];
  const ok = (g === null || p === null) ? g === p : close(g.y, p[0]) && g.poly === p[1];
  if (!ok) note('b80', `ground (${x}, ${z}, ${fy}) js ${g && [g.y, g.poly]} py ${p}`);
  const r = col80.move({ x, y: fy, z }, { x: 0.5, z: 0 }, { maxDrop: 4 }); moveCalls++;
  if (!sameMove(r, py.b80move[i])) note('b80', `move (${x}, ${z}, ${fy}) js ${JSON.stringify(r)} py ${JSON.stringify(py.b80move[i])}`);
});

// ---------------------------------------------------------------- absolute checks
// Behaviour against the game data and Dan's reports, not against the Python
// (which shares the rules). Worker 0061: walking is the walk-mesh walker
// (sub-tree-0 polygons, FUN_00357d48). Moves use play.js's options (maxDrop 4).
const PLAY = { radius: 3.5, height: 19, maxDrop: 4 };
const ABS2 = [];
const DECK = -98.64;                                   // walk polygon under the spawn
for (const [name, x, z, fy] of SPOTS) {                // Vincent's dumped feet vs the walk polygon
  const g = col.groundY(x, z, fy);
  ABS2.push([`dump feet ${name}`, g !== null && Math.abs(g - fy) < 0.4, `feet ${fy} ground ${g}`]);
}
// Walk n steps of (dx,dz); a drop (onGround false) falls like player.js to the
// first polygon below. -> {end, minY, maxY, fell, void, hits}
const walkFrom = (x, z, fy, dx, dz, n) => {
  let p = { x, y: col.groundY(x, z, fy) ?? fy, z }, minY = p.y, maxY = p.y, fell = false, vd = false, hits = 0;
  for (let s = 0; s < n; s++) {
    let r = col.move(p, { x: dx, z: dz }, PLAY);
    if (!r.onGround) {
      fell = true;
      const g = col.groundY(r.x, r.z, p.y, 0);
      if (g === null) { vd = true; break; }
      r = { x: r.x, y: g, z: r.z };
    }
    if (r.hitWall) hits++;
    p = r; minY = Math.min(minY, p.y); maxY = Math.max(maxY, p.y);
  }
  return { end: p, minY, maxY, fell, void: vd, hits };
};
const f2 = (v) => v.toFixed(2);
{ // report 20261026T175840Z-ae9d "collision missing on the bars": he walked through the
  // north railing onto the 45-degree roof (feet -96.46 at z -1083.5). The deck's walk
  // polygon ends at z -1094.8: walking N, NE, NW he must stop there, on the deck, sliding.
  for (const [dx, dz, name] of [[0, 1, 'N'], [0.7, 0.7, 'NE'], [-0.7, 0.7, 'NW']]) {
    const w = walkFrom(-1400.26, -1100, DECK, dx, dz, 40);
    ABS2.push([`report ae9d bars, walk ${name}`, w.end.z <= -1094.7 && Math.abs(w.maxY - DECK) < 0.05 && !w.fell && w.hits > 0,
      `end (${f2(w.end.x)}, ${f2(w.end.y)}, ${f2(w.end.z)}) maxY ${f2(w.maxY)} fell ${w.fell} slides ${w.hits}`]);
  }
  for (const [dx, dz, name, ok] of [[-1, 0, 'W', (e) => e.x >= -1418.0], [0, -1, 'S', (e) => e.z >= -1113.5]]) {
    const w = walkFrom(-1400, -1105, DECK, dx, dz, 40);
    ABS2.push([`deck railing, walk ${name}`, ok(w.end) && Math.abs(w.maxY - DECK) < 0.05 && !w.fell && w.hits > 0,
      `end (${f2(w.end.x)}, ${f2(w.end.y)}, ${f2(w.end.z)}) fell ${w.fell} slides ${w.hits}`]);
  }
}
{ // reports 20261026T175806Z-5f40 / 20261026T191925Z-7c95 "fell through the pipes" (feet
  // -71.08 / -72.77 on the roof under them). The walk mesh lays a flat strip over the pipes
  // (y -94.4, z -1110.8..-1106.6). Under both report spots the ground must be that strip,
  // and walking across it (+z, -z) and along it (+x) he must stay on it.
  for (const [name, x, z] of [['5f40', -1339.87, -1108.92], ['7c95', -1269.82, -1107.13]]) {
    const g = col.groundY(x, z, -94.4);
    ABS2.push([`report ${name} pipes: ground is the strip`, g !== null && Math.abs(g + 94.4) < 0.3, `ground ${g}`]);
  }
  for (const [dx, dz, n, name] of [[0, 1, 30, '+z'], [0, -1, 30, '-z'], [1, 0, 120, '+x']]) {
    const w = walkFrom(-1300, -1108.2, -94.4, dx, dz, n);
    const onStrip = w.end.z >= -1110.9 && w.end.z <= -1105.8;     // edges: z -1110.8..-1110.3 and -1106.6..-1105.9
    ABS2.push([`report 5f40/7c95 pipes, walk ${name}`, w.maxY < -90 && !w.fell && (name === '+x' || (onStrip && w.hits > 0)),
      `end (${f2(w.end.x)}, ${f2(w.end.y)}, ${f2(w.end.z)}) maxY ${f2(w.maxY)} fell ${w.fell} slides ${w.hits}`]);
  }
  const e = walkFrom(X0, Z0, DECK, 1, 0, 250);         // spawn east over the pipe walkway
  ABS2.push(['walkway: spawn east over the pipes', e.end.x > -1210 && e.maxY < -90 && !e.fell,
    `end (${f2(e.end.x)}, ${f2(e.end.y)}) maxY ${f2(e.maxY)} fell ${e.fell}`]);
}
{ // report 20261026T192011Z-a1a5 "jumped over the railings onto the street" (feet y 0 at
  // (-1291.65, -1143.41)). In the game the airborne ground point walks the same mesh
  // (FUN_003590c8 state 2 -> FUN_00357d48), so a jump ends on the deck. Jump (0057's
  // numbers: -1.333 up, gravity 0.07/tick, no air control) at run speed 1.6 from the
  // middle of the deck toward the south, west and north railings.
  for (const [dx, dz, name] of [[0, -1, 'S'], [-1, 0, 'W'], [0, 1, 'N'], [0.7, -0.7, 'SE'], [-0.7, -0.7, 'SW']]) {
    let p = { x: -1395, y: DECK, z: -1104 }, vy = -1.333, landed = null;
    for (let t = 0; t < 120 && !landed; t++) {
      const r = col.move(p, { x: dx * 1.6, z: dz * 1.6 }, { air: true });
      const g = col.groundY(r.x, r.z, p.y, 0);
      let y = p.y + vy; vy += 0.07;
      if (g !== null && vy > 0 && y >= g) { y = g; landed = { x: r.x, y, z: r.z }; }
      p = { x: r.x, y, z: r.z };
    }
    const ok = landed && Math.abs(landed.y - DECK) < 0.05 && landed.x >= -1418 && landed.z >= -1113.5 && landed.z <= -1094.7;
    ABS2.push([`report a1a5 jump ${name} stays on the deck`, !!ok,
      landed ? `landed (${f2(landed.x)}, ${f2(landed.y)}, ${f2(landed.z)})` : `never landed (${f2(p.x)}, ${f2(p.y)}, ${f2(p.z)})`]);
  }
}
{ // the ladder by the deck (records 35/34, face x -1106..-1109.7) is a 0x10 walk polygon
  // joining the bottom landing (y -90.6) to the top one (y -146). The 0x10 rule
  // (FUN_00357d48): from the bottom it is a wall (no walking up the ladder); from the top
  // the body drops off (onGround false).
  const up = walkFrom(-1115, -1107, -90.6, 1, 0, 20);
  ABS2.push(['ladder 35 from the bottom is a wall', up.end.x <= -1109.6 && Math.abs(up.maxY + 90.6) < 0.05 && Math.abs(up.minY + 90.6) < 0.05 && !up.fell && up.hits > 0,
    `end (${f2(up.end.x)}, ${f2(up.end.y)}) minY ${f2(up.minY)} fell ${up.fell} slides ${up.hits}`]);
  let p = { x: -1104.5, y: -146, z: -1105.9 }, drop = null;
  for (let s = 0; s < 10 && !drop; s++) {
    const r = col.move(p, { x: -0.5, z: 0 }, PLAY);
    if (!r.onGround) drop = r; else p = r;
  }
  ABS2.push(['ladder 35 from the top drops off', !!drop && drop.x < -1105.9 && drop.x > -1106.6,
    drop ? `drop at (${f2(drop.x)}, ${f2(drop.y)}, ${f2(drop.z)})` : `no drop, end x ${f2(p.x)}`]);
}
{ // flood fill from the spawn in 2-unit steps (drops fall to the first polygon below): no
  // report spot is reachable (roof under the bars and the pipes, the street).
  const SPOTS_OFF = [['ae9d roof', -1400.26, -96.46, -1083.54], ['5f40 roof', -1339.87, -71.08, -1108.92],
                     ['7c95 roof', -1269.82, -72.77, -1107.13], ['a1a5 street', -1291.65, 0, -1143.41]];
  const key = p => `${Math.round(p.x / 2)},${Math.round(p.z / 2)},${Math.round(p.y / 3)}`;
  const start = { x: X0, y: DECK, z: Z0 };
  const seen = new Set([key(start)]), q = [start];
  let bad = null;
  for (let i = 0; i < q.length && seen.size < 20000 && !bad; i++) {
    const p = q[i];
    for (let k = 0; k < 8; k++) {
      const dx = Math.cos(k * Math.PI / 4) * 2, dz = Math.sin(k * Math.PI / 4) * 2;
      let n = col.move(p, { x: dx, z: dz }, PLAY);
      if (!n.onGround) {
        const g = col.groundY(n.x, n.z, p.y, 0);
        if (g === null) continue;
        n = { x: n.x, y: g, z: n.z };
      }
      for (const [nm, sx, sy, sz] of SPOTS_OFF)
        if (Math.hypot(n.x - sx, n.z - sz) < 3 && Math.abs(n.y - sy) < 3) bad = `${nm} at (${f2(n.x)}, ${f2(n.y)}, ${f2(n.z)})`;
      const k2 = key(n);
      if (!seen.has(k2)) { seen.add(k2); q.push(n); }
    }
  }
  ABS2.push(['flood from the spawn reaches no report spot', !bad && seen.size < 20000, bad ?? `${seen.size} states`]);
}
{ // 0x02 chunk seams (FUN_00357d48 0x358c60 -> FUN_00359f38 at the end point). z128's 22
  // 0x02 polygons sit in pairs across chunk files; no neighbour link crosses a file, so
  // the seam edge is a boundary edge and only the 0x02 search carries the body over.
  // From each one's centroid, walk (ground, then airborne) in 0.5 steps toward the
  // nearest 0x02 polygon of another file: no wall, never off the ground, every step
  // forward, and it ends on that polygon's height.
  const seam = RAWP.filter(P => P.flags & 0x02);
  for (const P of seam) {
    let T = null, bd = Infinity;
    for (const Q of seam) if (Q.file !== P.file) {
      const d = Math.hypot(Q.c[0] - P.c[0], Q.c[2] - P.c[2]);
      if (d < bd) { bd = d; T = Q; }
    }
    const res = [];
    let ok = T !== null;
    for (const air of [false, true]) {
      if (!T) break;
      let p = { x: P.c[0], y: P.c[1], z: P.c[2] }, why = null, s = 0;
      for (; s < 40 && !why; s++) {
        const rx = T.c[0] - p.x, rz = T.c[2] - p.z, d = Math.hypot(rx, rz);
        if (d < 0.01) break;
        const k = Math.min(0.5, d) / d, dx = rx * k, dz = rz * k;
        const r = col.move(p, { x: dx, z: dz }, air ? { air: true } : PLAY);
        if (r.hitWall) why = `wall at step ${s}`;
        else if (!air && !r.onGround) why = `off the ground at step ${s}`;
        else if ((r.x - p.x) * dx + (r.z - p.z) * dz < 0.5 * (dx * dx + dz * dz)) why = `held back at step ${s}`;
        const y = air ? col.groundY(r.x, r.z, p.y) : r.y;
        if (y === null) { why = `no ground at step ${s}`; break; }
        if (Math.abs(y - p.y) > 1.5) why = `jumped ${f2(y - p.y)} at step ${s}`;
        p = { x: r.x, y, z: r.z };
      }
      if (!why && s >= 40) why = 'never arrived';
      if (!why && Math.abs(p.y - T.c[1]) > 0.05) why = `y ${f2(p.y)} want ${f2(T.c[1])}`;
      if (why) ok = false;
      res.push(`${air ? 'air' : 'ground'} ${why ?? 'ok'}`);
    }
    ABS2.push([`0x02 seam: poly ${P.q} (file ${P.file}) -> ${T ? `${T.q} (file ${T.file})` : 'none'}`, ok, res.join(', ')]);
  }
  ABS2.push(['0x02 seam polygons: 22 in z128', seam.length === 22, `${seam.length}`]);
}
{ // airborne off the mesh: FUN_00357d48 does nothing without a polygon (body+0x110 == 0)
  const p = { x: bmin[0] - 50, y: -100, z: bmin[2] - 50 };
  const r = col.move(p, { x: 1.2, z: -0.7 }, { air: true });
  ABS2.push(['airborne off the mesh stays put', r.x === p.x && r.z === p.z && r.y === p.y && !r.onGround && !r.hitWall,
    JSON.stringify(r)]);
}
{ // wall-flag filter: with block80 no polygon search returns a 0x80 polygon
  let on80 = 0, onPlain = 0;
  for (const [x, z, fy] of b80) {
    const g = col80.groundHit(x, z, fy), f = col80.nearestPoly(x, fy, z, 4);
    if ((g && (g.mask & 0x80)) || (f && (col80.polys[f[0]].flags & 0x80))) on80++;
    const g0 = col.groundHit(x, z, fy);
    if (g0 && (g0.mask & 0x80)) onPlain++;
  }
  ABS2.push([`block80: no search stands the body on a 0x80 polygon (${b80.length} centroids)`,
    on80 === 0 && onPlain > 0.9 * b80.length, `on 0x80 with block80 ${on80}, without ${onPlain}`]);
}
{ // corrupt DCOL files fail at load with a 'DCOL:' error, in both loaders
  const nv = dv.getUint32(8, true), nt = dv.getUint32(12, true), po = 0x30 + nv * 12 + nt * 20;
  const cases = [
    ['triangle vertex index', b => b.writeUInt32LE(nv + 7, 0x30 + nv * 12 + 4)],
    ['polygon vertex index', b => b.writeUInt32LE(nv, po + 8 + 5 * 40 + 8)],
    ['polygon neighbour', b => { const w = po + 8 + 5 * 40; b.writeUInt32LE(dv.getUint32(po, true) + 3, w + 24); }],
    ['polygon vertex count', b => b.writeUInt16LE(5, po + 8 + 5 * 40)],
    ['truncated', b => b.subarray(0, b.length - 41)],
    ['truncated in the vertices', b => b.subarray(0, 0x30 + 1000)],
  ];
  const tmp = path.join(process.env.TMPDIR ?? '/tmp', `dcol_corrupt_${process.pid}.bin`);
  for (const [name, mut] of cases) {
    const b = Buffer.from(buf);
    const m = mut(b), c = Buffer.isBuffer(m) ? m : b;
    let js;
    try { new Collision(c); js = 'loaded'; } catch (e) { js = e.message; }
    writeFileSync(tmp, c);
    const pr2 = spawnSync('python3', ['-I', '-c',
      'import sys; sys.path.insert(0, sys.argv[1]); import cid_query\ntry:\n cid_query.Collision(sys.argv[2]); print("loaded")\nexcept ValueError as e: print(e)',
      pyDir, tmp], { encoding: 'utf8' });
    const pym = (pr2.stdout || pr2.stderr).trim();
    ABS2.push([`corrupt file (${name}) fails with a DCOL error`, js.startsWith('DCOL:') && pym.startsWith('DCOL:'), `js "${js}" py "${pym}"`]);
  }
  try { unlinkSync(tmp); } catch {}
}
{ // 0x02 rules z128 cannot reach (its seams drop at most 1 and carry no other flag),
  // on a synthetic DCOL file, in both loaders. Polygon A (file 0, y 0, x 0..10,
  // z 0..10) has no neighbours; 0x02 polygons of file 1 lie past three of its
  // edges: B east 5 lower (> DROP_FALL: a fall), E north 2 lower (walk on), C west
  // 1 lower and flagged 0x80 (FUN_0026f750 tests no flags: block80 bodies cross
  // too). Nothing south: a wall.
  const sq = (x0, x1, z0, z1, y) => [[x0, y, z0], [x1, y, z0], [x1, y, z1], [x0, y, z1]];
  const P = [[0, 0x02, sq(0, 10, 0, 10, 0)], [1, 0x02, sq(10, 20, 0, 10, 5)],
             [1, 0x02, sq(0, 10, 10, 20, 2)], [1, 0x82, sq(-10, 0, 0, 10, 1)]];
  const nvs = P.length * 4, b = Buffer.alloc(0x30 + nvs * 12 + 8 + P.length * 40);
  b.write('DCOL', 0); b.writeUInt32LE(2, 4); b.writeUInt32LE(nvs, 8); b.writeUInt32LE(0, 12);
  let o = 0x30;
  for (const [, , vs] of P) for (const v of vs) for (const c of v) { b.writeFloatLE(c, o); o += 4; }
  b.writeUInt32LE(P.length, o); o += 8;
  P.forEach(([file, flags], q) => {
    b.writeUInt32LE((4 | flags << 16) >>> 0, o); b.writeUInt32LE((file << 16) >>> 0, o + 4);
    for (let a = 0; a < 4; a++) { b.writeUInt32LE(q * 4 + a, o + 8 + 4 * a); b.writeUInt32LE(0xffffffff, o + 24 + 4 * a); }
    o += 40;
  });
  // [name, block80, air, dx, dz, expected {x, y, z, onGround, hitWall} (null: just check), extra test]
  const Q = [['east onto B, 5 lower: falls', false, false, 6, 0, { x: 11, y: 5, z: 5, onGround: false, hitWall: false }],
             ['north onto E, 2 lower: walks on', false, false, 0, 6, { x: 5, y: 2, z: 11, onGround: true, hitWall: false }],
             ['west onto C (0x80), block80 body', true, false, -6, 0, { x: -1, y: 1, z: 5, onGround: true, hitWall: false }],
             ['airborne east onto B: no fall rule', false, true, 6, 0, { x: 11, y: 0, z: 5, onGround: false, hitWall: false }],
             ['south: nothing there, a wall', false, false, 0, -6, null, r => r.hitWall && r.onGround && r.z >= 0 && r.y === 0]];
  const tmp = path.join(process.env.TMPDIR ?? '/tmp', `dcol_seam_${process.pid}.bin`);
  writeFileSync(tmp, b);
  const pr3 = spawnSync('python3', ['-I', '-c', String.raw`
import json, sys
sys.path.insert(0, sys.argv[1])
import cid_query
q = json.loads(sys.argv[3])
out = []
for name, b80, air, dx, dz in q:
    col = cid_query.Collision(sys.argv[2], block80=b80)
    out.append(list(col.move(5.0, 0.0, 5.0, dx, dz, air=air)))
print(json.dumps(out))`, pyDir, tmp, JSON.stringify(Q.map(c => c.slice(0, 5)))], { encoding: 'utf8' });
  try { unlinkSync(tmp); } catch {}
  const pys = pr3.status === 0 ? JSON.parse(pr3.stdout) : null;
  Q.forEach(([name, b80, air, dx, dz, want, test], i) => {
    const r = new Collision(b, { block80: b80 }).move({ x: 5, y: 0, z: 5 }, { x: dx, z: dz }, { air });
    const near = (a, e) => Math.abs(a - e) < 1e-4;
    const ok = (want ? near(r.x, want.x) && near(r.y, want.y) && near(r.z, want.z) && r.onGround === want.onGround && r.hitWall === want.hitWall
                     : test(r)) && pys !== null && sameMove(r, pys[i]);
    ABS2.push([`synthetic 0x02 seam: ${name}`, ok, `js ${JSON.stringify(r)} py ${pys ? JSON.stringify(pys[i]) : pr3.stderr.trim().split('\n').pop()}`]);
  });
}
const ABS = [];
for (const [name, ok, msg] of ABS2) if (!ok) note('abs', `${name}: ${msg}`);

// ---------------------------------------------------------------- timing
const time = (fn, n) => { for (let i = 0; i < 2000; i++) fn(i); const s = performance.now(); for (let i = 0; i < n; i++) fn(i); return (performance.now() - s) / n * 1000; };
const gq = ground.filter(g => g[2] !== null);
const usG = time(i => { const g = gq[i % gq.length]; col.groundY(g[0], g[1], g[2]); }, 200000);
let tp = { x: X0, y: FY0, z: Z0 };
const usM = time(i => {                               // walking in a slow circle around the start
  const a = i * 0.01; tp = col.move(tp, { x: Math.cos(a), z: Math.sin(a) });
  if (!tp.onGround) tp = { x: X0, y: FY0, z: Z0 };
}, 50000);
const usS = time(i => { const [a, b] = segs[i % segs.length]; col.segment({ x: a[0], y: a[1], z: a[2] }, { x: b[0], y: b[1], z: b[2] }); }, 50000);

// ---------------------------------------------------------------- report
const total = ground.length + pushes.length + moveCalls + segs.length;
const nbad = Object.values(bad).reduce((a, b) => a + b, 0);
const hits = py.ground.filter(g => g !== null).length;
console.log(`module ${path.relative(root, modPath)}`);
console.log(`player-set tris js ${col.ntris} py ${py.ntris}; cells js ${col.ncells} py ${py.ncells}; build ${loadMs.toFixed(0)} ms (py run ${(pyMs / 1000).toFixed(1)} s)`);
console.log(`walk polygons js ${col.npolys} py ${py.npolys}; cells js ${col.npcells} py ${py.npcells}`);
console.log(`checks ${total} + ${ABS2.length} absolute: groundY ${ground.length} (${hits} with a floor), capsule ${pushes.length}, move ${moveCalls} (self-test walk + ${walks.length} random walks), segment ${segs.length}`);
console.log(`self-test walk travelled: ${selfTravel.join(' ')}`);
console.log(`mismatches ${nbad}  ${JSON.stringify(bad)}  max |js-py| over matched values ${maxErr.toExponential(2)}`);
for (const e of examples) console.log('  ' + e);
console.log(`timing per call: groundY ${usG.toFixed(2)} us, move ${usM.toFixed(2)} us, segment ${usS.toFixed(2)} us`);
const tcount = col.ntris !== py.ntris || col.ncells !== py.ncells || col.npolys !== py.npolys || col.npcells !== py.npcells
  || !(col.ntris > 0) || !(hits > 0) || !(walks.length > 0);   // an empty player set would otherwise pass
if (tcount) console.log('MISMATCH: triangle/cell counts differ, or nothing was tested (ntris/hits/walks = 0)');
console.log(nbad || tcount ? 'FAIL' : 'PASS');
process.exit(nbad || tcount ? 1 : 0);
