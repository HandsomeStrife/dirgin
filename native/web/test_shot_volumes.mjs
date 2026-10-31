// Bullets through flag-0x20 trigger volumes (enemy_ai.js shotVolumes): FUN_005503e8 gives a shape-0 / 8
// trigger with flags +0xa4 bit 5 the bits +0x38 bit 0, +0x28 bit 8 and +0x40 bit 4, so FUN_005442d8 lists
// it as see-through for every bullet and FUN_005511e8 runs its script event 0x12 when the bullet's
// segment crosses the box nearer than the closest solid hit. Trigger 77 (script 100) is the volume over
// the platform soldiers (records 46-49, objects 68-71): its event 0x12 wakes them (wake n1).
//   node native/web/test_shot_volumes.mjs [enemy_ai.js]     (needs data/enemies/ai.json; else SKIP)
import { readFileSync, existsSync } from 'node:fs';
import { pathToFileURL, fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const AI = await import(pathToFileURL(path.resolve(process.argv[2] ?? path.join(here, 'enemy_ai.js'))).href);
const dataPath = path.join(here, 'data/enemies/ai.json');
let fail = 0;
const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) fail++; };
const done = () => { console.log(fail ? `${fail} FAILED` : 'all ok'); process.exit(fail ? 1 : 0); };
if (!existsSync(dataPath)) { console.log('SKIP: data/enemies/ai.json missing'); done(); }
const data = JSON.parse(readFileSync(dataPath, 'utf8'));

const SAVE = [44, 45, 46, 47, 48, 49, 66];
// one bullet that covers a..b in its first moving frame (delay 0, speed |b - a| / step)
function world(opts = {}) {
  const player = { pos: opts.ppos ?? [-1070, -146.1, -1105.6], hp: 780, id: 0x16, f118: 0, s64: 0 };
  const W = AI.createEnemyAI({ data, player, rand: () => 0.5, quiet: true, damage: !!opts.damage });
  for (const k of SAVE) W.addActor({ rec: k, astate: 1 });
  return { W, player };
}
function fire(W, from, a, b, extra = {}) {
  const v = [0, 1, 2].map(k => b[k] - a[k]), L = Math.hypot(...v);
  W.bullets.push({ from, start: a.slice(), dir: v.map(x => Math.fround(x / L)), end: b.slice(), endDist: L + 1, range: L + 2,
                   speed: L / 1.2, age: 0, pos: a.slice(), prev: a.slice(), f120: 0, delay: 0, damage: 10, hit: 0, kind: 0,
                   shooter: from === null ? 0x16 : from.id, ...extra });
}
function run(W, n = 3) { const ev = []; for (let i = 0; i < n; i++) ev.push(...W.tick(1.2, null, null)); return ev; }
const T77 = data.triggers[77].m[3];                          // (-908.4, -140.2, -857.4); box x +-19.5, z +-33.1, 27.5 tall
const Y = T77[1] - 20;                                       // 20 above the platform floor (over the soldiers' heads, 18)
const t77 = ev => ev.filter(e => e.startsWith('event t77 '));
const woke = ev => [68, 69, 70, 71].filter(id => ev.some(e => e.startsWith(`event ${id} script `) && / ev 2 r 1,/.test(e)));

{ // 1. Vincent's bullet across the box
  const { W, player } = world();
  fire(W, player, [T77[0] - 60, Y, T77[2]], [T77[0] + 60, Y, T77[2]]);
  const ev = run(W);
  ok(t77(ev).length === 1 && /ev 18 r 22,22,/.test(t77(ev)[0]) && W.triggers[77].vars[0] === 1,
     `a bullet across trigger 77 runs its event 0x12 once (r0 = owner 0x16, r1 = its id): ${t77(ev)[0]}; once-var ${W.triggers[77].vars[0]}`);
  ok(woke(ev).length === 4, `... and wakes 68-71 (records 46-49): ${woke(ev)}`);
}
{ // 2. beside the box (z 70 u off): nothing
  const { W, player } = world();
  fire(W, player, [T77[0] - 60, Y, T77[2] + 70], [T77[0] + 60, Y, T77[2] + 70]);
  ok(t77(run(W)).length === 0, 'a bullet beside trigger 77: no event');
}
{ // 3. start inside, end outside (the exit point, FUN_00550da0's second clip); both inside: nothing
  let { W, player } = world();
  fire(W, player, [T77[0], Y, T77[2]], [T77[0] + 60, Y, T77[2]]);
  ok(t77(run(W)).length === 1, 'a bullet from inside trigger 77 to outside: event 0x12 (exit point)');
  ({ W, player } = world());
  fire(W, player, [T77[0] - 5, Y, T77[2]], [T77[0] + 5, Y, T77[2]]);
  ok(t77(run(W)).length === 0, 'a bullet segment wholly inside trigger 77: no event (no clip point)');
}
{ // 4. trigger 204 (script 100 too; flags 0x20020: unarmed at stage start) only fires once armed (cmd 0x108)
  const T = data.triggers[204].m[3];
  let { W, player } = world();
  fire(W, player, [T[0] - 90, T[1] - 20, T[2]], [T[0] + 90, T[1] - 20, T[2]]);
  let ev = run(W);
  ok(!ev.some(e => e.startsWith('event t204 ')) && !ev.some(e => e.startsWith('event t77 ')), 'unarmed trigger 204 crossed: no event');
  ({ W, player } = world());
  W.setActive(W.objects.get('t204'), 1);
  fire(W, player, [T[0] - 90, T[1] - 20, T[2]], [T[0] + 90, T[1] - 20, T[2]]);
  ev = run(W);
  ok(ev.some(e => e.startsWith('event t204 script 100 ev 18')), 'trigger 204 armed, crossed: event 0x12');
}
{ // 5. trigger 89 (flags 3: enter / leave, no 0x20): bullets don't test it
  const T = data.triggers[89].m[3];
  const { W, player } = world();
  fire(W, player, [T[0] - 60, T[1] - 15, T[2]], [T[0] + 60, T[1] - 15, T[2]]);
  const ev = run(W);
  ok(!ev.some(e => e.startsWith('shot volume t89') || e.startsWith('event t89 ')), 'a bullet across trigger 89 (no flag 0x20): not listed, no event');
}
{ // 6. a soldier's bullet (damage on, aimed at Vincent): Vincent's hit nearer than the box blocks the
  // event; Vincent behind the box: the box first, then the hit
  const shooter = W => W.actors.find(a => a.rec === 49);
  const a0 = [T77[0] - 60, Y, T77[2]], b0 = [T77[0] + 60, Y, T77[2]];
  for (const [px, want] of [[T77[0] - 40, 0], [T77[0] + 40, 1]]) {
    const { W, player } = world({ damage: true });
    // his aim point (feet - PLAYER_AIM_H = 12.4 up) within BULLET_R of the line
    player.pos = [px, Y + 12, T77[2]];
    const s = shooter(W);
    fire(W, s, a0, b0, { target: player });
    const ev = run(W, 1);
    const hit = ev.some(e => e.startsWith('hit player'));
    ok(hit && t77(ev).length === want, `soldier bullet, Vincent ${px < T77[0] ? 'before' : 'behind'} the box: hit ${hit}, trigger 77 events ${t77(ev).length} (want ${want})`);
  }
}
{ // 7. #51 (record 51) inside trigger 76 (script 99, event 0x12, no run-once var): every one of his own
  // bullets that leaves the box re-runs the event, which sends the wake (event 2) to 50 / 51 (n 2) and
  // 46-49 (n 1) every time. But each soldier's event-2 script (126 / 127 / 128 / 141 / 142) is run-once on
  // its own: it starts with cmd 0x105 (its var 0) == 0 and sets it with cmd 0x104, so only the first wake
  // reaches cmd 0x143 (setMode); later shots rebuild no brain. Combined, as decoded (not traced: no PCSX2
  // run has #51 firing from inside 76).
  const { W } = world();
  const s50 = W.addActor({ rec: 50, astate: 1 }), s51 = W.addActor({ rec: 51, astate: 1 });
  const T76 = data.triggers[76].m[3];
  const shot = () => { const a = [T76[0], T76[1] - 15, T76[2]]; fire(W, s51, a, [a[0] + 60, a[1], a[2]]); return run(W, 2); };
  const t76 = ev => ev.filter(e => e.startsWith('event t76 '));
  const wakes = ev => ev.filter(e => / ev 2 r /.test(e)).map(e => +e.split(' ')[1]);
  const modes = ev => ev.filter(e => /^mode /.test(e)).map(e => +e.split(' ')[1]);
  const all = W.actors.filter(a => [46, 47, 48, 49, 50, 51].includes(a.rec));
  const ids = all.map(a => a.id).sort((a, b) => a - b);
  const ev1 = shot();
  ok(t76(ev1).length === 1 && JSON.stringify([...new Set(modes(ev1))].sort((a, b) => a - b)) === JSON.stringify(ids),
     `#51's first bullet out of trigger 76: event 0x12, woken with a mode: ${[...new Set(modes(ev1))]} (want ${ids})`);
  const brains = all.map(a => a.ai && a.ai.brain);                 // setMode makes a new brain object
  const ev2 = shot();
  const kept = all.filter((a, i) => a.ai && a.ai.brain === brains[i]).length;
  ok(t76(ev2).length === 1 && W.triggers[76].vars.every(v => !v) && wakes(ev2).length === 6,
     `his second bullet: trigger 76 runs again (no once-var), wakes sent again to ${wakes(ev2)}`);
  ok(modes(ev2).length === 0 && kept === all.length && all.every(a => a.vars && a.vars[0] === 1),
     `... but each soldier's wake script is run-once (its var 0): no setMode, ${kept}/${all.length} brains untouched`);
}
done();
