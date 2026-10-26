// sweep_collision.mjs -- headless pass-through sweep for the z128 player collision.
//
//   node native/assets/collision/sweep_collision.mjs [collision.js] [z128_collision.bin] [--json out.json]
//
// Flood-fills the area Vincent can reach from the spawn the way play.html moves him
// (collision.js move() with the player's options; walking off an edge falls to the
// first floor below), in 2-unit steps in 8 directions. Every step is checked against
// the VISIBLE room (native/web/data room meshes, sky excluded): four horizontal body
// segments (6, 10, 14 and 18 units above the feet) from the old to the new position,
// and for falls the vertical segment from the feet to the landing. A segment crossing
// a room triangle is a pass-through. Reported: reachable cells, pass-through steps,
// and pass-through places (8x8-unit cells) grouped by room mesh.
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../../..');
const args = process.argv.slice(2);
const jsonAt = args.indexOf('--json');
const jsonOut = jsonAt >= 0 ? args.splice(jsonAt, 2)[1] : null;
const modPath = path.resolve(args[0] ?? path.join(root, 'native/web/collision.js'));
const binPath = path.resolve(args[1] ?? path.join(root, 'native/web/data/z128_collision.bin'));
const dataDir = path.join(root, 'native/web/data');

const { Collision } = await import(pathToFileURL(modPath).href);
const b = readFileSync(binPath);
const col = new Collision(b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength));

// ------------------------------------------------------------ room triangles
const scene = JSON.parse(readFileSync(path.join(dataDir, 'scene.json'), 'utf8'));
const RT = [];          // flat: 9 floats per triangle
const RNAME = [];       // mesh name per triangle
const seen = new Set();
for (const d of scene.draws) {
  if (d.kind !== 'room' || seen.has(d.mesh) || /sky/.test(d.mesh)) continue;
  seen.add(d.mesh);
  const raw = readFileSync(path.join(dataDir, scene.meshes[d.mesh].file));
  const f = new Float32Array(raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength));
  for (let v = 0; v + 2 < f.length / 12; v += 3) {
    for (let k = 0; k < 3; k++) for (let c = 0; c < 3; c++) RT.push(f[(v + k) * 12 + c]);
    RNAME.push(d.mesh);
  }
}
const NR = RNAME.length;
const RC = 16;
const rgrid = new Map();
for (let t = 0; t < NR; t++) {
  const p = t * 9;
  const x0 = Math.floor(Math.min(RT[p], RT[p + 3], RT[p + 6]) / RC), x1 = Math.floor(Math.max(RT[p], RT[p + 3], RT[p + 6]) / RC);
  const z0 = Math.floor(Math.min(RT[p + 2], RT[p + 5], RT[p + 8]) / RC), z1 = Math.floor(Math.max(RT[p + 2], RT[p + 5], RT[p + 8]) / RC);
  if ((x1 - x0 + 1) * (z1 - z0 + 1) > 4096) continue;        // huge backdrop planes
  for (let gx = x0; gx <= x1; gx++) for (let gz = z0; gz <= z1; gz++) {
    const k = gx * 100000 + gz;
    let a = rgrid.get(k); if (!a) rgrid.set(k, a = []); a.push(t);
  }
}
let stampGen = 0; const stamp = new Uint32Array(NR);
// first room triangle crossed by segment a->b (Moller-Trumbore, two-sided), or -1
function roomHit(ax, ay, az, bx, by, bz) {
  const dx = bx - ax, dy = by - ay, dz = bz - az;
  const gx0 = Math.floor(Math.min(ax, bx) / RC), gx1 = Math.floor(Math.max(ax, bx) / RC);
  const gz0 = Math.floor(Math.min(az, bz) / RC), gz1 = Math.floor(Math.max(az, bz) / RC);
  stampGen++;
  for (let gx = gx0; gx <= gx1; gx++) for (let gz = gz0; gz <= gz1; gz++) {
    const list = rgrid.get(gx * 100000 + gz); if (!list) continue;
    for (const t of list) {
      if (stamp[t] === stampGen) continue; stamp[t] = stampGen;
      const p = t * 9;
      const e1x = RT[p + 3] - RT[p], e1y = RT[p + 4] - RT[p + 1], e1z = RT[p + 5] - RT[p + 2];
      const e2x = RT[p + 6] - RT[p], e2y = RT[p + 7] - RT[p + 1], e2z = RT[p + 8] - RT[p + 2];
      const px = dy * e2z - dz * e2y, py = dz * e2x - dx * e2z, pz = dx * e2y - dy * e2x;
      const det = e1x * px + e1y * py + e1z * pz;
      if (Math.abs(det) < 1e-12) continue;
      const inv = 1 / det;
      const sx = ax - RT[p], sy = ay - RT[p + 1], sz = az - RT[p + 2];
      const u = (sx * px + sy * py + sz * pz) * inv; if (u < 0 || u > 1) continue;
      const qx = sy * e1z - sz * e1y, qy = sz * e1x - sx * e1z, qz = sx * e1y - sy * e1x;
      const v = (dx * qx + dy * qy + dz * qz) * inv; if (v < 0 || u + v > 1) continue;
      const tt = (e2x * qx + e2y * qy + e2z * qz) * inv;
      if (tt > 1e-6 && tt < 1 - 1e-6) return t;
    }
  }
  return -1;
}

// ------------------------------------------------------------ player step
const OPTS = { radius: 3.5, height: 19, maxDrop: 4 };        // play.js / player.js
const MAX_FALL = 400;
function step(pos, dx, dz) {
  const r = col.move(pos, { x: dx, z: dz }, OPTS);
  if (r.onGround) return { x: r.x, y: r.y, z: r.z, fell: false };
  // walked off an edge: player.js falls with capsule pushes and lands on the first floor
  let nx = pos.x + dx, nz = pos.z + dz;
  if (col.capsulePush) [nx, nz] = col.capsulePush(nx, pos.y, nz, OPTS.radius, OPTS.height);
  const g = col.groundY(nx, nz, pos.y, 0);
  if (g === null || g - pos.y > MAX_FALL) return null;
  return { x: nx, y: g, z: nz, fell: true };
}

// ------------------------------------------------------------ flood fill
const SPAWN = { x: -1395.63, y: -98.4375, z: -1110.51 };
const STEP = 2, LIMIT = 400000;
// States are binned at 1 unit (half a step): on a 45-degree ramp a 2-unit move only
// advances 1.4 horizontally (the walk mesh keeps the move's length along the slope),
// and 2-unit bins merged those states and stalled the fill (worker 0061).
const BIN = STEP / 2;
const key = p => `${Math.round(p.x / BIN)},${Math.round(p.z / BIN)},${Math.round(p.y / 3)}`;
const DIRS = [];
for (let k = 0; k < 8; k++) DIRS.push([Math.cos(k * Math.PI / 4) * STEP, Math.sin(k * Math.PI / 4) * STEP]);
const HEIGHTS = [6, 10, 14, 18];
const visited = new Map([[key(SPAWN), SPAWN]]);
const queue = [SPAWN];
let steps = 0, ptSteps = 0, falls = 0;
const places = new Map();       // "cx,cz" -> {x,z,y,mesh:Map,n,fall}
for (let qi = 0; qi < queue.length && visited.size < LIMIT; qi++) {
  const p = queue[qi];
  for (const [dx, dz] of DIRS) {
    const n = step(p, dx, dz);
    if (!n) continue;
    steps++;
    if (Math.hypot(n.x - p.x, n.z - p.z) < 0.2 && Math.abs(n.y - p.y) < 0.2) continue;
    if (n.fell) falls++;
    let hit = -1, kind = 'walk';
    for (const h of HEIGHTS) {
      hit = roomHit(p.x, p.y - h, p.z, n.x, Math.max(n.y, p.y) - h, n.z);
      if (hit >= 0) break;
    }
    if (hit < 0 && n.y - p.y > 1) { hit = roomHit(n.x, p.y - 0.5, n.z, n.x, n.y - 0.5, n.z); kind = 'fall'; }
    if (hit >= 0) {
      ptSteps++;
      const k = `${Math.floor(n.x / 8)},${Math.floor(n.z / 8)}`;
      let pl = places.get(k);
      if (!pl) places.set(k, pl = { x: n.x, z: n.z, y: n.y, mesh: new Map(), n: 0, fall: 0 });
      pl.n++; if (kind === 'fall') pl.fall++;
      pl.mesh.set(RNAME[hit], (pl.mesh.get(RNAME[hit]) || 0) + 1);
    }
    const k = key(n);
    if (!visited.has(k)) { visited.set(k, n); queue.push(n); }
  }
}
const cells = new Set([...visited.values()].map(p => `${Math.floor(p.x / 8)},${Math.floor(p.z / 8)},${Math.round(p.y / 3)}`));
const byMesh = new Map();
for (const pl of places.values()) {
  const top = [...pl.mesh.entries()].sort((a, b) => b[1] - a[1])[0][0];
  byMesh.set(top, (byMesh.get(top) || 0) + 1);
}
let lowest = -Infinity; for (const p of visited.values()) if (p.y > lowest) lowest = p.y;
console.log(`module ${path.relative(root, modPath)}  bin ${path.relative(root, binPath)}`);
console.log(`room tris ${NR}; reachable states ${visited.size}${visited.size >= LIMIT ? ' (limit)' : ''}, 8-unit cells ${cells.size}, lowest feet y ${lowest.toFixed(1)}`);
console.log(`steps ${steps} (falls ${falls}); pass-through steps ${ptSteps}; pass-through places (8x8 cells) ${places.size}`);
console.log('places by room mesh: ' + [...byMesh.entries()].sort((a, b) => b[1] - a[1]).map(([m, n]) => `${m} ${n}`).join(', '));
const worst = [...places.values()].sort((a, b) => b.n - a.n).slice(0, 12);
for (const pl of worst)
  console.log(`  (${pl.x.toFixed(1)}, ${pl.y.toFixed(1)}, ${pl.z.toFixed(1)})  ${pl.n} steps${pl.fall ? ` (${pl.fall} falls)` : ''}  ${[...pl.mesh.keys()].join(' ')}`);
if (jsonOut) writeFileSync(jsonOut, JSON.stringify({
  reachable: visited.size, cells: cells.size, steps, ptSteps, places: places.size,
  byMesh: Object.fromEntries(byMesh),
  list: [...places.values()].map(pl => ({ x: pl.x, y: pl.y, z: pl.z, n: pl.n, fall: pl.fall, mesh: Object.fromEntries(pl.mesh) })),
  states: [...visited.values()].map(p => [Math.round(p.x * 10) / 10, Math.round(p.y * 10) / 10, Math.round(p.z * 10) / 10]),
}));
