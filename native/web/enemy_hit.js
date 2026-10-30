// Bullet vs actor geometry for enemy_ai.js (pure: no three.js). Translations, addresses per function:
//   cylinderHit  FUN_0055bda8's capsule step: FUN_0025c0f8 (segment vs the actor's cylinder), then
//                FUN_0025c770 (segment start inside it)
//   partTest     FUN_005ad118 -> FUN_0030baa0: the segment against the model's part boxes ('mkd'
//                chunk box array, ai.json hitboxes), closest box wins; -1 = none
//   modelPose    the local-pose compose of player.js SpeedAnimator.poseLocal (FUN_005a9db0 at one
//                clip, weight 1), for tests and headless callers; the page reads the live pose
//   boneRows     a bone's world matrix (row-vector rows, as the game's) from a model-space pose
// Decode notes: notes/enemy-research.md §13.

const f = Math.fround;
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const mul = (a, k) => [a[0] * k, a[1] * k, a[2] * k];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const len = a => Math.sqrt(dot(a, a));

// DAT_0046dc10: the unit cube's 8 corners (+-1); DAT_0046dc90: its 6 faces, 4 corner indices each
// (ELF data, read with the ELF's PT_LOAD map)
export const CUBE = [[-1, -1, -1], [1, -1, -1], [1, -1, 1], [-1, -1, 1], [-1, 1, -1], [1, 1, -1], [1, 1, 1], [-1, 1, 1]];
export const FACES = [[0, 1, 2, 3], [4, 7, 6, 5], [0, 3, 7, 4], [2, 1, 5, 6], [1, 0, 4, 5], [3, 2, 6, 7]];
const FLT_MAX = 3.4028234663852886e38;   // DAT_0049fe20 (start of the closest-t search)

// row-vector transform: p' = p.x r0 + p.y r1 + p.z r2 + r3 (FUN_0026138c with w = 1)
const xf = (M, p) => [0, 1, 2].map(k => p[0] * M[0][k] + p[1] * M[1][k] + p[2] * M[2][k] + M[3][k]);

// FUN_00259580(quad, 4, q): q on the face polygon when the edge crosses (p[i+1] - p[i]) x (p[i+1] - q)
// of consecutive edges never point against each other (dot < 0 -> outside)
function inPolygon(pts, q) {
  const n = pts.length, c = [];
  for (let i = 0; i < n; i++) {
    const j = i + 1 === n ? 0 : i + 1;
    c.push(cross(sub(pts[j], pts[i]), sub(pts[j], q)));
  }
  for (let i = 1; i < n; i++) if (dot(c[i - 1], c[i]) < 0) return false;
  return true;
}

// The box's world corners: corner x local x bone x model (FUN_0030baa0: local = diag(scale) with
// row 3 = offset, FUN_0026125c twice; bone = FUN_005b29c0 from the posed skeleton, model = MP+0x10).
// boneWorld = bone x model as rows.
export function boxCorners(box, boneWorld) {
  const [sx, sy, sz] = box.scale, o = box.offset;
  return CUBE.map(c => xf(boneWorld, [c[0] * sx + o[0], c[1] * sy + o[1], c[2] * sz + o[2]]));
}

// FUN_0030baa0(mkd, p0, p1, model matrix, skd): every box, every face: the plane through corner 0
// with normal (c0 - c1) x (c0 - c3); t = (n.c0 - n.p0) / (n.(p1 - p0)); 0 <= t <= 1 and the point on
// the face polygon -> a candidate; the smallest t wins (strictly smaller: the first box on a tie).
// boneWorld(bone) -> rows. Returns the box index (= the part), or -1.
export function partTest(boxes, boneWorld, p0, p1) {
  const d = sub(p1, p0);
  let best = FLT_MAX, part = -1;
  for (let i = 0; i < boxes.length; i++) {
    const c = boxCorners(boxes[i], boneWorld(boxes[i].bone));
    for (const [a, b, e, g] of FACES) {
      const n = cross(sub(c[a], c[b]), sub(c[a], c[g]));
      const t = (dot(c[a], n) - dot(p0, n)) / dot(d, n);   // n.d = 0: +-inf / NaN, never in [0, 1]
      if (!(t >= 0 && t <= 1)) continue;
      if (inPolygon([c[a], c[b], c[e], c[g]], add(mul(d, t), p0)) && t < best) { part = i; best = t; }
    }
  }
  return part;
}

// FUN_0055bda8 (param 2 = 0), the capsule step: FUN_0025c0f8(r, out, n, p0, p1, A, B) = the segment
// crosses the closed cylinder A..B of radius r (its two end discs, FUN_00260628 / FUN_00260700 in
// [0, 1], or its side: the root pair of FUN_00260e28, axis parameter in [0, 1]); else FUN_0025c770(r,
// p0, A, B) = p0 inside it (distance to the axis line FUN_00261498 <= r, FUN_002614f4 in [0, 1]),
// and the hit point is p0. The three early outs of FUN_0025c0f8 (bounding distances) can't reject a
// crossing, so they are left out.
// Returns null or { point } (L: the point is the first crossing along the segment; the game tries
// the end discs before the side, which gives the same point for a segment that enters once).
export function cylinderHit(r, p0, p1, A, B) {
  const ax = sub(B, A), L = len(ax);
  if (!(L > 0)) return null;
  const u = mul(ax, 1 / L), d = sub(p1, p0);
  const w0 = sub(p0, A);
  const h0 = dot(w0, u), hd = dot(d, u);                         // axis coordinate along the segment
  const radial = s => { const w = add(w0, mul(d, s)); const h = dot(w, u); return len(sub(w, mul(u, h))); };
  const ss = [];
  if (hd !== 0) for (const H of [0, L]) {                        // the end discs
    const s = (H - h0) / hd;
    if (s >= 0 && s <= 1 && radial(s) <= r) ss.push(s);
  }
  const wp = sub(w0, mul(u, h0)), dp = sub(d, mul(u, hd));       // the side: |wp + s dp| = r
  const qa = dot(dp, dp), qb = 2 * dot(wp, dp), qc = dot(wp, wp) - r * r;
  const disc = qb * qb - 4 * qa * qc;
  if (qa > 0 && disc >= 0) for (const sg of [-1, 1]) {
    const s = (-qb + sg * Math.sqrt(disc)) / (2 * qa), h = h0 + s * hd;
    if (s >= 0 && s <= 1 && h >= 0 && h <= L) ss.push(s);
  }
  if (ss.length) return { point: add(p0, mul(d, Math.min(...ss))) };
  if (len(wp) <= r && h0 >= 0 && h0 <= L) return { point: p0.slice() };   // FUN_0025c770
  return null;
}

// ---------------------------------------------------------------- pose (for tests / headless use)
const qmul = (a, b) => [a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]];
const qrot = (q, v) => {
  const [x, y, z, w] = q, uv = cross([x, y, z], v), uuv = cross([x, y, z], uv);
  return [v[0] + 2 * (w * uv[0] + uuv[0]), v[1] + 2 * (w * uv[1] + uuv[1]), v[2] + 2 * (w * uv[2] + uuv[2])];
};

// The same maths as player.js SpeedAnimator.sampleLocal + poseLocal (one clip, weight 1): t/s lerp
// and q nlerp (hemisphere flip) between the bracketing frames, then R = Rp q, S = Sp s,
// P = Pp + Rp (Sp t). lp = MODEL.json local_pose, data = Float32Array of MODEL_local.bin.
export function modelPose(lp, data, clip, frame) {
  const n = lp.bones, c = lp.clips[clip], par = lp.parent;
  if (!c) throw new Error(`modelPose: no clip ${clip}`);
  let i0 = Math.max(0, Math.min(Math.floor(frame), c.frames - 2));
  const u = frame - i0, o0 = (c.offset + i0 * n) * 10, o1 = o0 + n * 10;
  const R = [], S = [], P = [], done = new Uint8Array(n);
  const visit = i => {
    if (done[i]) return; done[i] = 1;
    const p = par[i] < 0 || par[i] >= n || par[i] === i ? -1 : par[i];
    if (p >= 0) visit(p);
    const k0 = o0 + i * 10, k1 = o1 + i * 10, A = new Array(10);
    for (let j = 0; j < 6; j++) A[j] = data[k0 + j] * (1 - u) + data[k1 + j] * u;
    const sg = data[k0 + 6] * data[k1 + 6] + data[k0 + 7] * data[k1 + 7] + data[k0 + 8] * data[k1 + 8] + data[k0 + 9] * data[k1 + 9] < 0 ? -1 : 1;
    let m = 0;
    for (let j = 6; j < 10; j++) { A[j] = data[k0 + j] * (1 - u) + sg * data[k1 + j] * u; m += A[j] * A[j]; }
    m = Math.sqrt(m) || 1;
    const q = [A[6] / m, A[7] / m, A[8] / m, A[9] / m];
    if (p < 0) { R[i] = q; S[i] = [A[3], A[4], A[5]]; P[i] = [A[0], A[1], A[2]]; return; }
    R[i] = qmul(R[p], q);
    S[i] = [S[p][0] * A[3], S[p][1] * A[4], S[p][2] * A[5]];
    P[i] = add(P[p], qrot(R[p], [S[p][0] * A[0], S[p][1] * A[1], S[p][2] * A[2]]));
  };
  for (let i = 0; i < n; i++) visit(i);
  return { R, S, P };
}

// A bone's world rows from its model-space TRS (the flat joint node: T(P) R S) and the body
// placement as enemies.js draws it: world = pos + Ry(atan2(face.x, face.z) + pi) x (play.js
// bodyYaw; the glb root's Rx 180 and enemies.js's flip cancel). rq = [x, y, z, w].
export function boneRows(rq, s, p, pos, face) {
  const h = Math.atan2(face[0], face[2]) + Math.PI, c = Math.cos(h), sn = Math.sin(h);
  const Y = v => [v[0] * c + v[2] * sn, v[1], -v[0] * sn + v[2] * c];
  const rows = [0, 1, 2].map(k => { const e = [0, 0, 0]; e[k] = s[k]; return Y(qrot(rq, e)); });
  rows.push(add(Y(p), pos));
  return rows;
}

// FUN_0025a058(s, out, dir): tilt dir by (r1 - 0.5) x s degrees about p = unit(dir x e), e = the unit
// axis picked from dir (|y| <= |x| or |z| <= |x|: z if |z| <= |y| else y; otherwise x), then spin
// the result about dir by 2 pi r2. r1, r2 = two FUN_0040b5c0 draws in that order. Rotations are
// right-handed (FUN_00259f60 + FUN_0026135c; signs checked on the k_bullet / k_hits12 call logs).
export function cone(s, dir, rand) {
  const r1 = rand();
  const d = mul(dir, 1 / len(dir));
  const ax = Math.abs(d[0]), ay = Math.abs(d[1]), az = Math.abs(d[2]);
  const e = (ay <= ax || az <= ax) ? (az <= ay ? [0, 0, 1] : [0, 1, 0]) : [1, 0, 0];
  const pc = cross(d, e), p = mul(pc, 1 / len(pc));
  const d1 = rodrigues(d, p, f(f(f(r1 - 0.5) * s) * f(0.017453294)));
  const r2 = rand();
  return rodrigues(d1, d, f(f(r2 + r2) * f(3.1415927)));
}
function rodrigues(v, k, a) {
  const c = Math.cos(a), s = Math.sin(a), kv = cross(k, v), kd = dot(k, v) * (1 - c);
  return [0, 1, 2].map(i => v[i] * c + kv[i] * s + k[i] * kd);
}
