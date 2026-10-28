// Wall-slide regression test (worker 0096): replays PCSX2 traces of running into the
// z128 start-deck edge through player.js + collision.js and checks, per frame, the
// position and the actual slide speed (displacement x 50) against the game while the
// game's wall bit C+0x1c0 0x400 (set by FUN_00357d48 on a boundary-edge slide) is on.
//   node --import ./test_hooks.mjs test_wall.mjs
// Traces are game-derived (tools/trace/out, gitignored); missing ones are skipped.
// Record them with tools/trace/scripts/j_wall{30,45,60,80,_along}.json and c_run.json.
import { readFileSync, existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const WEB = process.env.WEB ?? path.dirname(fileURLToPath(import.meta.url));
const OUT = process.env.TRACE_OUT ?? path.join(WEB, '../../tools/trace/out');
const { PlayerController, SpeedAnimator, padAxis, stickToWorld } = await import(path.join(WEB, 'player.js'));
const { Collision } = await import(path.join(WEB, 'collision.js'));

const buf = readFileSync(path.join(WEB, 'data/z128_collision.bin'));
const col = new Collision(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
const KEY = { h_st: 'idle', h_wk: 'walk', h_rn: 'run' };
const headOf = d => d.mp_layers[Buffer.from(d.mp_head, 'hex')[8] & 7];
// Position: j_wall80 drifts 0.00012 u/frame along the wall (0.022 after 180 frames), the same
// size as the open-ground f32 drift (navMove works in doubles; the PS2 rounds every VU0 op).
const POS_TOL = 0.03;
const SPD_TOL = 2.0;       // u/s per frame (the reported gap was 42 vs 59.5)

function replay(file) {
  const [, ...R] = readFileSync(file, 'utf8').trim().split('\n').map(l => JSON.parse(l));
  const r0 = R[0], p0 = r0.matrix[3], f0 = r0.matrix[2];
  const ctrl = new PlayerController(col, { pos: { x: p0[0], y: p0[1], z: p0[2] }, heading: Math.atan2(f0[0], f0[2]) });
  ctrl.C.face = { x: f0[0], y: f0[1], z: f0[2] };
  const anim = Object.create(SpeedAnimator.prototype);
  const h0 = headOf(r0);
  Object.assign(anim, { frames: { idle: 160, walk: 40, run: 20 }, head: { key: KEY[h0.clip], frame: h0.frame, prevFrame: h0.frame },
                        out: null, blend: { len: 0, rem: 0, w: 1 }, installed: false, cur: -1, override: null,
                        rate: 0.5, current: KEY[h0.clip], actions: {}, names: {} });
  ctrl.attach({ anim });
  let maxPos = 0, maxSpd = 0, wallFrames = 0, slideSum = 0, peak = 0, pagePeak = 0;
  let prev = { ...ctrl.pos };
  for (let i = 0; i + 1 < R.length; i++) {
    const d = R[i], g = R[i + 1], pad = d.pad_in;
    if (pad.buttons) throw new Error(`${file}: buttons are not replayed here`);
    const sx = padAxis(pad.lx - 128, pad.ly - 128), sy = padAxis(pad.ly - 128, pad.lx - 128);
    const cm = g.cam_matrix, w = stickToWorld(sx, sy, cm[0], cm[2]);
    ctrl.step({ x: w.x, z: w.z, stick: w.mag, run: false, keys: new Set(), pressed: new Set() });
    const gp = g.matrix[3], gq = d.matrix[3];
    maxPos = Math.max(maxPos, Math.hypot(ctrl.pos.x - gp[0], ctrl.pos.z - gp[2]));
    if (g.mflags & 0x400) {
      const ps = Math.hypot(ctrl.pos.x - prev.x, ctrl.pos.z - prev.z) * 50;
      const gs = Math.hypot(gp[0] - gq[0], gp[2] - gq[2]) * 50;
      maxSpd = Math.max(maxSpd, Math.abs(ps - gs)); wallFrames++; slideSum += gs;
      peak = Math.max(peak, gs); pagePeak = Math.max(pagePeak, ps);
    }
    prev = { ...ctrl.pos };
  }
  return { maxPos, maxSpd, wallFrames, peak, pagePeak, meanSlide: wallFrames ? slideSum / wallFrames : 0 };
}

let fails = 0, ran = 0;
for (const name of ['c_run', 'j_wall30', 'j_wall45', 'j_wall60', 'j_wall80', 'j_wall_along', 'f_railing']) {
  const f = path.join(OUT, name + '.jsonl');
  if (!existsSync(f)) { console.log(`skip ${name} (no trace)`); continue; }
  const r = replay(f); ran++;
  const ok = r.maxPos <= POS_TOL && r.maxSpd <= SPD_TOL && r.wallFrames > 0;
  if (!ok) fails++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name.padEnd(13)} wall frames ${String(r.wallFrames).padStart(3)}  slide peak game/page ${r.peak.toFixed(1)}/${r.pagePeak.toFixed(1)} u/s  ` +
              `max |pos| ${r.maxPos.toExponential(2)}  max |slide speed| ${r.maxSpd.toExponential(2)}`);
}
console.log(ran ? (fails ? `${fails} FAILED` : 'all passed') : 'no traces found');
process.exit(fails ? 1 : 0);
