// The page's enemy activation path (play.js -> enemy_ai.js onActivate / onDeactivate -> enemies.js
// add / remove), in a headless browser on the real page:
//   1. at the save the save's seven live soldiers are drawn, records 44-49 and 66 (T: k_enemy_engage8
//      f760), each at its placement and driven by its AI actor in the save's actor state;
//   1b. walking Vincent (W, steered with A / D) into #66: the push-out keeps them apart (enemy_ai pushOut);
//   1c. then idle beside him on open floor: the push (FUN_0035a440 adds it every frame) eases him clear
//       of #66 and then stops (no slide);
//   2. walking east across trigger 56 (x -1228 on the start deck) makes enemies.js draw two new
//      bodies, records 50 / 51 (script 91 event 0xd: cmd 0x108 on both; notes/enemy-activation.md §2);
//   3. the save's dead records (41-43) refuse activation (FUN_00559748), so no body appears;
//   4. an e030 record (71) activated by cmd 0x108 gets a body and an AI actor (enemy_ai AI_TYPES), and
//      cmd 0x130 removes it again; switching off the newest body (51) first, 71 is drawn right in front
//      of 50;
//   5. Vincent's bullets (aim.js onShot -> enemy_ai playerShot): the part test's pose (enemies.js
//      partPose) equals the drawn joints; the gun drawn with R1, the camera turned until the reticle ray
//      crosses #66's chest: the lock takes him (aim point = his bone 3); the camera turns until the ray
//      passes beside his body (yaw increasing: the keep test holds the lock, then drops it; yaw
//      decreasing: R1 fires while it holds): with the auto-lock option off the same shot leaves #66
//      (HP 1) alive, with the lock he dies (actor state 7, AI mode 0, death clip held);
//   6. an e048 (record 24, w class 2: stance 0x1e) gets a body and an AI actor, drawn in its stance's
//      clip (m_gst) and posed for the part test (enemies.js partPose); after 5, so it doesn't shift #66;
//   7. record 51 (Dan's report): mode 1, the fire clip's blend-out against k_fire_emp15;
//   8. record 69 hit (script event 8 -> AI mode 5) keeps firing; with Vincent close he attempts the melee
//      attack (m_atk drawn);
//   9. a shot over the platform (trigger 77, bullets only) wakes records 46-49; no console errors or page errors.
//   node native/web/test_enemies_page.mjs          (needs data/enemies + playwright; else SKIP, exit 0)
// Serves native/web with python3 http.server on a free port. three.js comes from the CDN (play.html importmap).
import { existsSync, readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { spawn, execSync } from 'node:child_process';
import net from 'node:net';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
let fail = 0;
const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) fail++; };
const done = () => { console.log(fail ? `${fail} FAILED` : 'all ok'); process.exit(fail ? 1 : 0); };

if (!existsSync(path.join(here, 'data/enemies/ai.json')) || !existsSync(path.join(here, 'data/enemies/e027.glb'))) {
  console.log('SKIP: data/enemies missing (tools/byo/build_web.sh)'); done();
}
let chromium;
try {
  const g = execSync('npm root -g', { encoding: 'utf8' }).trim();
  ({ chromium } = createRequire(path.join(g, '/'))('playwright'));
} catch { console.log('SKIP: playwright not installed (npm i -g playwright)'); done(); }

const port = await new Promise(r => { const s = net.createServer().listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => r(p)); }); });
const srv = spawn('python3', ['-I', '-m', 'http.server', String(port), '--bind', '127.0.0.1', '--directory', here], { stdio: 'ignore' });
const browser = await chromium.launch({ args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
try {
  const pg = await browser.newPage({ viewport: { width: 960, height: 540 } });
  const errs = [];
  pg.on('console', m => { if (m.type() === 'error') errs.push(m.text()); });
  pg.on('pageerror', e => errs.push(e.message));
  for (let i = 0; ; i++) {                                 // the server needs a moment
    try { await pg.goto(`http://127.0.0.1:${port}/play.html`, { waitUntil: 'load' }); break; }
    catch (e) { if (i > 20) throw e; await new Promise(r => setTimeout(r, 250)); }
  }
  await pg.waitForFunction(() => window.__play && window.__play.stats.ticks > 30, null, { timeout: 120000 });
  const recs = () => pg.evaluate(() => window.__play.enemies.actors.map(a => a.rec).sort((a, b) => a - b));
  const SAVE = [44, 45, 46, 47, 48, 49, 66];
  ok(JSON.stringify(await recs()) === JSON.stringify(SAVE), `at the save: records ${await recs()} drawn (want ${SAVE})`);
  // the platform soldiers (46-49 on the raised floor north-east of the ladder, 66 west of it): each at its
  // placement, with an AI actor in the save's actor state (#68 / #69 = 1, #70 / #71 = 2, #88 = 1)
  const save = await pg.evaluate(async () => {
    const P = window.__play, recs = await fetch('data/enemies/ai.json').then(r => r.json()).then(a => a.records);
    return [46, 47, 48, 49, 66].map(k => { const b = P.enemies.actors.find(a => a.rec === k), s = P.enemyAI.actors.find(a => a.rec === k);
      const p = b && b.body.position.toArray();
      return { k, err: p ? Math.hypot(p[0] - recs[k].pos[0], p[2] - recs[k].pos[2]) : null, inRoot: !!b && b.body.parent === P.root, astate: s ? s.astate : null }; });
  });
  const SAVE_STATE = { 46: 1, 47: 1, 48: 2, 49: 2, 66: 1 };
  for (const s of save) ok(s.inRoot && s.err !== null && s.err < 0.2 && s.astate === SAVE_STATE[s.k],
    `record ${s.k}: drawn ${s.err?.toFixed(2)} u from its placement, AI actor state ${s.astate} (save ${SAVE_STATE[s.k]})`);
  // the save's +0x118 and script vars (+0xa0) of the seven, read from the save state (object table
  // *(DAT_0046e880+0x1164)[rec + 22]); record 45 (#67) has 0x10000000 (its event 3's cmd 0x14c(28)), so
  // overlapping him, the push-out moves only Vincent (FUN_00542ab8). Skipped without the save state.
  {
    const saveP2s = process.env.SAVE_STATE ?? '/srv/shared/dirge-port/save-states/sea-of-flames.p2s';
    if (!existsSync(saveP2s)) console.log(`skip save +0x118 check: ${saveP2s} missing`);
    else {
      const py = `import sys,struct,json\nsys.path.insert(0,sys.argv[1])\nfrom p2s import read_p2s\nm=read_p2s(sys.argv[2])['eeMemory.bin']\n` +
        `u=lambda a:struct.unpack_from('<I',m,a)[0]\ntab=u(u(0x46e880)+0x1164)\n` +
        `print(json.dumps({k:[u(u(tab+4*(k+22))+0x118),list(struct.unpack_from('<8h',m,u(tab+4*(k+22))+0xa0))] for k in ${JSON.stringify(SAVE)}}))`;
      const want = JSON.parse(execSync(`python3 -I -c "$PY" ${path.resolve(here, '../../tools/trace')} ${saveP2s}`, { env: { ...process.env, PY: py }, encoding: 'utf8' }));
      const got = await pg.evaluate(SAVE => {
        const W = window.__play.enemyAI;
        const out = {};
        for (const k of SAVE) { const a = W.actors.find(x => x.rec === k); out[k] = [a.f118 >>> 0, [...a.vars]]; }
        const b = W.actors.find(x => x.rec === 45);
        const push = W.pushOut([b.pos[0] + 1, b.pos[1], b.pos[2]]);
        return { out, pushB: b.push.map(Math.abs).reduce((s, x) => s + x, 0), pushV: push.map(Math.abs).reduce((s, x) => s + x, 0) };
      }, SAVE);
      const bad = SAVE.filter(k => JSON.stringify(got.out[k]) !== JSON.stringify(want[k]));
      ok(bad.length === 0, `save +0x118 / vars of ${SAVE}: page = save state (${bad.length ? bad.map(k => `${k}: page ${JSON.stringify(got.out[k])} save ${JSON.stringify(want[k])}`).join('; ') : 'all equal'})`);
      ok(got.pushV > 0 && got.pushB === 0, `Vincent 1 u from #67: push on Vincent ${got.pushV.toFixed(3)} (> 0), on #67 ${got.pushB} (0: +0x118 bit 0x10000000)`);
    }
  }
  ok(await pg.evaluate(() => !!window.__play.enemyAI), 'enemy AI running');

  // 1b. Dan: "I can run through the soldiers". Vincent on the ladder-top deck, in line with #66 (record
  // 44) 30 u behind him along the camera's forward (~west; test aid: position write; the walk mesh is clear), then W held
  // (the start camera looks east) for 6 s; then he is put back where he was: the push-out (enemy_ai pushOut = FUN_00542ab8, player.js integrate) keeps the
  // two cylinders apart (5 + 5; FUN_0035a440 adds the push to his step, player.js pushIn; test_push_out
  // checks it against k_push16).
  const push = await pg.evaluate(async () => {
    const P = window.__play, c = P.ctrl, rec = await fetch('data/enemies/ai.json').then(r => r.json()).then(a => a.records[44]);
    const home = { ...c.pos };
    c.pos = { x: rec.pos[0] - 30, y: rec.pos[1], z: rec.pos[2] }; c.prev = { ...c.pos }; c.locate(); if (c.onTeleport) c.onTeleport();
    let dmin = Infinity, pushed = 0, xmax = -Infinity;
    const tick = P.enemies.tick;
    P.enemies.tick = x => { tick(x); const d = Math.hypot(c.pos.x - rec.pos[0], c.pos.z - rec.pos[2]); dmin = Math.min(dmin, d);
      xmax = Math.max(xmax, c.pos.x); if (c.push && (c.push[0] || c.push[2])) pushed++; };
    window.__pushT = { tick, home, get: () => ({ dmin, pushed, xmax }),
      side: () => { const b = P.follow.basis(), v = [rec.pos[0] - c.pos.x, rec.pos[2] - c.pos.z]; return v[0] * b.r.x + v[1] * b.r.z; } };
    return true;
  });
  await pg.keyboard.down('w');
  let held = null;
  for (let i = 0; i < 80; i++) {
    await pg.waitForTimeout(100);
    const { side, pushed } = await pg.evaluate(() => ({ side: window.__pushT.side(), pushed: window.__pushT.get().pushed }));
    if (pushed > 60) break;
    const want = side > 0.5 ? 'd' : side < -0.5 ? 'a' : null;
    if (want !== held) { if (held) await pg.keyboard.up(held); if (want) await pg.keyboard.down(want); held = want; }
  }
  if (held) await pg.keyboard.up(held);
  await pg.keyboard.up('w');
  // 1c. then idle beside #66, keys up: FUN_0035a440 adds the push to his step every frame, standing
  // included (worker 0158), so on open floor it eases him out of the overlap (radii 5 + 5) and then
  // fades with it; he must end clear of #66 and not keep sliding. (In k_push16 f909-f1019 a 0x80 wall
  // edge held him in place under the push; there is no wall here.)
  const dist = () => pg.evaluate(() => { const c = window.__play.ctrl, P = window.__play;
    const a = P.enemyAI.actors.find(s => s.rec === 44), p = a ? [a.pos.x ?? a.pos[0], a.pos.z ?? a.pos[2]] : null;
    return { x: c.pos.x, z: c.pos.z, d: p ? Math.hypot(c.pos.x - p[0], c.pos.z - p[1]) : NaN,
      q: c.push ? Math.hypot(c.push[0], c.push[2]) : 0 }; });
  const idleA = await dist();
  await pg.waitForTimeout(1000);
  const idle0 = await dist();
  await pg.waitForTimeout(1000);
  const idle1 = await dist();
  const idleMove = Math.hypot(idle1.x - idle0.x, idle1.z - idle0.z);
  // The push shrinks with the overlap, so the last of it closes slowly: in the last 1 s he may only
  // move by what is left of the overlap (no slide past contact).
  ok(idleA.d < 9.5 && idle1.d >= idle0.d && idle0.d > idleA.d && idle1.d <= 10.01 && idle1.q < 0.01
     && idleMove <= (10 - idle0.d) + 0.01,
     `idle beside #66: ${idleA.d.toFixed(2)} -> ${idle0.d.toFixed(2)} -> ${idle1.d.toFixed(2)} u apart (eases toward 10, never past), push now ${idle1.q.toFixed(3)} u, moved ${idleMove.toFixed(3)} u in the last 1 s (<= the ${(10 - idle0.d).toFixed(3)} u of overlap left)`);
  const pr = await pg.evaluate(() => { const T = window.__pushT, P = window.__play, c = P.ctrl, r = T.get(); P.enemies.tick = T.tick;
    c.pos = { ...T.home }; c.prev = { ...c.pos }; c.locate(); if (c.onTeleport) c.onTeleport(); return r; });
  ok(push && pr.pushed > 10 && pr.dmin > 4.5, `walking into #66: pushed on ${pr.pushed} frames, closest ${pr.dmin.toFixed(2)} u between the centres (radii 5 + 5; no pass-through), x reached ${pr.xmax.toFixed(1)}`);

  // 2. walk across trigger 56. Test aid: put him at the west end of the passage east of the start
  // (a plain position write: the walk keeps body.poly and FUN_00357d48 resolves it onto the walk
  // mesh), then hold W (the start camera looks east) until he is past x -1228.
  await pg.evaluate(() => { const c = window.__play.ctrl; c.pos = { x: -1356, y: c.pos.y, z: -1106 }; });
  await pg.keyboard.down('w');
  let x = -Infinity;
  for (let i = 0; i < 80 && x < -1215; i++) {
    await pg.waitForTimeout(250);
    x = await pg.evaluate(() => window.__play.ctrl.pos.x);
  }
  await pg.keyboard.up('w');
  ok(x >= -1215, `walked east past trigger 56 (x ${x.toFixed(1)})`);
  await pg.waitForFunction(() => window.__play.enemies.actors.length >= 9, null, { timeout: 15000 }).catch(() => {});
  const after = await recs();
  ok(JSON.stringify(after) === '[44,45,46,47,48,49,50,51,66]', `after trigger 56: records ${after} drawn (want 44-49,50,51,66)`);
  const st = await pg.evaluate(() => window.__play.enemies.actors.filter(a => a.rec === 50 || a.rec === 51).map(a => ({
    rec: a.rec, inRoot: a.body.parent === window.__play.root, pos: a.body.position.toArray().map(v => +v.toFixed(1)),
    ai: !!window.__play.enemyAI.actors.find(s => s.rec === a.rec) })));
  for (const s of st) ok(s.inRoot && s.ai, `record ${s.rec}: body in the scene at ${s.pos}, driven by its AI actor`);
  // record 50's placement from the page's own data (ai.json, gitignored), not copied here
  const want50 = await pg.evaluate(() => fetch('data/enemies/ai.json').then(r => r.json()).then(a => a.records[50].pos));
  ok(st.some(s => s.rec === 50 && Math.abs(s.pos[0] - want50[0]) < 0.2 && Math.abs(s.pos[2] - want50[2]) < 0.2), `record 50 at its placement (${want50.map(v => v.toFixed(1))})`);

  // 3 / 4. direct cmd 0x108 / 0x130 through enemy_ai's setActive (object id = record + 22)
  const r = await pg.evaluate(async () => {
    const P = window.__play, ai = P.enemyAI, obj = k => ai.objects.get(k + 22);
    for (const k of [41, 42, 43]) ai.setActive(obj(k), 1);
    // draw order: 51 is the newest body (right after the list head); switching it off
    // (FUN_005a7d88 unlink) leaves 50 there, so the next body goes in front of 50 (FUN_005a7978)
    const ch = () => P.root.children, body = k => P.enemies.actors.find(a => a.rec === k)?.body;
    const b50 = body(50), b51 = body(51);
    const before = b50 && b51 ? ch().indexOf(b51) + 1 === ch().indexOf(b50) && ch().indexOf(b50) < ch().indexOf(P.vin.body) : false;
    ai.setActive(obj(51), 0);
    ai.setActive(obj(71), 1);
    const t0 = performance.now();
    while (!P.enemies.actors.some(a => a.rec === 71) && performance.now() - t0 < 15000) await new Promise(r => setTimeout(r, 100));
    const e030 = P.enemies.actors.find(a => a.rec === 71);
    const i71 = e030 ? ch().indexOf(e030.body) : -1;
    const order = { before, after: i71 >= 0 && i71 + 1 === ch().indexOf(b50) && !b51.parent };
    const out = { order, dead: P.enemies.actors.filter(a => a.rec >= 41 && a.rec <= 43).length,
                  e030: e030 ? { model: e030.model, clip: e030.clip, ai: !!ai.actors.find(s => s.rec === 71) } : null };
    ai.setActive(obj(71), 0);
    out.removed = !P.enemies.actors.some(a => a.rec === 71) && !(e030 && e030.body.parent);
    return out;
  });
  ok(r.order.before, 'draw order: 51 (newest) right in front of 50, both before Vincent');
  ok(r.order.after, 'draw order: 51 switched off, then 71 is linked right in front of 50 (FUN_005a7d88 / FUN_005a7978)');
  ok(r.dead === 0, 'dead records 41-43: activation refused, no body');
  ok(r.e030 && r.e030.model === 'e030' && r.e030.clip === 'st_a' && r.e030.ai, `record 71 (e030): body in its idle clip, AI actor (${JSON.stringify(r.e030)})`);
  ok(r.removed, 'record 71: cmd 0x130 removes the body');
  // 5. Vincent's bullets
  const sh = await pg.evaluate(async () => {
    const P = window.__play, ai = P.enemyAI, wait = n => new Promise(r => setTimeout(r, n));
    const a = ai.actors.find(s => s.rec === 44), body = P.enemies.actors.find(x => x.rec === 44);
    // part-test pose vs the drawn skeleton (same frame: both from the last enemies.tick)
    const rows = P.enemies.partPose(44);
    let poseErr = 0;
    for (const b of [2, 6, 13, 28, 33]) {
      let j = null; body.body.traverse(n => { if (!j && n.name === 'j' + String(b).padStart(2, '0')) j = n; });
      j.updateWorldMatrix(true, false);
      const e = j.matrixWorld.elements, r = rows(b)[3];
      poseErr = Math.max(poseErr, Math.hypot(e[12] - r[0], e[13] - r[1], e[14] - r[2]));
    }
    // stand where k_enemy_engage11a's Vincent shot from (31 u west of #66, same deck), so the camera
    // ray's first world hit is #66's side, not a railing between (test aid: position write, walk
    // polygon re-found by locate(), the camera reset as on the H / ladder warps)
    const c = P.ctrl;
    c.pos = { x: -1070.0, y: -146.1, z: -1105.6 }; c.prev = { ...c.pos }; c.locate(); if (c.onTeleport) c.onTeleport();
    await wait(400);
    P.aim.press('r1'); await wait(800);                                     // draw
    const tgt = [a.pos[0], a.pos[1] - 12, a.pos[2]];
    const dirTo = (from, to) => { const v = [0, 1, 2].map(k => to[k] - from[k]); const l = Math.hypot(...v); return v.map(x => x / l); };
    const angles = v => [Math.atan2(v[0], v[2]), Math.asin(v[1])];
    const err = () => { const c = P.aim.core, e = [c.eye.x, c.eye.y, c.eye.z], h = [c.hit.x, c.hit.y, c.hit.z];
                        const w = angles(dirTo(e, tgt)), g = angles(dirTo(e, h)); return [w[0] - g[0], w[1] - g[1]]; };
    // the yaw / pitch signs from one probe step each, then a few corrections
    const e0 = err(); P.follow.yaw += 0.02; await wait(120); const sy = Math.sign(e0[0] - err()[0]) || 1;
    const e1 = err(); P.follow.pitch += 0.02; await wait(120); const sp = Math.sign(e1[1] - err()[1]) || 1;
    for (let i = 0; i < 25; i++) {
      const [dy, dp] = err();
      if (Math.abs(dy) < 0.003 && Math.abs(dp) < 0.003) break;
      P.follow.yaw += sy * dy * 0.02 / 0.02; P.follow.pitch += sp * dp; await wait(120);
    }
    const aimErr = Math.max(...err().map(Math.abs));
    // lock-on (lock_on.js, FUN_00328968): the ray over #66 locks him; CAM+0xa0 = his bone 3 (C+0x765)
    // Sampled inside one game frame: the aim point (aim.js pre, start of the frame) against bone 3 of the
    // pose it was made from, i.e. just before this frame's enemies.tick advances #66's clip. Sampled
    // between rendered frames instead, the hit is one clip frame behind the pose (error 3e-4..1.4e-3).
    // A stalled enemies.tick resolves after 5 s with no lock (id null), so the asserts fail instead of hanging.
    const b3 = () => new Promise(res => {
      const E = P.enemies, t0 = E.tick;
      const to = setTimeout(() => { if (E.tick !== t0) E.tick = t0; res({ id: null, err: Infinity, stalled: true }); }, 5000);
      E.tick = function (...x) {
        E.tick = t0; clearTimeout(to);
        const r = E.partPose(44)(3)[3], h = P.aim.core.hit;
        res({ id: P.aim.lock.id, err: Math.hypot(h.x - r[0], h.y - r[1], h.z - r[2]) });
        return t0.apply(this, x);
      };
    });
    const { id: lockId, err: lockErr } = await b3();
    // turn off him in 0.01 rad steps: the reticle ray's distance from his body axis at his depth
    // ('side') grows; the keep test (the cone, 19.3 deg, is measured from S, 10 past Vincent, ~20 u short
    // of him at this range, then the screen box) holds the lock for some steps, then drops it.
    const side = () => { const c = P.aim.core, e = [c.eye.x, c.eye.y, c.eye.z], m = P.follow.M[2], l = Math.hypot(m[0], m[2]);
                         const f = [m[0] / l, m[2] / l], v = [a.pos[0] - e[0], a.pos[2] - e[2]], t = v[0] * f[0] + v[1] * f[1];
                         return Math.hypot(v[0] - f[0] * t, v[1] - f[1] * t); };
    // The held check is measured during the sweep, at its last held step: once dropped, the lock is only
    // re-taken by a body hit (lock_on.js), so stepping back from the far end does not re-lock him; back
    // on his chest (y0) the ray over his body re-locks him.
    const y0 = P.follow.yaw, p0 = P.follow.pitch;
    const sweep = async (n, dir, out) => {
      for (let i = 1; i <= n; i++) { P.follow.yaw = y0 + dir * 0.01 * i; P.follow.pitch = p0; await wait(100); const s = await b3(); out.push({ i, id: s.id, side: side(), err: s.err }); }
    };
    // the keep test's drop, yaw increasing (no shot)
    const up = [];
    await sweep(25, +1, up);
    const upHeld = up.findIndex(x => x.id !== 66), upAfter = upHeld < 0 ? [] : up.slice(upHeld);
    const upDrop = { held: upHeld, side: upHeld > 0 ? up[upHeld - 1].side : 0, ids: [...new Set(upAfter.map(x => x.id))] };
    // the shot, yaw decreasing: on the increasing side the muzzle's offset from the eye (over the
    // shoulder) puts the unlocked line of fire through his part boxes
    P.follow.yaw = y0; P.follow.pitch = p0; await wait(300);
    const steps = [];
    await sweep(25, -1, steps);
    const pick = steps.filter(x => x.id === 66).pop();
    const heldId = pick ? pick.id : steps[0].id, heldErr = pick ? pick.err : Infinity, offErr = pick ? pick.side : 0;
    const fireI = pick ? Math.max(1, pick.i - 1) : 1, hp0 = a.hp;
    // Counterfactual: the SAME shot (same yaw / pitch, one step short of the last held one) with the
    // lock off (core.lockOn = false: the auto-lock option off, aim.js aimRay keeps the world ray). He
    // must survive; then with the lock (back on his chest, the same sweep to fireI) he must die.
    P.aim.core.lockOn = false;
    P.follow.yaw = y0 - 0.01 * fireI; P.follow.pitch = p0; await wait(300);
    // count the shots this press really fires (aim.js onShot), so "survives" can't pass on no shot
    const onShot0 = P.aim.onShot; let shotsNoLock = 0;
    P.aim.onShot = (...x) => { shotsNoLock++; return onShot0 && onShot0(...x); };
    P.aim.press('r1'); await wait(1500);
    P.aim.onShot = onShot0;
    const hpNoLock = a.hp, stateNoLock = a.astate;
    P.aim.core.lockOn = true;
    P.follow.yaw = y0; P.follow.pitch = p0; await wait(300);
    const again = [];
    await sweep(fireI, -1, again);
    const fire = again[again.length - 1] || { id: null, side: 0 };
    P.aim.press('r1'); await wait(1500);                                    // fire, the reticle off him

    return { poseErr, aimErr, lockId, lockErr, heldId, heldErr, offErr, dropAt: (steps.find(x => x.id !== 66) || {}).i, fireId: fire.id, fireSide: fire.side, fireI, upDrop, shotsNoLock, hpNoLock, stateNoLock, hp0, hp: a.hp, astate: a.astate, mode: a.ai.mode, clip: a.C.clip, drawn: body.clip, hold: a.C.hold, drawnLoop: body.head ? body.head.loop : null, drawnFrame: body.frame, drawnN: body.side.clips[body.clip]?.frames };
  });
  ok(sh.poseErr < 1e-3, `part-test pose = the drawn joints (max ${sh.poseErr.toExponential(1)})`);
  ok(sh.aimErr < 0.01, `reticle ray on #66's chest (angle error ${sh.aimErr.toFixed(4)} rad)`);
  ok(sh.lockId === 66 && sh.lockErr < 1e-3, `lock-on: #${sh.lockId} locked, aim point on his bone 3 (${sh.lockErr.toExponential(1)})`);
  ok(sh.heldId === 66 && sh.heldErr < 1e-3 && sh.offErr > 2.5, `lock held with the ray ${sh.offErr.toFixed(2)} u beside his axis: aim still on bone 3 (${sh.heldErr.toFixed(4)}); ${sh.dropAt ? 'dropped after ' + sh.dropAt + ' x 0.01 rad' : 'held over the 25-step sweep'}`);
  ok(sh.upDrop.held > 1 && sh.upDrop.ids.length === 1 && sh.upDrop.ids[0] === -1,
     `keep test, yaw increasing (no shot): held for ${sh.upDrop.held} x 0.01 rad (ray ${sh.upDrop.side.toFixed(2)} u beside him), then dropped to ${JSON.stringify(sh.upDrop.ids)}`);
  ok(sh.shotsNoLock > 0 && sh.hp0 === 1 && sh.hpNoLock === 1 && sh.stateNoLock !== 7, `the same shot (step ${sh.fireI}, ${sh.shotsNoLock} fired) with the lock off (auto-lock option off): #66 survives (HP ${sh.hpNoLock}, actor state ${sh.stateNoLock})`);
  ok(sh.fireId === 66 && sh.hp0 === 1 && sh.hp === 0 && sh.astate === 7 && sh.mode === 0, `R1 with the reticle ${sh.fireSide.toFixed(2)} u off #66 (auto-aim, lock #${sh.fireId}): HP ${sh.hp0} -> ${sh.hp}, actor state ${sh.astate}, AI mode ${sh.mode}`);
  ok(sh.drawn === sh.clip && /^r_st(laf|bf)0$/.test(sh.clip) && sh.hold && sh.drawnLoop === false && sh.drawnFrame === sh.drawnN - 1,
     `#66 draws his death clip ${sh.drawn} (held: head layer loop ${sh.drawnLoop}, frame ${sh.drawnFrame} of ${sh.drawnN})`);
  // 6. an e048 (record 24). After step 5: activating it earlier shifts #66's clip phase (frames
  // spent waiting for its model), which moves bone 3 off the lock check's limit.
  const e48 = await pg.evaluate(async () => {
    const P = window.__play, ai = P.enemyAI, obj = k => ai.objects.get(k + 22);
    ai.setActive(obj(24), 1);
    const t1 = performance.now();
    while (!P.enemies.actors.some(a => a.rec === 24) && performance.now() - t1 < 15000) await new Promise(r => setTimeout(r, 100));
    await new Promise(r => setTimeout(r, 300));       // a few game frames: posed
    const b24 = P.enemies.actors.find(a => a.rec === 24), s24 = ai.actors.find(a => a.rec === 24);
    const out = b24 ? { model: b24.model, drawn: b24.clip, clip: s24 && s24.C.clip, ai: !!s24, posed: !!P.enemies.partPose(24) } : null;
    ai.setActive(obj(24), 0);
    return out;
  });
  ok(e48 && e48.model === 'e048' && e48.ai && e48.drawn === 'm_gst' && e48.clip === 'm_gst' && e48.posed,
     `record 24 (e048): body drawn in its stance clip, AI actor, posed for the part test (${JSON.stringify(e48)})`);
  // 7. Dan's report 20261031T000852Z-9974: the alley guard, record 51 (e027, stance 0x1e). Activated and
  // sent message 2 (trigger 90's wake), script 142 puts him in brain mode 1 (FUN_00362998); he fires
  // (m_st_f). The body plays the game's motion-player blend (enemies.js; k_fire_emp15): each request is
  // a new head layer, the old head blends out over 11 clip ticks (rem -= STEP), so after the fire
  // request the next request (m_st_a, class 6) leaves m_st_f as the blend-out layer, fading out.
  const g51 = await pg.evaluate(async () => {
    const P = window.__play, ai = P.enemyAI, obj = k => ai.objects.get(k + 22);
    // stand where Dan's report has Vincent (test aid: position write, as in step 5)
    const c = P.ctrl;
    c.pos = { x: -786.83, y: -159.97, z: -630.22 }; c.prev = { ...c.pos }; c.locate(); if (c.onTeleport) c.onTeleport();
    ai.setActive(obj(51), 1);
    const t1 = performance.now();
    while (!P.enemies.actors.some(a => a.rec === 51) && performance.now() - t1 < 15000) await new Promise(r => setTimeout(r, 100));
    const body = P.enemies.actors.find(a => a.rec === 51);
    if (!body) return null;
    const log = [], tick = P.enemies.tick;
    P.enemies.tick = x => { tick(x); log.push({ head: body.head && body.head.clip, out: body.out ? body.out.clip : null, rem: body.blend ? body.blend.rem : null, len: body.blend ? body.blend.len : null, w: body.weight, outF: body.out ? body.out.frame : null }); };
    // Test aid: trigger 76 disarmed for this step. #51 stands inside it, so his own bullets run its event
    // 0x12 (script 99: wake 46-50); their shots then draw from the shared RNG and bring his next shot
    // forward, cutting the fade that is compared with k_fire_emp15 (where no other soldier fires).
    const t76 = ai.objects.get('t76'), t76on = t76.active;
    ai.setActive(t76, 0);
    ai.runEvent(obj(51), 2, [2, 0, 't90', 0]);
    const s51 = ai.actors.find(a => a.rec === 51);
    const t2 = performance.now();
    while (!log.some(l => l.out === 'm_st_f') && performance.now() - t2 < 20000) await new Promise(r => setTimeout(r, 100));
    // until m_st_f stops being the blend-out layer (a fixed 600 ms cut the fade short on a slow frame rate)
    const t3 = performance.now(), i0 = log.findIndex(l => l.out === 'm_st_f');
    while (i0 >= 0 && log.slice(i0).every(l => l.out === 'm_st_f') && performance.now() - t3 < 10000) await new Promise(r => setTimeout(r, 100));
    P.enemies.tick = tick;
    ai.setActive(t76, t76on);
    const mode = s51 ? s51.ai.mode : null;
    ai.setActive(obj(51), 0);
    return { mode, log };
  });
  {
    const L = g51 ? g51.log : [];
    const i = L.findIndex(l => l.out === 'm_st_f');
    const fade = [];                                                          // the frames m_st_f is the blend-out layer
    for (let j = i; i >= 0 && j < L.length && L[j].out === 'm_st_f'; j++) fade.push(L[j]);
    const fired = L.some(l => l.head === 'm_st_f');
    // The fade ends when the blend runs out, or earlier when his next shot requests m_st_f (a new head
    // layer replaces it, in the game too). When that shot comes depends on the page's shared RNG (one
    // GameRandom for every soldier), whose draws before this step depend on how many frames the earlier
    // steps' waits lasted, so an early cut is accepted, and the frames before it are compared below.
    const nxt = i >= 0 ? L[i + fade.length] : null, cut = !!nxt && nxt.head === 'm_st_f';
    ok(g51 && g51.mode === 1 && fired && i >= 0 && L[i].head === 'm_st_a' && fade.length >= 5 && fade[fade.length - 1].outF > fade[0].outF,
       `record 51 (mode ${g51 && g51.mode}): fire clip m_st_f drawn, then blends out under m_st_a over ${fade.length} frames, its frame advancing ${fade[0]?.outF?.toFixed(1)} -> ${fade[fade.length - 1]?.outF?.toFixed(1)}`);
    // The game's fade (k_fire_emp15 #66: r_st_f is the blend-out layer f788-797): the new head's countdown
    // +0x24 and length +0x1d each frame. The page's countdown after each frame must match, and the weight
    // it drew that frame (MP+0x408) = (len - the countdown before the frame) / len.
    const tf = path.join(here, '../../tools/trace/out/k_fire_emp15.jsonl');
    if (!existsSync(tf)) ok(false, 'k_fire_emp15.jsonl missing (tools/trace/out; TRACE_EMP=1 run): blend weights unchecked');
    else {
      const R = readFileSync(tf, 'utf8').trim().split('\n').slice(1).map(l => JSON.parse(l));
      const game = [];
      for (const r of R) {
        const h = r.e0_mp_head, hd = parseInt(h.slice(16, 18), 16), bo = parseInt(h.slice(20, 22), 16);
        if (bo !== 0xff && r.e0_mp_layers[bo]?.clip === 'r_st_f') game.push({ rem: r.e0_mp_layers[hd].blend_len, len: r.e0_mp_layers[hd].blend_frames });
      }
      const len = game[0]?.len;
      const want = game.map((g, j) => ({ rem: g.rem, w: (len - (j ? game[j - 1].rem : len)) / len }));
      const bad = !(want.length === fade.length || (cut && fade.length < want.length)) ? -2
        : fade.findIndex((l, j) => Math.abs(l.rem - want[j].rem) > 1e-4 || Math.abs(l.w - want[j].w) > 1e-6 || l.len !== len);
      ok(game.length >= 8 && fade.length >= 5 && bad === -1,
         `record 51 blend vs k_fire_emp15 (len ${len}): countdown game ${want.map(g => g.rem.toFixed(1)).join(' ')} page ${fade.map(l => l.rem?.toFixed(1)).join(' ')}; ` +
         `weight game ${want.map(g => g.w.toFixed(3)).join(' ')} page ${fade.map(l => l.w?.toFixed(3)).join(' ')}${cut ? ' (cut by his next shot)' : ''}${bad >= 0 ? ` (first diff at ${bad})` : bad === -2 ? ' (length differs, not cut by a shot)' : ''}`);
    }
  }
  // 8. A hit soldier keeps acting: record 69 (e027, stance 0x1e, script 24) activated; his damage event 8
  // (what enemyHP runs on a hit) sets AI mode 5 (cmd 0x143 r0 5). Mode 5 (FUN_003639d0) turns the combat
  // layer on: he goes on firing (m_st_f drawn) from where he stands (the move layers are not translated).
  // Then Vincent stands 12 u in front of him: d34 < 22 -> state 10 -> the attack pick -> actor state 0x80,
  // the body draws m_atk (the attack table's play-clip command); Vincent's HP is unchanged (the hit
  // volume, FUN_00541310, is not decoded).
  const g69 = await pg.evaluate(async () => {
    const P = window.__play, ai = P.enemyAI, obj = k => ai.objects.get(k + 22);
    const rec = await fetch('data/enemies/ai.json').then(r => r.json()).then(a => a.records[69]);
    const c = P.ctrl, wait = async (f, ms) => { const t = performance.now(); while (!f() && performance.now() - t < ms) await new Promise(r => setTimeout(r, 50)); return f(); };
    // Vincent 80 u in front of him, on his level (test aid: position write, as in step 5)
    const put = d => { c.pos = { x: rec.pos[0] + rec.face[0] * d, y: rec.pos[1], z: rec.pos[2] + rec.face[2] * d }; c.prev = { ...c.pos }; c.locate(); if (c.onTeleport) c.onTeleport(); };
    put(80);
    ai.setActive(obj(69), 1);
    if (!await wait(() => P.enemies.actors.some(a => a.rec === 69), 15000)) return null;
    const a = ai.actors.find(x => x.rec === 69), body = P.enemies.actors.find(x => x.rec === 69);
    ai.runEvent(a, 8, [10, 0, 0, 0]);
    const mode = a.ai.mode;
    const seen = new Set(), clips = new Set(), log = [], tick = P.enemies.tick;
    P.enemies.tick = x => { tick(x); seen.add(a.ai.bstate); if (body.head) clips.add(body.head.clip); };
    const fired = await wait(() => clips.has('m_st_f') && seen.size >= 2, 20000);
    const after = { fired, states: [...seen], cmode: a.ai.cmode, astate: a.astate };
    clips.clear();
    const hp0 = P.php.hp;
    put(12);
    const melee = await wait(() => clips.has('m_atk'), 20000);
    const atk = { melee, astate: a.astate, hp0, hp: P.php.hp };
    P.enemies.tick = tick;
    ai.setActive(obj(69), 0);
    return { mode, after, atk };
  });
  ok(g69 && g69.mode === 5 && g69.after.fired && g69.after.cmode === 2 && g69.after.astate === 3,
     `record 69 hit (event 8): AI mode ${g69?.mode} (5), keeps firing: m_st_f drawn ${g69?.after.fired}, brain states ${JSON.stringify(g69?.after.states)}, combat layer ${g69?.after.cmode}, actor state ${g69?.after.astate}`);
  ok(g69 && g69.atk.melee && g69.atk.hp === g69.atk.hp0,
     `record 69 with Vincent 12 u in front: melee attempt, body draws m_atk (${g69?.atk.melee}), Vincent HP ${g69?.atk.hp0} -> ${g69?.atk.hp} (no hit: hit volume not decoded)`);
  // 9. The platform soldiers (records 46-49, Dan's report "they can't react"). Trigger 77 (script 100) is a
  // flag-0x20 volume over their raised floor: FUN_005503e8 makes it see-through to bullets, and a bullet
  // crossing it runs event 0x12 (FUN_005442d8 -> vt+0x1b4 FUN_005511e8): wake n1 -> 46-49. Walking into it
  // fires nothing (its enter / leave bits 0 / 1 are clear). Vincent stands on the ladder-top deck as in
  // step 5 (test aid: position write), the gun drawn, the camera turned onto the volume's middle (the
  // same correction loop as step 5), then R1: aim.js onShot -> playerShot -> the bullet update.
  // Script 126 (48 / 49) sets AI mode 1; scripts 127 / 128 (46 / 47) set mode 0 and run a sub-routine
  // the page's VM stops at (op 37: notes/fidelity-audit.md).
  // A fresh page: earlier in this run #51's bullets (he stands inside trigger 76, script 99: wake n1 ->
  // 46-49) and then 46-49's own bullets (inside 77) have already woken them.
  await pg.goto(`http://127.0.0.1:${port}/play.html`, { waitUntil: 'load' });
  await pg.waitForFunction(() => window.__play && window.__play.stats.ticks > 30, null, { timeout: 120000 });
  const pf = await pg.evaluate(async () => {
    const P = window.__play, ai = P.enemyAI, c = P.ctrl, wait = n => new Promise(r => setTimeout(r, n));
    const recs = [46, 47, 48, 49], act = k => ai.actors.find(a => a.rec === k);
    const before = recs.map(k => act(k)?.ai?.mode);
    const seen = [], t0 = ai.tick;
    ai.tick = (...x) => { const ev = t0(...x); for (const e of ev) if (/^shot volume t77|^event t77 |^event (68|69|70|71) /.test(e)) seen.push(e); return ev; };
    c.pos = { x: -1070.0, y: -146.1, z: -1105.6 }; c.prev = { ...c.pos }; c.locate(); if (c.onTeleport) c.onTeleport();
    await wait(400);
    if (!P.aim.state().drawn) { P.aim.press('r1'); await wait(800); }
    const m = ai.triggers[77].t.m, tgt = [m[3][0], m[3][1] - 14, m[3][2]];
    const dirTo = (from, to) => { const v = [0, 1, 2].map(k => to[k] - from[k]); const l = Math.hypot(...v); return v.map(x => x / l); };
    const angles = v => [Math.atan2(v[0], v[2]), Math.asin(v[1])];
    const err = () => { const k = P.aim.core, e = [k.eye.x, k.eye.y, k.eye.z], h = [k.hit.x, k.hit.y, k.hit.z];
                        const w = angles(dirTo(e, tgt)), g = angles(dirTo(e, h)); return [w[0] - g[0], w[1] - g[1]]; };
    const e0 = err(); P.follow.yaw += 0.02; await wait(120); const sy = Math.sign(e0[0] - err()[0]) || 1;
    const e1 = err(); P.follow.pitch += 0.02; await wait(120); const sp = Math.sign(e1[1] - err()[1]) || 1;
    for (let i = 0; i < 40; i++) {
      const [dy, dp] = err();
      if (Math.abs(dy) < 0.01 && Math.abs(dp) < 0.01) break;
      P.follow.yaw += sy * dy; P.follow.pitch += sp * dp; await wait(120);
    }
    const quiet = seen.length;                                             // nothing fired before R1
    P.aim.press('r1'); await wait(2500);
    ai.tick = t0;
    return { before, after: recs.map(k => act(k)?.ai?.mode), seen, quiet, var0: ai.triggers[77].vars[0] };
  });
  ok(pf && pf.quiet === 0 && pf.seen.includes('shot volume t77') && pf.seen.some(e => e.startsWith('event t77 script 100 ev 18')) && pf.var0 === 1,
     `a shot over the platform crosses trigger 77: event 0x12 ran (script 100; once-var ${pf?.var0}); ${pf?.seen.slice(0, 3).join(' | ')}`);
  ok(pf && [68, 69, 70, 71].every(id => pf.seen.some(e => e.startsWith(`event ${id} script `) && / ev 2 r 1,/.test(e))) &&
     pf.before.every(m => m === 0) && pf.after[2] === 1 && pf.after[3] === 1 && pf.after[0] === 0 && pf.after[1] === 0,
     `records 46-49 woken (event 2, n 1): AI modes ${pf?.before} -> ${pf?.after} (scripts 127 / 128: 0, 126: 1)`);
  ok(errs.length === 0, `no console errors${errs.length ? ': ' + errs.slice(0, 3).join(' | ') : ''}`);
} finally {
  await browser.close();
  srv.kill();
}
done();
