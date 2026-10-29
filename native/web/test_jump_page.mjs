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
const { CLIP_FRAMES, canJump } = await import(pathToFileURL(path.join(here, 'jump_core.js')).href);
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
// aim_g_air: a standing jump with the gun out (no weapon in this replay; the page plays the plain
// h_jp set for the armed h_jp*_a sets, see player.js step): flight and ground bit only (facing: the aim turn; the game's
// on-foot state is 3 there, the weapon's)
run('aim_g_air', { state: false, clips: false, dir: path.join(OUT, 'aim') });
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
    for (const k of ['h_jp0', 'h_jp1', 'h_jp2', 'h_jp3', 'h_jp4']) {
      anim.ensure(k);
      ok(anim.frames[k] === CLIP_FRAMES[k], `pack frame count ${k}: ${anim.frames[k]} (game ${CLIP_FRAMES[k]})`);
    }
  }
}
process.exit(fail ? 1 : 0);
