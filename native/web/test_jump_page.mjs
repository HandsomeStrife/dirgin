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

// from: start the replay at this record's frame (a standstill inside the trace; the page starts
// at rest, and the trace logs no P+0x299 stick ramp to seed a run)
function run(name, { state = true, clips = true, dir = OUT, from = null } = {}) {
  const file = path.join(dir, name + '.jsonl');
  if (!existsSync(file)) { console.log(`skip ${name}: no trace (${file})`); return; }
  ran++;
  let [, ...R] = load(file);
  if (from !== null) R = R.filter(d => d.frame >= from);
  const r0 = R[0], p0 = r0.matrix[3], f0 = r0.matrix[2];
  const ctrl = new PlayerController(col, { pos: { x: p0[0], y: p0[1], z: p0[2] }, heading: Math.atan2(f0[0], f0[2]) });
  ctrl.C.face = { x: f0[0], y: f0[1], z: f0[2] };
  ctrl.P.state = stateOf(r0);
  const anim = stubAnim(headOf(r0));
  if (from !== null && headOf(r0).blend_len > 0) {
    // started inside a blend: seed the blend-out layer and the head's countdown (+0x24) from the record
    const h = headOf(r0), o = outOf(r0);
    anim.out = { key: KEY[o.clip] ?? o.clip, frame: o.frame, prevFrame: o.frame, loop: (o.lflags & 1) !== 0 };
    anim.blend = { len: h.blend_frames, rem: h.blend_len, w: (h.blend_frames - h.blend_len) / h.blend_frames };
  }
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
// Dan's ladder jump (worker 0129's trace; script tools/trace/scripts/l_ladder_jump.json): a running
// jump into the ladder's 0x10 drop polygon from outside the trigger ray. The game doesn't mount:
// the air walk (collision.js body.air) stops him at x -1109.13 on the ramp, he slides back down,
// lands in body state 3 (FUN_00357860(body, 3)), the state-3 walk crosses onto the floor
// polygon (state 0, body+0x40 = 0: f558's air control starts from 0) and the run-up resumes.
// From f480: he stood still at f479 (the logged route starts mid-run at f440).
run('l_ladder_jump', { from: 480 });
// Worker 0131 (Dan: "jumping onto the ladder should start the climb"): the game never mounts from a
// jump. The mount is reached only from the ✕ press edge in FUN_00333150 through the gate
// FUN_005771c8, which needs P+0x118 bit 0x200; the jump start clears it (0x20209 -> 0x20009) and
// the landing state's enter (FUN_005382e0 / FUN_00538218 / FUN_005383a8) sets it again.
// _a: running ✕ from x -1113.75 (inside the 7-unit ray) jumps; lands at the ladder foot, no climb.
// _b: ✕ again mid-air at the ladder face (f535) = the second jump, state 0x1f c_wjp0, no climb.
// _c: ✕ on the drop-polygon slide frame (f556) = the second jump too (still state 8), no climb.
// _d: standing ✕ at the ladder foot (f500) mounts: state 0xb (reference; the climb is ladder.js's).
// _e: _a's run-up with ✕ one frame later (f521) mounts mid-run: the trigger chosen from f520's pose.
for (const n of ['l_ladder_jump_a', 'l_ladder_jump_b', 'l_ladder_jump_c']) run(n, { from: 480 });
// The replay above has no ladder action, so run ladder.js's own canEnter on the frame before the
// ✕ press edge (no press: the map update's gimmick choice) and on the ✕ frame, with the game's pose,
// state and speed from those records (= what frame f's logic sees), against the game's state on
// the next record (0xb = mounted) and its jump (state 8 / 0x1f = the ✕ was not taken).
{
  const realFetch = globalThis.fetch;
  globalThis.fetch = async u => String(u).startsWith('file:')
    ? { ok: true, json: async () => JSON.parse(readFileSync(fileURLToPath(u), 'utf8')) } : realFetch(u);
  const { ladderAction } = await import(pathToFileURL(path.join(here, 'actions/ladder.js')).href);
  ladderAction();                                     // starts the data load (z128_ladders.json)
  for (let i = 0; i < 5; i++) await new Promise(r => setImmediate(r));
  for (const [name, f] of [['l_ladder_jump', 515], ['l_ladder_jump_a', 520], ['l_ladder_jump_b', 535],
                           ['l_ladder_jump_c', 556], ['l_ladder_jump_d', 500], ['l_ladder_jump_e', 521]]) {
    const file = path.join(OUT, name + '.jsonl');
    if (!existsSync(file)) { console.log(`skip ${name} ladder check: no trace`); continue; }
    ran++;
    const R = load(file), lad = ladderAction();
    const at = (r, press) => {
      const p = r.matrix[3], fw = r.matrix[2];
      return { ctrl: { pos: { x: p[0], y: p[1], z: p[2] }, heading: Math.atan2(fw[0], fw[2]), onGround: groundOf(r) === 1,
                       P: { state: stateOf(r) }, speed: r.ground_speed / 60, scale: 1 },
               input: { pressed: new Set(press ? ['Space'] : []), keys: new Set() }, anim: { has: () => true }, motions: {} };
    };
    const get = k => R.find(r => r.frame === k);
    // M+0x10ac per frame (traces with the m_gimmick field): the choice from record r - 1's pose
    // must be what the game shows in record r, on every frame before the ✕
    if (R[1].m_gimmick !== undefined) {
      const sel = ladderAction(); let off = 0, first = null, n = 0;
      for (let i = 2; i < R.length && R[i].frame <= f; i++) {
        sel.canEnter(at(R[i - 1], false)); n++;
        if (!!sel.gimmick !== ((R[i].m_gimmick & 0xffff) !== 0xffff)) { off++; first ??= R[i].frame; }
      }
      ok(off === 0, `${name}: ladder gimmick choice (M+0x10ac) off ${off} of ${n}${first !== null ? ', first ' + first : ''}`);
    }
    lad.canEnter(at(get(f - 1), false));
    const page = lad.canEnter(at(get(f), true)), g = stateOf(get(f + 1));
    const game = g === 0xb, gameJump = g === 8 || g === 0x1f;
    ok(page === game && (page || lad.ate !== gameJump) && (!page || lad.gimmick === null),
       `${name} ✕ f${f} (state ${stateOf(get(f))}): page ${page ? 'mounts' : lad.ate ? 'takes ✕, no mount' : 'leaves ✕ to the jump'}, game state ${g.toString(16)}`);
  }
  { // hand-built (FUN_00333150, not reached by a trace): the trigger chosen last frame takes ✕ even
    // when the origin has left its box this frame (FUN_005507a0 = 0): no mount and no jump (bVar2)
    const lad = ladderAction(), F = [0.859, 0, -0.512], h = Math.atan2(F[0], F[2]);
    const at = (x, press) => ({ ctrl: { pos: { x, y: -90.63, z: -1108.82 }, heading: h, onGround: true, P: { state: 2 }, speed: 1 },
                                input: { pressed: new Set(press ? ['Space'] : []) }, anim: { has: () => true }, motions: {} });
    lad.canEnter(at(-1114.4, false));                 // local z -3.99: inside (half size 4.173)
    const chosen = lad.gimmick, page = lad.canEnter(at(-1114.7, true));   // local z -4.29: outside
    ok(chosen === 'bottom' && !page && lad.ate, `ladder ✕ with the origin just out of the chosen box: chosen ${chosen}, mounts ${page}, takes ✕ ${lad.ate}`);
    // the ✕ gate is P+0x118 bit 0x200 (clear in state 8 also on the take-off frames, where the ground
    // bit is still set), and the choice outside states 2..5 (FUN_0053b500): cleared while moving
    // (C+0x1d8 > 0), else kept
    const st = (x, state, speed, press, gnd = true) => { const c = at(x, press); c.ctrl.P.state = state; c.ctrl.speed = speed; c.ctrl.onGround = gnd; return c; };
    const g2 = ladderAction(); g2.canEnter(st(-1109.71, 2, 0, false));
    const takeoff = g2.canEnter(st(-1109.71, 8, 0, true)), kept = g2.gimmick;
    g2.canEnter(st(-1109.71, 9, 0.5, false, false));
    ok(!takeoff && !g2.ate && kept === 'bottom' && g2.gimmick === null,
       `ladder ✕ in state 8 on the ground: mounts ${takeoff}, takes ✕ ${g2.ate}; air choice kept still ${kept}, cleared moving ${g2.gimmick}`);
    // player.js: an action that takes ✕ without entering stops the jump (takeOff's ladder flag)
    const ctrl = new PlayerController(col, { pos: { x: -1109.71, y: -90.63, z: -1108.82 }, heading: h });
    ctrl.attach({ anim: stubAnim({ clip: 'h_st', frame: 0 }) });
    ctrl.addAction({ name: 'eat', canEnter() { this.ate = true; return false; }, enter: () => null, update: () => ({}), exit() {} });
    ctrl.step({ x: 0, z: 0, stick: 0, run: false, keys: new Set(), pressed: new Set(), cross: true });
    ok(ctrl.P.state === 2 && !ctrl.jump.head, `player.js: ✕ taken by the ladder without a mount: state ${ctrl.P.state}, jump ${!!ctrl.jump.head}`);
  }
  // OPTION airgrab (play.html?airgrab=1, not in the game): a ✕ in an air state that passes the
  // ladder choice rule mounts. _b's inputs (✕ mid-air at the ladder face, f535) through the page's
  // step with the option on: the frames before the ✕ still match the game, the ✕ mounts (no
  // second jump). Then a mid-air ✕ away from every ladder is still the second jump.
  const airgrabRun = (name, fx, pos = null) => {
    const file = path.join(OUT, name + '.jsonl');
    if (!existsSync(file)) { console.log(`skip ${name} airgrab: no trace`); return null; }
    ran++;
    const R = load(file).slice(1).filter(d => d.frame >= 480), r0 = R[0], p0 = r0.matrix[3], f0 = r0.matrix[2];
    const off = pos ? [pos[0] - p0[0], pos[1] - p0[1], pos[2] - p0[2]] : [0, 0, 0];
    const ctrl = new PlayerController(col, { pos: { x: p0[0] + off[0], y: p0[1] + off[1], z: p0[2] + off[2] }, heading: Math.atan2(f0[0], f0[2]) });
    ctrl.C.face = { x: f0[0], y: f0[1], z: f0[2] };
    ctrl.P.state = stateOf(r0);
    const anim = stubAnim(headOf(r0));
    anim.has = n => n.startsWith('c_ld') || SpeedAnimator.prototype.has.call(anim, n);   // the ladder clips (owned by ladder.js)
    anim.play = (n, o) => { if (!n.startsWith('c_ld')) SpeedAnimator.prototype.play.call(anim, n, o); };
    ctrl.attach({ anim });
    ctrl.addAction(ladderAction({ airgrab: true }));
    let prev = 0, stOff = 0, firstOff = null, at = null;
    for (let i = 0; i + 1 < R.length && R[i].frame <= fx + 20; i++) {
      const d = R[i], pad = d.pad_in, m = R[i + 1].cam_matrix;
      const sx = padAxis(pad.lx - 128, pad.ly - 128), sy = padAxis(pad.ly - 128, pad.lx - 128);
      const w = stickToWorld(sx, sy, m[0], m[2]);
      const cross = (pad.buttons & CROSS) !== 0 && (prev & CROSS) === 0;
      prev = pad.buttons;
      const before = ctrl.P.state;
      ctrl.step({ x: w.x, z: w.z, stick: w.mag, run: false, keys: new Set(), pressed: new Set(cross ? ['Space'] : []), cross });
      if (d.frame < fx && ctrl.P.state !== stateOf(R[i + 1])) { stOff++; firstOff ??= d.frame; }
      if (d.frame === fx) at = { cross, before, active: ctrl.active?.name ?? null, phase: ctrl.active?.phase, jump: ctrl.jump.head?.clip ?? null, state: ctrl.P.state };
    }
    return { at, stOff, firstOff, end: { active: ctrl.active?.name ?? null, phase: ctrl.active?.phase, pos: { ...ctrl.pos } } };
  };
  {
    const r = airgrabRun('l_ladder_jump_b', 535);
    if (r) {
      ok(r.stOff === 0 && r.at.cross && (r.at.before === 8 || r.at.before === 9) && r.at.active === 'ladder' && r.at.state !== 0x1f &&
         r.end.active === 'ladder' && r.end.phase !== null,
         `airgrab: l_ladder_jump_b ✕ f535 mid-air (state ${r.at.before}) mounts: action ${r.at.active} (${r.at.phase}), state ${r.at.state}; ` +
         `before the ✕ state off ${r.stOff}${r.firstOff !== null ? ' first ' + r.firstOff : ''}; f555 ${r.end.active} ${r.end.phase} feet y ${r.end.pos.y.toFixed(2)}`);
    }
    // the same inputs 60 units along -z (no ladder box near): the ✕ is the second jump (state 0x1f)
    const p = (() => { const f = path.join(OUT, 'l_ladder_jump_b.jsonl'); if (!existsSync(f)) return null;
                       const r0 = load(f).slice(1).find(d => d.frame >= 480).matrix[3]; return [r0[0], r0[1], r0[2]]; })();
    const away = p && airgrabRun('l_ladder_jump_b', 535, [p[0], p[1], p[2] - 60]);
    if (away) ok(away.at.cross && away.at.active === null && away.at.state === 0x1f,
                 `airgrab: mid-air ✕ away from the ladder: action ${away.at.active}, state ${away.at.state.toString(16)} (second jump)`);
  }
  globalThis.fetch = realFetch;
}
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
function runArmed(name = 'aim_g_air', order = 'h_jp0_a h_jp1_a h_jp3_a h_jp4_a', after = null, { state = false, from = 0 } = {}) {
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
  const S = { clip: 0, clipF: null, fr: 0, frF: null, b: 0, bF: null, o: 0, oF: null, n: 0, pos: 0, st: 0, stF: null };
  const seen = [];
  for (i = 0; i + 1 < R.length; i++) {
    const d = R[i], pad = d.pad_in;
    const cross = (pad.buttons & CROSS) !== 0 && (prevButtons & CROSS) === 0;
    prevButtons = pad.buttons;
    ctrl.step({ x: 0, z: 0, stick: 0, run: false, keys: new Set(), pressed: new Set(), cross });
    const g = R[i + 1], gh = headOf(g), gp = g.matrix[3];
    const our = NAME[anim.head.key] ?? anim.head.key;
    if (g.frame < from) continue;
    if (state && ctrl.P.state !== stateOf(g)) { S.st++; S.stF ??= `${g.frame} ${ctrl.P.state} vs ${stateOf(g)}`; }
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
  if (state) ok(S.st === 0, `${L}: player state off ${S.st}${S.stF ? ', first ' + S.stF : ''}`);
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
// worker 0110: the motion-done events (FUN_00341af0 -> FUN_0056a038 case 0 -> state handler +0x1c ->
// FUN_0055b8e0). A walk-off (hand-built: no ledge is reachable from the save state): the locomotion
// class is done once the ground bit is clear, state 2's FUN_00537f38 gives 9, the old class's +0x24
// (FUN_00537d58 case 9) gives the fall class 0x22; it lands through FUN_00538aa0 (2, or 3 with the
// gun out: P+0x100 bit 44) and FUN_00538a30's landing class 0x21. Not while an action owns him.
{
  const JC = await import(new URL('./jump_core.js', import.meta.url).href);
  ok(JC.stateEvent(2, 3, false, false) === 9 && JC.stateEvent(3, 8, false, true) === 9 &&
     JC.stateEvent(2, 0x1e, false, false) === 0x10 && JC.stateEvent(2, 0x21, true, false) === 0 &&
     JC.stateEvent(2, 0x1c, true, true) === 3,
     'FUN_00537f38: ground clear -> 9 (0x1e / 0x3f -> 0x10); ground set: 0x1c / 0x1e / 0x3f -> 2 / 3, others 0');
  ok(JC.stateEvent(8, 0x4e, false, false) === 0 && JC.stateEvent(9, 0x55, false, true) === 0 &&
     JC.stateEvent(8, 0x4e, true, true) === 3 && JC.stateEvent(0x20, 0x59, false, false) === 2 &&
     JC.stateEvent(9, 0x22, true, false) === 2,
     'FUN_00538aa0: 0x55 / 0x4e airborne -> 0, else 2 (3 with bit 44); states 8 / 9 / 0x1f-0x21');
  const r0 = load(path.join(OUT, 'j_stand.jsonl'))[1], p0 = r0.matrix[3];
  for (const drawn of [false, true]) {
    const c = new PlayerController(col, { pos: { x: p0[0], y: p0[1], z: p0[2] }, heading: 0 });
    let feet = 0;
    c.weapon = { drawn: () => drawn, enterFoot() { feet++; } };
    c.P.state = drawn ? 3 : 2; c.onGround = false;
    c.jump.motionUpdate(c, { action: true });
    const owned = c.P.state;
    c.jump.motionUpdate(c, {});
    const s1 = c.P.state, m1 = c.jump.motion && c.jump.motion.id;
    c.jump.motionUpdate(c, {});                         // the fall class's phase 0 (still airborne)
    const s2 = c.P.state;
    c.onGround = true;
    c.jump.motionUpdate(c, {});                         // on the ground: done -> event 0x22
    const s3 = c.P.state, m3 = c.jump.motion && c.jump.motion.id;
    const L = drawn ? 'gun out' : 'gun away';
    ok(owned === (drawn ? 3 : 2) && s1 === 9 && m1 === 0x22 && s2 === 9 && s3 === (drawn ? 3 : 2) && m3 === 0x21 && feet === 1,
       `walk-off (${L}): owned ${owned}, ground lost -> state ${s1} motion ${m1 && m1.toString(16)}, landed -> state ${s3} motion ${m3 && m3.toString(16)}, enter ${feet}`);
    // the landing class done while the ground bit is clear again: FUN_00537f38 -> 9, a new fall class
    c.jump.motion.done = true; c.onGround = false;
    c.jump.motion.update = () => [];
    c.jump.motionUpdate(c, {});
    ok(c.P.state === 9 && c.jump.motion && c.jump.motion.id === 0x22, `${L}: landing class done in the air -> state ${c.P.state}`);
  }
}
// the ladder hand-over GUESS (player.js): a drop hands over in state 9; the action's own landing
// (locomotion + done, ground bit set) gives state 2. A stub action in ladder.js's result shape.
{
  const r0 = load(path.join(OUT, 'j_stand.jsonl'))[1], p0 = r0.matrix[3];
  const c = new PlayerController(col, { pos: { x: p0[0], y: p0[1], z: p0[2] }, heading: 0 });
  let n = 0;
  c.addAction({ name: 'ladder', canEnter: () => n === 0, enter: () => null, exit: () => {},
                update: () => (++n === 1 ? { onGround: false } : c.onGround ? { locomotion: true, done: true } : { locomotion: true }) });
  const idle = { x: 0, z: 0, keys: new Set(), pressed: new Set() };
  c.step(idle);
  const dropped = c.P.state;
  for (let i = 0; i < 120 && c.active; i++) c.step(idle);
  ok(dropped === 9 && !c.active && c.P.state === 2, `ladder stub: drop -> state ${dropped}, its landing -> state ${c.P.state}`);
}
// worker 0112: FUN_0055b8e0's motion half for the ground draw / holster (FUN_00537d58 case 3,
// FUN_00538120 case 2; moving = C+0x1d8 != 0), and the ladder mount: FUN_0055b8e0(P, 0xb / 0xc)
// sets the ladder's motion 0x26 / 0x27 (jumptable 0x589e70), so a running jump / fall class stops.
{
  const JC = await import(new URL('./jump_core.js', import.meta.url).href);
  ok(JC.stateMotion(2, 3, false) === 5 && JC.stateMotion(2, 3, true) === 9 && JC.stateMotion(3, 2, false) === 3 &&
     JC.stateMotion(3, 2, true) === 8 && JC.stateMotion(3, 9) === 0x22 && JC.stateMotion(9, 3) === 0x21,
     'stateMotion: 2 -> 3: 5 / 9 moving; 3 -> 2: 3 / 8 moving; 2 / 3 -> 9: 0x22; air -> 2 / 3: 0x21');
  const r0 = load(path.join(OUT, 'j_stand.jsonl'))[1], p0 = r0.matrix[3];
  const c = new PlayerController(col, { pos: { x: p0[0], y: p0[1], z: p0[2] }, heading: 0 });
  c.P.state = 9; c.jump.motion = new JC.FallMotion(0x20);
  let n = 0;
  c.addAction({ name: 'ladder', canEnter: () => n === 0, enter: () => null, exit: () => {},
                update: () => (++n, {}) });
  c.step({ x: 0, z: 0, keys: new Set(), pressed: new Set() });
  ok(c.active && c.jump.motion === null && !c.jump.events.some(e => e.ev === 'land'),
     `ladder mount: the running fall class stops (motion ${c.jump.motion && c.jump.motion.id})`);
  // mount on the frame after landing (state 8, jump class not done yet): the game's mount state
  // 0xb / 0xc isn't an air state, so he must not stay in 8 with no motion left (can't jump again)
  const d = new PlayerController(col, { pos: { x: p0[0], y: p0[1], z: p0[2] }, heading: 0 });
  const idle = { x: 0, z: 0, keys: new Set(), pressed: new Set() };
  for (let i = 0; i < 5; i++) d.step(idle);
  JC.enterState(d.P, 8); d.P.jumpBits = 1; d.jump.motion = new JC.JumpMotion(false);
  let m = 0;
  d.addAction({ name: 'ladder', canEnter: () => m === 0, enter: () => null, exit: () => {},
                update: () => (++m < 3 ? {} : { locomotion: true, done: true }) });
  for (let i = 0; i < 40; i++) d.step(idle);
  const st = d.P.state, bits = d.P.jumpBits, again = d.jump.takeOff(d, true, {});
  ok(st === 2 && bits === 0 && again === 8,
     `ladder mount on the landing frame: state ${st}, bits ${bits}, jump -> ${again} after the ladder (want 2, 0, 8)`);
}
// worker 0112: a draw / holster on the ground during the landing: FUN_005766c0 -> FUN_0055b8e0(P, 3 / 2,
// 0, 0), whose motion half (the old state's +0x24: FUN_00537d58 case 3 -> 5 / 9, FUN_00538120 case 2
// -> 3 / 8) replaces the landing class 0x21 at once (jump_core Jump.enter). j_gun_land_holster:
// gun out, lands f81 (state 3), circle f82 -> state 2 + h_st, h_jp4_a never plays. j_land_draw:
// lands f51, h_jp4 from f52, R1 f58 -> state 3 + h_st_a. Clip and state checked on every frame.
// (j_gun_land_holster is compared from f83, after the holster: no jump clip may show where the game
// plays h_st. Before that the game holds h_jp1_a at 9.0 one frame longer than ClipClock's GUESS
// queue rule (f61 vs f60), and the offset runs to the landing: a separate gap, notes/jump-research.md)
for (const [n, order, f, st, next, from] of [['j_gun_land_holster', '', 82, 2, 3, 83],
                                             ['j_land_draw', 'h_jp0 h_jp1 h_jp2 h_jp3 h_jp4', 58, 3, 5, 0]]) {
  runArmed(n, order, (ctrl, L) => {
    const ev = ctrl.jump.events.filter(e => e.ev === 'loco').map(e => `${e.frame}:${e.next}`);
    ok(ev.length === 1 && ev[0] === `${f}:${next}`, `${L}: landing class replaced by the motion of FUN_0055b8e0(P, ${st}) on [${ev}] (game: f${f}, motion ${next})`);
  }, { state: true, from });
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
// body state 3 mid-ramp (hand-built: the trace lands at the ramp's foot and crosses at once, so it
// can't tell these apart): on the ladder's drop polygon 25, 13 above its foot, with body+0x40 = 0
// and the stick's velocity along +z. FUN_003590c8 case 3 doesn't copy body+0x30 into +0x40 and
// walks (stale +0x40 across the slope + the slide) projected along the plane normal (FUN_00260e28),
// so he moves straight down the ramp: z unchanged, y down by the slide's length, on the plane.
{
  const x0 = -1109.4, z0 = -1108.6, f = col.findPoly(x0, z0, -115, -92);
  const c = new PlayerController(col, { pos: { x: x0, y: f[1], z: z0 }, heading: 0 });
  const B = c.body;
  Object.assign(B, { poly: f[0], gy: f[1], state: 3, vel: { x: 0, y: 0, z: 0 }, t: 0.5, rise: 0.3338, v0: -78.5 });
  c.C.dir = { x: 0, y: 0, z: 1 }; c.C.spd = 1;
  const y0 = c.pos.y;
  c.moveTick();
  const pl = col.polys[f[0]].plane, onPlane = Math.abs(pl[0] * c.pos.x + pl[1] * c.pos.y + pl[2] * c.pos.z + pl[3]);
  ok(f[0] === 25 && Math.abs(c.pos.z - z0) < 1e-3 && c.pos.y - y0 > 0.5 && onPlane < 1e-3 && B.state === 3 && !c.onGround,
     `state 3 mid-ramp: slides down the plane only (dz ${(c.pos.z - z0).toExponential(1)}, dy ${(c.pos.y - y0).toFixed(3)}, off plane ${onPlane.toExponential(1)}, state ${B.state})`);
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
