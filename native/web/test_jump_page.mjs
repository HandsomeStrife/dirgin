// Replays the PCSX2 jump traces (tools/trace/out, game-derived, gitignored) through the page's
// own path: PlayerController.step (player.js, with the jump wired in: ✕ edge -> jump_core
// takeOff / motionUpdate / freeze) and SpeedAnimator.gameFrame (the jump clock as the head
// layer, the hand-back to the locomotion choice). Unlike test_jump.mjs there is no position
// re-sync on the ✕ frame, so the run-up's ground drift (player.js, 2.2e-3 in e_jumps) is in
// the numbers. Per trace it prints the per-frame errors and fails on:
//   state / ground bit / head clip / head clip frame mismatches (every frame),
//   position error during the jump (✕ frame to the hand-back) > POS_TOL,
//   blend weight MP+0x408 error > 1e-3 on frames where the game's head is a jump clip or the
//   hand-back blend.
//   node --import ./native/web/test_hooks.mjs native/web/test_jump_page.mjs [player.js]
import { readFileSync, existsSync } from 'node:fs';
import { pathToFileURL, fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const PL = await import(pathToFileURL(path.resolve(process.argv[2] ?? path.join(here, 'player.js'))).href);
const { CLIP_FRAMES, SLOT, UNLOADED, canJump, AirFireMotion } = await import(pathToFileURL(path.join(here, 'jump_core.js')).href);
const { WeaponSystem } = await import(pathToFileURL(path.join(here, 'aim_core.js')).href);
const { Collision } = await import(pathToFileURL(path.join(here, 'collision.js')).href);
const { PlayerController, SpeedAnimator, padAxis, stickToWorld } = PL;

const buf = readFileSync(path.join(here, 'data/z128_collision.bin'));
const col = new Collision(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
const OUT = process.env.TRACE_OUT ?? path.join(root, 'tools/trace/out');
const CROSS = 64;                                     // pad_in.buttons bit for ✕
const POS_TOL = 5e-3;                                 // no re-sync: run-up drift (2.2e-3) + the air railing slide (2.7e-3, test_jump.mjs)
const FRAME_TOL = 1e-3;

let fail = 0, ran = 0;
const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) fail++; };
const load = p => readFileSync(p, 'utf8').trim().split('\n').map(l => JSON.parse(l));
const headOf = d => { const h = Buffer.from(d.mp_head, 'hex'); return d.mp_layers[h[8] & 7]; };
// the blend-out layer FUN_005aa5b8 poses under the head (MP+0x44a = head index - 1) while the head's
// countdown +0x24 is running
const outOf = d => { const h = Buffer.from(d.mp_head, 'hex'); return d.mp_layers[(h[8] - 1) & 7]; };
const stateOf = d => Buffer.from(d.p_state, 'hex')[4];
const groundOf = d => (d.mflags >> 9) & 1;
const KEY = { h_st: 'idle', h_wk: 'walk', h_rn: 'run' }, NAME = { idle: 'h_st', walk: 'h_wk', run: 'h_rn' };
const JUMPISH = /^h_jp|^c_wjp|^c_bj/;

// the animator's game-clock half without three.js (as the locomotion replay does)
function stubAnim(h0) {
  const anim = Object.create(SpeedAnimator.prototype);
  const frames = { idle: 160, walk: 40, run: 20, ...CLIP_FRAMES };
  const actions = Object.fromEntries(Object.keys(frames).map(k => [k, {}]));
  Object.assign(anim, { frames, head: { key: KEY[h0.clip] ?? h0.clip, frame: h0.frame, prevFrame: h0.frame },
                        out: null, blend: { len: 0, rem: 0, w: 1 }, installed: false, cur: -1, override: null,
                        rate: 0.5, current: KEY[h0.clip], actions, names: { idle: 'idle', walk: 'walk', run: 'run' },
                        library: new Map(), table: null });
  return anim;
}

function run(name, { state = true, clips = true, dir = OUT } = {}) {
  const file = path.join(dir, name + '.jsonl');
  if (!existsSync(file)) { console.log(`skip ${name}: no trace (${file})`); return; }
  ran++;
  const [, ...R] = load(file);
  const r0 = R[0], p0 = r0.matrix[3], f0 = r0.matrix[2];
  const ctrl = new PlayerController(col, { pos: { x: p0[0], y: p0[1], z: p0[2] }, heading: Math.atan2(f0[0], f0[2]) });
  ctrl.C.face = { x: f0[0], y: f0[1], z: f0[2] };
  ctrl.P.state = stateOf(r0);
  const anim = stubAnim(headOf(r0));
  ctrl.attach({ anim });
  let prevButtons = 0, inJump = false;
  const S = { pos: 0, posF: null, posAll: 0, st: 0, stF: null, gnd: 0, gndF: null, clip: 0, clipF: null, fr: 0, frF: null, w: 0, wF: null, wn: 0, fc: 0, fcF: null, b: 0, bF: null, bn: 0, o: 0, oF: null, on: 0, n: 0 };
  for (let i = 0; i + 1 < R.length; i++) {
    const d = R[i], pad = d.pad_in;
    // aim traces log no camera matrix: only usable while the stick is centred (aim_g)
    const m = R[i + 1].cam_matrix ?? (pad.lx === 128 && pad.ly === 128 ? [[1, 0, 0], [0, 1, 0], [0, 0, 1]] : null);
    if (!m) throw new Error(`${name}: frame ${d.frame}: stick moved and no camera matrix`);
    const sx = padAxis(pad.lx - 128, pad.ly - 128), sy = padAxis(pad.ly - 128, pad.lx - 128);
    const w = stickToWorld(sx, sy, m[0], m[2]);
    const cross = (pad.buttons & CROSS) !== 0 && (prevButtons & CROSS) === 0;
    prevButtons = pad.buttons;
    ctrl.step({ x: w.x, z: w.z, stick: w.mag, run: false, keys: new Set(), pressed: new Set(), cross });
    const g = R[i + 1], gp = g.matrix[3], gh = headOf(g);
    S.n++;
    if (ctrl.jump.head) inJump = true;
    else if (inJump && !JUMPISH.test(gh.clip)) inJump = false;
    const e = Math.hypot(ctrl.pos.x - gp[0], ctrl.pos.y - gp[1], ctrl.pos.z - gp[2]);
    S.posAll = Math.max(S.posAll, e);
    const gf = g.matrix[2], fe = Math.hypot(ctrl.C.face.x - gf[0], ctrl.C.face.z - gf[2]);
    if (inJump && fe > S.fc) { S.fc = fe; S.fcF = g.frame; }
    if (inJump && e > S.pos) { S.pos = e; S.posF = g.frame; }
    if (state && ctrl.P.state !== stateOf(g)) { S.st++; S.stF ??= `${g.frame} ${ctrl.P.state} vs ${stateOf(g)}`; }
    if ((ctrl.onGround ? 1 : 0) !== groundOf(g)) { S.gnd++; S.gndF ??= g.frame; }
    const our = NAME[anim.head.key] ?? anim.head.key;
    if (our !== gh.clip) { S.clip++; S.clipF ??= `${g.frame} ${our} vs ${gh.clip}`; }
    else if (Math.abs(anim.head.frame - gh.frame) > FRAME_TOL) { S.fr++; S.frF ??= `${g.frame} ${our} ${anim.head.frame.toFixed(2)} vs ${gh.frame.toFixed(2)}`; }
    if (process.env.JP_DUMP === name) console.log(g.frame, ctrl.P.state, stateOf(g), ctrl.onGround ? 1 : 0, groundOf(g), e.toExponential(1), our, anim.head.frame.toFixed(2), gh.clip, gh.frame.toFixed(2), anim.blend.w.toFixed(3));
    // the head layer's blend countdown (layer +0x24; 0 = no blend) on jump / hand-back frames
    if (JUMPISH.test(gh.clip) || (anim.out && JUMPISH.test(anim.out.key))) {
      S.bn++;
      const rem = anim.out ? anim.blend.rem : 0;
      if (Math.abs(rem - gh.blend_len) > 1e-3) { S.b++; S.bF ??= `${g.frame} ${rem.toFixed(2)} vs ${gh.blend_len.toFixed(2)}`; }
    }
    // what the pose blends FROM: the blend-out layer's clip and frame (Dan's standing-jump flicker:
    // the page looped h_jp4 back to its frame 0 under the h_st blend; the game holds 14.0)
    if (gh.blend_len > 0) {
      S.on++;
      const go = outOf(g), oo = anim.out;
      const oc = oo ? (NAME[oo.key] ?? oo.key) : null;
      if (oc !== go.clip || Math.abs(oo.frame - go.frame) > FRAME_TOL) { S.o++; S.oF ??= `${g.frame} ${oc} ${oo?.frame.toFixed(2)} vs ${go.clip} ${go.frame.toFixed(2)}`; }
    }
    if (!g.mp_rate) continue;                       // jump field set: no MP+0x408
    const gw = Buffer.from(g.mp_rate, 'hex').readFloatLE(8), ow = anim.out ? anim.blend.w : 1;
    if ((JUMPISH.test(gh.clip) || (anim.out && JUMPISH.test(anim.out.key))) && Math.abs(ow - gw) > 1e-3 && S.wF === null) S.wF = `${g.frame} ${ow.toFixed(3)} vs ${gw.toFixed(3)}`;
    if (JUMPISH.test(gh.clip) || (anim.out && JUMPISH.test(anim.out.key))) { S.wn++; S.w = Math.max(S.w, Math.abs(ow - gw)); }
  }
  const L = `${name} (${S.n} frames)`;
  ok(S.pos <= POS_TOL, `${L}: position in the jump max ${S.pos.toExponential(2)} (frame ${S.posF}); whole trace ${S.posAll.toExponential(2)}`);
  if (clips) ok(S.fc <= 1e-4, `${L}: facing in the jump max ${S.fc.toExponential(2)}${S.fcF !== null ? ' (frame ' + S.fcF + ')' : ''}`);
  if (state) ok(S.st === 0, `${L}: player state off ${S.st}${S.stF ? ', first ' + S.stF : ''}`);
  ok(S.gnd === 0, `${L}: ground bit off ${S.gnd}${S.gndF !== null ? ', first ' + S.gndF : ''}`);
  if (clips) ok(S.clip === 0, `${L}: head clip off ${S.clip}${S.clipF ? ', first ' + S.clipF : ''}`);
  if (clips) ok(S.fr === 0, `${L}: head clip frame off ${S.fr}${S.frF ? ', first ' + S.frF : ''}`);
  if (clips) ok(S.b === 0, `${L}: head blend countdown off ${S.b} of ${S.bn}${S.bF ? ', first ' + S.bF : ''}`);
  if (clips) ok(S.o === 0, `${L}: blend-out layer clip / frame off ${S.o} of ${S.on}${S.oF ? ', first ' + S.oF : ''}`);
  if (S.wn && clips) ok(S.w <= 1e-3, `${L}: blend weight MP+0x408 (jump clips / hand-back, ${S.wn} frames) max err ${S.w.toExponential(2)}${S.wF ? ', first ' + S.wF : ''}`);
  else if (clips) console.log(`--   ${L}: no MP+0x408 in this field set`);
}

for (const n of ['e_jumps', 'i_air_release', 'j_stand', 'j_run', 'j_steer', 'j_double', 'j_back']) run(n);
// aim_g_air without a weapon: flight and ground bit only (the game's on-foot state is 3 there)
run('aim_g_air', { state: false, clips: false, dir: path.join(OUT, 'aim') });

// aim_g_air with the gun out, through the page's step with the real weapon translation (aim_core
// WeaponSystem: FUN_00579570 decides the shots; its camera is fed from the trace as test_aim.mjs
// does). The game: h_jp0_a (f41, blend 10) -> h_jp1_a (f45) -> R1 f48 = a shot in the air ->
// air-shot class 0x4e: h_jp3_a with blend 1 (h_jp2_a never plays) -> fall class 0x22 (no restart)
// -> landing h_jp4_a (cut) -> h_st_a. Checked on every frame where either head is a jump clip:
// head clip, frame, blend countdown (+0x24), and the blend-out layer's clip / frame.
// node ... test_jump_page.mjs [player.js] [jump_core.js]: a mutant jump_core can't be swapped in
// (player.js imports it), so the mutation runs edit the file in place (scratchpad mutate script).
function runArmed(name = 'aim_g_air', order = 'h_jp0_a h_jp1_a h_jp3_a h_jp4_a', after = null) {
  const file = path.join(OUT, 'aim', name + '.jsonl');
  if (!existsSync(file)) { console.log(`skip armed ${name}: no trace`); return; }
  ran++;
  const J = p => JSON.parse(readFileSync(path.join(here, p), 'utf8'));
  const [, ...R] = load(file);
  const hex = h => Buffer.from(h, 'hex'), F = (h, o) => hex(h).readFloatLE(o), U = (h, o) => hex(h).readUInt32LE(o);
  const V = (h, o) => ({ x: F(h, o), y: F(h, o + 4), z: F(h, o + 8) });
  const r0 = R[0], p0 = r0.matrix[3], f0 = r0.matrix[2];
  const ctrl = new PlayerController(col, { pos: { x: p0[0], y: p0[1], z: p0[2] }, heading: Math.atan2(f0[0], f0[2]) });
  ctrl.C.face = { x: f0[0], y: f0[1], z: f0[2] };
  ctrl.P.state = stateOf(r0);
  const anim = stubAnim(headOf(r0));
  ctrl.attach({ anim });
  const sim = new WeaponSystem(J('data/aim/weapon.json'), J('data/aim/reticle.json'));
  sim.P.acc = F(r0.p_weapon, 4); sim.P.st = U(r0.p_weapon, 0x14); sim.P.T = F(r0.p_weapon, 0);
  sim.hud.mode = U(r0.hud, 0); sim.r.kind = hex(r0.ret)[0];
  let i = 0, shots = 0;
  const btn = m => R[i].pad_in.buttons & m, prevBtn = m => (i ? R[i - 1].pad_in.buttons : 0) & m;
  ctrl.weapon = sim;
  ctrl.weaponHook = {
    pre() {
      const r = R[i], nx = R[i + 1], st = U(r.p_weapon, 0x14), nst = U(nx.p_weapon, 0x14);
      const shot = (nst === 2 && st !== 2 && st !== 1) || (st === 1 && nst === 2);
      const k198u = Math.fround(Math.fround(Math.fround(Math.fround(sim.cam.k19c - sim.cam.k198) * 0.3) * 1.1999999284744263) + sim.cam.k198);
      sim.switchAngles();
      sim.cameraFrame(F(nx.cam_170, 4) - (shot && !(sim.W.flags & 0x10) ? k198u : 0));
      sim.kickDecay();
      sim.eye = V(nx.cam_60, 0x30); sim.hit = V(nx.cam_60, 0x40);
      sim.input({ r1Pressed: !!(btn(8) && !prevBtn(8)), r1Held: !!btn(8), r3Pressed: !!(btn(0x400) && !prevBtn(0x400)),
                  circlePressed: !!(btn(0x20) && !prevBtn(0x20)) }, (r.mflags & 0x200) !== 0, V(nx.cam_60, 0x50), ctrl);
    },
    weapon() { sim.stateMachine(V(R[i + 1].cam_60, 0x40)); sim.reticleFrame(20); const s = sim.events.some(e => e.type === 'shot'); if (s) shots++; return s; },
    post() { sim.events.length = 0; },
  };
  let prevButtons = 0;
  const S = { clip: 0, clipF: null, fr: 0, frF: null, b: 0, bF: null, o: 0, oF: null, n: 0, pos: 0 };
  const seen = [];
  for (i = 0; i + 1 < R.length; i++) {
    const d = R[i], pad = d.pad_in;
    const cross = (pad.buttons & CROSS) !== 0 && (prevButtons & CROSS) === 0;
    prevButtons = pad.buttons;
    ctrl.step({ x: 0, z: 0, stick: 0, run: false, keys: new Set(), pressed: new Set(), cross });
    const g = R[i + 1], gh = headOf(g), gp = g.matrix[3];
    const our = NAME[anim.head.key] ?? anim.head.key;
    if (!JUMPISH.test(gh.clip) && !JUMPISH.test(our)) continue;
    S.n++;
    if (seen[seen.length - 1] !== gh.clip) seen.push(gh.clip);
    S.pos = Math.max(S.pos, Math.hypot(ctrl.pos.x - gp[0], ctrl.pos.y - gp[1], ctrl.pos.z - gp[2]));
    if (our !== gh.clip) { S.clip++; S.clipF ??= `${g.frame} ${our} vs ${gh.clip}`; }
    else if (Math.abs(anim.head.frame - gh.frame) > FRAME_TOL) { S.fr++; S.frF ??= `${g.frame} ${our} ${anim.head.frame.toFixed(2)} vs ${gh.frame.toFixed(2)}`; }
    const rem = anim.out ? anim.blend.rem : 0;
    if (Math.abs(rem - gh.blend_len) > 1e-3) { S.b++; S.bF ??= `${g.frame} ${rem.toFixed(2)} vs ${gh.blend_len.toFixed(2)}`; }
    // (the blend-out under h_jp0_a is the aimed idle h_st_a, which aim.js's clip classes play: not
    // run in this replay, so only jump-clip blend-out layers are compared)
    if (gh.blend_len > 0 && JUMPISH.test(outOf(g).clip)) {
      const go = outOf(g), oo = anim.out, oc = oo ? (NAME[oo.key] ?? oo.key) : null;
      if (oc !== go.clip || Math.abs(oo.frame - go.frame) > FRAME_TOL) { S.o++; S.oF ??= `${g.frame} ${oc} ${oo?.frame.toFixed(2)} vs ${go.clip} ${go.frame.toFixed(2)}`; }
    }
    if (process.env.JP_DUMP === 'armed') console.log(g.frame, our, anim.head.frame.toFixed(2), rem.toFixed(2), '|', gh.clip, gh.frame.toFixed(2), gh.blend_len.toFixed(2));
  }
  const L = `armed ${name} (${S.n} jump-clip frames)`;
  ok(seen.join(' ') === order, `${L}: game clip order ${seen.join(' ')}`);
  ok(S.clip === 0, `${L}: head clip off ${S.clip}${S.clipF ? ', first ' + S.clipF : ''}`);
  ok(S.fr === 0, `${L}: head clip frame off ${S.fr}${S.frF ? ', first ' + S.frF : ''}`);
  ok(S.b === 0, `${L}: head blend countdown off ${S.b}${S.bF ? ', first ' + S.bF : ''}`);
  ok(S.o === 0, `${L}: blend-out layer clip / frame off ${S.o}${S.oF ? ', first ' + S.oF : ''}`);
  ok(S.pos <= POS_TOL, `${L}: position max ${S.pos.toExponential(2)}`);
  if (after) return after(ctrl, L, shots);
  const air = ctrl.jump.events.filter(e => e.ev === 'airShot').map(e => e.frame);
  ok(shots >= 1 && air.length === 1 && air[0] === 48, `${L}: shots ${shots}, air-shot class entered on frames [${air}] (game: R1 f48 only; f60 fires nothing)`);
}
runArmed();
// worker 0109: R1 in the h_jp0 wind-up (aim_j_draw_windup: ✕ f20, R1 f22, state 8, P+0x100 bits 46-47
// = 1, ground bit still set). FUN_005766c0 takes the air branch with no fall restart (the ground bit
// is set): state 9 (FUN_0055b8e0(P, 9, 1, 0)), whose forced motion 0x22 ends the wind-up: the
// rise starts at once and h_jp3_a plays (jump_core forceFall). FUN_00576548 sees the ground bit:
// no camera jump flag.
// R1 / R3 while falling without a take-off (a walk-off or a ladder drop: P+0x100 bits 46-47 = 0; no
// such ledge is reachable from the z128 save state, so this is set up by hand): FUN_005766c0 skips the
// fall restart (LAB_005767bc) and still goes to state 9 (ground bit clear). After a take-off
// (bits 1) the same draw restarts the fall with DAT_0046ee88 = 1.0. (The landing clear is checked per
// frame against the game in test_cam_traces 'page'.)
{
  const J0 = p => JSON.parse(readFileSync(path.join(here, p), 'utf8'));
  const r0 = load(path.join(OUT, 'j_stand.jsonl'))[1], p0 = r0.matrix[3];
  const fall = (bits, inp) => {
    const c = new PL.PlayerController(col, { pos: { x: p0[0], y: p0[1], z: p0[2] }, heading: 0 });
    c.onGround = false; c.P.state = 9; c.P.jumpBits = bits;
    const hs = []; const sf = c.startFall.bind(c); c.startFall = h => { hs.push(h); return sf(h); };
    const w = new WeaponSystem(J0('data/aim/weapon.json'), J0('data/aim/reticle.json'));
    w.input({ r1Pressed: false, r1Held: false, r3Pressed: false, circlePressed: false, ...inp }, false, null, c);
    return { hs, st: c.P.state, wst: w.P.state, b3: ((c.P.flags2a8 ?? 0) & 8) !== 0, bits: c.P.jumpBits, drawn: w.P.drawn, c };
  };
  for (const [k, inp] of [['R1', { r1Pressed: true, r1Held: true }], ['R3', { r3Pressed: true }]]) {
    const off = fall(0, inp), jumped = fall(1, inp);
    ok(off.drawn && off.hs.length === 0 && off.st === 9 && off.wst === 9 && off.b3 && off.bits === 0,
       `${k} falling with no take-off: drawn, no fall restart (${off.hs}), state ${off.st} / returned ${off.wst}, P+0x2a8 bit 3 ${off.b3}`);
    ok(jumped.drawn && jumped.hs.length === 1 && jumped.hs[0] === 1 && jumped.st === 9 && jumped.bits === 1,
       `${k} falling after a take-off: fall restarted with h ${jumped.hs} (want 1), state ${jumped.st}`);
  }
}
runArmed('aim_j_draw_windup', 'h_jp0 h_jp3_a h_jp4_a', (ctrl, L) => {
  const ev = ctrl.jump.events.filter(e => e.ev === 'forceFall').map(e => e.frame);
  ok(ev.length === 1 && ev[0] === 22, `${L}: fall class forced on frames [${ev}] (game: R1 f22)`);
  ok((ctrl.weapon.cam.flags & 0x1000000) === 0, `${L}: camera jump flag stays clear (ground bit set at the draw)`);
});
// ✕ priority (FUN_00333150): an action that takes the press (the ladder) or owns him keeps it;
// otherwise the page's Space edge (input.pressed) is the jump
{
  const mk = () => {
    const r0 = load(path.join(OUT, 'j_stand.jsonl'))[1], p0 = r0.matrix[3];
    return new PlayerController(col, { pos: { x: p0[0], y: p0[1], z: p0[2] }, heading: 0 });
  };
  const idle = { x: 0, z: 0, keys: new Set() };
  let c = mk();
  c.step({ ...idle, pressed: new Set(['Space']) });
  ok(c.jump.active && c.P.state === 8 && c.jump.frozen, `Space edge on foot: jump (state ${c.P.state}, frozen ${c.jump.frozen})`);
  c = mk();
  let took = 0;
  c.addAction({ name: 'ladder', canEnter: ctx => ctx.input.pressed?.has('Space') && !took++, enter: () => null,
                update: () => ({ delta: { x: 0, y: 0, z: 0 } }), exit: () => {} });
  c.step({ ...idle, pressed: new Set(['Space']) });
  ok(!c.jump.active && c.P.state === 2, `Space taken by the ladder: no jump (state ${c.P.state})`);
  c.step({ ...idle, pressed: new Set(['Space']) });
  ok(!c.jump.active, 'Space while an action owns him: no jump');
  // a run-up then ✕: no move and no turn while h_jp0 plays (C+0x1c0 bit 0x20), then the flight
  c = mk();
  for (let i = 0; i < 30; i++) c.step({ x: 1, z: 0, stick: 1, keys: new Set(), pressed: new Set() });
  const p1 = { ...c.pos }, h1 = c.heading;
  c.step({ x: 0, z: 1, stick: 1, keys: new Set(), pressed: new Set(), cross: true });
  ok(c.jump.frozen && c.pos.x === p1.x && c.pos.z === p1.z && c.heading === h1, `wind-up: frozen, no move / turn (moved ${Math.hypot(c.pos.x - p1.x, c.pos.z - p1.z).toExponential(1)})`);
}
// the armed choice (player.js step): C+0x590 bit 24 = the gun is out. A weapon that is attached
// but holstered plays the plain h_jp0; drawn plays h_jp0_a (FUN_00598f50 phase 0)
for (const drawn of [false, true]) {
  const r0 = load(path.join(OUT, 'j_stand.jsonl'))[1], p0 = r0.matrix[3];
  const c = new PlayerController(col, { pos: { x: p0[0], y: p0[1], z: p0[2] }, heading: 0 });
  c.weapon = { drawn: () => drawn, isAiming: () => false, P: { facePt: null }, camJump() {}, enterFoot() {} };
  c.weaponHook = { pre() {}, weapon: () => false, post() {} };
  c.step({ x: 0, z: 0, keys: new Set(), pressed: new Set(), cross: true });
  const want = drawn ? 'h_jp0_a' : 'h_jp0';
  ok(c.jump.clock && c.jump.clock.clip === want, `armed choice, gun ${drawn ? 'drawn' : 'holstered'}: ${c.jump.clock?.clip} (want ${want})`);
}
// the armed timeline with no shot (decomp only; no trace has it): FUN_00598f50 phases 4-7 play
// h_jp0_a -> h_jp1_a -> h_jp2_a -> h_jp3_a (flags 0x83: looped) -> landing h_jp4_a. Then a shot
// while h_jp3_a plays: FUN_00596830's h_jp3_a request (flags 0x83) is dropped (FUN_00344608: same
// level clip and partner), so the clip runs on, and the fall class follows.
{
  const r0 = load(path.join(OUT, 'j_stand.jsonl'))[1], p0 = r0.matrix[3];
  const c = new PlayerController(col, { pos: { x: p0[0], y: p0[1], z: p0[2] }, heading: 0 });
  let fire = false;
  c.weapon = { drawn: () => true, isAiming: () => false, P: { facePt: null }, camJump() {}, enterFoot() {} };
  c.weaponHook = { pre() {}, weapon: () => { const f = fire; fire = false; return f; }, post() {} };
  const seq = [];
  let loop3 = null, shotAt = null, after = null;
  for (let i = 0; i < 70; i++) {
    if (c.jump.clock && c.jump.clock.clip === 'h_jp3_a' && c.jump.clock.frame >= 3 && shotAt === null && !c.onGround) {
      fire = true; shotAt = c.jump.clock.frame;
    }
    c.step({ x: 0, z: 0, keys: new Set(), pressed: new Set(), cross: i === 0 });
    const k = c.jump.clock && c.jump.clock.clip;
    if (k && seq[seq.length - 1] !== k) seq.push(k);
    if (k === 'h_jp3_a' && loop3 === null) loop3 = c.jump.clock.loop;
    if (shotAt !== null && after === null) after = { clip: k, frame: c.jump.clock.frame, motion: c.jump.motion?.id };
  }
  ok(seq.join(' ') === 'h_jp0_a h_jp1_a h_jp2_a h_jp3_a h_jp4_a', `armed, no shot: ${seq.join(' ')}`);
  ok(loop3 === true, `armed, no shot: h_jp3_a looped (${loop3})`);
  ok(after && after.clip === 'h_jp3_a' && Math.abs(after.frame - (shotAt + 0.6)) < 1e-4 && after.motion === 0x22 && c.jump.events.some(e => e.ev === 'airShot'),
     `shot during h_jp3_a: not restarted (${after?.clip} ${after?.frame?.toFixed(2)} after ${shotAt?.toFixed(2)}, motion 0x${after?.motion?.toString(16)})`);
}
// the armed back jump (decomp only; no trace has it): FUN_005a1510 phases 4-7 play c_bj0 ->
// c_bj1 -> c_bj2 (FUN_003456c0, flags 2), then the h_jp3_a set through FUN_00345818 with flags
// 0x83 (now, looped; lines ~201 / ~223), then the landing h_jp4_a. Run-up, then ✕ with the stick
// pulled back (more than 90 deg from the facing: FUN_00575768 -> state 0x20).
{
  const r0 = load(path.join(OUT, 'j_stand.jsonl'))[1], p0 = r0.matrix[3];
  const c = new PlayerController(col, { pos: { x: p0[0], y: p0[1], z: p0[2] }, heading: 0 });
  c.weapon = { drawn: () => true, isAiming: () => false, P: { facePt: null }, camJump() {}, enterFoot() {} };
  c.weaponHook = { pre() {}, weapon: () => false, post() {} };
  for (let i = 0; i < 30; i++) c.step({ x: 1, z: 0, stick: 1, keys: new Set(), pressed: new Set() });
  const seq = []; let motion = null, loop3 = null, req3 = null;
  for (let i = 0; i < 90; i++) {
    // ✕ one frame after the stick is pulled back: the take-off reads last frame's move (C+0x2d0)
    c.step({ x: -1, z: 0, stick: 1, keys: new Set(), pressed: new Set(), cross: i === 1 });
    if (i === 1) motion = c.jump.motion?.id;
    const k = c.jump.clock && c.jump.clock.clip;
    if (k && seq[seq.length - 1] !== k) {
      seq.push(k);
      if (k === 'h_jp3_a') { loop3 = c.jump.clock.loop; req3 = c.jump.clock.frame; }
    }
  }
  ok(motion === 0x59, `armed back jump: motion 0x${motion?.toString(16)} (want 0x59, FUN_005a1510)`);
  ok(seq.slice(0, 5).join(' ') === 'c_bj0 c_bj1 c_bj2 h_jp3_a h_jp4_a', `armed back jump: ${seq.join(' ')}`);
  ok(loop3 === true, `armed back jump: h_jp3_a looped (${loop3}; flags 0x83)`);
  ok(req3 !== null && req3 < 0.61, `armed back jump: h_jp3_a played now (first seen at frame ${req3})`);
}
// FUN_00596830 with slot 0xc1 (h_jp_f) loaded: its branch isn't ported, so the page leaves it
// out (plays the h_jp3_a set, warns once) and the frame loop keeps running
{
  const warn = console.warn; let warned = 0; console.warn = () => { warned++; };
  let got = null, threw = null;
  try {
    for (let k = 0; k < 2; k++) {
      const m = new AirFireMotion();
      got = m.update({ usable: () => true, headClip: 'h_jp2_a', headBlending: false });
    }
  } catch (e) { threw = e; } finally { console.warn = warn; }
  ok(!threw && got && got.length === 1 && got[0].aimed && got[0].loop && got[0].now && warned === 1,
     `air shot, slot 0xc1 loaded: no throw (${threw?.message ?? 'ok'}), h_jp3_a set requested, ${warned} warning(s)`);
}
// the gate branches no trace reaches (FUN_00577230 P+0x118 bit 0x40000; FUN_0055bb40 / FUN_0055bb78 states)
{
  const c = { P: { state: 2 }, C: {}, onGround: true };
  ok(canJump(c, 3, { pressed: true }), 'gate: open on foot with the save-state flags');
  ok(!canJump(c, 3, { pressed: true, p118: 0x20209 | 0x40000 }), 'gate: P+0x118 bit 0x40000 closes it');
  for (const st of [0x1a, 0x80, 0xbf, 0x13, 0x14, 0x1d, 0xc0]) { c.P.state = st; ok(!canJump(c, 3, { pressed: true }), `gate: state 0x${st.toString(16)} closed`); }
  c.P.state = 2;
  ok(!canJump(c, 3, { pressed: true, inputLock: 2 }) && !canJump(c, 3, { pressed: true, weaponBlock: true }), 'gate: input lock / weapon block close it');
}
ok(ran > 0, `traces replayed: ${ran}`);
// the page's frame counts for the pack clips (SpeedAnimator.ensure via addClips + the pack's
// `clips` table): a one-shot's glb duration is (n - 1)/60, the hold / wrap need the game's n
{
  const side = path.join(here, 'data/vincent/vincent_jump.json');
  if (!existsSync(side)) console.log('skip pack frame counts: no data/vincent/vincent_jump.json');
  else {
    const meta = JSON.parse(readFileSync(side, 'utf8')).clips;
    const anim = Object.create(SpeedAnimator.prototype);
    Object.assign(anim, { mixer: { clipAction: () => ({ setLoop() {} }) }, actions: {}, names: {}, frames: {}, library: new Map() });
    // the glb's durations: keys 0..n-1 for a one-shot (export_web_vincent.py keys_note)
    anim.addClips(Object.entries(meta).map(([name, c]) => ({ name, duration: c.duration_s })), meta);
    // every clip the jump motion states can request (SLOT, minus the slots empty in z128) is in
    // the pack with the frame count jump_core's clock uses
    const want = [...new Set(Object.entries(SLOT).filter(([s]) => !UNLOADED.has(+s)).map(([, n]) => n))];
    for (const k of want) {
      const has = anim.ensure(k);
      ok(has && anim.frames[k] === CLIP_FRAMES[k], `pack clip ${k}: ${has ? anim.frames[k] + ' frames' : 'missing'} (jump_core ${CLIP_FRAMES[k]})`);
    }
    // loop flags: h_jp3 / h_jp3_a loop (played with flag 1), the rest are one-shots
    for (const k of want) ok(meta[k] && meta[k].loop === /^h_jp3/.test(k), `pack clip ${k}: loop ${meta[k]?.loop}`);
  }
}
process.exit(fail ? 1 : 0);
