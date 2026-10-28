// The follow camera (player.js FollowCamera = FUN_00330478 -> FUN_0032ace0, type 0xc / view 1)
// against PCSX2 camera traces (TRACE_FIELDSET=cam; $CAM_TRACES or <repo>/tools/trace/out/cam,
// game-derived, never committed; scripts in tools/trace/scripts/cam_*.json).
//  - open loop (worker 0089's six: idle, walk, run, right-stick turn, run in a circle, look): the
//    camera runs on its own from the first record, fed the recorded player position, C+0xc0 and pad;
//    eye CAM+0x90, look-at CAM+0xf0, yaw CAM+0x178, pitch CAM+0x174 checked every frame.
//  - per tick (worker 0099's near-eye traces: a wall pulls the eye in behind him, then he turns
//    the camera or runs away and it eases out): each frame starts from the game's previous eye,
//    matrix, distance CAM+0x1c0 and angles, so one rule at a time is checked. These need the view 1
//    near-eye ease-out (last eye within 17.5 of base + pivot -> rate (0.2 - 0.03)(17.5 - d)/10 + 0.03);
//    the test asserts that frames using it occur. Frames where the walk-away x0.1 test is a
//    float tie (pinned on a wall, |dNew - dOld| < TIE) are counted, not compared.
// node --import ./test_hooks.mjs test_cam_traces.mjs [player.js path]
import { readFileSync, existsSync } from 'node:fs';
import { pathToFileURL, fileURLToPath } from 'node:url';
const P = process.argv[2] ? pathToFileURL(process.argv[2]).href : new URL('./player.js', import.meta.url).href;
const { FollowCamera, cameraRays, padAxis, CAM_TYPE_C } = await import(P);
const DIR = process.env.CAM_TRACES || fileURLToPath(new URL('../../tools/trace/out/cam/', import.meta.url));
const buf = readFileSync(new URL('./data/z128_collision.bin', import.meta.url));
const rays = cameraRays(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
const hex = h => Buffer.from(h, 'hex');
const F = (h, o) => hex(h).readFloatLE(o);
const V3 = (h, o) => [F(h, o), F(h, o + 4), F(h, o + 8)];
const d3 = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
const wrap = a => Math.atan2(Math.sin(a), Math.cos(a));
const TIE = 1e-3;
// open loop: worker 0089's bounds (eye 0.0075); per tick: the near_back wall measures 0.048
// closer than the game while pinned (collision, not the ease; see notes/fidelity-audit.md)
const LIM = { open: { eye: 0.01, at: 0.002, yaw: 1e-5, pitch: 3e-4 }, tick: { eye: 0.06, at: 0.002, yaw: 1e-5, pitch: 1e-5, out: 0.01 } };
const CASES = [
  ['cam_idle', 'open'], ['cam_walk', 'open'], ['cam_run', 'open'], ['cam_turn', 'open'],
  ['cam_lturn', 'open'], ['cam_look', 'open'], ['cam_near_back', 'tick'], ['cam_near_turn', 'tick'],
];

let fails = 0;
for (const [name, mode] of CASES) {
  const file = DIR + '/' + name + '.jsonl';
  if (!existsSync(file)) { console.log('FAIL missing', file); fails++; continue; }
  const R = readFileSync(file, 'utf8').trim().split('\n').slice(1).map(l => JSON.parse(l));
  // worker 0089 recorded idle / walk / turn / look before C+0xc0 was in the field set: fine while
  // the left stick stays under 0.5 (the auto turn-behind gain stays 0); checked below
  const noWant = !R[0].want_dir_speed;
  const r0 = R[0], c0 = r0.cam_170;
  const cam = new FollowCamera(null, rays);
  cam.reset({ x: r0.matrix[3][0], y: r0.matrix[3][1], z: r0.matrix[3][2] }, { pitch: F(c0, 4), yaw: F(c0, 8) });
  cam.eye = V3(r0.cam_60, 0x30);
  cam.M = [0, 1, 2].map(i => V3(r0.cam_60, 16 * i));
  cam.dist = F(c0, 0x50);
  const err = { eye: [0, 0], at: [0, 0], yaw: [0, 0], pitch: [0, 0], out: [0, 0] };
  const put = (k, v, f) => { if (v > err[k][0]) err[k] = [v, f]; };
  let n = 0, ties = 0, nearOut = 0, gainUsed = false;
  for (let i = 0; i + 1 < R.length; i++) {
    const r = R[i], g = R[i + 1], pad = r.pad_in;
    if (mode === 'tick') {                        // start from the game's state after the previous frame
      cam.eye = V3(r.cam_60, 0x30); cam.M = [0, 1, 2].map(k => V3(r.cam_60, 16 * k));
      cam.dist = F(r.cam_170, 0x50); cam.yaw = F(r.cam_170, 8); cam.pitch = F(r.cam_170, 4);
    }
    const lx = padAxis(pad.lx - 128, pad.ly - 128), ly = padAxis(pad.ly - 128, pad.lx - 128);
    const rx = padAxis(pad.rx - 128, pad.ry - 128), ry = padAxis(pad.ry - 128, pad.rx - 128);
    const w = r.want_dir_speed || [0, 0, 0];
    // the near-eye test's inputs, as place() takes them (last eye, this frame's base + pivot)
    const le = cam.eye.slice();
    // the cam field set has no P+0x104: on foot, standing or moving = state 2 (the slow set 2-5)
    cam.tick({ pos: { x: r.matrix[3][0], y: r.matrix[3][1], z: r.matrix[3][2] }, want: { x: w[0], y: w[1], z: w[2] },
               state: 2, stick: Math.hypot(lx, ly), pad: [rx, ry] });
    const pv = CAM_TYPE_C.pivot, b = cam.base;
    const near = d3([b[0] + pv[0], b[1] + pv[1], b[2] + pv[2]], le) < 17.5;
    if (near && F(g.cam_170, 0x50) > F(r.cam_170, 0x50)) nearOut++;
    if (cam.gain > 0) gainUsed = true;
    // a tie only matters when the two factors differ (x0.1 vs |stick| x 0.9 + 0.1)
    const gap = cam.ease ? cam.ease.slowGap : null;
    if (gap !== null && Math.abs(gap) < TIE && Math.hypot(lx, ly) > 0) {
      ties++;
      if (mode === 'tick') continue;
    }
    put('eye', d3(cam.eye, V3(g.cam_60, 0x30)), g.frame);
    put('at', d3(cam.at, V3(g.cam_f0, 0)), g.frame);
    put('yaw', Math.abs(wrap(cam.yaw - F(g.cam_170, 8))), g.frame);
    put('pitch', Math.abs(cam.pitch - F(g.cam_170, 4)), g.frame);
    // the distance CAM+0x1c0 on the frames it eases out (where the near-eye rule acts)
    // (both ease out: where only one does, the collision measured a different distance)
    if (mode === 'tick' && F(g.cam_170, 0x50) > F(r.cam_170, 0x50) && cam.ease.out) put('out', Math.abs(cam.dist - F(g.cam_170, 0x50)), g.frame);
    n++;
  }
  const L = LIM[mode];
  const bad = Object.keys(L).filter(k => err[k][0] > L[k]);
  if (mode === 'tick' && nearOut === 0) bad.push('no near-eye frames');
  if (noWant && gainUsed) bad.push('auto turn-behind acts but the trace has no want_dir_speed');
  console.log(name.padEnd(14), mode.padEnd(5), `frames ${n} (ties ${ties}, near-eye ease-out ${nearOut})  ` +
              Object.keys(err).map(k => `${k} ${err[k][0].toExponential(1)} (f${err[k][1]})`).join('  ') + (bad.length ? '  FAIL ' + bad.join(',') : '  ok'));
  if (bad.length) fails++;
}
process.exit(fails ? 1 : 0);
