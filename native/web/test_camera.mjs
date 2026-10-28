// FUN_0032ace0 base follow: |dy| >= 15 -> the base lands on the player in one frame;
// |dy| < 15 -> min(rate x 1.2, 1) per axis. Also: reset() snaps (flag 0x2000) and measures dist.
// node --import ./native/web/test_hooks.mjs native/web/test_camera.mjs [player.js path]
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
const P = process.argv[2] ? pathToFileURL(process.argv[2]).href : new URL('./player.js', import.meta.url).href;
const { FollowCamera, cameraRays } = await import(P);
let fail = 0;
const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) fail++; };
const near = (a, b, e = 1e-5) => Math.abs(a - b) <= e;
const tick = (cam, pos) => cam.tick({ pos, want: { x: 0, y: 0, z: 1 }, state: 0, stick: 0, pad: null });
for (const dy of [15, 20, -15, -40]) {                // game space, -Y up: dy > 0 = he dropped
  const cam = new FollowCamera(null); cam.reset({ x: 0, y: 0, z: 0 }); tick(cam, { x: 0, y: 0, z: 0 });
  tick(cam, { x: 3, y: dy, z: -2 });
  ok(near(cam.base[0], 3) && near(cam.base[1], dy) && near(cam.base[2], -2), `|dy| ${Math.abs(dy)} (dy ${dy}): base snaps -> ${cam.base.map(v => v.toFixed(3))}`);
}
for (const [dy, ky] of [[14.9, 0.5 * 1.2], [-14.9, 0.15 * 1.2]]) {
  const cam = new FollowCamera(null); cam.reset({ x: 0, y: 0, z: 0 }); tick(cam, { x: 0, y: 0, z: 0 });
  tick(cam, { x: 4, y: dy, z: 0 });
  ok(near(cam.base[1], dy * ky) && near(cam.base[0], 4 * 0.125 * 1.2), `dy ${dy}: eases (y ${cam.base[1].toFixed(4)} want ${(dy * ky).toFixed(4)}, x ${cam.base[0].toFixed(4)})`);
}
{ // reset: next tick snaps, even far away
  const cam = new FollowCamera(null); cam.reset({ x: 0, y: 0, z: 0 });
  cam.reset({ x: 100, y: 5, z: 50 });
  tick(cam, { x: 101, y: 5, z: 50 });
  ok(near(cam.base[0], 101), `after reset the first frame snaps (base x ${cam.base[0].toFixed(3)})`);
}
{ // reset with collision: dist = the measured eye distance, eye consistent with it
  const buf = readFileSync(new URL('data/z128_collision.bin', P));
  const rays = cameraRays(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
  const cam = new FollowCamera(null, rays);
  const sp = { x: -631.348 + 0, y: 0, z: 0 };
  cam.reset(sp);
  const r = [cam.at[0] - cam.eye[0], cam.at[1] - cam.eye[1], cam.at[2] - cam.eye[2]];
  ok(cam.dist < 1000 && cam.dist > 1, `reset measures dist (${cam.dist.toFixed(3)}), no mouse latch: ${!cam.mouseActive}`);
}
{ // FUN_00254ac8: mouse sets DAT_004e17d0, pad x|x| > 0.3 clears it (0.55^2 = 0.3025), 0.54^2 does not
  const cam = new FollowCamera(null); cam.reset({ x: 0, y: 0, z: 0 });
  cam.addMouse(5, 0); tick(cam, { x: 0, y: 0, z: 0 });
  const on = cam.mouseActive;
  cam.tick({ pos: { x: 0, y: 0, z: 0 }, want: { x: 0, y: 0, z: 1 }, state: 0, stick: 0, pad: [0.54, 0] });
  const keep = cam.mouseActive;
  cam.tick({ pos: { x: 0, y: 0, z: 0 }, want: { x: 0, y: 0, z: 1 }, state: 0, stick: 0, pad: [0, -0.55] });
  ok(on && keep && !cam.mouseActive, `mouse latch: set ${on}, kept at 0.54 ${keep}, cleared at 0.55 ${!cam.mouseActive}`);
}
process.exit(fail ? 1 : 0);
