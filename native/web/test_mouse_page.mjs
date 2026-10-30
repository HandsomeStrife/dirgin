// The page's mouse look, in a headless browser on the real play.html (drag path: headless has
// no pointer lock, and pointermove feeds the same MouseFeed as the locked mousemove):
//   1. default (the game's USB-mouse path): a steady drag turns at the full-stick rate,
//      0.05 rad per game frame (FUN_00254ac8 cap, FUN_003256d8 / step, rate 0.05 x step), even
//      though swiftshader renders well under 50 fps (MouseFeed shares the counts per game frame);
//   2. ?mouselook=direct (OPTION, on by default; the game path is ?mouselook=game): yaw turns by exactly pixels x 0.15 deg;
//      ?mousesens=0.3 doubles it;
//   3. no console errors or page errors.
//   node native/web/test_mouse_page.mjs            (needs playwright; else SKIP, exit 0)
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { spawn, execSync } from 'node:child_process';
import net from 'node:net';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
let fail = 0;
const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) fail++; };
const done = () => { console.log(fail ? `${fail} FAILED` : 'all ok'); process.exit(fail ? 1 : 0); };
let chromium;
try {
  const g = execSync('npm root -g', { encoding: 'utf8' }).trim();
  ({ chromium } = createRequire(path.join(g, '/'))('playwright'));
} catch { console.log('SKIP: playwright not installed (npm i -g playwright)'); done(); }

const port = await new Promise(r => { const s = net.createServer().listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => r(p)); }); });
const srv = spawn('python3', ['-I', '-m', 'http.server', String(port), '--bind', '127.0.0.1', '--directory', here], { stdio: 'ignore' });
const browser = await chromium.launch({ args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
const wrap = a => { while (a < -Math.PI) a += 2 * Math.PI; while (a > Math.PI) a -= 2 * Math.PI; return a; };
async function open(q) {
  const pg = await browser.newPage({ viewport: { width: 960, height: 540 } });
  const errs = [];
  pg.on('console', m => { if (m.type() === 'error') errs.push(m.text()); });
  pg.on('pageerror', e => errs.push(e.message));
  for (let i = 0; ; i++) {
    try { await pg.goto(`http://127.0.0.1:${port}/play.html${q}`, { waitUntil: 'load' }); break; }
    catch (e) { if (i > 20) throw e; await new Promise(r => setTimeout(r, 250)); }
  }
  await pg.waitForFunction(() => window.__play && window.__play.stats.ticks > 30, null, { timeout: 120000 });
  return { pg, errs };
}
const state = pg => pg.evaluate(() => ({ yaw: window.__play.follow.yaw, ticks: window.__play.stats.ticks }));
// drag right by `px` per step for `steps` steps, `ms` apart; returns yaw turned and game frames run
async function drag(pg, px, steps, ms, measureFrom = 0) {
  await pg.mouse.move(480, 270); await pg.mouse.down();
  let x = 480, s0 = null, turned = 0;
  for (let i = 0; i < steps; i++) {
    if (i === measureFrom) s0 = await state(pg);
    x += px; await pg.mouse.move(x, 270); await pg.waitForTimeout(ms);
  }
  await pg.waitForTimeout(200);                       // let the last counts reach a game frame
  const s1 = await state(pg);
  await pg.mouse.up();
  return { turned: wrap(s1.yaw - s0.yaw), frames: s1.ticks - s0.ticks };
}
try {
  { // 1. the game's path: one pointermove of 80 px per rendered frame (a moving mouse sends
    // events every frame), dispatched in the page; past the cap of 12 px per game frame
    const { pg, errs } = await open('?mouselook=game');
    const r = await pg.evaluate(() => new Promise(res => {
      const P = window.__play, F = P.follow, t = F.tick.bind(F), log = [];
      F.tick = p => { const y = F.yaw; t(p); let d = F.yaw - y; if (d < -Math.PI) d += 2 * Math.PI; if (d > Math.PI) d -= 2 * Math.PI; log.push(d); };
      const c = P.renderer.domElement; let x = 100, n = 0; const t0 = performance.now();
      c.dispatchEvent(new PointerEvent('pointerdown', { clientX: x, clientY: 270, bubbles: true }));
      const f = () => {
        if (n++ < 40) { x += 80; window.dispatchEvent(new PointerEvent('pointermove', { clientX: x, clientY: 270 })); requestAnimationFrame(f); return; }
        window.dispatchEvent(new PointerEvent('pointerup', {}));
        F.tick = t; res({ log, fps: 40 * 1000 / (performance.now() - t0) });
      };
      requestAnimationFrame(f);
    }));
    const s0 = r.log.findIndex(d => d > 0);           // the first game frame with counts
    const moving = r.log.slice(s0 + 14, -4);           // after the 14-frame ramp, before the last frames
    const slow = moving.filter(d => Math.abs(d - 0.05) > 1e-6).length;
    ok(moving.length > 20 && slow === 0, `game path at ${r.fps.toFixed(0)} fps: ${moving.length} game frames after the ramp, ${slow} below the full-stick 0.05 rad (max ${Math.max(...moving).toFixed(4)})`);
    ok(errs.length === 0, `no page errors (${errs.slice(0, 2).join(' | ')})`);
    await pg.close();
  }
  for (const [q, deg] of [['', 0.15], ['?mouselook=direct', 0.15], ['?mousesens=0.3', 0.3]]) {
    const { pg, errs } = await open(q);
    const r = await drag(pg, 10, 30, 20);
    const want = 300 * deg * Math.PI / 180;
    ok(Math.abs(r.turned - want) < 1e-3, `${q}: 300 px dragged -> yaw ${r.turned.toFixed(4)} rad (want ${want.toFixed(4)})`);
    ok(errs.length === 0, `no page errors (${errs.slice(0, 2).join(' | ')})`);
    await pg.close();
  }
} finally { await browser.close(); srv.kill(); }
done();
