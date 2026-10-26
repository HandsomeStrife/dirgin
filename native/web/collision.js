// collision.js -- browser port of native/assets/collision/cid_query.py.
// Reads a DCOL v1 file (FORMAT.md section 8). No dependencies.
//
// Game space, -Y up: "down" is +y, feet sit at a larger y than the head.
//
//   const col = await loadCollision('data/z128_collision.bin');
//   col.groundY(x, z, feetY)   -> y of the first floor at or below the feet, or null
//   col.groundHit(x, z, feetY) -> {y, tri, normal, surface, subtree, mask} | null
//   col.move(pos, delta, opts) -> {x, y, z, onGround, hitWall}
//   col.segment(a, b)          -> {t, point, tri, normal, surface} | null
//
// The rules and the float maths follow cid_query.py line for line (all maths
// in doubles, triangles visited in the same order), so results match it exactly.

export const FLOOR_NY = 0.55;  // cos(56.6 deg): 45-degree ramps and the 55-degree walkway bevels (n.y 0.57) are floor
export const STEP_UP = 5.0;    // max rise the feet snap up onto (stairs are ~3.4)
export const CELL = 32.0;      // XZ grid cell size for the broad phase
// Character probe flag words (FUN_0054fc38 table, FUN_002db1f0 ground ray).
export const PROBE_FLAGS = [0x18d, 0x18b, 0x187];

// info word (FORMAT.md section 8) -> is this triangle player collision?
// Every sub-tree-1 packet that some character probe tests: FUN_0026f8a0 tests a
// packet only if (mask & ~(flags | 0x100)) != 0. Sub-tree 0 is not used (it floats
// above the visible walkway). See cid_query.py for the decode.
export function inPlayerSet(inf) {
  if ((inf & 3) !== 1) return false;
  const m = inf >>> 16;
  for (const f of PROBE_FLAGS) if ((m & ~(f | 0x100) & 0xffff) !== 0) return true;
  return false;
}

const K = 12;                  // per triangle: a, b, c (9), unit normal (3)

export async function loadCollision(src, options) {
  if (typeof src === 'string' || src instanceof URL) {
    const r = await fetch(src);
    if (!r.ok) throw new Error(`collision: ${src}: HTTP ${r.status}`);
    src = await r.arrayBuffer();
  }
  return new Collision(src, options);
}

export class Collision {
  // buf: ArrayBuffer (or a typed array view of one).
  // options.includeAll: keep every triangle (debug; the player set is the default).
  constructor(buf, options = {}) {
    let off = 0;
    if (ArrayBuffer.isView(buf)) { off = buf.byteOffset; buf = buf.buffer.slice(off, off + buf.byteLength); }
    const dv = new DataView(buf);
    const magic = String.fromCharCode(dv.getUint8(0), dv.getUint8(1), dv.getUint8(2), dv.getUint8(3));
    if (magic !== 'DCOL' || dv.getUint32(4, true) !== 1) throw new Error('not a DCOL v1 file');
    const nv = dv.getUint32(8, true), nt = dv.getUint32(12, true);
    let o = 0x30;
    const v = new Float32Array(buf, o, nv * 3); o += nv * 12;
    const tri = new Uint32Array(buf, o, nt * 3); o += nt * 12;
    const surf = new Uint32Array(buf, o, nt); o += nt * 4;
    const info = new Uint32Array(buf, o, nt); o += nt * 4;
    if (o !== buf.byteLength) throw new Error('size mismatch');
    // (DCOL is little-endian; every browser we target is too.)

    // Player set (inPlayerSet). Drop degenerate triangles.
    const includeAll = !!options.includeAll;
    const T = new Float64Array(nt * K);
    const S = new Uint32Array(nt);     // surface word
    const src = new Uint32Array(nt);   // index of the triangle in the file
    const inf2 = new Uint32Array(nt);  // its info word (sub-tree, source, file, packet mask)
    let n = 0;
    for (let t = 0; t < nt; t++) {
      const inf = info[t];
      if (!includeAll && !inPlayerSet(inf)) continue;
      const i = tri[3 * t] * 3, j = tri[3 * t + 1] * 3, k = tri[3 * t + 2] * 3;
      const ax = v[i], ay = v[i + 1], az = v[i + 2];
      const bx = v[j], by = v[j + 1], bz = v[j + 2];
      const cx = v[k], cy = v[k + 1], cz = v[k + 2];
      const ux = bx - ax, uy = by - ay, uz = bz - az;
      const wx = cx - ax, wy = cy - ay, wz = cz - az;
      const nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
      const ln = Math.sqrt(nx * nx + ny * ny + nz * nz);
      if (ln < 1e-9) continue;
      const p = n * K;
      T[p] = ax; T[p + 1] = ay; T[p + 2] = az;
      T[p + 3] = bx; T[p + 4] = by; T[p + 5] = bz;
      T[p + 6] = cx; T[p + 7] = cy; T[p + 8] = cz;
      T[p + 9] = nx / ln; T[p + 10] = ny / ln; T[p + 11] = nz / ln;
      S[n] = surf[t]; src[n] = t; inf2[n] = inf;
      n++;
    }
    this.ntris = n;
    this.T = T.subarray(0, n * K);
    this.S = S.subarray(0, n);
    this.src = src.subarray(0, n);
    this.info = inf2.subarray(0, n);

    // Uniform XZ grid (dense, CSR): each triangle goes in every cell its XZ
    // bbox touches, in triangle order, like the Python dict of lists.
    const cellRange = (p) => {
      const x0 = Math.floor(Math.min(T[p], T[p + 3], T[p + 6]) / CELL);
      const x1 = Math.floor(Math.max(T[p], T[p + 3], T[p + 6]) / CELL);
      const z0 = Math.floor(Math.min(T[p + 2], T[p + 5], T[p + 8]) / CELL);
      const z1 = Math.floor(Math.max(T[p + 2], T[p + 5], T[p + 8]) / CELL);
      return [x0, x1, z0, z1];
    };
    let gx0 = Infinity, gx1 = -Infinity, gz0 = Infinity, gz1 = -Infinity;
    for (let i = 0; i < n; i++) {
      const [x0, x1, z0, z1] = cellRange(i * K);
      if (x0 < gx0) gx0 = x0; if (x1 > gx1) gx1 = x1;
      if (z0 < gz0) gz0 = z0; if (z1 > gz1) gz1 = z1;
    }
    if (n === 0) { gx0 = gz0 = 0; gx1 = gz1 = -1; }
    const W = gx1 - gx0 + 1, H = gz1 - gz0 + 1;
    const start = new Uint32Array(W * H + 1);
    for (let i = 0; i < n; i++) {
      const [x0, x1, z0, z1] = cellRange(i * K);
      for (let gx = x0; gx <= x1; gx++)
        for (let gz = z0; gz <= z1; gz++) start[(gx - gx0) * H + (gz - gz0) + 1]++;
    }
    for (let c = 0; c < W * H; c++) start[c + 1] += start[c];
    const fill = start.slice(0, W * H);
    const items = new Uint32Array(start[W * H]);
    for (let i = 0; i < n; i++) {
      const [x0, x1, z0, z1] = cellRange(i * K);
      for (let gx = x0; gx <= x1; gx++)
        for (let gz = z0; gz <= z1; gz++) items[fill[(gx - gx0) * H + (gz - gz0)]++] = i;
    }
    Object.assign(this, { gx0, gz0, W, H, start, items });
    let used = 0;
    for (let c = 0; c < W * H; c++) if (start[c + 1] > start[c]) used++;
    this.ncells = used;

    this._stamp = new Uint32Array(n);   // dedupe for cells()
    this._gen = 0;
    this._list = new Uint32Array(Math.max(n, 1));
  }

  // Triangle indices whose cells overlap the XZ box, deduplicated, in the
  // Python's order (gx outer, gz inner, cell order). Returns the count; the
  // indices are in this._list (reused, valid until the next call).
  _cells(xmin, zmin, xmax, zmax) {
    const { gx0, gz0, W, H, start, items, _stamp: stamp, _list: out } = this;
    if (++this._gen === 0xffffffff) { stamp.fill(0); this._gen = 1; }
    const gen = this._gen;
    const ax = Math.max(Math.floor(xmin / CELL), gx0), bx = Math.min(Math.floor(xmax / CELL), gx0 + W - 1);
    const az = Math.max(Math.floor(zmin / CELL), gz0), bz = Math.min(Math.floor(zmax / CELL), gz0 + H - 1);
    let m = 0;
    for (let gx = ax; gx <= bx; gx++) {
      for (let gz = az; gz <= bz; gz++) {
        const c = (gx - gx0) * H + (gz - gz0);
        for (let q = start[c], e = start[c + 1]; q < e; q++) {
          const i = items[q];
          if (stamp[i] !== gen) { stamp[i] = gen; out[m++] = i; }
        }
      }
    }
    return m;
  }

  // ------------------------------------------------------------------ ground
  // Height of triangle i's plane at (x,z) if inside its XZ projection, else null.
  _surfaceY(i, x, z) {
    const T = this.T, p = i * K;
    const ax = T[p], az = T[p + 2];
    const ux = T[p + 3] - ax, uz = T[p + 5] - az;
    const wx = T[p + 6] - ax, wz = T[p + 8] - az;
    const den = ux * wz - wx * uz;
    if (Math.abs(den) < 1e-9) return null;          // vertical triangle
    const px = x - ax, pz = z - az;
    const u = (px * wz - wx * pz) / den;
    const v = (ux * pz - px * uz) / den;
    const e = 1e-6;
    if (u < -e || v < -e || u + v > 1 + e) return null;
    return T[p + 1] + u * (T[p + 4] - T[p + 1]) + v * (T[p + 7] - T[p + 1]);
  }

  // First floor (|n.y| >= FLOOR_NY, either winding: the game's ray is two-sided) under (x,z), counting from feetY - stepUp
  // downwards (+y): the smallest y >= feetY - stepUp. feetY null/undefined:
  // the topmost floor. Returns {y, tri, normal, surface} or null.
  groundHit(x, z, feetY, stepUp = STEP_UP) {
    const T = this.T, list = this._list;
    const lim = feetY == null ? null : feetY - stepUp;
    const m = this._cells(x, z, x, z);
    let best = null, bi = -1;
    for (let q = 0; q < m; q++) {
      const i = list[q];
      if (Math.abs(T[i * K + 10]) < FLOOR_NY) continue;
      const y = this._surfaceY(i, x, z);
      if (y === null || (lim !== null && y < lim)) continue;
      if (best === null || y < best) { best = y; bi = i; }
    }
    if (best === null) return null;
    const p = bi * K;
    const inf = this.info[bi];
    return { y: best, tri: this.src[bi], normal: [T[p + 9], T[p + 10], T[p + 11]], surface: this.S[bi],
             subtree: inf & 3, mask: inf >>> 16 };
  }

  groundY(x, z, feetY, stepUp = STEP_UP) {
    const g = this.groundHit(x, z, feetY, stepUp);
    return g === null ? null : g.y;
  }

  // -------------------------------------------------------------- walls
  // Push a vertical capsule out of walls, horizontally only (cid_query.py
  // capsule_push). Returns [x, z, hit].
  capsulePush(x, feetY, z, radius = 3.5, height = 19.0, stepUp = STEP_UP, iters = 3) {
    const T = this.T, list = this._list;
    const y0 = feetY - stepUp - radius;
    const y1 = feetY - height + radius;
    const ns = Math.max(1, Math.ceil((y0 - y1) / radius));
    const ys = [];
    if (y1 < y0) for (let k = 0; k <= ns; k++) ys.push(y0 + (y1 - y0) * k / ns);
    else ys.push(y0);
    const r2 = radius * radius;
    let hit = false;
    for (let it = 0; it < iters; it++) {
      let moved = false;
      const m = this._cells(x - radius, z - radius, x + radius, z + radius);
      for (let q = 0; q < m; q++) {
        const i = list[q], p = i * K;
        if (Math.abs(T[p + 10]) >= FLOOR_NY) continue;
        for (let s = 0; s < ys.length; s++) {
          const sy = ys[s];
          closestOnTri(x, sy, z, T, p);
          let dx = x - CQ[0], dz = z - CQ[2];
          const dy = sy - CQ[1];
          if (dx * dx + dy * dy + dz * dz >= r2) continue;
          let h = Math.sqrt(dx * dx + dz * dz), dist;
          if (h < 1e-6) {                   // straight above/below: use the face normal
            dx = T[p + 9]; dz = T[p + 11];
            h = Math.sqrt(dx * dx + dz * dz);
            if (h < 1e-6) continue;
            dist = 0.0;
          } else {
            dist = h;
          }
          const need = Math.sqrt(Math.max(r2 - dy * dy, 0.0)) - dist;
          if (need > 1e-4) {
            x += dx / h * need;
            z += dz / h * need;
            hit = moved = true;
          }
        }
      }
      if (!moved) break;
    }
    return [x, z, hit];
  }

  // pos {x,y,z} = feet; delta {x,z} = horizontal move this tick.
  // Push the capsule out of walls at the new spot, then snap the feet to the
  // ground (rise <= STEP_UP). No ground, or a drop > maxDrop: do not move.
  move(pos, delta, opts = {}) {
    const radius = opts.radius ?? 3.5, height = opts.height ?? 19;
    const maxDrop = opts.maxDrop ?? 40;
    const [nx, nz, hitWall] = this.capsulePush(pos.x + delta.x, pos.y, pos.z + delta.z, radius, height);
    const g = this.groundY(nx, nz, pos.y);
    if (g === null || g - pos.y > maxDrop)
      return { x: pos.x, y: pos.y, z: pos.z, onGround: false, hitWall };
    return { x: nx, y: g, z: nz, onGround: true, hitWall };
  }

  // ------------------------------------------------------------- segment
  // First triangle hit by segment a->b (double-sided Moller-Trumbore).
  // Uses the XZ cells under the segment's bbox: fine for short probes (camera).
  segment(a, b) {
    const T = this.T, list = this._list;
    const dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    const m = this._cells(Math.min(a.x, b.x), Math.min(a.z, b.z), Math.max(a.x, b.x), Math.max(a.z, b.z));
    let bt = null, bi = -1;
    for (let q = 0; q < m; q++) {
      const i = list[q], p = i * K;
      const e1x = T[p + 3] - T[p], e1y = T[p + 4] - T[p + 1], e1z = T[p + 5] - T[p + 2];
      const e2x = T[p + 6] - T[p], e2y = T[p + 7] - T[p + 1], e2z = T[p + 8] - T[p + 2];
      const px = dy * e2z - dz * e2y;
      const py = dz * e2x - dx * e2z;
      const pz = dx * e2y - dy * e2x;
      const det = e1x * px + e1y * py + e1z * pz;
      if (Math.abs(det) < 1e-12) continue;
      const inv = 1.0 / det;
      const sx = a.x - T[p], sy = a.y - T[p + 1], sz = a.z - T[p + 2];
      const u = (sx * px + sy * py + sz * pz) * inv;
      if (u < 0 || u > 1) continue;
      const qx = sy * e1z - sz * e1y;
      const qy = sz * e1x - sx * e1z;
      const qz = sx * e1y - sy * e1x;
      const v = (dx * qx + dy * qy + dz * qz) * inv;
      if (v < 0 || u + v > 1) continue;
      const t = (e2x * qx + e2y * qy + e2z * qz) * inv;
      if (t >= 0 && t <= 1 && (bt === null || t < bt)) { bt = t; bi = i; }
    }
    if (bt === null) return null;
    const p = bi * K;
    return {
      t: bt,
      point: { x: a.x + bt * dx, y: a.y + bt * dy, z: a.z + bt * dz },
      tri: this.src[bi],
      normal: [T[p + 9], T[p + 10], T[p + 11]],
      surface: this.S[bi],
    };
  }
}

// Closest point on triangle (at T[p..p+8]) to point P (Ericson, RTCD 5.1.5).
// Writes the result to CQ (no allocation per call).
const CQ = new Float64Array(3);
function closestOnTri(px, py, pz, T, p) {
  const ax = T[p], ay = T[p + 1], az = T[p + 2];
  const bx = T[p + 3], by = T[p + 4], bz = T[p + 5];
  const cx = T[p + 6], cy = T[p + 7], cz = T[p + 8];
  const abx = bx - ax, aby = by - ay, abz = bz - az;
  const acx = cx - ax, acy = cy - ay, acz = cz - az;
  const apx = px - ax, apy = py - ay, apz = pz - az;
  const d1 = abx * apx + aby * apy + abz * apz;
  const d2 = acx * apx + acy * apy + acz * apz;
  if (d1 <= 0 && d2 <= 0) { CQ[0] = ax; CQ[1] = ay; CQ[2] = az; return; }
  const bpx = px - bx, bpy = py - by, bpz = pz - bz;
  const d3 = abx * bpx + aby * bpy + abz * bpz;
  const d4 = acx * bpx + acy * bpy + acz * bpz;
  if (d3 >= 0 && d4 <= d3) { CQ[0] = bx; CQ[1] = by; CQ[2] = bz; return; }
  const vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) {
    const t = d1 / (d1 - d3);
    CQ[0] = ax + t * abx; CQ[1] = ay + t * aby; CQ[2] = az + t * abz; return;
  }
  const cpx = px - cx, cpy = py - cy, cpz = pz - cz;
  const d5 = abx * cpx + aby * cpy + abz * cpz;
  const d6 = acx * cpx + acy * cpy + acz * cpz;
  if (d6 >= 0 && d5 <= d6) { CQ[0] = cx; CQ[1] = cy; CQ[2] = cz; return; }
  const vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) {
    const t = d2 / (d2 - d6);
    CQ[0] = ax + t * acx; CQ[1] = ay + t * acy; CQ[2] = az + t * acz; return;
  }
  const va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
    const t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    CQ[0] = bx + t * (cx - bx); CQ[1] = by + t * (cy - by); CQ[2] = bz + t * (cz - bz); return;
  }
  const den = 1.0 / (va + vb + vc);
  const v = vb * den, w = vc * den;
  CQ[0] = ax + abx * v + acx * w; CQ[1] = ay + aby * v + acy * w; CQ[2] = az + abz * v + acz * w;
}
