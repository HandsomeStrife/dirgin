// test_collision.mjs -- parity test: collision.js vs native/assets/collision/cid_query.py.
//
//   node native/web/test_collision.mjs [path/to/collision.js] [path/to/z128_collision.bin]
//
// Runs cid_query.py (unchanged, imported by an inline helper) on the same
// queries and compares: groundY (random XZ x several feetY, the 3 known spots,
// points near the start), capsule pushes, move() over the self-test's 8-direction
// walk plus random walks, and segments. Exit code 1 on any mismatch.
import { readFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const modPath = path.resolve(process.argv[2] ?? path.join(here, 'collision.js'));
const binPath = path.resolve(process.argv[3] ?? path.join(here, 'data/z128_collision.bin'));
const pyDir = path.join(root, 'native/assets/collision');
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
// feet just above. Picked from the file itself with an inline copy of the player-set
// rule (sub-tree 1 with a class bit in 0x0e), not from the module under test.
{
  const nv = dv.getUint32(8, true), nt = dv.getUint32(12, true);
  const V = new Float32Array(buf.buffer.slice(buf.byteOffset + 0x30, buf.byteOffset + 0x30 + nv * 12));
  const I = new Uint32Array(buf.buffer.slice(buf.byteOffset + 0x30 + nv * 12, buf.byteOffset + 0x30 + nv * 12 + nt * 12));
  const F = new Uint32Array(buf.buffer.slice(buf.byteOffset + 0x30 + nv * 12 + nt * 16, buf.byteOffset + 0x30 + nv * 12 + nt * 20));
  const picks = [];
  for (let t = 0; t < nt; t++) {
    if ((F[t] & 3) !== 1 || !((F[t] >>> 16) & 0x0e)) continue;
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
for (let w = 0; w < 30; w++) {
  const x = X0 + (rnd() - 0.5) * 200, z = Z0 + (rnd() - 0.5) * 200;
  const y = col.groundY(x, z, FY0 + 20);            // start on a floor (same answer checked above)
  if (y === null) continue;
  let a = rnd() * 2 * Math.PI;
  const deltas = [];
  for (let s = 0; s < 120; s++) {
    a += (rnd() - 0.5) * 0.6;
    const sp = lerp(0.5, 2.0, rnd());
    deltas.push([Math.cos(a) * sp, Math.sin(a) * sp]);
  }
  walks.push({ x, y, z, deltas });
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
def move(x, y, z, dx, dz):
    nx, nz, hit = col.capsule_push(x + dx, y, z + dz)
    g = col.ground_height(nx, nz, y)
    if g is None or g[0] - y > 40:
        return [x, y, z, False, hit]
    return [nx, g[0], nz, True, hit]
out = {'ntris': len(col.tris), 'ncells': len(col.grid)}
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
        r = move(x, y, z, dx, dz)
        steps.append(r)
        x, y, z = r[0], r[1], r[2]
    rw.append(steps)
out['walks'] = rw
def seg(a, b):
    h = col.segment_hit(a, b)
    return None if h is None else [h[0], list(h[1]), list(h[2])]
out['segs'] = [seg(a, b) for a, b in q['segs']]
json.dump(out, sys.stdout)
`;
t0 = performance.now();
const pr = spawnSync('python3', ['-I', '-c', PY, pyDir, binPath], {
  input: JSON.stringify({ ground, pushes, walks, segs, spot: [X0, Z0, FY0] }),
  maxBuffer: 1 << 28, encoding: 'utf8',
});
if (pr.status !== 0) { console.error(pr.stderr); process.exit(2); }
const py = JSON.parse(pr.stdout);
const pyMs = performance.now() - t0;

// ---------------------------------------------------------------- compare
const bad = { ground: 0, push: 0, selfwalk: 0, walks: 0, segs: 0, abs: 0 };
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
    const r = col.move(pos, { x: w.deltas[s][0], z: w.deltas[s][1] }); moveCalls++;
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

// ---------------------------------------------------------------- absolute checks
// Behaviour against the game data, not against the Python (which shares the rules).
// Fall-through spots (worker 0053; found by a flood fill from the start: the old
// one-sided rule with FLOOR_NY 0.6 dropped him to oldY): the ground must stay within
// maxDrop units below the feet.
const ABS = [
  ['walkway rim (55-degree bevel top)', -1272, -1110, -93.8, -69.89, 1],
  ['walkway rim (reverse-wound face)', -1331, -1106, -93.5, -74.00, 1],
  ['stairs at x -1430, z -670 (next tread is 1.5 down)', -1430, -670, -94.2, -90.00, 2.5],
  ['stairs at x -1428, z -694 (next tread is 1.6 down)', -1428, -694, -96.9, -91.99, 2.5],
];
for (const [name, x, z, fy, oldY, maxDrop] of ABS) {
  const g = col.groundY(x, z, fy);
  if (g === null || g - fy > maxDrop) note('abs', `${name}: ground ${g} under feet ${fy} (old rule fell to ${oldY})`);
}
for (const [name, x, z, fy] of SPOTS) {                // Vincent's dumped feet vs the ground
  const g = col.groundY(x, z, fy);
  if (g === null || Math.abs(g - fy) >= 0.4) note('abs', `${name}: feet ${fy} ground ${g}`);
}
{                                                      // the invisible railing plane (mask 0x0104) stops him
  let pos = { x: X0, y: FY0, z: Z0 };
  for (let s = 0; s < 30; s++) pos = col.move(pos, { x: 0, z: -1 });
  if (pos.z < Z0 - 10 || !pos.onGround) note('abs', `walk toward -z went to z ${pos.z.toFixed(2)} (railing at -1120.1)`);
}

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
console.log(`checks ${total} + ${ABS.length + SPOTS.length + 1} absolute: groundY ${ground.length} (${hits} with a floor), capsule ${pushes.length}, move ${moveCalls} (self-test walk + ${walks.length} random walks), segment ${segs.length}`);
console.log(`self-test walk travelled: ${selfTravel.join(' ')}`);
console.log(`mismatches ${nbad}  ${JSON.stringify(bad)}  max |js-py| over matched values ${maxErr.toExponential(2)}`);
for (const e of examples) console.log('  ' + e);
console.log(`timing per call: groundY ${usG.toFixed(2)} us, move ${usM.toFixed(2)} us, segment ${usS.toFixed(2)} us`);
const tcount = col.ntris !== py.ntris || col.ncells !== py.ncells
  || !(col.ntris > 0) || !(hits > 0) || !(walks.length > 0);   // an empty player set would otherwise pass
if (tcount) console.log('MISMATCH: triangle/cell counts differ, or nothing was tested (ntris/hits/walks = 0)');
console.log(nbad || tcount ? 'FAIL' : 'PASS');
process.exit(nbad || tcount ? 1 : 0);
