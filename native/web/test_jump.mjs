// Replays PCSX2 jump traces (tools/trace, game-derived, gitignored) through jump_core.js +
// player.js's body and checks every frame: position, player state, ground bit, the take-off
// record (body t / y0 / v0 / rise where logged) and the jump clip on the motion player's
// head layer.
//   node --import ./native/web/test_hooks.mjs native/web/test_jump.mjs [jump_core.js] [player.js]
// Traces: tools/trace/out/{e_jumps,i_air_release}.jsonl (default fields) and
// j_stand / j_run / j_steer / j_double / j_back (TRACE_FIELDSET=jump). Missing traces are
// reported and skipped; the test fails if none was found.
import { readFileSync, existsSync } from 'node:fs';
import { pathToFileURL, fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const JC = await import(pathToFileURL(path.resolve(process.argv[2] ?? path.join(here, 'jump_core.js'))).href);
const PL = await import(pathToFileURL(path.resolve(process.argv[3] ?? path.join(here, 'player.js'))).href);
const { Collision } = await import(pathToFileURL(path.join(here, 'collision.js')).href);
const { PlayerController, padAxis, stickToWorld } = PL;
const { Jump, ClipClock } = JC;

const buf = readFileSync(path.join(here, 'data/z128_collision.bin'));
const col = new Collision(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
const OUT = process.env.TRACE_OUT ?? path.join(root, 'tools/trace/out');
const CROSS = 64;                                     // pad_in.buttons bit for ✕ (e_jumps frame 10)
const POS_TOL = 3e-3;                                 // units, re-synced on each ✕ frame. Residual: the air slide along the z ≈ -1113.35 railing loses ~1e-4 per frame (collision.js air walker; j_back 2.7e-3, j_steer 2.1e-3)
const STEP_TOL = 3e-4;                                // units per frame in a jump state (railing slide above; and the first airborne step of a running jump is 2.5e-4 longer in the game, player.js integrate)
const FRAME_TOL = 1e-3;                               // clip frames

let fail = 0, ran = 0;
const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) fail++; };
const load = p => readFileSync(p, 'utf8').trim().split('\n').map(l => JSON.parse(l));
const headOf = d => { const h = Buffer.from(d.mp_head, 'hex'); return d.mp_layers[h[8] & 7]; };
const stateOf = d => Buffer.from(d.p_state, 'hex')[4];
const groundOf = d => (d.mflags >> 9) & 1;
const JUMP_CLIPS = /^h_jp[0-4]$|^c_wjp[0-2]$/;
const bodyF = (d, o) => Buffer.from(d.body, 'hex').readFloatLE(o);

function replay(name, { from = null, to = null } = {}) {
  const file = path.join(OUT, name + '.jsonl');
  if (!existsSync(file)) { console.log(`skip ${name}: no trace (${file})`); return null; }
  ran++;
  const [, ...R] = load(file);
  const i0 = from === null ? 0 : R.findIndex(d => d.frame === from);
  const iN = to === null ? R.length - 1 : R.findIndex(d => d.frame === to);
  const r0 = R[i0], p0 = r0.matrix[3], f0 = r0.matrix[2];
  const ctrl = new PlayerController(col, { pos: { x: p0[0], y: p0[1], z: p0[2] }, heading: Math.atan2(f0[0], f0[2]) });
  ctrl.C.face = { x: f0[0], y: f0[1], z: f0[2] };
  ctrl.P.state = stateOf(r0);
  const jump = new Jump();
  const res = { name, R, frames: [], events: jump.events, ctrl, jump };
  let prevButtons = r0.frame > 0 ? (R[i0 - 1]?.pad_in.buttons ?? 0) : 0;
  for (let i = i0; i < iN; i++) {
    const d = R[i], pad = d.pad_in, m = R[i + 1].cam_matrix;   // the camera is updated before FUN_00333150 in a frame (worker 0089)
    const sx = padAxis(pad.lx - 128, pad.ly - 128), sy = padAxis(pad.ly - 128, pad.lx - 128);
    const w = stickToWorld(sx, sy, m[0], m[2]);
    const cross = (pad.buttons & CROSS) !== 0 && (prevButtons & CROSS) === 0;
    prevButtons = pad.buttons;
    ctrl.frames = d.frame;
    // re-sync on the ✕ frame (while on the ground): removes player.js's ground-run drift
    // (2.2e-3 in e_jumps' run) so the check measures the jump itself
    let sync = false;
    if (cross && ctrl.onGround) { const q = d.matrix[3]; ctrl.pos = { x: q[0], y: q[1], z: q[2] }; sync = true; }
    const reqs = jump.frame(ctrl, { x: w.x, z: w.z, stick: w.mag }, cross);
    const g = R[i + 1], gp = g.matrix[3], gh = headOf(g);
    res.frames.push({
      frame: g.frame, pos: { ...ctrl.pos }, gpos: gp, state: ctrl.P.state, gstate: stateOf(g),
      ground: ctrl.onGround ? 1 : 0, gground: groundOf(g), clip: jump.clock?.clip ?? null,
      cframe: jump.clock?.frame ?? null, gclip: gh.clip, gcframe: gh.frame, body: { ...ctrl.body },
      gbody: g.body ? { t: bodyF(g, 0xd4), y0: bodyF(g, 0xd8), dy: bodyF(g, 0xdc), v0: bodyF(g, 0xe0), rise: bodyF(g, 0xe8), state: Buffer.from(g.body, 'hex').readUInt32LE(0xa8) } : null,
      spd: ctrl.C.spd, gspd: g.want_dir_speed[3], active: jump.active, sync, face: { ...ctrl.C.face }, gface: g.matrix[2],
    });
  }
  return res;
}

function check(res, label) {
  if (!res) return;
  const F = res.frames;
  const dump = (process.env.JUMP_DUMP ?? '').split(':');   // JUMP_DUMP=name:from:to prints frames
  if (dump[0] === res.name) for (const f of F) if (f.frame >= +dump[1] && f.frame < +dump[2])
    console.log(f.frame, f.state, f.gstate, f.ground, f.gground, [f.pos.x, f.pos.y, f.pos.z].map(v => v.toFixed(4)).join(','),
      f.gpos.slice(0, 3).map(v => v.toFixed(4)).join(','), f.spd.toFixed(6), f.gspd.toFixed(6), f.clip, f.cframe?.toFixed(2), f.gclip, f.gcframe.toFixed(2));
  let fe = 0, prev = null, de = 0, df = null, pe = 0, pf = null, se = 0, sf = null, ge = 0, gf = null, ce = 0, cf = null, ve = 0, be = 0, bf = null;
  for (const f of F) {
    // position: during the jump (from the ✕ frame to the end of the landing clip)
    const e = Math.hypot(f.pos.x - f.gpos[0], f.pos.y - f.gpos[1], f.pos.z - f.gpos[2]);
    if (f.active && e > pe) { pe = e; pf = f.frame; }
    if (f.state !== f.gstate) { se++; sf ??= f.frame; }
    if (prev && !f.sync && (f.gstate === 8 || f.gstate === 0x1f || f.gstate === 0x20 || f.gstate === 0x21)) {
      const e2 = Math.hypot(f.pos.x - prev.pos.x - (f.gpos[0] - prev.gpos[0]), f.pos.y - prev.pos.y - (f.gpos[1] - prev.gpos[1]), f.pos.z - prev.pos.z - (f.gpos[2] - prev.gpos[2]));
      if (e2 > de) { de = e2; df = f.frame; }
    }
    prev = f;
    if (f.active) fe = Math.max(fe, Math.hypot(f.face.x - f.gface[0], f.face.z - f.gface[2]));
    if (f.ground !== f.gground) { ge++; gf ??= f.frame; }
    ve = Math.max(ve, Math.abs(f.spd - f.gspd));
    // jump clip on the head layer: whenever the game's head is a jump clip, ours must match
    if (JUMP_CLIPS.test(f.gclip) || f.active) {
      if (f.clip !== f.gclip || Math.abs(f.cframe - f.gcframe) > FRAME_TOL) { ce++; cf ??= `${f.frame} ${f.clip}:${f.cframe?.toFixed(2)} vs ${f.gclip}:${f.gcframe.toFixed(2)}`; }
    }
    if (f.gbody && f.gbody.state === 2 && f.body.state === 2) {
      const b = Math.max(Math.abs(f.body.t - f.gbody.t), Math.abs(f.body.v0 - f.gbody.v0), Math.abs(f.body.rise - f.gbody.rise), Math.abs(f.body.dy - f.gbody.dy));
      if (b > be) { be = b; bf = f.frame; }
    }
  }
  ok(pe <= POS_TOL, `${label}: position max err ${pe.toExponential(2)} (frame ${pf}) over ${F.length} frames`);
  ok(de <= STEP_TOL, `${label}: per-frame displacement in the jump states max err ${de.toExponential(2)} (frame ${df})`);
  ok(fe <= 1e-4, `${label}: facing (matrix row 2) during the jump max err ${fe.toExponential(2)}`);
  ok(se === 0, `${label}: player state matches every frame (${se} off${sf !== null ? ', first ' + sf : ''})`);
  ok(ge === 0, `${label}: ground bit matches every frame (${ge} off${gf !== null ? ', first ' + gf : ''})`);
  ok(ve <= 1e-5, `${label}: C+0xcc move speed max err ${ve.toExponential(2)}`);
  ok(ce === 0, `${label}: jump clip on the head layer matches (${ce} off${cf ? ', first ' + cf : ''})`);
  // landing motion done (FUN_00599aa0 -> 3 / 8) on frame k: the game's next motion state plays
  // its clip on frame k + 1, so the head is still a jump clip in record k + 1 and not in k + 2
  const byFrame = new Map(res.R.map(d => [d.frame, d]));
  let le = 0, lf = null;
  for (const e of res.events.filter(e => e.ev === 'loco')) {
    const a1 = byFrame.get(e.frame + 1), a2 = byFrame.get(e.frame + 2);
    if (!a1 || !a2) continue;
    if (!(JUMP_CLIPS.test(headOf(a1).clip) && !JUMP_CLIPS.test(headOf(a2).clip))) { le++; lf ??= e.frame; }
  }
  ok(le === 0, `${label}: landing hands back to locomotion on the game's frame (${le} off${lf !== null ? ', first ' + lf : ''})`);
  if (F[0].gbody) ok(be <= 1e-4, `${label}: body take-off record t/v0/rise/dy max err ${be.toExponential(2)}${bf !== null ? ' (frame ' + bf + ')' : ''}`);
}

// ---------------------------------------------------------------- traces
const ej = replay('e_jumps');
check(ej, 'e_jumps (standing + running jump)');
if (ej) {
  const ev = ej.events.filter(e => e.ev === 'jump').map(e => e.frame);
  ok(ev.length === 2 && ev[0] === 10 && ev[1] === 140, `e_jumps: jumps start on the ✕ frames 10, 140 (got ${ev})`);
  // apex 13.09 above the start
  const y0 = ej.R[0].matrix[3][1];
  const apex = Math.min(...ej.frames.filter(f => f.frame < 60).map(f => f.pos.y));
  ok(Math.abs((y0 - apex) - 13.09) < 0.01, `e_jumps: standing apex ${(y0 - apex).toFixed(3)} (game 13.09)`);
}
check(replay('i_air_release'), 'i_air_release (running jump, stick released in the air)');
for (const n of ['j_stand', 'j_run', 'j_steer', 'j_double', 'j_back']) check(replay(n), n);

ok(ran > 0, `traces replayed: ${ran}`);
process.exit(fail ? 1 : 0);
