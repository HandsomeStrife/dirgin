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
const { FollowCamera, cameraRays, padAxis, CAM_TYPE_C, PlayerController, stickToWorld } = await import(P);
const { WeaponSystem, aimRayPoints, reticleRay } = await import(new URL('./aim_core.js', import.meta.url).href);
const { airState } = await import(new URL('./jump_core.js', import.meta.url).href);
const { Collision } = await import(new URL('./collision.js', import.meta.url).href);
const DIR = process.env.CAM_TRACES || fileURLToPath(new URL('../../tools/trace/out/cam/', import.meta.url));
const buf = readFileSync(new URL('./data/z128_collision.bin', import.meta.url));
const rays = cameraRays(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
const col = new Collision(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
const J = p => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const hex = h => Buffer.from(h, 'hex');
const F = (h, o) => hex(h).readFloatLE(o);
const V3 = (h, o) => [F(h, o), F(h, o + 4), F(h, o + 8)];
const d3 = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
const wrap = a => Math.atan2(Math.sin(a), Math.cos(a));
const TIE = 1e-3;
// open loop: worker 0089's bounds (eye 0.0075); per tick: 1.3e-3 since the view-1 ray start
// (DAT_0046e4d4, worker 0143; before it the walls measured 0.048 closer than the game)
// page: closed loop (below); the player's run-up and air drift (test_jump_page: run-up 2.2e-3 + air
// railing slide 2.7e-3) moves the base, which can flip one step of the look point's quantised angle
// change (pi / 4096 = 7.67e-4 rad: cam_j_run f44 yaw, cam_j_back f75 pitch x 0.25 x 2), and the back
// jump's railing pulled the eye in 0.048 before the view-1 ray start (now 3.9e-3)
const LIM = { open: { eye: 0.01, at: 0.002, yaw: 1e-5, pitch: 3e-4 }, tick: { eye: 0.01, at: 0.002, yaw: 1e-5, pitch: 1e-5, out: 0.01 },
              page: { eye: 0.035, at: 0.005, yaw: 8e-4, pitch: 5e-4, a220: 1e-5, pos: 6e-3 } };
// worker 0107's jump traces (scripts tools/trace/scripts/cam_j_*.json): open loop with the player
// state P+0x104 from the trace (the second jump's landing has one quantiser tie in the look point's
// pitch change, 1.9e-4: within 'open', not 'tick'); the back jump per tick (pinned on the railing:
// the collision residual); and every one closed loop through the page ('page')
const CASES = [
  ['cam_idle', 'open'], ['cam_walk', 'open'], ['cam_run', 'open'], ['cam_turn', 'open'],
  ['cam_lturn', 'open'], ['cam_look', 'open'], ['cam_near_back', 'tick'], ['cam_near_turn', 'tick'],
  ['cam_j_stand', 'open'], ['cam_j_run', 'open'], ['cam_j_double', 'open'], ['cam_j_back', 'tick'],
  ['cam_j_stand', 'page'], ['cam_j_run', 'page'], ['cam_j_double', 'page'], ['cam_j_back', 'page'], ['cam_j_gun', 'page'],
  ['cam_j_gun_holster', 'page'], ['cam_j_gun_near', 'page'],
  ['cam_j_draw_r1', 'page'], ['cam_j_draw_r3', 'page'], ['cam_j_draw_gate', 'page'],
  // worker 0109: gun-out back jump (turn mode 3) and second jump (height - 0.1), and a draw in the
  // jump's wind-up (P+0x100 bits 46-47 already 1, still on the ground: state 9, no fall restart)
  ['cam_j_gun_back', 'page'], ['cam_j_gun_double', 'page'], ['cam_j_draw_windup', 'page'],
  // gun out, then circle / R3 in the wind-up: holster into state 9 with no state-2 enter (the camera jump
  // flag stays); R3 with the gun out is first person only (state 8 stays)
  ['cam_j_gun_windup_holster', 'page'], ['cam_j_gun_windup_r3', 'page'],
  // worker 0142: the ladder (l_ladder_cam: standing ✕ mount at the bottom, climb to the top exit with W,
  // walk off). CAM+0x1c4 eases to CAM+0x1c8 = 4 and pulls the base back along C+0x20; the player's
  // writes of CAM+0x1c8 (FUN_00328640) come from the trace, as the position does
  ['l_ladder_cam', 'open'],
  // and the right stick on the ladder (l_ladder_cam_look: a few rungs, then full right, then left + up).
  // Turned into the wall the game's eye comes in to ~3.3 from the base on the camera collision. The
  // rays start DAT_0046e4d4 = 1.5 further back in view 1 (worker 0143): without it ~130 frames were
  // 0.02-0.24 off and f667 1.3 (a corner ray grazing the wall hit the other face of the slab)
  ['l_ladder_cam_look', 'open'],
];

// 'page': the page's own frame, as play.js / aim.js wire it, fed only the trace's pad: follow.tick
// (state = ctrl.P.state) -> ctrl.step (stick through the camera's new matrix; the ✕ edge -> jump;
// the weapon hook: the aim ray from this frame's camera -> WeaponSystem.input, then stateMachine /
// reticleFrame). Checked every frame: eye, look-at, yaw, pitch, CAM+0x220 (the gun-out jump's eye
// offset blend) and flag 0x1000000, and the player position. The gun-out jump (cam_j_gun) needs
// FUN_00575768's FUN_0032a0f8 (take-off), the state 2 / 3 enter's clear (landing) and
// FUN_0032a180 / FUN_0032a1d8 (FollowCamera.place).
// P+0x100 bits 46-47: byte 5 of the u64, bits 6-7
const jbits = r => (hex(r.p_state)[5] >> 6) & 3;
function runPage(name, R) {
  const r0 = R[0], c0 = r0.cam_170, p0 = r0.matrix[3], f0 = r0.matrix[2];
  const ctrl = new PlayerController(col, { pos: { x: p0[0], y: p0[1], z: p0[2] }, heading: Math.atan2(f0[0], f0[2]) });
  ctrl.C.face = { x: f0[0], y: f0[1], z: f0[2] };
  ctrl.P.state = hex(r0.p_state)[4];
  ctrl.P.jumpBits = jbits(r0);
  const W = J('./data/aim/weapon.json');
  const core = new WeaponSystem(W, J('./data/aim/reticle.json'));
  const cam = new FollowCamera(null, rays);
  cam.reset({ x: p0[0], y: p0[1], z: p0[2] }, { pitch: F(c0, 4), yaw: F(c0, 8) });
  cam.eye = V3(r0.cam_60, 0x30); cam.M = [0, 1, 2].map(i => V3(r0.cam_60, 16 * i));
  cam.dist = F(c0, 0x50); cam.hit = (hex(c0).readUInt32LE(0x78) & 0x80) !== 0;
  cam.weapon = core;
  let i = 0, lastRay = null;
  const btn = m => R[i].pad_in.buttons & m, prevBtn = m => (i ? R[i - 1].pad_in.buttons : 0) & m;
  ctrl.weapon = core;
  ctrl.weaponHook = {
    pre() {
      const O = { x: cam.eye[0], y: cam.eye[1], z: cam.eye[2] }, look = { x: cam.at[0], y: cam.at[1], z: cam.at[2] };
      lastRay = aimRayPoints(O, reticleRay(cam.M, core.cam.c1cc), look, W.range10, (a, b) => col.segment(a, b));
      core.eye = O; core.hit = lastRay.hit;
      core.input({ r1Pressed: !!(btn(8) && !prevBtn(8)), r1Held: !!btn(8), r3Pressed: !!(btn(0x400) && !prevBtn(0x400)),
                   circlePressed: !!(btn(0x20) && !prevBtn(0x20)) }, ctrl.onGround, lastRay.far, ctrl);
    },
    weapon() {
      const before = core.cam.pitch;
      core.stateMachine(lastRay ? lastRay.hit : null);
      if (core.cam.pitch !== before) cam.pitch = core.cam.pitch;
      core.reticleFrame(20);
      return core.events.some(e => e.type === 'shot');
    },
    post() {},
  };
  const err = { eye: [0, 0], at: [0, 0], yaw: [0, 0], pitch: [0, 0], a220: [0, 0], pos: [0, 0] };
  const put = (k, v, f) => { if (v > err[k][0]) err[k] = [v, f]; };
  let flagOff = 0, flagF = null, bitsOff = 0, bitsF = null, jOff = 0, jF = null, stateOff = 0, stateF = null, maxA = 0, n = 0;
  for (i = 0; i + 1 < R.length; i++) {
    const r = R[i], g = R[i + 1], pad = r.pad_in;
    const lx = padAxis(pad.lx - 128, pad.ly - 128), ly = padAxis(pad.ly - 128, pad.lx - 128);
    const rx = padAxis(pad.rx - 128, pad.ry - 128), ry = padAxis(pad.ry - 128, pad.rx - 128);
    cam.tick({ pos: ctrl.pos, want: ctrl.C.dir, state: ctrl.P.state, stick: Math.hypot(lx, ly), pad: [rx, ry] });
    const w = stickToWorld(lx, ly, cam.M[0], cam.M[2]);
    const cross = (pad.buttons & 64) !== 0 && (i === 0 || (R[i - 1].pad_in.buttons & 64) === 0);
    ctrl.step({ x: w.x, z: w.z, stick: w.mag, run: false, keys: new Set(), pressed: new Set(), cross });
    const gc = hex(g.cam_170), ga = gc.readFloatLE(0xb0), gfl = gc.readUInt32LE(0x78);
    put('eye', d3(cam.eye, V3(g.cam_60, 0x30)), g.frame);
    put('at', d3(cam.at, V3(g.cam_f0, 0)), g.frame);
    put('yaw', Math.abs(wrap(cam.yaw - F(g.cam_170, 8))), g.frame);
    put('pitch', Math.abs(cam.pitch - F(g.cam_170, 4)), g.frame);
    put('a220', Math.abs(core.cam.a220 - ga), g.frame);
    put('pos', d3([ctrl.pos.x, ctrl.pos.y, ctrl.pos.z], g.matrix[3]), g.frame);
    if (((core.cam.flags ^ gfl) & 0x1000000) !== 0) { flagOff++; flagF ??= g.frame; }
    // P+0x2a8 bit 0 (drawn) and bit 3 (drawn in the air: FUN_003320c0 l.288's R1 gate)
    if (g.p_2a8 !== undefined && (((ctrl.P.flags2a8 ?? 0) ^ g.p_2a8) & 9) !== 0) { bitsOff++; bitsF ??= g.frame; }
    // P+0x100 bits 46-47 (the take-off bits: jump_core enterState / startJump / weaponSwitch)
    // and the state byte P+0x104 itself (2 / 3 on foot by P+0x100 bit 44: jump_core stateEvent)
    if (ctrl.P.state !== hex(g.p_state)[4]) { stateOff++; stateF ??= g.frame; if (process.env.STATE_DEBUG) console.log(name, g.frame, ctrl.P.state, hex(g.p_state)[4]); }
    if ((ctrl.P.jumpBits ?? 0) !== jbits(g)) { jOff++; jF ??= g.frame; }
    maxA = Math.max(maxA, ga);
    if (process.env.CAM_DEBUG) console.log(g.frame, (cam.yaw - F(g.cam_170, 8)).toExponential(2), 'want', [ctrl.C.dir.x, ctrl.C.dir.z].map(v => v.toFixed(4)).join(','), (R[i + 1].want_dir_speed || []).slice(0, 3).filter((_, j) => j !== 1).map(v => v.toFixed(4)).join(','), 'gain', cam.gain.toFixed(3), hex(g.cam_170).readFloatLE(0x38).toFixed(3), ctrl.P.state, hex(g.p_state)[4], d3(cam.eye, V3(g.cam_60, 0x30)).toExponential(2), core.cam.a220.toFixed(4), ga.toFixed(4), (core.cam.flags >>> 24) & 1, (gfl >>> 24) & 1);
    n++;
  }
  const L = LIM.page, bad = Object.keys(L).filter(k => err[k][0] > L[k]);
  if (flagOff) bad.push(`flag 0x1000000 off ${flagOff} (first f${flagF})`);
  if (bitsOff) bad.push(`P+0x2a8 bits 0/3 off ${bitsOff} (first f${bitsF})`);
  if (jOff) bad.push(`P+0x100 bits 46-47 off ${jOff} (first f${jF})`);
  if (stateOff) bad.push(`P+0x104 state off ${stateOff} (first f${stateF})`);
  console.log(name.padEnd(14), 'page ', `frames ${n} (CAM+0x220 max ${maxA.toFixed(3)})  ` +
              Object.keys(err).map(k => `${k} ${err[k][0].toExponential(1)} (f${err[k][1]})`).join('  ') +
              (bad.length ? '  FAIL ' + bad.join(',') : '  ok'));
  return bad.length ? 1 : 0;
}
if (process.env.CAM_ONLY) CASES.splice(0, CASES.length, ...process.env.CAM_ONLY.split(',').map(x => x.split(':')));

let fails = 0;
// FollowCamera.reset (warp / H: FUN_00329f68 -> FUN_003285d0) leaves CAM+0x220 alone
{
  const core = new WeaponSystem(J('./data/aim/weapon.json'), J('./data/aim/reticle.json'));
  const cam = new FollowCamera(null, rays);
  cam.weapon = core; core.cam.a220 = 0.5;
  cam.reset({ x: -1395.15, y: -98.64, z: -1111.01 });
  const ok = core.cam.a220 === 0.5;
  console.log('reset         ', ok ? 'CAM+0x220 kept  ok' : `CAM+0x220 ${core.cam.a220}  FAIL`);
  if (!ok) fails++;
}
for (const [name, mode] of CASES) {
  const file = DIR + '/' + name + '.jsonl';
  if (!existsSync(file)) { console.log('FAIL missing', file); fails++; continue; }
  const R = readFileSync(file, 'utf8').trim().split('\n').slice(1).map(l => JSON.parse(l));
  if (mode === 'page') { fails += runPage(name, R); continue; }
  // worker 0089 recorded idle / walk / turn / look before C+0xc0 was in the field set: fine while
  // the left stick stays under 0.5 (the auto turn-behind gain stays 0); checked below
  const noWant = !R[0].want_dir_speed;
  const r0 = R[0], c0 = r0.cam_170;
  const cam = new FollowCamera(null, rays);
  cam.reset({ x: r0.matrix[3][0], y: r0.matrix[3][1], z: r0.matrix[3][2] }, { pitch: F(c0, 4), yaw: F(c0, 8) });
  cam.eye = V3(r0.cam_60, 0x30);
  cam.M = [0, 1, 2].map(i => V3(r0.cam_60, 16 * i));
  cam.dist = F(c0, 0x50);
  cam.back = F(c0, 0x54);                         // CAM+0x1c4
  let backUsed = false;
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
    // CAM+0x1c8 as the player left it last frame (the camera runs first): FUN_00328640(CAM, on)
    cam.setBack(F(r.cam_170, 0x58) !== 0);
    if (cam.back > 0) backUsed = true;
    // the cam field set has no P+0x104: on foot, standing or moving = state 2 (the slow set 2-5)
    cam.tick({ pos: { x: r.matrix[3][0], y: r.matrix[3][1], z: r.matrix[3][2] }, want: { x: w[0], y: w[1], z: w[2] },
               facing: { x: r.matrix[2][0], y: r.matrix[2][1], z: r.matrix[2][2] },
               state: r.p_state ? hex(r.p_state)[4] : 2, stick: Math.hypot(lx, ly), pad: [rx, ry] });
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
    // ladder: on a settled distance the measured one equals CAM+0x1c0 to ~1e-4, so the in / out
    // branch (in: measured + the whole recede; out: ease + recede x (d - 25) / 35) is a float tie;
    // the mount starts from one (l_ladder_cam f502: the game goes in). Counted, and CAM+0x1c0 taken
    // from the game on that frame
    if (name.startsWith('l_ladder') && Math.abs(cam.ease.inGap) < TIE && Math.abs(cam.dist - F(g.cam_170, 0x50)) > TIE) {
      ties++; cam.dist = F(g.cam_170, 0x50); cam.eye = V3(g.cam_60, 0x30);
      continue;
    }
    if (process.env.CAM_DEBUG) console.log(g.frame, d3(cam.eye, V3(g.cam_60, 0x30)).toExponential(2), (cam.yaw - F(g.cam_170, 8)).toExponential(2), (cam.pitch - F(g.cam_170, 4)).toExponential(2), cam.dist.toFixed(4), F(g.cam_170, 0x50).toFixed(4), cam.ease.out, gap);
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
  if (name.startsWith('l_ladder') && !backUsed) bad.push('no ladder pull-back frames');
  console.log(name.padEnd(14), mode.padEnd(5), `frames ${n} (ties ${ties}, near-eye ease-out ${nearOut})  ` +
              Object.keys(err).map(k => `${k} ${err[k][0].toExponential(1)} (f${err[k][1]})`).join('  ') + (bad.length ? '  FAIL ' + bad.join(',') : '  ok'));
  if (bad.length) fails++;
}
process.exit(fails ? 1 : 0);
