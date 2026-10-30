// The page's enemy activation path (play.js -> enemy_ai.js onActivate / onDeactivate -> enemies.js
// add / remove), in a headless browser on the real page:
//   1. at the save only records 44 / 45 are drawn;
//   2. walking east across trigger 56 (x -1228 on the start deck) makes enemies.js draw two new
//      bodies, records 50 / 51 (script 91 event 0xd: cmd 0x108 on both; notes/enemy-activation.md §2);
//   3. the save's dead records (41-43) refuse activation (FUN_00559748), so no body appears;
//   4. a non-e027 record (71, e030) activated by cmd 0x108 gets a body with no AI, and cmd 0x130
//      removes it again; switching off the newest body (51) first, 71 is drawn right in front of 50;
//   5. Vincent's bullets (aim.js onShot -> enemy_ai playerShot): the part test's pose (enemies.js
//      partPose) equals the drawn joints; the gun drawn with R1, the camera turned until the reticle ray
//      crosses #66's chest, R1 fires: #66 (HP 1) dies (actor state 7, AI mode 0, death clip held);
//   6. no console errors or page errors.
//   node native/web/test_enemies_page.mjs          (needs data/enemies + playwright; else SKIP, exit 0)
// Serves native/web with python3 http.server on a free port. three.js comes from the CDN (play.html importmap).
import { existsSync } from 'node:fs';
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
  ok(r.e030 && r.e030.model === 'e030' && r.e030.clip === 'st_a' && !r.e030.ai, `record 71 (e030): body in its idle clip, no AI (${JSON.stringify(r.e030)})`);
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
    const hp0 = a.hp;
    P.aim.press('r1'); await wait(1500);                                    // fire

    return { poseErr, aimErr, hp0, hp: a.hp, astate: a.astate, mode: a.ai.mode, clip: a.C.clip, drawn: body.clip, hold: a.C.hold };
  });
  ok(sh.poseErr < 1e-3, `part-test pose = the drawn joints (max ${sh.poseErr.toExponential(1)})`);
  ok(sh.aimErr < 0.01, `reticle ray on #66's chest (angle error ${sh.aimErr.toFixed(4)} rad)`);
  ok(sh.hp0 === 1 && sh.hp === 0 && sh.astate === 7 && sh.mode === 0, `R1 on #66: HP ${sh.hp0} -> ${sh.hp}, actor state ${sh.astate}, AI mode ${sh.mode}`);
  ok(sh.drawn === sh.clip && /^r_st(laf|bf)0$/.test(sh.clip) && sh.hold, `#66 draws his death clip ${sh.drawn} (held)`);
  ok(errs.length === 0, `no console errors${errs.length ? ': ' + errs.slice(0, 3).join(' | ') : ''}`);
} finally {
  await browser.close();
  srv.kill();
}
done();
