// The weapon views of the camera (player.js FollowCamera with aim_core.js): replays the PCSX2 aim
// traces (TRACE_FIELDSET=aim; $AIM_TRACES or <repo>/tools/trace/out/aim) open loop from the
// recorded player position and pad, the weapon core driven by the recorded buttons, and checks
// per frame the eye CAM+0x90, the look-at CAM+0xf0, yaw CAM+0x178 / pitch CAM+0x174 and the
// forward row CAM+0x80. Also: the aim ray through the reticle (FUN_00329778) from the camera's own
// matrix and CAM+0x1cc vs the recorded far point CAM+0xb0.
// node --import ./test_hooks.mjs test_aim_camera.mjs [trace ...]
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
const { FollowCamera, cameraRays, padAxis } = await import('./player.js');
const { WeaponSystem } = await import('./aim_core.js');
const { aimRayPoints, reticleRay } = await import('./aim_core.js');

const DIR = process.env.AIM_TRACES || fileURLToPath(new URL('../../tools/trace/out/aim/', import.meta.url));
const J = p => JSON.parse(readFileSync(new URL(p, import.meta.url)));
const W = J('./data/aim/weapon.json'), RET = J('./data/aim/reticle.json');
const buf = readFileSync(new URL('./data/z128_collision.bin', import.meta.url));
const rays = cameraRays(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
const files = process.argv.slice(2).length ? process.argv.slice(2)
  : ['aim_a_tap', 'aim_b_hold', 'aim_c_walk', 'aim_d_turn', 'aim_e_zoom', 'aim_f_r3cold', 'aim_g_air', 'aim_h_holster_run', 'aim_i_fp_holster'].map(n => DIR + '/' + n + '.jsonl');
const hex = h => Buffer.from(h, 'hex');
const F = (h, o) => hex(h).readFloatLE(o);
const V3 = (h, o) => [F(h, o), F(h, o + 4), F(h, o + 8)];
const BTN = { r1: 0x0008, r3: 0x0400, circle: 0x0020 };
const d3 = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
// limits: the follow camera's own trace checks reach 0.0075 (worker 0089)
const LIM = { eye: 0.02, at: 0.002, ang: 1e-4, fwd: 1e-4, far: 0.05 };

let fails = 0;
// The loop below rebuilds one game frame by hand (setupAim needs the DOM). Check that the page's
// own wiring runs the same steps in the same order: play.js follow.tick -> ctrl.step; player.js
// step(): weaponHook.pre -> intent -> weaponHook.weapon (FUN_00579570, in FUN_005786e8 after
// FUN_00578748) -> jump motion -> move -> weaponHook.post (clips); aim.js hook: the aim ray from
// this frame's camera -> core.input, then core.stateMachine -> pitch carry-over -> reticleFrame.
{
  const src = f => readFileSync(new URL(f, import.meta.url), 'utf8');
  const region = (s, a, b) => { const i = s.indexOf(a), j = s.indexOf(b, i + a.length); return i < 0 || j < 0 ? '' : s.slice(i, j); };
  const order = (what, s, toks) => {
    const at = toks.map(t => [t, s.indexOf(t), s.indexOf(t) === s.lastIndexOf(t)]);
    const ok = s && at.every(([, i, once], k) => i >= 0 && once && (k === 0 || i > at[k - 1][1]));
    console.log((ok ? 'ok   ' : 'FAIL ') + 'order ' + what + ': ' + at.map(([t, i, once]) => `${t}${i < 0 ? ' (missing)' : once ? '' : ' (twice)'}`).join(' -> '));
    if (!ok) fails++;
  };
  order('play.js frame', region(src('./play.js'), 'while (acc >= TICK', 'acc -= TICK'), ['follow.tick(', 'ctrl.step(']);
  order('player.js step', region(src('./player.js'), 'if (owned) this.locate();', 'this.frames++;'),
        ['this.weaponHook.pre(', 'this.stickInput(', 'this.intent(', 'this.weaponHook.weapon(', 'this.jump.motionUpdate(', 'this.moveTick()', 'this.rotate()', 'this.weaponHook.post(']);
  const hookSrc = region(src('./aim.js'), 'const hook = {', 'ctrl.weapon = core');
  order('aim.js hook.pre', region(hookSrc, 'pre(input', 'actionEnter'), ['aimRay()', 'core.eye =', 'core.hit =', 'core.input(']);
  order('aim.js hook.weapon', region(hookSrc, 'weapon(input', 'post(input'), ['core.stateMachine(', 'follow.pitch = core.cam.pitch', 'core.reticleFrame(', "core.events.some(e => e.type === 'shot')"]);
}
for (const file of files) {
  if (!existsSync(file)) { console.log('missing', file); fails++; continue; }
  const R = readFileSync(file, 'utf8').trim().split('\n').slice(1).map(l => JSON.parse(l));
  const core = new WeaponSystem(W, RET);
  const r0 = R[0], c0 = r0.cam_170;
  core.P.acc = F(r0.p_weapon, 4); core.P.st = hex(r0.p_weapon).readUInt32LE(0x14); core.P.T = F(r0.p_weapon, 0);
  { // a trace logged from mid-run (log_from > 0): start from its weapon and camera state
    const c = core.cam, h = hex(c0), drawn = (hex(r0.p_weapon).readUInt32LE(0x18) & 1) === 1;
    c.view = h.readInt32LE(0xac); c.type = hex(r0.cam_view).readUInt32LE(0); c.phase = hex(r0.cam_view).readInt16LE(4);
    c.ramp = F(c0, 0xc); c.flags = h.readUInt32LE(0x78); c.fade = h[0x70]; c.fov = F(c0, 0); c.pitch = F(c0, 4);
    c.k198 = F(c0, 0x28); c.k19c = F(c0, 0x2c); c.kick = F(c0, 0xb4); c.a1a0 = F(c0, 0x30); c.a1a4 = F(c0, 0x34);
    c.d1d8 = F(c0, 0x68); c.d1dc = F(c0, 0x6c);
    if (drawn) { core.P.drawn = true; core.P.m29c = 3; core.P.state = 3; }
    core.hud.mode = hex(r0.hud).readUInt32LE(0); core.r.kind = hex(r0.ret)[0];
  }
  const cam = new FollowCamera(null, rays);
  cam.reset({ x: r0.matrix[3][0], y: r0.matrix[3][1], z: r0.matrix[3][2] }, { pitch: F(c0, 4), yaw: F(c0, 8) });
  cam.eye = V3(r0.cam_60, 0x30);
  cam.M = [0, 1, 2].map(i => V3(r0.cam_60, 16 * i));
  cam.dist = F(c0, 0x50); cam.snap = false; cam.base = [r0.matrix[3][0], r0.matrix[3][1], r0.matrix[3][2]];
  cam.weapon = core;
  const err = { eye: [0, 0], at: [0, 0], ang: [0, 0], fwd: [0, 0], far: [0, 0] };
  const put = (k, v, f) => { if (v > err[k][0]) err[k] = [v, f]; };
  let n = 0, leadN = 0, skipN = 0, jumped = false, turned = false;
  for (let f = 0; f + 1 < R.length; f++) {
    const r = R[f], g = R[f + 1], pad = r.pad_in, prevB = f ? R[f - 1].pad_in.buttons : 0;
    const pressed = m => !!((pad.buttons & m) && !(prevB & m));
    const lx = padAxis(pad.lx - 128, pad.ly - 128), ly = padAxis(pad.ly - 128, pad.lx - 128);
    const rx = padAxis(pad.rx - 128, pad.ry - 128), ry = padAxis(pad.ry - 128, pad.rx - 128);
    // C+0xc0 isn't in the aim field set: the recorded facing stands in (auto turn-behind only)
    const fw = r.matrix[2];
    cam.tick({ pos: { x: r.matrix[3][0], y: r.matrix[3][1], z: r.matrix[3][2] }, want: { x: fw[0], y: fw[1], z: fw[2] },
               state: hex(r.p_state)[4], stick: Math.hypot(lx, ly), pad: [rx, ry] });
    // the aim ray from this frame's camera (FUN_00329778), the world hit taken from the trace
    const look = { x: cam.at[0], y: cam.at[1], z: cam.at[2] };
    const O = { x: cam.eye[0], y: cam.eye[1], z: cam.eye[2] };
    const Fd = reticleRay(cam.M, core.cam.c1cc);
    const pts = aimRayPoints(O, Fd, look, W.range10, null);
    const gfar = V3(g.cam_60, 0x50), ghit = V3(g.cam_60, 0x40);
    core.eye = O; core.hit = { x: ghit[0], y: ghit[1], z: ghit[2] };
    const before = core.cam.pitch;
    core.input({ r1Pressed: pressed(BTN.r1), r1Held: !!(pad.buttons & BTN.r1), r3Pressed: pressed(BTN.r3),
                 circlePressed: pressed(BTN.circle) }, (r.mflags & 0x200) !== 0, { x: gfar[0], y: gfar[1], z: gfar[2] });
    core.stateMachine(core.hit);
    if (core.cam.pitch !== before) cam.pitch = core.cam.pitch;   // shot: pitch carry-over (message 0x3e9)
    core.reticleFrame(20);
    // not comparable: (1) after a jump with the gun out the game's eye leaves the view axis by ~4.9
    // (FUN_00575768 -> FUN_0032a0f8 sets CAM+0x1e8 0x1000000 / CAM+0x220; their reader is not in
    // the FUN_0032ace0 decomp: OPEN, notes/fidelity-audit.md); (2) the auto turn-behind in view 1
    // needs C+0xc0, which the aim field set doesn't record (the facing stands in)
    if ((r.mflags & 0x200) === 0 || hex(r.p_state)[4] === 8) jumped = true;
    if (core.cam.type === 0xc && cam.gain > 0.1) turned = true;   // from here on the yaw can differ
    if (jumped || turned) { skipN++; continue; }
    // compare with the record after this frame
    const ge = V3(g.cam_60, 0x30), gl = V3(g.cam_f0, 0), gf = V3(g.cam_60, 0x20);
    put('eye', d3(cam.eye, ge), g.frame);
    put('at', d3(cam.at, gl), g.frame);
    const dy = Math.abs(Math.atan2(Math.sin(cam.yaw - F(g.cam_170, 8)), Math.cos(cam.yaw - F(g.cam_170, 8))));
    put('ang', Math.max(dy, Math.abs(cam.pitch - F(g.cam_170, 4))), g.frame);
    put('fwd', d3(cam.M[2], gf), g.frame);
    // the far point needs the game's ray: CAM+0x1fc / +0x200 (the reticle lead from the pad's right
    // stick, FUN_0032ace0 l.1700-1722) aren't on the page (no right stick; 0 with the USB mouse):
    // frames with a lead are counted, not compared. In view 1 the game doesn't update CAM+0xb0.
    const lead = F(g.cam_170, 0x8c) !== 0 || F(g.cam_170, 0x90) !== 0;
    if (lead) leadN++;
    if (!lead && hex(g.cam_view).readUInt32LE(0) !== 0xc) put('far', d3([pts.far.x, pts.far.y, pts.far.z], gfar), g.frame);
    if (process.env.DBG && g.frame >= +process.env.DBG.split(':')[0] && g.frame <= +process.env.DBG.split(':')[1])
      console.log(g.frame, 'type', core.cam.type.toString(16), 'ph', core.cam.phase, 'e', core.cam.e.toFixed(3), 'eye', d3(cam.eye, ge).toFixed(4), 'at', d3(cam.at, gl).toFixed(4),
                  'dist', cam.dist.toFixed(3), F(g.cam_170, 0x50).toFixed(3), 'far', d3([pts.far.x, pts.far.y, pts.far.z], gfar).toFixed(2), 'kick', core.cam.kick.toFixed(3), F(g.cam_170, 0xb4).toFixed(3),
                  'st', hex(r.p_state)[4], 'd', cam.eye.map((v, j) => (v - ge[j]).toFixed(3)).join(','), 'pos', r.matrix[3].slice(0, 3).map(v => v.toFixed(2)).join(','), 'gain', cam.gain.toFixed(3), F(g.cam_170, 0x38).toFixed(3));
    n++;
  }
  const bad = Object.keys(LIM).filter(k => err[k][0] > LIM[k]);
  console.log(file.split('/').pop().padEnd(24), `frames ${n} (lead ${leadN}, not compared ${skipN})  ` + Object.keys(err).map(k => `${k} ${err[k][0].toExponential(1)} (f${err[k][1]})`).join('  ') + (bad.length ? '  FAIL ' + bad.join(',') : '  ok'));
  if (bad.length) fails++;
}
process.exit(fails ? 1 : 0);
