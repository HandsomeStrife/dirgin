# Capstone: native engine loop + native GPU renderer — what's REAL vs STUBBED

The capstone (`main.c` + `Makefile`) runs **one native program, zero emulation**:
the reauthored Dirge of Cerberus engine frame loop (`doc_engine_frame`, the
native transcription of the PS2 per-frame `while(1)` at `FUN_00248110`) drives
the native OpenGL renderer (`native/gpu`) to draw a real decoded DoC scene —
the posed, textured character `char_e000` (Vincent) standing in the decoded,
textured BMD room `06960212` — once per frame, from the engine's own
`DocCamera`, as the camera orbits under scripted `DocInput` and the engine
state machine advances. See `out/run.log` and `out/frame_00..08.png`.

## Isolation
Built **only** from `engine_snapshot/` — a one-time copy of the STABLE
`native/engine` modules, taken at the start of this task to avoid races with
the agent adding the actor subsystem. `scene/actor.{h,c}` is **excluded**; no
snapshot file `#include`s `actor.h` (the only "actor" mentions in the snapshot
are prose in `light.h` / `camera.h` comments), so nothing had to be stubbed to
compile standalone. `native/gpu` and `native/assets` are compiled / linked
read-only from their own trees and never modified.

## REAL (ported / native code actually executing)
- **The frame loop.** `doc_engine_frame` / `doc_engine_frame_preamble`
  (`core/frame_loop.c`) — the real control structure of `FUN_00248110`. It is
  the driver: `R->submit_scene` fires at the object-list draw point *inside*
  the loop body, and `R->present` reads the FBO to PNG. Not a standalone render
  call.
- **The state machine.** `engine_state_dispatch` (`core/engine_state.c`) runs
  every frame via the loop's `run_state_dispatch` hook and really transitions
  `BOOT → TITLE → TITLE_HOLD → ATTRACT → LOAD_DISPATCH → FIELD_SETUP →
  FIELD_LOAD → FIELD_RUN` (then holds in FIELD_RUN). The transition logic is the
  ported code; see the per-frame states in `run.log`.
- **The object-list walk.** `doc_scene_object_list_update` /
  `doc_scene_object_set_draw_mode` (`scene/object_list.c`, `scene/scene_graph.c`)
  run each frame via the loop's `run_object_list` hook over a real
  `DocSceneObject` (the character, flagged `VISIBLE | LIST_ACTIVE`), setting its
  draw mode (`char draw_mode=1` in the log).
- **The camera.** `DocCamera` (`scene/camera.c`): `doc_camera_set` (the real
  `KerberosCamera.set` eye/target/up → Euler path), `doc_camera_view_matrix`
  (Euler rotation + rigid inverse), `doc_camera_projection_matrix`. The DocCamera
  is the **authoritative view**; its eye orbits the scene each frame.
- **Input.** `DocInput` (`core/input.c`): the real active-low→active-high button
  decode, stick dead-zone normalize (`doc_input_stick_normalize`), and
  begin/latch, driven behind the loop's `input_poll` / `input_latch` hooks.
- **Lighting state.** `DocLightSet` (`scene/light.c`): `doc_light_init` /
  `doc_light_set_ambient` / `doc_light_commit` — the real env light-state API.
- **Math.** `dmath` / `vu0`.
- **The GPU renderer.** `native/gpu` `rgl_render_scene` (renderer_gl + gl_api +
  EGL-offscreen FBO): the real GL 3.3-core purist RENDER-MATH shader pair,
  one shared depth buffer, readback → PNG. EGL/Mesa line in `run.log`.
- **The assets.** Real decoded `DMSH` meshes (`char_e000.mesh`, 16
  `bmd_06960212_tex_g*.mesh` groups) and real `libdtex`-decoded TXD textures.
  Both on-disk formats are loaded with the same bounds-checked loaders the scene
  renderer uses (magic/size/vcount validation before any allocation).

## How the DocCamera drives the GPU render (and why it's honest)
The read-only `native/gpu` scene entry point (`rgl_render_scene`) **frames its
own camera** from a focus box + orbit params `(azimuth, elevation, dist_scale)`;
it exposes no "take a raw view/projection mat4" entry. So each frame the camera
controller moves the **DocCamera eye** on an orbit (from stick input), and
`cap_submit` **derives** `(az, el, dist_scale)** back out of
`DocCamera.eye` and feeds those to the renderer. The renderer then reconstructs
**exactly** that eye — verified every frame: `eye-match err=0.0000` in
`run.log`. The orbit/eye math in `framing_setup` / `orbit_eye` is copied
byte-for-byte from the renderer's own `frame_camera_ex`, which is what makes the
reconstruction exact. The DocCamera's own `view_matrix` (PS2 row-vector,
+Z-forward convention) is also built and logged (`view row0=...`); the GL
renderer independently builds the GL-convention (−Z-forward) equivalent for the
same eye/target/fov, because its public API frames itself. So: the **viewpoint
is entirely the DocCamera's**; the renderer is told where to look from by
inverting the DocCamera eye to its orbit params. No raw-matrix handoff is
claimed, because the read-only renderer has no such entry.

## STUBBED / SCRIPTED (honest gaps — all documented, none silently faked)
- **The ~40 below-seam per-frame subsystem updates** (audio, VU/GS DMA, loader
  IO, script VM, HUD, etc.) are the frame loop's documented `DocFrameHooks`
  subsystem hooks. Here they are no-ops (`cap_sub`), exactly as the loop header
  intends — each names a subsystem pending its own port. The one exception is
  the renderer transport bracket, which is the real GPU renderer.
- **The engine state-machine poll results are SCRIPTED** (`make_dispatch_hooks`
  / the `sc_*` functions): the *dispatch logic* is the real ported code, but the
  below-seam queries it branches on (`title_boot_poll`, `field_run_poll`,
  `field_exit_poll`, clock, …) return scripted values to drive the boot→field
  arc. In the real engine these come from loader/VM state; modelling them is how
  you exercise the real transitions without the full VM.
- **The per-scene light rig** passed to the GL shader is the documented
  placeholder rig (`rgl_default_lightrig`, RENDER-MATH §6): the lighting *model*
  and EE→VU1 upload path are recovered, but the per-light *constants* live in
  per-zone KEL-VM bytecode (a known engine gap) and were deliberately not
  fabricated. The `DocLightSet` state API is real and committed; the GL draw
  uses the placeholder rig for the character's LIT shading.
- **Frame pacing / mutex / vblank** (`frame_lock`/`unlock`/`frame_advance`) are
  no-op hooks; the loop's *ordering* is preserved, there is no real scheduler.
- **Character placement in the room** is the same HAND-SET transform
  (scale 2.6, yaw 160°, feet at world (268, 0, 330)) the scene renderer uses —
  the two assets share no authored placement frame; documented in
  `place_character`.

## Reproduce
```
cd native/capstone && make && ./capstone -n 9
# -> out/frame_00..08.png  +  out/run.log
```
Hero frame copied to `/srv/shared/dirge-port/26_capstone_engine_gpu.png`,
log to `/srv/shared/dirge-port/26_capstone_run.log`.
