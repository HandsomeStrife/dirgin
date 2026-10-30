// Lock-on and auto-aim: FUN_00329778's lock branch (CAM+0x1e8 bit 2) -> FUN_00328968, and the two
// object probes it uses (FUN_005449f0 index 2 / 1 -> actor vt+0x17c = FUN_0055bda8 modes 2 / 1).
// notes/aim-research.md §4 "Lock-on" (worker 0141). Pure: the page (aim.js) passes this frame's
// ray, the soldiers and the world probe.
//
// What the game does each frame with the gun out (and also holstered: the lock runs whenever the
// camera's aim ray does):
//   1. probe 2 along the reticle ray S..S+F x max(1050, CAM+0x1b8): a soldier whose lock point
//      (vt+0x5c = FUN_00563720: the world position of bone C+0x765) lies within 1.5 x radius of the
//      ray, ahead of S, on screen and in line of sight. Within CAM+0x1b0 of S it becomes the target
//      (CAM+0x1e4 = its id; beep 0x4a05d0 on the first lock, 0x4a05d8 on a switch).
//   2. otherwise probe 1 (world + soldier cylinders); a soldier hit within range is locked the same way.
//   3. otherwise the old target is kept while it is alive and lockable (FUN_0055ef38), its lock point
//      within 1.1 x CAM+0x1b0 of S, within FOV x pi/240 x CAM+0x1b4 x 0.875 of the ray (19.3 deg at FOV
//      73.5) and its projected point inside x (-60, 572), y (-110, 558) of the 512 x 448 screen; or
//      always while the weapon state P+0x2a4 is 2 or 4 (FUN_00577458).
//   4. dropped: CAM+0xa0 = the probe's hit, or the far point CAM+0xb0.
//   While locked, CAM+0xa0 = the target's lock point (FUN_00329778): the weapon's aim point P+0x2b0
//   (FUN_005784d8), so the shots fly at the target whatever the reticle is on. The reticle does not
//   move (HUD+0x7c = 0) and the camera does not turn: the turn toward the target in FUN_0032ace0
//   (l.442-536, FUN_0033c018(HUD, 1)) needs HUD+0x64 bit 0x40, clear in this save (trace k_lock13).
const f = Math.fround;
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const mul = (a, k) => [a[0] * k, a[1] * k, a[2] * k];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const len = a => Math.sqrt(dot(a, a));
const unit = a => { const l = len(a); return l > 0 ? mul(a, 1 / l) : [0, 0, 0]; };

export const LOCK_FAR = 1050.0;          // FUN_00328968 / FUN_00329778: far = max(1050, CAM+0x1b8)
export const LOCK_KEEP = 1.1;            // keep within 1.1 x CAM+0x1b0
export const LOCK_CONE_K = 0.013089971;  // pi / 240 (x FOV x CAM+0x1b4 x DAT_0046e360)
export const FOV_K = 0.875;              // DAT_0046e360
export const PROBE2_R = 1.5;             // FUN_0055bda8 mode 2: within 1.5 x actor +0x2c of the ray
export const LOCK_TURN0 = 0.05;          // CAM+0x218 = 0x3d4ccccd on every lock change
// screen bounds of the keep test (FUN_00328968) and of probe 2 (FUN_0055bda8), the non-0x1e video mode
export const KEEP_BOX = [-60, 572, -110, 558];
export const PROBE2_BOX = [-50, 562, -100, 548];

export function createLock() {
  // CAM+0x1e4 target, +0x1e6 target / under the reticle, +0x1e8 bits 0x10 / 0x20 (lock / still
  // bookkeeping), +0x218 turn gain; HUD+0x64 bits 2 (on target) / 0x10 (locked); P+0x272 (FUN_00573378)
  return { id: -1, id6: -1, b10: false, b20: false, f218: 0, hudOn: false, hudLocked: false, p272: -1, beeps: [] };
}

// FUN_0055bda8 mode 2 (actor +0x118 bit 0x1000000 clear): the closest point on S..E to the lock point
// (FUN_002614f4: t = (C - S).D / D.D), a hit when t > 0 and that point is within 1.5 x radius; then the
// lock point on screen (PROBE2_BOX) and the line of sight: probe 3 from S to FUN_005634e0(actor, 3)
// (actor +0x30 - 2 up the body) finds nothing or this actor. Hit point = the lock point, normal =
// unit(S - lock point). GUESS: FUN_0033c018(HUD, 0) false (the 3D branch; the flat one needs HUD+0x64
// bit 0x40, clear in the save); probe 3 = the caller's world segment only (its object classes are
// not decoded; other soldiers don't block).
function probe2Actor(a, S, E, c) {
  const C = a.point, D = sub(E, S);
  const t = dot(sub(C, S), D) / dot(D, D);
  const q = add(S, mul(D, t));
  if (t <= 0 || a.radius * PROBE2_R <= len(sub(q, C))) return null;
  if (!onScreen(c.screen(C), PROBE2_BOX)) return null;
  if (c.blocked && c.blocked(S, a.losPoint)) return null;
  return { a, hit: C.slice(), normal: unit(sub(S, C)) };
}
// FUN_0055bda8 mode 1: dead actors are skipped (FUN_00326500 true in the save, FUN_00316cf8); the
// cylinder (feet .. height up, radius): FUN_0025c0f8 crossing S..E, then the crossing E..S, hit = their
// midpoint (FUN_002611f4(0.5, ...)); no crossing but S inside (FUN_0025c770) -> hit = S. No part test
// (that is mode 0's, the bullets'). The caller's cylinder(r, p0, p1, A, B) is enemy_hit.js cylinderHit.
function probe1Actor(a, S, E, c) {
  if (!a.alive) return null;
  const A = a.feet, B = [a.feet[0], a.feet[1] - a.height, a.feet[2]];
  const h = c.cylinder(a.radius, S, E, A, B);
  if (!h) return null;
  const back = c.cylinder(a.radius, E, S, A, B);
  if (back && (h.point[0] !== S[0] || h.point[1] !== S[1] || h.point[2] !== S[2])) {
    return { a, hit: mul(add(h.point, back.point), 0.5), normal: null };
  }
  return { a, hit: h.point.slice(), normal: null };
}
// FUN_005449f0(world, k, ...): the closest object whose vt+0x17c hits, by |S - hit|, minus the
// actor's radius +0x2c for actors (vt+0x8c set) in probes 1 / 2. Objects need +0x28 bit 0 (active:
// the caller passes active soldiers only) and +0x38 bit k. Probe 1 also sees the room (the caller's
// world segment; GUESS: the room is one object with no radius term).
function probe(k, S, E, c) {
  let best = null, bd = Infinity;
  for (const a of c.actors) {
    if (!(a.probeBits & (1 << k))) continue;
    const h = k === 2 ? probe2Actor(a, S, E, c) : probe1Actor(a, S, E, c);
    if (!h) continue;
    const d = len(sub(S, h.hit)) - a.radius;
    if (d < bd) { bd = d; best = h; }
  }
  if (k === 1 && c.world) {
    const w = c.world(S, E);
    if (w && len(sub(S, w)) < bd) best = { a: null, hit: w.slice(), normal: null };
  }
  return best;
}
const onScreen = (p, b) => !!p && b[0] < p[0] && p[0] < b[1] && b[2] < p[1] && p[1] < b[3];

// FUN_00328968(CAM, S, F) then the rest of FUN_00329778's lock branch. L = createLock(); c = {
//   S, F: the ray (FUN_00329778: S = eye + F (|eye - CAM+0xf0| + 10)), far: CAM+0xb0 (S + F x 10000),
//   r1b0, r1b8: CAM+0x1b0 / +0x1b8 (FUN_00575510), fov: CAM+0x170, k1b4: CAM+0x1b4, look: [CAM+0x190, +0x194],
//   noLock: player status +0x64 bit 4 (FUN_0055f0e0(P, 4)), st2a4: P+0x2a4, drawn: P+0x2a8 bit 0,
//   actors: [{ id, alive, lockable, probeBits, point (vt+0x5c), feet, radius, height, losPoint }],
//   world(S, E) -> point | null, blocked(A, B) -> bool, cylinder: enemy_hit cylinderHit,
//   screen(p) -> [x, y] on the 512 x 448 HUD grid (FUN_0040d7f0) }
// Returns CAM+0xa0 (the aim point).
export function lockFrame(L, c) {
  const S = c.S, far = Math.max(LOCK_FAR, c.r1b8);
  const E = add(S, mul(c.F, far));
  const byId = id => c.actors.find(a => a.id === id) || null;               // FUN_0053e040
  const noLock = !!c.noLock;
  let inRange = false, cand = null, pr = probe(2, S, E, c);
  if (pr && pr.a && len(sub(S, pr.hit)) <= c.r1b0) { cand = pr; inRange = true; }
  if (!inRange) {                                                            // LAB_00328af8
    pr = probe(1, S, E, c);
    cand = pr && pr.a && (pr.a.probeBits & 4) ? pr : null;                   // +0x38 bit 2
  }
  // HUD "on target": the candidate within CAM+0x1b8 (and an actor: +0x28 bit 0x1000 clear)
  const hudOn = !!(cand && cand.a && len(sub(S, cand.hit)) < c.r1b8);
  let changed = false, keep = false, acquired = false;
  if (!noLock && cand && cand.a && len(sub(S, cand.hit)) < c.r1b0) {
    if (L.id !== cand.a.id) {
      L.beeps.push(L.id === -1 ? 0x4a05d0 : 0x4a05d8);                       // FUN_0033cc48
      L.id = cand.a.id; L.b20 = false; L.b10 = true; changed = true;
    }
    acquired = true;                                                         // goto LAB_00328ff8
  }
  if (!acquired) {
    const still = c.look[0] === 0 && c.look[1] === 0;                        // CAM+0x190 / +0x194
    if (still) { if (!L.b20) L.b20 = true; } else if (L.b20) L.b10 = false;
    if (L.id !== -1 && !noLock) {
      const t = byId(L.id);
      // flag 0x80000 (auto-lock option 2) and the target within 60 of the eye: kept; not this save
      if (t && t.alive && t.lockable) {                                      // vt+0x204 = FUN_0055ef38
        if (c.st2a4 === 2 || c.st2a4 === 4) keep = true;                     // FUN_00577458
        else {
          const v = sub(t.point, S);                                         // vt+0x5c - S
          if (len(v) < c.r1b0 * LOCK_KEEP) {
            const cone = f(f(f(c.fov * LOCK_CONE_K) * c.k1b4) * FOV_K);
            // GUESS: the screen test projects the target point. The game projects the normalised
            // direction (vt+0x5c - S) through FUN_0040d7f0 / FUN_00444a10, whose result depends on its w
            // (not decoded; notes/fidelity-audit.md 'Lock keep screen test')
            if (angle(unit(v), c.F) < cone) keep = onScreen(c.screen(t.point), KEEP_BOX);
          }
        }
      }
    }
    // FUN_00326500 and FUN_003251f8 and not FUN_00577458 and FUN_0040d778: GUESS FUN_003251f8 false
    // (as in enemy_ai playerShot), so this drop is never taken
    if (!keep) {
      L.id = -1;
      L.aim = pr ? pr.hit.slice() : c.far.slice();                           // lVar9: the last probe's hit
      changed = true;
    }
  }
  L.hudLocked = L.id !== -1;                                                 // FUN_0033c340
  L.hudOn = L.id !== -1 ? true : hudOn;                                      // FUN_0033c2b8
  if (changed) L.f218 = LOCK_TURN0;
  // FUN_00329778 after FUN_00328968: the target gone -> -1; locked -> CAM+0xa0 = its lock point
  // (+ 0.5 x C+0x410 only for a target with a parent object, actor +8: none on the page, trace 0)
  if (L.id !== -1 && !byId(L.id)) L.id = -1;
  if (L.id !== -1) L.aim = byId(L.id).point.slice();
  L.id6 = L.id;
  L.p272 = L.id !== -1 ? (c.drawn ? L.id : L.p272) : -1;                     // FUN_00573378 (P+0x2a8 bit 0)
  return L.aim;
}
// FUN_00329778: the lock ray F = row 2 of rotX(CAM+0x174 + 5 x CAM+0x1fc + CAM+0x1cc) x rotY(CAM+0x178 +
// 5 x CAM+0x200) (FUN_00259928 / FUN_00259990, product FUN_0026125c; DAT_0046e428 = 5): built from the
// view angles, not the camera matrix (trace k_lock13: 2-9 deg from CAM+0x80 while the pad's reticle
// lead CAM+0x1fc / +0x200 is set). The page's mouse leaves the lead at 0 (as the game's USB mouse:
// FUN_0032ace0 l.1700-1718 skips it when FUN_00254db0), so F = (pitch + CAM+0x1cc, yaw).
export function lockRay(pitch, yaw, c1cc, a1fc = 0, a200 = 0) {
  const p = f(f(pitch + f(5.0 * a1fc)) + c1cc), y = f(yaw + f(5.0 * a200));
  return [Math.cos(p) * Math.sin(y), -Math.sin(p), Math.cos(p) * Math.cos(y)];
}

// FUN_00261038: the angle between two vectors (acos of the normalised dot; pi when opposite)
export function angle(a, b) {
  const d = dot(a, b) / Math.sqrt(dot(a, a) * dot(b, b));
  return d >= 1 ? 0 : d <= -1 ? Math.PI : Math.acos(d);
}

// FUN_0040d7f0 for the page: camera rows M (FollowCamera.M: 0 = x, 1 = y (down), 2 = forward) and the
// eye; 512 x 448 HUD grid, centre (256, 224). L: the GS projection's matrices (view +0x340 / +0x380)
// are not read; the scale is the one FUN_0032ace0 (l.517-535) uses for screen <-> angle: half width
// = tan(FOV x DAT_0046e364 (1.0) / 2), half height = tan(FOV x DAT_0046e360 (0.875) / 2).
export function screenPoint(M, eye, fov, p) {
  const v = sub(p, eye), z = dot(v, M[2]);
  if (!(z > 0)) return null;                                                  // behind the eye: off screen
  const tx = Math.tan(fov * 0.017453294 * 0.5), ty = Math.tan(fov * FOV_K * 0.017453294 * 0.5);
  return [256 + 256 * dot(v, M[0]) / z / tx, 224 + 224 * dot(v, M[1]) / z / ty];
}
