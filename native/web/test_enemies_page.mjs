// The page's enemy activation path (play.js -> enemy_ai.js onActivate / onDeactivate -> enemies.js
// add / remove), in a headless browser on the real page:
//   1. at the save only records 44 / 45 are drawn;
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
//      attack (m_atk drawn); no console errors or page errors.
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
  ok(JSON.stringify(await recs()) === '[44,45]', `at the save: records ${await recs()} drawn (want 44,45)`);
  ok(await pg.evaluate(() => !!window.__play.enemyAI), 'enemy AI running');

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
  await pg.waitForFunction(() => window.__play.enemies.actors.length >= 4, null, { timeout: 15000 }).catch(() => {});
  const after = await recs();
  ok(JSON.stringify(after) === '[44,45,50,51]', `after trigger 56: records ${after} drawn (want 44,45,50,51)`);
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
    ai.runEvent(obj(51), 2, [2, 0, 't90', 0]);
    const s51 = ai.actors.find(a => a.rec === 51);
    const t2 = performance.now();
    while (!log.some(l => l.out === 'm_st_f') && performance.now() - t2 < 20000) await new Promise(r => setTimeout(r, 100));
    await new Promise(r => setTimeout(r, 600));
    P.enemies.tick = tick;
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
    ok(g51 && g51.mode === 1 && fired && i >= 0 && L[i].head === 'm_st_a' && fade.length >= 8 && fade[fade.length - 1].outF > fade[0].outF,
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
      const bad = want.length !== fade.length ? -2 : want.findIndex((g, j) => Math.abs(fade[j].rem - g.rem) > 1e-4 || Math.abs(fade[j].w - g.w) > 1e-6 || fade[j].len !== len);
      ok(game.length >= 8 && bad === -1,
         `record 51 blend vs k_fire_emp15 (len ${len}): countdown game ${want.map(g => g.rem.toFixed(1)).join(' ')} page ${fade.map(l => l.rem?.toFixed(1)).join(' ')}; ` +
         `weight game ${want.map(g => g.w.toFixed(3)).join(' ')} page ${fade.map(l => l.w?.toFixed(3)).join(' ')}${bad >= 0 ? ` (first diff at ${bad})` : bad === -2 ? ' (length differs)' : ''}`);
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
  ok(errs.length === 0, `no console errors${errs.length ? ': ' + errs.slice(0, 3).join(' | ') : ''}`);
} finally {
  await browser.close();
  srv.kill();
}
done();
