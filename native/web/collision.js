// collision.js -- browser port of native/assets/collision/cid_query.py.
// Reads a DCOL v2 file (FORMAT.md section 8). No dependencies.
//
// Game space, -Y up: "down" is +y, feet sit at a larger y than the head.
//
//   const col = await loadCollision('data/z128_collision.bin');
//   col.groundY(x, z, feetY)   -> y of the walk polygon under the feet, or null
//   col.groundHit(x, z, feetY) -> {y, tri, poly, normal, surface, subtree, mask} | null
//   col.move(pos, delta, opts) -> {x, y, z, onGround, hitWall}
//        opts.air: walk only the ground point (jump / fall horizontal step),
//        y unchanged, onGround false; the polygon height there is groundY().
//   col.capsulePush(x, feetY, z, radius, height) -> [x, z, hit]   (triangle walls)
//   col.segment(a, b)          -> {t, point, tri, normal, surface} | null
//
// Walking is the game's navmesh walker (worker 0061; cid_query.py docstring and
// FORMAT.md 3d have the decode): z128 runs the physics-body movement mode 0xc,
// whose FUN_00357d48 walks the sub-tree-0 tag-0 polygons through their edge
// neighbours. A boundary edge is a wall (slide along it, 1% pushed back in).
// Flags: 0x80 walkable unless options.block80; 0x10 one-way drop (fall if the
// shared edge's outward normal points up, else wall); 0x02 boundary edges look
// for a polygon in another file; 0x04/0x40/0x100 (absent in z128) walls; the
// polygon searches skip polygons that are walls for the body. No
// slope limit, step height, ledge or headroom rule: the polygons are the
// walkable area. The triangle set (capsulePush, segment) = sub-tree-1 packets
// the character probes test (mask & 0xfe7e) + sub-tree-0 tag-1 primitives.
// The rules and the float maths follow cid_query.py line for line (all maths
// in doubles, items visited in the same order), so results match it exactly.

export const FLOOR_NY = 0.55;   // capsulePush ignores faces this flat
export const SEARCH_UP = 7.5;   // FUN_0035cbc8: polygon search starts 7.5 above the point
export const SEARCH_DOWN = 200; // ... and ends 200 below it
export const DROP_FALL = 3.0;   // FUN_00359f38: a polygon more than 3 lower starts a fall
export const NAV_PASSES = 8;    // FUN_00357d48 loop limit
export const EDGE_EPS = 0.001;  // FUN_0035d4e8: start point inside an edge plane up to 0.001
export const SLIDE_IN = 0.01;   // slide move pushed 1% of its length back inside the edge
export const HEIGHT = 19.0;     // body height for capsulePush (a guess)
export const STEP_UP = 5.0;     // capsulePush: walls lower than this above the feet do not block
export const CELL = 32.0;       // XZ grid cell size for the broad phase
export const INSIDE_EPS = 1e-4; // vertical polygon search: tolerance on the edge planes
// Character probe flag words (FUN_0054fc38 table, FUN_002db1f0 ground ray).
export const PROBE_FLAGS = [0x18d, 0x18b, 0x187];

export const SRC_PRIM = 3;      // info bits 2-3: tag-1 primitive (FORMAT.md 3c)
export const F_CHUNK = 0x02, F_DROP = 0x10, F_NPC = 0x80, F_WALLS = 0x04 | 0x40 | 0x100;

// info word (FORMAT.md section 8) -> is this triangle in the triangle set?
// Sub-tree-1 packets some character probe tests: FUN_0026f8a0 tests a packet only
// if (mask & ~(flags | 0x100)) != 0, i.e. mask & 0xfe7e != 0 over the three flag
// words; plus the sub-tree-0 tag-1 primitives (FUN_0035a440's body push).
export function inPlayerSet(inf) {
  if ((inf & 3) === 0) return ((inf >>> 2) & 3) === SRC_PRIM;
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

// FUN_0025ff10: plane through p1, p2, p3 (arrays), normal (p2-p1) x (p3-p2) made
// unit, w = -n.p1. null if degenerate.
function plane3(p1, p2, p3) {
  const ax = p2[0] - p1[0], ay = p2[1] - p1[1], az = p2[2] - p1[2];
  const bx = p3[0] - p2[0], by = p3[1] - p2[1], bz = p3[2] - p2[2];
  const n0 = ay * bz - az * by, n1 = az * bx - ax * bz, n2 = ax * by - ay * bx;
  const ln = Math.sqrt(n0 * n0 + n1 * n1 + n2 * n2);
  if (ln < 1e-12) return null;
  const nx = n0 / ln, ny = n1 / ln, nz = n2 / ln;
  return [nx, ny, nz, -(nx * p1[0] + ny * p1[1] + nz * p1[2])];
}

// Uniform XZ grid (dense, CSR): each item goes in every cell its XZ bbox
// [x0, x1, z0, z1] touches, in item order, like the Python dict of lists.
function buildGrid(n, boxOf) {
  const rng = (i) => { const b = boxOf(i); return [Math.floor(b[0] / CELL), Math.floor(b[1] / CELL), Math.floor(b[2] / CELL), Math.floor(b[3] / CELL)]; };
  let gx0 = Infinity, gx1 = -Infinity, gz0 = Infinity, gz1 = -Infinity;
  const R = new Array(n);
  for (let i = 0; i < n; i++) {
    const r = R[i] = rng(i);
    if (r[0] < gx0) gx0 = r[0]; if (r[1] > gx1) gx1 = r[1];
    if (r[2] < gz0) gz0 = r[2]; if (r[3] > gz1) gz1 = r[3];
  }
  if (n === 0) { gx0 = gz0 = 0; gx1 = gz1 = -1; }
  const W = gx1 - gx0 + 1, H = gz1 - gz0 + 1;
  const start = new Uint32Array(W * H + 1);
  for (let i = 0; i < n; i++) {
    const [x0, x1, z0, z1] = R[i];
    for (let gx = x0; gx <= x1; gx++) for (let gz = z0; gz <= z1; gz++) start[(gx - gx0) * H + (gz - gz0) + 1]++;
  }
  for (let c = 0; c < W * H; c++) start[c + 1] += start[c];
  const fill = start.slice(0, W * H);
  const items = new Uint32Array(start[W * H]);
  for (let i = 0; i < n; i++) {
    const [x0, x1, z0, z1] = R[i];
    for (let gx = x0; gx <= x1; gx++) for (let gz = z0; gz <= z1; gz++) items[fill[(gx - gx0) * H + (gz - gz0)]++] = i;
  }
  let used = 0;
  for (let c = 0; c < W * H; c++) if (start[c + 1] > start[c]) used++;
  return { gx0, gz0, W, H, start, items, used, stamp: new Uint32Array(Math.max(n, 1)), gen: 0, list: new Uint32Array(Math.max(n, 1)) };
}

// Item indices whose cells overlap the XZ box, deduplicated, in the Python's
// order (gx outer, gz inner, cell order). Returns the count; the indices are
// in g.list (reused, valid until the next call on this grid).
function gridCells(g, xmin, zmin, xmax, zmax) {
  const { gx0, gz0, W, H, start, items, stamp, list: out } = g;
  if (++g.gen === 0xffffffff) { stamp.fill(0); g.gen = 1; }
  const gen = g.gen;
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

export class Collision {
  // buf: ArrayBuffer (or a typed array view of one).
  // options.includeAll: keep every triangle in the triangle set (debug).
  // options.block80: polygons flagged 0x80 are walls (bodies with flag 0x200).
  constructor(buf, options = {}) {
    let off = 0;
    if (ArrayBuffer.isView(buf)) { off = buf.byteOffset; buf = buf.buffer.slice(off, off + buf.byteLength); }
    const dv = new DataView(buf);
    const magic = String.fromCharCode(dv.getUint8(0), dv.getUint8(1), dv.getUint8(2), dv.getUint8(3));
    if (magic !== 'DCOL' || dv.getUint32(4, true) !== 2)
      throw new Error('not a DCOL v2 file (rebuild: make -C native/assets/collision z128web)');
    const nv = dv.getUint32(8, true), nt = dv.getUint32(12, true);
    let o = 0x30;
    const pOff = o + nv * 12 + nt * 20;                      // npoly word
    if (pOff + 8 > buf.byteLength) throw new Error('DCOL: size mismatch (file shorter than its header says)');
    const v = new Float32Array(buf, o, nv * 3); o += nv * 12;
    const tri = new Uint32Array(buf, o, nt * 3); o += nt * 12;
    const surf = new Uint32Array(buf, o, nt); o += nt * 4;
    const info = new Uint32Array(buf, o, nt); o += nt * 4;
    const npoly = dv.getUint32(o, true); o += 8;
    if (o + npoly * 40 !== buf.byteLength) throw new Error('DCOL: size mismatch');
    const pw = new Uint32Array(buf, o, npoly * 10); o += npoly * 40;
    // A corrupt file fails here with a clear error, not a TypeError or NaN later.
    for (let t = 0; t < nt * 3; t++)
      if (tri[t] >= nv) throw new Error(`DCOL: triangle ${Math.floor(t / 3)} vertex index ${tri[t]} >= ${nv}`);
    for (let q = 0; q < npoly; q++) {
      const w = q * 10, pnv = pw[w] & 0xffff;
      if (pnv < 3 || pnv > 4) throw new Error(`DCOL: walk polygon ${q} has ${pnv} vertices (3 or 4)`);
      for (let a = 0; a < pnv; a++) {
        if (pw[w + 2 + a] >= nv) throw new Error(`DCOL: walk polygon ${q} vertex index ${pw[w + 2 + a]} >= ${nv}`);
        const nb = pw[w + 6 + a];
        if (nb !== 0xffffffff && nb >= npoly) throw new Error(`DCOL: walk polygon ${q} neighbour ${nb} >= ${npoly}`);
      }
    }
    // (DCOL is little-endian; every browser we target is too.)
    this.block80 = !!options.block80;

    // ---- triangle set (capsulePush, segment). Drop degenerate triangles.
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
    const Tt = this.T;
    this.tgrid = buildGrid(n, (i) => {
      const p = i * K;
      return [Math.min(Tt[p], Tt[p + 3], Tt[p + 6]), Math.max(Tt[p], Tt[p + 3], Tt[p + 6]),
              Math.min(Tt[p + 2], Tt[p + 5], Tt[p + 8]), Math.max(Tt[p + 2], Tt[p + 5], Tt[p + 8])];
    });
    this.ncells = this.tgrid.used;
    this._list = this.tgrid.list;

    // ---- walk mesh (FORMAT.md 3d)
    const polys = this.polys = new Array(npoly);
    for (let q = 0; q < npoly; q++) {
      const w = q * 10;
      const pnv = pw[w] & 0xffff;
      const P = { nv: pnv, flags: pw[w] >>> 16, word6: pw[w + 1] & 0xffff, file: pw[w + 1] >>> 16,
                  v: [], nbr: [], plane: null, edges: [], ok: false };
      for (let a = 0; a < pnv; a++) {
        const vi = pw[w + 2 + a] * 3;
        P.v.push([v[vi], v[vi + 1], v[vi + 2]]);
        P.nbr.push(pw[w + 6 + a] === 0xffffffff ? null : pw[w + 6 + a]);
      }
      const vs = P.v;
      P.plane = plane3(vs[0], vs[1], vs[2]);           // FUN_0035d0a0
      // FUN_0035d2e0: N = (v1-v0) x (v2-v1) (not unit); edge plane j through v[j], v[j+1], v[j] + N.
      const ax = vs[1][0] - vs[0][0], ay = vs[1][1] - vs[0][1], az = vs[1][2] - vs[0][2];
      const bx = vs[2][0] - vs[1][0], by = vs[2][1] - vs[1][1], bz = vs[2][2] - vs[1][2];
      const N = [ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx];
      for (let a = 0; a < pnv; a++) {
        const p1 = vs[a], p2 = vs[(a + 1) % pnv];
        P.edges.push(plane3(p1, p2, [p1[0] + N[0], p1[1] + N[1], p1[2] + N[2]]));
      }
      P.ok = P.plane !== null && P.edges.every(e => e !== null);
      polys[q] = P;
    }
    this.npolys = npoly;
    this.pgrid = buildGrid(npoly, (i) => {
      const vs = polys[i].v;
      let x0 = Infinity, x1 = -Infinity, z0 = Infinity, z1 = -Infinity;
      for (const p of vs) { if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0]; if (p[2] < z0) z0 = p[2]; if (p[2] > z1) z1 = p[2]; }
      return [x0, x1, z0, z1];
    });
    this.npcells = this.pgrid.used;
  }

  _cells(xmin, zmin, xmax, zmax) { return gridCells(this.tgrid, xmin, zmin, xmax, zmax); }

  // ------------------------------------------------------------ walk mesh
  // FUN_002605c8: inside every edge plane.
  _inside(P, x, y, z, eps = 0.0) {
    for (const e of P.edges) if (e[0] * x + e[1] * y + e[2] * z + e[3] > eps) return false;
    return true;
  }

  // A polygon the walker never enters (navMove's neighbour rules): 0x04/0x40/0x100,
  // and 0x80 for block80 bodies. 0x10 (drop) polygons are entered, so they count.
  _isWall(P) { return (P.flags & F_WALLS) !== 0 || (this.block80 && (P.flags & F_NPC) !== 0); }

  // First walk polygon hit by the vertical segment yTop -> yBot (yTop < yBot,
  // -Y up): the smallest y >= yTop. Returns [poly, y] or null. yTop null: no limit.
  // Skips polygons that are walls for this body unless all = true (locate: the
  // game's own search, FUN_0026f750, tests no flags).
  findPoly(x, z, yTop, yBot, all = false) {
    const g = this.pgrid, m = gridCells(g, x, z, x, z), list = g.list;
    let bi = -1, by = 0;
    for (let q = 0; q < m; q++) {
      const i = list[q], P = this.polys[i];
      if (!P.ok || Math.abs(P.plane[1]) < 1e-6 || (!all && this._isWall(P))) continue;
      const pl = P.plane;
      const y = -(pl[0] * x + pl[2] * z + pl[3]) / pl[1];
      if ((yTop !== null && y < yTop) || y > yBot) continue;
      if (!this._inside(P, x, y, z, INSIDE_EPS)) continue;
      if (bi < 0 || y < by) { bi = i; by = y; }
    }
    return bi < 0 ? null : [bi, by];
  }

  // The polygon a walking body at the feet is on: nearest the feet between
  // SEARCH_UP above and maxDrop below (cid_query.py nearest_poly). Skips walls.
  nearestPoly(x, y, z, maxDrop) {
    const g = this.pgrid, m = gridCells(g, x, z, x, z), list = g.list;
    let bi = -1, by = 0, bd = 0;
    for (let q = 0; q < m; q++) {
      const i = list[q], P = this.polys[i];
      if (!P.ok || Math.abs(P.plane[1]) < 1e-6 || this._isWall(P)) continue;
      const pl = P.plane;
      const py = -(pl[0] * x + pl[2] * z + pl[3]) / pl[1];
      if (py < y - SEARCH_UP || py > y + maxDrop) continue;
      if (!this._inside(P, x, py, z, INSIDE_EPS)) continue;
      const d = Math.abs(py - y);
      if (bi < 0 || d < bd) { bi = i; by = py; bd = d; }
    }
    return bi < 0 ? null : [bi, by];
  }

  // FUN_0035cbc8(out, hit, point, 0): the segment from SEARCH_UP above the point
  // (DAT_004a2178 = (0, -7.5, 0)) to SEARCH_DOWN (200.0) below it, over the
  // tag-0 faces of every loaded file (FUN_0026a668); the smallest-t hit wins and
  // no flag is tested.
  locate(x, y, z) { return this.findPoly(x, z, y - SEARCH_UP, y + SEARCH_DOWN, true); }

  // FUN_0035d788: closest point on the polygon's edges.
  _clamp(P, x, y, z) {
    let bd = null, q = null;
    const n = P.nv;
    for (let a = 0; a < n; a++) {
      const [ax, ay, az] = P.v[a], [bx, by, bz] = P.v[(a + 1) % n];
      const ex = bx - ax, ey = by - ay, ez = bz - az;
      const ll = ex * ex + ey * ey + ez * ez;
      const t = ll > 0 ? ((x - ax) * ex + (y - ay) * ey + (z - az) * ez) / ll : 0.0;
      let c;
      if (t < 0.0) c = [ax, ay, az];
      else if (t > 1.0) c = [bx, by, bz];
      else c = [ax + ex * t, ay + ey * t, az + ez * t];
      const d = (x - c[0]) ** 2 + (y - c[1]) ** 2 + (z - c[2]) ** 2;
      if (bd === null || d < bd) { bd = d; q = c; }
    }
    return q;
  }

  // FUN_0035d4e8: first edge plane crossed from p to c (the move flattened onto
  // the polygon plane). Returns [edge, x, y, z]; edge -1 and p if none.
  _exit(P, px, py, pz, cx, cy, cz) {
    const pl = P.plane;
    let dx = cx - px, dy = cy - py, dz = cz - pz;
    const dn = dx * pl[0] + dy * pl[1] + dz * pl[2];
    dx = dx - pl[0] * dn; dy = dy - pl[1] * dn; dz = dz - pl[2] * dn;
    let bt = null, best = [-1, px, py, pz];
    for (let a = 0; a < P.edges.length; a++) {
      const e = P.edges[a];
      const sp = e[0] * px + e[1] * py + e[2] * pz + e[3];
      const sc = e[0] * cx + e[1] * cy + e[2] * cz + e[3];
      if (sp <= EDGE_EPS && sc > 0.0) {
        const den = e[0] * dx + e[1] * dy + e[2] * dz;
        if (den === 0.0) continue;
        const t = -sp / den;
        if (bt === null || t < bt) { bt = t; best = [a, px + dx * t, py + dy * t, pz + dz * t]; }
      }
    }
    return best;
  }

  // FUN_00260628 along the plane normal: the point on P's plane.
  _project(P, x, y, z) {
    const pl = P.plane;
    const s = pl[0] * x + pl[1] * y + pl[2] * z + pl[3];
    return [x - pl[0] * s, y - pl[1] * s, z - pl[2] * s];
  }

  // FUN_00357d48 over the walk mesh from polygon pi at (x,y,z) by (dx, 0, dz)
  // (cid_query.py nav_move). Returns [x, y, z, poly, hitWall, fall].
  navMove(pi, x, y, z, dx, dz, air = false) {
    let P = this.polys[pi];
    let mx = dx, my = 0.0, mz = dz;
    let prev = null, hit = false;
    for (let it = 0; it < NAV_PASSES; it++) {
      if (!this._inside(P, x, y, z)) [x, y, z] = this._clamp(P, x, y, z);   // FUN_0035d788
      const L = Math.sqrt(mx * mx + my * my + mz * mz);
      if (L < 0.001) break;
      const pl = P.plane;
      // FUN_002606b8 (along the up axis) then rescale to the length L
      if (Math.abs(pl[1]) > 1e-9) my = my - (pl[0] * mx + pl[1] * my + pl[2] * mz) / pl[1];
      const l2 = Math.sqrt(mx * mx + my * my + mz * mz);
      if (l2 > 0.0) { mx = mx / l2 * L; my = my / l2 * L; mz = mz / l2 * L; }
      const cx = x + mx, cy = y + my, cz = z + mz;
      if (this._inside(P, cx, cy, cz)) { [x, y, z] = this._project(P, cx, cy, cz); break; }
      const [e, ex, ey, ez] = this._exit(P, x, y, z, cx, cy, cz);
      if ((ex - x) ** 2 + (ey - y) ** 2 + (ez - z) ** 2 > mx * mx + my * my + mz * mz) {
        [x, y, z] = this._clamp(P, cx, cy, cz);
        break;
      }
      if (e < 0) break;
      let nb = P.nbr[e];
      if (nb !== null && (nb === prev || !this.polys[nb].ok)) nb = null;
      let fall = false;
      if (nb !== null) {
        const N = this.polys[nb];
        if ((N.flags & F_NPC) && this.block80) nb = null;
        else if (N.flags & F_WALLS) nb = null;
        else if (!air && (N.flags & F_DROP)) {
          const k = N.nbr.indexOf(pi);
          if (k >= 0 && N.edges[k][1] < 0.0) fall = true;        // FUN_00357148: drop off
          else nb = null;
        }
      }
      if (nb !== null) {
        x = ex; y = ey; z = ez;
        mx = cx - ex; my = cy - ey; mz = cz - ez;
        prev = pi; pi = nb; P = this.polys[nb];
        if (fall) return [x, y, z, pi, hit, true];
        if (mx * mx + my * my + mz * mz === 0.0) break;
        continue;
      }
      // boundary edge
      x = ex; y = ey; z = ez;
      mx = cx - ex; my = cy - ey; mz = cz - ez;
      // 0x02 on the current polygon (0x358c60..0x358c8c): FUN_00359f38(body, &done,
      // end) with end = pass start + move (the stack slot built at 0x358350), so
      // the search is at the end point c. A different polygon found: the body is
      // put at the hit point on it and the pass loop ends (done = 1); the rest of
      // the move is not wall-tested. More than DROP_FALL lower than the crossing
      // point: ground states start a fall (FUN_00357148).
      if (P.flags & F_CHUNK) {
        const f = this.locate(cx, cy, cz);
        if (f !== null && f[0] !== pi) {
          const fy = f[1];
          return [cx, fy, cz, f[0], hit, fy - y > DROP_FALL];
        }
      }
      const a = P.v[e], b = P.v[(e + 1) % P.nv];
      let ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
      let keepY = 0;
      if (air) { uy = 0.0; keepY = my; my = 0.0; }               // state 2: horizontal slide
      const lu = Math.sqrt(ux * ux + uy * uy + uz * uz);
      if (lu > 0.0) { ux = ux / lu; uy = uy / lu; uz = uz / lu; }
      const s = mx * ux + my * uy + mz * uz;                      // FUN_0025a778(0, ...): onto the edge
      mx = ux * s; my = uy * s; mz = uz * s;
      const k = -Math.sqrt(mx * mx + my * my + mz * mz) * SLIDE_IN;
      const en = P.edges[e];
      mx = mx + en[0] * k; mz = mz + en[2] * k;
      if (air) my = keepY;
      else my = my + en[1] * k;
      hit = true;
      if (mx * mx + my * my + mz * mz === 0.0) break;
    }
    return [x, y, z, pi, hit, false];
  }

  // ------------------------------------------------------------------ ground
  // The walk polygon under (x,z): first hit going down from feetY - stepUp to
  // feetY + SEARCH_DOWN. feetY null/undefined: the topmost polygon.
  // Returns {y, tri, poly, normal, surface, subtree, mask} or null (tri = poly
  // index, surface = mask = its flags; subtree 0).
  groundHit(x, z, feetY, stepUp = SEARCH_UP) {
    const f = this.findPoly(x, z, feetY == null ? null : feetY - stepUp, feetY == null ? Infinity : feetY + SEARCH_DOWN);
    if (f === null) return null;
    const P = this.polys[f[0]];
    return { y: f[1], tri: f[0], poly: f[0], normal: [P.plane[0], P.plane[1], P.plane[2]],
             surface: P.flags, subtree: 0, mask: P.flags };
  }

  groundY(x, z, feetY, stepUp = SEARCH_UP) {
    const g = this.groundHit(x, z, feetY, stepUp);
    return g === null ? null : g.y;
  }

  // pos {x,y,z} = feet; delta {x,z} = horizontal move this tick (cid_query.py move).
  // Off the mesh (no polygon from SEARCH_UP above to maxDrop below): stay,
  // onGround false (the caller falls). A drop: onGround false at the drop point.
  // opts.air: walk the ground point only; y unchanged, onGround false. Airborne
  // and off the mesh: stay too. FUN_00357d48 skips the whole walk when the body
  // has no polygon (body+0x110 == 0), and an airborne body keeps the polygon it
  // took off from, so the game's body never moves off the mesh.
  // opts.radius/height: unused (the polygons are the area the body's centre may use).
  move(pos, delta, opts = {}) {
    const maxDrop = opts.maxDrop ?? 40, air = !!opts.air;
    const f = air ? this.findPoly(pos.x, pos.z, pos.y - SEARCH_UP, pos.y + SEARCH_DOWN)
                  : this.nearestPoly(pos.x, pos.y, pos.z, maxDrop);
    if (f === null) {
      return { x: pos.x, y: pos.y, z: pos.z, onGround: false, hitWall: false };
    }
    const r = this.navMove(f[0], pos.x, f[1], pos.z, delta.x, delta.z, air);
    if (air) return { x: r[0], y: pos.y, z: r[2], onGround: false, hitWall: r[4] };
    return { x: r[0], y: r[1], z: r[2], onGround: !r[5], hitWall: r[4] };
  }

  // -------------------------------------------------------------- walls
  // Push a vertical capsule out of the triangle set's walls, horizontally only
  // (cid_query.py capsule_push). Not the game's walk; kept for the airborne
  // helpers. Returns [x, z, hit].
  capsulePush(x, feetY, z, radius = 3.5, height = HEIGHT, stepUp = STEP_UP, iters = 3) {
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
