// The follow camera on the ladder, in a headless browser on the real play.html (worker 0142; Dan's
// report 20261030T124414Z-cc39: "climbing a ladder doesn't move the camera to a correct
// orientation, and zooms into the terrain"). The game (trace l_ladder_cam, test_cam_traces.mjs)
// keeps camera type 0xc on the ladder and pulls its base back along his facing: CAM+0x1c4 eases to
// CAM+0x1c8 = 4 (FUN_00328640, set by the ladder; FUN_0032ace0 l.254 / l.1748), and the turn-behind
// follows the facing in states 10-14 (FUN_0055a410). Checked here through the page's wiring
// (actions/ladder.js setBack / camState, play.js):
//   1. ?ladder=0, Space mounts: from the mount tick the pull-back follows the game's ease
//      (back += (4 - back) x 0.2 x 1.2 per frame, 0.96 on the first frame) and the camera reads
//      state 0xb, then 10 on the rungs;
//   2. W climbs: settled on a rung, the camera base is 4 behind him (pos - facing x 4);
//   3. a mouse drag (the default direct mouse look) turns the camera along and into the wall: it
//      turns by pixels x 0.15 deg, and the eye never crosses to the far side of the ladder face;
//   4. every way off the ladder clears CAM+0x1c8 at the game's moment and CAM+0x1c4 eases back to 0:
//      the bottom exit from its first frame (FUN_00576f98, state 0xe), the top exit when it ends
//      (still 4 through 0xd: FUN_00573a38 -> FUN_00577110, traced), the jump-off when it lands
//      (GUESS, actions/ladder.js exit()); each from a fresh page;
//   5. no console errors or page errors.
//   node native/web/test_ladder_cam_page.mjs        (needs playwright; else SKIP, exit 0)
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
const sleep = ms => new Promise(r => setTimeout(r, ms));
try {
  const errs = [];
  // play.html at ladder 0's foot, with a per game frame log: the camera's inputs and state after its tick
  const open = async () => {
    const pg = await browser.newPage({ viewport: { width: 960, height: 540 } });
    pg.on('console', m => { if (m.type() === 'error') errs.push(m.text()); });
    pg.on('pageerror', e => errs.push(e.message));
    for (let i = 0; ; i++) {
      try { await pg.goto(`http://127.0.0.1:${port}/play.html?ladder=0`, { waitUntil: 'load' }); break; }
      catch (e) { if (i > 20) throw e; await sleep(250); }
    }
    await pg.waitForFunction(() => window.__play && window.__play.stats.ticks > 30 && !window.__play.ctrl.active, null, { timeout: 180000 });
    await pg.evaluate(() => {
      const P = window.__play, F = P.follow, t = F.tick.bind(F);
      window.__log = [];
      F.tick = p => {
        t(p);
        window.__log.push({ state: p.state, back: F.back, target: F.backTarget, base: F.base.slice(), eye: F.eye.slice(), yaw: F.yaw,
                            pos: [p.pos.x, p.pos.y, p.pos.z], face: p.facing ? [p.facing.x, p.facing.y || 0, p.facing.z] : null,
                            phase: P.ctrl.active && P.ctrl.active.phase });
      };
    });
    return pg;
  };
  const pg = await open();
  // 1. mount
  await pg.evaluate(() => { window.__play.keys.add('Space'); window.__play.pressed.add('Space'); });
  await sleep(150);
  await pg.evaluate(() => window.__play.keys.delete('Space'));
  await pg.waitForFunction(() => window.__log.some(r => r.phase === 'idle'), null, { timeout: 60000 });
  let L = await pg.evaluate(() => window.__log);
  const m = L.findIndex(r => r.phase === 'mountBottom');   // first camera frame after the mount (FUN_00576c88)
  let bad = 0, b = 0;
  for (let i = m; i < L.length && i < m + 40; i++) {
    const want = (4 - b) * 0.2 * 1.2 + b;             // l.1748 (f32 in the game; 1e-5 here)
    if (Math.abs(L[i].back - want) > 1e-5) bad++;
    b = L[i].back;
  }
  ok(m > 0 && L[m].target === 4, `bottom mount sets CAM+0x1c8 = 4 on its first frame (${m > 0 ? L[m].target : '-'})`);
  ok(m > 0 && L[m - 1].back === 0 && Math.abs(L[m].back - 0.96) < 1e-6 && bad === 0,
     `mount: CAM+0x1c4 eases to 4 by the game's rule (first ${m > 0 ? L[m].back.toFixed(4) : '-'}, 40 frames, ${bad} off)`);
  if (m <= 0) { console.log('FAIL no mount seen'); fail++; await pg.close(); throw new Error('no mount'); }
  const st = [...new Set(L.slice(m).map(r => r.state))];
  ok(st[0] === 0xb && st.includes(10), `the camera reads the ladder states ${st.map(s => '0x' + s.toString(16)).join(' -> ')} (0xb, then 10)`);
  // 2. climb with W, then settle on a rung
  await pg.evaluate(() => { window.__play.keys.add('KeyW'); });
  await sleep(1200);
  await pg.evaluate(() => { window.__play.keys.delete('KeyW'); });
  await pg.waitForFunction(() => window.__play.ctrl.active && window.__play.ctrl.active.phase === 'idle', null, { timeout: 60000 });
  await sleep(1500);
  L = await pg.evaluate(() => window.__log);
  const r = L[L.length - 1];
  const want = [0, 1, 2].map(j => r.pos[j] - r.face[j] * 4);
  const off = Math.hypot(...want.map((v, j) => v - r.base[j]));
  ok(r.state === 10 && r.pos[1] < L[m].pos[1] - 4 && off < 1e-3, `settled on a rung (climbed ${(L[m].pos[1] - r.pos[1]).toFixed(1)}): base = pos - facing x 4 within ${off.toExponential(1)}`);
  // 3. turn the camera with the mouse along and into the wall (direct mouse look, the page default)
  const y0 = await pg.evaluate(() => window.__play.follow.yaw);
  const n0 = L.length;
  await pg.mouse.move(480, 270); await pg.mouse.down();
  let x = 480;
  for (let i = 0; i < 60; i++) { x += 20; await pg.mouse.move(x, 270); await sleep(25); }   // 1200 px = 180 deg
  await sleep(300);
  await pg.mouse.up();
  const y1 = await pg.evaluate(() => window.__play.follow.yaw);
  let dy = y1 - y0; while (dy < -Math.PI) dy += 2 * Math.PI; while (dy > Math.PI) dy -= 2 * Math.PI;
  ok(Math.abs(Math.abs(dy) - Math.PI) < 1e-3, `mouse on the ladder: 1200 px -> yaw ${Math.abs(dy).toFixed(4)} rad (want ${Math.PI.toFixed(4)})`);
  L = await pg.evaluate(() => window.__log);
  // ladder 0's face (z128_ladders.json): x = -1106, the climber on the -x side (facing +x)
  const faceX = -1106.0;
  const turn = L.slice(n0);
  const worst = Math.max(...turn.map(q => q.eye[0] - faceX));
  ok(turn.length > 30 && turn.every(q => q.state === 10) && worst < 0,
     `turning on the ladder (${turn.length} frames): the eye stays on his side of the ladder face (max x - face ${worst.toFixed(3)})`);
  await pg.close();

  // 4. the ways off the ladder, each from a fresh page: mount, then drive it off
  const wayOff = async (name, drive) => {
    const q = await open();
    await q.evaluate(() => { window.__play.keys.add('Space'); window.__play.pressed.add('Space'); });
    await sleep(150);
    await q.evaluate(() => window.__play.keys.delete('Space'));
    await q.waitForFunction(() => window.__log.some(r => r.phase === 'idle'), null, { timeout: 60000 });
    await drive(q);
    await q.waitForFunction(() => !window.__play.ctrl.active, null, { timeout: 90000 });
    await sleep(2000);                                   // ~100 frames: 4 x 0.76^100 ~ 5e-12
    const R = await q.evaluate(() => window.__log);
    await q.close();
    const end = R.findLastIndex(r => r.phase);           // the last camera frame inside the action
    const after = R.slice(end + 1), last = R[R.length - 1];
    return { R, end, after, last, name };
  };
  const hold = (q, k) => q.evaluate(k => window.__play.keys.add(k), k);
  const release = (q, k) => q.evaluate(k => window.__play.keys.delete(k), k);
  {   // bottom exit: climb two rungs, then hold S
    const { R, after, last } = await wayOff('bottom', async q => {
      await hold(q, 'KeyW'); await sleep(600); await release(q, 'KeyW');
      await q.waitForFunction(() => window.__play.ctrl.active && window.__play.ctrl.active.phase === 'idle', null, { timeout: 60000 });
      await hold(q, 'KeyS');
      await q.waitForFunction(() => window.__log.some(r => r.phase === 'exitBottom'), null, { timeout: 60000 });
      await release(q, 'KeyS');
    });
    const ex = R.filter(r => r.phase === 'exitBottom'), i0 = R.findIndex(r => r.phase === 'exitBottom');
    ok(ex.length > 5 && R[i0 - 1].target === 4 && ex.every(r => r.target === 0) && after.length > 30 && last.back < 1e-6,
       `bottom exit: CAM+0x1c8 4 -> 0 on its first frame (${ex.length} exit frames, all 0: ${ex.every(r => r.target === 0)}), CAM+0x1c4 back to ${last.back.toExponential(1)}`);
  }
  {   // top exit: hold W to the top
    const { R, after, last } = await wayOff('top', async q => { await hold(q, 'KeyW'); });
    const ex = R.filter(r => r.phase === 'exitTop');
    ok(ex.length > 5 && ex.every(r => r.target === 4) && after.length > 30 && after[0].target === 0 && last.back < 1e-6,
       `top exit: CAM+0x1c8 4 through the exit (${ex.length} frames), 0 the frame it ends (${after.length ? after[0].target : '-'}), CAM+0x1c4 back to ${last.back.toExponential(1)}`);
  }
  {   // jump-off: climb, settle, Space
    const { R, after, last } = await wayOff('jump', async q => {
      await hold(q, 'KeyW'); await sleep(1200); await release(q, 'KeyW');
      await q.waitForFunction(() => window.__play.ctrl.active && window.__play.ctrl.active.phase === 'idle', null, { timeout: 60000 });
      await sleep(300);
      await q.evaluate(() => { window.__play.keys.add('Space'); window.__play.pressed.add('Space'); });
      await sleep(150);
      await release(q, 'Space');
    });
    const air = R.filter(r => r.phase === 'jump' || r.phase === 'fall');
    ok(air.length > 5 && air.some(r => r.phase === 'fall') && air.every(r => r.target === 4) && after.length > 30 && after[0].target === 0 && last.back < 1e-6,
       `jump-off: CAM+0x1c8 4 in the air (${air.length} frames), 0 on landing (${after.length ? after[0].target : '-'}), CAM+0x1c4 back to ${last.back.toExponential(1)}`);
  }
  ok(errs.length === 0, `no page errors (${errs.slice(0, 2).join(' | ')})`);
} finally { await browser.close(); srv.kill(); }
done();
