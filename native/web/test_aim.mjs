// node --import ./test_hooks.mjs test_aim.mjs [trace.jsonl ...] -- replays the PCSX2 aim traces (tools/trace,
// TRACE_FIELDSET=aim; default folder <repo>/tools/trace/out/aim, or $AIM_TRACES) and checks,
// frame by frame (sim frame f = pad_in of record f -> record f+1):
//  core   aim_core.js: weapon state / timer / accumulator / drawn (FUN_00579570), camera type /
//         phase / ramp / fade / FOV (FUN_0032a420, FUN_0032ace0), HUD mode, reticle offset
//         HUD+0x78, recoil CAM+0x198, reticle kind / phase (FUN_003b0440 / FUN_003b0ac0)
//  turn   player.js intent() (its weapon early return) + aimTurn() + rotate() (rotation mode 6,
//         FUN_0034de50 case 6), ground and air, against the recorded facing
//  clips  aim.js makeClipControl() driving player.js SpeedAnimator's clip choice (FUN_00595440,
//         FUN_00596460) against the recorded head clip
//  ray    aim.js aimRayPoints(): the decoded far point CAM+0xb0 and the hit vs CAM+0xa0
//  face   aim_core.js press + AIM: the face point P+0x120
// three.js is stubbed by test_hooks.mjs (nothing here draws):
//   AIM_TRACES=<repo>/tools/trace/out/aim node --import ./test_hooks.mjs test_aim.mjs
// The camera's weapon views (eye, look-at, switch lerp, kick) are checked by test_aim_camera.mjs.
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
const { WeaponSystem, fracStep } = await import('./aim_core.js');
const { PlayerController, SpeedAnimator, GAME, STEP } = await import('./player.js');
const { makeClipControl, aimRayPoints, mouseR1Gate } = await import('./aim.js');
const { Collision } = await import('./collision.js');

const DIR = process.env.AIM_TRACES || fileURLToPath(new URL('../../tools/trace/out/aim/', import.meta.url));
const J = p => JSON.parse(readFileSync(new URL(p, import.meta.url)));
const W = J('./data/aim/weapon.json'), RET = J('./data/aim/reticle.json');
const MOTIONS = { ...J('./data/vincent/vincent.json').clips, ...J('./data/vincent/vincent_shoot.json').clips };
const cb = readFileSync(new URL('./data/z128_collision.bin', import.meta.url));
const COL = new Collision(cb.buffer.slice(cb.byteOffset, cb.byteOffset + cb.byteLength));
const files = process.argv.slice(2).length ? process.argv.slice(2)
  : ['aim_a_tap', 'aim_b_hold', 'aim_c_walk', 'aim_d_turn', 'aim_e_zoom', 'aim_f_r3cold', 'aim_g_air', 'aim_h_holster_run'].map(n => DIR + '/' + n + '.jsonl');

const hex = h => Buffer.from(h, 'hex');
const F = (h, o) => hex(h).readFloatLE(o);
const U = (h, o) => hex(h).readUInt32LE(o);
const B = (h, o) => hex(h)[o];
const V = (h, o) => ({ x: F(h, o), y: F(h, o + 4), z: F(h, o + 8) });
const BTN = { r1: 0x0008, r3: 0x0400, circle: 0x0020 };
const dist = (a, b) => Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z);

function rec(r) {
  const w = r.p_weapon, c = r.cam_170;
  return {
    st: U(w, 0x14), T: F(w, 0), acc: F(w, 4), drawn: (U(w, 0x18) & 1) === 1,
    type: U(r.cam_view, 0), phase: hex(r.cam_view).readInt16LE(4), ramp: F(c, 0xc), fade: B(c, 0x70), fov: F(c, 0),
    pitch: F(c, 4), hudMode: U(r.hud, 0), offY: F(r.hud, 0x18),
    kind: B(r.ret, 0), rphase: B(r.ret, 2 + B(r.ret, 0)),
    k198: F(c, 0x28), k19c: F(c, 0x2c), d1d8: F(c, 0x68), d1dc: F(c, 0x6c), kick: F(c, 0xb4),
    onGround: (r.mflags & 0x200) !== 0, buttons: r.pad_in.buttons, pstate: hex(r.p_state)[4],
    pos: r.matrix[3], face: r.matrix[2], facePt: V(r.p_face, 8),
    eye: V(r.cam_60, 0x30), hit: V(r.cam_60, 0x40), far: V(r.cam_60, 0x50), look: V(r.cam_f0, 0),
    clip: r.mp_layers[parseInt(r.mp_head.slice(16, 18), 16)].clip, speed: r.ground_speed,
  };
}

// the clip clock without three.js (as the walk/run replay harness does)
function headlessAnim() {
  const a = Object.create(SpeedAnimator.prototype);
  Object.assign(a, { frames: { idle: 160, walk: 40, run: 20 }, head: { key: 'idle', frame: 0, prevFrame: 0 },
                     out: null, blend: { len: 0, rem: 0, w: 1 }, installed: false, cur: -1, override: null,
                     rate: 0.5, current: 'idle', actions: {}, names: {}, table: GAME.loco, request: null });
  a.has = n => !!MOTIONS[n];
  a.ensure = function (k) { if (this.frames[k]) return true; if (!MOTIONS[k]) return false; this.frames[k] = MOTIONS[k].frames; return true; };
  return a;
}
const NAME = { idle: 'h_st', walk: 'h_wk', run: 'h_rn' };
const stubCol = { navMove() {}, nearestPoly() { return null; }, findPoly() { return null; } };

let fails = 0;
for (const file of files) {
  if (!existsSync(file)) { console.log('missing', file); fails++; continue; }
  const R = readFileSync(file, 'utf8').trim().split('\n').slice(1).map(l => rec(JSON.parse(l)));
  const sim = new WeaponSystem(W, RET);
  sim.P.acc = R[0].acc; sim.P.st = R[0].st; sim.P.T = R[0].T;
  sim.hud.mode = R[0].hudMode; sim.r.kind = R[0].kind;
  // player.js controller for the turn check, and a second one + animator for the clip check
  const pc = new PlayerController(stubCol, { pos: { x: 0, y: 0, z: 0 }, heading: 0 });
  const fakeWpn = { P: { facePt: null }, isAiming: () => true };
  pc.weapon = fakeWpn;
  const anim = headlessAnim();
  const actrl = { C: { groundSpeed: 0, face: { x: 0, y: 0, z: 1 }, moveDir: { x: 0, y: 0, z: 1 } } };
  const ctl = makeClipControl({ core: sim, anim, ctrl: actrl, motions: MOTIONS, step: STEP });
  const mism = {};
  const note = (k, f, a, b) => { (mism[k] ||= []).push(`${f}:${a}/${b}`); };
  let facePress = 0, turnErr = 0, turnN = 0, clipN = 0, rayN = 0, farErr = 0, hitN = 0, hitOk = 0, faceErr = 0, faceN = 0;
  let lastMove = { x: 0, y: 0, z: 1 };
  for (let f = 0; f + 1 < R.length; f++) {
    const r = R[f], nx = R[f + 1], prevB = f ? R[f - 1].buttons : 0;
    const pressed = m => (r.buttons & m) && !(prevB & m);
    // ---- core (pitch after this frame's stick, minus a same-frame shot's carry-over)
    const shot = (nx.st === 2 && r.st !== 2 && r.st !== 1) || (r.st === 1 && nx.st === 2);
    const k198u = Math.fround(Math.fround(Math.fround(Math.fround(sim.cam.k19c - sim.cam.k198) * 0.3) * 1.1999999284744263) + sim.cam.k198);
    sim.switchAngles();                            // l.1509 (the pitch comes from the trace)
    sim.cameraFrame(nx.pitch - (shot && !(W.flags & 0x10) ? k198u : 0));
    sim.kickDecay();
    sim.eye = nx.eye; sim.hit = nx.hit;            // CAM+0x90 / +0xa0 after this frame's camera
    sim.input({ r1Pressed: !!pressed(BTN.r1), r1Held: !!(r.buttons & BTN.r1), r3Pressed: !!pressed(BTN.r3),
                circlePressed: !!pressed(BTN.circle) }, r.onGround, nx.far);
    sim.stateMachine(nx.hit);
    sim.reticleFrame(20);
    const s = sim;
    if (s.P.st !== nx.st) note('state', f + 1, s.P.st, nx.st);
    if (Math.abs(s.P.T - nx.T) > 1e-4) note('timer', f + 1, s.P.T, nx.T);
    if (Math.abs(s.P.acc - nx.acc) > 1e-4) note('acc', f + 1, s.P.acc.toFixed(2), nx.acc.toFixed(2));
    if (s.P.drawn !== nx.drawn) note('drawn', f + 1, s.P.drawn, nx.drawn);
    if (s.cam.type !== nx.type) note('camtype', f + 1, s.cam.type, nx.type);
    if (s.cam.phase !== nx.phase) note('camphase', f + 1, s.cam.phase, nx.phase);
    if (Math.abs(s.cam.ramp - nx.ramp) > 2e-3) note('ramp', f + 1, s.cam.ramp.toFixed(3), nx.ramp.toFixed(3));
    if (s.cam.fade !== nx.fade) note('fade', f + 1, s.cam.fade, nx.fade);
    if (Math.abs(s.cam.fov - nx.fov) > 1e-3) note('fov', f + 1, s.cam.fov, nx.fov);
    if (s.hud.mode !== nx.hudMode) note('hudmode', f + 1, s.hud.mode, nx.hudMode);
    if (Math.abs(s.hud.offY - nx.offY) > 2e-3) note('offY', f + 1, s.hud.offY.toFixed(4), nx.offY.toFixed(4));
    if (Math.abs(s.cam.k198 - nx.k198) > 1e-4) note('k198', f + 1, s.cam.k198.toFixed(4), nx.k198.toFixed(4));
    if (Math.abs(s.cam.kick - nx.kick) > 1e-4) note('kick', f + 1, s.cam.kick.toFixed(4), nx.kick.toFixed(4));
    // FUN_0032aaf8's switch angles CAM+0x1d8 / +0x1dc (from the old / new type and |CAM+0xa0 - CAM+0x90|)
    if (Math.abs(s.cam.d1dc - nx.d1dc) > 2e-4 || Math.abs(s.cam.d1d8 - nx.d1d8) > 2e-4)
      note('switchAng', f + 1, `${s.cam.d1d8.toFixed(5)},${s.cam.d1dc.toFixed(5)}`, `${nx.d1d8.toFixed(5)},${nx.d1dc.toFixed(5)}`);
    if (s.r.kind !== nx.kind) note('kind', f + 1, s.r.kind, nx.kind);
    if (s.r.kind && s.r.phase[s.r.kind] !== nx.rphase) note('rphase', f + 1, s.r.phase[s.r.kind], nx.rphase);
    // ---- face point P+0x120 (press: CAM+0xb0, then FUN_005784d8: CAM+0xa0)
    // UNDECODED: on a press frame (draw / shot) the game ends with P+0x120 = CAM+0xa0 although the
    // press writes CAM+0xb0 (FUN_003296e0 a2 = 0 at all four call sites); the later writer isn't
    // found. Press frames are counted, not compared.
    const press = (r.st === 0 && (nx.st === 2 || nx.st === 4 || nx.st === 0xd));
    if (nx.drawn && s.P.facePt) {
      if (press) facePress++;
      else { const e = dist(s.P.facePt, nx.facePt); if (e > 1e-3) note('facept', f + 1, `st${r.st}->${nx.st}`, e.toFixed(1)); faceErr = Math.max(faceErr, e); faceN++; }
    }
    // ---- turn: player.js intent() -> aimTurn() -> rotate(), with the recorded face point
    if (nx.drawn && r.drawn) {
      pc.pos = { x: nx.pos[0], y: nx.pos[1], z: nx.pos[2] };
      pc.C.face = { x: r.face[0], y: r.face[1], z: r.face[2] };
      pc.C.dir = { x: 0, y: 0, z: 0 }; pc.C.spd = 0;
      pc.P.state = nx.pstate === 3 ? 3 : nx.pstate; pc.P.dir = { x: 0, y: 0, z: 0 }; pc.P.b298 = 0; pc.P.b299 = 0;
      fakeWpn.P.facePt = nx.facePt;
      pc.intent(false);
      pc.rotate();
      const want = Math.atan2(nx.face[0], nx.face[2]);
      const e = Math.abs(Math.atan2(Math.sin(pc.heading - want), Math.cos(pc.heading - want))) * 180 / Math.PI;
      turnErr = Math.max(turnErr, e); turnN++;
    }
    // ---- clips: makeClipControl + SpeedAnimator choice (after this frame's move)
    const mv = { x: nx.pos[0] - r.pos[0], y: 0, z: nx.pos[2] - r.pos[2] };
    const blocked = Math.hypot(mv.x, mv.z) < 0.95 * Math.min(r.speed, nx.speed) * 1.2 / 60;
    if (Math.hypot(mv.x, mv.z) > 1e-4 && !blocked) lastMove = mv;   // C+0x90 isn't in the aim field set
    actrl.C.groundSpeed = nx.speed; actrl.C.face = { x: nx.face[0], y: 0, z: nx.face[2] }; actrl.C.moveDir = lastMove;
    if (f === 0) { const k = { h_st: 'idle', h_wk: 'walk', h_rn: 'run' }[r.clip]; if (k) anim.head = { key: k, frame: 0, prevFrame: 0 }; }
    ctl.clips();
    anim.gameFrame(actrl, STEP);
    const air = s => s === 8 || s === 9 || (s >= 0x1f && s <= 0x21);   // no jump action on the page
    if ((nx.drawn || r.drawn) && !air(nx.pstate) && !air(r.pstate) && !/^h_jp/.test(nx.clip)) {   // landing h_jp4*: jump
      const got = NAME[anim.head.key] || anim.head.key;
      clipN++;
      if (got !== nx.clip) note('clip', f + 1, got, nx.clip);
    }
    // ---- aim ray: far point from the decoded constants; hit vs collision.js
    if (sim.cam.type === 0xd && nx.type === 0xd) {
      const Fd = { x: nx.far.x - nx.eye.x, y: nx.far.y - nx.eye.y, z: nx.far.z - nx.eye.z };
      const L = Math.hypot(Fd.x, Fd.y, Fd.z); Fd.x /= L; Fd.y /= L; Fd.z /= L;
      const pts = aimRayPoints(nx.eye, Fd, nx.look, W.range10, (a, b) => COL.segment(a, b));
      farErr = Math.max(farErr, dist(pts.far, nx.far)); rayN++;
      const gameHitWorld = dist(nx.hit, nx.far) > 1;
      if (gameHitWorld) { hitN++; if (pts.hitWorld && dist(pts.hit, nx.hit) < 1.0) hitOk++; }
    }
  }
  const name = file.split('/').pop();
  // aim_e draws and holsters without walking: the locomotion table must still be uninstalled
  // (the traced install-at-5-u/s rule, player.js gameFrame), with h_st playing
  if (name === 'aim_e_zoom.jsonl' && (anim.installed || anim.table !== GAME.loco)) note('locoInstall', R.length - 1, anim.installed, false);
  const keys = Object.keys(mism);
  console.log(`${name}: turn ${turnErr.toFixed(3)} deg / ${turnN}, clips ${clipN}, face ${faceErr.toFixed(3)} / ${faceN} (+${facePress} press),` +
              ` far ${farErr.toFixed(3)} / ${rayN}, hit ${hitOk}/${hitN}` + (keys.length ? '' : ', all fields match'));
  for (const k of keys)
    console.log(`  ${k}: ${mism[k].length} frames differ (sim/game): ${mism[k].slice(0, 6).join(' ')}${mism[k].length > 6 ? ' ...' : ''}`);
  if (turnErr > 0.5) { console.log('  turn-to-aim error above 0.5 deg'); fails++; }
  if (faceErr > 1e-3) { console.log('  face point differs'); fails++; }
  if (farErr > 0.05) { console.log('  far point CAM+0xb0 differs'); fails++; }
  // the hit is a GUESS (collision.js segment() stands in for FUN_005449f0's probe class): reported, not asserted
  fails += keys.length ? 1 : 0;
}
// air with the weapon out (player state 9): the air branch keeps rotation mode 6 (it doesn't call
// FUN_0034e960), so intent() + rotate() must take the same step toward the face point as on foot.
// The traces can't show this (his facing has converged before aim_g's jump), so it is set up here.
{
  const pc = new PlayerController(stubCol, { pos: { x: 0, y: 0, z: 0 }, heading: 0 });
  pc.weapon = { P: { facePt: { x: Math.sin(0.7) * 100, y: -10, z: Math.cos(0.7) * 100 } }, isAiming: () => true };
  pc.P.state = 9; pc.P.dir = { x: 0, y: 0, z: 0 }; pc.C.dir = { x: 0, y: 0, z: 1 }; pc.C.spd = 0.5;
  pc.C.face = { x: 0, y: 0, z: 1 };
  pc.intent(false); pc.rotate();
  const t = fracStep(0.5, STEP);
  let fx = Math.sin(0.7) * t, fz = 1 + (Math.cos(0.7) - 1) * t; const want = Math.atan2(fx, fz);
  const e = Math.abs(pc.heading - want) * 180 / Math.PI;
  console.log(`air turn (state 9, gun out): ${e.toFixed(4)} deg from the mode-6 step`);
  if (e > 1e-3) { console.log('  air turn differs'); fails++; }
}
// FUN_003320c0 l.288: R1 draws on the ground, or in the air while P+0x2a8 bit 3 is clear (a draw
// in the air sets it: jump_core weaponSwitch). Traced: cam_j_draw_r1 / cam_j_draw_gate
// (test_cam_traces); set up here without the player too
{
  const w = new WeaponSystem(W, RET), pl = { P: { flags2a8: 8 } };
  w.input({ r1Pressed: true, r1Held: true, r3Pressed: false, circlePressed: false }, false, null, pl);
  const blocked = !w.P.drawn;
  w.input({ r1Pressed: true, r1Held: true, r3Pressed: false, circlePressed: false }, false, null);
  const air = w.P.drawn && w.cam.view === 2 && w.cam.a220 === 1 && (w.cam.flags & 0x1000000) !== 0;
  const g = new WeaponSystem(W, RET);
  g.input({ r1Pressed: true, r1Held: true, r3Pressed: false, circlePressed: false }, true, null);
  const ground = g.P.drawn && g.cam.view === 2 && g.cam.a220 === 0 && !(g.cam.flags & 0x1000000);
  console.log(`R1 draw: air with bit 3 ${blocked ? 'no draw' : 'DRAWS (wrong)'}, air without ${air ? 'draw + CAM+0x220 = 1' : 'WRONG'}, ground ${ground ? 'draw, flag clear' : 'WRONG'}`);
  if (!blocked || !air || !ground) fails++;
}
// the ladder mount (FUN_00576548(P, 0)) clears the gun-out jump flag only in the air
{
  const mk = () => { const w = new WeaponSystem(W, RET); w.camJump(true, true); return w; };
  const a = mk(), g = mk();
  a.ladderMount(false); g.ladderMount(true);
  const ok = !(a.cam.flags & 0x1000000) && (g.cam.flags & 0x1000000) !== 0;
  console.log(`ladder mount: flag 0x1000000 ${ok ? 'cleared in the air only' : 'WRONG'}`);
  if (!ok) fails++;
}
// the same through the page's wiring: player.js step enters the ladder action -> weaponHook.actionEnter
// -> aim.js makeClipControl actionEnter -> ladderMount(ctrl.onGround) (aim.js passes the ground bit)
{
  const run = ground => {
    const c = new PlayerController(stubCol, { pos: { x: 0, y: 0, z: 0 }, heading: 0 });
    const w = new WeaponSystem(W, RET); w.camJump(true, true);
    const ctl = makeClipControl({ core: w, anim: headlessAnim(), ctrl: c, motions: MOTIONS, step: STEP });
    c.weaponHook = { actionEnter: a => ctl.actionEnter(a) };
    c.onGround = ground;
    let took = 0;
    c.addAction({ name: 'ladder', canEnter: () => !took++, enter: () => null,
                  update: () => ({ delta: { x: 0, y: 0, z: 0 } }), exit: () => {} });
    c.step({ x: 0, z: 0, keys: new Set(), pressed: new Set() });
    return { entered: took > 0 && w.P.st === 0xe, flag: (w.cam.flags & 0x1000000) !== 0 };
  };
  const a = run(false), g = run(true);
  const ok = a.entered && g.entered && !a.flag && g.flag;
  console.log(`ladder mount via aim.js: ${ok ? 'air clears the flag, ground keeps it' : `WRONG (air ${JSON.stringify(a)}, ground ${JSON.stringify(g)})`}`);
  if (!ok) fails++;
}
// LMB -> R1 gate with fake pointer-lock states: the lock-taking click and a drag-look never draw
{
  const g = mouseR1Gate(), bad = [];
  const ck = (what, got, want) => { if (got !== want) bad.push(`${what}: ${got} (want ${want})`); };
  ck('lock click down', g.down({ locked: false, refused: false, x: 0, y: 0 }), 'ignore');
  ck('lock click up', g.up({ locked: true, refused: false, x: 0, y: 0, onCanvas: true }), null);
  ck('locked press', g.down({ locked: true, refused: false, x: 0, y: 0 }), 'press');
  ck('locked release', g.up({ locked: true, refused: false, x: 0, y: 0, onCanvas: true }), null);
  ck('no-lock tap down', g.down({ locked: false, refused: true, x: 10, y: 10 }), 'pending');
  ck('no-lock tap up', g.up({ locked: false, refused: true, x: 11, y: 10, onCanvas: true }), 'tap');
  g.down({ locked: false, refused: true, x: 10, y: 10 });
  ck('drag-look up', g.up({ locked: false, refused: true, x: 60, y: 10, onCanvas: true }), null);
  g.down({ locked: false, refused: true, x: 10, y: 10 });
  ck('tap released off canvas', g.up({ locked: false, refused: true, x: 10, y: 10, onCanvas: false }), null);
  console.log(`mouse R1 gate: ${bad.length ? bad.join('; ') : 'lock click, drag-look and taps as specified'}`);
  if (bad.length) fails++;
}
process.exit(fails ? 1 : 0);
